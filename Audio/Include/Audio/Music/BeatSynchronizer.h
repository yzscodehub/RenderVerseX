/**
 * @file BeatSynchronizer.h
 * @brief Beat synchronization for music-driven gameplay
 */

#pragma once

#include "Audio/Music/MusicTrack.h"
#include <functional>
#include <vector>

namespace RVX::Audio
{

/**
 * @brief Beat callback type
 */
using BeatCallback = std::function<void(int bar, int beat)>;

/**
 * @brief Bar callback type
 */
using BarCallback = std::function<void(int bar)>;

/**
 * @brief Marker callback type
 */
using MarkerCallback = std::function<void(const std::string& markerName)>;

/**
 * @brief Beat synchronizer for music-driven events
 * 
 * Tracks the current playback position and fires callbacks
 * on beats, bars, and markers for rhythm-based gameplay.
 * 
 * Usage:
 * @code
 * BeatSynchronizer sync;
 * sync.SetTrack(musicTrack);
 * 
 * sync.OnBeat([](int bar, int beat) {
 *     // Flash the screen, pulse UI, etc.
 * });
 * 
 * sync.OnBar([](int bar) {
 *     // Change game state, spawn enemies, etc.
 * });
 * 
 * // In game loop
 * sync.Update(currentPlaybackPosition);
 * @endcode
 */
class BeatSynchronizer
{
public:
    BeatSynchronizer() = default;
    ~BeatSynchronizer() = default;

    // =========================================================================
    // Track
    // =========================================================================

    void SetTrack(MusicTrack::Ptr track);
    MusicTrack::Ptr GetTrack() const { return m_track; }

    // =========================================================================
    // Update
    // =========================================================================

    /**
     * @brief Update with current playback position
     * @param playbackTime Current playback time in seconds
     */
    void Update(float playbackTime);

    /**
     * @brief Reset to beginning
     */
    void Reset();

    // =========================================================================
    // Callbacks
    // =========================================================================

    /**
     * @brief Set callback for every beat
     */
    void OnBeat(BeatCallback callback);

    /**
     * @brief Set callback for every bar
     */
    void OnBar(BarCallback callback);

    /**
     * @brief Set callback for markers
     */
    void OnMarker(MarkerCallback callback);

    // =========================================================================
    // State
    // =========================================================================

    int GetCurrentBar() const { return m_currentBar; }
    int GetCurrentBeat() const { return m_currentBeat; }
    float GetBeatProgress() const { return m_beatProgress; }  // 0-1 within beat

    /**
     * @brief Get time until next beat
     */
    float GetTimeToNextBeat() const;

    /**
     * @brief Get time until next bar
     */
    float GetTimeToNextBar() const;

    /**
     * @brief Check if we're within a window of a beat
     * @param windowMs Window size in milliseconds
     */
    bool IsNearBeat(float windowMs = 50.0f) const;

    // =========================================================================
    // Quantization
    // =========================================================================

    /**
     * @brief Quantize a time to the nearest beat
     */
    float QuantizeToBeat(float time) const;

    /**
     * @brief Quantize a time to the nearest bar
     */
    float QuantizeToBar(float time) const;

    /**
     * @brief Get time of next beat boundary after given time
     */
    float GetNextBeatTime(float afterTime) const;

    /**
     * @brief Get time of next bar boundary after given time
     */
    float GetNextBarTime(float afterTime) const;

private:
    MusicTrack::Ptr m_track;
    
    // Current state
    int m_currentBar = 0;
    int m_currentBeat = 0;
    float m_beatProgress = 0.0f;
    float m_lastTime = 0.0f;
    
    // Last fired state (to avoid duplicate callbacks)
    int m_lastFiredBar = -1;
    int m_lastFiredBeat = -1;
    size_t m_lastFiredMarkerIndex = 0;
    
    // Callbacks
    BeatCallback m_beatCallback;
    BarCallback m_barCallback;
    MarkerCallback m_markerCallback;

    float GetSecondsPerBeat() const;
    float GetSecondsPerBar() const;
};

} // namespace RVX::Audio
