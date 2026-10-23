/**
 * @file InputActionMap.cpp
 * @brief InputActionMap implementation
 */

#include "Runtime/Input/InputActionMap.h"
#include "HAL/Input/KeyCodes.h"
#include "Core/Log.h"
#include <algorithm>
#include <cmath>
#include <fstream>

namespace RVX
{

// =============================================================================
// Action Management
// =============================================================================

void InputActionMap::AddAction(const InputAction& action)
{
    if (action.name.empty())
    {
        RVX_CORE_WARN("InputActionMap: Cannot add action with empty name");
        return;
    }

    ActionState state;
    state.action = action;
    m_actions[action.name] = std::move(state);
}

void InputActionMap::RemoveAction(const std::string& name)
{
    m_actions.erase(name);
}

bool InputActionMap::HasAction(const std::string& name) const
{
    return m_actions.find(name) != m_actions.end();
}

const InputAction* InputActionMap::GetAction(const std::string& name) const
{
    auto it = m_actions.find(name);
    if (it != m_actions.end())
    {
        return &it->second.action;
    }
    return nullptr;
}

InputAction* InputActionMap::GetActionMutable(const std::string& name)
{
    auto it = m_actions.find(name);
    if (it != m_actions.end())
    {
        return &it->second.action;
    }
    return nullptr;
}

void InputActionMap::Clear()
{
    m_actions.clear();
}

// =============================================================================
// Input State Update
// =============================================================================

void InputActionMap::Update(
    const HAL::InputState& keyboardMouse,
    const std::array<HAL::GamepadState, HAL::MAX_GAMEPADS>& gamepads,
    const HAL::TouchState* touch,
    float deltaTime)
{
    for (auto& [name, state] : m_actions)
    {
        // Save previous value
        state.previousValue = state.currentValue;

        // Evaluate action
        EvaluateAction(state, keyboardMouse, gamepads, touch, deltaTime);

        // Check for callbacks
        if (state.currentValue.triggered)
        {
            InvokeCallbacks(name, state.currentValue);
        }
    }
}

void InputActionMap::Update(const HAL::InputState& keyboardMouse, float deltaTime)
{
    std::array<HAL::GamepadState, HAL::MAX_GAMEPADS> emptyGamepads{};
    Update(keyboardMouse, emptyGamepads, nullptr, deltaTime);
}

// =============================================================================
// Action Queries
// =============================================================================

bool InputActionMap::IsActionPressed(const std::string& name) const
{
    auto it = m_actions.find(name);
    if (it == m_actions.end())
        return false;
    
    const auto& state = it->second;
    return state.currentValue.active && !state.previousValue.active;
}

bool InputActionMap::IsActionReleased(const std::string& name) const
{
    auto it = m_actions.find(name);
    if (it == m_actions.end())
        return false;
    
    const auto& state = it->second;
    return !state.currentValue.active && state.previousValue.active;
}

bool InputActionMap::IsActionHeld(const std::string& name) const
{
    auto it = m_actions.find(name);
    if (it == m_actions.end())
        return false;
    
    return it->second.currentValue.active;
}

float InputActionMap::GetAxisValue(const std::string& name) const
{
    auto it = m_actions.find(name);
    if (it == m_actions.end())
        return 0.0f;
    
    return it->second.currentValue.value;
}

void InputActionMap::GetAxis2D(const std::string& name, float& x, float& y) const
{
    auto it = m_actions.find(name);
    if (it == m_actions.end())
    {
        x = 0.0f;
        y = 0.0f;
        return;
    }
    
    x = it->second.currentValue.x;
    y = it->second.currentValue.y;
}

ActionValue InputActionMap::GetActionValue(const std::string& name) const
{
    auto it = m_actions.find(name);
    if (it == m_actions.end())
        return ActionValue{};
    
    return it->second.currentValue;
}

// =============================================================================
// Callbacks
// =============================================================================

uint32 InputActionMap::RegisterCallback(const std::string& actionName, ActionCallback callback)
{
    if (!HasAction(actionName))
    {
        RVX_CORE_WARN("InputActionMap: Cannot register callback for unknown action '{}'", actionName);
        return 0;
    }

    uint32 handle = m_nextCallbackHandle++;
    m_callbacks[handle] = {actionName, std::move(callback)};
    return handle;
}

void InputActionMap::UnregisterCallback(uint32 handle)
{
    m_callbacks.erase(handle);
}

void InputActionMap::InvokeCallbacks(const std::string& actionName, const ActionValue& value)
{
    for (const auto& [handle, entry] : m_callbacks)
    {
        if (entry.actionName == actionName && entry.callback)
        {
            entry.callback(value);
        }
    }
}

// =============================================================================
// Serialization
// =============================================================================

namespace
{
    std::string Trim(const std::string& str)
    {
        size_t start = str.find_first_not_of(" \t\r\n");
        if (start == std::string::npos)
            return "";
        size_t end = str.find_last_not_of(" \t\r\n");
        return str.substr(start, end - start + 1);
    }

