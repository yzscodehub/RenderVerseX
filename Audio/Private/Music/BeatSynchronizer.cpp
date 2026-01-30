/**
 * @file BeatSynchronizer.cpp
 * @brief BeatSynchronizer implementation
 */

#include "Audio/Music/BeatSynchronizer.h"
#include <cmath>

namespace RVX::Audio
{

void BeatSynchronizer::SetTrack(MusicTrack::Ptr track)
{
    m_track = std::move(track);
    Reset();
}

void BeatSynchronizer::Update(float playbackTime)
{
    if (!m_track)
    {
        return;
    }

    m_lastTime = playbackTime;

    const auto& timing = m_track->GetTiming();
    float adjustedTime = playbackTime - timing.firstBeatOffset;

    if (adjustedTime < 0.0f)
    {
        m_currentBar = 0;
        m_currentBeat = 0;
        m_beatProgress = 0.0f;
        return;
    }

    float secondsPerBeat = GetSecondsPerBeat();
    float totalBeatsFloat = adjustedTime / secondsPerBeat;
    int totalBeats = static_cast<int>(totalBeatsFloat);

    m_currentBar = totalBeats / timing.beatsPerBar;
    m_currentBeat = totalBeats % timing.beatsPerBar;
    m_beatProgress = totalBeatsFloat - static_cast<float>(totalBeats);

    // Fire beat callback
    if (m_beatCallback && (m_currentBar != m_lastFiredBar || m_currentBeat != m_lastFiredBeat))
    {
        m_beatCallback(m_currentBar, m_currentBeat);
        m_lastFiredBeat = m_currentBeat;
    }

    // Fire bar callback
    if (m_barCallback && m_currentBar != m_lastFiredBar && m_currentBeat == 0)
    {
        m_barCallback(m_currentBar);
        m_lastFiredBar = m_currentBar;
    }

    // Fire marker callbacks
    if (m_markerCallback)
    {
        const auto& markers = m_track->GetMarkers();
        while (m_lastFiredMarkerIndex < markers.size() && 
               markers[m_lastFiredMarkerIndex].time <= playbackTime)
        {
            m_markerCallback(markers[m_lastFiredMarkerIndex].name);
            m_lastFiredMarkerIndex++;
        }
    }
}

void BeatSynchronizer::Reset()
{
    m_currentBar = 0;
    m_currentBeat = 0;
    m_beatProgress = 0.0f;
    m_lastTime = 0.0f;
    m_lastFiredBar = -1;
    m_lastFiredBeat = -1;
    m_lastFiredMarkerIndex = 0;
}

void BeatSynchronizer::OnBeat(BeatCallback callback)
{
    m_beatCallback = std::move(callback);
}

void BeatSynchronizer::OnBar(BarCallback callback)
{
    m_barCallback = std::move(callback);
}

void BeatSynchronizer::OnMarker(MarkerCallback callback)
{
    m_markerCallback = std::move(callback);
}

float BeatSynchronizer::GetTimeToNextBeat() const
{
    return (1.0f - m_beatProgress) * GetSecondsPerBeat();
}

float BeatSynchronizer::GetTimeToNextBar() const
{
    if (!m_track)
    {
        return 0.0f;
    }

    const auto& timing = m_track->GetTiming();
    int beatsRemainingInBar = timing.beatsPerBar - m_currentBeat - 1;
    
    return GetTimeToNextBeat() + beatsRemainingInBar * GetSecondsPerBeat();
}

bool BeatSynchronizer::IsNearBeat(float windowMs) const
{
    float windowSec = windowMs / 1000.0f;
    float secondsPerBeat = GetSecondsPerBeat();
    
    // Check if we're near the start of a beat
    float timeIntoBeat = m_beatProgress * secondsPerBeat;
    if (timeIntoBeat < windowSec)
    {
        return true;
    }
    
    // Check if we're near the end (next beat)
    float timeToNextBeat = GetTimeToNextBeat();
    if (timeToNextBeat < windowSec)
    {
        return true;
    }
    
    return false;
}

float BeatSynchronizer::QuantizeToBeat(float time) const
{
    if (!m_track)
    {
        return time;
    }

    const auto& timing = m_track->GetTiming();
    float adjustedTime = time - timing.firstBeatOffset;
    float secondsPerBeat = GetSecondsPerBeat();

    float beats = adjustedTime / secondsPerBeat;
    float quantizedBeats = std::round(beats);

    return timing.firstBeatOffset + quantizedBeats * secondsPerBeat;
}

float BeatSynchronizer::QuantizeToBar(float time) const
{
    if (!m_track)
    {
        return time;
    }

    const auto& timing = m_track->GetTiming();
    float adjustedTime = time - timing.firstBeatOffset;
    float secondsPerBar = GetSecondsPerBar();

    float bars = adjustedTime / secondsPerBar;
    float quantizedBars = std::round(bars);

    return timing.firstBeatOffset + quantizedBars * secondsPerBar;
}

float BeatSynchronizer::GetNextBeatTime(float afterTime) const
{
    if (!m_track)
    {
        return afterTime;
    }

    const auto& timing = m_track->GetTiming();
    float adjustedTime = afterTime - timing.firstBeatOffset;
    float secondsPerBeat = GetSecondsPerBeat();

    float beats = adjustedTime / secondsPerBeat;
    float nextBeat = std::ceil(beats);

    return timing.firstBeatOffset + nextBeat * secondsPerBeat;
}

float BeatSynchronizer::GetNextBarTime(float afterTime) const
{
    if (!m_track)
    {
        return afterTime;
    }

    const auto& timing = m_track->GetTiming();
    float adjustedTime = afterTime - timing.firstBeatOffset;
    float secondsPerBar = GetSecondsPerBar();

    float bars = adjustedTime / secondsPerBar;
    float nextBar = std::ceil(bars);

    return timing.firstBeatOffset + nextBar * secondsPerBar;
}

float BeatSynchronizer::GetSecondsPerBeat() const
{
    if (!m_track)
    {
        return 0.5f;  // Default 120 BPM
    }
    return 60.0f / m_track->GetTiming().bpm;
}

float BeatSynchronizer::GetSecondsPerBar() const
{
    if (!m_track)
    {
        return 2.0f;  // Default 4/4 at 120 BPM
    }
    return GetSecondsPerBeat() * m_track->GetTiming().beatsPerBar;
}

} // namespace RVX::Audio
