/**
 * @file RenderSubsystem.cpp
 * @brief RenderSubsystem implementation
 */

#include "Render/RenderSubsystem.h"
#include "Context/RenderContextInternal.h"
#include "Core/Camera/Camera.h"
#include "Core/Event/EventBus.h"
#include "Core/Log.h"
#include "HAL/Window/WindowEvents.h"
#include "Render/Context/RenderContext.h"
#include "Render/GPUResourceManager.h"
#include "Render/Passes/OpaquePass.h"
#include "Render/Renderer/RenderScene.h"
#include "Render/Renderer/SceneRenderer.h"
#include "RenderExtraction/WorldCameraBridge.h"
#include "Resources/RenderResourceRegistry.h"
#include "Resources/RenderRetirementQueue.h"
#include "Resources/RenderUploadProcessor.h"
#include "Resources/RenderSubmissionTracker.h"
#include "Runtime/DedicatedRenderExecutor.h"
#include "Runtime/RenderThreadRuntime.h"
#include "Runtime/Window/WindowSubsystem.h"

#include <stdexcept>
#include <utility>
#include <vector>

namespace RVX
{

class LegacySynchronousRenderBridge final : public NonMovable
{
public:
    RenderConfig config{};
    WindowSubsystem* windowSubsystem = nullptr;
    std::unique_ptr<RenderContext> renderContext;
    std::unique_ptr<SceneRenderer> sceneRenderer;
    bool frameActive = false;
};

namespace
{
    [[nodiscard]] bool IsBackendEnabledInBuild(
        RHIBackendType backend) noexcept
    {
        switch (backend)
        {
#if RVX_ENABLE_DX11
            case RHIBackendType::DX11:
                return true;
#endif
#if RVX_ENABLE_DX12
            case RHIBackendType::DX12:
                return true;
#endif
#if RVX_ENABLE_VULKAN
            case RHIBackendType::Vulkan:
                return true;
#endif
#if RVX_ENABLE_METAL
            case RHIBackendType::Metal:
                return true;
#endif
#if RVX_ENABLE_OPENGL
            case RHIBackendType::OpenGL:
                return true;
#endif
            default:
                return false;
        }
    }

