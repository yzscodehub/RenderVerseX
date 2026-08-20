#pragma once

/**
 * @file InputActionMap.h
 * @brief Input action map - manages action bindings and state
 * 
 * Usage:
 * @code
 * InputActionMap actionMap;
 * 
 * // Define actions
 * actionMap.AddAction(
 *     InputAction()
 *         .SetName("Jump")
 *         .SetType(ActionType::Button)
 *         .AddBinding(InputBinding::Keyboard(Key::Space))
 *         .AddBinding(InputBinding::GamepadBtn(GamepadButton::A))
 * );
 * 
 * actionMap.AddAction(
 *     InputAction()
 *         .SetName("Move")
 *         .SetType(ActionType::Axis2D)
 *         .AddBinding(InputBinding::KeyboardAxis(Key::W, 1))   // +Y
 *         .AddBinding(InputBinding::KeyboardAxis(Key::S, -1))  // -Y
 *         .AddBinding(InputBinding::KeyboardAxis(Key::A, -1))  // -X
 *         .AddBinding(InputBinding::KeyboardAxis(Key::D, 1))   // +X
 *         .AddBinding(InputBinding::GamepadAxisBinding(GamepadAxis::LeftX))
 *         .AddBinding(InputBinding::GamepadAxisBinding(GamepadAxis::LeftY))
 * );
 * 
 * // In game loop
 * if (actionMap.IsActionPressed("Jump")) {
 *     player.Jump();
 * }
 * 
 * auto moveValue = actionMap.GetAction2D("Move");
 * player.Move(moveValue.x, moveValue.y);
 * @endcode
 */

#include "HAL/Input/InputAction.h"
#include "HAL/Input/InputState.h"
#include "HAL/Input/GamepadState.h"
#include "HAL/Input/TouchState.h"
#include <string>
#include <unordered_map>
#include <functional>
#include <memory>

namespace RVX
{
    /**
     * @brief Input action map - manages actions and their bindings
     */
    class InputActionMap
    {
    public:
        // =====================================================================
        // Construction
        // =====================================================================

        InputActionMap() = default;
        ~InputActionMap() = default;

        InputActionMap(const InputActionMap&) = default;
        InputActionMap& operator=(const InputActionMap&) = default;
        InputActionMap(InputActionMap&&) noexcept = default;
        InputActionMap& operator=(InputActionMap&&) noexcept = default;

        // =====================================================================
        // Action Management
        // =====================================================================

        /**
         * @brief Add or update an action
         */
        void AddAction(const InputAction& action);

        /**
         * @brief Remove an action
         */
        void RemoveAction(const std::string& name);

        /**
         * @brief Check if an action exists
         */
        bool HasAction(const std::string& name) const;

        /**
         * @brief Get an action by name
         */
        const InputAction* GetAction(const std::string& name) const;

        /**
         * @brief Get mutable action for modification
         */
        InputAction* GetActionMutable(const std::string& name);

        /**
         * @brief Clear all actions
         */
        void Clear();

        // =====================================================================
        // Input State Update
        // =====================================================================

        /**
         * @brief Update action states from input
         * @param keyboardMouse Current keyboard/mouse state
         * @param gamepads Current gamepad states
         * @param touch Current touch state (optional)
         * @param deltaTime Frame delta time
         */
        void Update(
            const HAL::InputState& keyboardMouse,
            const std::array<HAL::GamepadState, HAL::MAX_GAMEPADS>& gamepads,
            const HAL::TouchState* touch,
            float deltaTime
        );

        /**
         * @brief Simplified update for keyboard/mouse only
         */
        void Update(const HAL::InputState& keyboardMouse, float deltaTime);

        // =====================================================================
        // Action Queries (Button Actions)
        // =====================================================================

        /**
         * @brief Check if a button action was just pressed this frame
         */
        bool IsActionPressed(const std::string& name) const;

        /**
         * @brief Check if a button action was just released this frame
         */
        bool IsActionReleased(const std::string& name) const;

        /**
         * @brief Check if a button action is currently held
         */
        bool IsActionHeld(const std::string& name) const;

        // =====================================================================
        // Action Queries (Axis Actions)
        // =====================================================================

        /**
         * @brief Get the value of a 1D axis action (-1 to 1)
         */
        float GetAxisValue(const std::string& name) const;

        /**
         * @brief Get the value of a 2D axis action
         */
        void GetAxis2D(const std::string& name, float& x, float& y) const;

        /**
         * @brief Get full action value
         */
        ActionValue GetActionValue(const std::string& name) const;

        // =====================================================================
        // Callbacks
        // =====================================================================

        /**
         * @brief Register a callback for an action
         * @param actionName Name of the action
         * @param callback Function to call when action is triggered
         * @return Handle for unregistering (0 = failed)
         */
        uint32 RegisterCallback(const std::string& actionName, ActionCallback callback);

        /**
         * @brief Unregister a callback
         */
        void UnregisterCallback(uint32 handle);

        // =====================================================================
        // Serialization
        // =====================================================================

        /**
         * @brief Load actions from a JSON file
         * @param path Path to the JSON file
         * @return true if successful
         */
        bool LoadFromFile(const std::string& path);

        /**
         * @brief Save actions to a JSON file
         * @param path Path to the JSON file
         * @return true if successful
         */
        bool SaveToFile(const std::string& path) const;

        // =====================================================================
        // Iteration
        // =====================================================================

        /**
         * @brief Get all action names
         */
        std::vector<std::string> GetActionNames() const;

        /**
         * @brief Get action count
         */
        size_t GetActionCount() const { return m_actions.size(); }

    private:
        // =====================================================================
        // Internal Types
        // =====================================================================

        struct ActionState
        {
            InputAction action;
            ActionValue currentValue;
            ActionValue previousValue;
            float holdTimer = 0.0f;
            bool holdTriggered = false;
        };

        struct CallbackEntry
        {
            std::string actionName;
            ActionCallback callback;
        };

        // =====================================================================
        // Internal Methods
        // =====================================================================

        float EvaluateBinding(
            const InputBinding& binding,
            const HAL::InputState& keyboard,
            const std::array<HAL::GamepadState, HAL::MAX_GAMEPADS>& gamepads,
            const HAL::TouchState* touch
        ) const;

        void EvaluateAction(
            ActionState& state,
            const HAL::InputState& keyboard,
            const std::array<HAL::GamepadState, HAL::MAX_GAMEPADS>& gamepads,
            const HAL::TouchState* touch,
            float deltaTime
        );

        bool CheckModifiers(ModifierFlags required, const HAL::InputState& keyboard) const;

        void InvokeCallbacks(const std::string& actionName, const ActionValue& value);

        // =====================================================================
        // Data
        // =====================================================================

        std::unordered_map<std::string, ActionState> m_actions;
        std::unordered_map<uint32, CallbackEntry> m_callbacks;
        uint32 m_nextCallbackHandle = 1;
    };

} // namespace RVX
