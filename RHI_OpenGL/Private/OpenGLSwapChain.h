#pragma once

#include "OpenGLCommon.h"
#include "OpenGLDebug.h"
#include "OpenGLResources.h"
#include "RHI/RHISwapChain.h"
#include <vector>

namespace RVX
{
    class OpenGLDevice;

    // =============================================================================
    // OpenGL SwapChain
    // Uses GLFW's double-buffering and glfwSwapBuffers
    // =============================================================================
    class OpenGLSwapChain : public RHISwapChain
    {
    public:
        OpenGLSwapChain(OpenGLDevice* device, const RHISwapChainDesc& desc);
        ~OpenGLSwapChain() override;

        // RHISwapChain interface
        RHITexture* GetCurrentBackBuffer() override;
        RHITextureView* GetCurrentBackBufferView() override;
        uint32 GetCurrentBackBufferIndex() const override { return m_currentBufferIndex; }

        void Present() override;
        void Resize(uint32 width, uint32 height) override;

        uint32 GetWidth() const override { return m_width; }
        uint32 GetHeight() const override { return m_height; }
        RHIFormat GetFormat() const override { return m_format; }
        uint32 GetBufferCount() const override { return m_bufferCount; }

        // OpenGL specific
        GLFWwindow* GetWindow() const { return m_window; }

    private:
        void CreateBackBufferProxies();
        void DestroyBackBufferProxies();

        OpenGLDevice* m_device = nullptr;
        GLFWwindow* m_window = nullptr;

        uint32 m_width = 0;
        uint32 m_height = 0;
        RHIFormat m_format = RHIFormat::RGBA8_UNORM;
        uint32 m_bufferCount = 2;
        bool m_vsync = true;

        uint32 m_currentBufferIndex = 0;

        // Proxy textures representing the back buffer
        // OpenGL doesn't have explicit back buffer textures like other APIs
        // We create proxy textures that point to FBO 0 (default framebuffer)
        std::vector<Ref<OpenGLTexture>> m_backBufferTextures;
        std::vector<Ref<OpenGLTextureView>> m_backBufferViews;
    };

} // namespace RVX
