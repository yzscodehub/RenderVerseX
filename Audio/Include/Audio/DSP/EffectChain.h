/**
 * @file EffectChain.h
 * @brief Chain of audio effects for sequential processing
 */

#pragma once

#include "Audio/DSP/IAudioEffect.h"
#include <vector>
#include <memory>

namespace RVX::Audio
{

/**
 * @brief Chain of audio effects for sequential processing
 * 
 * Processes audio through multiple effects in order.
 * Each effect's output is the next effect's input.
 * 
 * Usage:
 * @code
 * EffectChain chain;
 * chain.AddEffect(std::make_shared<LowPassEffect>());
 * chain.AddEffect(std::make_shared<ReverbEffect>());
 * chain.AddEffect(std::make_shared<CompressorEffect>());
 * 
 * chain.Process(buffer, frameCount, channels);
 * @endcode
 */
class EffectChain
{
public:
    EffectChain() = default;
    ~EffectChain() = default;

    // =========================================================================
    // Effect Management
    // =========================================================================

    /**
     * @brief Add an effect to the end of the chain
     */
    void AddEffect(std::shared_ptr<IAudioEffect> effect);

    /**
     * @brief Insert an effect at a specific position
     */
    void InsertEffect(size_t index, std::shared_ptr<IAudioEffect> effect);

    /**
     * @brief Remove an effect by index
     */
    void RemoveEffect(size_t index);

    /**
     * @brief Remove an effect by pointer
     */
    void RemoveEffect(IAudioEffect* effect);

    /**
     * @brief Clear all effects
     */
    void Clear();

    /**
     * @brief Get the number of effects in the chain
     */
    size_t GetEffectCount() const { return m_effects.size(); }

    /**
     * @brief Get an effect by index
     */
    IAudioEffect* GetEffect(size_t index);
    const IAudioEffect* GetEffect(size_t index) const;

    /**
     * @brief Get all effects
     */
    const std::vector<std::shared_ptr<IAudioEffect>>& GetEffects() const { return m_effects; }

    // =========================================================================
    // Processing
    // =========================================================================

    /**
     * @brief Process audio through all effects
     */
    void Process(float* buffer, uint32 frameCount, uint32 channels);

    /**
     * @brief Reset all effects
     */
    void Reset();

    // =========================================================================
    // Enable/Disable
    // =========================================================================

    /**
     * @brief Enable/disable the entire chain
     */
    void SetEnabled(bool enabled) { m_enabled = enabled; }
    bool IsEnabled() const { return m_enabled; }

    /**
     * @brief Set bypass (pass through without processing)
     */
    void SetBypassed(bool bypassed) { m_bypassed = bypassed; }
    bool IsBypassed() const { return m_bypassed; }

private:
    std::vector<std::shared_ptr<IAudioEffect>> m_effects;
    bool m_enabled = true;
    bool m_bypassed = false;
};

} // namespace RVX::Audio
