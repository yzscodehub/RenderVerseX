/**
 * @file Triangle.h
 * @brief Triangle primitive for ray tracing and physics
 */

#pragma once

#include "Core/MathTypes.h"
#include "Core/Math/AABB.h"
#include "Geometry/Constants.h"
#include <cmath>

namespace RVX::Geometry
{

/**
 * @brief Triangle defined by three vertices
 * 
 * Uses counter-clockwise winding order for normal calculation.
 * Commonly used for:
 * - Ray-triangle intersection in ray tracing
 * - Mesh collision detection
 * - Barycentric coordinate calculations
 */
struct Triangle
{
    Vec3 v0{0.0f};
    Vec3 v1{1.0f, 0.0f, 0.0f};
    Vec3 v2{0.0f, 1.0f, 0.0f};

    Triangle() = default;

    Triangle(const Vec3& a, const Vec3& b, const Vec3& c)
        : v0(a), v1(b), v2(c)
    {}

    // =========================================================================
    // Basic Properties
    // =========================================================================

    /**
     * @brief Get the geometric normal (unnormalized)
     */
    Vec3 GetNormalUnnormalized() const
    {
        return glm::cross(v1 - v0, v2 - v0);
    }

    /**
     * @brief Get the geometric normal (normalized)
     */
    Vec3 GetNormal() const
    {
        Vec3 n = GetNormalUnnormalized();
        float len = glm::length(n);
        return len > EPSILON ? n / len : Vec3(0.0f, 1.0f, 0.0f);
    }

    /**
     * @brief Get the center (centroid) of the triangle
     */
    Vec3 GetCenter() const
    {
        return (v0 + v1 + v2) / 3.0f;
    }

    /**
     * @brief Calculate the area of the triangle
     */
    float GetArea() const
    {
        return 0.5f * glm::length(GetNormalUnnormalized());
    }

    /**
     * @brief Get the perimeter of the triangle
     */
    float GetPerimeter() const
    {
        return glm::length(v1 - v0) + glm::length(v2 - v1) + glm::length(v0 - v2);
    }

    /**
     * @brief Check if the triangle is degenerate (zero area)
     */
    bool IsDegenerate() const
    {
        return GetArea() < DEGENERATE_TOLERANCE;
    }

    // =========================================================================
    // Bounding Volume
    // =========================================================================

    /**
     * @brief Get axis-aligned bounding box
     */
    AABB GetBoundingBox() const
    {
        Vec3 minPt = glm::min(glm::min(v0, v1), v2);
        Vec3 maxPt = glm::max(glm::max(v0, v1), v2);
        return AABB(minPt, maxPt);
    }

    // =========================================================================
    // Barycentric Coordinates
    // =========================================================================

    /**
     * @brief Compute barycentric coordinates for a point
     * @param point Point to compute coordinates for
     * @return Vec3(u, v, w) where u + v + w = 1
     */
    Vec3 GetBarycentric(const Vec3& point) const
    {
        Vec3 e0 = v1 - v0;
        Vec3 e1 = v2 - v0;
        Vec3 e2 = point - v0;

        float d00 = glm::dot(e0, e0);
        float d01 = glm::dot(e0, e1);
        float d11 = glm::dot(e1, e1);
        float d20 = glm::dot(e2, e0);
        float d21 = glm::dot(e2, e1);

        float denom = d00 * d11 - d01 * d01;
        if (std::abs(denom) < DEGENERATE_TOLERANCE)
        {
            return Vec3(1.0f, 0.0f, 0.0f);
        }

        float invDenom = 1.0f / denom;
        float v = (d11 * d20 - d01 * d21) * invDenom;
        float w = (d00 * d21 - d01 * d20) * invDenom;
        float u = 1.0f - v - w;

        return Vec3(u, v, w);
    }

    /**
     * @brief Check if barycentric coordinates represent a point inside the triangle
     */
    static bool IsInsideBarycentric(const Vec3& bary)
    {
        return bary.x >= 0.0f && bary.y >= 0.0f && bary.z >= 0.0f;
    }

