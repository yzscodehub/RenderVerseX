#pragma once

#include "Core/Types.h"

namespace RVX
{
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

    struct WindowEvent
    {
        WindowEventType type = WindowEventType::None;
    };

    struct WindowResizeEvent : public WindowEvent
    {
        uint32 width = 0;
        uint32 height = 0;
    };

    struct WindowDpiEvent : public WindowEvent
    {
        float dpiScale = 1.0f;
    };
} // namespace RVX
