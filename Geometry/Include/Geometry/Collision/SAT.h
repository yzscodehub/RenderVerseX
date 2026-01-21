/**
 * @file SAT.h
 * @brief Separating Axis Theorem for convex shape collision
 */

#pragma once

#include "Core/MathTypes.h"
#include "Geometry/Primitives/OBB.h"
#include "Geometry/Collision/ContactManifold.h"
#include "Geometry/Constants.h"
#include <cmath>
#include <array>
#include <algorithm>

namespace RVX::Geometry
{

/**
 * @brief SAT collision test between two OBBs
 * 
 * Uses the Separating Axis Theorem to test collision between
 * two oriented bounding boxes. Can optionally generate contact manifold.
 * 
 * @param a First OBB
 * @param b Second OBB
 * @param outManifold Optional contact manifold output
 * @return true if the OBBs intersect
 */
inline bool SATTestOBB(
    const OBB& a,
    const OBB& b,
    ContactManifold* outManifold = nullptr)
{
    Mat3 rotA = a.GetRotationMatrix();
    Mat3 rotB = b.GetRotationMatrix();

    Vec3 axesA[3] = { rotA[0], rotA[1], rotA[2] };
    Vec3 axesB[3] = { rotB[0], rotB[1], rotB[2] };

    // Compute rotation matrix expressing B in A's coordinate frame
    Mat3 R, AbsR;
    for (int i = 0; i < 3; ++i)
    {
        for (int j = 0; j < 3; ++j)
        {
            R[i][j] = glm::dot(axesA[i], axesB[j]);
            AbsR[i][j] = std::abs(R[i][j]) + EPSILON;
        }
    }

    Vec3 t = b.center - a.center;
    t = Vec3(glm::dot(t, axesA[0]), glm::dot(t, axesA[1]), glm::dot(t, axesA[2]));

    float minPenetration = FLT_MAX;
    Vec3 minAxis;
    int minAxisType = -1;

    // Helper to test an axis
    auto TestAxis = [&](const Vec3& axis, float ra, float rb, int axisType) -> bool
    {
        float separation = std::abs(glm::dot(t, axis)) - (ra + rb);
        if (separation > 0) return false;  // Separating axis found

        float penetration = -(separation);
        if (penetration < minPenetration)
        {
            minPenetration = penetration;
            minAxis = axis;
            minAxisType = axisType;
        }
        return true;
    };

    // Test axes L = A0, A1, A2
    for (int i = 0; i < 3; ++i)
    {
        float ra = a.halfExtents[i];
        float rb = b.halfExtents[0] * AbsR[i][0] + 
                   b.halfExtents[1] * AbsR[i][1] + 
                   b.halfExtents[2] * AbsR[i][2];
        Vec3 localAxis = Vec3(0);
        localAxis[i] = 1.0f;
        if (!TestAxis(localAxis, ra, rb, i)) return false;
    }

    // Test axes L = B0, B1, B2
    for (int i = 0; i < 3; ++i)
    {
        float ra = a.halfExtents[0] * AbsR[0][i] + 
                   a.halfExtents[1] * AbsR[1][i] + 
                   a.halfExtents[2] * AbsR[2][i];
        float rb = b.halfExtents[i];
        Vec3 localAxis(R[0][i], R[1][i], R[2][i]);
        if (!TestAxis(localAxis, ra, rb, 3 + i)) return false;
    }

    // Test cross product axes (9 axes)
    // A0 x B0
    {
        float ra = a.halfExtents[1] * AbsR[2][0] + a.halfExtents[2] * AbsR[1][0];
        float rb = b.halfExtents[1] * AbsR[0][2] + b.halfExtents[2] * AbsR[0][1];
        Vec3 localAxis(0, -R[2][0], R[1][0]);
        if (glm::dot(localAxis, localAxis) > EPSILON)
        {
            localAxis = glm::normalize(localAxis);
            if (!TestAxis(localAxis, ra, rb, 6)) return false;
        }
    }

    // A0 x B1
    {
        float ra = a.halfExtents[1] * AbsR[2][1] + a.halfExtents[2] * AbsR[1][1];
        float rb = b.halfExtents[0] * AbsR[0][2] + b.halfExtents[2] * AbsR[0][0];
        Vec3 localAxis(0, -R[2][1], R[1][1]);
        if (glm::dot(localAxis, localAxis) > EPSILON)
        {
            localAxis = glm::normalize(localAxis);
            if (!TestAxis(localAxis, ra, rb, 7)) return false;
        }
    }

    // A0 x B2
    {
        float ra = a.halfExtents[1] * AbsR[2][2] + a.halfExtents[2] * AbsR[1][2];
        float rb = b.halfExtents[0] * AbsR[0][1] + b.halfExtents[1] * AbsR[0][0];
        Vec3 localAxis(0, -R[2][2], R[1][2]);
        if (glm::dot(localAxis, localAxis) > EPSILON)
        {
            localAxis = glm::normalize(localAxis);
            if (!TestAxis(localAxis, ra, rb, 8)) return false;
        }
    }

    // A1 x B0
    {
        float ra = a.halfExtents[0] * AbsR[2][0] + a.halfExtents[2] * AbsR[0][0];
        float rb = b.halfExtents[1] * AbsR[1][2] + b.halfExtents[2] * AbsR[1][1];
        Vec3 localAxis(R[2][0], 0, -R[0][0]);
        if (glm::dot(localAxis, localAxis) > EPSILON)
        {
            localAxis = glm::normalize(localAxis);
            if (!TestAxis(localAxis, ra, rb, 9)) return false;
        }
    }

    // A1 x B1
    {
        float ra = a.halfExtents[0] * AbsR[2][1] + a.halfExtents[2] * AbsR[0][1];
        float rb = b.halfExtents[0] * AbsR[1][2] + b.halfExtents[2] * AbsR[1][0];
        Vec3 localAxis(R[2][1], 0, -R[0][1]);
        if (glm::dot(localAxis, localAxis) > EPSILON)
        {
            localAxis = glm::normalize(localAxis);
            if (!TestAxis(localAxis, ra, rb, 10)) return false;
        }
    }

    // A1 x B2
    {
        float ra = a.halfExtents[0] * AbsR[2][2] + a.halfExtents[2] * AbsR[0][2];
        float rb = b.halfExtents[0] * AbsR[1][1] + b.halfExtents[1] * AbsR[1][0];
        Vec3 localAxis(R[2][2], 0, -R[0][2]);
        if (glm::dot(localAxis, localAxis) > EPSILON)
        {
            localAxis = glm::normalize(localAxis);
            if (!TestAxis(localAxis, ra, rb, 11)) return false;
        }
    }

    // A2 x B0
    {
        float ra = a.halfExtents[0] * AbsR[1][0] + a.halfExtents[1] * AbsR[0][0];
        float rb = b.halfExtents[1] * AbsR[2][2] + b.halfExtents[2] * AbsR[2][1];
        Vec3 localAxis(-R[1][0], R[0][0], 0);
        if (glm::dot(localAxis, localAxis) > EPSILON)
        {
            localAxis = glm::normalize(localAxis);
            if (!TestAxis(localAxis, ra, rb, 12)) return false;
        }
    }

    // A2 x B1
    {
        float ra = a.halfExtents[0] * AbsR[1][1] + a.halfExtents[1] * AbsR[0][1];
        float rb = b.halfExtents[0] * AbsR[2][2] + b.halfExtents[2] * AbsR[2][0];
        Vec3 localAxis(-R[1][1], R[0][1], 0);
        if (glm::dot(localAxis, localAxis) > EPSILON)
        {
            localAxis = glm::normalize(localAxis);
            if (!TestAxis(localAxis, ra, rb, 13)) return false;
        }
    }

    // A2 x B2
    {
        float ra = a.halfExtents[0] * AbsR[1][2] + a.halfExtents[1] * AbsR[0][2];
        float rb = b.halfExtents[0] * AbsR[2][1] + b.halfExtents[1] * AbsR[2][0];
        Vec3 localAxis(-R[1][2], R[0][2], 0);
        if (glm::dot(localAxis, localAxis) > EPSILON)
        {
            localAxis = glm::normalize(localAxis);
            if (!TestAxis(localAxis, ra, rb, 14)) return false;
        }
    }

    // No separating axis found - boxes intersect
    if (outManifold)
    {
        outManifold->Clear();

        // Convert minAxis to world space
        Vec3 worldAxis;
        if (minAxisType < 3)
        {
            worldAxis = axesA[minAxisType];
        }
        else if (minAxisType < 6)
        {
            worldAxis = axesB[minAxisType - 3];
        }
        else
        {
            // Cross product axis - already normalized
            worldAxis = axesA[0] * minAxis.x + axesA[1] * minAxis.y + axesA[2] * minAxis.z;
        }

        // Ensure normal points from A to B
        Vec3 centerDiff = b.center - a.center;
        if (glm::dot(worldAxis, centerDiff) < 0)
        {
            worldAxis = -worldAxis;
        }

        outManifold->normal = worldAxis;

        // Generate contact points (simplified - just use closest points)
        Vec3 pointOnA = a.ClosestPoint(b.center);
        Vec3 pointOnB = b.ClosestPoint(a.center);

        outManifold->Add(pointOnA, pointOnB, worldAxis, minPenetration);
    }

    return true;
}

/**
 * @brief SAT test for OBB vs Sphere (simplified)
 */
inline bool SATTestOBBSphere(
    const OBB& obb,
    const Vec3& sphereCenter,
    float sphereRadius,
    ContactManifold* outManifold = nullptr)
{
    Vec3 closest = obb.ClosestPoint(sphereCenter);
    Vec3 diff = sphereCenter - closest;
    float distSq = glm::dot(diff, diff);
    float radiusSq = sphereRadius * sphereRadius;

    if (distSq > radiusSq)
        return false;

    if (outManifold)
    {
        outManifold->Clear();

        float dist = std::sqrt(distSq);
        Vec3 normal = dist > EPSILON ? diff / dist : Vec3(0, 1, 0);

        Vec3 pointOnOBB = closest;
        Vec3 pointOnSphere = sphereCenter - normal * sphereRadius;
        float penetration = sphereRadius - dist;

        outManifold->normal = normal;
        outManifold->Add(pointOnOBB, pointOnSphere, normal, penetration);
    }

    return true;
}

/**
 * @brief SAT test for Capsule vs Capsule
 */
inline bool SATTestCapsule(
    const Capsule& a,
    const Capsule& b,
    ContactManifold* outManifold = nullptr)
{
    float s, t;
    float distSq = SegmentSegmentDistanceSquared(a.GetSegment(), b.GetSegment(), s, t);
    float radiusSum = a.radius + b.radius;

    if (distSq > radiusSum * radiusSum)
        return false;

    if (outManifold)
    {
        outManifold->Clear();

        Vec3 closestA = a.GetSegment().PointAt(s);
        Vec3 closestB = b.GetSegment().PointAt(t);

        Vec3 diff = closestB - closestA;
        float dist = std::sqrt(distSq);
        Vec3 normal = dist > EPSILON ? diff / dist : Vec3(0, 1, 0);

        Vec3 pointOnA = closestA + normal * a.radius;
        Vec3 pointOnB = closestB - normal * b.radius;
        float penetration = radiusSum - dist;

        outManifold->normal = normal;
        outManifold->Add(pointOnA, pointOnB, normal, penetration);
    }

    return true;
}

} // namespace RVX::Geometry
