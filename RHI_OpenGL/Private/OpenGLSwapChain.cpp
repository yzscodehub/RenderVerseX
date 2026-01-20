#include "OpenGLSwapChain.h"
#include "OpenGLDevice.h"
#include "Core/Log.h"

namespace RVX
{
    OpenGLSwapChain::OpenGLSwapChain(OpenGLDevice* device, const RHISwapChainDesc& desc)
        : m_device(device)
        , m_width(desc.width)
        , m_height(desc.height)
        , m_format(desc.format)
        , m_bufferCount(desc.bufferCount)
        , m_vsync(desc.vsync)
    {
        // In OpenGL with GLFW, the window handle is the GLFWwindow*
        m_window = static_cast<GLFWwindow*>(desc.windowHandle);
        
        if (!m_window)
        {
            RVX_RHI_ERROR("OpenGLSwapChain: Invalid window handle");
            return;
        }

        // Set VSync
        glfwSwapInterval(m_vsync ? 1 : 0);

        // Get actual framebuffer size
        int fbWidth, fbHeight;
        glfwGetFramebufferSize(m_window, &fbWidth, &fbHeight);
        m_width = static_cast<uint32>(fbWidth);
        m_height = static_cast<uint32>(fbHeight);

        // Create proxy textures for the back buffers
        CreateBackBufferProxies();

        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        RVX_RHI_INFO("Created OpenGL SwapChain: {}x{}, format={}, buffers={}, vsync={}",
                    m_width, m_height, static_cast<int>(m_format), m_bufferCount, m_vsync);
    }

    OpenGLSwapChain::~OpenGLSwapChain()
    {
        DestroyBackBufferProxies();
        RVX_RHI_DEBUG("Destroyed OpenGL SwapChain");
    }

    void OpenGLSwapChain::CreateBackBufferProxies()
    {
        DestroyBackBufferProxies();

        // Create proxy texture descriptions
        RHITextureDesc texDesc;
        texDesc.width = m_width;
        texDesc.height = m_height;
        texDesc.depth = 1;
        texDesc.mipLevels = 1;
        texDesc.arraySize = 1;
        texDesc.format = m_format;
        texDesc.dimension = RHITextureDimension::Texture2D;
        texDesc.usage = RHITextureUsage::RenderTarget;
        texDesc.sampleCount = RHISampleCount::Count1;

        // For OpenGL, we don't actually have separate back buffer textures
        // The default framebuffer (FBO 0) IS the back buffer
        // We create a single proxy texture that represents it
        for (uint32 i = 0; i < m_bufferCount; ++i)
        {
            texDesc.debugName = "SwapChain_BackBuffer";
            
            // Create a "fake" texture that represents the default framebuffer
            // In OpenGL, we render to FBO 0 for the swap chain
            // Note: OpenGLTexture with handle 0 represents the default framebuffer
            auto texture = OpenGLTexture::CreateFromExisting(m_device, 0, GL_TEXTURE_2D, texDesc);
            m_backBufferTextures.push_back(texture);

            RHITextureViewDesc viewDesc;
            viewDesc.format = m_format;
            viewDesc.debugName = "SwapChain_BackBufferView";
            
            // Create a view that just wraps the texture
            auto view = MakeRef<OpenGLTextureView>(m_device, texture.Get(), viewDesc);
            m_backBufferViews.push_back(view);
        }

        RVX_RHI_DEBUG("Created {} back buffer proxies", m_bufferCount);
    }

    void OpenGLSwapChain::DestroyBackBufferProxies()
    {
        m_backBufferViews.clear();
        m_backBufferTextures.clear();
    }

    RHITexture* OpenGLSwapChain::GetCurrentBackBuffer()
    {
        if (m_currentBufferIndex < m_backBufferTextures.size())
        {
            return m_backBufferTextures[m_currentBufferIndex].Get();
        }
        return nullptr;
    }

    RHITextureView* OpenGLSwapChain::GetCurrentBackBufferView()
    {
        if (m_currentBufferIndex < m_backBufferViews.size())
        {
            return m_backBufferViews[m_currentBufferIndex].Get();
        }
        return nullptr;
    }

    void OpenGLSwapChain::Present()
    {
        GL_DEBUG_SCOPE("Present");

        if (!m_window)
        {
            RVX_RHI_ERROR("SwapChain::Present: No window");
            return;
        }

        // Swap buffers (this is where the actual present happens)
        glfwSwapBuffers(m_window);

        // Advance to next buffer (for tracking purposes)
        m_currentBufferIndex = (m_currentBufferIndex + 1) % m_bufferCount;
    }

    void OpenGLSwapChain::Resize(uint32 width, uint32 height)
    {
        if (width == 0 || height == 0)
        {
            RVX_RHI_WARN("SwapChain::Resize: Invalid size {}x{}", width, height);
            return;
        }

        if (width == m_width && height == m_height)
        {
            return;  // No change
        }

        RVX_RHI_INFO("SwapChain resizing: {}x{} -> {}x{}", m_width, m_height, width, height);

        m_width = width;
        m_height = height;

        // Recreate proxy textures with new size
        CreateBackBufferProxies();

        // Reset current buffer index
        m_currentBufferIndex = 0;
    }

} // namespace RVX
