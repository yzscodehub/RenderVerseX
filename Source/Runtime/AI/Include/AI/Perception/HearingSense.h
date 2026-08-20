#pragma once

/**
 * @file HearingSense.h
 * @brief Audio perception sense
 */

#include "AI/AITypes.h"
#include "Core/Types.h"

#include <vector>
#include <string>

namespace RVX::AI
{

/**
 * @brief Noise event data
 */
struct NoiseEvent
{
    Vec3 location;
    float loudness = 1.0f;          ///< Base loudness (0-1)
    float maxRange = 30.0f;         ///< Maximum hearing range
    uint64 sourceId = 0;            ///< Entity that made the noise
    Affiliation affiliation = Affiliation::Neutral;
    std::string tag;                ///< Optional tag for filtering
    float timeStamp = 0.0f;         ///< When the noise was made
};

/**
 * @brief Hearing sense for audio perception
 * 
 * The HearingSense processes audio stimuli based on:
 * - Distance attenuation
 * - Loudness threshold
 * - Optional filtering by source affiliation
 * 
 * Unlike sight, hearing does not require line of sight but may be
 * affected by obstacles in more advanced implementations.
 */
class HearingSense
{
public:
    // =========================================================================
    // Construction
    // =========================================================================
    HearingSense();
    ~HearingSense() = default;

    // =========================================================================
    // Configuration
    // =========================================================================

    /**
     * @brief Set hearing configuration
     */
    void SetConfig(const HearingConfig& config) { m_config = config; }

    /**
     * @brief Get hearing configuration
     */
    const HearingConfig& GetConfig() const { return m_config; }

    /**
     * @brief Set maximum hearing range
     */
    void SetHearingRange(float range) { m_config.hearingRange = range; }

    /**
     * @brief Set minimum loudness threshold
     */
    void SetLoudnessThreshold(float threshold) { m_config.loudnessThreshold = threshold; }

    /**
     * @brief Set whether to only hear hostile targets
     */
    void SetHearEnemiesOnly(bool enemiesOnly) { m_config.hearEnemiesOnly = enemiesOnly; }

    // =========================================================================
    // Perception
    // =========================================================================

    /**
     * @brief Check if a noise can be heard
     * @param listenerPos Listener position
     * @param noise Noise event
     * @param listenerAffiliation Listener's affiliation
     * @param outStrength Output perception strength (0-1)
     * @return True if noise is audible
     */
    bool CanHear(const Vec3& listenerPos, const NoiseEvent& noise,
                 Affiliation listenerAffiliation, float& outStrength) const;

    /**
     * @brief Calculate effective loudness at a distance
     * @param baseLoudness Base loudness of the noise
     * @param distance Distance from noise source
     * @param maxRange Maximum range of the noise
     * @return Perceived loudness (0-1)
     */
    float CalculateLoudness(float baseLoudness, float distance, float maxRange) const;

    /**
     * @brief Check if a position is within hearing range
     */
    bool IsInRange(const Vec3& listenerPos, const Vec3& noisePos) const;

    // =========================================================================
    // Noise Processing
    // =========================================================================

    /**
     * @brief Process a noise event
     * @param listenerPos Listener position
     * @param listenerAffiliation Listener's affiliation
     * @param noise Noise event
     * @param outStimulus Output stimulus if heard
     * @return True if noise was heard
     */
    bool ProcessNoise(const Vec3& listenerPos, Affiliation listenerAffiliation,
                      const NoiseEvent& noise, PerceptionStimulus& outStimulus) const;

    /**
     * @brief Process multiple noise events
     * @param listenerPos Listener position
     * @param listenerAffiliation Listener's affiliation
     * @param noises Noise events to process
     * @param outStimuli Output stimuli for heard noises
     */
    void ProcessNoises(const Vec3& listenerPos, Affiliation listenerAffiliation,
                       const std::vector<NoiseEvent>& noises,
                       std::vector<PerceptionStimulus>& outStimuli) const;

    // =========================================================================
    // Noise Tags
    // =========================================================================

    /**
     * @brief Add a tag to the ignore list
     */
    void IgnoreTag(const std::string& tag) { m_ignoredTags.push_back(tag); }

    /**
     * @brief Remove a tag from the ignore list
     */
    void StopIgnoringTag(const std::string& tag);

    /**
     * @brief Clear the ignore list
     */
    void ClearIgnoredTags() { m_ignoredTags.clear(); }

    /**
     * @brief Check if a tag is ignored
     */
    bool IsTagIgnored(const std::string& tag) const;

private:
    HearingConfig m_config;
    std::vector<std::string> m_ignoredTags;

    bool ShouldIgnoreNoise(const NoiseEvent& noise, Affiliation listenerAffiliation) const;
};

} // namespace RVX::AI
