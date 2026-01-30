/**
 * @file CompressorEffect.cpp
 * @brief Dynamics compressor and limiter implementations
 */

#include "Audio/DSP/CompressorEffect.h"
#include <cmath>
#include <algorithm>

namespace RVX::Audio
{

// =============================================================================
// CompressorEffect
// =============================================================================

CompressorEffect::CompressorEffect()
{
    m_parameters["threshold"] = -20.0f;
    m_parameters["ratio"] = 4.0f;
    m_parameters["attack"] = 10.0f;
    m_parameters["release"] = 100.0f;
    m_parameters["makeupGain"] = 0.0f;
    m_parameters["knee"] = 0.0f;
    UpdateCoefficients();
}

void CompressorEffect::Process(float* buffer, uint32 frameCount, uint32 channels)
{
    if (!m_enabled) return;

    for (uint32 i = 0; i < frameCount; ++i)
    {
        // Find peak level across all channels
        float peak = 0.0f;
        for (uint32 ch = 0; ch < channels; ++ch)
        {
            peak = std::max(peak, std::abs(buffer[i * channels + ch]));
        }

        // Convert to dB
        float inputDb = (peak > 0.0f) ? LinearToDb(peak) : -100.0f;

        // Calculate gain reduction
        float gainDb = 0.0f;
        if (inputDb > m_threshold)
        {
            // Above threshold: apply compression
            float excess = inputDb - m_threshold;
            gainDb = -(excess * (1.0f - 1.0f / m_ratio));
        }

        // Soft knee (optional)
        if (m_knee > 0.0f)
        {
            float kneeStart = m_threshold - m_knee / 2.0f;
            float kneeEnd = m_threshold + m_knee / 2.0f;

            if (inputDb > kneeStart && inputDb < kneeEnd)
            {
                // In the knee region: smooth transition
                float t = (inputDb - kneeStart) / m_knee;
                float ratio = 1.0f + (m_ratio - 1.0f) * t;
                float excess = inputDb - m_threshold;
                gainDb = -(excess * (1.0f - 1.0f / ratio)) * t;
            }
        }

        // Apply attack/release envelope
        float targetEnvelope = DbToLinear(gainDb);
        if (targetEnvelope < m_envelope)
        {
            // Attack
            m_envelope = m_attackCoef * m_envelope + (1.0f - m_attackCoef) * targetEnvelope;
        }
        else
        {
            // Release
            m_envelope = m_releaseCoef * m_envelope + (1.0f - m_releaseCoef) * targetEnvelope;
        }

        // Apply gain
        float gain = m_envelope * m_makeupLinear;

        for (uint32 ch = 0; ch < channels; ++ch)
        {
            buffer[i * channels + ch] *= gain;
        }

        // Update gain reduction meter
        m_gainReduction = LinearToDb(m_envelope);
    }
}

void CompressorEffect::Reset()
{
    m_envelope = 1.0f;
    m_gainReduction = 0.0f;
}

void CompressorEffect::SetSampleRate(uint32 sampleRate)
{
    m_sampleRate = sampleRate;
    UpdateCoefficients();
}

void CompressorEffect::OnParameterChanged(const std::string& name, float value)
{
    if (name == "threshold")
    {
        m_threshold = std::clamp(value, -60.0f, 0.0f);
    }
    else if (name == "ratio")
    {
        m_ratio = std::clamp(value, 1.0f, 20.0f);
    }
    else if (name == "attack")
    {
        m_attack = std::clamp(value, 0.1f, 100.0f);
    }
    else if (name == "release")
    {
        m_release = std::clamp(value, 10.0f, 1000.0f);
    }
    else if (name == "makeupGain")
    {
        m_makeupGain = std::clamp(value, 0.0f, 30.0f);
    }
    else if (name == "knee")
    {
        m_knee = std::clamp(value, 0.0f, 20.0f);
    }
    UpdateCoefficients();
}

void CompressorEffect::UpdateCoefficients()
{
    // Time constant coefficients
    m_attackCoef = std::exp(-1.0f / (m_attack * 0.001f * static_cast<float>(m_sampleRate)));
    m_releaseCoef = std::exp(-1.0f / (m_release * 0.001f * static_cast<float>(m_sampleRate)));
    m_makeupLinear = DbToLinear(m_makeupGain);
}

float CompressorEffect::LinearToDb(float linear)
{
    if (linear <= 0.0f) return -100.0f;
    return 20.0f * std::log10(linear);
}

float CompressorEffect::DbToLinear(float db)
{
    return std::pow(10.0f, db / 20.0f);
}

// =============================================================================
// LimiterEffect
// =============================================================================

LimiterEffect::LimiterEffect()
{
    m_parameters["threshold"] = -0.1f;
    m_parameters["release"] = 100.0f;
    UpdateCoefficients();
}

void LimiterEffect::Process(float* buffer, uint32 frameCount, uint32 channels)
{
    if (!m_enabled) return;

    for (uint32 i = 0; i < frameCount; ++i)
    {
        // Find peak level
        float peak = 0.0f;
        for (uint32 ch = 0; ch < channels; ++ch)
        {
            peak = std::max(peak, std::abs(buffer[i * channels + ch]));
        }

        // Calculate gain reduction (hard limiting)
        float targetGain = 1.0f;
        if (peak > m_thresholdLinear)
        {
            targetGain = m_thresholdLinear / peak;
        }

        // Apply release envelope (attack is instant)
        if (targetGain < m_envelope)
        {
            m_envelope = targetGain;
        }
        else
        {
            m_envelope = m_releaseCoef * m_envelope + (1.0f - m_releaseCoef) * targetGain;
        }

        // Apply gain
        for (uint32 ch = 0; ch < channels; ++ch)
        {
            buffer[i * channels + ch] *= m_envelope;
        }

        // Update meter
        if (m_envelope < 1.0f)
        {
            m_gainReduction = 20.0f * std::log10(m_envelope);
        }
        else
        {
            m_gainReduction = 0.0f;
        }
    }
}

void LimiterEffect::Reset()
{
    m_envelope = 1.0f;
    m_gainReduction = 0.0f;
}

void LimiterEffect::SetSampleRate(uint32 sampleRate)
{
    m_sampleRate = sampleRate;
    UpdateCoefficients();
}

void LimiterEffect::OnParameterChanged(const std::string& name, float value)
{
    if (name == "threshold")
    {
        m_threshold = std::clamp(value, -20.0f, 0.0f);
    }
    else if (name == "release")
    {
        m_release = std::clamp(value, 10.0f, 1000.0f);
    }
    UpdateCoefficients();
}

void LimiterEffect::UpdateCoefficients()
{
    m_releaseCoef = std::exp(-1.0f / (m_release * 0.001f * static_cast<float>(m_sampleRate)));
    m_thresholdLinear = std::pow(10.0f, m_threshold / 20.0f);
}

} // namespace RVX::Audio
