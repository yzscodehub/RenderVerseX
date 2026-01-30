#pragma once

/**
 * @file InputSubsystem.h
 * @brief Input handling subsystem
 * 
 * Provides comprehensive input handling including:
 * - Keyboard and mouse polling
 * - Gamepad support with vibration
 * - Touch input with gesture recognition
 * - Input action mapping system
 */

#include "Core/Subsystem/EngineSubsystem.h"
#include "HAL/Input/IInputBackend.h"
#include "HAL/Input/InputState.h"
#include "HAL/Input/KeyCodes.h"
#include "HAL/Input/Input.h"
#include "HAL/Input/GamepadState.h"
#include "HAL/Input/TouchState.h"
#include "Runtime/Input/InputActionMap.h"
#include <memory>
#include <array>

namespace RVX
{
    // Forward declarations
    namespace HAL { 
        class IWindow; 
        class IGamepadBackend;
        class ITouchBackend;
    }
    using Window = HAL::IWindow;

    class GestureRecognizer;
    class VirtualJoystick;

    /**
     * @brief Input subsystem - manages input state and events
     * 
     * This subsystem provides polling-based input for keyboard, mouse,
     * gamepads, and touch input. It also supports an action mapping system
     * for input abstraction.
     * 
     * Usage:
     * @code
     * auto* inputSys = engine.GetSubsystem<InputSubsystem>();
     * 
     * // Polling-based keyboard/mouse
     * if (inputSys->IsKeyDown(Key::W)) {
     *     MoveForward();
     * }
     * 
     * if (inputSys->IsKeyPressed(Key::Space)) {
     *     Jump();  // Only triggers once when pressed
     * }
     * 
     * // Gamepad
     * if (inputSys->IsGamepadConnected(0)) {
     *     float lx, ly;
     *     inputSys->GetGamepadLeftStick(0, lx, ly);
     *     Move(lx, ly);
     * }
     * 
     * // Action system
     * auto& actions = inputSys->GetActionMap();
     * actions.AddAction(
     *     InputAction()
     *         .SetName("Jump")
     *         .AddBinding(InputBinding::Keyboard(Key::Space))
     *         .AddBinding(InputBinding::GamepadBtn(GamepadButton::A))
     * );
     * 
     * if (actions.IsActionPressed("Jump")) {
     *     player.Jump();
     * }
     * @endcode
     */
    class InputSubsystem : public EngineSubsystem
    {
    public:
        // =====================================================================
        // Construction
        // =====================================================================

        InputSubsystem();
        ~InputSubsystem();

        // =====================================================================
        // Subsystem Interface
        // =====================================================================

        const char* GetName() const override { return "InputSubsystem"; }

        /// Dependencies - requires WindowSubsystem
        const char** GetDependencies(int& outCount) const override
        {
            static const char* deps[] = { "WindowSubsystem" };
            outCount = 1;
            return deps;
        }

        void Initialize() override;
        void Deinitialize() override;
        void Tick(float deltaTime) override;
        bool ShouldTick() const override { return true; }

        /// Set the window to capture input from
        void SetWindow(Window* window);

        // =====================================================================
        // Keyboard Polling API
        // =====================================================================

        /// Check if a key is currently pressed
        bool IsKeyDown(int keyCode) const;

        /// Check if a key was just pressed this frame
        bool IsKeyPressed(int keyCode) const;

        /// Check if a key was just released this frame
        bool IsKeyReleased(int keyCode) const;

        // =====================================================================
        // Mouse Polling API
        // =====================================================================

        /// Check if a mouse button is currently pressed
        bool IsMouseButtonDown(int button) const;

        /// Check if a mouse button was just pressed this frame
        bool IsMouseButtonPressed(int button) const;

        /// Check if a mouse button was just released this frame
        bool IsMouseButtonReleased(int button) const;

        /// Get current mouse position
        void GetMousePosition(float& x, float& y) const;

        /// Get mouse movement since last frame
        void GetMouseDelta(float& dx, float& dy) const;

        /// Get scroll wheel delta
        void GetScrollDelta(float& x, float& y) const;

        // =====================================================================
        // Gamepad API
        // =====================================================================

        /// Check if a gamepad is connected
        bool IsGamepadConnected(int index) const;

        /// Get gamepad name
        const std::string& GetGamepadName(int index) const;

