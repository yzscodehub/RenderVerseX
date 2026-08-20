#include "Core/Camera/Camera.h"

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
        m_orthoWidth = width;
        m_orthoHeight = height;
        if (height != 0.0f)
        {
            m_aspect = width / height;
        }
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
        m_useLookAt = false;
        UpdateMatrices();
    }

    void Camera::LookAt(const Vec3& target)
    {
        m_target = target;
        m_useLookAt = true;
        UpdateMatrices();
    }

    void Camera::UpdateMatrices()
    {
        if (m_useLookAt)
        {
            const Vec3 up(0.0f, 1.0f, 0.0f);
            m_view = glm::lookAt(m_position, m_target, up);
        }
        else
        {
            const Mat4 rotX = glm::rotate(Mat4(1.0f), -m_rotation.x, Vec3(1.0f, 0.0f, 0.0f));
            const Mat4 rotY = glm::rotate(Mat4(1.0f), -m_rotation.y, Vec3(0.0f, 1.0f, 0.0f));
            const Mat4 rotZ = glm::rotate(Mat4(1.0f), -m_rotation.z, Vec3(0.0f, 0.0f, 1.0f));
            const Mat4 rotation = rotZ * rotX * rotY;
            const Mat4 translation = MakeTranslation(-m_position);
            m_view = rotation * translation;
        }

        if (m_projectionType == CameraProjection::Perspective)
        {
            m_projection = MakePerspective(m_fov, m_aspect, m_nearZ, m_farZ);
        }

        m_viewProjection = m_projection * m_view;
    }
} // namespace RVX
