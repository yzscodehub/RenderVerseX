/**
 * @file IConvexShape.h
 * @brief Interface for convex shapes used in GJK/EPA
 */

#pragma once

#include "Core/MathTypes.h"

namespace RVX::Geometry
{

/**
 * @brief Interface for convex shapes that can be used with GJK/EPA
 * 
 * Any shape that implements the Support function can be used with
 * the GJK algorithm for collision detection and EPA for penetration depth.
 */
class IConvexShape
{
public:
    virtual ~IConvexShape() = default;

    /**
     * @brief Get the support point in a given direction
     * 
     * The support point is the point on the shape's surface that is
     * furthest in the given direction.
     * 
     * @param direction World-space direction (does not need to be normalized)
     * @return The support point in world space
     */
    virtual Vec3 Support(const Vec3& direction) const = 0;

    /**
     * @brief Get the center of the shape
     * 
     * Used as an initial guess for GJK.
     */
    virtual Vec3 GetCenter() const = 0;
};

/**
 * @brief Convex shape wrapper for Sphere
 */
class ConvexSphere : public IConvexShape
{
public:
    Vec3 center{0};
    float radius{1.0f};

    ConvexSphere() = default;
    ConvexSphere(const Vec3& c, float r) : center(c), radius(r) {}

    Vec3 Support(const Vec3& direction) const override
    {
        float len = glm::length(direction);
        if (len < 1e-8f)
            return center + Vec3(radius, 0, 0);
        return center + (direction / len) * radius;
    }

    Vec3 GetCenter() const override { return center; }
};

/**
 * @brief Convex shape wrapper for OBB
 */
class ConvexOBB : public IConvexShape
{
public:
    Vec3 center{0};
    Vec3 halfExtents{1};
    Quat orientation{1, 0, 0, 0};

    ConvexOBB() = default;
    ConvexOBB(const Vec3& c, const Vec3& extents, const Quat& orient)
        : center(c), halfExtents(extents), orientation(orient) {}

    Vec3 Support(const Vec3& direction) const override
    {
        // Transform direction to local space
        Vec3 localDir = glm::conjugate(orientation) * direction;

        // Support in local space
        Vec3 localSupport(
            localDir.x >= 0.0f ? halfExtents.x : -halfExtents.x,
            localDir.y >= 0.0f ? halfExtents.y : -halfExtents.y,
            localDir.z >= 0.0f ? halfExtents.z : -halfExtents.z
        );

        // Transform back to world space
        return center + orientation * localSupport;
    }

    Vec3 GetCenter() const override { return center; }
};

/**
 * @brief Convex shape wrapper for Capsule
 */
class ConvexCapsule : public IConvexShape
{
public:
    Vec3 a{0, 0, 0};
    Vec3 b{0, 1, 0};
    float radius{0.5f};

    ConvexCapsule() = default;
    ConvexCapsule(const Vec3& pointA, const Vec3& pointB, float r)
        : a(pointA), b(pointB), radius(r) {}

    Vec3 Support(const Vec3& direction) const override
    {
        float len = glm::length(direction);
        if (len < 1e-8f)
            return a + Vec3(0, radius, 0);

        Vec3 normDir = direction / len;

        // Find which endpoint is furthest in the direction
        float dotA = glm::dot(a, normDir);
        float dotB = glm::dot(b, normDir);

        Vec3 endPoint = (dotA > dotB) ? a : b;
        return endPoint + normDir * radius;
    }

    Vec3 GetCenter() const override { return (a + b) * 0.5f; }
};

/**
 * @brief Convex shape wrapper for Cylinder
 */
class ConvexCylinder : public IConvexShape
{
public:
    Vec3 a{0, 0, 0};
    Vec3 b{0, 1, 0};
    float radius{0.5f};

    ConvexCylinder() = default;
    ConvexCylinder(const Vec3& bottom, const Vec3& top, float r)
        : a(bottom), b(top), radius(r) {}

    Vec3 Support(const Vec3& direction) const override
    {
        Vec3 axis = b - a;
        float axisLen = glm::length(axis);
        if (axisLen < 1e-8f)
            return a;

        Vec3 axisNorm = axis / axisLen;

        float dirLen = glm::length(direction);
        if (dirLen < 1e-8f)
            return a;

        Vec3 normDir = direction / dirLen;

        // Determine which end cap
        float dotA = glm::dot(a, normDir);
        float dotB = glm::dot(b, normDir);
        Vec3 endPoint = (dotA > dotB) ? a : b;

        // Project direction onto plane perpendicular to axis
        Vec3 radialDir = normDir - axisNorm * glm::dot(normDir, axisNorm);
        float radialLen = glm::length(radialDir);

        if (radialLen < 1e-8f)
            return endPoint;

        return endPoint + (radialDir / radialLen) * radius;
    }

    Vec3 GetCenter() const override { return (a + b) * 0.5f; }
};

} // namespace RVX::Geometry
