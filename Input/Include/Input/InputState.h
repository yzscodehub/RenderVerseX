#pragma once

#include "Core/Types.h"
#include <array>

namespace RVX
{
    constexpr uint32 RVX_MAX_KEYS = 512;
    constexpr uint32 RVX_MAX_MOUSE_BUTTONS = 8;

    struct InputState
    {
        std::array<bool, RVX_MAX_KEYS> keys{};
        std::array<bool, RVX_MAX_MOUSE_BUTTONS> mouseButtons{};
        float mouseX = 0.0f;
        float mouseY = 0.0f;
        float mouseDeltaX = 0.0f;
        float mouseDeltaY = 0.0f;
        float mouseWheel = 0.0f;
    };
} // namespace RVX
