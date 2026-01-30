/**
 * @file GLFWGamepadBackend.cpp
 * @brief GLFWGamepadBackend implementation
 */

#include "GLFWGamepadBackend.h"
#include "Core/Log.h"
#include <algorithm>
#include <cstring>

#if RVX_XINPUT_VIBRATION
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
    #include <xinput.h>
    #pragma comment(lib, "xinput.lib")
#endif

namespace RVX::HAL
{

GLFWGamepadBackend::GLFWGamepadBackend()
{
    RVX_CORE_DEBUG("GLFWGamepadBackend: Initialized");
}

GLFWGamepadBackend::~GLFWGamepadBackend()
{
    // Stop all vibrations
    for (int i = 0; i < static_cast<int>(MAX_GAMEPADS); ++i)
    {
        StopVibration(i);
    }
}

void GLFWGamepadBackend::Poll(std::array<GamepadState, MAX_GAMEPADS>& states)
{
    for (int i = 0; i < static_cast<int>(MAX_GAMEPADS); ++i)
    {
        // Save previous button states
        states[i].prevButtons = states[i].buttons;
        
        // Poll this gamepad
        PollGamepad(i, states[i]);
    }
}

void GLFWGamepadBackend::PollGamepad(int index, GamepadState& state)
{
    // GLFW joystick IDs are GLFW_JOYSTICK_1 through GLFW_JOYSTICK_16
    // We only support MAX_GAMEPADS (4)
    int jid = GLFW_JOYSTICK_1 + index;

    if (!glfwJoystickPresent(jid))
    {
        if (state.connected)
        {
            RVX_CORE_INFO("Gamepad {} disconnected", index);
        }
        state.connected = false;
        state.name.clear();
        state.buttons.fill(false);
        state.axes.fill(0.0f);
        return;
    }

    // Check if this is a gamepad (has standard mapping)
    if (!glfwJoystickIsGamepad(jid))
    {
        // It's a joystick but not a recognized gamepad
        // Still mark as connected but use raw joystick data
        if (!state.connected)
        {
            const char* name = glfwGetJoystickName(jid);
            state.name = name ? name : "Unknown Joystick";
            RVX_CORE_INFO("Joystick {} connected: {}", index, state.name);
        }
        state.connected = true;

        // Get raw joystick data
        int axisCount = 0;
        const float* axes = glfwGetJoystickAxes(jid, &axisCount);
        for (int i = 0; i < std::min(axisCount, static_cast<int>(GamepadAxis::Count)); ++i)
        {
            state.axes[i] = axes[i];
        }

        int buttonCount = 0;
        const unsigned char* buttons = glfwGetJoystickButtons(jid, &buttonCount);
        for (int i = 0; i < std::min(buttonCount, static_cast<int>(GamepadButton::Count)); ++i)
        {
            state.buttons[i] = (buttons[i] == GLFW_PRESS);
        }
        return;
    }

    // It's a recognized gamepad
    if (!state.connected)
    {
        const char* name = glfwGetGamepadName(jid);
        state.name = name ? name : "Unknown Gamepad";
        RVX_CORE_INFO("Gamepad {} connected: {}", index, state.name);
    }
    state.connected = true;

    // Get gamepad state (uses standard mapping)
    GLFWgamepadstate glfwState;
    if (glfwGetGamepadState(jid, &glfwState))
    {
        // Map GLFW gamepad buttons to our button indices
        state.buttons[GamepadButton::A]           = (glfwState.buttons[GLFW_GAMEPAD_BUTTON_A] == GLFW_PRESS);
        state.buttons[GamepadButton::B]           = (glfwState.buttons[GLFW_GAMEPAD_BUTTON_B] == GLFW_PRESS);
        state.buttons[GamepadButton::X]           = (glfwState.buttons[GLFW_GAMEPAD_BUTTON_X] == GLFW_PRESS);
        state.buttons[GamepadButton::Y]           = (glfwState.buttons[GLFW_GAMEPAD_BUTTON_Y] == GLFW_PRESS);
        state.buttons[GamepadButton::LeftBumper]  = (glfwState.buttons[GLFW_GAMEPAD_BUTTON_LEFT_BUMPER] == GLFW_PRESS);
        state.buttons[GamepadButton::RightBumper] = (glfwState.buttons[GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER] == GLFW_PRESS);
        state.buttons[GamepadButton::Back]        = (glfwState.buttons[GLFW_GAMEPAD_BUTTON_BACK] == GLFW_PRESS);
        state.buttons[GamepadButton::Start]       = (glfwState.buttons[GLFW_GAMEPAD_BUTTON_START] == GLFW_PRESS);
        state.buttons[GamepadButton::Guide]       = (glfwState.buttons[GLFW_GAMEPAD_BUTTON_GUIDE] == GLFW_PRESS);
        state.buttons[GamepadButton::LeftThumb]   = (glfwState.buttons[GLFW_GAMEPAD_BUTTON_LEFT_THUMB] == GLFW_PRESS);
        state.buttons[GamepadButton::RightThumb]  = (glfwState.buttons[GLFW_GAMEPAD_BUTTON_RIGHT_THUMB] == GLFW_PRESS);
        state.buttons[GamepadButton::DPadUp]      = (glfwState.buttons[GLFW_GAMEPAD_BUTTON_DPAD_UP] == GLFW_PRESS);
        state.buttons[GamepadButton::DPadRight]   = (glfwState.buttons[GLFW_GAMEPAD_BUTTON_DPAD_RIGHT] == GLFW_PRESS);
        state.buttons[GamepadButton::DPadDown]    = (glfwState.buttons[GLFW_GAMEPAD_BUTTON_DPAD_DOWN] == GLFW_PRESS);
        state.buttons[GamepadButton::DPadLeft]    = (glfwState.buttons[GLFW_GAMEPAD_BUTTON_DPAD_LEFT] == GLFW_PRESS);

        // Map axes
        state.axes[GamepadAxis::LeftX]        = glfwState.axes[GLFW_GAMEPAD_AXIS_LEFT_X];
        state.axes[GamepadAxis::LeftY]        = glfwState.axes[GLFW_GAMEPAD_AXIS_LEFT_Y];
        state.axes[GamepadAxis::RightX]       = glfwState.axes[GLFW_GAMEPAD_AXIS_RIGHT_X];
        state.axes[GamepadAxis::RightY]       = glfwState.axes[GLFW_GAMEPAD_AXIS_RIGHT_Y];
        // Triggers come as -1 to 1, convert to 0 to 1
        state.axes[GamepadAxis::LeftTrigger]  = (glfwState.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] + 1.0f) * 0.5f;
        state.axes[GamepadAxis::RightTrigger] = (glfwState.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] + 1.0f) * 0.5f;
    }
}

