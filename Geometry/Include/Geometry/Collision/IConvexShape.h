/**
 * @file IConvexShape.h
 * @brief Interface for convex shapes used in GJK/EPA
 */

#pragma once

#include "Core/MathTypes.h"
#include <vector>
#include <span>
#include <limits>

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

/**
 * @brief Convex shape wrapper for convex hull (point cloud)
 * 
 * Represents a convex hull defined by a set of vertices.
 * The vertices should form the convex hull - internal points are ignored
 * but waste computation time.
 */
class ConvexHull : public IConvexShape
{
public:
    std::vector<Vec3> vertices;
    Vec3 cachedCenter{0};

    ConvexHull() = default;

    ConvexHull(std::span<const Vec3> verts)
        : vertices(verts.begin(), verts.end())
    {
        ComputeCenter();
    }

    ConvexHull(std::initializer_list<Vec3> verts)
        : vertices(verts)
    {
        ComputeCenter();
    }

    /**
     * @brief Set vertices and recompute center
     */
    void SetVertices(std::span<const Vec3> verts)
    {
        vertices.assign(verts.begin(), verts.end());
        ComputeCenter();
    }

    /**
     * @brief Add a vertex to the hull
     */
    void AddVertex(const Vec3& v)
    {
        vertices.push_back(v);
        ComputeCenter();
    }

    /**
     * @brief Clear all vertices
     */
    void Clear()
    {
        vertices.clear();
        cachedCenter = Vec3(0);
    }

    Vec3 Support(const Vec3& direction) const override
    {
        if (vertices.empty())
            return Vec3(0);

        float maxDot = -std::numeric_limits<float>::max();
        Vec3 support = vertices[0];

        for (const auto& v : vertices)
        {
            float d = glm::dot(v, direction);
            if (d > maxDot)
            {
                maxDot = d;
                support = v;
            }
        }

        return support;
    }

    Vec3 GetCenter() const override
    {
        return cachedCenter;
    }

    /**
     * @brief Transform all vertices by a matrix
     */
    ConvexHull Transformed(const Mat4& transform) const
    {
        ConvexHull result;
        result.vertices.reserve(vertices.size());

        for (const auto& v : vertices)
        {
            result.vertices.push_back(Vec3(transform * Vec4(v, 1.0f)));
        }
        result.ComputeCenter();

        return result;
    }

    /**
     * @brief Get axis-aligned bounding box
     */
    void GetBounds(Vec3& outMin, Vec3& outMax) const
    {
        if (vertices.empty())
        {
            outMin = outMax = Vec3(0);
            return;
        }

        outMin = outMax = vertices[0];
        for (size_t i = 1; i < vertices.size(); ++i)
        {
            outMin = glm::min(outMin, vertices[i]);
            outMax = glm::max(outMax, vertices[i]);
        }
    }

private:
    void ComputeCenter()
    {
        if (vertices.empty())
        {
            cachedCenter = Vec3(0);
            return;
        }

        Vec3 sum(0);
        for (const auto& v : vertices)
        {
            sum += v;
        }
        cachedCenter = sum / static_cast<float>(vertices.size());
    }
};

/**
 * @brief Convex shape wrapper for a Box (AABB treated as convex shape)
 */
class ConvexBox : public IConvexShape
{
public:
    Vec3 center{0};
    Vec3 halfExtents{1};

    ConvexBox() = default;
    ConvexBox(const Vec3& c, const Vec3& extents)
        : center(c), halfExtents(extents) {}

    Vec3 Support(const Vec3& direction) const override
    {
        return center + Vec3(
            direction.x >= 0.0f ? halfExtents.x : -halfExtents.x,
            direction.y >= 0.0f ? halfExtents.y : -halfExtents.y,
            direction.z >= 0.0f ? halfExtents.z : -halfExtents.z
        );
    }

    Vec3 GetCenter() const override { return center; }
};

/**
 * @brief Convex shape wrapper for a Triangle
 */
class ConvexTriangle : public IConvexShape
{
public:
    Vec3 v0{0}, v1{1, 0, 0}, v2{0, 1, 0};

    ConvexTriangle() = default;
    ConvexTriangle(const Vec3& a, const Vec3& b, const Vec3& c)
        : v0(a), v1(b), v2(c) {}

    Vec3 Support(const Vec3& direction) const override
    {
        float d0 = glm::dot(v0, direction);
        float d1 = glm::dot(v1, direction);
        float d2 = glm::dot(v2, direction);

        if (d0 >= d1 && d0 >= d2) return v0;
        if (d1 >= d0 && d1 >= d2) return v1;
        return v2;
    }

    Vec3 GetCenter() const override
    {
        return (v0 + v1 + v2) / 3.0f;
    }
};

} // namespace RVX::Geometry
