#pragma once

/**
 * @file SightSense.h
 * @brief Visual perception sense
 */

#include "AI/AITypes.h"
#include "Core/Types.h"

#include <vector>
#include <functional>

namespace RVX::AI
{

/**
 * @brief Target data for sight checks
 */
struct SightTarget
{
    uint64 id = 0;
    Vec3 position;
    Vec3 velocity;
    float boundingRadius = 0.5f;
    Affiliation affiliation = Affiliation::Neutral;
    bool isVisible = false;
};

/**
 * @brief Sight sense for visual perception
 * 
 * The SightSense processes visual information based on:
 * - Distance from observer
 * - Angle within field of view
 * - Line of sight (optional obstruction check)
 * 
 * Features:
 * - Configurable FOV and range
 * - Peripheral vision with reduced effectiveness
 * - Hysteresis to prevent rapid gain/loss flickering
 * - Auto-success range for very close targets
 */
class SightSense
{
public:
    // =========================================================================
    // Construction
    // =========================================================================
    SightSense();
    ~SightSense() = default;

    // =========================================================================
    // Configuration
    // =========================================================================

    /**
     * @brief Set sight configuration
     */
    void SetConfig(const SightConfig& config) { m_config = config; }

    /**
     * @brief Get sight configuration
     */
    const SightConfig& GetConfig() const { return m_config; }

    /**
     * @brief Set maximum sight radius
     */
    void SetSightRadius(float radius) { m_config.sightRadius = radius; }

    /**
     * @brief Set sight cone angle (half-angle in degrees)
     */
    void SetSightAngle(float angle) { m_config.sightAngle = angle; }

    /**
     * @brief Set peripheral vision angle (half-angle in degrees)
     */
    void SetPeripheralAngle(float angle) { m_config.peripheralVisionAngle = angle; }

    /**
     * @brief Enable/disable line of sight checks
     */
    void SetRequireLineOfSight(bool require) { m_config.requireLineOfSight = require; }

    // =========================================================================
    // Raycast Callback
    // =========================================================================

    /**
     * @brief Raycast function for line of sight checks
     * Returns true if the ray is blocked
     */
    using RaycastFunction = std::function<bool(const Vec3& start, const Vec3& end, uint64 ignoreId)>;

    /**
     * @brief Set the raycast function
     */
    void SetRaycastFunction(RaycastFunction func) { m_raycastFunc = std::move(func); }

    // =========================================================================
    // Perception
    // =========================================================================

    /**
     * @brief Check if a target can be seen
     * @param observerPos Observer position
     * @param observerForward Observer forward direction
     * @param target Target to check
     * @param outStrength Output perception strength (0-1)
     * @return True if target is visible
     */
    bool CanSee(const Vec3& observerPos, const Vec3& observerForward,
                const SightTarget& target, float& outStrength) const;

    /**
     * @brief Check if a position is in the field of view
     * @param observerPos Observer position
     * @param observerForward Observer forward direction
     * @param targetPos Position to check
     * @param outAngle Output angle from forward (in degrees)
     * @return True if in FOV
     */
    bool IsInFOV(const Vec3& observerPos, const Vec3& observerForward,
                 const Vec3& targetPos, float& outAngle) const;

    /**
     * @brief Check if a position is within sight range
     */
    bool IsInRange(const Vec3& observerPos, const Vec3& targetPos) const;

    /**
     * @brief Check line of sight to a position
     */
    bool HasLineOfSight(const Vec3& observerPos, const Vec3& targetPos,
                        uint64 ignoreId = 0) const;

    /**
     * @brief Calculate perception strength based on distance and angle
     * @param distance Distance to target
     * @param angle Angle from forward (in degrees)
     * @return Perception strength (0-1)
     */
    float CalculateStrength(float distance, float angle) const;

    // =========================================================================
    // Batch Processing
    // =========================================================================

    /**
     * @brief Process multiple targets at once
     * @param observerPos Observer position
     * @param observerForward Observer forward direction
     * @param targets Targets to process
     * @param outStimuli Output stimuli for visible targets
     */
    void ProcessTargets(const Vec3& observerPos, const Vec3& observerForward,
                        const std::vector<SightTarget>& targets,
                        std::vector<PerceptionStimulus>& outStimuli) const;

private:
    SightConfig m_config;
    RaycastFunction m_raycastFunc;

    float GetAngleToTarget(const Vec3& observerPos, const Vec3& observerForward,
                           const Vec3& targetPos) const;
};

} // namespace RVX::AI
