/**
 * @file IAudioEffect.h
 * @brief Base interface for audio DSP effects
 */

#pragma once

#include "Core/Types.h"
#include <algorithm>
#include <string>
#include <unordered_map>

namespace RVX::Audio
{

/**
 * @brief Effect type enumeration
 */
enum class EffectType : uint8
{
    Reverb,
    LowPass,
    HighPass,
    Delay,
    Compressor,
    Limiter,
    EQ,
    Distortion,
    Chorus,
    Flanger,
    Custom
};

/**
 * @brief Base interface for audio DSP effects
 * 
 * Effects can be attached to audio buses to process audio data.
 * Each effect has parameters that can be modified at runtime.
 * 
 * Usage:
 * @code
 * auto reverb = std::make_shared<ReverbEffect>();
 * reverb->SetParameter("roomSize", 0.8f);
 * reverb->SetParameter("damping", 0.5f);
 * 
 * audioMixer->GetBus(BusId::SFX)->AddEffect(reverb);
 * @endcode
 */
class IAudioEffect
{
public:
    virtual ~IAudioEffect() = default;

    // =========================================================================
    // Identification
    // =========================================================================

    /**
     * @brief Get the effect type
     */
    virtual EffectType GetType() const = 0;

    /**
     * @brief Get the effect name
     */
    virtual const char* GetName() const = 0;

    // =========================================================================
    // Processing
    // =========================================================================

    /**
     * @brief Process audio buffer in-place
     * @param buffer Interleaved audio samples (float, -1 to 1)
     * @param frameCount Number of frames (samples per channel)
     * @param channels Number of audio channels
     */
    virtual void Process(float* buffer, uint32 frameCount, uint32 channels) = 0;

    /**
     * @brief Reset effect state (clear delay lines, etc.)
     */
    virtual void Reset() = 0;

    // =========================================================================
    // Parameters
    // =========================================================================

    /**
     * @brief Set a parameter by name
     */
    virtual void SetParameter(const std::string& name, float value) = 0;

    /**
     * @brief Get a parameter by name
     */
    virtual float GetParameter(const std::string& name) const = 0;

    /**
     * @brief Check if a parameter exists
     */
    virtual bool HasParameter(const std::string& name) const = 0;

    // =========================================================================
    // Enable/Disable
    // =========================================================================

    /**
     * @brief Enable/disable the effect
     */
    virtual void SetEnabled(bool enabled) = 0;
    virtual bool IsEnabled() const = 0;

    // =========================================================================
    // Wet/Dry Mix
    // =========================================================================

    /**
     * @brief Set wet/dry mix (0 = all dry, 1 = all wet)
     */
    virtual void SetMix(float mix) = 0;
    virtual float GetMix() const = 0;
};

/**
 * @brief Base implementation with common functionality
 */
class AudioEffectBase : public IAudioEffect
{
public:
    void SetEnabled(bool enabled) override { m_enabled = enabled; }
    bool IsEnabled() const override { return m_enabled; }

    void SetMix(float mix) override { m_mix = std::clamp(mix, 0.0f, 1.0f); }
    float GetMix() const override { return m_mix; }

    void SetParameter(const std::string& name, float value) override
    {
        m_parameters[name] = value;
        OnParameterChanged(name, value);
    }

    float GetParameter(const std::string& name) const override
    {
        auto it = m_parameters.find(name);
        return (it != m_parameters.end()) ? it->second : 0.0f;
    }

    bool HasParameter(const std::string& name) const override
    {
        return m_parameters.find(name) != m_parameters.end();
    }

protected:
    /**
     * @brief Called when a parameter changes
     */
    virtual void OnParameterChanged(const std::string& name, float value) { (void)name; (void)value; }

    /**
     * @brief Apply wet/dry mix to output
     */
    void ApplyMix(float* wet, const float* dry, uint32 sampleCount)
    {
        for (uint32 i = 0; i < sampleCount; ++i)
        {
            wet[i] = dry[i] * (1.0f - m_mix) + wet[i] * m_mix;
        }
    }

    std::unordered_map<std::string, float> m_parameters;
    bool m_enabled = true;
    float m_mix = 1.0f;
};

} // namespace RVX::Audio
