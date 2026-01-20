#include "Camera/Camera.h"

namespace RVX
{
    void Camera::SetPerspective(float fovRadians, float aspect, float nearZ, float farZ)
    {
        m_projectionType = CameraProjection::Perspective;
        m_fov = fovRadians;
        m_aspect = aspect;
        m_nearZ = nearZ;
        m_farZ = farZ;
        UpdateMatrices();
    }

    void Camera::SetOrthographic(float width, float height, float nearZ, float farZ)
    {
        m_projectionType = CameraProjection::Orthographic;
        m_nearZ = nearZ;
        m_farZ = farZ;
        m_projection = MakeOrthographic(width, height, nearZ, farZ);
        UpdateMatrices();
    }

    void Camera::SetViewport(const CameraViewport& viewport)
    {
        m_viewport = viewport;
    }

    void Camera::SetPosition(const Vec3& position)
    {
        m_position = position;
        UpdateMatrices();
    }

    void Camera::SetRotation(const Vec3& eulerRadians)
    {
        m_rotation = eulerRadians;
        UpdateMatrices();
    }

    void Camera::UpdateMatrices()
    {
        Mat4 rotation = MakeRotationXYZ(Vec3(-m_rotation.x, -m_rotation.y, -m_rotation.z));
        Mat4 translation = MakeTranslation(Vec3(-m_position.x, -m_position.y, -m_position.z));
        m_view = rotation * translation;

        if (m_projectionType == CameraProjection::Perspective)
        {
            m_projection = MakePerspective(m_fov, m_aspect, m_nearZ, m_farZ);
        }

        m_viewProjection = m_projection * m_view;
    }
} // namespace RVX
