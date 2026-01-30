/**
 * @file AudioSource.cpp
 * @brief AudioSource implementation
 */

#include "Audio/AudioSource.h"
#include "Audio/AudioEngine.h"

namespace RVX::Audio
{

void AudioSource::Play()
{
    if (!m_clip || !m_clip->IsLoaded())
    {
        return;
    }

    // Stop any currently playing sound
    if (m_handle.IsValid())
    {
        Stop();
    }

    auto& engine = GetAudioEngine();

    // Determine if this is 3D or 2D playback
    bool is3D = m_settings3D.minDistance > 0.0f || m_settings3D.maxDistance < 10000.0f;

    if (is3D)
    {
        m_handle = engine.Play3D(m_clip, m_settings3D, m_settings);
    }
    else
    {
        m_handle = engine.Play(m_clip, m_settings);
    }

    if (m_handle.IsValid())
    {
        m_state = AudioState::Playing;
    }
}

void AudioSource::Pause()
{
    if (!m_handle.IsValid())
    {
        return;
    }

    auto& engine = GetAudioEngine();
    engine.Pause(m_handle);
    m_state = AudioState::Paused;
}

void AudioSource::Stop()
{
    if (!m_handle.IsValid())
    {
        return;
    }

    auto& engine = GetAudioEngine();
    engine.Stop(m_handle);
    m_handle = AudioHandle();
    m_state = AudioState::Stopped;
}

} // namespace RVX::Audio
