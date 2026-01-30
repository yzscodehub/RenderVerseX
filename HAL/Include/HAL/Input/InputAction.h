#pragma once

/**
 * @file InputAction.h
 * @brief Input action system for abstract input binding
 * 
 * Provides a layer of abstraction between physical inputs (keys, buttons, axes)
 * and game actions (Jump, Fire, MoveForward), allowing easy remapping.
 */

#include "Core/Types.h"
#include "HAL/Input/KeyCodes.h"
#include <string>
#include <vector>
#include <functional>

namespace RVX::HAL
{
    // =========================================================================
    // Input Source Types
    // =========================================================================

    /**
     * @brief Type of input device
     */
    enum class InputDeviceType : uint8
    {
        Keyboard,
        Mouse,
        Gamepad,
        Touch
    };

    /**
     * @brief Type of action trigger
     */
    enum class ActionType : uint8
    {
        Button,     ///< Digital on/off (key press, button press)
        Axis1D,     ///< Single axis (-1 to 1, or 0 to 1)
        Axis2D      ///< Two-axis input (stick, mouse delta)
    };

    /**
     * @brief Trigger mode for button actions
     */
    enum class TriggerMode : uint8
    {
        Pressed,    ///< Triggers once when pressed
        Released,   ///< Triggers once when released
        Held,       ///< Triggers every frame while held
        Tap,        ///< Quick press and release
        Hold        ///< Held for a duration
    };

    /**
     * @brief Modifier keys that must be held
     */
    enum class ModifierFlags : uint8
    {
        None    = 0,
        Shift   = 1 << 0,
        Ctrl    = 1 << 1,
        Alt     = 1 << 2,
        Super   = 1 << 3  ///< Windows key / Command key
    };

    inline ModifierFlags operator|(ModifierFlags a, ModifierFlags b)
    {
        return static_cast<ModifierFlags>(static_cast<uint8>(a) | static_cast<uint8>(b));
    }

    inline ModifierFlags operator&(ModifierFlags a, ModifierFlags b)
    {
        return static_cast<ModifierFlags>(static_cast<uint8>(a) & static_cast<uint8>(b));
    }

    inline bool HasFlag(ModifierFlags flags, ModifierFlags flag)
    {
        return (static_cast<uint8>(flags) & static_cast<uint8>(flag)) != 0;
    }

    // =========================================================================
    // Gamepad Constants
    // =========================================================================

    namespace GamepadButton
    {
        constexpr int A             = 0;
        constexpr int B             = 1;
        constexpr int X             = 2;
        constexpr int Y             = 3;
        constexpr int LeftBumper    = 4;
        constexpr int RightBumper   = 5;
        constexpr int Back          = 6;
        constexpr int Start         = 7;
        constexpr int Guide         = 8;
        constexpr int LeftThumb     = 9;
        constexpr int RightThumb    = 10;
        constexpr int DPadUp        = 11;
        constexpr int DPadRight     = 12;
        constexpr int DPadDown      = 13;
        constexpr int DPadLeft      = 14;
        constexpr int Count         = 15;
    }

    namespace GamepadAxis
    {
        constexpr int LeftX         = 0;
        constexpr int LeftY         = 1;
        constexpr int RightX        = 2;
        constexpr int RightY        = 3;
        constexpr int LeftTrigger   = 4;
        constexpr int RightTrigger  = 5;
        constexpr int Count         = 6;
    }

    // =========================================================================
    // Input Binding
    // =========================================================================

    /**
     * @brief A single input binding (one key/button/axis mapping)
     */
    struct InputBinding
    {
        InputDeviceType deviceType = InputDeviceType::Keyboard;

        /// For keyboard: key code, for mouse: button, for gamepad: button or axis index
        int code = 0;

        /// For axis bindings: which axis (GamepadAxis::*)
        int axisIndex = -1;

        /// For composite axis (e.g., WASD -> 2D axis): positive/negative direction
        /// -1 = negative direction, 0 = not directional, 1 = positive direction
        int direction = 0;

        /// Required modifier keys
        ModifierFlags modifiers = ModifierFlags::None;

