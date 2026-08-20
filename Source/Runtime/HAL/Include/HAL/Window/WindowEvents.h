#pragma once

/**
 * @file WindowEvents.h
 * @brief Window-related event types
 */

#include "Core/Types.h"
#include "Core/Event/Event.h"

namespace RVX::HAL
{
    /**
     * @brief Window event types
     */
    enum class WindowEventType
    {
        None,
        Resized,
        Focused,
        LostFocus,
        Minimized,
        Restored,
        Closed,
        DpiChanged
    };

    /**
     * @brief Base window event (legacy style)
     */
    struct WindowEvent
    {
        WindowEventType type = WindowEventType::None;
    };

    /**
     * @brief Window resize event (legacy style, non-EventBus)
     */
    struct WindowResizeEventData : public WindowEvent
    {
        uint32 width = 0;
        uint32 height = 0;
    };

    /**
     * @brief Window DPI change event (legacy style)
     */
    struct WindowDpiEvent : public WindowEvent
    {
        float dpiScale = 1.0f;
    };

    // =========================================================================
    // EventBus-compatible events
    // =========================================================================

    /**
     * @brief Window resized event (EventBus compatible)
     */
    struct WindowResizedEvent : public RVX::Event
    {
        RVX_EVENT_TYPE(WindowResizedEvent)
        
        uint32 width = 0;
        uint32 height = 0;

        WindowResizedEvent() = default;
        WindowResizedEvent(uint32 w, uint32 h) : width(w), height(h) {}
    };

    /**
     * @brief Window closed event (EventBus compatible)
     */
    struct WindowClosedEvent : public RVX::Event
    {
        RVX_EVENT_TYPE(WindowClosedEvent)
    };

    /**
     * @brief Window focus event (EventBus compatible)
     */
    struct WindowFocusEvent : public RVX::Event
    {
        RVX_EVENT_TYPE(WindowFocusEvent)
        
        bool focused = false;

        WindowFocusEvent() = default;
        explicit WindowFocusEvent(bool f) : focused(f) {}
    };

} // namespace RVX::HAL

// Backward compatibility - expose HAL types in RVX namespace
// Note: RVX::WindowResizeEvent and RVX::WindowFocusEvent already defined in Core/Event/Event.h
namespace RVX
{
    using HAL::WindowEventType;
    using HAL::WindowEvent;
    using HAL::WindowResizeEventData;
    using HAL::WindowDpiEvent;
    using HAL::WindowResizedEvent;
    using HAL::WindowClosedEvent;
    // WindowFocusEvent already in RVX namespace from Core/Event/Event.h
}
