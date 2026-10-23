/**
 * @file MusicTrack.cpp
 * @brief MusicTrack implementation
 */

#include "Audio/Music/MusicTrack.h"
#include <algorithm>

namespace RVX::Audio
{

MusicTrack::MusicTrack(const std::string& name)
    : m_name(name)
{
}

float MusicTrack::GetDuration() const
{
    if (m_mainClip)
    {
        return m_mainClip->GetDuration();
    }
    return 0.0f;
}

void MusicTrack::AddLayer(const MusicLayer& layer)
{
    m_layers.push_back(layer);
}

MusicLayer* MusicTrack::GetLayer(size_t index)
{
    return (index < m_layers.size()) ? &m_layers[index] : nullptr;
}

const MusicLayer* MusicTrack::GetLayer(size_t index) const
{
    return (index < m_layers.size()) ? &m_layers[index] : nullptr;
}

MusicLayer* MusicTrack::GetLayer(const std::string& name)
{
    for (auto& layer : m_layers)
    {
        if (layer.name == name)
        {
            return &layer;
        }
    }
    return nullptr;
}

void MusicTrack::SetLayerEnabled(const std::string& name, bool enabled)
{
    if (auto* layer = GetLayer(name))
    {
        layer->enabled = enabled;
    }
}

void MusicTrack::SetLayerVolume(const std::string& name, float volume)
{
    if (auto* layer = GetLayer(name))
    {
        layer->volume = volume;
    }
}

float MusicTrack::GetTimeForBeat(int bar, int beat) const
{
    float secondsPerBeat = 60.0f / m_timing.bpm;
    int totalBeats = bar * m_timing.beatsPerBar + beat;
    return m_timing.firstBeatOffset + totalBeats * secondsPerBeat;
}

void MusicTrack::GetBeatForTime(float time, int& outBar, int& outBeat) const
{
    float adjustedTime = time - m_timing.firstBeatOffset;
    if (adjustedTime < 0.0f)
    {
        outBar = 0;
        outBeat = 0;
        return;
    }

    float secondsPerBeat = 60.0f / m_timing.bpm;
    int totalBeats = static_cast<int>(adjustedTime / secondsPerBeat);
    
    outBar = totalBeats / m_timing.beatsPerBar;
    outBeat = totalBeats % m_timing.beatsPerBar;
}

void MusicTrack::AddMarker(const MusicMarker& marker)
{
    m_markers.push_back(marker);
    
    // Keep markers sorted by time
    std::sort(m_markers.begin(), m_markers.end(),
        [](const MusicMarker& a, const MusicMarker& b) {
            return a.time < b.time;
        });
}

const MusicMarker* MusicTrack::GetMarker(const std::string& name) const
{
    for (const auto& marker : m_markers)
    {
        if (marker.name == name)
        {
            return &marker;
        }
    }
    return nullptr;
}

const MusicMarker* MusicTrack::GetNextMarker(float currentTime) const
{
    for (const auto& marker : m_markers)
    {
        if (marker.time > currentTime)
        {
            return &marker;
        }
    }
    return nullptr;
}

void MusicTrack::SetLoopPoints(float startTime, float endTime)
{
    m_loopStart = startTime;
    m_loopEnd = endTime;
}

MusicTrack::Ptr MusicTrack::Create(AudioClip::Ptr clip)
{
    auto track = std::make_shared<MusicTrack>();
    if (clip)
    {
        track->SetName(clip->GetName());
        track->SetClip(clip);
    }
    return track;
}

} // namespace RVX::Audio
