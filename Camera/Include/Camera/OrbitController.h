#pragma once

#include "Camera/CameraController.h"

namespace RVX
{
    class OrbitController : public CameraController
    {
    public:
        void Update(Camera& camera, const InputState& input, float deltaTime) override;

        void SetTarget(const Vec3& target) { m_target = target; }
        void SetDistance(float distance) { m_distance = distance; }

    private:
        Vec3 m_target{0.0f, 0.0f, 0.0f};
        float m_distance = 5.0f;
        float m_rotateSpeed = 1.5f;
        float m_zoomSpeed = 5.0f;
    };
} // namespace RVX
