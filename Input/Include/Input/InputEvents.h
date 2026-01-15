#pragma once

#include "Core/Types.h"

namespace RVX
{
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

    struct InputEvent
    {
        InputEventType type = InputEventType::None;
    };

    struct KeyEvent : public InputEvent
    {
        uint32 key = 0;
    };

    struct MouseMoveEvent : public InputEvent
    {
        float x = 0.0f;
        float y = 0.0f;
    };

    struct MouseButtonEvent : public InputEvent
    {
        uint32 button = 0;
    };

    struct MouseWheelEvent : public InputEvent
    {
        float delta = 0.0f;
    };
} // namespace RVX
