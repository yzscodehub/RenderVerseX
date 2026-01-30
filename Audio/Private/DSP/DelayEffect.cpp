/**
 * @file DelayEffect.cpp
 * @brief Delay effect implementations
 */

#include "Audio/DSP/DelayEffect.h"
#include <cmath>
#include <algorithm>

namespace RVX::Audio
{

// =============================================================================
// DelayEffect
// =============================================================================

DelayEffect::DelayEffect()
{
    m_parameters["delayTime"] = 250.0f;
    m_parameters["feedback"] = 0.5f;
    m_parameters["wetLevel"] = 0.5f;
    UpdateDelayLine();
}

void DelayEffect::Process(float* buffer, uint32 frameCount, uint32 channels)
{
    if (!m_enabled || m_delayBufferSize == 0) return;

    uint32 numChannels = std::min(channels, static_cast<uint32>(kMaxChannels));

    for (uint32 i = 0; i < frameCount; ++i)
    {
        for (uint32 ch = 0; ch < numChannels; ++ch)
        {
            // Read from delay line
            int readIndex = m_writeIndex[ch] - m_delaySamples;
            if (readIndex < 0)
            {
                readIndex += m_delayBufferSize;
            }

            float delayed = m_delayLines[ch][readIndex];
            float input = buffer[i * channels + ch];

            // Write to delay line with feedback
            m_delayLines[ch][m_writeIndex[ch]] = input + (delayed * m_feedback);

            // Mix wet and dry
            buffer[i * channels + ch] = input * (1.0f - m_wetLevel) + delayed * m_wetLevel;

            // Advance write index
            m_writeIndex[ch]++;
            if (m_writeIndex[ch] >= m_delayBufferSize)
            {
                m_writeIndex[ch] = 0;
            }
        }
    }
}

void DelayEffect::Reset()
{
    for (auto& line : m_delayLines)
    {
        std::fill(line.begin(), line.end(), 0.0f);
    }
    m_writeIndex.fill(0);
}

void DelayEffect::SetSampleRate(uint32 sampleRate)
{
    m_sampleRate = sampleRate;
    UpdateDelayLine();
}

void DelayEffect::SetMaxDelayMs(float maxDelayMs)
{
    m_maxDelayMs = maxDelayMs;
    UpdateDelayLine();
}

void DelayEffect::OnParameterChanged(const std::string& name, float value)
{
    if (name == "delayTime")
    {
        m_delayTime = std::clamp(value, 0.0f, m_maxDelayMs);
        m_delaySamples = static_cast<int>((m_delayTime / 1000.0f) * static_cast<float>(m_sampleRate));
    }
    else if (name == "feedback")
    {
        m_feedback = std::clamp(value, 0.0f, 0.99f);  // Limit to prevent runaway
    }
    else if (name == "wetLevel")
    {
        m_wetLevel = std::clamp(value, 0.0f, 1.0f);
    }
}

void DelayEffect::UpdateDelayLine()
{
    m_delayBufferSize = static_cast<int>((m_maxDelayMs / 1000.0f) * static_cast<float>(m_sampleRate)) + 1;
    
    for (auto& line : m_delayLines)
    {
        line.resize(m_delayBufferSize, 0.0f);
    }
    
    m_delaySamples = static_cast<int>((m_delayTime / 1000.0f) * static_cast<float>(m_sampleRate));
    m_writeIndex.fill(0);
}

// =============================================================================
// PingPongDelay
// =============================================================================

PingPongDelay::PingPongDelay()
{
    m_parameters["delayTime"] = 250.0f;
    m_parameters["feedback"] = 0.5f;
    m_parameters["wetLevel"] = 0.5f;
    m_parameters["spread"] = 0.5f;
    UpdateDelayLine();
}

void PingPongDelay::Process(float* buffer, uint32 frameCount, uint32 channels)
{
    if (!m_enabled || channels < 2 || m_delayBufferSize == 0) return;

    for (uint32 i = 0; i < frameCount; ++i)
    {
        float inL = buffer[i * channels];
        float inR = buffer[i * channels + 1];
        float input = (inL + inR) * 0.5f;

        // Read from delay lines
        int readIndexL = m_writeIndexL - m_delaySamples;
        int readIndexR = m_writeIndexR - m_delaySamples;
        if (readIndexL < 0) readIndexL += m_delayBufferSize;
        if (readIndexR < 0) readIndexR += m_delayBufferSize;

        float delayedL = m_delayLineL[readIndexL];
        float delayedR = m_delayLineR[readIndexR];

        // Ping-pong: Left feeds into right delay, right feeds into left
        m_delayLineL[m_writeIndexL] = input + (delayedR * m_feedback);
        m_delayLineR[m_writeIndexR] = input + (delayedL * m_feedback);

        // Mix
        buffer[i * channels] = inL * (1.0f - m_wetLevel) + delayedL * m_wetLevel;
        buffer[i * channels + 1] = inR * (1.0f - m_wetLevel) + delayedR * m_wetLevel;

        // Advance indices
        m_writeIndexL++;
        m_writeIndexR++;
        if (m_writeIndexL >= m_delayBufferSize) m_writeIndexL = 0;
        if (m_writeIndexR >= m_delayBufferSize) m_writeIndexR = 0;
    }
}

void PingPongDelay::Reset()
{
    std::fill(m_delayLineL.begin(), m_delayLineL.end(), 0.0f);
    std::fill(m_delayLineR.begin(), m_delayLineR.end(), 0.0f);
    m_writeIndexL = 0;
    m_writeIndexR = 0;
}

void PingPongDelay::SetSampleRate(uint32 sampleRate)
{
    m_sampleRate = sampleRate;
    UpdateDelayLine();
}

void PingPongDelay::OnParameterChanged(const std::string& name, float value)
{
    if (name == "delayTime")
    {
        m_delayTime = std::clamp(value, 0.0f, 2000.0f);
        m_delaySamples = static_cast<int>((m_delayTime / 1000.0f) * static_cast<float>(m_sampleRate));
    }
    else if (name == "feedback")
    {
        m_feedback = std::clamp(value, 0.0f, 0.99f);
    }
    else if (name == "wetLevel")
    {
        m_wetLevel = std::clamp(value, 0.0f, 1.0f);
    }
    else if (name == "spread")
    {
        m_spread = std::clamp(value, 0.0f, 1.0f);
    }
}

void PingPongDelay::UpdateDelayLine()
{
    m_delayBufferSize = static_cast<int>(2.0f * static_cast<float>(m_sampleRate)) + 1;  // 2 seconds max
    
    m_delayLineL.resize(m_delayBufferSize, 0.0f);
    m_delayLineR.resize(m_delayBufferSize, 0.0f);
    
    m_delaySamples = static_cast<int>((m_delayTime / 1000.0f) * static_cast<float>(m_sampleRate));
    m_writeIndexL = 0;
    m_writeIndexR = 0;
}

} // namespace RVX::Audio
