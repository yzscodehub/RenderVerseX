/**
 * @file HearingSense.cpp
 * @brief Hearing sense implementation
 */

#include "AI/Perception/HearingSense.h"

#include <cmath>
#include <algorithm>

namespace RVX::AI
{

// =========================================================================
// Construction
// =========================================================================

HearingSense::HearingSense() = default;

// =========================================================================
// Perception
// =========================================================================

bool HearingSense::CanHear(const Vec3& listenerPos, const NoiseEvent& noise,
                           Affiliation listenerAffiliation, float& outStrength) const
{
    outStrength = 0.0f;

    // Check if should ignore this noise
    if (ShouldIgnoreNoise(noise, listenerAffiliation))
    {
        return false;
    }

    // Check range
    float distance = glm::length(noise.location - listenerPos);
    float effectiveRange = std::min(m_config.hearingRange, noise.maxRange);
    
    if (distance > effectiveRange)
    {
        return false;
    }

    // Calculate loudness at this distance
    outStrength = CalculateLoudness(noise.loudness, distance, effectiveRange);

    // Check threshold
    if (outStrength < m_config.loudnessThreshold)
    {
        return false;
    }

    return true;
}

float HearingSense::CalculateLoudness(float baseLoudness, float distance, 
                                      float maxRange) const
{
    if (distance <= 0.0f)
    {
        return baseLoudness;
    }

    if (distance >= maxRange)
    {
        return 0.0f;
    }

    // Inverse square falloff with minimum
    // Using a modified formula that doesn't drop to 0 immediately
    float normalizedDist = distance / maxRange;
    
    // Smooth falloff curve
    float falloff = 1.0f - normalizedDist * normalizedDist;
    falloff = std::max(0.0f, falloff);

    return baseLoudness * falloff;
}

bool HearingSense::IsInRange(const Vec3& listenerPos, const Vec3& noisePos) const
{
    float distance = glm::length(noisePos - listenerPos);
    return distance <= m_config.hearingRange;
}

bool HearingSense::ProcessNoise(const Vec3& listenerPos, Affiliation listenerAffiliation,
                                const NoiseEvent& noise, PerceptionStimulus& outStimulus) const
{
    float strength;
    if (!CanHear(listenerPos, noise, listenerAffiliation, strength))
    {
        return false;
    }

    outStimulus.sense = SenseType::Hearing;
    outStimulus.location = noise.location;
    outStimulus.direction = glm::normalize(noise.location - listenerPos);
    outStimulus.strength = strength;
    outStimulus.sourceId = noise.sourceId;
    outStimulus.affiliation = noise.affiliation;
    outStimulus.tag = noise.tag;
    outStimulus.isActive = true;

    return true;
}

void HearingSense::ProcessNoises(const Vec3& listenerPos, Affiliation listenerAffiliation,
                                 const std::vector<NoiseEvent>& noises,
                                 std::vector<PerceptionStimulus>& outStimuli) const
{
    outStimuli.clear();
    outStimuli.reserve(noises.size() / 2);

    for (const auto& noise : noises)
    {
        PerceptionStimulus stimulus;
        if (ProcessNoise(listenerPos, listenerAffiliation, noise, stimulus))
        {
            outStimuli.push_back(stimulus);
        }
    }
}

// =========================================================================
// Noise Tags
// =========================================================================

void HearingSense::StopIgnoringTag(const std::string& tag)
{
    auto it = std::find(m_ignoredTags.begin(), m_ignoredTags.end(), tag);
    if (it != m_ignoredTags.end())
    {
        m_ignoredTags.erase(it);
    }
}

bool HearingSense::IsTagIgnored(const std::string& tag) const
{
    return std::find(m_ignoredTags.begin(), m_ignoredTags.end(), tag) != 
           m_ignoredTags.end();
}

// =========================================================================
// Internal Methods
// =========================================================================

bool HearingSense::ShouldIgnoreNoise(const NoiseEvent& noise, 
                                     Affiliation listenerAffiliation) const
{
    // Check tag filter
    if (!noise.tag.empty() && IsTagIgnored(noise.tag))
    {
        return true;
    }

    // Check affiliation filter
    if (m_config.hearEnemiesOnly)
    {
        // Only hear noises from hostile targets
        if (listenerAffiliation == Affiliation::Friendly)
        {
            // Friendly listener only hears hostile noises
            if (noise.affiliation != Affiliation::Hostile)
            {
                return true;
            }
        }
        else if (listenerAffiliation == Affiliation::Hostile)
        {
            // Hostile listener only hears friendly noises
            if (noise.affiliation != Affiliation::Friendly)
            {
                return true;
            }
        }
    }

    return false;
}

} // namespace RVX::AI
