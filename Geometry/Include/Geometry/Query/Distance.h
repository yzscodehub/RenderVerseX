/**
 * @file Distance.h
 * @brief Distance queries between geometry primitives
 */

#pragma once

#include "Core/MathTypes.h"
#include "Core/Math/AABB.h"
#include "Core/Math/Sphere.h"
#include "Geometry/GeometryFwd.h"
#include "Geometry/Constants.h"
#include "Geometry/Primitives/OBB.h"
#include "Geometry/Primitives/Capsule.h"
#include "Geometry/Primitives/Triangle.h"
#include "Geometry/Primitives/Line.h"
#include <cmath>

namespace RVX::Geometry
{

// ============================================================================
// Point-to-Shape Distances
// ============================================================================

/**
 * @brief Squared distance from a point to a line segment
 * @param point The point
 * @param a Segment start
 * @param b Segment end
 * @param outClosest Output: closest point on segment (optional)
 * @return Squared distance
 */
inline float PointToSegmentDistanceSquared(
    const Vec3& point,
    const Vec3& a,
    const Vec3& b,
    Vec3* outClosest = nullptr)
{
    Vec3 ab = b - a;
    float t = glm::dot(point - a, ab);

    if (t <= 0.0f)
    {
        if (outClosest) *outClosest = a;
        Vec3 d = point - a;
        return glm::dot(d, d);
    }

    float denom = glm::dot(ab, ab);
    if (t >= denom)
    {
        if (outClosest) *outClosest = b;
        Vec3 d = point - b;
        return glm::dot(d, d);
    }

    t /= denom;
    Vec3 closest = a + ab * t;
    if (outClosest) *outClosest = closest;
    Vec3 d = point - closest;
    return glm::dot(d, d);
}

/**
 * @brief Distance from a point to a line segment
 */
inline float PointToSegmentDistance(
    const Vec3& point,
    const Vec3& a,
    const Vec3& b,
    Vec3* outClosest = nullptr)
{
    return std::sqrt(PointToSegmentDistanceSquared(point, a, b, outClosest));
}

/**
 * @brief Squared distance from a point to a triangle
 */
inline float PointToTriangleDistanceSquared(
    const Vec3& point,
    const Triangle& tri,
    Vec3* outClosest = nullptr)
{
    Vec3 closest = tri.ClosestPoint(point);
    if (outClosest) *outClosest = closest;
    Vec3 d = point - closest;
    return glm::dot(d, d);
}

/**
 * @brief Distance from a point to a triangle
 */
inline float PointToTriangleDistance(
    const Vec3& point,
    const Triangle& tri,
    Vec3* outClosest = nullptr)
{
    return std::sqrt(PointToTriangleDistanceSquared(point, tri, outClosest));
}

/**
 * @brief Squared distance from a point to an OBB
 */
inline float PointToOBBDistanceSquared(
    const Vec3& point,
    const OBB& obb,
    Vec3* outClosest = nullptr)
{
    Vec3 closest = obb.ClosestPoint(point);
    if (outClosest) *outClosest = closest;
    Vec3 d = point - closest;
    return glm::dot(d, d);
}

/**
 * @brief Distance from a point to an OBB
 */
inline float PointToOBBDistance(
    const Vec3& point,
    const OBB& obb,
    Vec3* outClosest = nullptr)
{
    return std::sqrt(PointToOBBDistanceSquared(point, obb, outClosest));
}

/**
 * @brief Squared distance from a point to a capsule surface
 */
inline float PointToCapsuleDistanceSquared(
    const Vec3& point,
    const Capsule& capsule,
    Vec3* outClosest = nullptr)
{
    Vec3 axisClosest = capsule.ClosestPointOnAxis(point);
    Vec3 toPoint = point - axisClosest;
    float dist = glm::length(toPoint);

    float surfaceDist = dist - capsule.radius;
    if (surfaceDist <= 0.0f)
    {
        if (outClosest) *outClosest = point;
        return 0.0f;
    }

    if (outClosest)
    {
        if (dist > EPSILON)
            *outClosest = axisClosest + toPoint * (capsule.radius / dist);
        else
            *outClosest = axisClosest + Vec3(capsule.radius, 0, 0);
    }

    return surfaceDist * surfaceDist;
}

/**
 * @brief Distance from a point to a capsule surface
 */
inline float PointToCapsuleDistance(
    const Vec3& point,
    const Capsule& capsule,
    Vec3* outClosest = nullptr)
{
    return std::sqrt(PointToCapsuleDistanceSquared(point, capsule, outClosest));
}

// ============================================================================
// Shape-to-Shape Distances
// ============================================================================

/**
 * @brief Distance between two AABBs
 * @return 0 if overlapping, otherwise minimum distance
 */
inline float AABBAABBDistance(const AABB& a, const AABB& b)
{
    float sqDist = 0.0f;

    for (int i = 0; i < 3; ++i)
    {
        float aMin = a.GetMin()[i];
        float aMax = a.GetMax()[i];
        float bMin = b.GetMin()[i];
        float bMax = b.GetMax()[i];

        if (aMax < bMin)
            sqDist += Square(bMin - aMax);
        else if (bMax < aMin)
            sqDist += Square(aMin - bMax);
    }

    return std::sqrt(sqDist);
}

/**
 * @brief Distance between two spheres
 * @return 0 if overlapping, otherwise minimum distance
 */
inline float SphereSphereDistance(const Sphere& a, const Sphere& b)
{
    float centerDist = glm::length(b.GetCenter() - a.GetCenter());
    float surfaceDist = centerDist - a.GetRadius() - b.GetRadius();
    return surfaceDist > 0.0f ? surfaceDist : 0.0f;
}

/**
 * @brief Distance between an AABB and a sphere
 */
inline float AABBSphereDistance(const AABB& aabb, const Sphere& sphere)
{
    Vec3 closest = glm::clamp(sphere.GetCenter(), aabb.GetMin(), aabb.GetMax());
    float dist = glm::length(sphere.GetCenter() - closest) - sphere.GetRadius();
    return dist > 0.0f ? dist : 0.0f;
}

/**
 * @brief Distance between two OBBs (approximate using closest points)
 * Note: For exact distance, use GJK algorithm
 */
inline float OBBOBBDistanceApprox(const OBB& a, const OBB& b)
{
    // Get closest point on B to center of A
    Vec3 closestOnB = b.ClosestPoint(a.center);
    // Get closest point on A to that point
    Vec3 closestOnA = a.ClosestPoint(closestOnB);
    // Get closest point on B to that point
    closestOnB = b.ClosestPoint(closestOnA);

    return glm::length(closestOnA - closestOnB);
}

/**
 * @brief Distance between OBB and sphere
 */
inline float OBBSphereDistance(const OBB& obb, const Sphere& sphere)
{
    Vec3 closest = obb.ClosestPoint(sphere.GetCenter());
    float dist = glm::length(sphere.GetCenter() - closest) - sphere.GetRadius();
    return dist > 0.0f ? dist : 0.0f;
}

// Note: CapsuleCapsuleDistance is defined in Capsule.h

/**
 * @brief Distance between capsule and sphere
 */
inline float CapsuleSphereDistance(const Capsule& capsule, const Sphere& sphere)
{
    float axisDist = capsule.GetSegment().Distance(sphere.GetCenter());
    float surfaceDist = axisDist - capsule.radius - sphere.GetRadius();
    return surfaceDist > 0.0f ? surfaceDist : 0.0f;
}

/**
 * @brief Distance between capsule and AABB
 */
inline float CapsuleAABBDistance(const Capsule& capsule, const AABB& aabb)
{
    // Find closest point on AABB to capsule axis
    Vec3 closestOnAxis = capsule.GetSegment().ClosestPoint(aabb.GetCenter());
    Vec3 closestOnAABB = glm::clamp(closestOnAxis, aabb.GetMin(), aabb.GetMax());

    // Find closest point on capsule axis to that AABB point
    closestOnAxis = capsule.GetSegment().ClosestPoint(closestOnAABB);

    float dist = glm::length(closestOnAxis - closestOnAABB) - capsule.radius;
    return dist > 0.0f ? dist : 0.0f;
}

// ============================================================================
// Segment-to-Shape Distances
// ============================================================================

/**
 * @brief Distance between two line segments
 */
inline float SegmentSegmentDistance(
    const Vec3& a0, const Vec3& a1,
    const Vec3& b0, const Vec3& b1,
    float& outS, float& outT)
{
    Segment s1(a0, a1);
    Segment s2(b0, b1);
    return SegmentSegmentDistance(s1, s2, outS, outT);
}

/**
 * @brief Distance from a line segment to a triangle
 */
inline float SegmentTriangleDistance(
    const Segment& seg,
    const Triangle& tri,
    Vec3* outClosestOnSeg = nullptr,
    Vec3* outClosestOnTri = nullptr)
{
    float minDistSq = FLT_MAX;
    Vec3 bestSegPoint, bestTriPoint;

    // Sample points along segment and find closest to triangle
    constexpr int samples = 8;
    for (int i = 0; i <= samples; ++i)
    {
        float t = static_cast<float>(i) / samples;
        Vec3 segPoint = seg.PointAt(t);
        Vec3 triClosest = tri.ClosestPoint(segPoint);
        Vec3 d = segPoint - triClosest;
        float distSq = glm::dot(d, d);

        if (distSq < minDistSq)
        {
            minDistSq = distSq;
            bestSegPoint = segPoint;
            bestTriPoint = triClosest;
        }
    }

    // Also check triangle edges against segment
    for (int i = 0; i < 3; ++i)
    {
        auto [e0, e1] = tri.GetEdge(i);
        Segment edge(e0, e1);
        float s, t;
        float distSq = SegmentSegmentDistanceSquared(seg, edge, s, t);

        if (distSq < minDistSq)
        {
            minDistSq = distSq;
            bestSegPoint = seg.PointAt(s);
            bestTriPoint = edge.PointAt(t);
        }
    }

    if (outClosestOnSeg) *outClosestOnSeg = bestSegPoint;
    if (outClosestOnTri) *outClosestOnTri = bestTriPoint;

    return std::sqrt(minDistSq);
}

// ============================================================================
// Signed Distance Functions
// ============================================================================

/**
 * @brief Signed distance from a point to a plane
 */
inline float SignedDistanceToPlane(const Vec3& point, const Vec3& planeNormal, float planeD)
{
    return glm::dot(point, planeNormal) + planeD;
}

/**
 * @brief Signed distance from a point to a sphere (negative inside)
 */
inline float SignedDistanceToSphere(const Vec3& point, const Sphere& sphere)
{
    return glm::length(point - sphere.GetCenter()) - sphere.GetRadius();
}

/**
 * @brief Signed distance from a point to an AABB (negative inside)
 */
inline float SignedDistanceToAABB(const Vec3& point, const AABB& aabb)
{
    Vec3 center = aabb.GetCenter();
    Vec3 halfExtent = aabb.GetExtent();
    Vec3 q = glm::abs(point - center) - halfExtent;

    float outsideDist = glm::length(glm::max(q, Vec3(0.0f)));
    float insideDist = std::min(std::max(q.x, std::max(q.y, q.z)), 0.0f);

    return outsideDist + insideDist;
}

/**
 * @brief Signed distance from a point to a capsule (negative inside)
 */
inline float SignedDistanceToCapsule(const Vec3& point, const Capsule& capsule)
{
    return capsule.SignedDistance(point);
}

} // namespace RVX::Geometry
