/**
 * @file ShapeQuery.cpp
 * @brief Shape-based spatial queries (overlap, sweep, etc.)
 */

#include "Physics/PhysicsTypes.h"
#include "Physics/RigidBody.h"
#include "Physics/Shapes/CollisionShape.h"
#include <vector>
#include <cmath>
#include <algorithm>

namespace RVX::Physics
{

/**
 * @brief Shape query system for spatial queries
 * 
 * Provides:
 * - Sphere/box/capsule overlap queries
 * - Shape sweep tests (for CCD)
 * - Point containment tests
 */
class ShapeQuery
{
public:
    // =========================================================================
    // Overlap Queries
    // =========================================================================

    /**
     * @brief Find all bodies overlapping a sphere
     */
    static size_t OverlapSphere(const Vec3& center, float radius,
                                const std::vector<RigidBody*>& bodies,
                                std::vector<RigidBody*>& outBodies,
                                uint32 layerMask = 0xFFFFFFFF)
    {
        outBodies.clear();

        for (auto* body : bodies)
        {
            if (!body) continue;
            
            // Layer check
            if ((body->GetLayer() & layerMask) == 0)
                continue;

            // Simple distance check (would need shape-specific tests for accuracy)
            Vec3 toBody = body->GetPosition() - center;
            float dist = length(toBody);

            // Estimate body radius from shapes
            float bodyRadius = 1.0f;  // Default estimate
            // Would iterate body shapes here

            if (dist < radius + bodyRadius)
            {
                outBodies.push_back(body);
            }
        }

        return outBodies.size();
    }

    /**
     * @brief Find all bodies overlapping a box
     */
    static size_t OverlapBox(const Vec3& center, const Vec3& halfExtents,
                             const Quat& rotation,
                             const std::vector<RigidBody*>& bodies,
                             std::vector<RigidBody*>& outBodies,
                             uint32 layerMask = 0xFFFFFFFF)
    {
        outBodies.clear();

        // Compute box AABB (conservative)
        Mat3 rotMat = mat3_cast(rotation);
        Vec3 absExtents(
            std::abs(rotMat[0][0]) * halfExtents.x + 
            std::abs(rotMat[1][0]) * halfExtents.y + 
            std::abs(rotMat[2][0]) * halfExtents.z,
            std::abs(rotMat[0][1]) * halfExtents.x + 
            std::abs(rotMat[1][1]) * halfExtents.y + 
            std::abs(rotMat[2][1]) * halfExtents.z,
            std::abs(rotMat[0][2]) * halfExtents.x + 
            std::abs(rotMat[1][2]) * halfExtents.y + 
            std::abs(rotMat[2][2]) * halfExtents.z
        );

        Vec3 boxMin = center - absExtents;
        Vec3 boxMax = center + absExtents;

        for (auto* body : bodies)
        {
            if (!body) continue;

            if ((body->GetLayer() & layerMask) == 0)
                continue;

            // AABB overlap for now
            Vec3 bodyMin, bodyMax;
            GetBodyAABB(*body, bodyMin, bodyMax);

            if (AABBOverlap(boxMin, boxMax, bodyMin, bodyMax))
            {
                outBodies.push_back(body);
            }
        }

        return outBodies.size();
    }

    /**
     * @brief Find all bodies overlapping a capsule
     */
    static size_t OverlapCapsule(const Vec3& pointA, const Vec3& pointB,
                                  float radius,
                                  const std::vector<RigidBody*>& bodies,
                                  std::vector<RigidBody*>& outBodies,
                                  uint32 layerMask = 0xFFFFFFFF)
    {
        outBodies.clear();

        // Capsule AABB
        Vec3 capsuleMin = glm::min(pointA, pointB) - Vec3(radius);
        Vec3 capsuleMax = glm::max(pointA, pointB) + Vec3(radius);

        for (auto* body : bodies)
        {
            if (!body) continue;

            if ((body->GetLayer() & layerMask) == 0)
                continue;

            Vec3 bodyMin, bodyMax;
            GetBodyAABB(*body, bodyMin, bodyMax);

            if (AABBOverlap(capsuleMin, capsuleMax, bodyMin, bodyMax))
            {
                // More precise check would go here
                outBodies.push_back(body);
            }
        }

        return outBodies.size();
    }

    // =========================================================================
    // Sweep Tests (CCD)
    // =========================================================================

