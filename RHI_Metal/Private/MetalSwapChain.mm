#include "MetalSwapChain.h"
#include "MetalCommandContext.h"
#include "MetalDevice.h"
#include "MetalResources.h"
#include "MetalConversions.h"

#include <algorithm>

namespace RVX
{
    MetalSwapChain::MetalSwapChain(MetalDevice* device, const RHISwapChainDesc& desc)
        : m_device(device)
        , m_width(desc.surface.width)
        , m_height(desc.surface.height)
        , m_format(desc.surface.preferredFormat)
        , m_bufferCount(std::clamp(desc.bufferCount, 2u, 3u))
        , m_vsync(desc.surface.vsync)
    {
        if (!desc.surface.IsValidFor(RHIBackendType::Metal))
        {
            RVX_RHI_ERROR("MetalSwapChain: Invalid HAL-owned presentation layer");
            return;
        }

        m_metalLayer = (__bridge CAMetalLayer*)
            reinterpret_cast<void*>(desc.surface.nativeLayer);
        m_metalLayer.device = device->GetMTLDevice();
        m_metalLayer.pixelFormat =
            ToMTLPixelFormat(desc.surface.preferredFormat);
        m_metalLayer.drawableSize =
            CGSizeMake(desc.surface.width, desc.surface.height);
        m_metalLayer.maximumDrawableCount = m_bufferCount;
        m_metalLayer.framebufferOnly = YES;
#if RVX_PLATFORM_MACOS
        m_metalLayer.displaySyncEnabled = desc.surface.vsync;
#endif

        CreateBackBuffers();

        RVX_RHI_INFO("Created Metal SwapChain: {}x{}, {} buffers, format {}",
            m_width, m_height, m_bufferCount, static_cast<int>(m_format));
    }

    MetalSwapChain::~MetalSwapChain()
    {
        m_backBufferViews.clear();
        m_backBuffers.clear();
        m_currentDrawable = nil;
        m_metalLayer = nil;
    }

    void MetalSwapChain::CreateBackBuffers()
    {
        m_backBuffers.clear();
        m_backBufferViews.clear();

        // Metal's drawable textures are obtained dynamically in AcquireNextImage
        // We'll create placeholder texture objects that wrap the drawable textures
        m_backBuffers.resize(m_bufferCount);
        m_backBufferViews.resize(m_bufferCount);
    }

    RHITexture* MetalSwapChain::GetCurrentBackBuffer()
    {
        // Acquire drawable on demand if needed
        if (!m_currentDrawable)
        {
            @autoreleasepool
            {
                m_currentDrawable = [m_metalLayer nextDrawable];
                if (!m_currentDrawable)
                {
                    RVX_RHI_WARN("Failed to acquire Metal drawable");
                    return nullptr;
                }

                // Create texture wrapper for current drawable
                RHITextureDesc texDesc;
                texDesc.width = m_width;
                texDesc.height = m_height;
                texDesc.depth = 1;
                texDesc.format = m_format;
                texDesc.dimension = RHITextureDimension::Texture2D;
                texDesc.usage = RHITextureUsage::RenderTarget;
                texDesc.mipLevels = 1;
                texDesc.arraySize = 1;

                // Wrap drawable texture
                m_backBuffers[m_currentBackBufferIndex] = MakeRef<MetalTexture>(
                    [m_currentDrawable texture], texDesc);

                // Create view
                RHITextureViewDesc viewDesc;
                viewDesc.format = m_format;
                viewDesc.type = RHITextureViewType::RenderTarget;
                m_backBufferViews[m_currentBackBufferIndex] = MakeRef<MetalTextureView>(
                    static_cast<MetalTexture*>(m_backBuffers[m_currentBackBufferIndex].Get()),
                    viewDesc);
            }
        }

        if (m_currentBackBufferIndex < m_backBuffers.size() && m_backBuffers[m_currentBackBufferIndex])
        {
            return m_backBuffers[m_currentBackBufferIndex].Get();
        }
        return nullptr;
    }

    RHITextureView* MetalSwapChain::GetCurrentBackBufferView()
    {
        // Ensure drawable is acquired
        GetCurrentBackBuffer();

        if (m_currentBackBufferIndex < m_backBufferViews.size() && m_backBufferViews[m_currentBackBufferIndex])
        {
            return m_backBufferViews[m_currentBackBufferIndex].Get();
        }
        return nullptr;
    }

    void MetalSwapChain::Resize(uint32 width, uint32 height)
    {
        if (width == m_width && height == m_height)
            return;

        m_width = width;
        m_height = height;

        m_metalLayer.drawableSize = CGSizeMake(width, height);

        CreateBackBuffers();

        RVX_RHI_INFO("Resized Metal SwapChain to {}x{}", width, height);
    }

    void MetalSwapChain::Present()
    {
        // Presentation remains command-buffer owned and follows queued render work.
        if (m_currentDrawable)
        {
            MetalCommandContext presentationContext(
                m_device, RHICommandQueueType::Graphics);
            presentationContext.Begin();
            presentationContext.SetPresentationDrawable(m_currentDrawable);
            presentationContext.Submit(nullptr);
        }

        AdvanceFrame();
    }

    void MetalSwapChain::AdvanceFrame()
    {
        // Clear current drawable reference and advance to next frame
        m_currentDrawable = nil;
        m_currentBackBufferIndex = (m_currentBackBufferIndex + 1) % m_bufferCount;
    }

} // namespace RVX