bool GLFWGamepadBackend::SupportsVibration() const
{
#if RVX_XINPUT_VIBRATION
    return true;
#else
    return false;
#endif
}

void GLFWGamepadBackend::SetVibration(int gamepadIndex, const GamepadVibration& vibration)
{
    if (gamepadIndex < 0 || gamepadIndex >= static_cast<int>(MAX_GAMEPADS))
        return;

    auto& vib = m_vibration[gamepadIndex];
    vib.lowFrequency = std::clamp(vibration.lowFrequency, 0.0f, 1.0f);
    vib.highFrequency = std::clamp(vibration.highFrequency, 0.0f, 1.0f);
    vib.remainingTime = vibration.duration;
    vib.active = true;

#if RVX_XINPUT_VIBRATION
    ApplyXInputVibration(gamepadIndex, vib.lowFrequency, vib.highFrequency);
#endif
}

void GLFWGamepadBackend::StopVibration(int gamepadIndex)
{
    if (gamepadIndex < 0 || gamepadIndex >= static_cast<int>(MAX_GAMEPADS))
        return;

    auto& vib = m_vibration[gamepadIndex];
    vib.lowFrequency = 0.0f;
    vib.highFrequency = 0.0f;
    vib.remainingTime = 0.0f;
    vib.active = false;

#if RVX_XINPUT_VIBRATION
    ApplyXInputVibration(gamepadIndex, 0.0f, 0.0f);
#endif
}

void GLFWGamepadBackend::UpdateVibration(float deltaTime)
{
    for (int i = 0; i < static_cast<int>(MAX_GAMEPADS); ++i)
    {
        auto& vib = m_vibration[i];
        if (!vib.active)
            continue;

        // Duration of 0 means continuous
        if (vib.remainingTime > 0.0f)
        {
            vib.remainingTime -= deltaTime;
            if (vib.remainingTime <= 0.0f)
            {
                StopVibration(i);
            }
        }
    }
}

#if RVX_XINPUT_VIBRATION
void GLFWGamepadBackend::ApplyXInputVibration(int index, float lowFreq, float highFreq)
{
    if (index < 0 || index >= XUSER_MAX_COUNT)
        return;

    XINPUT_VIBRATION vibration;
    vibration.wLeftMotorSpeed = static_cast<WORD>(lowFreq * 65535.0f);
    vibration.wRightMotorSpeed = static_cast<WORD>(highFreq * 65535.0f);
    XInputSetState(static_cast<DWORD>(index), &vibration);
}
#endif

} // namespace RVX::HAL
