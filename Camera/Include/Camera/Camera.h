#pragma once

#include "Core/MathTypes.h"

namespace RVX
{
    enum class CameraProjection
    {
        Perspective,
        Orthographic
    };

    struct CameraViewport
    {
        float x = 0.0f;
        float y = 0.0f;
        float width = 1.0f;
        float height = 1.0f;
    };

    class Camera
    {
    public:
        void SetPerspective(float fovRadians, float aspect, float nearZ, float farZ);
        void SetOrthographic(float width, float height, float nearZ, float farZ);
        void SetViewport(const CameraViewport& viewport);

        void SetPosition(const Vec3& position);
        void SetRotation(const Vec3& eulerRadians);
        const Vec3& GetPosition() const { return m_position; }
        const Vec3& GetRotation() const { return m_rotation; }

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
    };
} // namespace RVX
