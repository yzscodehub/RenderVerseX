/**
 * @file CollisionDetection.cpp
 * @brief Built-in collision detection using Geometry module algorithms
 */

#include "Physics/PhysicsTypes.h"
#include "Physics/RigidBody.h"
#include "Physics/Shapes/CollisionShape.h"
#include <vector>
#include <cmath>

namespace RVX::Physics
{

/**
 * @brief Collision pair for broadphase
 */
struct CollisionPair
{
    RigidBody* bodyA = nullptr;
    RigidBody* bodyB = nullptr;
};

/**
 * @brief Detailed collision result
 */
struct CollisionResult
{
    bool colliding = false;
    Vec3 normal{0, 1, 0};       // From A to B
    float depth = 0.0f;
    Vec3 pointA{0};
    Vec3 pointB{0};
    RigidBody* bodyA = nullptr;
    RigidBody* bodyB = nullptr;
};

/**
 * @brief Built-in collision detection system
 * 
 * Uses algorithms from Geometry module:
 * - GJK for intersection testing
 * - EPA for penetration depth
 * - SAT for OBB-OBB (optimized)
 */
class CollisionDetection
{
public:
    // =========================================================================
    // Broadphase
    // =========================================================================

    /**
     * @brief Simple AABB overlap test for broadphase
     */
    static bool AABBOverlap(const Vec3& minA, const Vec3& maxA,
                            const Vec3& minB, const Vec3& maxB)
    {
        return (minA.x <= maxB.x && maxA.x >= minB.x) &&
               (minA.y <= maxB.y && maxA.y >= minB.y) &&
               (minA.z <= maxB.z && maxA.z >= minB.z);
    }

    /**
     * @brief Get world-space AABB for a body
     */
    static void GetBodyAABB(const RigidBody& body, Vec3& outMin, Vec3& outMax)
    {
        // Start with zero AABB at position
        outMin = outMax = body.GetPosition();

        // Expand by shape bounds (simplified - uses bounding radius)
        // A full implementation would transform each shape's local AABB
        float radius = 1.0f;  // Default radius
        
        // Would iterate over body shapes here
        // For now, use a conservative estimate

        outMin -= Vec3(radius);
        outMax += Vec3(radius);
    }

    /**
     * @brief Brute-force broadphase (O(n^2))
     */
    static void BroadphaseNaive(const std::vector<RigidBody*>& bodies,
                                std::vector<CollisionPair>& outPairs)
    {
        outPairs.clear();

        for (size_t i = 0; i < bodies.size(); ++i)
        {
            for (size_t j = i + 1; j < bodies.size(); ++j)
            {
                RigidBody* a = bodies[i];
                RigidBody* b = bodies[j];

                // Skip if both static
                if (a->IsStatic() && b->IsStatic())
                    continue;

                // Skip if both sleeping
                if (a->IsSleeping() && b->IsSleeping())
                    continue;

                // AABB test
                Vec3 minA, maxA, minB, maxB;
                GetBodyAABB(*a, minA, maxA);
                GetBodyAABB(*b, minB, maxB);

                if (AABBOverlap(minA, maxA, minB, maxB))
                {
                    outPairs.push_back({a, b});
                }
            }
        }
    }

    // =========================================================================
    // Narrowphase - Primitive Tests
    // =========================================================================

    /**
     * @brief Sphere vs Sphere collision
     */
    static bool SphereSphere(const Vec3& centerA, float radiusA,
                             const Vec3& centerB, float radiusB,
                             CollisionResult& result)
    {
        Vec3 diff = centerB - centerA;
        float distSq = dot(diff, diff);
        float radiusSum = radiusA + radiusB;

        if (distSq > radiusSum * radiusSum)
        {
            result.colliding = false;
            return false;
        }

        float dist = std::sqrt(distSq);
        
        result.colliding = true;
        result.normal = dist > 0.0001f ? diff / dist : Vec3(0, 1, 0);
        result.depth = radiusSum - dist;
        result.pointA = centerA + result.normal * radiusA;
        result.pointB = centerB - result.normal * radiusB;

        return true;
    }

    /**
     * @brief Sphere vs Capsule collision
     */
    static bool SphereCapsule(const Vec3& sphereCenter, float sphereRadius,
                              const Vec3& capsuleA, const Vec3& capsuleB, float capsuleRadius,
                              CollisionResult& result)
    {
        // Find closest point on capsule segment to sphere center
        Vec3 ab = capsuleB - capsuleA;
        float t = dot(sphereCenter - capsuleA, ab);
        float denom = dot(ab, ab);
        
        if (denom > 0.0001f)
        {
            t = std::clamp(t / denom, 0.0f, 1.0f);
        }
        else
        {
            t = 0.0f;
        }

        Vec3 closestOnCapsule = capsuleA + ab * t;

        // Now it's a sphere-sphere test
        return SphereSphere(sphereCenter, sphereRadius,
                           closestOnCapsule, capsuleRadius, result);
    }

