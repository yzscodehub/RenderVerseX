/**
 * @file AudioSubsystem.h
 * @brief Engine subsystem for audio management
 */

#pragma once

#include "Core/Subsystem/EngineSubsystem.h"
#include "Audio/AudioEngine.h"
#include <memory>

namespace RVX::Audio
{

class AudioMixer;
class MusicPlayer;

/**
 * @brief Engine subsystem for audio management
 * 
 * Provides centralized audio functionality as an engine subsystem.
 * Initializes the AudioEngine and provides access to audio services.
 * 
 * Usage:
 * @code
 * // Get the subsystem from engine
 * auto* audioSub = engine->GetSubsystem<AudioSubsystem>();
 * 
 * // Play a sound
 * auto clip = audioSub->GetEngine().LoadClip("explosion.wav");
 * audioSub->GetEngine().Play(clip);
 * @endcode
 */
class AudioSubsystem : public EngineSubsystem
{
public:
    // =========================================================================
    // Construction
    // =========================================================================

    AudioSubsystem() = default;
    ~AudioSubsystem() override = default;

    // Non-copyable
    AudioSubsystem(const AudioSubsystem&) = delete;
    AudioSubsystem& operator=(const AudioSubsystem&) = delete;

    // =========================================================================
    // ISubsystem Interface
    // =========================================================================

    const char* GetName() const override { return "AudioSubsystem"; }

    bool ShouldTick() const override { return true; }

    TickPhase GetTickPhase() const override { return TickPhase::PostUpdate; }

    void Initialize() override;
    void Deinitialize() override;
    void Tick(float deltaTime) override;

    // =========================================================================
    // Audio Access
    // =========================================================================

    /**
     * @brief Get the audio engine
     */
    AudioEngine& GetEngine() { return m_engine; }
    const AudioEngine& GetEngine() const { return m_engine; }

    /**
     * @brief Get the audio mixer (if available)
     */
    AudioMixer* GetMixer() { return m_mixer.get(); }
    const AudioMixer* GetMixer() const { return m_mixer.get(); }

    /**
     * @brief Get the music player (if available)
     */
    MusicPlayer* GetMusicPlayer() { return m_musicPlayer.get(); }
    const MusicPlayer* GetMusicPlayer() const { return m_musicPlayer.get(); }

    // =========================================================================
    // Configuration
    // =========================================================================

    /**
     * @brief Set audio configuration before initialization
     * @note Must be called before Initialize()
     */
    void SetConfig(const AudioEngineConfig& config);

    /**
     * @brief Get current configuration
     */
    const AudioEngineConfig& GetConfig() const { return m_config; }

    // =========================================================================
    // Convenience Methods
    // =========================================================================

    /**
     * @brief Quick play a sound by path
     */
    AudioHandle PlaySound(const std::string& path, float volume = 1.0f);

    /**
     * @brief Quick play a 3D sound by path
     */
    AudioHandle PlaySound3D(const std::string& path, const Vec3& position, float volume = 1.0f);

    /**
     * @brief Set global audio pause state
     */
    void SetPaused(bool paused);
    bool IsPaused() const { return m_paused; }

    /**
     * @brief Pause all audio (e.g., when game is paused)
     */
    void PauseAll();

    /**
     * @brief Resume all audio
     */
    void ResumeAll();

private:
    AudioEngineConfig m_config;
    AudioEngine m_engine;
    
    std::unique_ptr<AudioMixer> m_mixer;
    std::unique_ptr<MusicPlayer> m_musicPlayer;
    
    bool m_paused = false;
    
    // Cache for quick play sounds
    std::unordered_map<std::string, AudioClip::Ptr> m_clipCache;
};

} // namespace RVX::Audio
