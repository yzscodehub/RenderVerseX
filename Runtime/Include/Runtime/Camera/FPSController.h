#pragma once

/**
 * @file FPSController.h
 * @brief First-person shooter style camera controller
 */

#include "Runtime/Camera/CameraController.h"

namespace RVX
{
    /**
     * @brief FPS-style camera controller
     * 
     * Uses WASD for movement, mouse for looking.
     */
    class FPSController : public CameraController
    {
    public:
        void Update(Camera& camera, const HAL::InputState& input, float deltaTime) override;

        /// Set movement speed (units/second)
        void SetMoveSpeed(float speed) { m_moveSpeed = speed; }
        
        /// Set look sensitivity
        void SetLookSpeed(float speed) { m_lookSpeed = speed; }

        float GetMoveSpeed() const { return m_moveSpeed; }
        float GetLookSpeed() const { return m_lookSpeed; }

    private:
        float m_moveSpeed = 5.0f;
        float m_lookSpeed = 2.0f;
    };
} // namespace RVX
