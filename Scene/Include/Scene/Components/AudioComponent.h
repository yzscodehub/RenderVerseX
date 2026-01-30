/**
 * @file AudioComponent.h
 * @brief Audio component for scene entities
 */

#pragma once

#include "Scene/Component.h"
#include "Audio/AudioTypes.h"
#include "Audio/AudioClip.h"
#include "Audio/AudioSource.h"
#include <string>

namespace RVX
{

// Forward declarations
namespace Audio
{
    class AudioEngine;
}

/**
 * @brief Audio playback settings for the component
 */
struct AudioComponentSettings
{
    float volume = 1.0f;
    float pitch = 1.0f;
    bool loop = false;
    bool playOnStart = false;
    bool spatialize = true;  // Enable 3D positioning
    
    // 3D settings
    float minDistance = 1.0f;
    float maxDistance = 100.0f;
    float rolloffFactor = 1.0f;
    Audio::AttenuationModel attenuationModel = Audio::AttenuationModel::Inverse;
    
    // Cone settings (for directional sounds)
    float coneInnerAngle = 360.0f;
    float coneOuterAngle = 360.0f;
    float coneOuterGain = 0.0f;
};

/**
 * @brief Audio component for entity-attached audio playback
 * 
 * Adds audio playback capability to a SceneEntity. Supports both
 * 2D and 3D positioned audio, with automatic position updates
 * based on the entity's transform.
 * 
 * Usage:
 * @code
 * // Get or create audio component
 * auto* audio = entity.AddComponent<AudioComponent>();
 * 
 * // Set clip and play
 * audio->SetClip(footstepClip);
 * audio->Play();
 * 
 * // Or use events
 * audio->SetEvent("player/footstep");
 * audio->PostEvent();
 * @endcode
 */
class AudioComponent : public Component
{
public:
    AudioComponent() = default;
    ~AudioComponent() override;

    // =========================================================================
    // Component Interface
    // =========================================================================

    const char* GetTypeName() const override { return "Audio"; }

    void OnAttach() override;
    void OnDetach() override;
    void Tick(float deltaTime) override;

    // =========================================================================
    // Clip-based Playback
    // =========================================================================

    /**
     * @brief Set the audio clip
     */
    void SetClip(Audio::AudioClip::Ptr clip);
    Audio::AudioClip::Ptr GetClip() const { return m_clip; }

    /**
     * @brief Play the current clip
     */
    void Play();

    /**
     * @brief Stop playback
     */
    void Stop();

    /**
     * @brief Pause playback
     */
    void Pause();

    /**
     * @brief Resume paused playback
     */
    void Resume();

    /**
     * @brief Check if playing
     */
    bool IsPlaying() const;

    // =========================================================================
    // Event-based Playback
    // =========================================================================

    /**
     * @brief Set the audio event name
     */
    void SetEvent(const std::string& eventName);
    const std::string& GetEventName() const { return m_eventName; }

    /**
     * @brief Post (trigger) the audio event
     */
    void PostEvent();

    // =========================================================================
    // Settings
    // =========================================================================

    void SetSettings(const AudioComponentSettings& settings);
    const AudioComponentSettings& GetSettings() const { return m_settings; }

    void SetVolume(float volume);
    float GetVolume() const { return m_settings.volume; }

    void SetPitch(float pitch);
    float GetPitch() const { return m_settings.pitch; }

    void SetLoop(bool loop);
    bool IsLooping() const { return m_settings.loop; }

    void SetPlayOnStart(bool playOnStart) { m_settings.playOnStart = playOnStart; }
    bool GetPlayOnStart() const { return m_settings.playOnStart; }

    void SetSpatialize(bool spatialize);
    bool IsSpatializing() const { return m_settings.spatialize; }

    // =========================================================================
    // 3D Settings
    // =========================================================================

    void SetMinDistance(float distance);
    void SetMaxDistance(float distance);
    void SetRolloff(float rolloff);
    void SetAttenuationModel(Audio::AttenuationModel model);

    void SetConeAngles(float innerAngle, float outerAngle, float outerGain);

    // =========================================================================
    // Advanced
    // =========================================================================

    /**
     * @brief Get the internal handle
     */
    Audio::AudioHandle GetHandle() const { return m_handle; }

    /**
     * @brief Get playback position in seconds
     */
    float GetPlaybackPosition() const;

    /**
     * @brief Set playback position in seconds
     */
    void SetPlaybackPosition(float position);

private:
    Audio::AudioClip::Ptr m_clip;
    std::string m_eventName;
    AudioComponentSettings m_settings;
    
    Audio::AudioHandle m_handle;
    Audio::AudioSource m_source;
    
    bool m_needsPositionUpdate = true;

    void UpdatePosition();
    Audio::AudioEngine* GetAudioEngine() const;
};

} // namespace RVX
