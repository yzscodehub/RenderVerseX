/**
 * @file EventBus.cpp
 * @brief EventBus implementation with channel, priority, and filter support
 */

#include "Core/Event/EventBus.h"
#include <algorithm>

namespace RVX
{

EventBus& EventBus::Get()
{
    static EventBus instance;
    return instance;
}

EventHandle EventBus::SubscribeToChannel(EventChannel channel,
                                          std::function<void(const Event&)> callback,
                                          SubscriptionOptions options)
{
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    
    auto handle = EventHandle(++m_nextHandleId);

    Subscriber sub;
    sub.handle = handle;
    sub.priority = options.GetPriorityValue();
    sub.filter = options.filter;
    sub.debugName = options.debugName;
    sub.callback = std::move(callback);

    auto& subscribers = m_channelSubscribers[channel];
    subscribers.push_back(std::move(sub));
    
    // Sort by priority (higher first)
    std::stable_sort(subscribers.begin(), subscribers.end(),
        [](const Subscriber& a, const Subscriber& b) {
            return a.priority > b.priority;
        });

    return handle;
}

void EventBus::Unsubscribe(EventHandle handle)
{
    if (!handle.IsValid())
        return;

    std::unique_lock<std::shared_mutex> lock(m_mutex);

    // Check type-based subscribers
    for (auto& [typeIndex, subscribers] : m_subscribers)
    {
        auto it = std::remove_if(subscribers.begin(), subscribers.end(),
            [handle](const Subscriber& s) { return s.handle == handle; });
        
        if (it != subscribers.end())
        {
            subscribers.erase(it, subscribers.end());
            return;
        }
    }

    // Check channel-based subscribers
    for (auto& [channel, subscribers] : m_channelSubscribers)
    {
        auto it = std::remove_if(subscribers.begin(), subscribers.end(),
            [handle](const Subscriber& s) { return s.handle == handle; });
        
        if (it != subscribers.end())
        {
            subscribers.erase(it, subscribers.end());
            return;
        }
    }
}

void EventBus::NotifyChannelSubscribers(EventChannel channel, const Event& event)
{
    std::vector<Subscriber> subscribersCopy;
    {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        
        // Get subscribers for the specific channel
        auto it = m_channelSubscribers.find(channel);
        if (it != m_channelSubscribers.end())
        {
            subscribersCopy = it->second;
        }

        // Also get subscribers for EventChannel::All
        auto allIt = m_channelSubscribers.find(EventChannel::All);
        if (allIt != m_channelSubscribers.end())
        {
            subscribersCopy.insert(subscribersCopy.end(), 
                                   allIt->second.begin(), 
                                   allIt->second.end());
        }
    }

    if (subscribersCopy.empty())
        return;

    // Sort merged list by priority
    std::stable_sort(subscribersCopy.begin(), subscribersCopy.end(),
        [](const Subscriber& a, const Subscriber& b) {
            return a.priority > b.priority;
        });

    // Dispatch to channel subscribers
    for (auto& subscriber : subscribersCopy)
    {
        if (!subscriber.filter.Accepts(event))
            continue;

        subscriber.callback(event);
        if (event.handled)
            break;
    }
}

void EventBus::ProcessDeferredEvents()
{
    std::queue<std::function<void()>> eventsToProcess;
    
    {
        std::lock_guard<std::mutex> lock(m_deferredMutex);
        std::swap(eventsToProcess, m_deferredEvents);
    }

    while (!eventsToProcess.empty())
    {
        eventsToProcess.front()();
        eventsToProcess.pop();
    }
}

size_t EventBus::GetTotalSubscriberCount() const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    
    size_t count = 0;
    for (const auto& [typeIndex, subscribers] : m_subscribers)
    {
        count += subscribers.size();
    }
    for (const auto& [channel, subscribers] : m_channelSubscribers)
    {
        count += subscribers.size();
    }
    return count;
}

size_t EventBus::GetDeferredEventCount() const
{
    std::lock_guard<std::mutex> lock(m_deferredMutex);
    return m_deferredEvents.size();
}

void EventBus::Clear()
{
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_subscribers.clear();
    m_channelSubscribers.clear();
}

} // namespace RVX
