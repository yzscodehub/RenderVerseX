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

bool RenderContext::Initialize(const RenderContextConfig& config)
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

    m_initialized = true;
    m_frameIndex = 0;
    m_frameNumber = 0;

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

    RVX_CORE_INFO("RenderContext shutdown complete");
}

bool RenderContext::CreateSwapChain(void* windowHandle, uint32_t width, uint32_t height)
{
    if (!m_device)
    {
        RVX_CORE_ERROR("RenderContext: Cannot create swap chain without device");
        return false;
    }

    if (!windowHandle)
    {
        RVX_CORE_ERROR("RenderContext: Invalid window handle");
        return false;
    }

    // Destroy existing swap chain
    if (m_swapChain)
    {
        WaitIdle();
        m_swapChain.Reset();
    }

    RHISwapChainDesc swapChainDesc;
    swapChainDesc.windowHandle = windowHandle;
    swapChainDesc.width = width;
    swapChainDesc.height = height;
    swapChainDesc.format = RHIFormat::BGRA8_UNORM;
    swapChainDesc.bufferCount = m_config.frameBuffering + 1;  // One extra for presentation
    swapChainDesc.vsync = m_config.vsync;
    swapChainDesc.debugName = "MainSwapChain";

    m_swapChain = m_device->CreateSwapChain(swapChainDesc);
    if (!m_swapChain)
    {
        RVX_CORE_ERROR("RenderContext: Failed to create swap chain");
        return false;
    }

    RVX_CORE_INFO("RenderContext: Created swap chain {}x{}", width, height);
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

    // Wait for all frames to complete before resizing
    WaitIdle();

    m_swapChain->Resize(width, height);
}

void RenderContext::BeginFrame()
{
    if (m_frameActive)
    {
        RVX_CORE_WARN("RenderContext: BeginFrame called while frame already active");
        return;
    }

    // Wait for this frame slot to be available
    m_frameSynchronizer.WaitForFrame(m_frameIndex);

    // Begin device frame
    if (m_device)
    {
        m_device->BeginFrame();
    }

    // Reset and begin the command context for this frame
    RHICommandContext* ctx = GetGraphicsContext();
    if (ctx)
    {
        ctx->Reset();
        ctx->Begin();
    }

    m_frameActive = true;
}

void RenderContext::EndFrame()
{
    if (!m_frameActive)
    {
        RVX_CORE_WARN("RenderContext: EndFrame called without BeginFrame");
        return;
    }

    // End the command context
    RHICommandContext* ctx = GetGraphicsContext();
    if (ctx)
    {
        ctx->End();
    }

    // Submit commands
    if (m_device && ctx)
    {
        RHIFence* fence = m_frameSynchronizer.GetFence(m_frameIndex);
        m_device->SubmitCommandContext(ctx, fence);
    }

    // Signal frame completion
    m_frameSynchronizer.SignalFrame(m_frameIndex);

    // End device frame
    if (m_device)
    {
        m_device->EndFrame();
    }

    m_frameActive = false;
}

void RenderContext::Present()
{
    if (m_swapChain)
    {
        m_swapChain->Present();
    }

    // Advance frame index
    m_frameIndex = (m_frameIndex + 1) % m_frameSynchronizer.GetFrameCount();
    m_frameNumber++;
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
        m_graphicsContexts[i] = m_device->CreateCommandContext(RHICommandQueueType::Graphics);
        if (!m_graphicsContexts[i])
        {
            RVX_CORE_ERROR("RenderContext: Failed to create command context for frame {}", i);
        }
    }
}

void RenderContext::DestroyCommandContexts()
{
    for (uint32_t i = 0; i < RVX_MAX_FRAME_COUNT; ++i)
    {
        m_graphicsContexts[i].Reset();
    }
}

} // namespace RVX
