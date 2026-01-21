#include "Runtime/Camera/Camera.h"

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
        m_useLookAt = false;  // Switch to euler angle mode
        UpdateMatrices();
    }

    void Camera::LookAt(const Vec3& target)
    {
        m_target = target;
        m_useLookAt = true;  // Switch to look-at mode
        UpdateMatrices();
    }

    void Camera::UpdateMatrices()
    {
        // Use glm::lookAt for reliable view matrix construction
        if (m_useLookAt)
        {
            // Standard up vector
            Vec3 up(0.0f, 1.0f, 0.0f);
            m_view = glm::lookAt(m_position, m_target, up);
        }
        else
        {
            // Build view matrix from euler angles
            // Order: first yaw (Y), then pitch (X), then roll (Z)
            Mat4 rotX = glm::rotate(Mat4(1.0f), -m_rotation.x, Vec3(1.0f, 0.0f, 0.0f));
            Mat4 rotY = glm::rotate(Mat4(1.0f), -m_rotation.y, Vec3(0.0f, 1.0f, 0.0f));
            Mat4 rotZ = glm::rotate(Mat4(1.0f), -m_rotation.z, Vec3(0.0f, 0.0f, 1.0f));
            // Camera rotations: Y (yaw) first, then X (pitch), then Z (roll)
            Mat4 rotation = rotZ * rotX * rotY;
            Mat4 translation = MakeTranslation(-m_position);
            m_view = rotation * translation;
        }

        if (m_projectionType == CameraProjection::Perspective)
        {
            m_projection = MakePerspective(m_fov, m_aspect, m_nearZ, m_farZ);
        }

        m_viewProjection = m_projection * m_view;
    }
} // namespace RVX
