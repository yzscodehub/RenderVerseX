#pragma once

/**
 * @file ParticleEventHandler.h
 * @brief Particle event handler with callbacks
 */

#include "Particle/Events/ParticleEvent.h"
#include <functional>
#include <unordered_map>
#include <vector>

namespace RVX::Particle
{
    /**
     * @brief Callback type for particle events
     */
    using ParticleEventCallback = std::function<void(const ParticleEvent&)>;

    /**
     * @brief Handles particle events and dispatches callbacks
     */
    class ParticleEventHandler
    {
    public:
        ParticleEventHandler() = default;
        ~ParticleEventHandler() = default;

        // Non-copyable
        ParticleEventHandler(const ParticleEventHandler&) = delete;
        ParticleEventHandler& operator=(const ParticleEventHandler&) = delete;

        // =====================================================================
        // Registration
        // =====================================================================

        /// Register a callback for an event type
        void RegisterCallback(ParticleEventType type, ParticleEventCallback callback)
        {
            m_callbacks[type].push_back(std::move(callback));
        }

        /// Unregister all callbacks for an event type
        void UnregisterCallback(ParticleEventType type)
        {
            m_callbacks.erase(type);
        }

        /// Clear all callbacks
        void ClearCallbacks()
        {
            m_callbacks.clear();
        }

        // =====================================================================
        // Convenience Methods
        // =====================================================================

        /// Register OnBirth callback
        void OnBirth(ParticleEventCallback callback)
        {
            RegisterCallback(ParticleEventType::OnBirth, std::move(callback));
        }

        /// Register OnDeath callback
        void OnDeath(ParticleEventCallback callback)
        {
            RegisterCallback(ParticleEventType::OnDeath, std::move(callback));
        }

        /// Register OnCollision callback
        void OnCollision(ParticleEventCallback callback)
        {
            RegisterCallback(ParticleEventType::OnCollision, std::move(callback));
        }

        /// Register OnTriggerEnter callback
        void OnTriggerEnter(ParticleEventCallback callback)
        {
            RegisterCallback(ParticleEventType::OnTriggerEnter, std::move(callback));
        }

        /// Register OnTriggerExit callback
        void OnTriggerExit(ParticleEventCallback callback)
        {
            RegisterCallback(ParticleEventType::OnTriggerExit, std::move(callback));
        }

        // =====================================================================
        // Dispatching
        // =====================================================================

        /// Queue an event for deferred dispatch
        void QueueEvent(const ParticleEvent& event)
        {
            m_eventQueue.push_back(event);
        }

        /// Dispatch all queued events
        void DispatchQueuedEvents()
        {
            for (const auto& event : m_eventQueue)
            {
                DispatchEvent(event);
            }
            m_eventQueue.clear();
        }

        /// Dispatch a single event immediately
        void DispatchEvent(const ParticleEvent& event)
        {
            auto it = m_callbacks.find(event.type);
            if (it != m_callbacks.end())
            {
                for (const auto& callback : it->second)
                {
                    callback(event);
                }
            }
        }

        /// Dispatch multiple events
        void DispatchEvents(const std::vector<ParticleEvent>& events)
        {
            for (const auto& event : events)
            {
                DispatchEvent(event);
            }
        }

        // =====================================================================
        // Query
        // =====================================================================

        /// Check if there are callbacks for an event type
        bool HasCallbacks(ParticleEventType type) const
        {
            auto it = m_callbacks.find(type);
            return it != m_callbacks.end() && !it->second.empty();
        }

        /// Check if there are any callbacks at all
        bool HasAnyCallbacks() const
        {
            return !m_callbacks.empty();
        }

        /// Get number of queued events
        size_t GetQueuedEventCount() const
        {
            return m_eventQueue.size();
        }

    private:
        std::unordered_map<ParticleEventType, std::vector<ParticleEventCallback>> m_callbacks;
        std::vector<ParticleEvent> m_eventQueue;
    };

} // namespace RVX::Particle