    std::vector<std::string> Split(const std::string& str, char delimiter)
    {
        std::vector<std::string> parts;
        size_t start = 0;
        size_t pos = 0;
        while ((pos = str.find(delimiter, start)) != std::string::npos)
        {
            parts.push_back(str.substr(start, pos - start));
            start = pos + 1;
        }
        parts.push_back(str.substr(start));
        return parts;
    }

    ActionType ParseActionType(const std::string& str)
    {
        if (str == "Button" || str == "button") return ActionType::Button;
        if (str == "Axis1D" || str == "axis1d") return ActionType::Axis1D;
        if (str == "Axis2D" || str == "axis2d") return ActionType::Axis2D;
        return ActionType::Button;
    }

    TriggerMode ParseTriggerMode(const std::string& str)
    {
        if (str == "Pressed" || str == "pressed") return TriggerMode::Pressed;
        if (str == "Released" || str == "released") return TriggerMode::Released;
        if (str == "Held" || str == "held") return TriggerMode::Held;
        if (str == "Tap" || str == "tap") return TriggerMode::Tap;
        if (str == "Hold" || str == "hold") return TriggerMode::Hold;
        return TriggerMode::Pressed;
    }

    int ParseKeyCode(const std::string& name)
    {
        // Common key mappings
        if (name == "Space") return HAL::Key::Space;
        if (name == "Enter") return HAL::Key::Enter;
        if (name == "Escape") return HAL::Key::Escape;
        if (name == "Tab") return HAL::Key::Tab;
        if (name == "Backspace") return HAL::Key::Backspace;
        if (name == "Up") return HAL::Key::Up;
        if (name == "Down") return HAL::Key::Down;
        if (name == "Left") return HAL::Key::Left;
        if (name == "Right") return HAL::Key::Right;
        if (name == "LeftShift") return HAL::Key::LeftShift;
        if (name == "RightShift") return HAL::Key::RightShift;
        if (name == "LeftControl" || name == "LeftCtrl") return HAL::Key::LeftControl;
        if (name == "RightControl" || name == "RightCtrl") return HAL::Key::RightControl;
        if (name == "LeftAlt") return HAL::Key::LeftAlt;
        if (name == "RightAlt") return HAL::Key::RightAlt;

        // F-keys
        if (name == "F1") return HAL::Key::F1;
        if (name == "F2") return HAL::Key::F2;
        if (name == "F3") return HAL::Key::F3;
        if (name == "F4") return HAL::Key::F4;
        if (name == "F5") return HAL::Key::F5;
        if (name == "F6") return HAL::Key::F6;
        if (name == "F7") return HAL::Key::F7;
        if (name == "F8") return HAL::Key::F8;
        if (name == "F9") return HAL::Key::F9;
        if (name == "F10") return HAL::Key::F10;
        if (name == "F11") return HAL::Key::F11;
        if (name == "F12") return HAL::Key::F12;

        // Single letter/digit
        if (name.size() == 1)
        {
            char c = name[0];
            if (c >= 'A' && c <= 'Z') return static_cast<int>(c);
            if (c >= 'a' && c <= 'z') return static_cast<int>(c - 'a' + 'A');
            if (c >= '0' && c <= '9') return HAL::Key::Num0 + (c - '0');
        }

        return 0;
    }

