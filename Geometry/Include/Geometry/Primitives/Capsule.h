/**
 * @file Capsule.h
 * @brief Capsule primitive (swept sphere / stadium)
 */

#pragma once

#include "Core/MathTypes.h"
#include "Core/Math/AABB.h"
#include "Core/Math/Sphere.h"
#include "Geometry/Constants.h"
#include "Geometry/Primitives/Line.h"
#include <cmath>

namespace RVX::Geometry
{

/**
 * @brief Capsule (swept sphere / stadium shape)
 * 
 * Defined by two endpoints and a radius. The capsule is the set of all
 * points within the radius distance from the line segment.
 * 
 * Commonly used for:
 * - Character collision (simpler than convex hull)
 * - Bone/limb collision in ragdoll physics
 * - Swept sphere collision detection
 */
struct Capsule
{
    Vec3 a{0.0f, 0.0f, 0.0f};   ///< First endpoint (bottom)
    Vec3 b{0.0f, 1.0f, 0.0f};   ///< Second endpoint (top)
    float radius{0.5f};         ///< Radius of the swept sphere

    Capsule() = default;

    Capsule(const Vec3& pointA, const Vec3& pointB, float r)
        : a(pointA)
        , b(pointB)
        , radius(r)
    {}

    /**
     * @brief Construct from center, height, radius, and up direction
     */
    static Capsule FromCenterHeight(
        const Vec3& center,
        float height,
        float r,
        const Vec3& up = Vec3(0.0f, 1.0f, 0.0f))
    {
        float halfHeight = (height - 2.0f * r) * 0.5f;
        if (halfHeight < 0.0f) halfHeight = 0.0f;

        Vec3 offset = glm::normalize(up) * halfHeight;
        return Capsule(center - offset, center + offset, r);
    }

    // =========================================================================
    // Basic Properties
    // =========================================================================

    /**
     * @brief Get the line segment forming the capsule's axis
     */
    Segment GetSegment() const
    {
        return Segment(a, b);
    }

    /**
     * @brief Get the axis direction (from a to b, not normalized)
     */
    Vec3 GetAxis() const
    {
        return b - a;
    }

    /**
     * @brief Get the normalized axis direction
     */
    Vec3 GetAxisNormalized() const
    {
        Vec3 axis = b - a;
        float len = glm::length(axis);
        return len > EPSILON ? axis / len : Vec3(0.0f, 1.0f, 0.0f);
    }

    /**
     * @brief Get the center of the capsule
     */
    Vec3 GetCenter() const
    {
        return (a + b) * 0.5f;
    }

    /**
     * @brief Get the length of the axis segment (not including hemispherical caps)
     */
    float GetSegmentLength() const
    {
        return glm::length(b - a);
    }

    /**
     * @brief Get the total height of the capsule (including caps)
     */
    float GetHeight() const
    {
        return GetSegmentLength() + 2.0f * radius;
    }

    /**
     * @brief Check if the capsule is degenerate (becomes a sphere)
     */
    bool IsSphere() const
    {
        return GetSegmentLength() < EPSILON;
    }

    // =========================================================================
    // Bounding Volumes
    // =========================================================================

    /**
     * @brief Get axis-aligned bounding box
     */
    AABB GetBoundingBox() const
    {
        Vec3 r(radius);
        Vec3 minPt = glm::min(a, b) - r;
        Vec3 maxPt = glm::max(a, b) + r;
        return AABB(minPt, maxPt);
    }

    /**
     * @brief Get bounding sphere
     */
    Sphere GetBoundingSphere() const
    {
        Vec3 center = GetCenter();
        float dist = GetSegmentLength() * 0.5f + radius;
        return Sphere(center, dist);
    }

    // =========================================================================
    // Metrics
    // =========================================================================

    /**
     * @brief Get the volume of the capsule
     */
    float GetVolume() const
    {
        float segLen = GetSegmentLength();
        // Volume = cylinder + sphere
        float cylinderVol = PI * radius * radius * segLen;
        float sphereVol = (4.0f / 3.0f) * PI * radius * radius * radius;
        return cylinderVol + sphereVol;
    }

    /**
     * @brief Get the surface area of the capsule
     */
    float GetSurfaceArea() const
    {
        float segLen = GetSegmentLength();
        // Surface = cylinder side + sphere
        float cylinderArea = 2.0f * PI * radius * segLen;
        float sphereArea = 4.0f * PI * radius * radius;
        return cylinderArea + sphereArea;
    }

    // =========================================================================
    // Point Queries
    // =========================================================================

