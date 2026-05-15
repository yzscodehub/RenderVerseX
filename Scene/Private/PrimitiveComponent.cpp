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

void PrimitiveComponent::SetLocalBounds(const AABB& bounds)
{
    m_localBounds = bounds;
    m_boundsDirty = true;
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
}

} // namespace RVX