    int ParseGamepadButton(const std::string& name)
    {
        if (name == "A") return HAL::GamepadButton::A;
        if (name == "B") return HAL::GamepadButton::B;
        if (name == "X") return HAL::GamepadButton::X;
        if (name == "Y") return HAL::GamepadButton::Y;
        if (name == "LeftBumper" || name == "LB") return HAL::GamepadButton::LeftBumper;
        if (name == "RightBumper" || name == "RB") return HAL::GamepadButton::RightBumper;
        if (name == "Back" || name == "Select") return HAL::GamepadButton::Back;
        if (name == "Start") return HAL::GamepadButton::Start;
        if (name == "Guide" || name == "Home") return HAL::GamepadButton::Guide;
        if (name == "LeftThumb" || name == "LS") return HAL::GamepadButton::LeftThumb;
        if (name == "RightThumb" || name == "RS") return HAL::GamepadButton::RightThumb;
        if (name == "DPadUp") return HAL::GamepadButton::DPadUp;
        if (name == "DPadRight") return HAL::GamepadButton::DPadRight;
        if (name == "DPadDown") return HAL::GamepadButton::DPadDown;
        if (name == "DPadLeft") return HAL::GamepadButton::DPadLeft;
        return 0;
    }

    int ParseGamepadAxis(const std::string& name)
    {
        if (name == "LeftX") return HAL::GamepadAxis::LeftX;
        if (name == "LeftY") return HAL::GamepadAxis::LeftY;
        if (name == "RightX") return HAL::GamepadAxis::RightX;
        if (name == "RightY") return HAL::GamepadAxis::RightY;
        if (name == "LeftTrigger" || name == "LT") return HAL::GamepadAxis::LeftTrigger;
        if (name == "RightTrigger" || name == "RT") return HAL::GamepadAxis::RightTrigger;
        return -1;
    }

    InputBinding ParseBinding(const std::string& str)
    {
        auto parts = Split(str, ':');
        if (parts.empty())
            return InputBinding{};

        std::string device = Trim(parts[0]);

        if (device == "Keyboard" && parts.size() >= 2)
        {
            int keyCode = ParseKeyCode(Trim(parts[1]));
            return InputBinding::Keyboard(keyCode);
        }
        else if (device == "KeyboardAxis" && parts.size() >= 3)
        {
            int keyCode = ParseKeyCode(Trim(parts[1]));
            int direction = std::stoi(Trim(parts[2]));
            return InputBinding::KeyboardAxis(keyCode, direction);
        }
        else if (device == "Mouse" && parts.size() >= 2)
        {
            int button = std::stoi(Trim(parts[1]));
            return InputBinding::MouseButton(button);
        }
        else if (device == "Gamepad" && parts.size() >= 2)
        {
            int button = ParseGamepadButton(Trim(parts[1]));
            int padIndex = (parts.size() >= 3) ? std::stoi(Trim(parts[2])) : 0;
            return InputBinding::GamepadBtn(button, padIndex);
        }
        else if (device == "GamepadAxis" && parts.size() >= 2)
        {
            int axis = ParseGamepadAxis(Trim(parts[1]));
            float scale = (parts.size() >= 3) ? std::stof(Trim(parts[2])) : 1.0f;
            int padIndex = (parts.size() >= 4) ? std::stoi(Trim(parts[3])) : 0;
            return InputBinding::GamepadAxisBinding(axis, scale, padIndex);
        }

        return InputBinding{};
    }

    const char* ActionTypeToString(ActionType type)
    {
        switch (type)
        {
        case ActionType::Button: return "Button";
        case ActionType::Axis1D: return "Axis1D";
        case ActionType::Axis2D: return "Axis2D";
        default: return "Button";
        }
    }

