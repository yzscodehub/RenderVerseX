#pragma once

/**
 * @file Camera.h
 * @brief Camera data and projection matrices
 */

#include "Core/MathTypes.h"

namespace RVX
{
    /**
     * @brief Camera projection types
     */
    enum class CameraProjection
    {
        Perspective,
        Orthographic
    };

    /**
     * @brief Camera viewport specification
     */
    struct CameraViewport
    {
        float x = 0.0f;
        float y = 0.0f;
        float width = 1.0f;
        float height = 1.0f;
    };

    /**
     * @brief Camera class with view and projection matrices
     * 
     * Manages camera position, rotation, and projection settings.
     */
    class Camera
    {
    public:
        /// Set perspective projection
        void SetPerspective(float fovRadians, float aspect, float nearZ, float farZ);
        
        /// Set orthographic projection
        void SetOrthographic(float width, float height, float nearZ, float farZ);
        
        /// Set viewport rect (normalized 0-1)
        void SetViewport(const CameraViewport& viewport);

        /// Set world position
        void SetPosition(const Vec3& position);
        
        /// Set rotation in radians (pitch, yaw, roll)
        void SetRotation(const Vec3& eulerRadians);
        
        /// Set camera to look at a target point
        void LookAt(const Vec3& target);
        
        /// Get world position
        const Vec3& GetPosition() const { return m_position; }
        
        /// Get rotation in radians
        const Vec3& GetRotation() const { return m_rotation; }

        /// Get view matrix
        const Mat4& GetView() const { return m_view; }
        
        /// Get projection matrix
        const Mat4& GetProjection() const { return m_projection; }
        
        /// Get combined view-projection matrix
        const Mat4& GetViewProjection() const { return m_viewProjection; }

        /// Recalculate matrices from current settings
        void UpdateMatrices();

    private:
        CameraProjection m_projectionType = CameraProjection::Perspective;
        CameraViewport m_viewport;
        Vec3 m_position{0.0f, 0.0f, 0.0f};
        Vec3 m_rotation{0.0f, 0.0f, 0.0f};
        float m_fov = 1.0f;
        float m_aspect = 1.0f;
        float m_nearZ = 0.1f;
        float m_farZ = 1000.0f;

        Mat4 m_view = Mat4Identity();
        Mat4 m_projection = Mat4Identity();
        Mat4 m_viewProjection = Mat4Identity();
        
        Vec3 m_target{0.0f, 0.0f, 0.0f};  // Look-at target
        bool m_useLookAt = false;          // Use lookAt instead of euler angles
    };
} // namespace RVX