        /// For axis inputs: multiplier/invert
        float scale = 1.0f;

        /// Gamepad index (0-3 typically)
        int gamepadIndex = 0;

        // =====================================================================
        // Static factory methods
        // =====================================================================

        static InputBinding Keyboard(int keyCode, ModifierFlags mods = ModifierFlags::None)
        {
            InputBinding b;
            b.deviceType = InputDeviceType::Keyboard;
            b.code = keyCode;
            b.modifiers = mods;
            return b;
        }

        static InputBinding MouseButton(int button)
        {
            InputBinding b;
            b.deviceType = InputDeviceType::Mouse;
            b.code = button;
            return b;
        }

        static InputBinding GamepadBtn(int button, int padIndex = 0)
        {
            InputBinding b;
            b.deviceType = InputDeviceType::Gamepad;
            b.code = button;
            b.gamepadIndex = padIndex;
            return b;
        }

        static InputBinding GamepadAxisBinding(int axis, float axisScale = 1.0f, int padIndex = 0)
        {
            InputBinding b;
            b.deviceType = InputDeviceType::Gamepad;
            b.axisIndex = axis;
            b.scale = axisScale;
            b.gamepadIndex = padIndex;
            return b;
        }

        /// Create a keyboard binding for composite axis (e.g., W for +Y, S for -Y)
        static InputBinding KeyboardAxis(int keyCode, int dir)
        {
            InputBinding b;
            b.deviceType = InputDeviceType::Keyboard;
            b.code = keyCode;
            b.direction = dir;
            return b;
        }
    };

    // =========================================================================
    // Input Action
    // =========================================================================

    /**
     * @brief An input action with multiple possible bindings
     */
    struct InputAction
    {
        std::string name;
        ActionType type = ActionType::Button;
        TriggerMode triggerMode = TriggerMode::Pressed;
        std::vector<InputBinding> bindings;

        /// For hold actions: required hold duration in seconds
        float holdDuration = 0.5f;

        /// For axis actions: dead zone
        float deadZone = 0.1f;

        // =====================================================================
        // Builder pattern methods
        // =====================================================================

        InputAction& SetName(std::string actionName)
        {
            name = std::move(actionName);
            return *this;
        }

        InputAction& SetType(ActionType t)
        {
            type = t;
            return *this;
        }

        InputAction& SetTrigger(TriggerMode mode)
        {
            triggerMode = mode;
            return *this;
        }

        InputAction& AddBinding(const InputBinding& binding)
        {
            bindings.push_back(binding);
            return *this;
        }

        InputAction& SetDeadZone(float dz)
        {
            deadZone = dz;
            return *this;
        }

        InputAction& SetHoldDuration(float duration)
        {
            holdDuration = duration;
            return *this;
        }
    };

    // =========================================================================
    // Action Value
    // =========================================================================

    /**
     * @brief Current value of an action
     */
    struct ActionValue
    {
        /// For button: 1.0 if pressed, 0.0 if not
        /// For axis1D: -1.0 to 1.0
        float value = 0.0f;

        /// For axis2D
        float x = 0.0f;
        float y = 0.0f;

        /// Was the action just triggered this frame?
        bool triggered = false;

        /// Is the action currently active?
        bool active = false;

        float GetMagnitude() const
        {
            if (x != 0.0f || y != 0.0f)
            {
                return std::sqrt(x * x + y * y);
            }
            return std::abs(value);
        }

        bool IsPressed() const { return active; }
        bool WasJustPressed() const { return triggered && active; }
    };

    // =========================================================================
    // Action Callback
    // =========================================================================

    /**
     * @brief Callback function type for action events
     */
    using ActionCallback = std::function<void(const ActionValue&)>;

} // namespace RVX::HAL

// Backward compatibility
namespace RVX
{
    using HAL::InputDeviceType;
    using HAL::ActionType;
    using HAL::TriggerMode;
    using HAL::ModifierFlags;
    using HAL::InputBinding;
    using HAL::InputAction;
    using HAL::ActionValue;
    using HAL::ActionCallback;
    namespace GamepadButton = HAL::GamepadButton;
    namespace GamepadAxis = HAL::GamepadAxis;
}