    class ClearPresentFrameConsumer final : public IRenderFrameConsumer,
                                            public NonMovable
    {
    public:
        RenderRuntimeResult Initialize(
            const RenderRuntimeConfig& config,
            const NativeSurfaceDesc& surface,
            RenderResourceStatusTable& statusTable) override
        {
            RenderRuntimeResult result;
            RHIBackendType backend = config.backendType;
            if (backend == RHIBackendType::Auto)
            {
                backend = SelectBestBackend();
            }
            result.backend = backend;
            result.surfaceGeneration = surface.generation;
            if (!surface.IsValidFor(backend))
            {
                result.code = RenderRuntimeCode::InvalidSurface;
                result.message = "Initial surface is invalid for the selected backend";
                return result;
            }

            m_context = std::make_unique<RenderContext>();
            RenderContextConfig contextConfig;
            contextConfig.backendType = backend;
            contextConfig.enableValidation = config.enableValidation;
            contextConfig.enableGPUValidation = config.enableGPUValidation;
            contextConfig.vsync = surface.vsync;
            contextConfig.frameBuffering = config.frameBuffering;
            contextConfig.appName = "RenderVerseX";
            if (!m_context->Initialize(contextConfig, surface))
            {
                m_context.reset();
                result.code = RenderRuntimeCode::DeviceCreationFailed;
                result.message = "Render device creation failed";
                return result;
            }
            if (!m_context->CreateSwapChain(surface))
            {
                m_context->Shutdown();
                m_context.reset();
                result.code = RenderRuntimeCode::SurfaceCreationFailed;
                result.message = "Render surface creation failed";
                return result;
            }

            RenderSubmissionTracker* submissionTracker =
                RenderContextInternalAccess::GetSubmissionTracker(*m_context);
            if (submissionTracker == nullptr ||
                !m_retirementQueue.Initialize(submissionTracker) ||
                !m_resourceRegistry.Initialize(&statusTable,
                                               &m_retirementQueue) ||
                !m_uploadProcessor.Initialize(m_context->GetDevice(),
                                              &statusTable,
                                              &m_resourceRegistry,
                                              submissionTracker))
            {
                m_resourceRegistry.Shutdown();
                m_context->Shutdown();
                m_context.reset();
                result.code = RenderRuntimeCode::DeviceCreationFailed;
                result.message = "Render resource runtime initialization failed";
                return result;
            }

            m_sceneRenderer = std::make_unique<SceneRenderer>();
            m_sceneRenderer->Initialize(m_context.get(), &m_resourceRegistry);
            if (!m_sceneRenderer->IsInitialized())
            {
                m_sceneRenderer.reset();
                m_uploadProcessor.Shutdown();
                m_resourceRegistry.Shutdown();
                m_context->Shutdown();
                m_context.reset();
                result.code = RenderRuntimeCode::DeviceCreationFailed;
                result.message = "Packet renderer initialization failed";
                return result;
            }
            m_sceneRenderer->SetSurfaceCompatibilityKey(surface.generation);

            result.code = RenderRuntimeCode::Running;
            return result;
        }

        RenderRuntimeResult ApplySurface(
            const NativeSurfaceDesc& surface) override
        {
            RenderRuntimeResult result;
            result.code = RenderRuntimeCode::Running;
            result.backend = m_context != nullptr && m_context->GetDevice() != nullptr
                                 ? m_context->GetDevice()->GetBackendType()
                                 : RHIBackendType::None;
            result.surfaceGeneration = surface.generation;
            if (m_context == nullptr || m_context->GetDevice() == nullptr ||
                !surface.IsValidFor(result.backend))
            {
                result.code = RenderRuntimeCode::InvalidSurface;
                result.message = "Surface update is invalid";
                return result;
            }

            const NativeSurfaceUpdateKind updateKind =
                ClassifyNativeSurfaceUpdate(m_context->GetSurface(), surface);
            bool applied = false;
            if (updateKind == NativeSurfaceUpdateKind::Resize)
            {
                m_context->WaitIdle();
                if (m_sceneRenderer != nullptr)
                {
                    m_sceneRenderer->PrepareForSwapChainResize();
                }
                applied = m_context->ResizeSwapChain(surface);
            }
            else if (updateKind == NativeSurfaceUpdateKind::Replace &&
                     m_context->GetDevice()->SupportsSurfaceRebind(
                         m_context->GetSurface(), surface))
            {
                m_context->WaitIdle();
                if (m_sceneRenderer != nullptr)
                {
                    m_sceneRenderer->PrepareForSwapChainResize();
                }
                applied = m_context->CreateSwapChain(surface);
            }
            if (!applied)
            {
                result.code = RenderRuntimeCode::SurfaceCreationFailed;
                result.message = "Surface update could not be applied";
                return result;
            }
            m_sceneRenderer->SetSurfaceCompatibilityKey(surface.generation);
            if (m_sceneRenderer->GetLastPresentedFrameSequence() != 0)
            {
                return PresentAcceptedFrame(
                    m_sceneRenderer->GetLastPresentedFrameSequence(),
                    surface.generation);
            }
            return PresentDeterministicClear(surface.generation);
        }

        void ProcessRelease(RenderResourceHandle handle) override
        {
            m_uploadProcessor.ProcessRelease(handle);
        }

        void ProcessUpload(ResourceUploadRequestRef request) override
        {
            static_cast<void>(
                m_uploadProcessor.ProcessUpload(std::move(request)));
        }

        RenderRuntimeResult ConsumeFrame(
            const RenderFramePacket& packet) override
        {
            RenderRuntimeResult result;
            result.code = RenderRuntimeCode::Running;
            result.frameSequence = packet.GetHeader().sequence;
            if (m_context == nullptr || !m_context->HasSwapChain())
            {
                result.code = RenderRuntimeCode::SurfaceCreationFailed;
                result.message = "Cannot consume a frame without a surface";
                return result;
            }

            if (m_sceneRenderer == nullptr)
            {
                result.code = RenderRuntimeCode::OwnershipViolation;
                result.message = "Packet renderer is unavailable";
                return result;
            }

            const RenderFrameApplyResult applyResult =
                m_sceneRenderer->ApplyFramePacket(packet,
                                                  m_resourceRegistry);
            if (!applyResult.IsApplied())
            {
                result.code = RenderRuntimeCode::RenderGraphValidationFailed;
                result.message =
                    "Immutable frame packet was rejected before recording, code=" +
                    std::to_string(static_cast<uint32>(applyResult.code));
                return result;
            }

            return PresentAcceptedFrame(
                packet.GetHeader().sequence,
                m_context->GetSurface().generation);
        }

        void PollCompletion() override
        {
            static_cast<void>(m_uploadProcessor.PollCompletion());
        }

        void RetireCompleted() override
        {
            static_cast<void>(m_retirementQueue.Poll());
        }

        RenderShutdownResult Shutdown(
            RenderTeardownMode) noexcept override
        {
            RenderShutdownResult result;
            result.code = RenderShutdownCode::Completed;
            try
            {
                if (m_context != nullptr)
                {
                    if (m_context->GetDevice() != nullptr)
                    {
                        result.backend =
                            m_context->GetDevice()->GetBackendType();
                    }
                    result.surfaceGeneration =
                        m_context->GetSurface().generation;
                    m_context->WaitIdle();
                    if (m_sceneRenderer != nullptr)
                    {
                        m_sceneRenderer->Shutdown();
                        m_sceneRenderer.reset();
                    }
                    static_cast<void>(m_uploadProcessor.PollCompletion());
                    m_uploadProcessor.Shutdown();
                    static_cast<void>(m_retirementQueue.Poll());
                    m_resourceRegistry.Shutdown();
                    if (m_retirementQueue.GetDiagnostics().entryCount != 0)
                    {
                        static_cast<void>(
                            m_retirementQueue.ForceDeviceLostTeardown());
                    }
                    m_context->Shutdown();
                    m_context.reset();
                }
            }
            catch (const std::exception& exception)
            {
                result.code = RenderShutdownCode::ExecutorJoinFailed;
                result.message =
                    std::string("Render consumer cleanup threw: ") +
                    exception.what();
            }
            catch (...)
            {
                result.code = RenderShutdownCode::ExecutorJoinFailed;
                result.message = "Render consumer cleanup threw";
            }
            return result;
        }

    private:
        RenderRuntimeResult PresentAcceptedFrame(
            uint64 frameSequence,
            uint64 surfaceGeneration)
        {
            RenderRuntimeResult result;
            result.code = RenderRuntimeCode::Running;
            result.backend = m_context != nullptr &&
                                     m_context->GetDevice() != nullptr
                                 ? m_context->GetDevice()->GetBackendType()
                                 : RHIBackendType::None;
            result.frameSequence = frameSequence;
            result.surfaceGeneration = surfaceGeneration;
            if (m_context == nullptr || m_sceneRenderer == nullptr ||
                !m_context->BeginFrame())
            {
                result.code = RenderRuntimeCode::DeviceLost;
                result.message = "Frame slot completion was lost";
                return result;
            }

            RenderFrameExecutionResult executionResult =
                m_sceneRenderer->RenderAcceptedFrame();
            if (executionResult.code != RenderFrameExecutionCode::Rendered)
            {
                m_context->AbortFrame();
                result.code = RenderRuntimeCode::RenderGraphValidationFailed;
                result.message =
                    "Accepted frame failed RenderGraph validation or recording";
                return result;
            }
            const GPUCompletionPoint submittedPoint = m_context->EndFrame();
            if (submittedPoint.value == 0)
            {
                result.code = RenderRuntimeCode::DeviceLost;
                result.message =
                    "Graphics submission did not produce a completion point";
                return result;
            }

            GPUCompletionToken completion;
            if (!InsertGPUCompletionPoint(completion, submittedPoint) ||
                !StampReferencedResources(executionResult.referencedResources,
                                          completion))
            {
                result.code = RenderRuntimeCode::OwnershipViolation;
                result.message =
                    "Submitted frame resources could not be stamped by exact generation";
                return result;
            }
            m_context->Present();
            m_sceneRenderer->MarkAcceptedFramePresented();
            return result;
        }

        RenderRuntimeResult PresentDeterministicClear(
            uint64 surfaceGeneration)
        {
            RenderRuntimeResult result;
            result.code = RenderRuntimeCode::Running;
            result.backend = m_context != nullptr &&
                                     m_context->GetDevice() != nullptr
                                 ? m_context->GetDevice()->GetBackendType()
                                 : RHIBackendType::None;
            result.surfaceGeneration = surfaceGeneration;
            if (m_context == nullptr || !m_context->BeginFrame())
            {
                result.code = RenderRuntimeCode::DeviceLost;
                result.message = "Resize redraw could not begin a frame";
                return result;
            }
            RHICommandContext* commandContext =
                m_context->GetGraphicsContext();
            RHITexture* backBuffer = m_context->GetCurrentBackBuffer();
            RHITextureView* backBufferView =
                m_context->GetCurrentBackBufferView();
            if (commandContext == nullptr || backBuffer == nullptr ||
                backBufferView == nullptr)
            {
                m_context->AbortFrame();
                result.code = RenderRuntimeCode::SurfaceCreationFailed;
                result.message = "Resize redraw has no renderable back buffer";
                return result;
            }
            commandContext->TextureBarrier(backBuffer,
                                           RHIResourceState::Present,
                                           RHIResourceState::RenderTarget);
            RHIRenderPassDesc clearPass;
            clearPass.AddColorAttachment(
                backBufferView,
                RHILoadOp::Clear,
                RHIStoreOp::Store,
                RHIClearColor{0.015625f, 0.0234375f, 0.03125f, 1.0f});
            commandContext->BeginRenderPass(clearPass);
            commandContext->EndRenderPass();
            commandContext->TextureBarrier(backBuffer,
                                           RHIResourceState::RenderTarget,
                                           RHIResourceState::Present);
            const GPUCompletionPoint submittedPoint = m_context->EndFrame();
            if (submittedPoint.value == 0)
            {
                result.code = RenderRuntimeCode::DeviceLost;
                result.message = "Resize redraw submission failed";
                return result;
            }
            m_context->Present();
            return result;
        }

        bool StampReferencedResources(
            const std::vector<RenderResourceHandle>& resources,
            const GPUCompletionToken& completion)
        {
            return m_resourceRegistry.MergeLastUseClosure(resources,
                                                          completion);
        }

        std::unique_ptr<RenderContext> m_context;
        std::unique_ptr<SceneRenderer> m_sceneRenderer;
        RenderRetirementQueue m_retirementQueue;
        RenderResourceRegistry m_resourceRegistry;
        RenderUploadProcessor m_uploadProcessor;
    };
} // namespace

RenderSubsystem::RenderSubsystem()
    : m_legacyBridge(std::make_unique<LegacySynchronousRenderBridge>())
{
}
RenderSubsystem::~RenderSubsystem() = default;

void RenderSubsystem::Initialize()
{
    if (m_initializeAttempted)
    {
        if (m_runtimeConfigured)
        {
            const RenderRuntimeResult result = GetLastRuntimeResult();
            if (result.code != RenderRuntimeCode::Running)
            {
                throw RenderSubsystemInitializationError(result);
            }
        }
        return;
    }
    m_initializeAttempted = true;

    if (m_runtimeConfigured)
    {
        InitializeRuntime();
        return;
    }
    InitializeLegacy(m_legacyBridge->config);
}

void RenderSubsystem::InitializeRuntime()
{
    const RenderRuntimeResult result = m_runtime->Start();
    m_preRuntimeResult = result;
    if (result.code != RenderRuntimeCode::Running)
    {
        throw RenderSubsystemInitializationError(result);
    }
}

void RenderSubsystem::Configure(const RenderRuntimeConfig& config,
                                const NativeSurfaceDesc& surface)
{
    if (m_initializeAttempted)
    {
        throw std::logic_error(
            "RenderSubsystem::Configure is only valid before Initialize");
    }

    auto runtime = std::make_unique<RenderThreadRuntime>(
        config,
        surface,
        RenderExecutorKind::Dedicated,
        CreateDedicatedRenderExecutor(),
        std::make_unique<ClearPresentFrameConsumer>());
    m_runtime = std::move(runtime);
    m_runtimeConfig = config;
    m_runtimeSurface = surface;
    m_runtimeConfigured = true;
    m_preRuntimeResult = {};
    m_preShutdownResult = {};
}

RenderFramePublishResult RenderSubsystem::TryPublishFrame(
    std::unique_ptr<const RenderFramePacket> packet)
{
    if (m_runtime == nullptr)
    {
        RenderFramePublishResult result;
        result.code = RenderFramePublishCode::NotRunning;
        result.resultClass = ClassifyRenderFramePublishCode(result.code);
        result.sequence = packet != nullptr ? packet->GetHeader().sequence : 0U;
        return result;
    }
    return m_runtime->TryPublishFrame(std::move(packet));
}

RenderResizeResult RenderSubsystem::RequestResize(
    const NativeSurfaceDesc& surface)
{
    if (m_runtime == nullptr)
    {
        RenderResizeResult result;
        result.code = RenderResizeCode::NotRunning;
        result.resultClass = ClassifyRenderResizeCode(result.code);
        result.generation = surface.generation;
        return result;
    }
    RenderResizeResult result = m_runtime->RequestResize(surface);
    if (result.code == RenderResizeCode::Accepted ||
        result.code == RenderResizeCode::CoalescedOlder)
    {
        m_runtimeSurface = surface;
    }
    return result;
}

RenderDiagnosticsSnapshot RenderSubsystem::GetDiagnosticsSnapshot() const
{
    return m_runtime != nullptr ? m_runtime->GetDiagnosticsSnapshot()
                                : RenderDiagnosticsSnapshot{};
}

RenderRuntimeResult RenderSubsystem::GetLastRuntimeResult() const
{
    return m_runtime != nullptr ? m_runtime->GetLastRuntimeResult()
                                : m_preRuntimeResult;
}

RenderShutdownResult RenderSubsystem::GetLastShutdownResult() const
{
    return m_runtime != nullptr ? m_runtime->GetLastShutdownResult()
                                : m_preShutdownResult;
}

void RenderSubsystem::Initialize(const RenderConfig& config)
{
    if (m_initializeAttempted)
    {
        if (m_runtimeConfigured)
        {
            const RenderRuntimeResult result = GetLastRuntimeResult();
            if (result.code != RenderRuntimeCode::Running)
            {
                throw RenderSubsystemInitializationError(result);
            }
        }
        return;
    }
    m_initializeAttempted = true;

    if (m_runtimeConfigured)
    {
        InitializeRuntime();
        return;
    }
    InitializeLegacy(config);
}

void RenderSubsystem::InitializeLegacy(const RenderConfig& config)
{
    m_legacyBridge->config = config;

    // Handle Auto backend selection
    RHIBackendType actualBackend = config.backendType;
    if (actualBackend == RHIBackendType::Auto)
    {
        actualBackend = SelectBestBackend();
    }
    if (!IsBackendEnabledInBuild(actualBackend))
    {
        return;
    }
    if (config.backendType == RHIBackendType::Auto)
    {
        RVX_CORE_INFO("RenderSubsystem auto-selected backend: {}", ToString(actualBackend));
    }
    else
    {
        RVX_CORE_INFO("RenderSubsystem initializing with backend: {}", ToString(actualBackend));
    }

    // Create render context
    m_legacyBridge->renderContext = std::make_unique<RenderContext>();
    // Preserve the frozen legacy source-contract names until Task 18 removes
    // this synchronous bridge; ownership remains inside m_legacyBridge.
    RenderContext* const m_renderContext =
        m_legacyBridge->renderContext.get();
    WindowSubsystem* const m_windowSubsystem =
        m_legacyBridge->windowSubsystem;

    // Initialize render context with RHI device
    RenderContextConfig ctxConfig;
    ctxConfig.backendType = actualBackend;
    ctxConfig.enableValidation = config.enableValidation;
    ctxConfig.vsync = config.vsync;
    ctxConfig.frameBuffering = config.frameBuffering;
    ctxConfig.appName = "RenderVerseX";

    NativeSurfaceDesc initialSurface;
    if (m_legacyBridge->config.autoBindWindow && m_windowSubsystem)
    {
        initialSurface = m_windowSubsystem->CaptureRenderSurface();
        initialSurface.vsync = config.vsync;
        m_windowSubsystem->ReleaseGraphicsContextFromCurrentThread();
    }

    if (!m_renderContext->Initialize(ctxConfig, initialSurface))
    {
        RVX_CORE_ERROR("RenderSubsystem: Failed to initialize render context");
        m_legacyBridge->renderContext.reset();
        return;
    }

    // Create scene renderer
    m_legacyBridge->sceneRenderer = std::make_unique<SceneRenderer>();
    m_legacyBridge->sceneRenderer->Initialize(
        m_legacyBridge->renderContext.get());

    // Note: Default passes (OpaquePass) are added by SceneRenderer::SetupDefaultPasses()

    // Auto-bind to window if enabled
    if (m_legacyBridge->config.autoBindWindow)
    {
        if (initialSurface.IsValidFor(actualBackend))
        {
            if (!SetWindow(initialSurface))
            {
                RVX_CORE_ERROR("RenderSubsystem: Initial surface binding failed");
            }
        }
        else
        {
            AutoBindWindow();
        }
    }

    // Subscribe to window resize events
    EventBus::Get().Subscribe<HAL::WindowResizedEvent>(
        [this](const HAL::WindowResizedEvent& e) {
            if (e.width > 0 && e.height > 0)
            {
                OnResize(e.width, e.height);
            }
        });

    RVX_CORE_INFO("RenderSubsystem initialized successfully");
}

void RenderSubsystem::Deinitialize()
{
    RVX_CORE_DEBUG("RenderSubsystem deinitializing...");

    if (m_runtimeConfigured)
    {
        if (m_runtime != nullptr)
        {
            m_preShutdownResult = m_runtime->Stop();
        }
        RVX_CORE_INFO("RenderSubsystem dedicated runtime deinitialized");
        return;
    }

    // Wait for GPU to finish all pending work before destroying resources
    if (m_legacyBridge->renderContext)
    {
        m_legacyBridge->renderContext->WaitIdle();
    }

    // Shutdown in reverse order
    if (m_legacyBridge->sceneRenderer)
    {
        m_legacyBridge->sceneRenderer->Shutdown();
        m_legacyBridge->sceneRenderer.reset();
    }

    if (m_legacyBridge->renderContext)
    {
        m_legacyBridge->renderContext->Shutdown();
        m_legacyBridge->renderContext.reset();
    }

    RVX_CORE_INFO("RenderSubsystem deinitialized");
}

void RenderSubsystem::BeginFrame()
{
    if (m_runtimeConfigured)
    {
        RVX_CORE_WARN("BeginFrame is unavailable after Configure; publish a frame packet");
        return;
    }
    if (m_legacyBridge->frameActive)
    {
        RVX_CORE_WARN("BeginFrame called while frame already active");
        return;
    }

    if (m_legacyBridge->renderContext)
    {
        m_legacyBridge->frameActive = m_legacyBridge->renderContext->BeginFrame();
        return;
    }

    m_legacyBridge->frameActive = false;
}

void RenderSubsystem::Render(World* world, Camera* camera)
{
    if (m_runtimeConfigured)
    {
        RVX_CORE_WARN("Render is unavailable after Configure; publish a frame packet");
        return;
    }
    if (!m_legacyBridge->frameActive)
    {
        RVX_CORE_WARN("Render called without BeginFrame");
        return;
    }

    if (!m_legacyBridge->sceneRenderer || !camera)
    {
        return;
    }

    // Setup view and collect scene data
    m_legacyBridge->sceneRenderer->SetupView(*camera, world);

    // SceneRenderer::SetupView() requests uploads for visible resources.
    // Rendering skips objects whose GPU resources are not ready yet.

    // Execute render graph
    m_legacyBridge->sceneRenderer->Render();
}

void RenderSubsystem::EnsureVisibleResourcesResident()
{
    auto* gpuMgr = m_legacyBridge->sceneRenderer
                       ? m_legacyBridge->sceneRenderer->GetGPUResourceManager()
                       : nullptr;
    if (!gpuMgr)
        return;

    const auto& renderScene = m_legacyBridge->sceneRenderer->GetRenderScene();
    const auto& visibleIndices =
        m_legacyBridge->sceneRenderer->GetVisibleObjectIndices();

    for (uint32_t idx : visibleIndices)
    {
        const auto& obj = renderScene.GetObject(idx);
        
        if (gpuMgr->IsResident(obj.meshId))
        {
            gpuMgr->MarkUsed(obj.meshId);
            continue;
        }

        GPUResourceState state = gpuMgr->GetResourceState(obj.meshId);
        if (state == GPUResourceState::UploadQueued || state == GPUResourceState::Uploading)
            continue;

        if (obj.meshResource)
        {
            gpuMgr->RequestUpload(obj.meshResource, UploadPriority::High);
        }
        else
        {
            RVX_CORE_WARN("RenderSubsystem: Mesh {} has no CPU resource for async upload", obj.meshId);
        }
    }
}

void RenderSubsystem::EndFrame()
{
    if (m_runtimeConfigured)
    {
        RVX_CORE_WARN("EndFrame is unavailable after Configure; publish a frame packet");
        return;
    }
    if (!m_legacyBridge->frameActive)
    {
        RVX_CORE_WARN("EndFrame called without BeginFrame");
        return;
    }

    if (m_legacyBridge->renderContext)
    {
        m_legacyBridge->renderContext->EndFrame();
    }

    m_legacyBridge->frameActive = false;
}

void RenderSubsystem::Present()
{
    if (m_runtimeConfigured)
    {
        RVX_CORE_WARN("Present is unavailable after Configure; runtime owns presentation");
        return;
    }
    if (m_legacyBridge->renderContext)
    {
        m_legacyBridge->renderContext->Present();
    }
}

RenderContext* RenderSubsystem::GetRenderContext() const
{
    return m_runtimeConfigured ? nullptr :
                                 m_legacyBridge->renderContext.get();
}

SceneRenderer* RenderSubsystem::GetSceneRenderer() const
{
    return m_runtimeConfigured ? nullptr :
                                 m_legacyBridge->sceneRenderer.get();
}

IRHIDevice* RenderSubsystem::GetDevice() const
{
    return !m_runtimeConfigured && m_legacyBridge->renderContext
               ? m_legacyBridge->renderContext->GetDevice()
               : nullptr;
}

RHISwapChain* RenderSubsystem::GetSwapChain() const
{
    return !m_runtimeConfigured && m_legacyBridge->renderContext
               ? m_legacyBridge->renderContext->GetSwapChain()
               : nullptr;
}

RenderGraph* RenderSubsystem::GetRenderGraph() const
{
    return !m_runtimeConfigured && m_legacyBridge->sceneRenderer
               ? m_legacyBridge->sceneRenderer->GetRenderGraph()
               : nullptr;
}

bool RenderSubsystem::SetWindow(const NativeSurfaceDesc& surface)
{
    if (m_runtimeConfigured)
    {
        const RenderResizeResult result = RequestResize(surface);
        return result.code == RenderResizeCode::Accepted ||
               result.code == RenderResizeCode::CoalescedOlder;
    }
    RVX_CORE_INFO("RenderSubsystem setting window: {}x{}",
                  surface.width,
                  surface.height);

    // Preserve the frozen legacy source-contract names until Task 18 removes
    // this synchronous bridge; ownership remains inside m_legacyBridge.
    RenderContext* const m_renderContext =
        m_legacyBridge->renderContext.get();
    SceneRenderer* const m_sceneRenderer =
        m_legacyBridge->sceneRenderer.get();

    if (!m_renderContext || !m_renderContext->GetDevice())
    {
        RVX_CORE_ERROR("RenderSubsystem: Cannot bind a surface without a render device");
        return false;
    }

    if (!surface.IsValidFor(m_renderContext->GetDevice()->GetBackendType()))
    {
        RVX_CORE_ERROR("RenderSubsystem: Invalid native surface update");
        return false;
    }

    if (!m_renderContext->HasSwapChain())
    {
        return m_renderContext->CreateSwapChain(surface);
    }

    const NativeSurfaceUpdateKind updateKind = ClassifyNativeSurfaceUpdate(
        m_renderContext->GetSurface(), surface);
    switch (updateKind)
    {
        case NativeSurfaceUpdateKind::Reject:
            RVX_CORE_WARN("RenderSubsystem: Rejected stale surface generation {}",
                          surface.generation);
            return false;

        case NativeSurfaceUpdateKind::Resize:
            m_renderContext->WaitIdle();
            if (m_sceneRenderer)
            {
                m_sceneRenderer->PrepareForSwapChainResize();
            }
            return m_renderContext->ResizeSwapChain(surface);

        case NativeSurfaceUpdateKind::Replace:
            if (!m_renderContext->GetDevice()->SupportsSurfaceRebind(
                    m_renderContext->GetSurface(), surface))
            {
                RVX_CORE_WARN(
                    "RenderSubsystem: Surface replacement requires device recreation");
                return false;
            }
            m_renderContext->WaitIdle();
            if (m_sceneRenderer)
            {
                m_sceneRenderer->PrepareForSwapChainResize();
            }
            return m_renderContext->CreateSwapChain(surface);
    }

    RVX_UNREACHABLE();
}

void RenderSubsystem::SetWindowSubsystem(WindowSubsystem* windowSubsystem)
{
    m_legacyBridge->windowSubsystem = windowSubsystem;
    if (!m_runtimeConfigured &&
        m_legacyBridge->config.autoBindWindow &&
        m_legacyBridge->renderContext &&
        !m_legacyBridge->renderContext->HasSwapChain())
    {
        AutoBindWindow();
    }
}

void RenderSubsystem::OnResize(uint32_t width, uint32_t height)
{
    RVX_CORE_INFO("RenderSubsystem resize: {}x{}", width, height);

    if (m_runtimeConfigured)
    {
        NativeSurfaceDesc surface = m_runtimeSurface;
        surface.width = width;
        surface.height = height;
        ++surface.generation;
        const RenderResizeResult result = RequestResize(surface);
        if (result.code == RenderResizeCode::Accepted ||
            result.code == RenderResizeCode::CoalescedOlder)
        {
            m_runtimeSurface = surface;
        }
        return;
    }

    if (m_legacyBridge->renderContext)
    {
        // Ensure no submitted frame still references views/resources before releasing them.
        m_legacyBridge->renderContext->WaitIdle();
        if (m_legacyBridge->sceneRenderer)
        {
            m_legacyBridge->sceneRenderer->PrepareForSwapChainResize();
        }
        m_legacyBridge->renderContext->ResizeSwapChain(width, height);
    }
}

void RenderSubsystem::RenderFrame(World* world)
{
    if (!world)
    {
        return;
    }

    const WorldCameraBridge cameraBridge;
    Camera* camera = cameraBridge.GetActiveCamera(world);
    if (!camera)
    {
        RVX_CORE_WARN("RenderFrame: World has no active camera");
        return;
    }

    BeginFrame();
    Render(world, camera);
    EndFrame();
    Present();
}

void RenderSubsystem::ProcessGPUUploads(float timeBudgetMs)
{
    if (auto* gpuManager = GetGPUResourceManager())
    {
        gpuManager->ProcessPendingUploads(timeBudgetMs);
    }
}

GPUResourceManager* RenderSubsystem::GetGPUResourceManager() const
{
    return !m_runtimeConfigured && m_legacyBridge->sceneRenderer
               ? m_legacyBridge->sceneRenderer->GetGPUResourceManager()
               : nullptr;
}

RenderResourceReserveResult RenderSubsystem::ReserveResource(
    AssetId assetId,
    RenderResourceKind kind) noexcept
{
    return m_runtime != nullptr ? m_runtime->ReserveResource(assetId, kind)
                                : RenderResourceReserveResult{};
}

RenderUploadEnqueueResult RenderSubsystem::TryEnqueueUpload(
    const ResourceUploadRequestRef& request) noexcept
{
    return m_runtime != nullptr ? m_runtime->TryEnqueueUpload(request)
                                : RenderUploadEnqueueResult{};
}

RenderReleaseResult RenderSubsystem::RequestRelease(
    RenderResourceHandle handle) noexcept
{
    return m_runtime != nullptr ? m_runtime->RequestRelease(handle)
                                : RenderReleaseResult{};
}

RenderResourceStatus RenderSubsystem::QueryResourceStatus(
    RenderResourceHandle handle) const noexcept
{
    return m_runtime != nullptr ? m_runtime->QueryResourceStatus(handle)
                                : RenderResourceStatus{};
}

bool RenderSubsystem::IsReady() const
{
    if (m_runtimeConfigured)
    {
        return m_runtime != nullptr && m_runtime->IsReady();
    }
    return m_legacyBridge->renderContext != nullptr &&
           m_legacyBridge->renderContext->GetDevice() != nullptr &&
           m_legacyBridge->renderContext->HasSwapChain();
}

void RenderSubsystem::SetConfig(const RenderConfig& config)
{
    if (m_runtimeConfigured)
    {
        throw std::logic_error(
            "Legacy RenderConfig is unavailable after Configure");
    }
    m_legacyBridge->config = config;
}

const RenderConfig& RenderSubsystem::GetConfig() const
{
    return m_legacyBridge->config;
}

void RenderSubsystem::AutoBindWindow()
{
    if (!m_legacyBridge->windowSubsystem)
    {
        RVX_CORE_WARN("RenderSubsystem: Cannot auto-bind window - WindowSubsystem dependency was not injected");
        return;
    }

    if (!m_legacyBridge->renderContext ||
        !m_legacyBridge->renderContext->GetDevice())
    {
        RVX_CORE_WARN("RenderSubsystem: RenderContext has no valid device");
        return;
    }

    NativeSurfaceDesc surface =
        m_legacyBridge->windowSubsystem->CaptureRenderSurface();
    surface.vsync = m_legacyBridge->config.vsync;
    if (!surface.IsValidFor(
            m_legacyBridge->renderContext->GetDevice()->GetBackendType()))
    {
        RVX_CORE_WARN("RenderSubsystem: WindowSubsystem has no valid native surface");
        return;
    }

    m_legacyBridge->windowSubsystem->ReleaseGraphicsContextFromCurrentThread();
    RVX_CORE_INFO("RenderSubsystem: Auto-binding to window {}x{}",
                  surface.width,
                  surface.height);
    if (!SetWindow(surface))
    {
        RVX_CORE_WARN("RenderSubsystem: Auto-binding the native surface failed");
    }
}

} // namespace RVX
