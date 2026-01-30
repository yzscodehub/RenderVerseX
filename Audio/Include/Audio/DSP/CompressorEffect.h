/**
 * @file CompressorEffect.h
 * @brief Dynamics compressor audio effect
 */

#pragma once

#include "Audio/DSP/IAudioEffect.h"
#include <array>

namespace RVX::Audio
{

/**
 * @brief Dynamics compressor effect
 * 
 * Reduces dynamic range by attenuating signals above a threshold.
 * 
 * Parameters:
 * - threshold: Threshold in dB (-60 to 0)
 * - ratio: Compression ratio (1:1 to 20:1)
 * - attack: Attack time in ms (0.1 to 100)
 * - release: Release time in ms (10 to 1000)
 * - makeupGain: Makeup gain in dB (0 to 30)
 * - knee: Soft knee width in dB (0 to 20)
 */
class CompressorEffect : public AudioEffectBase
{
public:
    CompressorEffect();
    ~CompressorEffect() override = default;

    // =========================================================================
    // IAudioEffect Interface
    // =========================================================================

    EffectType GetType() const override { return EffectType::Compressor; }
    const char* GetName() const override { return "Compressor"; }

    void Process(float* buffer, uint32 frameCount, uint32 channels) override;
    void Reset() override;

    // =========================================================================
    // Configuration
    // =========================================================================

    void SetSampleRate(uint32 sampleRate);

    // =========================================================================
    // Metering
    // =========================================================================

    /**
     * @brief Get current gain reduction in dB
     */
    float GetGainReduction() const { return m_gainReduction; }

protected:
    void OnParameterChanged(const std::string& name, float value) override;

private:
    void UpdateCoefficients();

    // Convert between linear and dB
    static float LinearToDb(float linear);
    static float DbToLinear(float db);

    uint32 m_sampleRate = 44100;

    // Parameters
    float m_threshold = -20.0f;  // dB
    float m_ratio = 4.0f;
    float m_attack = 10.0f;      // ms
    float m_release = 100.0f;    // ms
    float m_makeupGain = 0.0f;   // dB
    float m_knee = 0.0f;         // dB

    // Coefficients
    float m_attackCoef = 0.0f;
    float m_releaseCoef = 0.0f;
    float m_makeupLinear = 1.0f;

    // State
    float m_envelope = 0.0f;
    float m_gainReduction = 0.0f;
};

/**
 * @brief Limiter effect
 * 
 * Hard limiter that prevents signal from exceeding a threshold.
 */
class LimiterEffect : public AudioEffectBase
{
public:
    LimiterEffect();
    ~LimiterEffect() override = default;

    EffectType GetType() const override { return EffectType::Limiter; }
    const char* GetName() const override { return "Limiter"; }

    void Process(float* buffer, uint32 frameCount, uint32 channels) override;
    void Reset() override;

    void SetSampleRate(uint32 sampleRate);

    float GetGainReduction() const { return m_gainReduction; }

protected:
    void OnParameterChanged(const std::string& name, float value) override;

private:
    void UpdateCoefficients();

    uint32 m_sampleRate = 44100;

    float m_threshold = -0.1f;  // dB
    float m_release = 100.0f;   // ms

    float m_releaseCoef = 0.0f;
    float m_thresholdLinear = 1.0f;

    float m_envelope = 0.0f;
    float m_gainReduction = 0.0f;
};

} // namespace RVX::Audio
