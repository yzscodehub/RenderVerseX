#include "GLFWWindow.h"
#include "HAL/Window/WindowEvents.h"
#include "Core/Event/EventBus.h"
#include "Core/Log.h"

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__APPLE__)
#include "Apple/GLFWMetalLayerBridge.h"
#else
#define GLFW_EXPOSE_NATIVE_X11
#define GLFW_EXPOSE_NATIVE_WAYLAND
#endif

#if !defined(__APPLE__)
#include <GLFW/glfw3native.h>
#endif

#ifdef _WIN32
// Undefine Windows macros that conflict with our function names
#ifdef CreateWindow
#undef CreateWindow
#endif
#endif

namespace RVX::HAL
{
    // Static GLFW initialization counter
    static int s_glfwInitCount = 0;

    GLFWWindow::GLFWWindow(const WindowDesc& desc)
        : m_desc(desc)
    {
        // Initialize GLFW if needed
        if (s_glfwInitCount == 0)
        {
            if (!glfwInit())
            {
                LOG_ERROR("Failed to initialize GLFW");
                return;
            }
        }
        ++s_glfwInitCount;

        // Set window hints
        glfwDefaultWindowHints();
        if (desc.graphicsApi == WindowGraphicsApi::OpenGL)
        {
            glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
            glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

#ifdef __APPLE__
            glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif
        }
        else
        {
            glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        }

        glfwWindowHint(GLFW_RESIZABLE, desc.resizable ? GLFW_TRUE : GLFW_FALSE);

        // Create window
        m_window = glfwCreateWindow(
            static_cast<int>(desc.width),
            static_cast<int>(desc.height),
            desc.title,
            desc.fullscreen ? glfwGetPrimaryMonitor() : nullptr,
            nullptr
        );

        if (!m_window)
        {
            LOG_ERROR("Failed to create GLFW window");
            return;
        }

        if (desc.graphicsApi == WindowGraphicsApi::OpenGL)
        {
            glfwMakeContextCurrent(m_window);
            glfwSwapInterval(desc.vsync ? 1 : 0);
        }

        // Set user pointer for callbacks
        glfwSetWindowUserPointer(m_window, this);

        // Set callbacks
        glfwSetFramebufferSizeCallback(m_window, FramebufferSizeCallback);
        glfwSetWindowCloseCallback(m_window, WindowCloseCallback);
        glfwSetWindowFocusCallback(m_window, WindowFocusCallback);

        LOG_INFO("Created window: {} ({}x{})", desc.title, desc.width, desc.height);
    }

    GLFWWindow::~GLFWWindow()
    {
        if (m_window)
        {
#ifdef __APPLE__
            DetachGLFWMetalLayer(m_window);
#endif
            glfwDestroyWindow(m_window);
            m_window = nullptr;
        }

        --s_glfwInitCount;
        if (s_glfwInitCount == 0)
        {
            glfwTerminate();
        }
    }

    void GLFWWindow::PollEvents()
    {
        glfwPollEvents();
    }

    bool GLFWWindow::ShouldClose() const
    {
        return m_window ? glfwWindowShouldClose(m_window) != 0 : true;
    }

    void GLFWWindow::GetFramebufferSize(uint32& width, uint32& height) const
    {
        if (m_window)
        {
            int w = 0, h = 0;
            glfwGetFramebufferSize(m_window, &w, &h);
            width = static_cast<uint32>(w);
            height = static_cast<uint32>(h);
        }
        else
        {
            width = m_desc.width;
            height = m_desc.height;
        }
    }

    float GLFWWindow::GetDpiScale() const
    {
        if (m_window)
        {
            float xscale = 1.0f, yscale = 1.0f;
            glfwGetWindowContentScale(m_window, &xscale, &yscale);
            return xscale;
        }
        return 1.0f;
    }

    void* GLFWWindow::GetNativeHandle() const
    {
#ifdef _WIN32
        return m_window ? glfwGetWin32Window(m_window) : nullptr;
#else
        return m_window;
#endif
    }

    WindowRenderSurfaceHandles GLFWWindow::CaptureRenderSurfaceHandles()
    {
        WindowRenderSurfaceHandles handles;
        if (!m_window)
        {
            return handles;
        }

        handles.backendWindow = reinterpret_cast<uintptr_t>(m_window);
        GetFramebufferSize(handles.width, handles.height);
        handles.contentScale = GetDpiScale();

#ifdef _WIN32
        handles.nativeWindow =
            reinterpret_cast<uintptr_t>(glfwGetWin32Window(m_window));
#elif defined(__APPLE__)
        handles.nativeLayer =
            AttachGLFWMetalLayer(m_window, handles.nativeWindow);
#else
        if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND)
        {
            handles.nativeDisplay =
                reinterpret_cast<uintptr_t>(glfwGetWaylandDisplay());
            handles.nativeWindow =
                reinterpret_cast<uintptr_t>(glfwGetWaylandWindow(m_window));
        }
        else
        {
            handles.nativeDisplay =
                reinterpret_cast<uintptr_t>(glfwGetX11Display());
            handles.nativeWindow =
                static_cast<uintptr_t>(glfwGetX11Window(m_window));
        }
#endif

        return handles;
    }

    void GLFWWindow::ReleaseGraphicsContextFromCurrentThread()
    {
        if (m_window && m_desc.graphicsApi == WindowGraphicsApi::OpenGL)
        {
            glfwMakeContextCurrent(nullptr);
        }
    }

    void GLFWWindow::FramebufferSizeCallback(GLFWwindow* window, int width, int height)
    {
        auto* self = static_cast<GLFWWindow*>(glfwGetWindowUserPointer(window));
        if (self)
        {
            self->m_desc.width = static_cast<uint32>(width);
            self->m_desc.height = static_cast<uint32>(height);

            // Publish resize event
            EventBus::Get().Publish(WindowResizedEvent{
                static_cast<uint32>(width),
                static_cast<uint32>(height)
            });
        }
    }

    void GLFWWindow::WindowCloseCallback(GLFWwindow* window)
    {
        (void)window;
        EventBus::Get().Publish(WindowClosedEvent{});
    }

    void GLFWWindow::WindowFocusCallback(GLFWwindow* window, int focused)
    {
        (void)window;
        EventBus::Get().Publish(WindowFocusEvent{focused != 0});
    }

    // Factory function
    std::unique_ptr<IWindow> CreateWindow(const WindowDesc& desc)
    {
        return std::make_unique<GLFWWindow>(desc);
    }

} // namespace RVX::HAL
