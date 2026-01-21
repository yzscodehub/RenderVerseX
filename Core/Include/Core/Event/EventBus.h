#pragma once

/**
 * @file EventBus.h
 * @brief Central event dispatcher for decoupled module communication
 */

#include "Core/Event/Event.h"
#include "Core/Event/EventHandle.h"
#include <functional>
#include <memory>
#include <mutex>
#include <typeindex>
#include <unordered_map>
#include <vector>
#include <queue>
#include <atomic>

namespace RVX
{
    /**
     * @brief Central event bus for publish/subscribe pattern
     * 
     * The EventBus enables decoupled communication between modules.
     * Modules can publish events without knowing who will handle them,
     * and subscribe to events without knowing who produces them.
     * 
     * Thread Safety:
     * - Subscribe/Unsubscribe are thread-safe
     * - Publish is thread-safe
     * - PublishDeferred is thread-safe
     * - ProcessDeferredEvents should be called from main thread
     * 
     * Usage:
     * @code
     * // Subscribe
     * auto handle = EventBus::Get().Subscribe<WindowResizeEvent>(
     *     [](const WindowResizeEvent& e) {
     *         LOG_INFO("Window resized to {}x{}", e.width, e.height);
     *     });
     * 
     * // Publish
     * EventBus::Get().Publish(WindowResizeEvent{1920, 1080});
     * 
     * // Unsubscribe (optional, handle destructor does this)
     * EventBus::Get().Unsubscribe(handle);
     * @endcode
     */
    class EventBus
    {
    public:
        /// Get the global EventBus instance
        static EventBus& Get();

        // =====================================================================
        // Subscription
        // =====================================================================

        /**
         * @brief Subscribe to an event type
         * @tparam T Event type (must derive from Event)
         * @param callback Function to call when event is published
         * @return Handle for unsubscription
         */
        template<typename T>
        EventHandle Subscribe(std::function<void(const T&)> callback)
        {
            static_assert(std::is_base_of_v<Event, T>, "T must derive from Event");

            std::lock_guard<std::mutex> lock(m_mutex);
            
            auto handle = EventHandle(++m_nextHandleId);
            auto typeIndex = std::type_index(typeid(T));

            auto wrapper = [callback](const Event& e) {
                callback(static_cast<const T&>(e));
            };

            m_subscribers[typeIndex].push_back({handle, std::move(wrapper)});
            return handle;
        }

        /**
         * @brief Subscribe with automatic RAII unsubscription
         */
        template<typename T>
        ScopedEventHandle SubscribeScoped(std::function<void(const T&)> callback)
        {
            auto handle = Subscribe<T>(std::move(callback));
            return ScopedEventHandle(handle, [this](EventHandle h) { Unsubscribe(h); });
        }

        /**
         * @brief Unsubscribe from an event
         * @param handle Handle returned from Subscribe
         */
        void Unsubscribe(EventHandle handle);

        // =====================================================================
        // Publishing
        // =====================================================================

        /**
         * @brief Publish an event immediately
         * 
         * All subscribers are called synchronously in the current thread.
         * @param event Event to publish
         */
        template<typename T>
        void Publish(const T& event)
        {
            static_assert(std::is_base_of_v<Event, T>, "T must derive from Event");

            std::vector<Subscriber> subscribersCopy;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                auto typeIndex = std::type_index(typeid(T));
                auto it = m_subscribers.find(typeIndex);
                if (it != m_subscribers.end())
                {
                    subscribersCopy = it->second;
                }
            }

            for (auto& subscriber : subscribersCopy)
            {
                subscriber.callback(event);
                if (event.handled)
                    break;
            }
        }

        /**
         * @brief Queue an event for deferred processing
         * 
         * The event will be published during the next ProcessDeferredEvents call.
         * Useful for cross-thread event posting.
         */
        template<typename T>
        void PublishDeferred(T event)
        {
            static_assert(std::is_base_of_v<Event, T>, "T must derive from Event");

            std::lock_guard<std::mutex> lock(m_deferredMutex);
            m_deferredEvents.push([this, e = std::move(event)]() mutable {
                Publish(e);
            });
        }

        /**
         * @brief Process all deferred events
         * 
         * Should be called once per frame from the main thread.
         */
        void ProcessDeferredEvents();

        // =====================================================================
        // Utility
        // =====================================================================

        /**
         * @brief Get the number of subscribers for an event type
         */
        template<typename T>
        size_t GetSubscriberCount() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto typeIndex = std::type_index(typeid(T));
            auto it = m_subscribers.find(typeIndex);
            return it != m_subscribers.end() ? it->second.size() : 0;
        }

        /**
         * @brief Clear all subscribers (use with caution)
         */
        void Clear();

    private:
        EventBus() = default;
        ~EventBus() = default;

        // Non-copyable
        EventBus(const EventBus&) = delete;
        EventBus& operator=(const EventBus&) = delete;

        struct Subscriber
        {
            EventHandle handle;
            std::function<void(const Event&)> callback;
        };

        mutable std::mutex m_mutex;
        std::unordered_map<std::type_index, std::vector<Subscriber>> m_subscribers;
        std::atomic<EventHandle::HandleId> m_nextHandleId{0};

        std::mutex m_deferredMutex;
        std::queue<std::function<void()>> m_deferredEvents;
    };

} // namespace RVX
