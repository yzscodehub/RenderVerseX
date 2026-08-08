#include "Scene/PrimitiveComponent.h"
#include "Scene/SceneEntity.h"
#include "Scene/SceneManager.h"
#include "Scene/SceneRuntime.h"

namespace RVX
{

void PrimitiveComponent::OnRegister()
{
    auto* entity = dynamic_cast<SceneEntity*>(GetOwner());
    if (!entity)
        return;

    if (Scene* scene = entity->GetScene())
    {
        scene->RegisterSpatialPrimitive(this);
    }
    else if (SceneManager* compatibilityManager =
                 entity->GetSceneManager())
    {
        // Compatibility-only path for standalone legacy SceneManager users.
        compatibilityManager->RegisterPrimitive(this);
    }
}

void PrimitiveComponent::OnUnregister()
{
    auto* entity = dynamic_cast<SceneEntity*>(GetOwner());
    if (!entity)
        return;

    if (Scene* scene = entity->GetScene())
    {
        scene->UnregisterSpatialPrimitive(this);
    }
    else if (SceneManager* compatibilityManager =
                 entity->GetSceneManager())
    {
        // Compatibility-only path for standalone legacy SceneManager users.
        compatibilityManager->UnregisterPrimitive(this);
    }
}

void PrimitiveComponent::SetEnabled(bool enabled)
{
    if (IsEnabled() == enabled)
        return;

    ActorComponent::SetEnabled(enabled);
    MarkSpatialDirty();
}

void PrimitiveComponent::SetVisible(bool visible)
{
    if (m_visible == visible)
        return;
    m_visible = visible;
    NotifySceneStateChanged();
    MarkSpatialDirty();
}

void PrimitiveComponent::SetLayerMask(uint32 layerMask)
{
    if (m_layerMask == layerMask)
        return;

    m_layerMask = layerMask;
    NotifySceneStateChanged();
    MarkSpatialDirty();
}

void PrimitiveComponent::SetLocalBounds(const AABB& bounds)
{
    m_localBounds = bounds;
    m_boundsDirty = true;
    NotifySceneStateChanged();
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
        if (entity->GetScene())
            entity->GetScene()->MarkSpatialPrimitiveDirty(this);
        else if (SceneManager* compatibilityManager =
                     entity->GetSceneManager())
            compatibilityManager->MarkPrimitiveSpatialDirty(this);

        entity->MarkBoundsDirty();
    }
}

} // namespace RVX
