/**
 * @file MusicTrack.h
 * @brief Music track with timing and layer information
 */

#pragma once

#include "Audio/AudioClip.h"
#include "Audio/AudioTypes.h"
#include <string>
#include <vector>

namespace RVX::Audio
{

/**
 * @brief Music layer for vertical remixing
 */
struct MusicLayer
{
    std::string name;
    AudioClip::Ptr clip;
    float volume = 1.0f;
    bool enabled = true;
};

/**
 * @brief Music track timing information
 */
struct MusicTiming
{
    float bpm = 120.0f;           ///< Beats per minute
    int beatsPerBar = 4;          ///< Time signature numerator
    int beatUnit = 4;             ///< Time signature denominator
    float firstBeatOffset = 0.0f; ///< Offset to first beat in seconds
};

/**
 * @brief Transition point for music
 */
struct MusicMarker
{
    std::string name;
    float time = 0.0f;      ///< Time in seconds
    int bar = 0;            ///< Bar number (if beat-synced)
    int beat = 0;           ///< Beat within bar
};

/**
 * @brief Music track with layers and timing
 * 
 * A MusicTrack represents a piece of music with optional layers
 * for vertical remixing (adding/removing instruments based on intensity).
 */
class MusicTrack
{
public:
    using Ptr = std::shared_ptr<MusicTrack>;

    MusicTrack() = default;
    explicit MusicTrack(const std::string& name);
    ~MusicTrack() = default;

    // =========================================================================
    // Properties
    // =========================================================================

    const std::string& GetName() const { return m_name; }
    void SetName(const std::string& name) { m_name = name; }

    // =========================================================================
    // Main Clip
    // =========================================================================

    void SetClip(AudioClip::Ptr clip) { m_mainClip = std::move(clip); }
    AudioClip::Ptr GetClip() const { return m_mainClip; }

    float GetDuration() const;

    // =========================================================================
    // Layers
    // =========================================================================

    /**
     * @brief Add a layer
     */
    void AddLayer(const MusicLayer& layer);

    /**
     * @brief Get layer by index
     */
    MusicLayer* GetLayer(size_t index);
    const MusicLayer* GetLayer(size_t index) const;

    /**
     * @brief Get layer by name
     */
    MusicLayer* GetLayer(const std::string& name);

    /**
     * @brief Get layer count
     */
    size_t GetLayerCount() const { return m_layers.size(); }

    /**
     * @brief Enable/disable a layer
     */
    void SetLayerEnabled(const std::string& name, bool enabled);
    void SetLayerVolume(const std::string& name, float volume);

    // =========================================================================
    // Timing
    // =========================================================================

    void SetTiming(const MusicTiming& timing) { m_timing = timing; }
    const MusicTiming& GetTiming() const { return m_timing; }

    float GetBPM() const { return m_timing.bpm; }
    void SetBPM(float bpm) { m_timing.bpm = bpm; }

    /**
     * @brief Get time for a specific bar/beat
     */
    float GetTimeForBeat(int bar, int beat) const;

    /**
     * @brief Get bar/beat for a specific time
     */
    void GetBeatForTime(float time, int& outBar, int& outBeat) const;

    // =========================================================================
    // Markers
    // =========================================================================

    void AddMarker(const MusicMarker& marker);
    const MusicMarker* GetMarker(const std::string& name) const;
    const std::vector<MusicMarker>& GetMarkers() const { return m_markers; }

    /**
     * @brief Get the next marker after a given time
     */
    const MusicMarker* GetNextMarker(float currentTime) const;

    // =========================================================================
    // Loop Points
    // =========================================================================

    void SetLoopPoints(float startTime, float endTime);
    float GetLoopStart() const { return m_loopStart; }
    float GetLoopEnd() const { return m_loopEnd; }
    bool HasLoopPoints() const { return m_loopEnd > m_loopStart; }

    // =========================================================================
    // Factory
    // =========================================================================

    static Ptr Create(const std::string& name) { return std::make_shared<MusicTrack>(name); }
    static Ptr Create(AudioClip::Ptr clip);

private:
    std::string m_name;
    AudioClip::Ptr m_mainClip;
    std::vector<MusicLayer> m_layers;
    
    MusicTiming m_timing;
    std::vector<MusicMarker> m_markers;
    
    float m_loopStart = 0.0f;
    float m_loopEnd = 0.0f;
};

} // namespace RVX::Audio
