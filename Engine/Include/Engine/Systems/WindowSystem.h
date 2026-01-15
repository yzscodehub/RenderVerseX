#pragma once

#include "Core/ISystem.h"
#include "Core/Types.h"
#include <functional>

namespace RVX
{
    class WindowSystem : public ISystem
    {
    public:
        WindowSystem(std::function<void()> pollEvents,
                     std::function<bool()> shouldClose,
                     std::function<void(uint32&, uint32&)> getFramebufferSize = {})
            : m_pollEvents(std::move(pollEvents)),
              m_shouldClose(std::move(shouldClose)),
              m_getFramebufferSize(std::move(getFramebufferSize)) {}

        const char* GetName() const override { return "WindowSystem"; }

        void OnUpdate(float) override
        {
            if (m_pollEvents)
                m_pollEvents();

            if (m_getFramebufferSize)
            {
                uint32 width = 0;
                uint32 height = 0;
                m_getFramebufferSize(width, height);
                if (width > 0 && height > 0 &&
                    (width != m_framebufferWidth || height != m_framebufferHeight))
                {
                    m_framebufferWidth = width;
                    m_framebufferHeight = height;
                    m_hasResizeEvent = true;
                }
            }
        }

        bool ShouldClose() const
        {
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

    private:
        std::function<void()> m_pollEvents;
        std::function<bool()> m_shouldClose;
        std::function<void(uint32&, uint32&)> m_getFramebufferSize;
        uint32 m_framebufferWidth = 0;
        uint32 m_framebufferHeight = 0;
        bool m_hasResizeEvent = false;
    };
} // namespace RVX
