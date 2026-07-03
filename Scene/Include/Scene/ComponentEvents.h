#pragma once

/**
 * @file ComponentEvents.h
 * @brief Scene component lifecycle events.
 */

#include "Core/Event/Event.h"

namespace RVX
{
    class Component;
    class SceneEntity;

    /**
     * @brief Published after a legacy Scene component is attached to an entity.
     */
    struct ComponentAttachedEvent : Event
    {
        RVX_EVENT_TYPE_CHANNEL(ComponentAttachedEvent, EventChannel::Entity)

        SceneEntity* entity = nullptr;
        Component* component = nullptr;

        ComponentAttachedEvent() = default;
        ComponentAttachedEvent(SceneEntity* owner, Component* attachedComponent)
            : entity(owner)
            , component(attachedComponent)
        {
        }
    };

} // namespace RVX
