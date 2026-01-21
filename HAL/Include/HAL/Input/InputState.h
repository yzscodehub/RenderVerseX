#pragma once

/**
 * @file InputState.h
 * @brief Input state data structure
 */

#include "Core/Types.h"
#include <array>

namespace RVX::HAL
{
    /// Maximum number of keyboard keys tracked
    constexpr uint32 MAX_KEYS = 512;
    
    /// Maximum number of mouse buttons tracked
    constexpr uint32 MAX_MOUSE_BUTTONS = 8;

    /**
     * @brief Current input state snapshot
     * 
     * Contains the state of all tracked input devices at a point in time.
     */
    struct InputState
    {
        /// Keyboard key states (true = pressed)
        std::array<bool, MAX_KEYS> keys{};
        
        /// Mouse button states (true = pressed)
        std::array<bool, MAX_MOUSE_BUTTONS> mouseButtons{};
        
        /// Current mouse X position in window coordinates
        float mouseX = 0.0f;
        
        /// Current mouse Y position in window coordinates
        float mouseY = 0.0f;
        
        /// Mouse X movement since last frame
        float mouseDeltaX = 0.0f;
        
        /// Mouse Y movement since last frame
        float mouseDeltaY = 0.0f;
        
        /// Mouse wheel delta since last frame
        float mouseWheel = 0.0f;
    };

} // namespace RVX::HAL

// Backward compatibility constants
namespace RVX
{
    constexpr uint32 RVX_MAX_KEYS = HAL::MAX_KEYS;
    constexpr uint32 RVX_MAX_MOUSE_BUTTONS = HAL::MAX_MOUSE_BUTTONS;
    using InputState = HAL::InputState;
}