    const char* TriggerModeToString(TriggerMode mode)
    {
        switch (mode)
        {
        case TriggerMode::Pressed: return "Pressed";
        case TriggerMode::Released: return "Released";
        case TriggerMode::Held: return "Held";
        case TriggerMode::Tap: return "Tap";
        case TriggerMode::Hold: return "Hold";
        default: return "Pressed";
        }
    }

    std::string KeyCodeToString(int keyCode)
    {
        switch (keyCode)
        {
        case HAL::Key::Space: return "Space";
        case HAL::Key::Enter: return "Enter";
        case HAL::Key::Escape: return "Escape";
        case HAL::Key::Tab: return "Tab";
        case HAL::Key::Backspace: return "Backspace";
        case HAL::Key::Up: return "Up";
        case HAL::Key::Down: return "Down";
        case HAL::Key::Left: return "Left";
        case HAL::Key::Right: return "Right";
        case HAL::Key::LeftShift: return "LeftShift";
        case HAL::Key::RightShift: return "RightShift";
        case HAL::Key::LeftControl: return "LeftControl";
        case HAL::Key::RightControl: return "RightControl";
        case HAL::Key::LeftAlt: return "LeftAlt";
        case HAL::Key::RightAlt: return "RightAlt";
        case HAL::Key::F1: return "F1";
        case HAL::Key::F2: return "F2";
        case HAL::Key::F3: return "F3";
        case HAL::Key::F4: return "F4";
        case HAL::Key::F5: return "F5";
        case HAL::Key::F6: return "F6";
        case HAL::Key::F7: return "F7";
        case HAL::Key::F8: return "F8";
        case HAL::Key::F9: return "F9";
        case HAL::Key::F10: return "F10";
        case HAL::Key::F11: return "F11";
        case HAL::Key::F12: return "F12";
        default:
            if (keyCode >= HAL::Key::A && keyCode <= HAL::Key::Z)
                return std::string(1, static_cast<char>(keyCode));
            if (keyCode >= HAL::Key::Num0 && keyCode <= HAL::Key::Num9)
                return std::string(1, static_cast<char>('0' + (keyCode - HAL::Key::Num0)));
            return std::to_string(keyCode);
        }
    }

    std::string GamepadButtonToString(int button)
    {
        switch (button)
        {
        case HAL::GamepadButton::A: return "A";
        case HAL::GamepadButton::B: return "B";
        case HAL::GamepadButton::X: return "X";
        case HAL::GamepadButton::Y: return "Y";
        case HAL::GamepadButton::LeftBumper: return "LeftBumper";
        case HAL::GamepadButton::RightBumper: return "RightBumper";
        case HAL::GamepadButton::Back: return "Back";
        case HAL::GamepadButton::Start: return "Start";
        case HAL::GamepadButton::Guide: return "Guide";
        case HAL::GamepadButton::LeftThumb: return "LeftThumb";
        case HAL::GamepadButton::RightThumb: return "RightThumb";
        case HAL::GamepadButton::DPadUp: return "DPadUp";
        case HAL::GamepadButton::DPadRight: return "DPadRight";
        case HAL::GamepadButton::DPadDown: return "DPadDown";
        case HAL::GamepadButton::DPadLeft: return "DPadLeft";
        default: return std::to_string(button);
        }
    }

    std::string GamepadAxisToString(int axis)
    {
        switch (axis)
        {
        case HAL::GamepadAxis::LeftX: return "LeftX";
        case HAL::GamepadAxis::LeftY: return "LeftY";
        case HAL::GamepadAxis::RightX: return "RightX";
        case HAL::GamepadAxis::RightY: return "RightY";
        case HAL::GamepadAxis::LeftTrigger: return "LeftTrigger";
        case HAL::GamepadAxis::RightTrigger: return "RightTrigger";
        default: return std::to_string(axis);
        }
    }

