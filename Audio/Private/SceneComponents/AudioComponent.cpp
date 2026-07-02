/**
 * @file AudioComponent.cpp
 * @brief AudioComponent implementation
 */

#include "Scene/Components/AudioComponent.h"

#include "Audio/AudioClip.h"
#include "Audio/AudioEngine.h"
#include "Audio/AudioSource.h"
#include "Audio/AudioSubsystem.h"
#include "Audio/AudioTypes.h"
#include "Core/Log.h"
#include "Scene/SceneEntity.h"

namespace RVX
{

AudioComponent::AudioComponent()
    : m_source(std::make_unique<Audio::AudioSource>())
    , m_lastOcclusion(std::make_unique<Audio::OcclusionResult>())
{
}

AudioComponent::~AudioComponent()
{
    Stop();
}

Audio::AudioHandle AudioComponent::GetHandle() const
{
    return Audio::AudioHandle(m_handleId);
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

    if (GetHandle().IsValid() && m_settings.spatialize)
    {
        UpdatePosition();
    }
}

void AudioComponent::SetClip(std::shared_ptr<Audio::AudioClip> clip)
{
    if (IsPlaying())
    {
        Stop();
    }

    m_clip = std::move(clip);
    if (m_source)
    {
        m_source->SetClip(m_clip);
    }
}

void AudioComponent::Play()
{
    auto* engine = GetAudioEngine();
    if (!engine || !m_clip)
    {
        return;
    }

    Stop();

    Audio::AudioPlaySettings playSettings;
    playSettings.volume = m_settings.volume;
    playSettings.pitch = m_settings.pitch;
    playSettings.loop = m_settings.loop;
    playSettings.busId = m_settings.busId;

    if (m_settings.spatialize && GetOwner())
    {
        Audio::Audio3DSettings spatial;
        spatial.position = GetAudioWorldPosition();
        spatial.minDistance = m_settings.minDistance;
        spatial.maxDistance = m_settings.maxDistance;
        spatial.rolloffFactor = m_settings.rolloffFactor;
        spatial.attenuationModel = m_settings.attenuationModel;
        spatial.coneInnerAngle = m_settings.coneInnerAngle;
        spatial.coneOuterAngle = m_settings.coneOuterAngle;
        spatial.coneOuterGain = m_settings.coneOuterGain;

        m_handleId = engine->Play3D(m_clip, spatial, playSettings).GetId();
        UpdatePosition();
    }
    else
    {
        m_handleId = engine->Play(m_clip, playSettings).GetId();
    }
}

void AudioComponent::Stop()
{
    const Audio::AudioHandle handle = GetHandle();
    if (handle.IsValid())
    {
        if (auto* engine = GetAudioEngine())
        {
            engine->Stop(handle);
        }
        m_handleId = 0;
    }
}

void AudioComponent::Pause()
{
    const Audio::AudioHandle handle = GetHandle();
    if (handle.IsValid())
    {
        if (auto* engine = GetAudioEngine())
        {
            engine->Pause(handle);
        }
    }
}

void AudioComponent::Resume()
{
    const Audio::AudioHandle handle = GetHandle();
    if (handle.IsValid())
    {
        if (auto* engine = GetAudioEngine())
        {
            engine->Resume(handle);
        }
    }
}

bool AudioComponent::IsPlaying() const
{
    const Audio::AudioHandle handle = GetHandle();
    if (!handle.IsValid())
    {
        return false;
    }

    if (auto* engine = GetAudioEngine())
    {
        return engine->IsPlaying(handle);
    }
    return false;
}

void AudioComponent::SetEvent(const std::string& eventName)
{
    m_eventName = eventName;
}

void AudioComponent::PostEvent()
{
    RVX_CORE_DEBUG("AudioComponent::PostEvent - {}", m_eventName);
}

void AudioComponent::SetSettings(const AudioComponentSettings& settings)
{
    m_settings = settings;

    const Audio::AudioHandle handle = GetHandle();
    if (!handle.IsValid())
    {
        return;
    }

    if (auto* engine = GetAudioEngine())
    {
        engine->SetPitch(handle, m_settings.pitch);
        engine->SetLooping(handle, m_settings.loop);
        if (m_settings.spatialize)
        {
            ApplySpatialOcclusion();
        }
        else
        {
            *m_lastOcclusion = Audio::OcclusionResult{};
            engine->SetVolume(handle, m_settings.volume);
            engine->SetLowPassCutoff(handle, m_lastOcclusion->lowPassCutoff);
        }
    }
}

void AudioComponent::SetVolume(float volume)
{
    m_settings.volume = volume;
    const Audio::AudioHandle handle = GetHandle();
    if (handle.IsValid())
    {
        if (m_settings.spatialize)
        {
            ApplySpatialOcclusion();
        }
        else if (auto* engine = GetAudioEngine())
        {
            engine->SetVolume(handle, volume);
        }
    }
}

void AudioComponent::SetPitch(float pitch)
{
    m_settings.pitch = pitch;
    const Audio::AudioHandle handle = GetHandle();
    if (handle.IsValid())
    {
        if (auto* engine = GetAudioEngine())
        {
            engine->SetPitch(handle, pitch);
        }
    }
}

void AudioComponent::SetLoop(bool loop)
{
    m_settings.loop = loop;
    const Audio::AudioHandle handle = GetHandle();
    if (handle.IsValid())
    {
        if (auto* engine = GetAudioEngine())
        {
            engine->SetLooping(handle, loop);
        }
    }
}

void AudioComponent::SetSpatialize(bool spatialize)
{
    m_settings.spatialize = spatialize;
    if (!m_settings.spatialize)
    {
        *m_lastOcclusion = Audio::OcclusionResult{};
        const Audio::AudioHandle handle = GetHandle();
        if (handle.IsValid())
        {
            if (auto* engine = GetAudioEngine())
            {
                engine->SetVolume(handle, m_settings.volume);
                engine->SetLowPassCutoff(handle, m_lastOcclusion->lowPassCutoff);
            }
        }
    }
}

void AudioComponent::SetBusId(uint32 busId)
{
    m_settings.busId = busId;
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

Vec3 AudioComponent::GetAudioWorldPosition() const
{
    const SceneEntity* owner = GetOwner();
    return owner ? owner->GetWorldPosition() : Vec3(0.0f);
}

const Audio::OcclusionResult& AudioComponent::GetLastOcclusionResult() const
{
    return *m_lastOcclusion;
}

float AudioComponent::GetPlaybackPosition() const
{
    const Audio::AudioHandle handle = GetHandle();
    if (handle.IsValid())
    {
        if (auto* engine = GetAudioEngine())
        {
            return engine->GetPlaybackPosition(handle);
        }
    }
    return 0.0f;
}

void AudioComponent::SetAudioEngine(Audio::AudioEngine* engine)
{
    if (m_audioEngine == engine && !m_audioSubsystem)
    {
        return;
    }

    Stop();
    m_audioSubsystem = nullptr;
    *m_lastOcclusion = Audio::OcclusionResult{};
    m_audioEngine = engine;
}

void AudioComponent::SetAudioSubsystem(Audio::AudioSubsystem* subsystem)
{
    if (m_audioSubsystem == subsystem)
    {
        return;
    }

    Stop();
    m_audioSubsystem = subsystem;
    m_audioEngine = subsystem ? &subsystem->GetEngine() : nullptr;
    *m_lastOcclusion = Audio::OcclusionResult{};
}

void AudioComponent::SetPlaybackPosition(float position)
{
    const Audio::AudioHandle handle = GetHandle();
    if (handle.IsValid())
    {
        if (auto* engine = GetAudioEngine())
        {
            engine->SetPlaybackPosition(handle, position);
        }
    }
}

void AudioComponent::UpdatePosition()
{
    const Audio::AudioHandle handle = GetHandle();
    if (!handle.IsValid() || !m_settings.spatialize)
    {
        return;
    }

    if (!GetOwner())
    {
        return;
    }

    auto* engine = GetAudioEngine();
    if (!engine)
    {
        return;
    }

    engine->SetPosition(handle, GetAudioWorldPosition());
    ApplySpatialOcclusion();
}

void AudioComponent::ApplySpatialOcclusion()
{
    const Audio::AudioHandle handle = GetHandle();
    if (!handle.IsValid() || !m_settings.spatialize)
    {
        return;
    }

    auto* engine = GetAudioEngine();
    if (!engine)
    {
        return;
    }

    if (!m_audioSubsystem)
    {
        *m_lastOcclusion = Audio::OcclusionResult{};
        engine->SetVolume(handle, m_settings.volume);
        engine->SetLowPassCutoff(handle, m_lastOcclusion->lowPassCutoff);
        return;
    }

    *m_lastOcclusion = m_audioSubsystem->GetOcclusion(GetAudioWorldPosition());
    engine->SetVolume(handle, m_settings.volume * m_lastOcclusion->volumeScale);
    engine->SetLowPassCutoff(handle, m_lastOcclusion->lowPassCutoff);
}

Audio::AudioEngine* AudioComponent::GetAudioEngine() const
{
    return m_audioEngine ? m_audioEngine : &Audio::GetAudioEngine();
}

} // namespace RVX
