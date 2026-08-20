#pragma once

/**
 * @file EventBus.h
 * @brief Central event dispatcher for decoupled module communication
 * 
 * Enhanced features:
 * - Event channels for domain isolation
 * - Priority-based handler ordering
 * - Event filtering by source/channel
 * - Scoped subscriptions with RAII
 */

#include "Core/Event/Event.h"
#include "Core/Event/EventHandle.h"
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <typeindex>
#include <unordered_map>
#include <vector>
#include <queue>
#include <atomic>
#include <algorithm>

namespace RVX
{
    /**
     * @brief Subscription options for event handlers
     */
    struct SubscriptionOptions
    {
        /// Handler priority (higher = called first)
        EventPriority priority = EventPriority::Normal;
        
        /// Custom priority value (overrides priority enum if non-zero)
        int32_t customPriority = 0;
        
        /// Event filter (optional)
        EventFilter filter;
        
        /// Debug name for this subscription
        const char* debugName = nullptr;

        /// Get effective priority value
        int32_t GetPriorityValue() const
        {
            return customPriority != 0 ? customPriority : static_cast<int32_t>(priority);
        }

        /// Create options with priority
        static SubscriptionOptions WithPriority(EventPriority p)
        {
            SubscriptionOptions opts;
            opts.priority = p;
            return opts;
        }

        /// Create options with custom priority
        static SubscriptionOptions WithPriority(int32_t p)
        {
            SubscriptionOptions opts;
            opts.customPriority = p;
            return opts;
        }

        /// Create options with filter
        static SubscriptionOptions WithFilter(EventFilter f)
        {
            SubscriptionOptions opts;
            opts.filter = std::move(f);
            return opts;
        }

        /// Create options for a specific channel
        static SubscriptionOptions ForChannel(EventChannel channel)
        {
            SubscriptionOptions opts;
            opts.filter.channelMask = channel;
            return opts;
        }
    };

    /**
     * @brief Central event bus for publish/subscribe pattern
     * 
     * The EventBus enables decoupled communication between modules.
     * Modules can publish events without knowing who will handle them,
     * and subscribe to events without knowing who produces them.
     * 
     * Features:
     * - Channel isolation for event domains
     * - Priority-based handler ordering
     * - Source-based event filtering
     * - Deferred event processing
     * - Thread-safe operations
     * 
     * Thread Safety:
     * - Subscribe/Unsubscribe are thread-safe
     * - Publish is thread-safe
     * - PublishDeferred is thread-safe
     * - ProcessDeferredEvents should be called from main thread
     * 
     * Usage:
     * @code
     * // Basic subscription
     * auto handle = EventBus::Get().Subscribe<WindowResizeEvent>(
     *     [](const WindowResizeEvent& e) {
     *         LOG_INFO("Window resized to {}x{}", e.width, e.height);
     *     });
     * 
     * // Subscribe with priority
     * auto highPriHandle = EventBus::Get().Subscribe<InputEvent>(
     *     [](const InputEvent& e) { },  // Handle first
     *     SubscriptionOptions::WithPriority(EventPriority::High));
     * 
     * // Subscribe with filter
     * auto filteredHandle = EventBus::Get().Subscribe<EntityEvent>(
     *     [](const EntityEvent& e) { },  // Handle
     *     SubscriptionOptions::WithFilter(EventFilter::FromSource(myEntityId)));
     * 
     * // Publish
     * EventBus::Get().Publish(WindowResizeEvent{1920, 1080});
     * 
     * // Publish on specific channel
     * EventBus::Get().PublishToChannel(EventChannel::UI, MyUIEvent{});
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
         * @param options Subscription options (priority, filter, etc.)
         * @return Handle for unsubscription
         */
        template<typename T>
        EventHandle Subscribe(std::function<void(const T&)> callback,
                              SubscriptionOptions options = {})
        {
            static_assert(std::is_base_of_v<Event, T>, "T must derive from Event");

            std::unique_lock<std::shared_mutex> lock(m_mutex);
            
            auto handle = EventHandle(++m_nextHandleId);
            auto typeIndex = std::type_index(typeid(T));

            Subscriber sub;
            sub.handle = handle;
            sub.priority = options.GetPriorityValue();
            sub.filter = options.filter;
            sub.debugName = options.debugName;
            sub.callback = [callback](const Event& e) {
                callback(static_cast<const T&>(e));
            };

            auto& subscribers = m_subscribers[typeIndex];
            subscribers.push_back(std::move(sub));
            
            // Sort by priority (higher first)
            std::stable_sort(subscribers.begin(), subscribers.end(),
                [](const Subscriber& a, const Subscriber& b) {
                    return a.priority > b.priority;
                });

            return handle;
        }

