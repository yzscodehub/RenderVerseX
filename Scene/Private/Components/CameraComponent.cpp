#include "Scene/Components/CameraComponent.h"
#include "Scene/SceneEntity.h"

namespace RVX
{

void CameraComponent::OnAttach()
{
    // Nothing special needed
}

void CameraComponent::OnDetach()
{
    // Nothing special needed
}

void CameraComponent::SetProjectionType(ProjectionType type)
{
    if (m_projectionType != type)
    {
        m_projectionType = type;
        m_projectionDirty = true;
    }
}

Mat4 CameraComponent::GetViewMatrix() const
{
    SceneEntity* owner = GetOwner();
    if (!owner)
    {
        return Mat4(1.0f);
    }

    // Get world transform and invert it for view matrix
    Mat4 worldMatrix = owner->GetWorldMatrix();
    return inverse(worldMatrix);
}

Mat4 CameraComponent::GetProjectionMatrix() const
{
    if (m_projectionDirty)
    {
        UpdateProjectionMatrix();
    }
    return m_projectionMatrix;
}

Mat4 CameraComponent::GetViewProjectionMatrix() const
{
    return GetProjectionMatrix() * GetViewMatrix();
}

Mat4 CameraComponent::GetInverseViewMatrix() const
{
    SceneEntity* owner = GetOwner();
    if (!owner)
    {
        return Mat4(1.0f);
    }
    return owner->GetWorldMatrix();
}

void CameraComponent::UpdateProjectionMatrix() const
{
    if (m_projectionType == ProjectionType::Perspective)
    {
        m_projectionMatrix = perspective(m_fieldOfView, m_aspectRatio, m_nearPlane, m_farPlane);
    }
    else
    {
        float halfHeight = m_orthoSize;
        float halfWidth = halfHeight * m_aspectRatio;
        m_projectionMatrix = ortho(-halfWidth, halfWidth, -halfHeight, halfHeight, m_nearPlane, m_farPlane);
    }
    m_projectionDirty = false;
}

void CameraComponent::ScreenToWorldRay(const Vec2& screenPos, Vec3& outOrigin, Vec3& outDirection) const
{
    SceneEntity* owner = GetOwner();
    if (!owner)
    {
        outOrigin = Vec3(0.0f);
        outDirection = Vec3(0.0f, 0.0f, -1.0f);
        return;
    }

    // Convert screen pos from [0,1] to [-1,1] (NDC)
    Vec4 ndcNear(screenPos.x * 2.0f - 1.0f, 1.0f - screenPos.y * 2.0f, 0.0f, 1.0f);
    Vec4 ndcFar(screenPos.x * 2.0f - 1.0f, 1.0f - screenPos.y * 2.0f, 1.0f, 1.0f);

    // Transform to world space
    Mat4 invViewProj = inverse(GetViewProjectionMatrix());
    
    Vec4 worldNear = invViewProj * ndcNear;
    Vec4 worldFar = invViewProj * ndcFar;

    // Perspective divide
    worldNear /= worldNear.w;
    worldFar /= worldFar.w;

    outOrigin = Vec3(worldNear);
    outDirection = normalize(Vec3(worldFar) - Vec3(worldNear));
}

Vec3 CameraComponent::WorldToScreenPoint(const Vec3& worldPos) const
{
    Mat4 viewProj = GetViewProjectionMatrix();
    Vec4 clipPos = viewProj * Vec4(worldPos, 1.0f);
    
    // Perspective divide
    if (clipPos.w != 0.0f)
    {
        clipPos /= clipPos.w;
    }

    // Convert from NDC [-1,1] to screen [0,1]
    return Vec3(
        (clipPos.x + 1.0f) * 0.5f,
        (1.0f - clipPos.y) * 0.5f,
        (clipPos.z + 1.0f) * 0.5f  // Depth in [0,1]
    );
}

Vec3 CameraComponent::GetForward() const
{
    SceneEntity* owner = GetOwner();
    if (!owner)
    {
        return Vec3(0.0f, 0.0f, -1.0f);
    }

    Mat4 worldMatrix = owner->GetWorldMatrix();
    return -Vec3(worldMatrix[2]);  // Negative Z axis
}

Vec3 CameraComponent::GetRight() const
{
    SceneEntity* owner = GetOwner();
    if (!owner)
    {
        return Vec3(1.0f, 0.0f, 0.0f);
    }

    Mat4 worldMatrix = owner->GetWorldMatrix();
    return Vec3(worldMatrix[0]);  // X axis
}

Vec3 CameraComponent::GetUp() const
{
    SceneEntity* owner = GetOwner();
    if (!owner)
    {
        return Vec3(0.0f, 1.0f, 0.0f);
    }

    Mat4 worldMatrix = owner->GetWorldMatrix();
    return Vec3(worldMatrix[1]);  // Y axis
}

} // namespace RVX
