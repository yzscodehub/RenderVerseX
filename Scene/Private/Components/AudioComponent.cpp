/**
 * @file AudioComponent.cpp
 * @brief AudioComponent implementation
 */

#include "Scene/Components/AudioComponent.h"
#include "Scene/SceneEntity.h"
#include "Audio/AudioEngine.h"
#include "Core/Log.h"

namespace RVX
{

AudioComponent::~AudioComponent()
{
    Stop();
}

void AudioComponent::OnAttach()
{
    if (m_settings.playOnStart && m_clip)
    {
        Play();
    }
}

void AudioComponent::OnDetach()
{
    Stop();
}

void AudioComponent::Tick(float deltaTime)
{
    (void)deltaTime;

    if (!IsEnabled())
    {
        return;
    }

    // Update 3D position if playing and spatialized
    if (m_handle.IsValid() && m_settings.spatialize)
    {
        UpdatePosition();
    }
}

void AudioComponent::SetClip(Audio::AudioClip::Ptr clip)
{
    if (IsPlaying())
    {
        Stop();
    }
    m_clip = std::move(clip);
    m_source.SetClip(m_clip);
}

void AudioComponent::Play()
{
    auto* engine = GetAudioEngine();
    if (!engine || !m_clip)
    {
        return;
    }

    // Stop any existing playback
    Stop();

    Audio::AudioPlaySettings playSettings;
    playSettings.volume = m_settings.volume;
    playSettings.pitch = m_settings.pitch;
    playSettings.loop = m_settings.loop;

    if (m_settings.spatialize && GetOwner())
    {
        // 3D playback
        Audio::Audio3DSettings spatial;
        
        // Get position from owner entity
        // Note: This assumes SceneEntity has GetWorldPosition()
        // Adjust based on actual SceneEntity API
        spatial.position = Vec3(0.0f);  // Will be updated in UpdatePosition()
        spatial.minDistance = m_settings.minDistance;
        spatial.maxDistance = m_settings.maxDistance;
        spatial.rolloffFactor = m_settings.rolloffFactor;
        spatial.attenuationModel = m_settings.attenuationModel;
        spatial.coneInnerAngle = m_settings.coneInnerAngle;
        spatial.coneOuterAngle = m_settings.coneOuterAngle;
        spatial.coneOuterGain = m_settings.coneOuterGain;

        m_handle = engine->Play3D(m_clip, spatial, playSettings);
        
        // Update position immediately
        UpdatePosition();
    }
    else
    {
        // 2D playback
        m_handle = engine->Play(m_clip, playSettings);
    }
}

void AudioComponent::Stop()
{
    if (m_handle.IsValid())
    {
        if (auto* engine = GetAudioEngine())
        {
            engine->Stop(m_handle);
        }
        m_handle = Audio::AudioHandle();
    }
}

void AudioComponent::Pause()
{
    if (m_handle.IsValid())
    {
        if (auto* engine = GetAudioEngine())
        {
            engine->Pause(m_handle);
        }
    }
}

void AudioComponent::Resume()
{
    if (m_handle.IsValid())
    {
        if (auto* engine = GetAudioEngine())
        {
            engine->Resume(m_handle);
        }
    }
}

bool AudioComponent::IsPlaying() const
{
    if (!m_handle.IsValid())
    {
        return false;
    }

    if (auto* engine = GetAudioEngine())
    {
        return engine->IsPlaying(m_handle);
    }
    return false;
}

void AudioComponent::SetEvent(const std::string& eventName)
{
    m_eventName = eventName;
}

void AudioComponent::PostEvent()
{
    // Note: This requires integration with AudioEventManager
    // For now, just log that the event was posted
    RVX_CORE_DEBUG("AudioComponent::PostEvent - {}", m_eventName);
    
    // TODO: Integrate with AudioEventManager
    // auto* eventManager = GetAudioEventManager();
    // if (eventManager && !m_eventName.empty())
    // {
    //     if (m_settings.spatialize && GetOwner())
    //     {
    //         m_handle = eventManager->PostEvent3D(m_eventName, GetOwner()->GetWorldPosition());
    //     }
    //     else
    //     {
    //         m_handle = eventManager->PostEvent(m_eventName);
    //     }
    // }
}

void AudioComponent::SetSettings(const AudioComponentSettings& settings)
{
    m_settings = settings;

    // Apply settings to currently playing sound
    if (m_handle.IsValid())
    {
        if (auto* engine = GetAudioEngine())
        {
            engine->SetVolume(m_handle, m_settings.volume);
            engine->SetPitch(m_handle, m_settings.pitch);
            engine->SetLooping(m_handle, m_settings.loop);
        }
    }
}

void AudioComponent::SetVolume(float volume)
{
    m_settings.volume = volume;
    if (m_handle.IsValid())
    {
        if (auto* engine = GetAudioEngine())
        {
            engine->SetVolume(m_handle, volume);
        }
    }
}

void AudioComponent::SetPitch(float pitch)
{
    m_settings.pitch = pitch;
    if (m_handle.IsValid())
    {
        if (auto* engine = GetAudioEngine())
        {
            engine->SetPitch(m_handle, pitch);
        }
    }
}

void AudioComponent::SetLoop(bool loop)
{
    m_settings.loop = loop;
    if (m_handle.IsValid())
    {
        if (auto* engine = GetAudioEngine())
        {
            engine->SetLooping(m_handle, loop);
        }
    }
}

void AudioComponent::SetSpatialize(bool spatialize)
{
    m_settings.spatialize = spatialize;
}

void AudioComponent::SetMinDistance(float distance)
{
    m_settings.minDistance = distance;
}

void AudioComponent::SetMaxDistance(float distance)
{
    m_settings.maxDistance = distance;
}

void AudioComponent::SetRolloff(float rolloff)
{
    m_settings.rolloffFactor = rolloff;
}

void AudioComponent::SetAttenuationModel(Audio::AttenuationModel model)
{
    m_settings.attenuationModel = model;
}

void AudioComponent::SetConeAngles(float innerAngle, float outerAngle, float outerGain)
{
    m_settings.coneInnerAngle = innerAngle;
    m_settings.coneOuterAngle = outerAngle;
    m_settings.coneOuterGain = outerGain;
}

float AudioComponent::GetPlaybackPosition() const
{
    if (m_handle.IsValid())
    {
        if (auto* engine = GetAudioEngine())
        {
            return engine->GetPlaybackPosition(m_handle);
        }
    }
    return 0.0f;
}

void AudioComponent::SetPlaybackPosition(float position)
{
    if (m_handle.IsValid())
    {
        if (auto* engine = GetAudioEngine())
        {
            engine->SetPlaybackPosition(m_handle, position);
        }
    }
}

void AudioComponent::UpdatePosition()
{
    if (!m_handle.IsValid() || !m_settings.spatialize)
    {
        return;
    }

    auto* owner = GetOwner();
    if (!owner)
    {
        return;
    }

    auto* engine = GetAudioEngine();
    if (!engine)
    {
        return;
    }

    // Get world position from the entity
    // Note: Adjust this based on actual SceneEntity API
    // For now, we assume there's a way to get the world position
    Vec3 position = Vec3(0.0f);  // owner->GetWorldPosition();
    
    // TODO: Get actual position from entity transform
    // This will need to be connected to SceneEntity's transform system
    
    engine->SetPosition(m_handle, position);
}

Audio::AudioEngine* AudioComponent::GetAudioEngine() const
{
    // Get the global audio engine
    return &Audio::GetAudioEngine();
}

} // namespace RVX
