/**
 * @file Cone.h
 * @brief Cone primitive
 */

#pragma once

#include "Core/MathTypes.h"
#include "Core/Math/AABB.h"
#include "Core/Math/Sphere.h"
#include "Geometry/Constants.h"
#include <cmath>

namespace RVX::Geometry
{

/**
 * @brief Cone primitive defined by apex, base center, and base radius
 * 
 * A right circular cone with a flat base.
 */
struct Cone
{
    Vec3 apex{0.0f, 1.0f, 0.0f};    ///< Apex (tip) of the cone
    Vec3 base{0.0f, 0.0f, 0.0f};    ///< Center of the base circle
    float radius{0.5f};              ///< Radius of the base

    Cone() = default;

    Cone(const Vec3& tipPoint, const Vec3& baseCenter, float baseRadius)
        : apex(tipPoint)
        , base(baseCenter)
        , radius(baseRadius)
    {}

    /**
     * @brief Construct from base center, height, radius, and up direction
     */
    static Cone FromBaseHeight(
        const Vec3& baseCenter,
        float height,
        float r,
        const Vec3& up = Vec3(0.0f, 1.0f, 0.0f))
    {
        Vec3 apexPoint = baseCenter + glm::normalize(up) * height;
        return Cone(apexPoint, baseCenter, r);
    }

    // =========================================================================
    // Basic Properties
    // =========================================================================

    /**
     * @brief Get the axis direction (from base to apex, not normalized)
     */
    Vec3 GetAxis() const
    {
        return apex - base;
    }

    /**
     * @brief Get the normalized axis direction (from base to apex)
     */
    Vec3 GetAxisNormalized() const
    {
        Vec3 axis = apex - base;
        float len = glm::length(axis);
        return len > EPSILON ? axis / len : Vec3(0.0f, 1.0f, 0.0f);
    }

    /**
     * @brief Get the height of the cone
     */
    float GetHeight() const
    {
        return glm::length(apex - base);
    }

    /**
     * @brief Get the half-angle at the apex (in radians)
     */
    float GetHalfAngle() const
    {
        float h = GetHeight();
        return h > EPSILON ? std::atan(radius / h) : HALF_PI;
    }

    /**
     * @brief Get the slant height (from apex to edge of base)
     */
    float GetSlantHeight() const
    {
        float h = GetHeight();
        return std::sqrt(h * h + radius * radius);
    }

    /**
     * @brief Get the center of mass
     */
    Vec3 GetCenterOfMass() const
    {
        // Center of mass is 1/4 of height from base
        return base + GetAxis() * 0.25f;
    }

    // =========================================================================
    // Bounding Volumes
    // =========================================================================

    /**
     * @brief Get axis-aligned bounding box
     */
    AABB GetBoundingBox() const
    {
        // Need to consider the circular base
        Vec3 axis = GetAxisNormalized();
        
        // Compute base extent in each world axis direction
        Vec3 extent;
        for (int i = 0; i < 3; ++i)
        {
            float sinAngle = std::sqrt(1.0f - axis[i] * axis[i]);
            extent[i] = radius * sinAngle;
        }

        Vec3 minPt = glm::min(apex, base - extent);
        Vec3 maxPt = glm::max(apex, base + extent);
        return AABB(minPt, maxPt);
    }

    /**
     * @brief Get bounding sphere
     */
    Sphere GetBoundingSphere() const
    {
        float h = GetHeight();
        // Optimal bounding sphere center and radius
        if (radius > h)
        {
            // Wide cone - sphere centered at base
            return Sphere(base, radius);
        }
        else
        {
            // Tall cone - sphere encompasses apex and base edge
            float slant = GetSlantHeight();
            Vec3 center = base + GetAxisNormalized() * (h * 0.5f);
            float sphereRadius = std::max(slant, h) * 0.5f;
            return Sphere(center, sphereRadius);
        }
    }

    // =========================================================================
    // Metrics
    // =========================================================================