        /// Check if a gamepad button is currently pressed
        bool IsGamepadButtonDown(int index, int button) const;

        /// Check if a gamepad button was just pressed this frame
        bool IsGamepadButtonPressed(int index, int button) const;

        /// Check if a gamepad button was just released this frame
        bool IsGamepadButtonReleased(int index, int button) const;

        /// Get gamepad axis value (-1 to 1 for sticks, 0 to 1 for triggers)
        float GetGamepadAxis(int index, int axis) const;

        /// Get gamepad left stick values
        void GetGamepadLeftStick(int index, float& x, float& y) const;

        /// Get gamepad right stick values
        void GetGamepadRightStick(int index, float& x, float& y) const;

        /// Get gamepad trigger values
        void GetGamepadTriggers(int index, float& left, float& right) const;

        /// Set gamepad vibration
        void SetGamepadVibration(int index, const HAL::GamepadVibration& vibration);

        /// Stop gamepad vibration
        void StopGamepadVibration(int index);

        /// Get full gamepad state
        const HAL::GamepadState& GetGamepadState(int index) const;

        // =====================================================================
        // Touch API
        // =====================================================================

        /// Check if touch input is available
        bool IsTouchAvailable() const;

        /// Get number of active touch points
        uint32 GetTouchCount() const;

        /// Get touch point by index
        const HAL::TouchPoint* GetTouch(uint32 index) const;

        /// Get touch point by ID
        const HAL::TouchPoint* GetTouchById(uint32 id) const;

        /// Get full touch state
        const HAL::TouchState& GetTouchState() const;

        /// Get gesture recognizer
        GestureRecognizer& GetGestureRecognizer();
        const GestureRecognizer& GetGestureRecognizer() const;

        // =====================================================================
        // Virtual Joystick API
        // =====================================================================

        /// Get left virtual joystick (for touch input)
        VirtualJoystick& GetLeftVirtualJoystick();
        const VirtualJoystick& GetLeftVirtualJoystick() const;

        /// Get right virtual joystick (for touch input)
        VirtualJoystick& GetRightVirtualJoystick();
        const VirtualJoystick& GetRightVirtualJoystick() const;

        /// Enable/disable virtual joysticks
        void SetVirtualJoysticksEnabled(bool enabled);
        bool AreVirtualJoysticksEnabled() const { return m_virtualJoysticksEnabled; }

        // =====================================================================
        // Action System API
        // =====================================================================

        /// Get the input action map
        InputActionMap& GetActionMap() { return m_actionMap; }
        const InputActionMap& GetActionMap() const { return m_actionMap; }

        // =====================================================================
        // Raw State Access
        // =====================================================================

        /// Get the underlying Input object (for advanced use)
        Input& GetInput() { return m_input; }
        const Input& GetInput() const { return m_input; }

    private:
        // =====================================================================
        // Internal Data
        // =====================================================================

        // Core input
        Input m_input;
        std::unique_ptr<HAL::IInputBackend> m_backend;
        Window* m_window = nullptr;
        
        // Per-frame state tracking
        std::array<bool, HAL::MAX_KEYS> m_keysPressed{};
        std::array<bool, HAL::MAX_KEYS> m_keysReleased{};
        std::array<bool, HAL::MAX_MOUSE_BUTTONS> m_mouseButtonsPressed{};
        std::array<bool, HAL::MAX_MOUSE_BUTTONS> m_mouseButtonsReleased{};
        
        float m_lastMouseX = 0.0f;
        float m_lastMouseY = 0.0f;

        // Gamepad
        std::unique_ptr<HAL::IGamepadBackend> m_gamepadBackend;
        std::array<HAL::GamepadState, HAL::MAX_GAMEPADS> m_gamepadStates{};
        static const std::string s_emptyGamepadName;

        // Touch
        std::unique_ptr<HAL::ITouchBackend> m_touchBackend;
        HAL::TouchState m_touchState;
        std::unique_ptr<GestureRecognizer> m_gestureRecognizer;

        // Virtual joysticks
        std::unique_ptr<VirtualJoystick> m_leftVirtualJoystick;
        std::unique_ptr<VirtualJoystick> m_rightVirtualJoystick;
        bool m_virtualJoysticksEnabled = false;

        // Action system
        InputActionMap m_actionMap;
    };

} // namespace RVX
