/**
 * @file LowPassEffect.cpp
 * @brief Low-pass and high-pass filter implementations
 */

#include "Audio/DSP/LowPassEffect.h"
#include <cmath>
#include <algorithm>

namespace RVX::Audio
{

static constexpr float kPi = 3.14159265358979323846f;

// =============================================================================
// LowPassEffect
// =============================================================================

LowPassEffect::LowPassEffect()
{
    m_parameters["cutoff"] = 5000.0f;
    m_parameters["resonance"] = 0.707f;
    UpdateCoefficients();
}

void LowPassEffect::Process(float* buffer, uint32 frameCount, uint32 channels)
{
    if (!m_enabled) return;

    uint32 numChannels = std::min(channels, static_cast<uint32>(kMaxChannels));

    for (uint32 i = 0; i < frameCount; ++i)
    {
        for (uint32 ch = 0; ch < numChannels; ++ch)
        {
            float input = buffer[i * channels + ch];

            // Direct Form II Transposed biquad
            float output = m_a0 * input + m_z1[ch];
            m_z1[ch] = m_a1 * input - m_b1 * output + m_z2[ch];
            m_z2[ch] = m_a2 * input - m_b2 * output;

            buffer[i * channels + ch] = output;
        }
    }
}

void LowPassEffect::Reset()
{
    m_z1.fill(0.0f);
    m_z2.fill(0.0f);
}

void LowPassEffect::OnParameterChanged(const std::string& name, float value)
{
    if (name == "cutoff")
    {
        m_cutoff = std::clamp(value, 20.0f, 20000.0f);
    }
    else if (name == "resonance")
    {
        m_resonance = std::clamp(value, 0.1f, 10.0f);
    }
    UpdateCoefficients();
}

void LowPassEffect::UpdateCoefficients()
{
    // Biquad low-pass filter design
    float omega = 2.0f * kPi * m_cutoff / static_cast<float>(m_sampleRate);
    float sinOmega = std::sin(omega);
    float cosOmega = std::cos(omega);
    float alpha = sinOmega / (2.0f * m_resonance);

    float b0 = (1.0f - cosOmega) / 2.0f;
    float b1 = 1.0f - cosOmega;
    float b2 = (1.0f - cosOmega) / 2.0f;
    float a0 = 1.0f + alpha;
    float a1 = -2.0f * cosOmega;
    float a2 = 1.0f - alpha;

    // Normalize
    m_a0 = b0 / a0;
    m_a1 = b1 / a0;
    m_a2 = b2 / a0;
    m_b1 = a1 / a0;
    m_b2 = a2 / a0;
}

// =============================================================================
// HighPassEffect
// =============================================================================

HighPassEffect::HighPassEffect()
{
    m_parameters["cutoff"] = 200.0f;
    m_parameters["resonance"] = 0.707f;
    UpdateCoefficients();
}

void HighPassEffect::Process(float* buffer, uint32 frameCount, uint32 channels)
{
    if (!m_enabled) return;

    uint32 numChannels = std::min(channels, static_cast<uint32>(kMaxChannels));

    for (uint32 i = 0; i < frameCount; ++i)
    {
        for (uint32 ch = 0; ch < numChannels; ++ch)
        {
            float input = buffer[i * channels + ch];

            float output = m_a0 * input + m_z1[ch];
            m_z1[ch] = m_a1 * input - m_b1 * output + m_z2[ch];
            m_z2[ch] = m_a2 * input - m_b2 * output;

            buffer[i * channels + ch] = output;
        }
    }
}

void HighPassEffect::Reset()
{
    m_z1.fill(0.0f);
    m_z2.fill(0.0f);
}

void HighPassEffect::OnParameterChanged(const std::string& name, float value)
{
    if (name == "cutoff")
    {
        m_cutoff = std::clamp(value, 20.0f, 20000.0f);
    }
    else if (name == "resonance")
    {
        m_resonance = std::clamp(value, 0.1f, 10.0f);
    }
    UpdateCoefficients();
}

void HighPassEffect::UpdateCoefficients()
{
    // Biquad high-pass filter design
    float omega = 2.0f * kPi * m_cutoff / static_cast<float>(m_sampleRate);
    float sinOmega = std::sin(omega);
    float cosOmega = std::cos(omega);
    float alpha = sinOmega / (2.0f * m_resonance);

    float b0 = (1.0f + cosOmega) / 2.0f;
    float b1 = -(1.0f + cosOmega);
    float b2 = (1.0f + cosOmega) / 2.0f;
    float a0 = 1.0f + alpha;
    float a1 = -2.0f * cosOmega;
    float a2 = 1.0f - alpha;

    // Normalize
    m_a0 = b0 / a0;
    m_a1 = b1 / a0;
    m_a2 = b2 / a0;
    m_b1 = a1 / a0;
    m_b2 = a2 / a0;
}

} // namespace RVX::Audio
