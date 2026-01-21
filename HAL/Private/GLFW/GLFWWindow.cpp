#include "GLFWWindow.h"
#include "HAL/Window/WindowEvents.h"
#include "Core/Event/EventBus.h"
#include "Core/Log.h"

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
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
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);  // No OpenGL context
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
