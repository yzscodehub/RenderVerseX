#pragma once

/**
 * @file GLFWWindow.h
 * @brief GLFW-based window implementation
 */

#include "HAL/Window/IWindow.h"
#include <GLFW/glfw3.h>

namespace RVX::HAL
{
    /**
     * @brief GLFW window implementation
     */
    class GLFWWindow final : public IWindow
    {
    public:
        explicit GLFWWindow(const WindowDesc& desc);
        ~GLFWWindow() override;

        // Non-copyable
        GLFWWindow(const GLFWWindow&) = delete;
        GLFWWindow& operator=(const GLFWWindow&) = delete;

        void PollEvents() override;
        bool ShouldClose() const override;
        void GetFramebufferSize(uint32& width, uint32& height) const override;
        float GetDpiScale() const override;
        void* GetNativeHandle() const override;
        void* GetInternalHandle() const override { return m_window; }

        /// Get the underlying GLFW window handle
        GLFWwindow* GetGLFWWindow() const { return m_window; }

    private:
        static void FramebufferSizeCallback(GLFWwindow* window, int width, int height);
        static void WindowCloseCallback(GLFWwindow* window);
        static void WindowFocusCallback(GLFWwindow* window, int focused);

        GLFWwindow* m_window = nullptr;
        WindowDesc m_desc;
    };

} // namespace RVX::HAL
