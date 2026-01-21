/**
 * @file RenderSubsystem.cpp
 * @brief RenderSubsystem implementation
 */

#include "Render/RenderSubsystem.h"
#include "Render/Context/RenderContext.h"
#include "Render/Renderer/SceneRenderer.h"
#include "Render/Renderer/RenderScene.h"
#include "Render/GPUResourceManager.h"
#include "Render/Passes/OpaquePass.h"
#include "Engine/Engine.h"
#include "Runtime/Window/WindowSubsystem.h"
#include "World/World.h"
#include "Runtime/Camera/Camera.h"
#include "Resource/ResourceManager.h"
#include "Resource/Types/MeshResource.h"
#include "Core/Log.h"
#include "Core/Event/EventBus.h"
#include "HAL/Window/WindowEvents.h"

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

    if (!m_renderContext->Initialize(ctxConfig))
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
        AutoBindWindow();
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

    // Ensure visible meshes are GPU-resident before rendering
    EnsureVisibleResourcesResident();

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
        
        if (!gpuMgr->IsResident(obj.meshId))
        {
            // Get mesh resource from cache and upload immediately
            Resource::ResourceHandle<Resource::MeshResource> meshHandle = 
                Resource::ResourceManager::Get().Load<Resource::MeshResource>(obj.meshId);
            if (meshHandle.IsValid())
            {
                RVX_CORE_INFO("RenderSubsystem: Uploading mesh {} to GPU", obj.meshId);
                gpuMgr->UploadImmediate(meshHandle.Get());
            }
            else
            {
                RVX_CORE_WARN("RenderSubsystem: Mesh {} not found in ResourceManager", obj.meshId);
            }
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

void RenderSubsystem::SetWindow(void* windowHandle, uint32_t width, uint32_t height)
{
    RVX_CORE_INFO("RenderSubsystem setting window: {}x{}", width, height);

    if (m_renderContext)
    {
        if (!m_renderContext->HasSwapChain())
        {
            m_renderContext->CreateSwapChain(windowHandle, width, height);
        }
        else
        {
            // If swap chain exists, just resize
            m_renderContext->ResizeSwapChain(width, height);
        }
    }
}

void RenderSubsystem::OnResize(uint32_t width, uint32_t height)
{
    RVX_CORE_INFO("RenderSubsystem resize: {}x{}", width, height);

    if (m_renderContext)
    {
        m_renderContext->ResizeSwapChain(width, height);
    }
}

void RenderSubsystem::RenderFrame(World* world)
{
    if (!world)
    {
        return;
    }

    Camera* camera = world->GetActiveCamera();
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
    Engine* engine = GetEngine();
    if (!engine)
    {
        RVX_CORE_WARN("RenderSubsystem: Cannot auto-bind window - no engine reference");
        return;
    }

    auto* windowSubsystem = engine->GetSubsystem<WindowSubsystem>();
    if (!windowSubsystem)
    {
        RVX_CORE_WARN("RenderSubsystem: Cannot auto-bind window - WindowSubsystem not found");
        return;
    }

    void* handle = windowSubsystem->GetNativeHandle();
    if (!handle)
    {
        RVX_CORE_WARN("RenderSubsystem: WindowSubsystem has no valid window handle");
        return;
    }

    uint32_t width, height;
    windowSubsystem->GetFramebufferSize(width, height);

    RVX_CORE_INFO("RenderSubsystem: Auto-binding to window {}x{}", width, height);
    SetWindow(handle, width, height);
}

} // namespace RVX
