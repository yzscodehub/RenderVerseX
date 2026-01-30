/**
 * @file CCD.h
 * @brief Continuous Collision Detection for high-speed objects
 * 
 * Prevents tunneling (objects passing through each other) by:
 * - Conservative advancement
 * - Time of impact (TOI) calculation
 * - Swept shape tests
 */

#pragma once

#include "Physics/PhysicsTypes.h"
#include "Physics/Shapes/CollisionShape.h"
#include <vector>
#include <cmath>
#include <algorithm>

namespace RVX::Physics
{

class RigidBody;
class PhysicsWorld;

/**
 * @brief Time of Impact result
 */
struct TOIResult
{
    bool hit = false;           ///< True if collision will occur
    float toi = 1.0f;           ///< Time of impact (0-1 within timestep)
    Vec3 normal{0, 1, 0};       ///< Collision normal at impact
    Vec3 point{0};              ///< Impact point
    RigidBody* bodyA = nullptr;
    RigidBody* bodyB = nullptr;
};

/**
 * @brief CCD configuration
 */
struct CCDConfig
{
    float velocityThreshold = 1.0f;    ///< Minimum velocity for CCD
    int maxTOIIterations = 20;         ///< Max iterations for TOI
    float toiSlop = 0.005f;            ///< TOI numerical tolerance
    bool enableSpeculative = true;     ///< Use speculative contacts
};

/**
 * @brief Continuous Collision Detection system
 * 
 * Usage:
 * @code
 * CCD ccd(world);
 * 
 * // Check if a body needs CCD
 * if (ccd.NeedsCCD(body))
 * {
 *     TOIResult toi;
 *     if (ccd.ComputeTOI(body, deltaTime, toi))
 *     {
 *         // Advance to TOI and resolve
 *         body->SetPosition(body->GetPosition() + velocity * toi.toi * deltaTime);
 *     }
 * }
 * @endcode
 */
class CCD
{
public:
    CCD() = default;
    explicit CCD(PhysicsWorld* world) : m_world(world) {}

    // =========================================================================
    // Configuration
    // =========================================================================

    void SetConfig(const CCDConfig& config) { m_config = config; }
    const CCDConfig& GetConfig() const { return m_config; }

    /**
     * @brief Set velocity threshold for CCD activation
     */
    void SetVelocityThreshold(float threshold)
    {
        m_config.velocityThreshold = std::max(0.0f, threshold);
    }

    // =========================================================================
    // CCD Queries
    // =========================================================================

    /**
     * @brief Check if a body needs CCD based on its velocity
     */
    bool NeedsCCD(const RigidBody* body) const
    {
        if (!body || !body->IsDynamic())
            return false;

        float speed = length(body->GetLinearVelocity());
        return speed > m_config.velocityThreshold;
    }

    /**
     * @brief Compute time of impact for a moving body
     */
    bool ComputeTOI(RigidBody* body, float deltaTime, TOIResult& result);

    /**
     * @brief Compute time of impact between two moving bodies
     */
    bool ComputeTOI(RigidBody* bodyA, RigidBody* bodyB,
                    float deltaTime, TOIResult& result);

    // =========================================================================
    // Sweep Tests
    // =========================================================================

    /**
     * @brief Sphere sweep test
     */
    bool SweepSphere(const Vec3& start, float radius,
                     const Vec3& displacement,
                     ShapeCastHit& hit, uint32 layerMask = 0xFFFFFFFF);

    /**
     * @brief Capsule sweep test
     */
    bool SweepCapsule(const Vec3& startA, const Vec3& startB, float radius,
                      const Vec3& displacement,
                      ShapeCastHit& hit, uint32 layerMask = 0xFFFFFFFF);

    /**
     * @brief Box sweep test (oriented)
     */
    bool SweepBox(const Vec3& center, const Vec3& halfExtents,
                  const Quat& orientation, const Vec3& displacement,
                  ShapeCastHit& hit, uint32 layerMask = 0xFFFFFFFF);

    // =========================================================================
    // Conservative Advancement
    // =========================================================================

