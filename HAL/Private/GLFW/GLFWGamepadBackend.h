#pragma once

/**
 * @file GLFWGamepadBackend.h
 * @brief GLFW-based gamepad backend implementation
 */

#include "HAL/Input/GamepadState.h"
#include <GLFW/glfw3.h>

#ifdef _WIN32
    #define RVX_XINPUT_VIBRATION 1
#else
    #define RVX_XINPUT_VIBRATION 0
#endif

namespace RVX::HAL
{
    /**
     * @brief GLFW gamepad backend implementation
     * 
     * Uses GLFW's joystick API for gamepad input.
     * On Windows, uses XInput for vibration feedback.
     */
    class GLFWGamepadBackend final : public IGamepadBackend
    {
    public:
        GLFWGamepadBackend();
        ~GLFWGamepadBackend() override;

        void Poll(std::array<GamepadState, MAX_GAMEPADS>& states) override;
        bool SupportsVibration() const override;
        void SetVibration(int gamepadIndex, const GamepadVibration& vibration) override;
        void StopVibration(int gamepadIndex) override;

        /**
         * @brief Update vibration timers (call each frame)
         */
        void UpdateVibration(float deltaTime);

    private:
        void PollGamepad(int index, GamepadState& state);

#if RVX_XINPUT_VIBRATION
        void ApplyXInputVibration(int index, float lowFreq, float highFreq);
#endif

        struct VibrationState
        {
            float lowFrequency = 0.0f;
            float highFrequency = 0.0f;
            float remainingTime = 0.0f;
            bool active = false;
        };

        std::array<VibrationState, MAX_GAMEPADS> m_vibration{};
    };

} // namespace RVX::HAL

// Backward compatibility
namespace RVX
{
    using GLFWGamepadBackend = HAL::GLFWGamepadBackend;
}
