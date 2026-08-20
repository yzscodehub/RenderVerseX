/**
 * @file SightSense.cpp
 * @brief Sight sense implementation
 */

#include "AI/Perception/SightSense.h"

#include <cmath>
#include <algorithm>

namespace RVX::AI
{

// =========================================================================
// Construction
// =========================================================================

SightSense::SightSense() = default;

// =========================================================================
// Perception
// =========================================================================

bool SightSense::CanSee(const Vec3& observerPos, const Vec3& observerForward,
                        const SightTarget& target, float& outStrength) const
{
    outStrength = 0.0f;

    // Check range
    float distance = glm::length(target.position - observerPos);
    if (distance > m_config.sightRadius)
    {
        return false;
    }

    // Auto-success at very close range
    if (distance <= m_config.autoSuccessRange)
    {
        outStrength = 1.0f;
        return true;
    }

    // Check angle
    float angle;
    if (!IsInFOV(observerPos, observerForward, target.position, angle))
    {
        return false;
    }

    // Check line of sight
    if (m_config.requireLineOfSight)
    {
        if (!HasLineOfSight(observerPos, target.position, target.id))
        {
            return false;
        }
    }

    // Calculate strength
    outStrength = CalculateStrength(distance, angle);
    return outStrength > 0.0f;
}

bool SightSense::IsInFOV(const Vec3& observerPos, const Vec3& observerForward,
                         const Vec3& targetPos, float& outAngle) const
{
    outAngle = GetAngleToTarget(observerPos, observerForward, targetPos);
    return outAngle <= m_config.sightAngle;
}

bool SightSense::IsInRange(const Vec3& observerPos, const Vec3& targetPos) const
{
    float distance = glm::length(targetPos - observerPos);
    return distance <= m_config.sightRadius;
}

bool SightSense::HasLineOfSight(const Vec3& observerPos, const Vec3& targetPos,
                                uint64 ignoreId) const
{
    if (!m_raycastFunc)
    {
        return true;  // Assume LOS if no raycast function
    }

    return !m_raycastFunc(observerPos, targetPos, ignoreId);
}

float SightSense::CalculateStrength(float distance, float angle) const
{
    // Distance falloff (linear)
    float distanceFactor = 1.0f - (distance / m_config.sightRadius);
    distanceFactor = std::max(0.0f, distanceFactor);

    // Angle falloff
    float angleFactor = 1.0f;
    
    if (angle > m_config.peripheralVisionAngle)
    {
        // In peripheral vision - reduced effectiveness
        float peripheralRange = m_config.sightAngle - m_config.peripheralVisionAngle;
        if (peripheralRange > 0.0f)
        {
            float peripheralProgress = (angle - m_config.peripheralVisionAngle) / peripheralRange;
            angleFactor = 1.0f - peripheralProgress * 0.5f;  // 50% reduction at edge
        }
    }

    return distanceFactor * angleFactor;
}

void SightSense::ProcessTargets(const Vec3& observerPos, const Vec3& observerForward,
                                const std::vector<SightTarget>& targets,
                                std::vector<PerceptionStimulus>& outStimuli) const
{
    outStimuli.clear();
    outStimuli.reserve(targets.size() / 2);  // Estimate half are visible

    for (const auto& target : targets)
    {
        float strength;
        if (CanSee(observerPos, observerForward, target, strength))
        {
            PerceptionStimulus stimulus;
            stimulus.sense = SenseType::Sight;
            stimulus.location = target.position;
            stimulus.direction = glm::normalize(target.position - observerPos);
            stimulus.strength = strength;
            stimulus.sourceId = target.id;
            stimulus.affiliation = target.affiliation;
            stimulus.isActive = true;

            outStimuli.push_back(stimulus);
        }
    }
}

// =========================================================================
// Internal Methods
// =========================================================================

float SightSense::GetAngleToTarget(const Vec3& observerPos, const Vec3& observerForward,
                                   const Vec3& targetPos) const
{
    Vec3 toTarget = targetPos - observerPos;
    toTarget.y = 0.0f;  // Project to horizontal plane
    
    float length = glm::length(toTarget);
    if (length < 0.001f)
    {
        return 0.0f;  // Target at same position
    }

    toTarget = glm::normalize(toTarget);

    Vec3 forward = observerForward;
    forward.y = 0.0f;
    forward = glm::normalize(forward);

    float dot = glm::clamp(glm::dot(forward, toTarget), -1.0f, 1.0f);
    float angle = glm::degrees(std::acos(dot));

    return angle;
}

} // namespace RVX::AI
