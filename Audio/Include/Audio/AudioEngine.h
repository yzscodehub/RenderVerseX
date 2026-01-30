/**
 * @file AudioEngine.h
 * @brief Core audio engine for playback and management
 */

#pragma once

#include "Audio/AudioTypes.h"
#include "Audio/AudioClip.h"
#include <memory>
#include <vector>
#include <functional>

namespace RVX::Audio
{

class AudioSource;
class AudioListener;

/**
 * @brief Audio engine configuration
 */
struct AudioEngineConfig
{
    uint32 sampleRate = 48000;
    uint32 channels = 2;
    uint32 bufferSizeFrames = 256;
    uint32 maxVoices = 64;
    bool enableSpatialization = true;
};

/**
 * @brief Main audio engine
 * 
 * Manages audio playback, mixing, and 3D spatialization.
 * 
 * Usage:
 * @code
 * AudioEngine engine;
 * engine.Initialize();
 * 
 * // Load and play a sound
 * auto clip = engine.LoadClip("explosion.wav");
 * AudioHandle sound = engine.Play(clip);
 * 
 * // 3D sound
 * Audio3DSettings settings3D;
 * settings3D.position = Vec3(10, 0, 5);
 * engine.Play3D(clip, settings3D);
 * 
 * // Update (call each frame)
 * engine.Update(deltaTime);
 * @endcode
 */
class AudioEngine
{
public:
    using Ptr = std::shared_ptr<AudioEngine>;

    AudioEngine() = default;
    ~AudioEngine();

    // Non-copyable
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // =========================================================================
    // Lifecycle
    // =========================================================================

    bool Initialize(const AudioEngineConfig& config = {});
    void Shutdown();
    bool IsInitialized() const { return m_initialized; }

    /**
     * @brief Update the audio engine (call each frame)
     */
    void Update(float deltaTime);

    // =========================================================================
    // Clip Management
    // =========================================================================

    /**
     * @brief Load an audio clip from file
     */
    AudioClip::Ptr LoadClip(const std::string& path);

    /**
     * @brief Unload a clip
     */
    void UnloadClip(AudioClip::Ptr clip);

    // =========================================================================
    // Playback
    // =========================================================================

    /**
     * @brief Play a clip (2D, non-positioned)
     */
    AudioHandle Play(AudioClip::Ptr clip, const AudioPlaySettings& settings = {});

    /**
     * @brief Play a clip with 3D positioning
     */
    AudioHandle Play3D(AudioClip::Ptr clip, const Audio3DSettings& settings3D,
                       const AudioPlaySettings& playSettings = {});

    /**
     * @brief Stop a playing sound
     */
    void Stop(AudioHandle handle, float fadeOutTime = 0.0f);

    /**
     * @brief Stop all sounds
     */
    void StopAll(float fadeOutTime = 0.0f);

    /**
     * @brief Pause a playing sound
     */
    void Pause(AudioHandle handle);

    /**
     * @brief Resume a paused sound
     */
    void Resume(AudioHandle handle);

    /**
     * @brief Check if a sound is playing
     */
    bool IsPlaying(AudioHandle handle) const;

    /**
     * @brief Get playback position (seconds)
     */
    float GetPlaybackPosition(AudioHandle handle) const;

    /**
     * @brief Set playback position (seconds)
     */
    void SetPlaybackPosition(AudioHandle handle, float position);

    // =========================================================================
    // Sound Properties (runtime modification)
    // =========================================================================

    void SetVolume(AudioHandle handle, float volume);
    void SetPitch(AudioHandle handle, float pitch);
    void SetPan(AudioHandle handle, float pan);
    void SetLooping(AudioHandle handle, bool loop);

    void SetPosition(AudioHandle handle, const Vec3& position);
    void SetVelocity(AudioHandle handle, const Vec3& velocity);

    // =========================================================================
    // Listener
    // =========================================================================

    /**
     * @brief Set the listener position and orientation
     */
    void SetListenerTransform(const Vec3& position, const Vec3& forward, const Vec3& up);

    /**
     * @brief Set listener velocity (for doppler)
     */
    void SetListenerVelocity(const Vec3& velocity);

    // =========================================================================
    // Master Volume
    // =========================================================================

    void SetMasterVolume(float volume) { m_masterVolume = volume; }
    float GetMasterVolume() const { return m_masterVolume; }

    void SetMuted(bool muted) { m_muted = muted; }
    bool IsMuted() const { return m_muted; }

    // =========================================================================
    // Audio Buses
    // =========================================================================

    /**
     * @brief Create an audio bus for grouping sounds
     */
    uint32 CreateBus(const std::string& name, uint32 parentBus = 0);

    /**
     * @brief Set bus volume
     */
    void SetBusVolume(uint32 busId, float volume);

    /**
     * @brief Mute/unmute a bus
     */
    void SetBusMuted(uint32 busId, bool muted);

    // =========================================================================
    // Statistics
    // =========================================================================

    struct Statistics
    {
        uint32 activeVoices = 0;
        uint32 totalVoices = 0;
        float cpuUsage = 0.0f;
        size_t memoryUsed = 0;
    };

    Statistics GetStatistics() const;

private:
    AudioEngineConfig m_config;
    bool m_initialized = false;

    float m_masterVolume = 1.0f;
    bool m_muted = false;

    Vec3 m_listenerPosition{0.0f};
    Vec3 m_listenerForward{0.0f, 0.0f, -1.0f};
    Vec3 m_listenerUp{0.0f, 1.0f, 0.0f};
    Vec3 m_listenerVelocity{0.0f};

    uint64 m_nextHandleId = 1;
    std::vector<std::unique_ptr<AudioSource>> m_sources;
    std::vector<AudioBus> m_buses;

    void* m_backendData = nullptr;  // miniaudio engine pointer
};

/**
 * @brief Global audio engine access
 */
AudioEngine& GetAudioEngine();

} // namespace RVX::Audio
