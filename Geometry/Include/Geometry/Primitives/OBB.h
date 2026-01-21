/**
 * @file OBB.h
 * @brief Oriented Bounding Box primitive
 */

#pragma once

#include "Core/MathTypes.h"
#include "Core/Math/AABB.h"
#include "Core/Math/Sphere.h"
#include "Geometry/Constants.h"
#include <array>

namespace RVX::Geometry
{

/**
 * @brief Oriented Bounding Box (OBB)
 * 
 * An axis-aligned box that has been rotated. Commonly used for:
 * - Tighter collision bounds for rotated objects
 * - Physics simulation
 * - Visibility culling of non-axis-aligned objects
 */
struct OBB
{
    Vec3 center{0.0f};                      ///< Center of the box
    Vec3 halfExtents{1.0f};                 ///< Half-size along each local axis
    Quat orientation{1.0f, 0.0f, 0.0f, 0.0f};  ///< Rotation quaternion

    OBB() = default;

    OBB(const Vec3& c, const Vec3& extents, const Quat& orient = Quat(1.0f, 0.0f, 0.0f, 0.0f))
        : center(c)
        , halfExtents(extents)
        , orientation(orient)
    {}

    /**
     * @brief Construct from AABB (creates axis-aligned OBB)
     */
    explicit OBB(const AABB& aabb)
        : center(aabb.GetCenter())
        , halfExtents(aabb.GetExtent())
        , orientation(1.0f, 0.0f, 0.0f, 0.0f)
    {}

    // =========================================================================
    // Axes
    // =========================================================================

    /**
     * @brief Get the local X axis in world space
     */
    Vec3 GetAxisX() const
    {
        return orientation * Vec3(1.0f, 0.0f, 0.0f);
    }

    /**
     * @brief Get the local Y axis in world space
     */
    Vec3 GetAxisY() const
    {
        return orientation * Vec3(0.0f, 1.0f, 0.0f);
    }

    /**
     * @brief Get the local Z axis in world space
     */
    Vec3 GetAxisZ() const
    {
        return orientation * Vec3(0.0f, 0.0f, 1.0f);
    }

    /**
     * @brief Get all three axes
     */
    void GetAxes(Vec3& axisX, Vec3& axisY, Vec3& axisZ) const
    {
        axisX = GetAxisX();
        axisY = GetAxisY();
        axisZ = GetAxisZ();
    }

    /**
     * @brief Get the rotation matrix
     */
    Mat3 GetRotationMatrix() const
    {
        return glm::mat3_cast(orientation);
    }

    // =========================================================================
    // Corners
    // =========================================================================

    /**
     * @brief Get a corner of the OBB by index (0-7)
     * 
     * Corner indices:
     * 0: (-x, -y, -z), 1: (+x, -y, -z)
     * 2: (-x, +y, -z), 3: (+x, +y, -z)
     * 4: (-x, -y, +z), 5: (+x, -y, +z)
     * 6: (-x, +y, +z), 7: (+x, +y, +z)
     */
    Vec3 GetCorner(int index) const
    {
        Vec3 local(
            (index & 1) ? halfExtents.x : -halfExtents.x,
            (index & 2) ? halfExtents.y : -halfExtents.y,
            (index & 4) ? halfExtents.z : -halfExtents.z
        );
        return center + orientation * local;
    }

    /**
     * @brief Get all 8 corners
     */
    std::array<Vec3, 8> GetCorners() const
    {
        std::array<Vec3, 8> corners;
        for (int i = 0; i < 8; ++i)
        {
            corners[i] = GetCorner(i);
        }
        return corners;
    }

    // =========================================================================
    // Transforms
    // =========================================================================

    /**
     * @brief Transform a world-space point to local OBB space
     */
    Vec3 WorldToLocal(const Vec3& worldPoint) const
    {
        Quat invOrient = glm::conjugate(orientation);
        return invOrient * (worldPoint - center);
    }

    /**
     * @brief Transform a local-space point to world space
     */
    Vec3 LocalToWorld(const Vec3& localPoint) const
    {
        return center + orientation * localPoint;
    }

    // =========================================================================
    // Bounding Volumes
    // =========================================================================