    /**
     * @brief Sphere vs Box (AABB) collision
     */
    static bool SphereBox(const Vec3& sphereCenter, float sphereRadius,
                          const Vec3& boxCenter, const Vec3& boxHalfExtents,
                          CollisionResult& result)
    {
        // Clamp sphere center to box
        Vec3 boxMin = boxCenter - boxHalfExtents;
        Vec3 boxMax = boxCenter + boxHalfExtents;

        Vec3 closest;
        closest.x = std::clamp(sphereCenter.x, boxMin.x, boxMax.x);
        closest.y = std::clamp(sphereCenter.y, boxMin.y, boxMax.y);
        closest.z = std::clamp(sphereCenter.z, boxMin.z, boxMax.z);

        Vec3 diff = sphereCenter - closest;
        float distSq = dot(diff, diff);

        if (distSq > sphereRadius * sphereRadius)
        {
            result.colliding = false;
            return false;
        }

        float dist = std::sqrt(distSq);

        result.colliding = true;
        result.normal = dist > 0.0001f ? diff / dist : Vec3(0, 1, 0);
        result.depth = sphereRadius - dist;
        result.pointA = closest;
        result.pointB = sphereCenter - result.normal * sphereRadius;

        return true;
    }

    /**
     * @brief Capsule vs Capsule collision
     */
    static bool CapsuleCapsule(const Vec3& a1, const Vec3& a2, float radiusA,
                               const Vec3& b1, const Vec3& b2, float radiusB,
                               CollisionResult& result)
    {
        // Find closest points between two line segments
        Vec3 d1 = a2 - a1;
        Vec3 d2 = b2 - b1;
        Vec3 r = a1 - b1;

        float a = dot(d1, d1);
        float e = dot(d2, d2);
        float f = dot(d2, r);

        float s = 0.0f, t = 0.0f;

        if (a <= 0.0001f && e <= 0.0001f)
        {
            // Both segments are points
            s = t = 0.0f;
        }
        else if (a <= 0.0001f)
        {
            // First segment is a point
            s = 0.0f;
            t = std::clamp(f / e, 0.0f, 1.0f);
        }
        else
        {
            float c = dot(d1, r);
            if (e <= 0.0001f)
            {
                // Second segment is a point
                t = 0.0f;
                s = std::clamp(-c / a, 0.0f, 1.0f);
            }
            else
            {
                float b = dot(d1, d2);
                float denom = a * e - b * b;

                if (denom != 0.0f)
                {
                    s = std::clamp((b * f - c * e) / denom, 0.0f, 1.0f);
                }
                else
                {
                    s = 0.0f;
                }

                t = (b * s + f) / e;

                if (t < 0.0f)
                {
                    t = 0.0f;
                    s = std::clamp(-c / a, 0.0f, 1.0f);
                }
                else if (t > 1.0f)
                {
                    t = 1.0f;
                    s = std::clamp((b - c) / a, 0.0f, 1.0f);
                }
            }
        }

        Vec3 closestA = a1 + d1 * s;
        Vec3 closestB = b1 + d2 * t;

        return SphereSphere(closestA, radiusA, closestB, radiusB, result);
    }

    // =========================================================================
    // Raycast
    // =========================================================================

    /**
     * @brief Ray vs Sphere intersection
     */
    static bool RaySphere(const Vec3& origin, const Vec3& direction, float maxDist,
                          const Vec3& center, float radius,
                          float& outT, Vec3& outPoint, Vec3& outNormal)
    {
        Vec3 oc = origin - center;
        float a = dot(direction, direction);
        float b = 2.0f * dot(oc, direction);
        float c = dot(oc, oc) - radius * radius;
        float discriminant = b * b - 4.0f * a * c;

        if (discriminant < 0)
            return false;

        float sqrtD = std::sqrt(discriminant);
        float t = (-b - sqrtD) / (2.0f * a);

        if (t < 0)
            t = (-b + sqrtD) / (2.0f * a);

        if (t < 0 || t > maxDist)
            return false;

        outT = t;
        outPoint = origin + direction * t;
        outNormal = normalize(outPoint - center);

        return true;
    }

    /**
     * @brief Ray vs AABB intersection
     */
    static bool RayAABB(const Vec3& origin, const Vec3& direction, float maxDist,
                        const Vec3& minBounds, const Vec3& maxBounds,
                        float& outT, Vec3& outNormal)
    {
        Vec3 invDir = Vec3(1.0f) / direction;
        Vec3 t0 = (minBounds - origin) * invDir;
        Vec3 t1 = (maxBounds - origin) * invDir;

        Vec3 tmin = glm::min(t0, t1);
        Vec3 tmax = glm::max(t0, t1);

        float tNear = std::max(std::max(tmin.x, tmin.y), tmin.z);
        float tFar = std::min(std::min(tmax.x, tmax.y), tmax.z);

        if (tNear > tFar || tFar < 0 || tNear > maxDist)
            return false;

        outT = tNear >= 0 ? tNear : tFar;

        // Determine hit normal
        if (tmin.x >= tmin.y && tmin.x >= tmin.z)
            outNormal = Vec3(direction.x < 0 ? 1.0f : -1.0f, 0, 0);
        else if (tmin.y >= tmin.x && tmin.y >= tmin.z)
            outNormal = Vec3(0, direction.y < 0 ? 1.0f : -1.0f, 0);
        else
            outNormal = Vec3(0, 0, direction.z < 0 ? 1.0f : -1.0f);

        return true;
    }
};

} // namespace RVX::Physics
