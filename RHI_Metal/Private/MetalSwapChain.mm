#include "MetalSwapChain.h"
#include "MetalDevice.h"
#include "MetalResources.h"
#include "MetalConversions.h"

namespace RVX
{
    MetalSwapChain::MetalSwapChain(MetalDevice* device, const RHISwapChainDesc& desc)
        : m_device(device)
        , m_width(desc.width)
        , m_height(desc.height)
        , m_format(desc.format)
        , m_bufferCount(desc.bufferCount)
        , m_vsync(desc.vsync)
    {
#if RVX_PLATFORM_MACOS
        // Get NSWindow from handle and create/get CAMetalLayer
        NSWindow* window = (__bridge NSWindow*)desc.windowHandle;
        if (!window)
        {
            RVX_RHI_ERROR("MetalSwapChain: Invalid window handle");
            return;
        }

        // Create CAMetalLayer
        m_metalLayer = [CAMetalLayer layer];
        m_metalLayer.device = device->GetMTLDevice();
        m_metalLayer.pixelFormat = ToMTLPixelFormat(desc.format);
        m_metalLayer.drawableSize = CGSizeMake(desc.width, desc.height);
        m_metalLayer.displaySyncEnabled = desc.vsync;
        m_metalLayer.framebufferOnly = YES;

        // Set layer on window's content view
        NSView* contentView = [window contentView];
        [contentView setWantsLayer:YES];
        [contentView setLayer:m_metalLayer];

#elif RVX_PLATFORM_IOS
        // Get UIView from handle
        UIView* view = (__bridge UIView*)desc.windowHandle;
        if (!view)
        {
            RVX_RHI_ERROR("MetalSwapChain: Invalid view handle");
            return;
        }

        m_metalLayer = (CAMetalLayer*)[view layer];
        m_metalLayer.device = device->GetMTLDevice();
        m_metalLayer.pixelFormat = ToMTLPixelFormat(desc.format);
        m_metalLayer.drawableSize = CGSizeMake(desc.width, desc.height);
        m_metalLayer.framebufferOnly = YES;
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
        // Present the drawable
        // Note: For optimal performance, presentDrawable should be called on the command buffer
        // before commit. This fallback uses immediate presentation.
        if (m_currentDrawable)
        {
            [m_currentDrawable present];
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
