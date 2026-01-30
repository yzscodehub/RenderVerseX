/**
 * @file ReverbEffect.cpp
 * @brief Reverb effect implementation (Freeverb-style algorithm)
 */

#include "Audio/DSP/ReverbEffect.h"
#include <cmath>
#include <algorithm>

namespace RVX::Audio
{

// Comb filter tunings (in samples at 44100 Hz)
static constexpr int kCombTuningL[] = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
static constexpr int kCombTuningR[] = { 1116 + 23, 1188 + 23, 1277 + 23, 1356 + 23, 1422 + 23, 1491 + 23, 1557 + 23, 1617 + 23 };

// All-pass filter tunings
static constexpr int kAllpassTuningL[] = { 556, 441, 341, 225 };
static constexpr int kAllpassTuningR[] = { 556 + 23, 441 + 23, 341 + 23, 225 + 23 };

// =============================================================================
// CombFilter
// =============================================================================

void ReverbEffect::CombFilter::SetSize(int size)
{
    bufferSize = size;
    buffer.resize(size, 0.0f);
    index = 0;
}

float ReverbEffect::CombFilter::Process(float input)
{
    float output = buffer[index];
    filterStore = (output * damp2) + (filterStore * damp1);
    buffer[index] = input + (filterStore * feedback);
    
    index++;
    if (index >= bufferSize)
    {
        index = 0;
    }
    
    return output;
}

void ReverbEffect::CombFilter::Clear()
{
    std::fill(buffer.begin(), buffer.end(), 0.0f);
    filterStore = 0.0f;
}

// =============================================================================
// AllpassFilter
// =============================================================================

void ReverbEffect::AllpassFilter::SetSize(int size)
{
    bufferSize = size;
    buffer.resize(size, 0.0f);
    index = 0;
}

float ReverbEffect::AllpassFilter::Process(float input)
{
    float bufOut = buffer[index];
    float output = -input + bufOut;
    buffer[index] = input + (bufOut * feedback);
    
    index++;
    if (index >= bufferSize)
    {
        index = 0;
    }
    
    return output;
}

void ReverbEffect::AllpassFilter::Clear()
{
    std::fill(buffer.begin(), buffer.end(), 0.0f);
}

// =============================================================================
// ReverbEffect
// =============================================================================

ReverbEffect::ReverbEffect()
{
    // Initialize with default parameters
    m_parameters["roomSize"] = 0.5f;
    m_parameters["damping"] = 0.5f;
    m_parameters["wetLevel"] = 0.3f;
    m_parameters["dryLevel"] = 0.7f;
    m_parameters["width"] = 1.0f;
    m_parameters["preDelay"] = 0.0f;

    // Set up comb filters
    for (int i = 0; i < kNumCombs; ++i)
    {
        m_combL[i].SetSize(kCombTuningL[i]);
        m_combR[i].SetSize(kCombTuningR[i]);
    }

    // Set up all-pass filters
    for (int i = 0; i < kNumAllpass; ++i)
    {
        m_allpassL[i].SetSize(kAllpassTuningL[i]);
        m_allpassR[i].SetSize(kAllpassTuningR[i]);
        m_allpassL[i].feedback = 0.5f;
        m_allpassR[i].feedback = 0.5f;
    }

    UpdateParameters();
}

void ReverbEffect::Process(float* buffer, uint32 frameCount, uint32 channels)
{
    if (!m_enabled || channels < 2) return;

    const float wet1 = m_wetLevel * ((m_width / 2.0f) + 0.5f);
    const float wet2 = m_wetLevel * ((1.0f - m_width) / 2.0f);

    for (uint32 i = 0; i < frameCount; ++i)
    {
        float inL = buffer[i * channels];
        float inR = buffer[i * channels + 1];
        float input = (inL + inR) * 0.5f;  // Sum to mono for processing

        float outL = 0.0f;
        float outR = 0.0f;

        // Comb filters in parallel
        for (int c = 0; c < kNumCombs; ++c)
        {
            outL += m_combL[c].Process(input);
            outR += m_combR[c].Process(input);
        }

        // All-pass filters in series
        for (int a = 0; a < kNumAllpass; ++a)
        {
            outL = m_allpassL[a].Process(outL);
            outR = m_allpassR[a].Process(outR);
        }

        // Mix wet and dry
        buffer[i * channels] = inL * m_dryLevel + outL * wet1 + outR * wet2;
        buffer[i * channels + 1] = inR * m_dryLevel + outR * wet1 + outL * wet2;
    }
}

void ReverbEffect::Reset()
{
    for (auto& comb : m_combL) comb.Clear();
    for (auto& comb : m_combR) comb.Clear();
    for (auto& ap : m_allpassL) ap.Clear();
    for (auto& ap : m_allpassR) ap.Clear();
}

void ReverbEffect::SetPreset(ReverbPreset preset)
{
    m_preset = preset;

    switch (preset)
    {
        case ReverbPreset::SmallRoom:
            SetParameter("roomSize", 0.3f);
            SetParameter("damping", 0.7f);
            SetParameter("wetLevel", 0.2f);
            SetParameter("dryLevel", 0.8f);
            break;

        case ReverbPreset::MediumRoom:
            SetParameter("roomSize", 0.5f);
            SetParameter("damping", 0.5f);
            SetParameter("wetLevel", 0.3f);
            SetParameter("dryLevel", 0.7f);
            break;

        case ReverbPreset::LargeRoom:
            SetParameter("roomSize", 0.7f);
            SetParameter("damping", 0.4f);
            SetParameter("wetLevel", 0.35f);
            SetParameter("dryLevel", 0.65f);
            break;

        case ReverbPreset::Hall:
            SetParameter("roomSize", 0.8f);
            SetParameter("damping", 0.3f);
            SetParameter("wetLevel", 0.4f);
            SetParameter("dryLevel", 0.6f);
            break;

        case ReverbPreset::Cathedral:
            SetParameter("roomSize", 0.95f);
            SetParameter("damping", 0.2f);
            SetParameter("wetLevel", 0.5f);
            SetParameter("dryLevel", 0.5f);
            break;

        case ReverbPreset::Cave:
            SetParameter("roomSize", 0.9f);
            SetParameter("damping", 0.6f);
            SetParameter("wetLevel", 0.6f);
            SetParameter("dryLevel", 0.4f);
            break;

        case ReverbPreset::Arena:
            SetParameter("roomSize", 0.85f);
            SetParameter("damping", 0.25f);
            SetParameter("wetLevel", 0.45f);
            SetParameter("dryLevel", 0.55f);
            break;

        case ReverbPreset::Custom:
        default:
            break;
    }
}

void ReverbEffect::OnParameterChanged(const std::string& name, float value)
{
    (void)name;
    (void)value;
    UpdateParameters();
}

void ReverbEffect::UpdateParameters()
{
    m_roomSize = m_parameters["roomSize"];
    m_damping = m_parameters["damping"];
    m_wetLevel = m_parameters["wetLevel"];
    m_dryLevel = m_parameters["dryLevel"];
    m_width = m_parameters["width"];

    // Update comb filter parameters
    float feedback = 0.28f + (m_roomSize * 0.7f);

    for (auto& comb : m_combL)
    {
        comb.feedback = feedback;
        comb.damp1 = m_damping;
        comb.damp2 = 1.0f - m_damping;
    }
    for (auto& comb : m_combR)
    {
        comb.feedback = feedback;
        comb.damp1 = m_damping;
        comb.damp2 = 1.0f - m_damping;
    }
}

} // namespace RVX::Audio
