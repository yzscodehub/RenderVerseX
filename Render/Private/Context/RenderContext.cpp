/**
 * @file RenderContext.cpp
 * @brief Render context implementation
 */

#include "Render/Context/RenderContext.h"
#include "Core/Log.h"

namespace RVX
{

RenderContext::~RenderContext()
{
    Shutdown();
}

bool RenderContext::Initialize(const RenderContextConfig& config,
                               const NativeSurfaceDesc& initialSurface)
{
    if (m_initialized)
    {
        RVX_CORE_WARN("RenderContext already initialized");
        return true;
    }

    m_config = config;

    RVX_CORE_INFO("RenderContext initializing with backend: {}", ToString(config.backendType));

    // Create RHI device
    RHIDeviceDesc deviceDesc;
    deviceDesc.initialSurface = initialSurface;
    deviceDesc.enableDebugLayer = config.enableValidation;
    deviceDesc.enableGPUValidation = config.enableGPUValidation;
    deviceDesc.applicationName = config.appName;

    m_device = CreateRHIDevice(config.backendType, deviceDesc);
    if (!m_device)
    {
        RVX_CORE_ERROR("RenderContext: Failed to create RHI device");
        return false;
    }

    RVX_CORE_INFO("RenderContext: Created device - Adapter: {}", 
                  m_device->GetCapabilities().adapterName);

    // Initialize frame synchronizer
    uint32_t frameCount = std::min(config.frameBuffering, RVX_MAX_FRAME_COUNT);
    if (!m_frameSynchronizer.Initialize(m_device.get(), frameCount))
    {
        RVX_CORE_ERROR("RenderContext: Failed to initialize frame synchronizer");
        m_device.reset();
        return false;
    }

    // Create command contexts
    CreateCommandContexts();

    // Check async compute support
    m_supportsAsyncCompute = m_device->GetCapabilities().supportsAsyncCompute;
    RVX_CORE_INFO("RenderContext: Async compute support: {}", m_supportsAsyncCompute ? "yes" : "no");

    m_initialized = true;
    m_frameActive = false;
    m_frameReadyToPresent = false;
    m_frameIndex = 0;
    m_frameNumber = 0;
    m_surface = {};

    RVX_CORE_INFO("RenderContext initialized successfully");
    return true;
}

void RenderContext::Shutdown()
{
    if (!m_initialized)
        return;

    RVX_CORE_DEBUG("RenderContext shutting down...");

    // Wait for all GPU work to complete
    WaitIdle();

    // Destroy resources in reverse order
    DestroyCommandContexts();
    m_swapChain.Reset();
    m_frameSynchronizer.Shutdown();
    m_device.reset();

    m_initialized = false;
    m_frameActive = false;
    m_frameReadyToPresent = false;
    m_surface = {};

    RVX_CORE_INFO("RenderContext shutdown complete");
}

bool RenderContext::CreateSwapChain(const NativeSurfaceDesc& surface)
{
    if (!m_device)
    {
        RVX_CORE_ERROR("RenderContext: Cannot create swap chain without device");
        return false;
    }

    const RHIBackendType backend = m_device->GetBackendType();
    if (!surface.IsValidFor(backend))
    {
        RVX_CORE_ERROR("RenderContext: Invalid native surface for {}", ToString(backend));
        return false;
    }

    if (m_swapChain && surface.generation <= m_surface.generation)
    {
        RVX_CORE_WARN("RenderContext: Ignoring stale surface generation {} (current {})",
                      surface.generation,
                      m_surface.generation);
        return false;
    }

    if (m_swapChain &&
        !m_device->SupportsSurfaceRebind(m_surface, surface))
    {
        RVX_CORE_WARN(
            "RenderContext: Surface replacement requires device recreation for {}",
            ToString(backend));
        return false;
    }

    // Destroy existing swap chain
    if (m_swapChain)
    {
        if (!WaitForSurfaceGeneration())
        {
            RVX_CORE_ERROR("RenderContext: Surface generation completion was lost");
            return false;
        }
        m_swapChain.Reset();
        m_surface = {};
    }

    RHISwapChainDesc swapChainDesc;
    swapChainDesc.surface = surface;
    swapChainDesc.bufferCount = m_config.frameBuffering + 1;  // One extra for presentation
    swapChainDesc.debugName = "MainSwapChain";

    m_swapChain = m_device->CreateSwapChain(swapChainDesc);
    if (!m_swapChain)
    {
        RVX_CORE_ERROR("RenderContext: Failed to create swap chain");
        return false;
    }

    m_surface = surface;
    RVX_CORE_INFO("RenderContext: Created swap chain {}x{}",
                  surface.width,
                  surface.height);
    return true;
}

bool RenderContext::ResizeSwapChain(const NativeSurfaceDesc& surface)
{
    if (!m_swapChain || !m_device)
    {
        RVX_CORE_ERROR("RenderContext: Cannot update a missing swap chain");
        return false;
    }
    if (!surface.IsValidFor(m_device->GetBackendType()))
    {
        RVX_CORE_ERROR("RenderContext: Invalid native surface resize update");
        return false;
    }
    if (ClassifyNativeSurfaceUpdate(m_surface, surface) !=
        NativeSurfaceUpdateKind::Resize)
    {
        RVX_CORE_WARN("RenderContext: Surface update is not a newer extent-only change");
        return false;
    }

    ResizeSwapChain(surface.width, surface.height);
    m_surface = surface;
    return true;
}

void RenderContext::ResizeSwapChain(uint32_t width, uint32_t height)
{
    if (!m_swapChain)
    {
        RVX_CORE_WARN("RenderContext: No swap chain to resize");
        return;
    }

    if (width == 0 || height == 0)
    {
        RVX_CORE_WARN("RenderContext: Invalid resize dimensions {}x{}", width, height);
        return;
    }

    RVX_CORE_INFO("RenderContext: Resizing swap chain to {}x{}", width, height);

    // Retire the old surface generation from exact Graphics completion evidence.
    if (!WaitForSurfaceGeneration())
    {
        RVX_CORE_ERROR("RenderContext: Swap-chain resize completion was lost");
        return;
    }

    m_swapChain->Resize(width, height);
    m_surface.width = width;
    m_surface.height = height;
}

bool RenderContext::BeginFrame()
{
    if (m_frameActive)
    {
        RVX_CORE_WARN("RenderContext: BeginFrame called while frame already active");
        return false;
    }

    m_frameReadyToPresent = false;
    if (!m_initialized || !m_device)
    {
        RVX_CORE_WARN("RenderContext: BeginFrame called before initialization");
        return false;
    }

    // Wait for this frame slot to be available
    if (!m_frameSynchronizer.WaitForFrame(m_frameIndex))
    {
        RVX_CORE_ERROR("RenderContext: Frame slot {} completion was lost", m_frameIndex);
        return false;
    }

    // Begin device frame
    m_device->BeginFrame();

    // Reset and begin the command context for this frame
    RHICommandContext* ctx = GetGraphicsContext();
    if (!ctx)
    {
        RVX_CORE_ERROR("RenderContext: No Graphics command context for frame {}", m_frameIndex);
        m_device->EndFrame();
        return false;
    }
    ctx->Reset();
    ctx->Begin();

    m_frameActive = true;
    return true;
}

GPUCompletionPoint RenderContext::EndFrame()
{
    if (!m_frameActive)
    {
        RVX_CORE_WARN("RenderContext: EndFrame called without BeginFrame");
        m_frameReadyToPresent = false;
        return {};
    }

    // End the command context
    RHICommandContext* ctx = GetGraphicsContext();
    if (ctx)
    {
        ctx->End();
    }

    // Submit commands
    GPUCompletionPoint submittedPoint;
    if (m_device && ctx)
    {
        submittedPoint = m_frameSynchronizer.SubmitGraphics(ctx);
        if (submittedPoint.domain == GPUQueueDomain::Graphics && submittedPoint.value != 0)
        {
            m_frameSynchronizer.SignalFrame(m_frameIndex, submittedPoint);
        }
    }

    // End device frame
    if (m_device)
    {
        m_device->EndFrame();
    }

    m_frameActive = false;
    m_frameReadyToPresent =
        submittedPoint.domain == GPUQueueDomain::Graphics && submittedPoint.value != 0;
    return submittedPoint;
}

void RenderContext::AbortFrame()
{
    if (!m_frameActive)
    {
        return;
    }
    if (RHICommandContext* context = GetGraphicsContext())
    {
        context->End();
    }
    if (m_device)
    {
        m_device->EndFrame();
    }
    m_frameActive = false;
    m_frameReadyToPresent = false;
}

void RenderContext::Present()
{
    if (!m_frameReadyToPresent)
    {
        RVX_CORE_WARN("RenderContext: Present called without a completed frame submission");
        return;
    }

    if (m_swapChain)
    {
        m_swapChain->Present();
    }

    // Advance frame index
    m_frameIndex = (m_frameIndex + 1) % m_frameSynchronizer.GetFrameCount();
    m_frameNumber++;
    m_frameReadyToPresent = false;
}

void RenderContext::WaitIdle()
{
    m_frameSynchronizer.WaitForAllFrames();
    
    if (m_device)
    {
        m_device->WaitIdle();
    }
}

RHICommandContext* RenderContext::GetGraphicsContext() const
{
    if (m_frameIndex >= RVX_MAX_FRAME_COUNT)
        return nullptr;
    return m_graphicsContexts[m_frameIndex].Get();
}

RHICommandContext* RenderContext::GetComputeContext() const
{
    if (!m_supportsAsyncCompute || m_frameIndex >= RVX_MAX_FRAME_COUNT)
        return nullptr;
    return m_computeContexts[m_frameIndex].Get();
}

RHITexture* RenderContext::GetCurrentBackBuffer() const
{
    if (!m_swapChain)
        return nullptr;
    return m_swapChain->GetCurrentBackBuffer();
}

RHITextureView* RenderContext::GetCurrentBackBufferView() const
{
    if (!m_swapChain)
        return nullptr;
    return m_swapChain->GetCurrentBackBufferView();
}

void RenderContext::CreateCommandContexts()
{
    if (!m_device)
        return;

    uint32_t frameCount = m_frameSynchronizer.GetFrameCount();
    for (uint32_t i = 0; i < frameCount; ++i)
    {
        // Create graphics context
        m_graphicsContexts[i] = m_device->CreateCommandContext(RHICommandQueueType::Graphics);
        if (!m_graphicsContexts[i])
        {
            RVX_CORE_ERROR("RenderContext: Failed to create graphics context for frame {}", i);
        }

        // Create compute context if async compute is supported
        if (m_device->GetCapabilities().supportsAsyncCompute)
        {
            m_computeContexts[i] = m_device->CreateCommandContext(RHICommandQueueType::Compute);
            if (!m_computeContexts[i])
            {
                RVX_CORE_WARN("RenderContext: Failed to create compute context for frame {}", i);
            }

        }
    }
}

void RenderContext::DestroyCommandContexts()
{
    for (uint32_t i = 0; i < RVX_MAX_FRAME_COUNT; ++i)
    {
        m_graphicsContexts[i].Reset();
        m_computeContexts[i].Reset();
    }
}

bool RenderContext::WaitForSurfaceGeneration()
{
    if (!m_frameSynchronizer.WaitForAllFrames())
    {
        return false;
    }

    // Compatibility trackers perform at most one bounded WaitIdle while
    // resolving the first pending Graphics point. Native timelines never
    // require a device-wide idle for surface-generation replacement.
    return true;
}

} // namespace RVX
