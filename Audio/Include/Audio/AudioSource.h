/**
 * @file AudioSource.h
 * @brief Audio source component
 */

#pragma once

#include "Audio/AudioTypes.h"
#include "Audio/AudioClip.h"
#include <memory>

namespace RVX::Audio
{

/**
 * @brief Audio source for positional audio playback
 */
class AudioSource
{
public:
    AudioSource() = default;
    ~AudioSource() = default;

    // =========================================================================
    // Clip
    // =========================================================================

    void SetClip(AudioClip::Ptr clip) { m_clip = std::move(clip); }
    AudioClip::Ptr GetClip() const { return m_clip; }

    // =========================================================================
    // Playback
    // =========================================================================

    void Play();
    void Pause();
    void Stop();

    bool IsPlaying() const { return m_state == AudioState::Playing; }
    bool IsPaused() const { return m_state == AudioState::Paused; }

    // =========================================================================
    // Settings
    // =========================================================================

    void SetVolume(float volume) { m_settings.volume = volume; }
    float GetVolume() const { return m_settings.volume; }

    void SetPitch(float pitch) { m_settings.pitch = pitch; }
    float GetPitch() const { return m_settings.pitch; }

    void SetLoop(bool loop) { m_settings.loop = loop; }
    bool GetLoop() const { return m_settings.loop; }

    // =========================================================================
    // 3D Settings
    // =========================================================================

    void Set3DSettings(const Audio3DSettings& settings) { m_settings3D = settings; }
    const Audio3DSettings& Get3DSettings() const { return m_settings3D; }

    void SetPosition(const Vec3& position) { m_settings3D.position = position; }
    void SetVelocity(const Vec3& velocity) { m_settings3D.velocity = velocity; }

private:
    AudioClip::Ptr m_clip;
    AudioPlaySettings m_settings;
    Audio3DSettings m_settings3D;
    AudioState m_state = AudioState::Stopped;
    AudioHandle m_handle;
};

} // namespace RVX::Audio
