#pragma once

/**
 * @file GLFWInputBackend.h
 * @brief GLFW-based input backend implementation
 */

#include "HAL/Input/IInputBackend.h"
#include "HAL/Input/InputState.h"
#include <GLFW/glfw3.h>
#include <unordered_map>
#include <mutex>

namespace RVX::HAL
{
    /**
     * @brief GLFW input backend implementation
     * 
     * Note: Uses a static map for scroll callbacks because GLFWWindow
     * already uses glfwSetWindowUserPointer for its own callbacks.
     */
    class GLFWInputBackend final : public IInputBackend
    {
    public:
        explicit GLFWInputBackend(GLFWwindow* window);
        ~GLFWInputBackend() override;

        void Poll(InputState& state) override;

    private:
        static void ScrollCallback(GLFWwindow* window, double xoffset, double yoffset);
        
        // Static registry for scroll callbacks (avoids conflicting with window's user pointer)
        static std::unordered_map<GLFWwindow*, GLFWInputBackend*> s_instances;
        static std::mutex s_instancesMutex;

        GLFWwindow* m_window = nullptr;
        double m_lastX = 0.0;
        double m_lastY = 0.0;
        bool m_firstSample = true;
        float m_scrollDelta = 0.0f;
    };

} // namespace RVX::HAL

// Backward compatibility
namespace RVX
{
    using GlfwInputBackend = HAL::GLFWInputBackend;
}
