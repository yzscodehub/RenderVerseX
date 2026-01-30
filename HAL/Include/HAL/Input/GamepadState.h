#pragma once

/**
 * @file GamepadState.h
 * @brief Gamepad/Controller state data structures
 */

#include "Core/Types.h"
#include "HAL/Input/InputAction.h"
#include <array>
#include <string>

namespace RVX::HAL
{
    /// Maximum number of gamepads supported
    constexpr uint32 MAX_GAMEPADS = 4;

    /**
     * @brief Current state of a gamepad
     */
    struct GamepadState
    {
        /// Is this gamepad currently connected?
        bool connected = false;

        /// Gamepad name/identifier
        std::string name;

        /// Button states (true = pressed)
        std::array<bool, GamepadButton::Count> buttons{};

        /// Axis values (-1.0 to 1.0 for sticks, 0.0 to 1.0 for triggers)
        std::array<float, GamepadAxis::Count> axes{};

        /// Previous frame's button states (for press/release detection)
        std::array<bool, GamepadButton::Count> prevButtons{};

        // =====================================================================
        // Query methods
        // =====================================================================

        bool IsButtonDown(int button) const
        {
            if (button < 0 || button >= GamepadButton::Count)
                return false;
            return buttons[button];
        }

        bool IsButtonPressed(int button) const
        {
            if (button < 0 || button >= GamepadButton::Count)
                return false;
            return buttons[button] && !prevButtons[button];
        }

        bool IsButtonReleased(int button) const
        {
            if (button < 0 || button >= GamepadButton::Count)
                return false;
            return !buttons[button] && prevButtons[button];
        }

        float GetAxis(int axis) const
        {
            if (axis < 0 || axis >= GamepadAxis::Count)
                return 0.0f;
            return axes[axis];
        }

        /// Get left stick as 2D vector
        void GetLeftStick(float& x, float& y) const
        {
            x = axes[GamepadAxis::LeftX];
            y = axes[GamepadAxis::LeftY];
        }

        /// Get right stick as 2D vector
        void GetRightStick(float& x, float& y) const
        {
            x = axes[GamepadAxis::RightX];
            y = axes[GamepadAxis::RightY];
        }

        /// Get trigger values
        void GetTriggers(float& left, float& right) const
        {
            left = axes[GamepadAxis::LeftTrigger];
            right = axes[GamepadAxis::RightTrigger];
        }

        /// Apply dead zone to axis value
        static float ApplyDeadZone(float value, float deadZone)
        {
            if (std::abs(value) < deadZone)
                return 0.0f;
            
            // Remap the remaining range to 0-1
            float sign = value > 0.0f ? 1.0f : -1.0f;
            return sign * (std::abs(value) - deadZone) / (1.0f - deadZone);
        }

        /// Update previous state (call at end of frame)
        void UpdatePreviousState()
        {
            prevButtons = buttons;
        }
    };

    /**
     * @brief Gamepad vibration/rumble parameters
     */
    struct GamepadVibration
    {
        /// Low frequency motor intensity (0.0 to 1.0)
        float lowFrequency = 0.0f;

        /// High frequency motor intensity (0.0 to 1.0)
        float highFrequency = 0.0f;

        /// Duration in seconds (0 = continuous until stopped)
        float duration = 0.0f;

        static GamepadVibration None()
        {
            return {0.0f, 0.0f, 0.0f};
        }

        static GamepadVibration Light(float dur = 0.1f)
        {
            return {0.2f, 0.2f, dur};
        }

        static GamepadVibration Medium(float dur = 0.2f)
        {
            return {0.5f, 0.5f, dur};
        }

        static GamepadVibration Heavy(float dur = 0.3f)
        {
            return {1.0f, 0.6f, dur};
        }

        static GamepadVibration Impact(float dur = 0.15f)
        {
            return {1.0f, 0.0f, dur};
        }
    };

    /**
     * @brief Abstract interface for gamepad backend
     */
    class IGamepadBackend
    {
    public:
        virtual ~IGamepadBackend() = default;

        /**
         * @brief Poll gamepad states
         * @param states Array of gamepad states to fill
         */
        virtual void Poll(std::array<GamepadState, MAX_GAMEPADS>& states) = 0;

        /**
         * @brief Check if vibration is supported
         */
        virtual bool SupportsVibration() const = 0;

        /**
         * @brief Set vibration for a gamepad
         * @param gamepadIndex Index of the gamepad (0-3)
         * @param vibration Vibration parameters
         */
        virtual void SetVibration(int gamepadIndex, const GamepadVibration& vibration) = 0;

        /**
         * @brief Stop vibration for a gamepad
         */
        virtual void StopVibration(int gamepadIndex) = 0;
    };

} // namespace RVX::HAL

// Backward compatibility
namespace RVX
{
    using HAL::MAX_GAMEPADS;
    using HAL::GamepadState;
    using HAL::GamepadVibration;
    using HAL::IGamepadBackend;
}
