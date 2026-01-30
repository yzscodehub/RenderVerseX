#include "Scene/Components/DecalComponent.h"
#include "Scene/SceneEntity.h"

namespace RVX
{

void DecalComponent::OnAttach()
{
    NotifyBoundsChanged();
}

void DecalComponent::OnDetach()
{
    // Nothing special needed
}

AABB DecalComponent::GetLocalBounds() const
{
    return AABB(-m_size, m_size);
}

void DecalComponent::SetMaterial(Resource::ResourceHandle<Resource::MaterialResource> material)
{
    m_material = material;
}

void DecalComponent::SetSize(const Vec3& size)
{
    if (m_size != size)
    {
        m_size = size;
        NotifyBoundsChanged();
    }
}

void DecalComponent::SetProjectionDepth(float depth)
{
    if (m_size.z != depth)
    {
        m_size.z = depth;
        NotifyBoundsChanged();
    }
}

Mat4 DecalComponent::GetProjectionMatrix() const
{
    SceneEntity* owner = GetOwner();
    if (!owner)
    {
        return Mat4(1.0f);
    }

    // Create orthographic projection for the decal box
    Mat4 worldMatrix = owner->GetWorldMatrix();

    // Scale by inverse size to create unit box projection
    Mat4 scaleInv = MakeScale(Vec3(1.0f / m_size.x, 1.0f / m_size.y, 1.0f / m_size.z));

    // Decal projection = ScaleInverse * WorldInverse
    // This transforms world positions into decal UV space
    return scaleInv * inverse(worldMatrix);
}

Mat4 DecalComponent::GetInverseProjectionMatrix() const
{
    SceneEntity* owner = GetOwner();
    if (!owner)
    {
        return Mat4(1.0f);
    }

    // Create the inverse of projection matrix
    Mat4 worldMatrix = owner->GetWorldMatrix();
    Mat4 scaleMatrix = MakeScale(m_size);

    return worldMatrix * scaleMatrix;
}

} // namespace RVX