    std::string BindingToString(const InputBinding& binding)
    {
        switch (binding.deviceType)
        {
        case InputDeviceType::Keyboard:
            if (binding.direction != 0)
                return "KeyboardAxis:" + KeyCodeToString(binding.code) + ":" + std::to_string(binding.direction);
            else
                return "Keyboard:" + KeyCodeToString(binding.code);

        case InputDeviceType::Mouse:
            return "Mouse:" + std::to_string(binding.code);

        case InputDeviceType::Gamepad:
            if (binding.axisIndex >= 0)
                return "GamepadAxis:" + GamepadAxisToString(binding.axisIndex) + ":" + 
                       std::to_string(binding.scale) + ":" + std::to_string(binding.gamepadIndex);
            else
                return "Gamepad:" + GamepadButtonToString(binding.code) + ":" + 
                       std::to_string(binding.gamepadIndex);

        case InputDeviceType::Touch:
            return "Touch";

        default:
            return "";
        }
    }

} // anonymous namespace

bool InputActionMap::LoadFromFile(const std::string& path)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        RVX_CORE_ERROR("InputActionMap: Failed to open file '{}'", path);
        return false;
    }

    Clear();

    InputAction currentAction;
    bool hasAction = false;

    std::string line;
    int lineNumber = 0;

    while (std::getline(file, line))
    {
        ++lineNumber;
        line = Trim(line);

        // Skip empty lines and comments
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;

        // Section header [Action:Name]
        if (line.front() == '[' && line.back() == ']')
        {
            // Save previous action
            if (hasAction && !currentAction.name.empty())
            {
                AddAction(currentAction);
            }

            // Parse new action header
            std::string header = line.substr(1, line.size() - 2);
            auto colonPos = header.find(':');
            if (colonPos != std::string::npos)
            {
                std::string type = header.substr(0, colonPos);
                std::string name = header.substr(colonPos + 1);

                if (type == "Action")
                {
                    currentAction = InputAction();
                    currentAction.name = Trim(name);
                    hasAction = true;
                }
            }
            continue;
        }

        // Key=Value pairs
        if (hasAction)
        {
            auto eqPos = line.find('=');
            if (eqPos != std::string::npos)
            {
                std::string key = Trim(line.substr(0, eqPos));
                std::string value = Trim(line.substr(eqPos + 1));

                if (key == "type")
                {
                    currentAction.type = ParseActionType(value);
                }
                else if (key == "trigger")
                {
                    currentAction.triggerMode = ParseTriggerMode(value);
                }
                else if (key == "deadzone")
                {
                    currentAction.deadZone = std::stof(value);
                }
                else if (key == "holdDuration")
                {
                    currentAction.holdDuration = std::stof(value);
                }
                else if (key == "binding")
                {
                    InputBinding binding = ParseBinding(value);
                    currentAction.bindings.push_back(binding);
                }
            }
        }
    }

    // Save last action
    if (hasAction && !currentAction.name.empty())
    {
        AddAction(currentAction);
    }

    RVX_CORE_INFO("InputActionMap: Loaded {} actions from '{}'", GetActionCount(), path);
    return true;
}

bool InputActionMap::SaveToFile(const std::string& path) const
{
    std::ofstream file(path);
    if (!file.is_open())
    {
        RVX_CORE_ERROR("InputActionMap: Failed to create file '{}'", path);
        return false;
    }

    file << "# RenderVerseX Input Action Map\n";
    file << "# Format: [Action:Name] followed by key=value pairs\n\n";

    for (const auto& [name, state] : m_actions)
    {
        const auto& action = state.action;

        file << "[Action:" << action.name << "]\n";
        file << "type=" << ActionTypeToString(action.type) << "\n";
        file << "trigger=" << TriggerModeToString(action.triggerMode) << "\n";
        
        if (action.deadZone != 0.1f)
            file << "deadzone=" << action.deadZone << "\n";
        
        if (action.triggerMode == TriggerMode::Hold || action.triggerMode == TriggerMode::Tap)
            file << "holdDuration=" << action.holdDuration << "\n";

        for (const auto& binding : action.bindings)
        {
            file << "binding=" << BindingToString(binding) << "\n";
        }

        file << "\n";
    }

    RVX_CORE_INFO("InputActionMap: Saved {} actions to '{}'", GetActionCount(), path);
    return true;
}

// =============================================================================
// Iteration
// =============================================================================

