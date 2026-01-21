/**
 * @file Cylinder.h
 * @brief Cylinder primitive
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
 * @brief Cylinder primitive defined by two endpoints and a radius
 * 
 * A right circular cylinder with flat end caps.
 */
struct Cylinder
{
    Vec3 a{0.0f, 0.0f, 0.0f};   ///< First endpoint (bottom center)
    Vec3 b{0.0f, 1.0f, 0.0f};   ///< Second endpoint (top center)
    float radius{0.5f};         ///< Radius of the cylinder

    Cylinder() = default;

    Cylinder(const Vec3& bottom, const Vec3& top, float r)
        : a(bottom)
        , b(top)
        , radius(r)
    {}

    /**
     * @brief Construct from center, height, radius, and up direction
     */
    static Cylinder FromCenterHeight(
        const Vec3& center,
        float height,
        float r,
        const Vec3& up = Vec3(0.0f, 1.0f, 0.0f))
    {
        Vec3 offset = glm::normalize(up) * (height * 0.5f);
        return Cylinder(center - offset, center + offset, r);
    }

    // =========================================================================
    // Basic Properties
    // =========================================================================

    /**
     * @brief Get the axis segment
     */
    Segment GetSegment() const
    {
        return Segment(a, b);
    }

    /**
     * @brief Get the axis direction (not normalized)
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
     * @brief Get the center of the cylinder
     */
    Vec3 GetCenter() const
    {
        return (a + b) * 0.5f;
    }

    /**
     * @brief Get the height of the cylinder
     */
    float GetHeight() const
    {
        return glm::length(b - a);
    }

    // =========================================================================
    // Bounding Volumes
    // =========================================================================

    /**
     * @brief Get axis-aligned bounding box
     */
    AABB GetBoundingBox() const
    {
        // Need to consider the circular cross-section
        Vec3 axis = GetAxisNormalized();
        
        // Compute extent in each world axis direction
        Vec3 extent;
        for (int i = 0; i < 3; ++i)
        {
            // The extent perpendicular to the axis
            float sinAngle = std::sqrt(1.0f - axis[i] * axis[i]);
            extent[i] = radius * sinAngle;
        }

        Vec3 minPt = glm::min(a, b) - extent;
        Vec3 maxPt = glm::max(a, b) + extent;
        return AABB(minPt, maxPt);
    }

    /**
     * @brief Get bounding sphere
     */
    Sphere GetBoundingSphere() const
    {
        Vec3 center = GetCenter();
        float halfHeight = GetHeight() * 0.5f;
        float dist = std::sqrt(halfHeight * halfHeight + radius * radius);
        return Sphere(center, dist);
    }

    // =========================================================================
    // Metrics
    // =========================================================================

    /**
     * @brief Get the volume of the cylinder
     */
    float GetVolume() const
    {
        return PI * radius * radius * GetHeight();
    }

    /**
     * @brief Get the surface area of the cylinder
     */
    float GetSurfaceArea() const
    {
        float h = GetHeight();
        // Side + two end caps
        return 2.0f * PI * radius * h + 2.0f * PI * radius * radius;
    }

    // =========================================================================
    // Point Queries
    // =========================================================================

    /**
     * @brief Check if a point is inside the cylinder
     */
    bool Contains(const Vec3& point) const
    {
        Vec3 axis = b - a;
        float height = glm::length(axis);
        if (height < EPSILON) return false;

        Vec3 axisNorm = axis / height;
        Vec3 toPoint = point - a;

        // Project onto axis
        float projLen = glm::dot(toPoint, axisNorm);
        if (projLen < 0.0f || projLen > height)
            return false;

        // Check radial distance
        Vec3 proj = a + axisNorm * projLen;
        float radialDistSq = glm::dot(point - proj, point - proj);
        return radialDistSq <= radius * radius;
    }

    /**
     * @brief Get the closest point on/in the cylinder to a given point
     */
    Vec3 ClosestPoint(const Vec3& point) const
    {
        Vec3 axis = b - a;
        float height = glm::length(axis);
        if (height < EPSILON) return a;

        Vec3 axisNorm = axis / height;
        Vec3 toPoint = point - a;

        // Project onto axis and clamp
        float projLen = glm::dot(toPoint, axisNorm);
        projLen = std::clamp(projLen, 0.0f, height);

        Vec3 axisPoint = a + axisNorm * projLen;
        Vec3 radial = point - axisPoint;
        float radialDist = glm::length(radial);

        if (radialDist < EPSILON)
        {
            // Point is on the axis
            return axisPoint;
        }

        if (radialDist <= radius)
        {
            // Point is inside (radially)
            // Check if on end cap
            float origProj = glm::dot(toPoint, axisNorm);
            if (origProj >= 0.0f && origProj <= height)
                return point;  // Inside cylinder
        }

        // Clamp to surface
        Vec3 radialNorm = radial / radialDist;
        return axisPoint + radialNorm * std::min(radialDist, radius);
    }

    /**
     * @brief Distance from a point to the cylinder surface
     */
    float Distance(const Vec3& point) const
    {
        Vec3 closest = ClosestPoint(point);
        return glm::length(point - closest);
    }

    // =========================================================================
    // Support Function (for GJK)
    // =========================================================================

    /**
     * @brief Get the support point in a given direction
     */
    Vec3 Support(const Vec3& direction) const
    {
        Vec3 axis = GetAxisNormalized();
        float dirLen = glm::length(direction);
        if (dirLen < EPSILON)
            return a;

        Vec3 normDir = direction / dirLen;

        // Determine which end cap to use
        float dotA = glm::dot(a, normDir);
        float dotB = glm::dot(b, normDir);
        Vec3 endPoint = (dotA > dotB) ? a : b;

        // Project direction onto the plane perpendicular to axis
        Vec3 radialDir = normDir - axis * glm::dot(normDir, axis);
        float radialLen = glm::length(radialDir);

        if (radialLen < EPSILON)
        {
            return endPoint;
        }

        return endPoint + (radialDir / radialLen) * radius;
    }
};

} // namespace RVX::Geometry