    /**
     * @brief Sweep a sphere through the world
     */
    static bool SweepSphere(const Vec3& start, float radius,
                            const Vec3& direction, float maxDistance,
                            const std::vector<RigidBody*>& bodies,
                            ShapeCastHit& outHit,
                            uint32 layerMask = 0xFFFFFFFF)
    {
        outHit = ShapeCastHit{};
        outHit.fraction = 1.0f;
        bool hit = false;

        Vec3 normalizedDir = length(direction) > 0.001f 
            ? normalize(direction) : Vec3(0, 0, 1);

        for (auto* body : bodies)
        {
            if (!body) continue;

            if ((body->GetLayer() & layerMask) == 0)
                continue;

            // Simple sphere sweep against body center
            Vec3 bodyPos = body->GetPosition();
            float bodyRadius = 1.0f;  // Estimate

            // Sphere-sphere sweep
            float hitFraction;
            if (SphereSweepSphere(start, radius, normalizedDir, maxDistance,
                                  bodyPos, bodyRadius, hitFraction))
            {
                if (hitFraction < outHit.fraction)
                {
                    outHit.hit = true;
                    outHit.fraction = hitFraction;
                    outHit.point = start + normalizedDir * (hitFraction * maxDistance);
                    outHit.normal = normalize(outHit.point - bodyPos);
                    outHit.bodyId = body->GetId();
                    hit = true;
                }
            }
        }

        return hit;
    }

    /**
     * @brief Sweep a box through the world
     */
    static bool SweepBox(const Vec3& start, const Vec3& halfExtents,
                         const Quat& rotation,
                         const Vec3& direction, float maxDistance,
                         const std::vector<RigidBody*>& bodies,
                         ShapeCastHit& outHit,
                         uint32 layerMask = 0xFFFFFFFF)
    {
        // Use sphere sweep with bounding radius for now
        float boundingRadius = length(halfExtents);
        return SweepSphere(start, boundingRadius, direction, maxDistance,
                           bodies, outHit, layerMask);
    }

    // =========================================================================
    // Point Queries
    // =========================================================================

    /**
     * @brief Check if a point is inside any body
     */
    static RigidBody* PointQuery(const Vec3& point,
                                  const std::vector<RigidBody*>& bodies,
                                  uint32 layerMask = 0xFFFFFFFF)
    {
        for (auto* body : bodies)
        {
            if (!body) continue;

            if ((body->GetLayer() & layerMask) == 0)
                continue;

            Vec3 bodyMin, bodyMax;
            GetBodyAABB(*body, bodyMin, bodyMax);

            if (PointInAABB(point, bodyMin, bodyMax))
            {
                // More precise shape check would go here
                return body;
            }
        }

        return nullptr;
    }

    /**
     * @brief Get closest point on a body to a given point
     */
    static Vec3 ClosestPoint(const Vec3& point, RigidBody* body)
    {
        if (!body) return point;

        // Simplified: clamp to body AABB
        Vec3 bodyMin, bodyMax;
        GetBodyAABB(*body, bodyMin, bodyMax);

        return Vec3(
            std::clamp(point.x, bodyMin.x, bodyMax.x),
            std::clamp(point.y, bodyMin.y, bodyMax.y),
            std::clamp(point.z, bodyMin.z, bodyMax.z)
        );
    }

private:
    static void GetBodyAABB(const RigidBody& body, Vec3& outMin, Vec3& outMax)
    {
        Vec3 pos = body.GetPosition();
        float radius = 1.0f;  // Default estimate

        outMin = pos - Vec3(radius);
        outMax = pos + Vec3(radius);
    }

    static bool AABBOverlap(const Vec3& minA, const Vec3& maxA,
                            const Vec3& minB, const Vec3& maxB)
    {
        return (minA.x <= maxB.x && maxA.x >= minB.x) &&
               (minA.y <= maxB.y && maxA.y >= minB.y) &&
               (minA.z <= maxB.z && maxA.z >= minB.z);
    }

    static bool PointInAABB(const Vec3& point, const Vec3& min, const Vec3& max)
    {
        return point.x >= min.x && point.x <= max.x &&
               point.y >= min.y && point.y <= max.y &&
               point.z >= min.z && point.z <= max.z;
    }

    static bool SphereSweepSphere(const Vec3& start, float radiusA,
                                   const Vec3& direction, float maxDistance,
                                   const Vec3& center, float radiusB,
                                   float& outFraction)
    {
        // Treat as ray vs expanded sphere
        float combinedRadius = radiusA + radiusB;
        
        Vec3 oc = start - center;
        float a = dot(direction, direction);
        float b = 2.0f * dot(oc, direction);
        float c = dot(oc, oc) - combinedRadius * combinedRadius;
        
        float discriminant = b * b - 4.0f * a * c;
        if (discriminant < 0)
            return false;

        float sqrtD = std::sqrt(discriminant);
        float t = (-b - sqrtD) / (2.0f * a);

        if (t < 0)
            t = (-b + sqrtD) / (2.0f * a);

        if (t < 0 || t > maxDistance)
            return false;

        outFraction = t / maxDistance;
        return true;
    }
};

} // namespace RVX::Physics