    /**
     * @brief Get axis-aligned bounding box that contains this OBB
     */
    AABB ToAABB() const
    {
        auto corners = GetCorners();
        AABB result;
        for (const auto& corner : corners)
        {
            result.Expand(corner);
        }
        return result;
    }

    /**
     * @brief Get bounding sphere
     */
    Sphere GetBoundingSphere() const
    {
        float radius = glm::length(halfExtents);
        return Sphere(center, radius);
    }

    // =========================================================================
    // Metrics
    // =========================================================================

    /**
     * @brief Get the volume of the OBB
     */
    float GetVolume() const
    {
        return 8.0f * halfExtents.x * halfExtents.y * halfExtents.z;
    }

    /**
     * @brief Get the surface area of the OBB
     */
    float GetSurfaceArea() const
    {
        return 8.0f * (halfExtents.x * halfExtents.y +
                       halfExtents.y * halfExtents.z +
                       halfExtents.z * halfExtents.x);
    }

    // =========================================================================
    // Point Queries
    // =========================================================================

    /**
     * @brief Check if a point is inside the OBB
     */
    bool Contains(const Vec3& point) const
    {
        Vec3 local = WorldToLocal(point);
        return std::abs(local.x) <= halfExtents.x &&
               std::abs(local.y) <= halfExtents.y &&
               std::abs(local.z) <= halfExtents.z;
    }

    /**
     * @brief Get the closest point on/in the OBB to a given point
     */
    Vec3 ClosestPoint(const Vec3& point) const
    {
        Vec3 local = WorldToLocal(point);

        // Clamp to box extents
        local.x = std::clamp(local.x, -halfExtents.x, halfExtents.x);
        local.y = std::clamp(local.y, -halfExtents.y, halfExtents.y);
        local.z = std::clamp(local.z, -halfExtents.z, halfExtents.z);

        return LocalToWorld(local);
    }

    /**
     * @brief Squared distance from a point to the OBB
     */
    float DistanceSquared(const Vec3& point) const
    {
        Vec3 local = WorldToLocal(point);
        float sqDist = 0.0f;

        // For each axis, add squared distance if outside
        for (int i = 0; i < 3; ++i)
        {
            float v = local[i];
            float extent = halfExtents[i];
            if (v < -extent)
                sqDist += Square(v + extent);
            else if (v > extent)
                sqDist += Square(v - extent);
        }

        return sqDist;
    }

    /**
     * @brief Distance from a point to the OBB
     */
    float Distance(const Vec3& point) const
    {
        return std::sqrt(DistanceSquared(point));
    }

    // =========================================================================
    // Support Function (for GJK)
    // =========================================================================

    /**
     * @brief Get the support point in a given direction
     * @param direction World-space direction (doesn't need to be normalized)
     * @return The point on the OBB surface furthest in the given direction
     */
    Vec3 Support(const Vec3& direction) const
    {
        // Transform direction to local space
        Vec3 localDir = glm::conjugate(orientation) * direction;

        // Find support in local space (just use signs)
        Vec3 localSupport(
            localDir.x >= 0.0f ? halfExtents.x : -halfExtents.x,
            localDir.y >= 0.0f ? halfExtents.y : -halfExtents.y,
            localDir.z >= 0.0f ? halfExtents.z : -halfExtents.z
        );

        // Transform back to world space
        return LocalToWorld(localSupport);
    }

    // =========================================================================
    // Transformation
    // =========================================================================

    /**
     * @brief Transform the OBB by a matrix
     * Note: Non-uniform scale will produce incorrect results
     */
    OBB Transformed(const Mat4& transform) const
    {
        // Extract translation
        Vec3 newCenter = Vec3(transform * Vec4(center, 1.0f));

        // Extract rotation (assumes no scale or uniform scale only)
        Mat3 rotMat = Mat3(transform);
        Quat newOrient = glm::quat_cast(rotMat) * orientation;

        // For uniform scale, we could extract and apply to extents
        // For now, assume no scale
        return OBB(newCenter, halfExtents, newOrient);
    }

    /**
     * @brief Create OBB from transform matrix and local extents
     */
    static OBB FromTransform(const Mat4& transform, const Vec3& localExtents)
    {
        Vec3 center = Vec3(transform[3]);
        Quat orient = glm::quat_cast(Mat3(transform));
        return OBB(center, localExtents, orient);
    }
};

} // namespace RVX::Geometry
