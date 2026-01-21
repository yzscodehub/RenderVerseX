#pragma once

/**
 * @file OrbitController.h
 * @brief Orbital camera controller
 */

#include "Runtime/Camera/CameraController.h"

namespace RVX
{
    /**
     * @brief Orbit camera controller
     * 
     * Orbits around a target point. Left mouse to rotate, scroll to zoom.
     */
    class OrbitController : public CameraController
    {
    public:
        void Update(Camera& camera, const HAL::InputState& input, float deltaTime) override;

        /// Set orbit target point
        void SetTarget(const Vec3& target) { m_target = target; }
        
        /// Set distance from target
        void SetDistance(float distance) { m_distance = distance; }
        
        const Vec3& GetTarget() const { return m_target; }
        float GetDistance() const { return m_distance; }

    private:
        Vec3 m_target{0.0f, 0.0f, 0.0f};
        float m_distance = 5.0f;
        float m_rotateSpeed = 1.5f;
        float m_zoomSpeed = 5.0f;
    };
} // namespace RVX
