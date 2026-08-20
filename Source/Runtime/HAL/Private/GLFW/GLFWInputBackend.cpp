#include "GLFWInputBackend.h"

namespace RVX::HAL
{
    // Static member definitions
    std::unordered_map<GLFWwindow*, GLFWInputBackend*> GLFWInputBackend::s_instances;
    std::mutex GLFWInputBackend::s_instancesMutex;

    GLFWInputBackend::GLFWInputBackend(GLFWwindow* window) 
        : m_window(window)
    {
        if (m_window)
        {
            // Register this instance in the static map (don't touch user pointer!)
            {
                std::lock_guard<std::mutex> lock(s_instancesMutex);
                s_instances[m_window] = this;
            }
            glfwSetScrollCallback(m_window, &GLFWInputBackend::ScrollCallback);
        }
    }

    GLFWInputBackend::~GLFWInputBackend()
    {
        if (m_window)
        {
            // Unregister from static map
            std::lock_guard<std::mutex> lock(s_instancesMutex);
            s_instances.erase(m_window);
        }
    }

    void GLFWInputBackend::Poll(InputState& state)
    {
        if (!m_window)
            return;

        // Get cursor position
        double x = 0.0;
        double y = 0.0;
        glfwGetCursorPos(m_window, &x, &y);
        
        if (m_firstSample)
        {
            m_lastX = x;
            m_lastY = y;
            m_firstSample = false;
        }

        // Calculate delta
        state.mouseDeltaX += static_cast<float>(x - m_lastX);
        state.mouseDeltaY += static_cast<float>(y - m_lastY);
        state.mouseX = static_cast<float>(x);
        state.mouseY = static_cast<float>(y);
        m_lastX = x;
        m_lastY = y;

        // Mouse buttons
        state.mouseButtons[0] = glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        state.mouseButtons[1] = glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
        state.mouseButtons[2] = glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;

        // Keyboard keys
        for (int key = 0; key < static_cast<int>(MAX_KEYS); ++key)
        {
            state.keys[key] = glfwGetKey(m_window, key) == GLFW_PRESS;
        }

        // Scroll wheel
        state.mouseWheel += m_scrollDelta;
        m_scrollDelta = 0.0f;
    }

    void GLFWInputBackend::ScrollCallback(GLFWwindow* window, double xoffset, double yoffset)
    {
        (void)xoffset;
        
        // Look up backend from static map instead of user pointer
        std::lock_guard<std::mutex> lock(s_instancesMutex);
        auto it = s_instances.find(window);
        if (it != s_instances.end() && it->second)
        {
            it->second->m_scrollDelta += static_cast<float>(yoffset);
        }
    }

} // namespace RVX::HAL
