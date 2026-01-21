/**
 * @file Plane.h
 * @brief 3D plane for geometric calculations
 */

#pragma once

#include "Core/MathTypes.h"

namespace RVX
{

/**
 * @brief 3D plane defined by ax + by + cz + d = 0
 * 
 * The plane equation is: dot(normal, point) + distance = 0
 * Points on the positive side (front) satisfy: dot(normal, point) + distance > 0
 */
struct Plane
{
    Vec3 normal{0, 1, 0};
    float distance{0};

    Plane() = default;
    
    Plane(const Vec3& n, float d) : normal(n), distance(d) {}
    
    Plane(float a, float b, float c, float d) 
        : normal(a, b, c), distance(d) 
    {
        Normalize();
    }

    /// Construct from normal and point on plane
    static Plane FromNormalAndPoint(const Vec3& normal, const Vec3& point)
    {
        Vec3 n = glm::normalize(normal);
        return Plane(n, -glm::dot(n, point));
    }

    /// Construct from three points (counter-clockwise winding)
    static Plane FromPoints(const Vec3& p0, const Vec3& p1, const Vec3& p2)
    {
        Vec3 edge1 = p1 - p0;
        Vec3 edge2 = p2 - p0;
        Vec3 n = glm::normalize(glm::cross(edge1, edge2));
        return Plane(n, -glm::dot(n, p0));
    }

    /// Normalize the plane equation
    void Normalize()
    {
        float len = glm::length(normal);
        if (len > 0.0f)
        {
            float invLen = 1.0f / len;
            normal *= invLen;
            distance *= invLen;
        }
    }

    /// Signed distance from point to plane (positive = front, negative = back)
    float SignedDistance(const Vec3& point) const
    {
        return glm::dot(normal, point) + distance;
    }

    /// Absolute distance from point to plane
    float Distance(const Vec3& point) const
    {
        return std::abs(SignedDistance(point));
    }

    /// Project a point onto the plane
    Vec3 ProjectPoint(const Vec3& point) const
    {
        return point - normal * SignedDistance(point);
    }

    /// Reflect a point across the plane
    Vec3 ReflectPoint(const Vec3& point) const
    {
        return point - 2.0f * normal * SignedDistance(point);
    }

    /// Reflect a direction vector
    Vec3 ReflectDirection(const Vec3& dir) const
    {
        return dir - 2.0f * normal * glm::dot(normal, dir);
    }

    /// Check if a point is on the front side of the plane
    bool IsOnFrontSide(const Vec3& point) const
    {
        return SignedDistance(point) > 0.0f;
    }

    /// Check if a point is on the back side of the plane
    bool IsOnBackSide(const Vec3& point) const
    {
        return SignedDistance(point) < 0.0f;
    }

    /// Get a point on the plane (closest to origin)
    Vec3 GetPoint() const
    {
        return -normal * distance;
    }
};

} // namespace RVX
