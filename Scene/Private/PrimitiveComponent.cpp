#include "Scene/PrimitiveComponent.h"

namespace RVX
{

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