    /**
     * @brief Interpolate a value using barycentric coordinates
     */
    template<typename T>
    static T InterpolateBarycentric(const T& a0, const T& a1, const T& a2, const Vec3& bary)
    {
        return a0 * bary.x + a1 * bary.y + a2 * bary.z;
    }

    /**
     * @brief Get point from barycentric coordinates
     */
    Vec3 PointFromBarycentric(const Vec3& bary) const
    {
        return v0 * bary.x + v1 * bary.y + v2 * bary.z;
    }

    // =========================================================================
    // Point Queries
    // =========================================================================

    /**
     * @brief Check if a point lies on the triangle plane
     */
    bool IsPointOnPlane(const Vec3& point, float tolerance = PLANE_THICKNESS) const
    {
        Vec3 n = GetNormal();
        float d = glm::dot(n, point - v0);
        return std::abs(d) <= tolerance;
    }

    /**
     * @brief Check if a point is inside the triangle (assumes point is on plane)
     */
    bool ContainsPoint(const Vec3& point) const
    {
        Vec3 bary = GetBarycentric(point);
        return IsInsideBarycentric(bary);
    }

    /**
     * @brief Get the closest point on the triangle to a given point
     */
    Vec3 ClosestPoint(const Vec3& point) const
    {
        // Check if point projects inside the triangle
        Vec3 ab = v1 - v0;
        Vec3 ac = v2 - v0;
        Vec3 ap = point - v0;

        float d1 = glm::dot(ab, ap);
        float d2 = glm::dot(ac, ap);
        if (d1 <= 0.0f && d2 <= 0.0f)
            return v0;  // Closest to v0

        Vec3 bp = point - v1;
        float d3 = glm::dot(ab, bp);
        float d4 = glm::dot(ac, bp);
        if (d3 >= 0.0f && d4 <= d3)
            return v1;  // Closest to v1

        float vc = d1 * d4 - d3 * d2;
        if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
        {
            float v = d1 / (d1 - d3);
            return v0 + ab * v;  // Closest on edge v0-v1
        }

        Vec3 cp = point - v2;
        float d5 = glm::dot(ab, cp);
        float d6 = glm::dot(ac, cp);
        if (d6 >= 0.0f && d5 <= d6)
            return v2;  // Closest to v2

        float vb = d5 * d2 - d1 * d6;
        if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
        {
            float w = d2 / (d2 - d6);
            return v0 + ac * w;  // Closest on edge v0-v2
        }

        float va = d3 * d6 - d5 * d4;
        if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
        {
            float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
            return v1 + (v2 - v1) * w;  // Closest on edge v1-v2
        }

        // Point projects inside the triangle
        float denom = 1.0f / (va + vb + vc);
        float v = vb * denom;
        float w = vc * denom;
        return v0 + ab * v + ac * w;
    }

    /**
     * @brief Squared distance from a point to the triangle
     */
    float DistanceSquared(const Vec3& point) const
    {
        Vec3 closest = ClosestPoint(point);
        Vec3 diff = point - closest;
        return glm::dot(diff, diff);
    }

    /**
     * @brief Distance from a point to the triangle
     */
    float Distance(const Vec3& point) const
    {
        return std::sqrt(DistanceSquared(point));
    }

    // =========================================================================
    // Edge Access
    // =========================================================================

    /**
     * @brief Get edge by index (0, 1, or 2)
     * @return Start and end points of the edge
     */
    std::pair<Vec3, Vec3> GetEdge(int index) const
    {
        switch (index % 3)
        {
            case 0: return {v0, v1};
            case 1: return {v1, v2};
            default: return {v2, v0};
        }
    }

    /**
     * @brief Get vertex by index (0, 1, or 2)
     */
    const Vec3& GetVertex(int index) const
    {
        switch (index % 3)
        {
            case 0: return v0;
            case 1: return v1;
            default: return v2;
        }
    }

    Vec3& GetVertex(int index)
    {
        switch (index % 3)
        {
            case 0: return v0;
            case 1: return v1;
            default: return v2;
        }
    }
};

} // namespace RVX::Geometry
