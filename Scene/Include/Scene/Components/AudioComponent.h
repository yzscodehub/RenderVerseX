/**
 * @file AudioComponent.h
 * @brief Audio component for scene entities
 */

#pragma once

#include "Core/Types.h"
#include "Scene/Component.h"

#include <memory>
#include <string>

namespace RVX::Audio
{
    class AudioClip;
    class AudioEngine;
    class AudioHandle;
    class AudioSource;
    class AudioSubsystem;
    enum class AttenuationModel : uint8;
    struct OcclusionResult;
} // namespace RVX::Audio

namespace RVX
{

/**
 * @brief Audio playback settings for the component
 */
struct AudioComponentSettings
{
    float volume = 1.0f;
    float pitch = 1.0f;
    bool loop = false;
    bool playOnStart = false;
    bool spatialize = true;

    float minDistance = 1.0f;
    float maxDistance = 100.0f;
    float rolloffFactor = 1.0f;
    Audio::AttenuationModel attenuationModel = static_cast<Audio::AttenuationModel>(2);

    float coneInnerAngle = 360.0f;
    float coneOuterAngle = 360.0f;
    float coneOuterGain = 0.0f;

    uint32 busId = 0;
};

/**
 * @brief Audio component for entity-attached audio playback
 */
class AudioComponent : public Component
{
public:
    AudioComponent();
    ~AudioComponent() override;

    // =========================================================================
    // Component Interface
    // =========================================================================

    const char* GetTypeName() const override { return "Audio"; }

    void OnAttach() override;
    void OnDetach() override;
    void Tick(float deltaTime) override;
    [[nodiscard]] SceneUpdatePhase GetSceneUpdatePhase() const override
    {
        return SceneUpdatePhase::FeatureSystems;
    }

    // =========================================================================
    // Clip-based Playback
    // =========================================================================

    void SetClip(std::shared_ptr<Audio::AudioClip> clip);
    std::shared_ptr<Audio::AudioClip> GetClip() const { return m_clip; }

    void Play();
    void Stop();
    void Pause();
    void Resume();
    bool IsPlaying() const;

    // =========================================================================
    // Event-based Playback
    // =========================================================================

    void SetEvent(const std::string& eventName);
    const std::string& GetEventName() const { return m_eventName; }
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

    void SetBusId(uint32 busId);
    uint32 GetBusId() const { return m_settings.busId; }

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

    Vec3 GetAudioWorldPosition() const;
    Audio::AudioHandle GetHandle() const;

    void SetAudioEngine(Audio::AudioEngine* engine);
    Audio::AudioEngine* GetBoundAudioEngine() const { return m_audioEngine; }

    void SetAudioSubsystem(Audio::AudioSubsystem* subsystem);
    Audio::AudioSubsystem* GetBoundAudioSubsystem() const { return m_audioSubsystem; }

    const Audio::OcclusionResult& GetLastOcclusionResult() const;

    float GetPlaybackPosition() const;
    void SetPlaybackPosition(float position);

private:
    std::shared_ptr<Audio::AudioClip> m_clip;
    std::string m_eventName;
    AudioComponentSettings m_settings;

    uint64 m_handleId = 0;
    std::unique_ptr<Audio::AudioSource> m_source;
    Audio::AudioEngine* m_audioEngine = nullptr;
    Audio::AudioSubsystem* m_audioSubsystem = nullptr;
    std::unique_ptr<Audio::OcclusionResult> m_lastOcclusion;

    bool m_needsPositionUpdate = true;

    void UpdatePosition();
    void ApplySpatialOcclusion();
    Audio::AudioEngine* GetAudioEngine() const;
};

} // namespace RVX
