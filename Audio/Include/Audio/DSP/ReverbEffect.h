/**
 * @file ReverbEffect.h
 * @brief Reverb audio effect
 */

#pragma once

#include "Audio/DSP/IAudioEffect.h"
#include <vector>
#include <array>

namespace RVX::Audio
{

/**
 * @brief Reverb preset
 */
enum class ReverbPreset : uint8
{
    SmallRoom,
    MediumRoom,
    LargeRoom,
    Hall,
    Cathedral,
    Cave,
    Arena,
    Custom
};

/**
 * @brief Reverb effect using Freeverb-style algorithm
 * 
 * Parameters:
 * - roomSize: Size of the room (0-1)
 * - damping: High frequency damping (0-1)
 * - wetLevel: Wet signal level (0-1)
 * - dryLevel: Dry signal level (0-1)
 * - width: Stereo width (0-1)
 * - preDelay: Pre-delay in milliseconds
 */
class ReverbEffect : public AudioEffectBase
{
public:
    ReverbEffect();
    ~ReverbEffect() override = default;

    // =========================================================================
    // IAudioEffect Interface
    // =========================================================================

    EffectType GetType() const override { return EffectType::Reverb; }
    const char* GetName() const override { return "Reverb"; }

    void Process(float* buffer, uint32 frameCount, uint32 channels) override;
    void Reset() override;

    // =========================================================================
    // Preset
    // =========================================================================

    void SetPreset(ReverbPreset preset);
    ReverbPreset GetPreset() const { return m_preset; }

protected:
    void OnParameterChanged(const std::string& name, float value) override;

private:
    static constexpr int kNumCombs = 8;
    static constexpr int kNumAllpass = 4;

    // Comb filter
    struct CombFilter
    {
        std::vector<float> buffer;
        int bufferSize = 0;
        int index = 0;
        float feedback = 0.0f;
        float filterStore = 0.0f;
        float damp1 = 0.0f;
        float damp2 = 0.0f;

        void SetSize(int size);
        float Process(float input);
        void Clear();
    };

    // All-pass filter
    struct AllpassFilter
    {
        std::vector<float> buffer;
        int bufferSize = 0;
        int index = 0;
        float feedback = 0.5f;

        void SetSize(int size);
        float Process(float input);
        void Clear();
    };

    void UpdateParameters();

    ReverbPreset m_preset = ReverbPreset::MediumRoom;
    uint32 m_sampleRate = 44100;

    // Stereo comb filters (left/right)
    std::array<CombFilter, kNumCombs> m_combL;
    std::array<CombFilter, kNumCombs> m_combR;

    // Stereo all-pass filters
    std::array<AllpassFilter, kNumAllpass> m_allpassL;
    std::array<AllpassFilter, kNumAllpass> m_allpassR;

    // Pre-delay buffer
    std::vector<float> m_preDelayBuffer;
    int m_preDelaySize = 0;
    int m_preDelayIndex = 0;

    // Cached parameters
    float m_roomSize = 0.5f;
    float m_damping = 0.5f;
    float m_wetLevel = 0.3f;
    float m_dryLevel = 0.7f;
    float m_width = 1.0f;
};

} // namespace RVX::Audio
