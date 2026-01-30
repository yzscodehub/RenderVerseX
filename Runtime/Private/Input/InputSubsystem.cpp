/**
 * @file InputSubsystem.cpp
 * @brief InputSubsystem implementation
 */

#include "Runtime/Input/InputSubsystem.h"
#include "Runtime/Input/GestureRecognizer.h"
#include "Runtime/Input/VirtualJoystick.h"
#include "Runtime/Window/WindowSubsystem.h"
#include "HAL/HAL.h"
#include "GLFW/GLFWInputBackend.h"
#include "GLFW/GLFWGamepadBackend.h"
#include "Core/Log.h"

namespace RVX
{

// Static member
const std::string InputSubsystem::s_emptyGamepadName;

InputSubsystem::InputSubsystem()
{
}

InputSubsystem::~InputSubsystem()
{
}

void InputSubsystem::Initialize()
{
    RVX_CORE_INFO("InputSubsystem initializing...");

    // Create gamepad backend
    m_gamepadBackend = std::make_unique<HAL::GLFWGamepadBackend>();

    // Create gesture recognizer
    m_gestureRecognizer = std::make_unique<GestureRecognizer>();

    // Create virtual joysticks with default configs
    VirtualJoystickConfig leftConfig;
    leftConfig.mode = VirtualJoystickMode::Floating;
    leftConfig.screenSideThreshold = 0.4f;  // Left 40% of screen
    leftConfig.centerX = 150.0f;
    leftConfig.centerY = 400.0f;
    m_leftVirtualJoystick = std::make_unique<VirtualJoystick>(leftConfig);

    VirtualJoystickConfig rightConfig;
    rightConfig.mode = VirtualJoystickMode::Floating;
    rightConfig.screenSideThreshold = 0.6f;  // Right 40% of screen (inverted logic)
    rightConfig.centerX = 1770.0f;
    rightConfig.centerY = 400.0f;
    m_rightVirtualJoystick = std::make_unique<VirtualJoystick>(rightConfig);

    RVX_CORE_INFO("InputSubsystem initialized");
}

void InputSubsystem::Deinitialize()
{
    RVX_CORE_DEBUG("InputSubsystem deinitializing...");
    
    // Stop all vibrations
    for (int i = 0; i < static_cast<int>(HAL::MAX_GAMEPADS); ++i)
    {
        StopGamepadVibration(i);
    }

    m_leftVirtualJoystick.reset();
    m_rightVirtualJoystick.reset();
    m_gestureRecognizer.reset();
    m_touchBackend.reset();
    m_gamepadBackend.reset();
    m_backend.reset();
    m_window = nullptr;

    RVX_CORE_INFO("InputSubsystem deinitialized");
}

void InputSubsystem::SetWindow(Window* window)
{
    m_window = window;
    
    if (window)
    {
        // Create GLFW backend using internal handle (GLFWwindow*)
        m_backend = std::make_unique<HAL::GLFWInputBackend>(
            static_cast<GLFWwindow*>(window->GetInternalHandle())
        );
        
        // Initialize mouse position
        const auto& state = m_input.GetState();
        m_lastMouseX = state.mouseX;
        m_lastMouseY = state.mouseY;

        // Update virtual joystick screen size
        // TODO: Get actual window size
        float width = 1920.0f;
        float height = 1080.0f;
        m_leftVirtualJoystick->SetScreenSize(width, height);
        m_rightVirtualJoystick->SetScreenSize(width, height);
    }
}

void InputSubsystem::Tick(float deltaTime)
{
    // Store previous state for press/release detection
    auto prevKeys = m_input.GetState().keys;
    auto prevMouseButtons = m_input.GetState().mouseButtons;

    // Clear per-frame state
    m_input.ClearFrameState();

    // Poll keyboard/mouse backend
    if (m_backend)
    {
        m_backend->Poll(m_input.GetMutableState());
    }

    // Update key press/release states
    const auto& currentState = m_input.GetState();
    for (size_t i = 0; i < m_keysPressed.size(); ++i)
    {
        m_keysPressed[i] = currentState.keys[i] && !prevKeys[i];
        m_keysReleased[i] = !currentState.keys[i] && prevKeys[i];
    }
    
    for (size_t i = 0; i < m_mouseButtonsPressed.size(); ++i)
    {
        m_mouseButtonsPressed[i] = currentState.mouseButtons[i] && !prevMouseButtons[i];
        m_mouseButtonsReleased[i] = !currentState.mouseButtons[i] && prevMouseButtons[i];
    }

    // Update mouse delta
    m_lastMouseX = currentState.mouseX;
    m_lastMouseY = currentState.mouseY;

    // Poll gamepads
    if (m_gamepadBackend)
    {
        m_gamepadBackend->Poll(m_gamepadStates);
        
        // Update vibration timers
        static_cast<HAL::GLFWGamepadBackend*>(m_gamepadBackend.get())->UpdateVibration(deltaTime);

        // Update previous button states
        for (auto& pad : m_gamepadStates)
        {
            if (pad.connected)
            {
                pad.UpdatePreviousState();
            }
        }
    }

    // Poll touch input
    if (m_touchBackend)
    {
        m_touchBackend->Poll(m_touchState);
    }

    // Process gestures
    if (m_gestureRecognizer)
    {
        m_gestureRecognizer->Process(m_touchState, deltaTime);
    }

    // Process virtual joysticks
    if (m_virtualJoysticksEnabled && m_leftVirtualJoystick && m_rightVirtualJoystick)
    {
        m_leftVirtualJoystick->Process(m_touchState, deltaTime);
        m_rightVirtualJoystick->Process(m_touchState, deltaTime);
    }

    // Update action map
    m_actionMap.Update(currentState, m_gamepadStates, 
                       m_touchBackend ? &m_touchState : nullptr, deltaTime);
}

// =============================================================================
// Keyboard API
// =============================================================================

bool InputSubsystem::IsKeyDown(int keyCode) const
{
    if (keyCode < 0 || keyCode >= static_cast<int>(HAL::MAX_KEYS))
        return false;
    return m_input.GetState().keys[keyCode];
}

bool InputSubsystem::IsKeyPressed(int keyCode) const
{
    if (keyCode < 0 || keyCode >= static_cast<int>(HAL::MAX_KEYS))
        return false;
    return m_keysPressed[keyCode];
}

bool InputSubsystem::IsKeyReleased(int keyCode) const
{
    if (keyCode < 0 || keyCode >= static_cast<int>(HAL::MAX_KEYS))
        return false;
    return m_keysReleased[keyCode];
}

// =============================================================================
// Mouse API
// =============================================================================

bool InputSubsystem::IsMouseButtonDown(int button) const
{
    if (button < 0 || button >= static_cast<int>(HAL::MAX_MOUSE_BUTTONS))
        return false;
    return m_input.GetState().mouseButtons[button];
}

bool InputSubsystem::IsMouseButtonPressed(int button) const
{
    if (button < 0 || button >= static_cast<int>(HAL::MAX_MOUSE_BUTTONS))
        return false;
    return m_mouseButtonsPressed[button];
}

bool InputSubsystem::IsMouseButtonReleased(int button) const
{
    if (button < 0 || button >= static_cast<int>(HAL::MAX_MOUSE_BUTTONS))
        return false;
    return m_mouseButtonsReleased[button];
}

void InputSubsystem::GetMousePosition(float& x, float& y) const
{
    const auto& state = m_input.GetState();
    x = state.mouseX;
    y = state.mouseY;
}

void InputSubsystem::GetMouseDelta(float& dx, float& dy) const
{
    const auto& state = m_input.GetState();
    dx = state.mouseDeltaX;
    dy = state.mouseDeltaY;
}

void InputSubsystem::GetScrollDelta(float& x, float& y) const
{
    const auto& state = m_input.GetState();
    x = 0.0f;
    y = state.mouseWheel;
}

// =============================================================================
// Gamepad API
// =============================================================================

bool InputSubsystem::IsGamepadConnected(int index) const
{
    if (index < 0 || index >= static_cast<int>(HAL::MAX_GAMEPADS))
        return false;
    return m_gamepadStates[index].connected;
}

const std::string& InputSubsystem::GetGamepadName(int index) const
{
    if (index < 0 || index >= static_cast<int>(HAL::MAX_GAMEPADS))
        return s_emptyGamepadName;
    return m_gamepadStates[index].name;
}

bool InputSubsystem::IsGamepadButtonDown(int index, int button) const
{
    if (index < 0 || index >= static_cast<int>(HAL::MAX_GAMEPADS))
        return false;
    return m_gamepadStates[index].IsButtonDown(button);
}

bool InputSubsystem::IsGamepadButtonPressed(int index, int button) const
{
    if (index < 0 || index >= static_cast<int>(HAL::MAX_GAMEPADS))
        return false;
    return m_gamepadStates[index].IsButtonPressed(button);
}

bool InputSubsystem::IsGamepadButtonReleased(int index, int button) const
{
    if (index < 0 || index >= static_cast<int>(HAL::MAX_GAMEPADS))
        return false;
    return m_gamepadStates[index].IsButtonReleased(button);
}

float InputSubsystem::GetGamepadAxis(int index, int axis) const
{
    if (index < 0 || index >= static_cast<int>(HAL::MAX_GAMEPADS))
        return 0.0f;
    return m_gamepadStates[index].GetAxis(axis);
}

void InputSubsystem::GetGamepadLeftStick(int index, float& x, float& y) const
{
    if (index < 0 || index >= static_cast<int>(HAL::MAX_GAMEPADS))
    {
        x = 0.0f;
        y = 0.0f;
        return;
    }
    m_gamepadStates[index].GetLeftStick(x, y);
}

void InputSubsystem::GetGamepadRightStick(int index, float& x, float& y) const
{
    if (index < 0 || index >= static_cast<int>(HAL::MAX_GAMEPADS))
    {
        x = 0.0f;
        y = 0.0f;
        return;
    }
    m_gamepadStates[index].GetRightStick(x, y);
}

void InputSubsystem::GetGamepadTriggers(int index, float& left, float& right) const
{
    if (index < 0 || index >= static_cast<int>(HAL::MAX_GAMEPADS))
    {
        left = 0.0f;
        right = 0.0f;
        return;
    }
    m_gamepadStates[index].GetTriggers(left, right);
}

void InputSubsystem::SetGamepadVibration(int index, const HAL::GamepadVibration& vibration)
{
    if (m_gamepadBackend)
    {
        m_gamepadBackend->SetVibration(index, vibration);
    }
}

void InputSubsystem::StopGamepadVibration(int index)
{
    if (m_gamepadBackend)
    {
        m_gamepadBackend->StopVibration(index);
    }
}

const HAL::GamepadState& InputSubsystem::GetGamepadState(int index) const
{
    static HAL::GamepadState emptyState;
    if (index < 0 || index >= static_cast<int>(HAL::MAX_GAMEPADS))
        return emptyState;
    return m_gamepadStates[index];
}

// =============================================================================
// Touch API
// =============================================================================

bool InputSubsystem::IsTouchAvailable() const
{
    return m_touchBackend && m_touchBackend->IsAvailable();
}

uint32 InputSubsystem::GetTouchCount() const
{
    return m_touchState.activeCount;
}

const HAL::TouchPoint* InputSubsystem::GetTouch(uint32 index) const
{
    return m_touchState.GetTouch(index);
}

const HAL::TouchPoint* InputSubsystem::GetTouchById(uint32 id) const
{
    return m_touchState.GetTouchById(id);
}

const HAL::TouchState& InputSubsystem::GetTouchState() const
{
    return m_touchState;
}

GestureRecognizer& InputSubsystem::GetGestureRecognizer()
{
    return *m_gestureRecognizer;
}

const GestureRecognizer& InputSubsystem::GetGestureRecognizer() const
{
    return *m_gestureRecognizer;
}

// =============================================================================
// Virtual Joystick API
// =============================================================================

VirtualJoystick& InputSubsystem::GetLeftVirtualJoystick()
{
    return *m_leftVirtualJoystick;
}

const VirtualJoystick& InputSubsystem::GetLeftVirtualJoystick() const
{
    return *m_leftVirtualJoystick;
}

VirtualJoystick& InputSubsystem::GetRightVirtualJoystick()
{
    return *m_rightVirtualJoystick;
}

const VirtualJoystick& InputSubsystem::GetRightVirtualJoystick() const
{
    return *m_rightVirtualJoystick;
}

void InputSubsystem::SetVirtualJoysticksEnabled(bool enabled)
{
    m_virtualJoysticksEnabled = enabled;
    if (!enabled)
    {
        m_leftVirtualJoystick->Reset();
        m_rightVirtualJoystick->Reset();
    }
}

} // namespace RVX
