/**
 * @file Line.h
 * @brief Line and line segment primitives
 */

#pragma once

#include "Core/MathTypes.h"
#include "Core/Math/AABB.h"
#include "Geometry/Constants.h"
#include <algorithm>

namespace RVX::Geometry
{

/**
 * @brief Infinite line defined by a point and direction
 */
struct Line
{
    Vec3 origin{0.0f};
    Vec3 direction{0.0f, 0.0f, 1.0f};  ///< Should be normalized

    Line() = default;

    Line(const Vec3& o, const Vec3& d)
        : origin(o)
        , direction(glm::normalize(d))
    {}

    /**
     * @brief Get point at parameter t along the line
     */
    Vec3 PointAt(float t) const
    {
        return origin + direction * t;
    }

    /**
     * @brief Project a point onto the line
     * @return Parameter t such that PointAt(t) is the closest point
     */
    float ProjectPoint(const Vec3& point) const
    {
        return glm::dot(point - origin, direction);
    }

    /**
     * @brief Get the closest point on the line to a given point
     */
    Vec3 ClosestPoint(const Vec3& point) const
    {
        return PointAt(ProjectPoint(point));
    }

    /**
     * @brief Squared distance from a point to the line
     */
    float DistanceSquared(const Vec3& point) const
    {
        Vec3 closest = ClosestPoint(point);
        Vec3 diff = point - closest;
        return glm::dot(diff, diff);
    }

    /**
     * @brief Distance from a point to the line
     */
    float Distance(const Vec3& point) const
    {
        return std::sqrt(DistanceSquared(point));
    }
};

/**
 * @brief Line segment defined by two endpoints
 */
struct Segment
{
    Vec3 a{0.0f};  ///< Start point
    Vec3 b{0.0f};  ///< End point

    Segment() = default;

    Segment(const Vec3& start, const Vec3& end)
        : a(start)
        , b(end)
    {}

    /**
     * @brief Get the direction vector (not normalized)
     */
    Vec3 GetDirection() const
    {
        return b - a;
    }

    /**
     * @brief Get the normalized direction
     */
    Vec3 GetNormalizedDirection() const
    {
        Vec3 dir = b - a;
        float len = glm::length(dir);
        return len > EPSILON ? dir / len : Vec3(0.0f);
    }

    /**
     * @brief Get the length of the segment
     */
    float GetLength() const
    {
        return glm::length(b - a);
    }

    /**
     * @brief Get the squared length
     */
    float GetLengthSquared() const
    {
        Vec3 d = b - a;
        return glm::dot(d, d);
    }

    /**
     * @brief Get the center point
     */
    Vec3 GetCenter() const
    {
        return (a + b) * 0.5f;
    }

    /**
     * @brief Get point at parameter t (0 = a, 1 = b)
     */
    Vec3 PointAt(float t) const
    {
        return a + (b - a) * t;
    }

    /**
     * @brief Project a point onto the segment line
     * @return Parameter t clamped to [0, 1]
     */
    float ProjectPoint(const Vec3& point) const
    {
        Vec3 ab = b - a;
        float lengthSq = glm::dot(ab, ab);
        if (lengthSq < DEGENERATE_TOLERANCE)
            return 0.0f;

        float t = glm::dot(point - a, ab) / lengthSq;
        return std::clamp(t, 0.0f, 1.0f);
    }

    /**
     * @brief Get the closest point on the segment to a given point
     */
    Vec3 ClosestPoint(const Vec3& point) const
    {
        return PointAt(ProjectPoint(point));
    }

    /**
     * @brief Squared distance from a point to the segment
     */
    float DistanceSquared(const Vec3& point) const
    {
        Vec3 closest = ClosestPoint(point);
        Vec3 diff = point - closest;
        return glm::dot(diff, diff);
    }

    /**
     * @brief Distance from a point to the segment
     */
    float Distance(const Vec3& point) const
    {
        return std::sqrt(DistanceSquared(point));
    }

    /**
     * @brief Get axis-aligned bounding box
     */
    AABB GetBoundingBox() const
    {
        return AABB(glm::min(a, b), glm::max(a, b));
    }

    /**
     * @brief Convert to infinite line
     */
    Line ToLine() const
    {
        return Line(a, b - a);
    }
};

// ============================================================================
// Segment-Segment Distance
// ============================================================================

/**
 * @brief Compute the closest points between two line segments
 * @param s1 First segment
 * @param s2 Second segment
 * @param outS Parameter on first segment [0,1]
 * @param outT Parameter on second segment [0,1]
 * @return Squared distance between the segments
 */
inline float SegmentSegmentDistanceSquared(
    const Segment& s1,
    const Segment& s2,
    float& outS,
    float& outT)
{
    Vec3 d1 = s1.b - s1.a;
    Vec3 d2 = s2.b - s2.a;
    Vec3 r = s1.a - s2.a;

    float a = glm::dot(d1, d1);
    float e = glm::dot(d2, d2);
    float f = glm::dot(d2, r);

    // Check if both segments degenerate into points
    if (a < DEGENERATE_TOLERANCE && e < DEGENERATE_TOLERANCE)
    {
        outS = outT = 0.0f;
        Vec3 diff = s1.a - s2.a;
        return glm::dot(diff, diff);
    }

    // First segment degenerates into a point
    if (a < DEGENERATE_TOLERANCE)
    {
        outS = 0.0f;
        outT = std::clamp(f / e, 0.0f, 1.0f);
    }
    else
    {
        float c = glm::dot(d1, r);

        // Second segment degenerates into a point
        if (e < DEGENERATE_TOLERANCE)
        {
            outT = 0.0f;
            outS = std::clamp(-c / a, 0.0f, 1.0f);
        }
        else
        {
            // General case
            float b = glm::dot(d1, d2);
            float denom = a * e - b * b;

            if (denom > DEGENERATE_TOLERANCE)
            {
                outS = std::clamp((b * f - c * e) / denom, 0.0f, 1.0f);
            }
            else
            {
                outS = 0.0f;
            }

            // Compute t for point on L2 closest to S1(s)
            outT = (b * outS + f) / e;

            // Clamp t and recompute s if needed
            if (outT < 0.0f)
            {
                outT = 0.0f;
                outS = std::clamp(-c / a, 0.0f, 1.0f);
            }
            else if (outT > 1.0f)
            {
                outT = 1.0f;
                outS = std::clamp((b - c) / a, 0.0f, 1.0f);
            }
        }
    }

    Vec3 c1 = s1.PointAt(outS);
    Vec3 c2 = s2.PointAt(outT);
    Vec3 diff = c1 - c2;
    return glm::dot(diff, diff);
}

/**
 * @brief Compute distance between two line segments
 */
inline float SegmentSegmentDistance(
    const Segment& s1,
    const Segment& s2,
    float& outS,
    float& outT)
{
    return std::sqrt(SegmentSegmentDistanceSquared(s1, s2, outS, outT));
}

} // namespace RVX::Geometry
