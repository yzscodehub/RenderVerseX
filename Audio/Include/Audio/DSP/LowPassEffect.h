/**
 * @file LowPassEffect.h
 * @brief Low-pass filter audio effect
 */

#pragma once

#include "Audio/DSP/IAudioEffect.h"
#include <array>

namespace RVX::Audio
{

/**
 * @brief Low-pass filter effect using biquad filter
 * 
 * Parameters:
 * - cutoff: Cutoff frequency in Hz (20-20000)
 * - resonance: Resonance/Q factor (0.1-10)
 */
class LowPassEffect : public AudioEffectBase
{
public:
    LowPassEffect();
    ~LowPassEffect() override = default;

    // =========================================================================
    // IAudioEffect Interface
    // =========================================================================

    EffectType GetType() const override { return EffectType::LowPass; }
    const char* GetName() const override { return "LowPass"; }

    void Process(float* buffer, uint32 frameCount, uint32 channels) override;
    void Reset() override;

    // =========================================================================
    // Configuration
    // =========================================================================

    void SetSampleRate(uint32 sampleRate) { m_sampleRate = sampleRate; UpdateCoefficients(); }

protected:
    void OnParameterChanged(const std::string& name, float value) override;

private:
    void UpdateCoefficients();

    uint32 m_sampleRate = 44100;
    
    // Filter coefficients
    float m_a0 = 1.0f, m_a1 = 0.0f, m_a2 = 0.0f;
    float m_b1 = 0.0f, m_b2 = 0.0f;

    // Filter state (per channel, up to 8 channels)
    static constexpr int kMaxChannels = 8;
    std::array<float, kMaxChannels> m_z1{};
    std::array<float, kMaxChannels> m_z2{};

    // Parameters
    float m_cutoff = 5000.0f;
    float m_resonance = 0.707f;
};

/**
 * @brief High-pass filter effect using biquad filter
 */
class HighPassEffect : public AudioEffectBase
{
public:
    HighPassEffect();
    ~HighPassEffect() override = default;

    EffectType GetType() const override { return EffectType::HighPass; }
    const char* GetName() const override { return "HighPass"; }

    void Process(float* buffer, uint32 frameCount, uint32 channels) override;
    void Reset() override;

    void SetSampleRate(uint32 sampleRate) { m_sampleRate = sampleRate; UpdateCoefficients(); }

protected:
    void OnParameterChanged(const std::string& name, float value) override;

private:
    void UpdateCoefficients();

    uint32 m_sampleRate = 44100;
    
    float m_a0 = 1.0f, m_a1 = 0.0f, m_a2 = 0.0f;
    float m_b1 = 0.0f, m_b2 = 0.0f;

    static constexpr int kMaxChannels = 8;
    std::array<float, kMaxChannels> m_z1{};
    std::array<float, kMaxChannels> m_z2{};

    float m_cutoff = 200.0f;
    float m_resonance = 0.707f;
};

} // namespace RVX::Audio
