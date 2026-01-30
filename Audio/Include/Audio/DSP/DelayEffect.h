/**
 * @file DelayEffect.h
 * @brief Delay audio effect
 */

#pragma once

#include "Audio/DSP/IAudioEffect.h"
#include <vector>
#include <array>

namespace RVX::Audio
{

/**
 * @brief Delay effect with feedback
 * 
 * Parameters:
 * - delayTime: Delay time in milliseconds (0-2000)
 * - feedback: Feedback amount (0-1)
 * - wetLevel: Wet signal level (0-1)
 */
class DelayEffect : public AudioEffectBase
{
public:
    DelayEffect();
    ~DelayEffect() override = default;

    // =========================================================================
    // IAudioEffect Interface
    // =========================================================================

    EffectType GetType() const override { return EffectType::Delay; }
    const char* GetName() const override { return "Delay"; }

    void Process(float* buffer, uint32 frameCount, uint32 channels) override;
    void Reset() override;

    // =========================================================================
    // Configuration
    // =========================================================================

    void SetSampleRate(uint32 sampleRate);
    void SetMaxDelayMs(float maxDelayMs);

protected:
    void OnParameterChanged(const std::string& name, float value) override;

private:
    void UpdateDelayLine();

    uint32 m_sampleRate = 44100;
    float m_maxDelayMs = 2000.0f;

    // Stereo delay lines
    static constexpr int kMaxChannels = 8;
    std::array<std::vector<float>, kMaxChannels> m_delayLines;
    std::array<int, kMaxChannels> m_writeIndex{};
    int m_delayBufferSize = 0;
    int m_delaySamples = 0;

    // Parameters
    float m_delayTime = 250.0f;  // ms
    float m_feedback = 0.5f;
    float m_wetLevel = 0.5f;
};

/**
 * @brief Ping-pong delay effect
 * 
 * Creates a stereo delay that bounces between left and right channels.
 */
class PingPongDelay : public AudioEffectBase
{
public:
    PingPongDelay();
    ~PingPongDelay() override = default;

    EffectType GetType() const override { return EffectType::Delay; }
    const char* GetName() const override { return "PingPongDelay"; }

    void Process(float* buffer, uint32 frameCount, uint32 channels) override;
    void Reset() override;

    void SetSampleRate(uint32 sampleRate);

protected:
    void OnParameterChanged(const std::string& name, float value) override;

private:
    void UpdateDelayLine();

    uint32 m_sampleRate = 44100;
    
    std::vector<float> m_delayLineL;
    std::vector<float> m_delayLineR;
    int m_writeIndexL = 0;
    int m_writeIndexR = 0;
    int m_delayBufferSize = 0;
    int m_delaySamples = 0;

    float m_delayTime = 250.0f;
    float m_feedback = 0.5f;
    float m_wetLevel = 0.5f;
    float m_spread = 0.5f;  // Stereo spread
};

} // namespace RVX::Audio