    /**
     * @brief Conservative advancement for two spheres
     */
    static float ConservativeAdvancementSpheres(
        const Vec3& posA, float radiusA, const Vec3& velA,
        const Vec3& posB, float radiusB, const Vec3& velB,
        float maxTime, int maxIterations = 10);

    /**
     * @brief Conservative advancement for sphere vs static geometry
     */
    float ConservativeAdvancementSphereStatic(
        const Vec3& spherePos, float sphereRadius, const Vec3& velocity,
        float maxTime);

private:
    PhysicsWorld* m_world = nullptr;
    CCDConfig m_config;

    /**
     * @brief Root finding for TOI using bisection
     */
    static float BisectionTOI(
        const Vec3& posA, const Vec3& velA, float radiusA,
        const Vec3& posB, const Vec3& velB, float radiusB,
        float maxTime, float tolerance, int maxIterations);

    /**
     * @brief Check overlap at a specific time
     */
    static bool CheckOverlapAtTime(
        const Vec3& posA, const Vec3& velA, float radiusA,
        const Vec3& posB, const Vec3& velB, float radiusB,
        float t);

    /**
     * @brief Compute distance at time t
     */
    static float DistanceAtTime(
        const Vec3& posA, const Vec3& velA, float radiusA,
        const Vec3& posB, const Vec3& velB, float radiusB,
        float t);
};

// =========================================================================
// Implementation
// =========================================================================

inline bool CCD::ComputeTOI(RigidBody* body, float deltaTime, TOIResult& result)
{
    if (!body || !m_world)
        return false;

    result = TOIResult{};

    Vec3 position = body->GetPosition();
    Vec3 velocity = body->GetLinearVelocity();
    Vec3 displacement = velocity * deltaTime;

    if (length(displacement) < 0.001f)
        return false;

    // Get body radius (simplified)
    float radius = 0.5f;  // Would get from shape

    // Sweep test
    ShapeCastHit hit;
    if (SweepSphere(position, radius, displacement, hit))
    {
        result.hit = true;
        result.toi = hit.fraction;
        result.normal = hit.normal;
        result.point = hit.point;
        result.bodyA = body;
        // result.bodyB = world->GetBody(hit.bodyId);
        return true;
    }

    return false;
}

inline bool CCD::ComputeTOI(RigidBody* bodyA, RigidBody* bodyB,
                            float deltaTime, TOIResult& result)
{
    if (!bodyA || !bodyB)
        return false;

    result = TOIResult{};

    Vec3 posA = bodyA->GetPosition();
    Vec3 posB = bodyB->GetPosition();
    Vec3 velA = bodyA->GetLinearVelocity();
    Vec3 velB = bodyB->GetLinearVelocity();

    // Get radii (simplified)
    float radiusA = 0.5f;
    float radiusB = 0.5f;

    // Relative motion
    Vec3 relVel = velA - velB;
    float speed = length(relVel);

    if (speed < 0.001f)
        return false;

    // Use conservative advancement
    float toi = ConservativeAdvancementSpheres(
        posA, radiusA, velA * deltaTime,
        posB, radiusB, velB * deltaTime,
        1.0f, m_config.maxTOIIterations);

    if (toi < 1.0f)
    {
        result.hit = true;
        result.toi = toi;
        result.bodyA = bodyA;
        result.bodyB = bodyB;

        // Compute contact info at TOI
        Vec3 posAAtTOI = posA + velA * deltaTime * toi;
        Vec3 posBAtTOI = posB + velB * deltaTime * toi;
        Vec3 diff = posBAtTOI - posAAtTOI;
        float dist = length(diff);

        if (dist > 0.0001f)
        {
            result.normal = diff / dist;
            result.point = posAAtTOI + result.normal * radiusA;
        }
        else
        {
            result.normal = Vec3(0, 1, 0);
            result.point = posAAtTOI;
        }

        return true;
    }

    return false;
}

inline bool CCD::SweepSphere(const Vec3& start, float radius,
                              const Vec3& displacement,
                              ShapeCastHit& hit, uint32 layerMask)
{
    if (!m_world)
        return false;

    Vec3 direction = normalize(displacement);
    float distance = length(displacement);

    return m_world->SphereCast(start, radius, direction, distance, hit, layerMask);
}

inline bool CCD::SweepCapsule(const Vec3& startA, const Vec3& startB, float radius,
                               const Vec3& displacement,
                               ShapeCastHit& hit, uint32 layerMask)
{
    // Sweep both endpoints and take earliest hit
    ShapeCastHit hitA, hitB;
    bool hasHitA = SweepSphere(startA, radius, displacement, hitA, layerMask);
    bool hasHitB = SweepSphere(startB, radius, displacement, hitB, layerMask);

    if (!hasHitA && !hasHitB)
        return false;

    if (hasHitA && (!hasHitB || hitA.fraction < hitB.fraction))
    {
        hit = hitA;
    }
    else
    {
        hit = hitB;
    }

    return true;
}

inline bool CCD::SweepBox(const Vec3& center, const Vec3& halfExtents,
                           const Quat& orientation, const Vec3& displacement,
                           ShapeCastHit& hit, uint32 layerMask)
{
    // Approximate with bounding sphere
    float boundingRadius = length(halfExtents);
    return SweepSphere(center, boundingRadius, displacement, hit, layerMask);
}

inline float CCD::ConservativeAdvancementSpheres(
    const Vec3& posA, float radiusA, const Vec3& velA,
    const Vec3& posB, float radiusB, const Vec3& velB,
    float maxTime, int maxIterations)
{
    float t = 0.0f;
    float combinedRadius = radiusA + radiusB;

    Vec3 currPosA = posA;
    Vec3 currPosB = posB;

    for (int i = 0; i < maxIterations && t < maxTime; ++i)
    {
        // Current separation
        Vec3 diff = currPosB - currPosA;
        float dist = length(diff);
        float separation = dist - combinedRadius;

        if (separation <= 0.0f)
        {
            // Already overlapping
            return t;
        }

        // Closing velocity
        Vec3 relVel = velA - velB;
        float closingSpeed = -dot(relVel, diff) / dist;

        if (closingSpeed <= 0.0f)
        {
            // Not approaching
            return maxTime;
        }

        // Time to cover remaining separation
        float dt = separation / closingSpeed;
        dt = std::min(dt, maxTime - t);

        // Advance
        t += dt;
        currPosA = posA + velA * t;
        currPosB = posB + velB * t;

        // Check if we've reached contact
        Vec3 newDiff = currPosB - currPosA;
        float newDist = length(newDiff);
        if (newDist <= combinedRadius + 0.001f)
        {
            return t;
        }
    }

    return maxTime;
}

inline float CCD::BisectionTOI(
    const Vec3& posA, const Vec3& velA, float radiusA,
    const Vec3& posB, const Vec3& velB, float radiusB,
    float maxTime, float tolerance, int maxIterations)
{
    float tLow = 0.0f;
    float tHigh = maxTime;

    for (int i = 0; i < maxIterations; ++i)
    {
        float tMid = (tLow + tHigh) * 0.5f;
        float dist = DistanceAtTime(posA, velA, radiusA, posB, velB, radiusB, tMid);

        if (dist <= tolerance)
        {
            tHigh = tMid;
        }
        else
        {
            tLow = tMid;
        }

        if (tHigh - tLow < tolerance)
            break;
    }

    return (tLow + tHigh) * 0.5f;
}

inline bool CCD::CheckOverlapAtTime(
    const Vec3& posA, const Vec3& velA, float radiusA,
    const Vec3& posB, const Vec3& velB, float radiusB,
    float t)
{
    Vec3 pA = posA + velA * t;
    Vec3 pB = posB + velB * t;
    float dist = length(pB - pA);
    return dist <= (radiusA + radiusB);
}

inline float CCD::DistanceAtTime(
    const Vec3& posA, const Vec3& velA, float radiusA,
    const Vec3& posB, const Vec3& velB, float radiusB,
    float t)
{
    Vec3 pA = posA + velA * t;
    Vec3 pB = posB + velB * t;
    float dist = length(pB - pA);
    return dist - (radiusA + radiusB);
}

} // namespace RVX::Physics
