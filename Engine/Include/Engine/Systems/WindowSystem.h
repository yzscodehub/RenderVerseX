#pragma once

#include "Core/ISystem.h"
#include "Core/Types.h"
#include "Platform/Window.h"
#include <functional>

namespace RVX
{
    class WindowSystem : public ISystem
    {
    public:
        /// @brief Default constructor for Window pointer based usage (preferred)
        WindowSystem() = default;

        /// @brief Legacy constructor with callbacks (for backward compatibility)
        WindowSystem(std::function<void()> pollEvents,
                     std::function<bool()> shouldClose,
                     std::function<void(uint32&, uint32&)> getFramebufferSize = {})
            : m_pollEvents(std::move(pollEvents)),
              m_shouldClose(std::move(shouldClose)),
              m_getFramebufferSize(std::move(getFramebufferSize)) {}

        const char* GetName() const override { return "WindowSystem"; }

        /// @brief Set the window to manage (preferred over callbacks)
        void SetWindow(Window* window) { m_window = window; }
        Window* GetWindow() const { return m_window; }

        void OnUpdate(float) override
        {
            // Prefer Window pointer, fall back to callbacks
            if (m_window)
            {
                m_window->PollEvents();
                
                uint32 width = 0;
                uint32 height = 0;
                m_window->GetFramebufferSize(width, height);
                CheckResize(width, height);
            }
            else
            {
                if (m_pollEvents)
                    m_pollEvents();

                if (m_getFramebufferSize)
                {
                    uint32 width = 0;
                    uint32 height = 0;
                    m_getFramebufferSize(width, height);
                    CheckResize(width, height);
                }
            }
        }

        bool ShouldClose() const
        {
            if (m_window)
                return m_window->ShouldClose();
            return m_shouldClose ? m_shouldClose() : false;
        }

        bool ConsumeResize(uint32& width, uint32& height)
        {
            if (!m_hasResizeEvent)
                return false;
            width = m_framebufferWidth;
            height = m_framebufferHeight;
            m_hasResizeEvent = false;
            return true;
        }

        uint32 GetFramebufferWidth() const { return m_framebufferWidth; }
        uint32 GetFramebufferHeight() const { return m_framebufferHeight; }

    private:
        void CheckResize(uint32 width, uint32 height)
        {
            if (width > 0 && height > 0 &&
                (width != m_framebufferWidth || height != m_framebufferHeight))
            {
                m_framebufferWidth = width;
                m_framebufferHeight = height;
                m_hasResizeEvent = true;
            }
        }

        // Preferred: Window pointer
        Window* m_window = nullptr;

        // Legacy: Callbacks (for backward compatibility)
        std::function<void()> m_pollEvents;
        std::function<bool()> m_shouldClose;
        std::function<void(uint32&, uint32&)> m_getFramebufferSize;

        // State
        uint32 m_framebufferWidth = 0;
        uint32 m_framebufferHeight = 0;
        bool m_hasResizeEvent = false;
    };
} // namespace RVX
