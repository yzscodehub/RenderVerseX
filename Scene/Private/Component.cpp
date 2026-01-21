#include "Scene/Component.h"
#include "Scene/SceneEntity.h"

namespace RVX
{

void Component::NotifyBoundsChanged()
{
    if (m_owner)
    {
        m_owner->MarkBoundsDirty();
    }
}

} // namespace RVX