        /**
         * @brief Subscribe with automatic RAII unsubscription
         */
        template<typename T>
        ScopedEventHandle SubscribeScoped(std::function<void(const T&)> callback,
                                           SubscriptionOptions options = {})
        {
            auto handle = Subscribe<T>(std::move(callback), std::move(options));
            return ScopedEventHandle(handle, [this](EventHandle h) { Unsubscribe(h); });
        }

        /**
         * @brief Subscribe to all events on a channel
         * @param channel Channel to subscribe to
         * @param callback Callback receiving any event on the channel
         * @param options Subscription options
         * @return Handle for unsubscription
         */
        EventHandle SubscribeToChannel(EventChannel channel,
                                        std::function<void(const Event&)> callback,
                                        SubscriptionOptions options = {});

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
         * Handlers are called in priority order (higher first).
         * If a handler sets event.handled = true, propagation stops.
         * 
         * @param event Event to publish
         */
        template<typename T>
        void Publish(const T& event)
        {
            static_assert(std::is_base_of_v<Event, T>, "T must derive from Event");

            std::vector<Subscriber> subscribersCopy;
            {
                std::shared_lock<std::shared_mutex> lock(m_mutex);
                auto typeIndex = std::type_index(typeid(T));
                auto it = m_subscribers.find(typeIndex);
                if (it != m_subscribers.end())
                {
                    subscribersCopy = it->second;
                }
            }

            // Also notify channel subscribers
            NotifyChannelSubscribers(event.GetChannel(), event);

            // Dispatch to type-specific subscribers
            for (auto& subscriber : subscribersCopy)
            {
                // Apply filter
                if (!subscriber.filter.Accepts(event))
                    continue;

                subscriber.callback(event);
                if (event.handled)
                    break;
            }
        }

        /**
         * @brief Publish an event with a specific source
         */
        template<typename T>
        void Publish(T event, EventSource source)
        {
            event.source = source;
            Publish(event);
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
         * @brief Queue an event with source for deferred processing
         */
        template<typename T>
        void PublishDeferred(T event, EventSource source)
        {
            event.source = source;
            PublishDeferred(std::move(event));
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
            std::shared_lock<std::shared_mutex> lock(m_mutex);
            auto typeIndex = std::type_index(typeid(T));
            auto it = m_subscribers.find(typeIndex);
            return it != m_subscribers.end() ? it->second.size() : 0;
        }

        /**
         * @brief Get total number of subscribers across all event types
         */
        size_t GetTotalSubscriberCount() const;

        /**
         * @brief Get number of pending deferred events
         */
        size_t GetDeferredEventCount() const;

        /**
         * @brief Clear all subscribers (use with caution)
         */
        void Clear();

        /**
         * @brief Clear subscribers for a specific event type
         */
        template<typename T>
        void ClearSubscribers()
        {
            std::unique_lock<std::shared_mutex> lock(m_mutex);
            auto typeIndex = std::type_index(typeid(T));
            m_subscribers.erase(typeIndex);
        }

    private:
        EventBus() = default;
        ~EventBus() = default;

        // Non-copyable
        EventBus(const EventBus&) = delete;
        EventBus& operator=(const EventBus&) = delete;

        struct Subscriber
        {
            EventHandle handle;
            int32_t priority = 0;
            EventFilter filter;
            const char* debugName = nullptr;
            std::function<void(const Event&)> callback;
        };

        void NotifyChannelSubscribers(EventChannel channel, const Event& event);

        mutable std::shared_mutex m_mutex;
        std::unordered_map<std::type_index, std::vector<Subscriber>> m_subscribers;
        std::unordered_map<EventChannel, std::vector<Subscriber>> m_channelSubscribers;
        std::atomic<EventHandle::HandleId> m_nextHandleId{0};

        mutable std::mutex m_deferredMutex;
        std::queue<std::function<void()>> m_deferredEvents;
    };

} // namespace RVX
