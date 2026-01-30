/**
 * @file CharacterController.cpp
 * @brief CharacterController additional implementations
 * 
 * Note: Most CharacterController methods are inline in the header for performance.
 * This file contains any additional helper functions or non-inline implementations.
 */

#include "Physics/CharacterController.h"
#include "Physics/PhysicsWorld.h"
#include <algorithm>
#include <cmath>

namespace RVX::Physics
{

// Additional CharacterController implementations can be added here if needed.
// Currently, all methods are inline in the header for performance.

/**
 * @brief Helper functions for character controller physics
 */
namespace CharacterPhysics
{

/**
 * @brief Calculate step-up movement
 * 
 * Attempts to move the character over a step by:
 * 1. Moving up by step height
 * 2. Moving forward
 * 3. Moving down to find ground
 */
bool TryStepUp(const Vec3& startPos, const Vec3& moveDir,
               float stepHeight, float radius,
               PhysicsWorld* world, Vec3& outPosition)
{
    if (!world) return false;
    
    // Step 1: Check if we can move up
    ShapeCastHit upHit;
    if (world->SphereCast(startPos, radius, Vec3(0, 1, 0), stepHeight, upHit))
    {
        // Blocked by ceiling
        return false;
    }

    Vec3 elevatedPos = startPos + Vec3(0, stepHeight, 0);

    // Step 2: Try to move forward at elevated position
    float moveLen = length(moveDir);
    if (moveLen < 0.001f) return false;

    Vec3 normalizedMove = moveDir / moveLen;
    ShapeCastHit forwardHit;
    if (world->SphereCast(elevatedPos, radius, normalizedMove, moveLen, forwardHit))
    {
        // Still blocked
        return false;
    }

    Vec3 targetPos = elevatedPos + moveDir;

    // Step 3: Move down to find ground
    ShapeCastHit downHit;
    if (world->SphereCast(targetPos, radius, Vec3(0, -1, 0), 
                          stepHeight * 2.0f, downHit))
    {
        outPosition = targetPos + Vec3(0, -1, 0) * downHit.fraction * stepHeight * 2.0f;
        return true;
    }

    // No ground found after step
    return false;
}

/**
 * @brief Slide along a wall
 */
Vec3 SlideAlongWall(const Vec3& velocity, const Vec3& wallNormal)
{
    // Remove component into wall
    float into = dot(velocity, wallNormal);
    if (into >= 0) return velocity;  // Moving away from wall

    return velocity - wallNormal * into;
}

/**
 * @brief Project velocity onto a surface
 */
Vec3 ProjectOntoSurface(const Vec3& velocity, const Vec3& normal)
{
    return velocity - normal * dot(velocity, normal);
}

/**
 * @brief Calculate ground adhesion force for slopes
 */
Vec3 CalculateGroundAdhesion(const Vec3& groundNormal, const Vec3& gravity,
                              float slopeAngle, float maxSlopeAngle)
{
    if (slopeAngle <= 0.01f || slopeAngle > maxSlopeAngle)
        return Vec3(0);

    // On walkable slope, add slight downward force to prevent bouncing
    float adhesion = std::sin(slopeAngle) * length(gravity) * 0.1f;
    return -groundNormal * adhesion;
}

/**
 * @brief Depenetrate character from geometry
 */
Vec3 Depenetrate(const Vec3& position, float radius, 
                 const Vec3& penetrationNormal, float penetrationDepth)
{
    if (penetrationDepth <= 0) return position;
    
    return position + penetrationNormal * (penetrationDepth + 0.001f);
}

/**
 * @brief Check if a surface is climbable (stairs, slopes)
 */
bool IsClimbable(const Vec3& surfaceNormal, float maxSlopeAngle,
                 float stepHeight, float verticalDistance)
{
    // Calculate surface angle
    float cosAngle = dot(surfaceNormal, Vec3(0, 1, 0));
    float angle = std::acos(std::clamp(cosAngle, -1.0f, 1.0f));

    // If it's within slope limit, it's walkable
    if (angle <= maxSlopeAngle)
        return true;

    // If it's a step (short vertical obstacle)
    if (angle > 1.2f && verticalDistance <= stepHeight)  // > ~70 degrees
        return true;

    return false;
}

/**
 * @brief Smooth character movement input
 */
Vec3 SmoothInput(const Vec3& currentVelocity, const Vec3& targetVelocity,
                 float acceleration, float deltaTime)
{
    Vec3 diff = targetVelocity - currentVelocity;
    float diffLen = length(diff);
    
    if (diffLen < 0.001f)
        return targetVelocity;

    float maxDelta = acceleration * deltaTime;
    if (diffLen <= maxDelta)
        return targetVelocity;

    return currentVelocity + (diff / diffLen) * maxDelta;
}

} // namespace CharacterPhysics

} // namespace RVX::Physics