    /**
     * @brief Get the closest point on the capsule's axis to a given point
     */
    Vec3 ClosestPointOnAxis(const Vec3& point) const
    {
        return GetSegment().ClosestPoint(point);
    }

    /**
     * @brief Get the closest point on/in the capsule to a given point
     */
    Vec3 ClosestPoint(const Vec3& point) const
    {
        Vec3 axisPoint = ClosestPointOnAxis(point);
        Vec3 toPoint = point - axisPoint;
        float dist = glm::length(toPoint);

        if (dist < EPSILON)
        {
            // Point is on the axis - return point on surface
            // Use arbitrary perpendicular direction
            Vec3 axis = GetAxisNormalized();
            Vec3 perp = std::abs(axis.x) < 0.9f
                ? glm::cross(axis, Vec3(1, 0, 0))
                : glm::cross(axis, Vec3(0, 1, 0));
            return axisPoint + glm::normalize(perp) * radius;
        }

        if (dist <= radius)
        {
            // Point is inside the capsule
            return point;
        }

        // Point is outside - return point on surface
        return axisPoint + toPoint * (radius / dist);
    }

    /**
     * @brief Check if a point is inside the capsule
     */
    bool Contains(const Vec3& point) const
    {
        float distSq = GetSegment().DistanceSquared(point);
        return distSq <= radius * radius;
    }

    /**
     * @brief Squared distance from a point to the capsule surface
     * Returns 0 if point is inside
     */
    float DistanceSquared(const Vec3& point) const
    {
        float axisDist = GetSegment().Distance(point);
        float surfaceDist = axisDist - radius;
        return surfaceDist > 0.0f ? surfaceDist * surfaceDist : 0.0f;
    }

    /**
     * @brief Distance from a point to the capsule surface
     * Returns 0 if point is inside
     */
    float Distance(const Vec3& point) const
    {
        float axisDist = GetSegment().Distance(point);
        float surfaceDist = axisDist - radius;
        return surfaceDist > 0.0f ? surfaceDist : 0.0f;
    }

    /**
     * @brief Signed distance from a point (negative if inside)
     */
    float SignedDistance(const Vec3& point) const
    {
        float axisDist = GetSegment().Distance(point);
        return axisDist - radius;
    }

    // =========================================================================
    // Support Function (for GJK)
    // =========================================================================

    /**
     * @brief Get the support point in a given direction
     * @param direction World-space direction (doesn't need to be normalized)
     * @return The point on the capsule surface furthest in the given direction
     */
    Vec3 Support(const Vec3& direction) const
    {
        float len = glm::length(direction);
        if (len < EPSILON)
        {
            return a + Vec3(0, radius, 0);
        }

        Vec3 normDir = direction / len;

        // Find which endpoint is furthest in the direction
        float dotA = glm::dot(a, normDir);
        float dotB = glm::dot(b, normDir);

        Vec3 endPoint = (dotA > dotB) ? a : b;

        // Add radius in the direction
        return endPoint + normDir * radius;
    }

    // =========================================================================
    // Transformation
    // =========================================================================

    /**
     * @brief Translate the capsule
     */
    Capsule Translated(const Vec3& offset) const
    {
        return Capsule(a + offset, b + offset, radius);
    }

    /**
     * @brief Transform the capsule by a matrix
     * Note: Non-uniform scale will produce incorrect results
     */
    Capsule Transformed(const Mat4& transform) const
    {
        Vec3 newA = Vec3(transform * Vec4(a, 1.0f));
        Vec3 newB = Vec3(transform * Vec4(b, 1.0f));
        // For uniform scale, extract and apply to radius
        // For now, assume no scale
        return Capsule(newA, newB, radius);
    }
};

// ============================================================================
// Capsule-Capsule Distance
// ============================================================================

/**
 * @brief Compute the distance between two capsules
 * @return Distance between surfaces (0 if overlapping)
 */
inline float CapsuleCapsuleDistance(const Capsule& c1, const Capsule& c2)
{
    float s, t;
    float segDist = SegmentSegmentDistance(c1.GetSegment(), c2.GetSegment(), s, t);
    float surfaceDist = segDist - c1.radius - c2.radius;
    return surfaceDist > 0.0f ? surfaceDist : 0.0f;
}

/**
 * @brief Check if two capsules overlap
 */
inline bool CapsuleOverlap(const Capsule& c1, const Capsule& c2)
{
    float s, t;
    float segDistSq = SegmentSegmentDistanceSquared(c1.GetSegment(), c2.GetSegment(), s, t);
    float radiusSum = c1.radius + c2.radius;
    return segDistSq <= radiusSum * radiusSum;
}

} // namespace RVX::Geometry
