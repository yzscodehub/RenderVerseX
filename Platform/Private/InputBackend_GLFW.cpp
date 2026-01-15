#include "Platform/InputBackend_GLFW.h"
#include "Input/InputState.h"

namespace RVX
{
    GlfwInputBackend::GlfwInputBackend(GLFWwindow* window) : m_window(window)
    {
        if (m_window)
        {
            glfwSetWindowUserPointer(m_window, this);
            glfwSetScrollCallback(m_window, &GlfwInputBackend::ScrollCallback);
        }
    }

    void GlfwInputBackend::Poll(InputState& state)
    {
        if (!m_window)
            return;

        double x = 0.0;
        double y = 0.0;
        glfwGetCursorPos(m_window, &x, &y);
        if (m_firstSample)
        {
            m_lastX = x;
            m_lastY = y;
            m_firstSample = false;
        }

        state.mouseDeltaX += static_cast<float>(x - m_lastX);
        state.mouseDeltaY += static_cast<float>(y - m_lastY);
        state.mouseX = static_cast<float>(x);
        state.mouseY = static_cast<float>(y);
        m_lastX = x;
        m_lastY = y;

        state.mouseButtons[0] = glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        state.mouseButtons[1] = glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
        state.mouseButtons[2] = glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;

        for (int key = 0; key < static_cast<int>(RVX_MAX_KEYS); ++key)
        {
            state.keys[key] = glfwGetKey(m_window, key) == GLFW_PRESS;
        }

        state.mouseWheel += m_scrollDelta;
        m_scrollDelta = 0.0f;
    }

    void GlfwInputBackend::ScrollCallback(GLFWwindow* window, double xoffset, double yoffset)
    {
        (void)xoffset;
        auto* backend = static_cast<GlfwInputBackend*>(glfwGetWindowUserPointer(window));
        if (!backend)
            return;
        backend->m_scrollDelta += static_cast<float>(yoffset);
    }
} // namespace RVX
