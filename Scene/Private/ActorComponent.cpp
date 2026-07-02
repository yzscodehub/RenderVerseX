#include "Scene/ActorComponent.h"

#include <atomic>

namespace RVX
{
namespace
{
    std::atomic<ActorComponent::ComponentId> s_nextComponentId{1};

    ActorComponent::ComponentId GenerateComponentId()
    {
        return s_nextComponentId.fetch_add(1, std::memory_order_relaxed);
    }

    void ReserveComponentId(ActorComponent::ComponentId componentId)
    {
        ActorComponent::ComponentId next = s_nextComponentId.load(std::memory_order_relaxed);
        while (next <= componentId &&
               !s_nextComponentId.compare_exchange_weak(next,
                                                         componentId + 1,
                                                         std::memory_order_relaxed,
                                                         std::memory_order_relaxed))
        {
        }
    }
}

ActorComponent::ActorComponent()
    : m_componentId(GenerateComponentId())
{
}

void ActorComponent::SetComponentIdForSerialization(ComponentId componentId)
{
    if (componentId == InvalidComponentId)
    {
        return;
    }

    m_componentId = componentId;
    ReserveComponentId(componentId);
}

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
