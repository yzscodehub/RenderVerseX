#pragma once

#include "Platform/InputBackend.h"
#include <GLFW/glfw3.h>

namespace RVX
{
    class GlfwInputBackend final : public InputBackend
    {
    public:
        explicit GlfwInputBackend(GLFWwindow* window);
        void Poll(InputState& state) override;

    private:
        static void ScrollCallback(GLFWwindow* window, double xoffset, double yoffset);

        GLFWwindow* m_window = nullptr;
        double m_lastX = 0.0;
        double m_lastY = 0.0;
        bool m_firstSample = true;
        float m_scrollDelta = 0.0f;
    };
} // namespace RVX
