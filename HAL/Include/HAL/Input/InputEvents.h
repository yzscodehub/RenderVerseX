#pragma once

/**
 * @file InputEvents.h
 * @brief Input event types
 */

#include "Core/Types.h"
#include "Core/Event/Event.h"

namespace RVX::HAL
{
    /**
     * @brief Input event types (legacy style)
     */
    enum class InputEventType
    {
        None,
        KeyDown,
        KeyUp,
        MouseMove,
        MouseButtonDown,
        MouseButtonUp,
        MouseWheel
    };

    /**
     * @brief Base input event (legacy style)
     */
    struct InputEvent
    {
        InputEventType type = InputEventType::None;
    };

    /**
     * @brief Key event (legacy style)
     */
    struct KeyEvent : public InputEvent
    {
        uint32 key = 0;
    };

    /**
     * @brief Mouse move event (legacy style)
     */
    struct MouseMoveEvent : public InputEvent
    {
        float x = 0.0f;
        float y = 0.0f;
    };

    /**
     * @brief Mouse button event (legacy style)
     */
    struct MouseButtonEvent : public InputEvent
    {
        uint32 button = 0;
    };

    /**
     * @brief Mouse wheel event (legacy style)
     */
    struct MouseWheelEvent : public InputEvent
    {
        float delta = 0.0f;
    };

    // =========================================================================
    // EventBus-compatible events
    // =========================================================================

    /**
     * @brief Key pressed event (EventBus compatible)
     */
    struct KeyPressedEvent : public RVX::Event
    {
        uint32 keyCode = 0;
        bool repeat = false;

        KeyPressedEvent() = default;
        KeyPressedEvent(uint32 key, bool r = false) : keyCode(key), repeat(r) {}

        const char* GetTypeName() const override { return "KeyPressed"; }
    };

    /**
     * @brief Key released event (EventBus compatible)
     */
    struct KeyReleasedEvent : public RVX::Event
    {
        uint32 keyCode = 0;

        KeyReleasedEvent() = default;
        explicit KeyReleasedEvent(uint32 key) : keyCode(key) {}

        const char* GetTypeName() const override { return "KeyReleased"; }
    };

    /**
     * @brief Mouse moved event (EventBus compatible)
     */
    struct MouseMovedEvent : public RVX::Event
    {
        float x = 0.0f;
        float y = 0.0f;

        MouseMovedEvent() = default;
        MouseMovedEvent(float px, float py) : x(px), y(py) {}

        const char* GetTypeName() const override { return "MouseMoved"; }
    };

    /**
     * @brief Mouse button pressed event (EventBus compatible)
     */
    struct MouseButtonPressedEvent : public RVX::Event
    {
        uint32 button = 0;

        MouseButtonPressedEvent() = default;
        explicit MouseButtonPressedEvent(uint32 b) : button(b) {}

        const char* GetTypeName() const override { return "MouseButtonPressed"; }
    };

    /**
     * @brief Mouse button released event (EventBus compatible)
     */
    struct MouseButtonReleasedEvent : public RVX::Event
    {
        uint32 button = 0;

        MouseButtonReleasedEvent() = default;
        explicit MouseButtonReleasedEvent(uint32 b) : button(b) {}

        const char* GetTypeName() const override { return "MouseButtonReleased"; }
    };

    /**
     * @brief Mouse scrolled event (EventBus compatible)
     */
    struct MouseScrolledEvent : public RVX::Event
    {
        float offsetX = 0.0f;
        float offsetY = 0.0f;

        MouseScrolledEvent() = default;
        MouseScrolledEvent(float x, float y) : offsetX(x), offsetY(y) {}

        const char* GetTypeName() const override { return "MouseScrolled"; }
    };

} // namespace RVX::HAL

// Backward compatibility
namespace RVX
{
    using HAL::InputEventType;
    using HAL::InputEvent;
    using HAL::KeyEvent;
    using HAL::MouseMoveEvent;
    using HAL::MouseButtonEvent;
    using HAL::MouseWheelEvent;
    using HAL::KeyPressedEvent;
    using HAL::KeyReleasedEvent;
    using HAL::MouseMovedEvent;
    using HAL::MouseButtonPressedEvent;
    using HAL::MouseButtonReleasedEvent;
    using HAL::MouseScrolledEvent;
}
