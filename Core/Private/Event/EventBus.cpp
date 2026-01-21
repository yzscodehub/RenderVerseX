/**
 * @file EventBus.cpp
 * @brief EventBus implementation
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

void EventBus::Unsubscribe(EventHandle handle)
{
    if (!handle.IsValid())
        return;

    std::lock_guard<std::mutex> lock(m_mutex);

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

void EventBus::Clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_subscribers.clear();
}

} // namespace RVX
