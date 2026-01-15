#pragma once

#include "Camera/CameraController.h"

namespace RVX
{
    class FPSController : public CameraController
    {
    public:
        void Update(Camera& camera, const InputState& input, float deltaTime) override;

        void SetMoveSpeed(float speed) { m_moveSpeed = speed; }
        void SetLookSpeed(float speed) { m_lookSpeed = speed; }

    private:
        float m_moveSpeed = 5.0f;
        float m_lookSpeed = 2.0f;
    };
} // namespace RVX
