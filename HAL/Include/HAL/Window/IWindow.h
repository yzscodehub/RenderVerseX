#pragma once

/**
 * @file IWindow.h
 * @brief Platform-independent window interface
 */

#include "Core/Types.h"
#include <memory>

// Undefine Windows macros that conflict with our API
#ifdef CreateWindow
#undef CreateWindow
#endif

namespace RVX::HAL
{
    /**
     * @brief Window creation descriptor
     */
    struct WindowDesc
    {
        uint32 width = 1280;
        uint32 height = 720;
        const char* title = "RenderVerseX";
        bool resizable = true;
        bool fullscreen = false;
    };

    /**
     * @brief Abstract window interface
     * 
     * Platform-specific implementations (GLFW, Win32, etc.) derive from this.
     */
    class IWindow
    {
    public:
        virtual ~IWindow() = default;

        /// Poll and process platform events
        virtual void PollEvents() = 0;

        /// Check if window close was requested
        virtual bool ShouldClose() const = 0;

        /// Get the framebuffer size in pixels
        virtual void GetFramebufferSize(uint32& width, uint32& height) const = 0;

        /// Get DPI scale factor
        virtual float GetDpiScale() const = 0;

        /// Get native window handle (HWND, NSWindow*, etc.)
        virtual void* GetNativeHandle() const = 0;
        
        /// Get internal implementation handle (e.g., GLFWwindow* for GLFW backend)
        /// Used for input systems that need direct access to the windowing library
        virtual void* GetInternalHandle() const = 0;
    };

    /**
     * @brief Create a platform window
     * @param desc Window creation parameters
     * @return Unique pointer to the created window
     */
    std::unique_ptr<IWindow> CreateWindow(const WindowDesc& desc);

} // namespace RVX::HAL

// Backward compatibility - expose in RVX namespace
namespace RVX
{
    using WindowDesc = HAL::WindowDesc;
    using Window = HAL::IWindow;
    using HAL::CreateWindow;
}