    /**
     * @brief Get the volume of the cone
     */
    float GetVolume() const
    {
        return (1.0f / 3.0f) * PI * radius * radius * GetHeight();
    }

    /**
     * @brief Get the surface area of the cone (including base)
     */
    float GetSurfaceArea() const
    {
        float slant = GetSlantHeight();
        // Lateral surface + base
        return PI * radius * slant + PI * radius * radius;
    }

    /**
     * @brief Get the lateral surface area (excluding base)
     */
    float GetLateralSurfaceArea() const
    {
        return PI * radius * GetSlantHeight();
    }

    // =========================================================================
    // Point Queries
    // =========================================================================

    /**
     * @brief Check if a point is inside the cone
     */
    bool Contains(const Vec3& point) const
    {
        Vec3 axis = apex - base;
        float height = glm::length(axis);
        if (height < EPSILON) return false;

        Vec3 axisNorm = axis / height;
        Vec3 toPoint = point - base;

        // Project onto axis (distance from base along axis)
        float projLen = glm::dot(toPoint, axisNorm);
        if (projLen < 0.0f || projLen > height)
            return false;

        // Radius at this height
        float radiusAtHeight = radius * (1.0f - projLen / height);

        // Check radial distance
        Vec3 proj = base + axisNorm * projLen;
        float radialDistSq = glm::dot(point - proj, point - proj);
        return radialDistSq <= radiusAtHeight * radiusAtHeight;
    }

    /**
     * @brief Get the closest point on the cone surface to a given point
     * (Approximate - returns closest point on the surface)
     */
    Vec3 ClosestPoint(const Vec3& point) const
    {
        Vec3 axis = apex - base;
        float height = glm::length(axis);
        if (height < EPSILON) return apex;

        Vec3 axisNorm = axis / height;
        Vec3 toPoint = point - base;

        // Project onto axis
        float projLen = glm::dot(toPoint, axisNorm);
        Vec3 axisPoint = base + axisNorm * std::clamp(projLen, 0.0f, height);
        Vec3 radial = point - axisPoint;
        float radialDist = glm::length(radial);

        // If on the axis, pick arbitrary direction
        if (radialDist < EPSILON)
        {
            Vec3 perp = std::abs(axisNorm.x) < 0.9f
                ? glm::cross(axisNorm, Vec3(1, 0, 0))
                : glm::cross(axisNorm, Vec3(0, 1, 0));
            radial = glm::normalize(perp);
            radialDist = 1.0f;
        }

        Vec3 radialNorm = radial / radialDist;
        projLen = std::clamp(projLen, 0.0f, height);
        float radiusAtHeight = radius * (1.0f - projLen / height);

        if (projLen <= 0.0f)
        {
            // Closest to base
            float clampedRadial = std::min(radialDist, radius);
            return base + radialNorm * clampedRadial;
        }

        if (projLen >= height)
        {
            // Closest to apex
            return apex;
        }

        // On the lateral surface
        return axisPoint + radialNorm * std::min(radialDist, radiusAtHeight);
    }

    // =========================================================================
    // Support Function (for GJK)
    // =========================================================================

    /**
     * @brief Get the support point in a given direction
     */
    Vec3 Support(const Vec3& direction) const
    {
        float dirLen = glm::length(direction);
        if (dirLen < EPSILON)
            return apex;

        Vec3 normDir = direction / dirLen;
        Vec3 axis = GetAxisNormalized();

        // Check apex
        float dotApex = glm::dot(apex, normDir);

        // Check points on base circle
        Vec3 radialDir = normDir - axis * glm::dot(normDir, axis);
        float radialLen = glm::length(radialDir);

        Vec3 basePoint;
        if (radialLen > EPSILON)
        {
            basePoint = base + (radialDir / radialLen) * radius;
        }
        else
        {
            basePoint = base;
        }

        float dotBase = glm::dot(basePoint, normDir);

        return (dotApex > dotBase) ? apex : basePoint;
    }
};

} // namespace RVX::Geometry
