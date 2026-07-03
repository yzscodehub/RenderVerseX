#pragma once

/**
 * @file Camera.h
 * @brief Shared camera data and projection matrices.
 */

#include "Core/Types.h"
#include "Core/MathTypes.h"

namespace RVX
{
    /**
     * @brief Camera projection types.
     */
    enum class CameraProjection : uint8
    {
        Perspective,
        Orthographic
    };

    /**
     * @brief Camera viewport specification.
     */
    struct CameraViewport
    {
        float x = 0.0f;
        float y = 0.0f;
        float width = 1.0f;
        float height = 1.0f;
    };

    /**
     * @brief Shared camera contract with view and projection matrices.
     */
    class Camera
    {
    public:
        // =========================================================================
        // Projection
        // =========================================================================
        void SetPerspective(float fovRadians, float aspect, float nearZ, float farZ);
        void SetOrthographic(float width, float height, float nearZ, float farZ);
        void SetViewport(const CameraViewport& viewport);

        // =========================================================================
        // Transform
        // =========================================================================
        void SetPosition(const Vec3& position);
        void SetRotation(const Vec3& eulerRadians);
        void LookAt(const Vec3& target);

        const Vec3& GetPosition() const { return m_position; }
        const Vec3& GetRotation() const { return m_rotation; }

        // =========================================================================
        // Matrices
        // =========================================================================
        const Mat4& GetView() const { return m_view; }
        const Mat4& GetProjection() const { return m_projection; }
        const Mat4& GetViewProjection() const { return m_viewProjection; }

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

        Vec3 m_target{0.0f, 0.0f, 0.0f};
        bool m_useLookAt = false;
    };
} // namespace RVX
