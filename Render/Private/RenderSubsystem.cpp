/**
 * @file RenderSubsystem.cpp
 * @brief RenderSubsystem implementation
 */

#include "Render/RenderSubsystem.h"
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
#include "Runtime/Window/WindowSubsystem.h"

namespace RVX
{

RenderSubsystem::RenderSubsystem() = default;
RenderSubsystem::~RenderSubsystem() = default;

void RenderSubsystem::Initialize()
{
    Initialize(m_config);
}

void RenderSubsystem::Initialize(const RenderConfig& config)
{
    m_config = config;

    // Handle Auto backend selection
    RHIBackendType actualBackend = config.backendType;
    if (actualBackend == RHIBackendType::Auto)
    {
        actualBackend = SelectBestBackend();
        RVX_CORE_INFO("RenderSubsystem auto-selected backend: {}", ToString(actualBackend));
    }
    else
    {
        RVX_CORE_INFO("RenderSubsystem initializing with backend: {}", ToString(actualBackend));
    }

    // Create render context
    m_renderContext = std::make_unique<RenderContext>();

    // Initialize render context with RHI device
    RenderContextConfig ctxConfig;
    ctxConfig.backendType = actualBackend;
    ctxConfig.enableValidation = config.enableValidation;
    ctxConfig.vsync = config.vsync;
    ctxConfig.frameBuffering = config.frameBuffering;
    ctxConfig.appName = "RenderVerseX";

    NativeSurfaceDesc initialSurface;
    if (m_config.autoBindWindow && m_windowSubsystem)
    {
        initialSurface = m_windowSubsystem->CaptureRenderSurface();
        initialSurface.vsync = config.vsync;
        m_windowSubsystem->ReleaseGraphicsContextFromCurrentThread();
    }

    if (!m_renderContext->Initialize(ctxConfig, initialSurface))
    {
        RVX_CORE_ERROR("RenderSubsystem: Failed to initialize render context");
        m_renderContext.reset();
        return;
    }

    // Create scene renderer
    m_sceneRenderer = std::make_unique<SceneRenderer>();
    m_sceneRenderer->Initialize(m_renderContext.get());

    // Note: Default passes (OpaquePass) are added by SceneRenderer::SetupDefaultPasses()

    // Auto-bind to window if enabled
    if (m_config.autoBindWindow)
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

    // Wait for GPU to finish all pending work before destroying resources
    if (m_renderContext)
    {
        m_renderContext->WaitIdle();
    }

    // Shutdown in reverse order
    if (m_sceneRenderer)
    {
        m_sceneRenderer->Shutdown();
        m_sceneRenderer.reset();
    }

    if (m_renderContext)
    {
        m_renderContext->Shutdown();
        m_renderContext.reset();
    }

    RVX_CORE_INFO("RenderSubsystem deinitialized");
}

void RenderSubsystem::BeginFrame()
{
    if (m_frameActive)
    {
        RVX_CORE_WARN("BeginFrame called while frame already active");
        return;
    }

    if (m_renderContext)
    {
        m_renderContext->BeginFrame();
    }

    m_frameActive = true;
}

void RenderSubsystem::Render(World* world, Camera* camera)
{
    if (!m_frameActive)
    {
        RVX_CORE_WARN("Render called without BeginFrame");
        return;
    }

    if (!m_sceneRenderer || !camera)
    {
        return;
    }

    // Setup view and collect scene data
    m_sceneRenderer->SetupView(*camera, world);

    // SceneRenderer::SetupView() requests uploads for visible resources.
    // Rendering skips objects whose GPU resources are not ready yet.

    // Execute render graph
    m_sceneRenderer->Render();
}

void RenderSubsystem::EnsureVisibleResourcesResident()
{
    auto* gpuMgr = m_sceneRenderer ? m_sceneRenderer->GetGPUResourceManager() : nullptr;
    if (!gpuMgr)
        return;

    const auto& renderScene = m_sceneRenderer->GetRenderScene();
    const auto& visibleIndices = m_sceneRenderer->GetVisibleObjectIndices();

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
    if (!m_frameActive)
    {
        RVX_CORE_WARN("EndFrame called without BeginFrame");
        return;
    }

    if (m_renderContext)
    {
        m_renderContext->EndFrame();
    }

    m_frameActive = false;
}

void RenderSubsystem::Present()
{
    if (m_renderContext)
    {
        m_renderContext->Present();
    }
}

IRHIDevice* RenderSubsystem::GetDevice() const
{
    return m_renderContext ? m_renderContext->GetDevice() : nullptr;
}

RHISwapChain* RenderSubsystem::GetSwapChain() const
{
    return m_renderContext ? m_renderContext->GetSwapChain() : nullptr;
}

RenderGraph* RenderSubsystem::GetRenderGraph() const
{
    return m_sceneRenderer ? m_sceneRenderer->GetRenderGraph() : nullptr;
}

bool RenderSubsystem::SetWindow(const NativeSurfaceDesc& surface)
{
    RVX_CORE_INFO("RenderSubsystem setting window: {}x{}",
                  surface.width,
                  surface.height);

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
    m_windowSubsystem = windowSubsystem;
    if (m_config.autoBindWindow && m_renderContext && !m_renderContext->HasSwapChain())
    {
        AutoBindWindow();
    }
}

void RenderSubsystem::OnResize(uint32_t width, uint32_t height)
{
    RVX_CORE_INFO("RenderSubsystem resize: {}x{}", width, height);

    if (m_renderContext)
    {
        // Ensure no submitted frame still references views/resources before releasing them.
        m_renderContext->WaitIdle();
        if (m_sceneRenderer)
        {
            m_sceneRenderer->PrepareForSwapChainResize();
        }
        m_renderContext->ResizeSwapChain(width, height);
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
    return m_sceneRenderer ? m_sceneRenderer->GetGPUResourceManager() : nullptr;
}

bool RenderSubsystem::IsReady() const
{
    return m_renderContext != nullptr && 
           m_renderContext->GetDevice() != nullptr &&
           m_renderContext->HasSwapChain();
}

void RenderSubsystem::AutoBindWindow()
{
    if (!m_windowSubsystem)
    {
        RVX_CORE_WARN("RenderSubsystem: Cannot auto-bind window - WindowSubsystem dependency was not injected");
        return;
    }

    if (!m_renderContext || !m_renderContext->GetDevice())
    {
        RVX_CORE_WARN("RenderSubsystem: RenderContext has no valid device");
        return;
    }

    NativeSurfaceDesc surface = m_windowSubsystem->CaptureRenderSurface();
    surface.vsync = m_config.vsync;
    if (!surface.IsValidFor(m_renderContext->GetDevice()->GetBackendType()))
    {
        RVX_CORE_WARN("RenderSubsystem: WindowSubsystem has no valid native surface");
        return;
    }

    m_windowSubsystem->ReleaseGraphicsContextFromCurrentThread();
    RVX_CORE_INFO("RenderSubsystem: Auto-binding to window {}x{}",
                  surface.width,
                  surface.height);
    if (!SetWindow(surface))
    {
        RVX_CORE_WARN("RenderSubsystem: Auto-binding the native surface failed");
    }
}

} // namespace RVX
