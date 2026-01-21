#pragma once

/**
 * @file WindowSubsystem.h
 * @brief Window management subsystem
 */

#include "Core/Subsystem/EngineSubsystem.h"
#include "Core/Event/EventBus.h"
#include "HAL/Window/IWindow.h"
#include "HAL/Window/WindowEvents.h"
#include <memory>
#include <functional>

namespace RVX
{
    /**
     * @brief Window configuration
     */
    struct WindowConfig
    {
        uint32_t width = 1280;
        uint32_t height = 720;
        const char* title = "RenderVerseX";
        bool resizable = true;
        bool fullscreen = false;
        bool vsync = true;
    };

    /**
     * @brief Window subsystem - manages application window lifecycle
     * 
     * This subsystem wraps the platform-specific Window implementation
     * and provides event-based notifications for window state changes.
     * 
     * Usage:
     * @code
     * auto* windowSys = engine.GetSubsystem<WindowSubsystem>();
     * 
     * // Get window handle
     * Window* window = windowSys->GetWindow();
     * 
     * // Check if should close
     * if (windowSys->ShouldClose()) {
     *     engine.RequestShutdown();
     * }
     * @endcode
     */
    class WindowSubsystem : public EngineSubsystem
    {
    public:
        const char* GetName() const override { return "WindowSubsystem"; }
        
        void Initialize() override;
        void Deinitialize() override;
        void Tick(float deltaTime) override;
        bool ShouldTick() const override { return true; }

        /// Set window configuration (call before Initialize)
        void SetConfig(const WindowConfig& config) { m_config = config; }
        const WindowConfig& GetConfig() const { return m_config; }

        /// Get the underlying window
        HAL::IWindow* GetWindow() const { return m_window.get(); }

        /// Check if window should close
        bool ShouldClose() const;

        /// Get framebuffer size
        void GetFramebufferSize(uint32_t& width, uint32_t& height) const;

        /// Get DPI scale
        float GetDpiScale() const;

        /// Get native window handle
        void* GetNativeHandle() const;

    private:
        WindowConfig m_config;
        std::unique_ptr<HAL::IWindow> m_window;
        uint32_t m_lastWidth = 0;
        uint32_t m_lastHeight = 0;
    };

} // namespace RVX
