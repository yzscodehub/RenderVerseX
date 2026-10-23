/**
 * @file MusicPlayer.cpp
 * @brief MusicPlayer implementation
 */

#include "Audio/Music/MusicPlayer.h"
#include "Core/Log.h"
#include <algorithm>

namespace RVX::Audio
{

MusicPlayer::MusicPlayer(AudioEngine* engine)
    : m_engine(engine)
{
}

void MusicPlayer::Play(MusicTrack::Ptr track, MusicTransition transition, float transitionDuration)
{
    if (!track)
    {
        RVX_CORE_WARN("Cannot play null music track");
        return;
    }

    m_pendingTrack = track;
    m_pendingTransition = transition;
    m_transitionDuration = transitionDuration;

    // For immediate transitions, start now
    if (transition == MusicTransition::Immediate ||
        transition == MusicTransition::Crossfade ||
        transition == MusicTransition::FadeOutIn)
    {
        StartTransition();
    }
    // Beat/bar-synced transitions will be handled in Update()
}

void MusicPlayer::Stop(float fadeOutDuration)
{
    StopCurrentTrack(fadeOutDuration);
    m_pendingTrack = nullptr;
}

void MusicPlayer::Pause()
{
    if (m_currentHandle.IsValid())
    {
        m_engine->Pause(m_currentHandle);
    }
    for (auto& handle : m_layerHandles)
    {
        if (handle.IsValid())
        {
            m_engine->Pause(handle);
        }
    }
}

void MusicPlayer::Resume()
{
    if (m_currentHandle.IsValid())
    {
        m_engine->Resume(m_currentHandle);
    }
    for (auto& handle : m_layerHandles)
    {
        if (handle.IsValid())
        {
            m_engine->Resume(handle);
        }
    }
}

bool MusicPlayer::IsPlaying() const
{
    return m_currentHandle.IsValid() && m_engine->IsPlaying(m_currentHandle);
}

void MusicPlayer::PlayStinger(AudioClip::Ptr stinger, StingerMode mode, float duckAmount)
{
    if (!stinger)
    {
        return;
    }

    // Stop any existing stinger
    if (m_stingerHandle.IsValid())
    {
        m_engine->Stop(m_stingerHandle);
    }

    m_stingerMode = mode;
    m_stingerDuckAmount = duckAmount;

    AudioPlaySettings settings;
    settings.volume = m_volume;

    m_stingerHandle = m_engine->Play(stinger, settings);

    if (mode == StingerMode::Duck && m_currentHandle.IsValid())
    {
        m_isDucking = true;
        m_engine->SetVolume(m_currentHandle, m_volume * duckAmount);
    }
}

void MusicPlayer::SetVolume(float volume)
{
    m_volume = volume;
    
    if (m_currentHandle.IsValid())
    {
        float actualVolume = m_isDucking ? m_volume * m_stingerDuckAmount : m_volume;
        m_engine->SetVolume(m_currentHandle, actualVolume);
    }
}

void MusicPlayer::SetLayerEnabled(const std::string& layerName, bool enabled)
{
    if (!m_currentTrack)
    {
        return;
    }

    m_currentTrack->SetLayerEnabled(layerName, enabled);

    // Find the layer handle and adjust volume
    for (size_t i = 0; i < m_currentTrack->GetLayerCount(); ++i)
    {
        const auto* layer = m_currentTrack->GetLayer(i);
        if (layer && layer->name == layerName && i < m_layerHandles.size())
        {
            float vol = enabled ? layer->volume * m_volume : 0.0f;
            m_engine->SetVolume(m_layerHandles[i], vol);
            break;
        }
    }
}

void MusicPlayer::SetLayerVolume(const std::string& layerName, float volume)
{
    if (!m_currentTrack)
    {
        return;
    }

    m_currentTrack->SetLayerVolume(layerName, volume);

    for (size_t i = 0; i < m_currentTrack->GetLayerCount(); ++i)
    {
        const auto* layer = m_currentTrack->GetLayer(i);
        if (layer && layer->name == layerName && i < m_layerHandles.size())
        {
            if (layer->enabled)
            {
                m_engine->SetVolume(m_layerHandles[i], volume * m_volume);
            }
            break;
        }
    }
}

void MusicPlayer::BlendLayerVolume(const std::string& layerName, float targetVolume, float duration)
{
    if (!m_currentTrack)
    {
        return;
    }

    const auto* layer = m_currentTrack->GetLayer(layerName);
    if (!layer)
    {
        return;
    }

    LayerBlend blend;
    blend.name = layerName;
    blend.startVolume = layer->volume;
    blend.targetVolume = targetVolume;
    blend.duration = duration;
    blend.elapsed = 0.0f;

    // Remove existing blend for this layer
    m_layerBlends.erase(
        std::remove_if(m_layerBlends.begin(), m_layerBlends.end(),
            [&layerName](const LayerBlend& b) { return b.name == layerName; }),
        m_layerBlends.end()
    );

    m_layerBlends.push_back(blend);
}

float MusicPlayer::GetPlaybackPosition() const
{
    if (m_currentHandle.IsValid())
    {
        return m_engine->GetPlaybackPosition(m_currentHandle);
    }
    return 0.0f;
}

void MusicPlayer::SetPlaybackPosition(float time)
{
    if (m_currentHandle.IsValid())
    {
        m_engine->SetPlaybackPosition(m_currentHandle, time);
    }
    for (auto& handle : m_layerHandles)
    {
        if (handle.IsValid())
        {
            m_engine->SetPlaybackPosition(handle, time);
        }
    }
}

void MusicPlayer::SeekToMarker(const std::string& markerName)
{
    if (!m_currentTrack)
    {
        return;
    }

    const auto* marker = m_currentTrack->GetMarker(markerName);
    if (marker)
    {
        SetPlaybackPosition(marker->time);
    }
}

void MusicPlayer::Update(float deltaTime)
{
    // Update beat sync
    float currentPos = GetPlaybackPosition();
    m_beatSync.Update(currentPos);

    // Check for pending beat/bar-synced transitions
    if (m_pendingTrack)
    {
        bool shouldTransition = false;

        switch (m_pendingTransition)
        {
            case MusicTransition::OnNextBeat:
                if (m_beatSync.IsNearBeat(10.0f))  // 10ms window
                {
                    shouldTransition = true;
                }
                break;

            case MusicTransition::OnNextBar:
                if (m_beatSync.GetCurrentBeat() == 0 && m_beatSync.IsNearBeat(10.0f))
                {
                    shouldTransition = true;
                }
                break;

            case MusicTransition::OnMarker:
                if (m_currentTrack)
                {
                    const auto* marker = m_currentTrack->GetMarker(m_transitionMarker);
                    if (marker && currentPos >= marker->time)
                    {
                        shouldTransition = true;
                    }
                }
                break;

            default:
                break;
        }

        if (shouldTransition)
        {
            StartTransition();
        }
    }

    // Update crossfade
    if (m_isCrossfading)
    {
        UpdateCrossfade(deltaTime);
    }

    // Update stinger
    if (m_stingerHandle.IsValid())
    {
        UpdateStinger(deltaTime);
    }

    // Update layer blends
    UpdateLayerBlends(deltaTime);
}

void MusicPlayer::StartTransition()
{
    if (!m_pendingTrack)
    {
        return;
    }

    switch (m_pendingTransition)
    {
        case MusicTransition::Immediate:
            StopCurrentTrack(0.0f);
            StartTrack(m_pendingTrack);
            break;

        case MusicTransition::Crossfade:
            // Start crossfade
            m_outgoingHandle = m_currentHandle;
            m_outgoingLayerHandles = m_layerHandles;
            m_isCrossfading = true;
            m_crossfadeProgress = 0.0f;
            StartTrack(m_pendingTrack);
            m_engine->SetVolume(m_currentHandle, 0.0f);  // Start at 0
            break;

        case MusicTransition::FadeOutIn:
            StopCurrentTrack(m_transitionDuration * 0.5f);
            // TODO: Delay start of new track
            StartTrack(m_pendingTrack);
            break;

        case MusicTransition::OnNextBeat:
        case MusicTransition::OnNextBar:
        case MusicTransition::OnMarker:
            // These are already handled, do crossfade
            m_outgoingHandle = m_currentHandle;
            m_outgoingLayerHandles = m_layerHandles;
            m_isCrossfading = true;
            m_crossfadeProgress = 0.0f;
            m_transitionDuration = 0.2f;  // Quick crossfade for synced transitions
            StartTrack(m_pendingTrack);
            m_engine->SetVolume(m_currentHandle, 0.0f);
            break;
    }

    m_pendingTrack = nullptr;
}

void MusicPlayer::UpdateCrossfade(float deltaTime)
{
    m_crossfadeProgress += deltaTime / m_transitionDuration;

    if (m_crossfadeProgress >= 1.0f)
    {
        m_crossfadeProgress = 1.0f;
        m_isCrossfading = false;

        // Stop outgoing
        if (m_outgoingHandle.IsValid())
        {
            m_engine->Stop(m_outgoingHandle);
        }
        for (auto& handle : m_outgoingLayerHandles)
        {
            if (handle.IsValid())
            {
                m_engine->Stop(handle);
            }
        }
        m_outgoingLayerHandles.clear();

        // Set new track to full volume
        if (m_currentHandle.IsValid())
        {
            m_engine->SetVolume(m_currentHandle, m_volume);
        }
    }
    else
    {
        // Linear crossfade
        float outVolume = (1.0f - m_crossfadeProgress) * m_volume;
        float inVolume = m_crossfadeProgress * m_volume;

        if (m_outgoingHandle.IsValid())
        {
            m_engine->SetVolume(m_outgoingHandle, outVolume);
        }
        if (m_currentHandle.IsValid())
        {
            m_engine->SetVolume(m_currentHandle, inVolume);
        }
    }
}

void MusicPlayer::UpdateStinger(float deltaTime)
{
    (void)deltaTime;

    if (!m_engine->IsPlaying(m_stingerHandle))
    {
        // Stinger finished
        if (m_isDucking && m_currentHandle.IsValid())
        {
            m_engine->SetVolume(m_currentHandle, m_volume);
            m_isDucking = false;
        }
        m_stingerHandle = AudioHandle();
    }
}

void MusicPlayer::UpdateLayerBlends(float deltaTime)
{
    for (auto it = m_layerBlends.begin(); it != m_layerBlends.end();)
    {
        it->elapsed += deltaTime;
        float t = std::min(it->elapsed / it->duration, 1.0f);
        
        float currentVolume = it->startVolume + (it->targetVolume - it->startVolume) * t;
        SetLayerVolume(it->name, currentVolume);

        if (t >= 1.0f)
        {
            it = m_layerBlends.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void MusicPlayer::StopCurrentTrack(float fadeOut)
{
    if (m_currentHandle.IsValid())
    {
        m_engine->Stop(m_currentHandle, fadeOut);
    }
    for (auto& handle : m_layerHandles)
    {
        if (handle.IsValid())
        {
            m_engine->Stop(handle, fadeOut);
        }
    }
    m_layerHandles.clear();
    m_currentTrack = nullptr;
    m_currentHandle = AudioHandle();
}

void MusicPlayer::StartTrack(MusicTrack::Ptr track)
{
    m_currentTrack = track;
    m_beatSync.SetTrack(track);

    auto clip = track->GetClip();
    if (clip)
    {
        AudioPlaySettings settings;
        settings.volume = m_volume;
        settings.loop = true;

        m_currentHandle = m_engine->Play(clip, settings);
    }

    // Start layers
    m_layerHandles.clear();
    for (size_t i = 0; i < track->GetLayerCount(); ++i)
    {
        const auto* layer = track->GetLayer(i);
        if (layer && layer->clip)
        {
            AudioPlaySettings layerSettings;
            layerSettings.volume = layer->enabled ? layer->volume * m_volume : 0.0f;
            layerSettings.loop = true;

            auto handle = m_engine->Play(layer->clip, layerSettings);
            m_layerHandles.push_back(handle);
        }
    }

    RVX_CORE_INFO("Started music track: {}", track->GetName());
}

} // namespace RVX::Audio
