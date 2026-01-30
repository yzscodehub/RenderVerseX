#include "Scene/Components/ReflectionProbeComponent.h"
#include "Scene/SceneEntity.h"

namespace RVX
{

void ReflectionProbeComponent::OnAttach()
{
    NotifyBoundsChanged();
}

void ReflectionProbeComponent::OnDetach()
{
    // Nothing special needed
}

AABB ReflectionProbeComponent::GetLocalBounds() const
{
    if (m_shape == ReflectionProbeShape::Sphere)
    {
        float radius = m_size.x;
        return AABB(-Vec3(radius), Vec3(radius));
    }
    else
    {
        return AABB(-m_size, m_size);
    }
}

void ReflectionProbeComponent::SetShape(ReflectionProbeShape shape)
{
    if (m_shape != shape)
    {
        m_shape = shape;
        NotifyBoundsChanged();
    }
}

void ReflectionProbeComponent::SetSize(const Vec3& size)
{
    if (m_size != size)
    {
        m_size = size;
        NotifyBoundsChanged();
    }
}

void ReflectionProbeComponent::SetCubemap(Resource::ResourceHandle<Resource::TextureResource> cubemap)
{
    m_cubemap = cubemap;
}

bool ReflectionProbeComponent::HasValidCubemap() const
{
    return m_cubemap.IsValid() && m_cubemap.IsLoaded();
}

void ReflectionProbeComponent::Bake()
{
    if (m_isBaking)
    {
        return;
    }

    m_isBaking = true;

    // TODO: Trigger cubemap capture through render system
    // This would typically:
    // 1. Set up 6 cameras facing each cube face
    // 2. Render scene from probe position
    // 3. Generate mipmaps for specular reflections
    // 4. Store result in m_cubemap

    // For now, just mark as complete
    m_isBaking = false;
}

void ReflectionProbeComponent::RequestRender()
{
    if (m_mode != ReflectionProbeMode::Realtime)
    {
        return;
    }

    // TODO: Queue probe for realtime rendering
    // This would be picked up by the render system
}

} // namespace RVX
