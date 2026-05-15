#include "Scene/PrimitiveComponent.h"
#include "Scene/SceneEntity.h"
#include "Scene/SceneManager.h"

namespace RVX
{

void PrimitiveComponent::OnRegister()
{
    auto* entity = dynamic_cast<SceneEntity*>(GetOwner());
    if (!entity || !entity->GetSceneManager())
        return;

    entity->GetSceneManager()->RegisterPrimitive(this);
}

void PrimitiveComponent::OnUnregister()
{
    auto* entity = dynamic_cast<SceneEntity*>(GetOwner());
    if (!entity || !entity->GetSceneManager())
        return;

    entity->GetSceneManager()->UnregisterPrimitive(this);
}

void PrimitiveComponent::SetEnabled(bool enabled)
{
    if (IsEnabled() == enabled)
        return;

    ActorComponent::SetEnabled(enabled);
    MarkSpatialDirty();
}

void PrimitiveComponent::SetLayerMask(uint32 layerMask)
{
    if (m_layerMask == layerMask)
        return;

    m_layerMask = layerMask;
    MarkSpatialDirty();
}

void PrimitiveComponent::SetLocalBounds(const AABB& bounds)
{
    m_localBounds = bounds;
    m_boundsDirty = true;
    MarkSpatialDirty();
}

AABB PrimitiveComponent::GetWorldBounds() const
{
    if (m_boundsDirty)
    {
        m_cachedWorldBounds = m_localBounds.Transformed(GetWorldTransform());
        m_boundsDirty = false;
    }
    return m_cachedWorldBounds;
}

void PrimitiveComponent::OnTransformChanged()
{
    m_boundsDirty = true;
    MarkSpatialDirty();
}

void PrimitiveComponent::MarkSpatialDirty()
{
    m_spatialDirty = true;

    auto* entity = dynamic_cast<SceneEntity*>(GetOwner());
    if (entity)
    {
        entity->MarkBoundsDirty();
    }
}

} // namespace RVX
