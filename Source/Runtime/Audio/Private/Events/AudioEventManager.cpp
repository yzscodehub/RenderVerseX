/**
 * @file AudioEventManager.cpp
 * @brief AudioEventManager implementation
 */

#include "Audio/Events/AudioEventManager.h"
#include "Core/Log.h"

namespace RVX::Audio
{

AudioEventManager::AudioEventManager(AudioEngine* engine)
    : m_engine(engine)
{
}

void AudioEventManager::RegisterEvent(const AudioEventDesc& desc)
{
    if (desc.name.empty())
    {
        RVX_CORE_WARN("Cannot register audio event with empty name");
        return;
    }

    if (m_events.find(desc.name) != m_events.end())
    {
        RVX_CORE_WARN("Audio event '{}' already registered, replacing", desc.name);
    }

    m_events[desc.name] = std::make_shared<AudioEvent>(desc);
    RVX_CORE_DEBUG("Registered audio event: {}", desc.name);
}

void AudioEventManager::UnregisterEvent(const std::string& name)
{
    auto it = m_events.find(name);
    if (it != m_events.end())
    {
        m_events.erase(it);
        RVX_CORE_DEBUG("Unregistered audio event: {}", name);
    }
}

bool AudioEventManager::HasEvent(const std::string& name) const
{
    return m_events.find(name) != m_events.end();
}

AudioEvent* AudioEventManager::GetEvent(const std::string& name)
{
    auto it = m_events.find(name);
    return (it != m_events.end()) ? it->second.get() : nullptr;
}

const AudioEvent* AudioEventManager::GetEvent(const std::string& name) const
{
    auto it = m_events.find(name);
    return (it != m_events.end()) ? it->second.get() : nullptr;
}

void AudioEventManager::ClearAllEvents()
{
    StopAllEvents();
    m_events.clear();
    m_handleToEventName.clear();
}

AudioHandle AudioEventManager::PostEvent(const std::string& name)
{
    auto* event = GetEvent(name);
    if (!event)
    {
        RVX_CORE_WARN("Audio event '{}' not found", name);
        return AudioHandle();
    }

    if (!event->CanPlay())
    {
        return AudioHandle();
    }

    auto clip = event->GetNextClip();
    if (!clip)
    {
        RVX_CORE_WARN("Audio event '{}' has no valid clips", name);
        return AudioHandle();
    }

    auto settings = event->GenerateSettings();
    AudioHandle handle = m_engine->Play(clip, settings);

    if (handle.IsValid())
    {
        event->RecordPlay(handle);
        m_handleToEventName[handle.GetId()] = name;

        if (m_onEventPlayed)
        {
            m_onEventPlayed(name, handle);
        }
    }

    return handle;
}

AudioHandle AudioEventManager::PostEvent3D(const std::string& name, const Vec3& position)
{
    Audio3DSettings spatial;
    spatial.position = position;
    return PostEvent3D(name, spatial);
}

AudioHandle AudioEventManager::PostEvent3D(const std::string& name, const Audio3DSettings& spatial)
{
    auto* event = GetEvent(name);
    if (!event)
    {
        RVX_CORE_WARN("Audio event '{}' not found", name);
        return AudioHandle();
    }

    if (!event->CanPlay())
    {
        return AudioHandle();
    }

    auto clip = event->GetNextClip();
    if (!clip)
    {
        RVX_CORE_WARN("Audio event '{}' has no valid clips", name);
        return AudioHandle();
    }

    // Merge event's spatial settings with provided position
    Audio3DSettings mergedSpatial = event->GetDesc().spatial;
    mergedSpatial.position = spatial.position;
    mergedSpatial.velocity = spatial.velocity;

    auto settings = event->GenerateSettings();
    AudioHandle handle = m_engine->Play3D(clip, mergedSpatial, settings);

    if (handle.IsValid())
    {
        event->RecordPlay(handle);
        m_handleToEventName[handle.GetId()] = name;

        if (m_onEventPlayed)
        {
            m_onEventPlayed(name, handle);
        }
    }

    return handle;
}

void AudioEventManager::StopEvent(const std::string& name, float fadeOutTime)
{
    auto* event = GetEvent(name);
    if (!event)
    {
        return;
    }

    // Find all handles for this event and stop them
    std::vector<uint64> toRemove;
    for (const auto& pair : m_handleToEventName)
    {
        if (pair.second == name)
        {
            m_engine->Stop(AudioHandle(pair.first), fadeOutTime);
            toRemove.push_back(pair.first);
        }
    }

    for (uint64 id : toRemove)
    {
        m_handleToEventName.erase(id);
        event->RemoveInstance(AudioHandle(id));

        if (m_onEventStopped)
        {
            m_onEventStopped(name, AudioHandle(id));
        }
    }
}

void AudioEventManager::StopAllEvents(float fadeOutTime)
{
    for (auto& pair : m_events)
    {
        StopEvent(pair.first, fadeOutTime);
    }
}

void AudioEventManager::SetOnEventPlayed(AudioEventCallback callback)
{
    m_onEventPlayed = std::move(callback);
}

void AudioEventManager::SetOnEventStopped(AudioEventCallback callback)
{
    m_onEventStopped = std::move(callback);
}

void AudioEventManager::Update(float deltaTime)
{
    // Update all events (cooldowns, etc.)
    for (auto& pair : m_events)
    {
        pair.second->Update(deltaTime);
    }

    // Check for finished instances
    std::vector<uint64> finished;
    for (const auto& pair : m_handleToEventName)
    {
        if (!m_engine->IsPlaying(AudioHandle(pair.first)))
        {
            finished.push_back(pair.first);
        }
    }

    // Clean up finished instances
    for (uint64 id : finished)
    {
        auto it = m_handleToEventName.find(id);
        if (it != m_handleToEventName.end())
        {
            const std::string& eventName = it->second;
            
            if (auto* event = GetEvent(eventName))
            {
                event->RemoveInstance(AudioHandle(id));
            }

            if (m_onEventStopped)
            {
                m_onEventStopped(eventName, AudioHandle(id));
            }

            m_handleToEventName.erase(it);
        }
    }
}

int AudioEventManager::GetTotalActiveInstances() const
{
    return static_cast<int>(m_handleToEventName.size());
}

} // namespace RVX::Audio