std::vector<std::string> InputActionMap::GetActionNames() const
{
    std::vector<std::string> names;
    names.reserve(m_actions.size());
    for (const auto& [name, state] : m_actions)
    {
        names.push_back(name);
    }
    return names;
}

// =============================================================================
// Internal Methods
// =============================================================================

bool InputActionMap::CheckModifiers(ModifierFlags required, const HAL::InputState& keyboard) const
{
    if (required == ModifierFlags::None)
        return true;

    bool shiftDown = keyboard.keys[HAL::Key::LeftShift] || keyboard.keys[HAL::Key::RightShift];
    bool ctrlDown = keyboard.keys[HAL::Key::LeftControl] || keyboard.keys[HAL::Key::RightControl];
    bool altDown = keyboard.keys[HAL::Key::LeftAlt] || keyboard.keys[HAL::Key::RightAlt];
    bool superDown = keyboard.keys[HAL::Key::LeftSuper] || keyboard.keys[HAL::Key::RightSuper];

    if (HasFlag(required, ModifierFlags::Shift) && !shiftDown)
        return false;
    if (HasFlag(required, ModifierFlags::Ctrl) && !ctrlDown)
        return false;
    if (HasFlag(required, ModifierFlags::Alt) && !altDown)
        return false;
    if (HasFlag(required, ModifierFlags::Super) && !superDown)
        return false;

    return true;
}

float InputActionMap::EvaluateBinding(
    const InputBinding& binding,
    const HAL::InputState& keyboard,
    const std::array<HAL::GamepadState, HAL::MAX_GAMEPADS>& gamepads,
    const HAL::TouchState* touch) const
{
    switch (binding.deviceType)
    {
    case InputDeviceType::Keyboard:
    {
        if (!CheckModifiers(binding.modifiers, keyboard))
            return 0.0f;

        if (binding.code >= 0 && binding.code < static_cast<int>(HAL::MAX_KEYS))
        {
            bool pressed = keyboard.keys[binding.code];
            if (pressed)
            {
                // For directional bindings (composite axis)
                if (binding.direction != 0)
                    return static_cast<float>(binding.direction);
                return 1.0f;
            }
        }
        return 0.0f;
    }

    case InputDeviceType::Mouse:
    {
        if (binding.code >= 0 && binding.code < static_cast<int>(HAL::MAX_MOUSE_BUTTONS))
        {
            return keyboard.mouseButtons[binding.code] ? 1.0f : 0.0f;
        }
        return 0.0f;
    }

    case InputDeviceType::Gamepad:
    {
        int padIndex = binding.gamepadIndex;
        if (padIndex < 0 || padIndex >= static_cast<int>(HAL::MAX_GAMEPADS))
            return 0.0f;

        const auto& pad = gamepads[padIndex];
        if (!pad.connected)
            return 0.0f;

        // Axis binding
        if (binding.axisIndex >= 0 && binding.axisIndex < HAL::GamepadAxis::Count)
        {
            return pad.axes[binding.axisIndex] * binding.scale;
        }

        // Button binding
        if (binding.code >= 0 && binding.code < HAL::GamepadButton::Count)
        {
            return pad.buttons[binding.code] ? 1.0f : 0.0f;
        }
        return 0.0f;
    }

    case InputDeviceType::Touch:
    {
        if (touch && touch->activeCount > 0)
        {
            // For button-like touch, any touch is considered pressed
            return 1.0f;
        }
        return 0.0f;
    }

    default:
        return 0.0f;
    }
}

