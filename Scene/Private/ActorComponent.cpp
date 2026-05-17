#include "Scene/ActorComponent.h"

namespace RVX
{

void ActorComponent::SetCanEverTick(bool canEverTick)
{
    m_canEverTick = canEverTick;
    if (!m_canEverTick)
    {
        m_tickEnabled = false;
    }
}

void ActorComponent::SetTickEnabled(bool enabled)
{
    m_tickEnabled = enabled && m_canEverTick;
}

} // namespace RVX
