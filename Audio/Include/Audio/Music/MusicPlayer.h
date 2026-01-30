/**
 * @file MusicPlayer.h
 * @brief Music player with crossfade and beat synchronization
 */

#pragma once

#include "Audio/Music/MusicTrack.h"
#include "Audio/Music/BeatSynchronizer.h"
#include "Audio/AudioEngine.h"
#include <queue>

namespace RVX::Audio
{

/**
 * @brief Music transition type
 */
enum class MusicTransition : uint8
{
    Immediate,      ///< Switch immediately
    Crossfade,      ///< Crossfade over duration
    FadeOutIn,      ///< Fade out then fade in
    OnNextBeat,     ///< Wait for next beat
    OnNextBar,      ///< Wait for next bar
    OnMarker        ///< Wait for specific marker
};

/**
 * @brief Stinger playback mode
 */
enum class StingerMode : uint8
{
    Replace,        ///< Replace current music temporarily
    Overlay,        ///< Play on top of current music
    Duck            ///< Duck current music while playing
};

/**
 * @brief Music player with advanced features
 * 
 * Features:
 * - Smooth crossfades between tracks
 * - Beat-synchronized transitions
 * - Stinger support (one-shot overlays)
 * - Layer-based vertical remixing
 * - Beat callbacks for game sync
 * 
 * Usage:
 * @code
 * MusicPlayer player(&engine);
 * 
 * // Play music with crossfade
 * player.Play(track, MusicTransition::Crossfade, 2.0f);
 * 
 * // Transition on next bar
 * player.Play(combatTrack, MusicTransition::OnNextBar);
 * 
 * // Play a stinger
 * player.PlayStinger(victoryStinger);
 * 
 * // Beat callbacks
 * player.GetBeatSync().OnBeat([](int bar, int beat) {
 *     // Sync gameplay to music
 * });
 * @endcode
 */
class MusicPlayer
{
public:
    MusicPlayer(AudioEngine* engine);
    ~MusicPlayer() = default;

    // =========================================================================
    // Playback
    // =========================================================================

    /**
     * @brief Play a music track
     */
    void Play(MusicTrack::Ptr track, 
              MusicTransition transition = MusicTransition::Crossfade,
              float transitionDuration = 1.0f);

    /**
     * @brief Stop current music
     */
    void Stop(float fadeOutDuration = 1.0f);

    /**
     * @brief Pause music
     */
    void Pause();

    /**
     * @brief Resume music
     */
    void Resume();

    /**
     * @brief Check if playing
     */
    bool IsPlaying() const;

    /**
     * @brief Get current track
     */
    MusicTrack::Ptr GetCurrentTrack() const { return m_currentTrack; }

    // =========================================================================
    // Stingers
    // =========================================================================

    /**
     * @brief Play a stinger (one-shot overlay)
     */
    void PlayStinger(AudioClip::Ptr stinger, 
                     StingerMode mode = StingerMode::Overlay,
                     float duckAmount = 0.3f);

    // =========================================================================
    // Volume
    // =========================================================================

    void SetVolume(float volume);
    float GetVolume() const { return m_volume; }

    // =========================================================================
    // Layers (for current track)
    // =========================================================================

    /**
     * @brief Enable/disable a layer by name
     */
    void SetLayerEnabled(const std::string& layerName, bool enabled);

    /**
     * @brief Set layer volume
     */
    void SetLayerVolume(const std::string& layerName, float volume);

    /**
     * @brief Blend layer volume over time
     */
    void BlendLayerVolume(const std::string& layerName, float targetVolume, float duration);

    // =========================================================================
    // Beat Sync
    // =========================================================================

    BeatSynchronizer& GetBeatSync() { return m_beatSync; }
    const BeatSynchronizer& GetBeatSync() const { return m_beatSync; }

    // =========================================================================
    // Position
    // =========================================================================

    float GetPlaybackPosition() const;
    void SetPlaybackPosition(float time);

    /**
     * @brief Seek to a marker
     */
    void SeekToMarker(const std::string& markerName);

    // =========================================================================
    // Update
    // =========================================================================

    /**
     * @brief Update the music player (call each frame)
     */
    void Update(float deltaTime);

private:
    AudioEngine* m_engine;
    
    // Current playback
    MusicTrack::Ptr m_currentTrack;
    AudioHandle m_currentHandle;
    std::vector<AudioHandle> m_layerHandles;
    
    // Pending transition
    MusicTrack::Ptr m_pendingTrack;
    MusicTransition m_pendingTransition = MusicTransition::Immediate;
    float m_transitionDuration = 1.0f;
    std::string m_transitionMarker;
    
    // Crossfade state
    bool m_isCrossfading = false;
    float m_crossfadeProgress = 0.0f;
    AudioHandle m_outgoingHandle;
    std::vector<AudioHandle> m_outgoingLayerHandles;
    
    // Stinger
    AudioHandle m_stingerHandle;
    StingerMode m_stingerMode = StingerMode::Overlay;
    float m_stingerDuckAmount = 0.3f;
    bool m_isDucking = false;
    
    // Volume
    float m_volume = 1.0f;
    
    // Beat sync
    BeatSynchronizer m_beatSync;
    
    // Layer blend state
    struct LayerBlend
    {
        std::string name;
        float startVolume;
        float targetVolume;
        float duration;
        float elapsed;
    };
    std::vector<LayerBlend> m_layerBlends;

    void StartTransition();
    void UpdateCrossfade(float deltaTime);
    void UpdateStinger(float deltaTime);
    void UpdateLayerBlends(float deltaTime);
    void StopCurrentTrack(float fadeOut);
    void StartTrack(MusicTrack::Ptr track);
};

} // namespace RVX::Audio