void InputActionMap::EvaluateAction(
    ActionState& state,
    const HAL::InputState& keyboard,
    const std::array<HAL::GamepadState, HAL::MAX_GAMEPADS>& gamepads,
    const HAL::TouchState* touch,
    float deltaTime)
{
    const auto& action = state.action;
    ActionValue& value = state.currentValue;
    
    // Reset triggered flag
    value.triggered = false;

    switch (action.type)
    {
    case ActionType::Button:
    {
        // Evaluate all bindings, any active = action active
        bool anyActive = false;
        for (const auto& binding : action.bindings)
        {
            float bindingValue = EvaluateBinding(binding, keyboard, gamepads, touch);
            if (bindingValue > 0.5f)
            {
                anyActive = true;
                break;
            }
        }

        bool wasActive = value.active;
        value.active = anyActive;
        value.value = anyActive ? 1.0f : 0.0f;

        // Handle trigger modes
        switch (action.triggerMode)
        {
        case TriggerMode::Pressed:
            value.triggered = anyActive && !wasActive;
            break;

        case TriggerMode::Released:
            value.triggered = !anyActive && wasActive;
            break;

        case TriggerMode::Held:
            value.triggered = anyActive;
            break;

        case TriggerMode::Hold:
            if (anyActive)
            {
                state.holdTimer += deltaTime;
                if (!state.holdTriggered && state.holdTimer >= action.holdDuration)
                {
                    value.triggered = true;
                    state.holdTriggered = true;
                }
            }
            else
            {
                state.holdTimer = 0.0f;
                state.holdTriggered = false;
            }
            break;

        case TriggerMode::Tap:
            // Tap is triggered on release if press was short
            if (!anyActive && wasActive && state.holdTimer < action.holdDuration)
            {
                value.triggered = true;
            }
            if (anyActive)
            {
                state.holdTimer += deltaTime;
            }
            else
            {
                state.holdTimer = 0.0f;
            }
            break;
        }
        break;
    }

    case ActionType::Axis1D:
    {
        float total = 0.0f;
        for (const auto& binding : action.bindings)
        {
            float bindingValue = EvaluateBinding(binding, keyboard, gamepads, touch);
            total += bindingValue;
        }

        // Apply dead zone
        if (std::abs(total) < action.deadZone)
            total = 0.0f;

        // Clamp to -1, 1
        total = std::clamp(total, -1.0f, 1.0f);

        value.value = total;
        value.active = std::abs(total) > 0.0f;
        value.triggered = value.active && !state.previousValue.active;
        break;
    }

    case ActionType::Axis2D:
    {
        float xTotal = 0.0f;
        float yTotal = 0.0f;

        for (const auto& binding : action.bindings)
        {
            float bindingValue = EvaluateBinding(binding, keyboard, gamepads, touch);

            // Determine which axis this binding contributes to
            if (binding.deviceType == InputDeviceType::Gamepad && binding.axisIndex >= 0)
            {
                // Gamepad axis: LeftX/RightX go to X, LeftY/RightY go to Y
                if (binding.axisIndex == HAL::GamepadAxis::LeftX || 
                    binding.axisIndex == HAL::GamepadAxis::RightX)
                {
                    xTotal += bindingValue;
                }
                else if (binding.axisIndex == HAL::GamepadAxis::LeftY || 
                         binding.axisIndex == HAL::GamepadAxis::RightY)
                {
                    yTotal += bindingValue;
                }
                else
                {
                    // Triggers or other - treat as 1D contribution
                    xTotal += bindingValue;
                }
            }
            else if (binding.direction != 0)
            {
                // Keyboard with direction: determine X or Y from key code
                // Convention: A/D or Left/Right = X, W/S or Up/Down = Y
                int code = binding.code;
                if (code == HAL::Key::A || code == HAL::Key::D ||
                    code == HAL::Key::Left || code == HAL::Key::Right)
                {
                    xTotal += bindingValue;
                }
                else
                {
                    yTotal += bindingValue;
                }
            }
            else
            {
                // Default: add to X axis
                xTotal += bindingValue;
            }
        }

        // Apply dead zone (circular)
        float magnitude = std::sqrt(xTotal * xTotal + yTotal * yTotal);
        if (magnitude < action.deadZone)
        {
            xTotal = 0.0f;
            yTotal = 0.0f;
        }
        else if (magnitude > 1.0f)
        {
            // Normalize to unit circle
            xTotal /= magnitude;
            yTotal /= magnitude;
        }

        value.x = xTotal;
        value.y = yTotal;
        value.value = magnitude;
        value.active = magnitude > 0.0f;
        value.triggered = value.active && !state.previousValue.active;
        break;
    }
    }
}

} // namespace RVX
