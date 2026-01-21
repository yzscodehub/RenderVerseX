/**
 * @file Frustum.h
 * @brief View frustum for visibility culling
 */

#pragma once

#include "Core/MathTypes.h"
#include "Core/Math/Plane.h"
#include "Core/Math/AABB.h"
#include "Core/Math/Sphere.h"
#include <array>

namespace RVX
{

/**
 * @brief Frustum plane indices
 */
enum class FrustumPlane : int
{
    Near = 0,
    Far = 1,
    Left = 2,
    Right = 3,
    Top = 4,
    Bottom = 5,
    Count = 6
};

/**
 * @brief Intersection result
 */
enum class IntersectionResult
{
    Outside,    // Completely outside
    Inside,     // Completely inside
    Intersects  // Partially intersects
};

/**
 * @brief View frustum for visibility culling
 * 
 * Supports:
 * - Extraction from view-projection matrix
 * - Construction from camera parameters
 * - Intersection tests with AABB, sphere, and points
 */
class Frustum
{
public:
    Frustum() = default;

    // =========================================================================
    // Construction
    // =========================================================================

    /// Extract frustum planes from a view-projection matrix
    void ExtractFromMatrix(const Mat4& viewProj)
    {
        // Left plane
        m_planes[static_cast<int>(FrustumPlane::Left)] = Plane(
            viewProj[0][3] + viewProj[0][0],
            viewProj[1][3] + viewProj[1][0],
            viewProj[2][3] + viewProj[2][0],
            viewProj[3][3] + viewProj[3][0]
        );

        // Right plane
        m_planes[static_cast<int>(FrustumPlane::Right)] = Plane(
            viewProj[0][3] - viewProj[0][0],
            viewProj[1][3] - viewProj[1][0],
            viewProj[2][3] - viewProj[2][0],
            viewProj[3][3] - viewProj[3][0]
        );

        // Bottom plane
        m_planes[static_cast<int>(FrustumPlane::Bottom)] = Plane(
            viewProj[0][3] + viewProj[0][1],
            viewProj[1][3] + viewProj[1][1],
            viewProj[2][3] + viewProj[2][1],
            viewProj[3][3] + viewProj[3][1]
        );

        // Top plane
        m_planes[static_cast<int>(FrustumPlane::Top)] = Plane(
            viewProj[0][3] - viewProj[0][1],
            viewProj[1][3] - viewProj[1][1],
            viewProj[2][3] - viewProj[2][1],
            viewProj[3][3] - viewProj[3][1]
        );

        // Near plane
        m_planes[static_cast<int>(FrustumPlane::Near)] = Plane(
            viewProj[0][3] + viewProj[0][2],
            viewProj[1][3] + viewProj[1][2],
            viewProj[2][3] + viewProj[2][2],
            viewProj[3][3] + viewProj[3][2]
        );

        // Far plane
        m_planes[static_cast<int>(FrustumPlane::Far)] = Plane(
            viewProj[0][3] - viewProj[0][2],
            viewProj[1][3] - viewProj[1][2],
            viewProj[2][3] - viewProj[2][2],
            viewProj[3][3] - viewProj[3][2]
        );
    }

    /// Construct from camera parameters
    void SetPerspective(const Vec3& position, const Vec3& forward,
                       const Vec3& up, float fovY, float aspect,
                       float nearZ, float farZ)
    {
        Mat4 view = glm::lookAt(position, position + forward, up);
        Mat4 proj = glm::perspective(fovY, aspect, nearZ, farZ);
        ExtractFromMatrix(proj * view);
    }

    // =========================================================================
    // Intersection Tests
    // =========================================================================

    /// Test if a point is inside the frustum
    bool Contains(const Vec3& point) const
    {
        for (int i = 0; i < 6; ++i)
        {
            if (m_planes[i].SignedDistance(point) < 0.0f)
                return false;
        }
        return true;
    }

    /// Test if a bounding box intersects the frustum
    IntersectionResult Intersects(const AABB& box) const
    {
        if (!box.IsValid()) return IntersectionResult::Outside;

        bool allInside = true;

        for (int i = 0; i < 6; ++i)
        {
            const Plane& plane = m_planes[i];

            // Find the positive vertex (furthest along plane normal)
            Vec3 pVertex = box.GetMin();
            if (plane.normal.x >= 0) pVertex.x = box.GetMax().x;
            if (plane.normal.y >= 0) pVertex.y = box.GetMax().y;
            if (plane.normal.z >= 0) pVertex.z = box.GetMax().z;

            // Find the negative vertex
            Vec3 nVertex = box.GetMax();
            if (plane.normal.x >= 0) nVertex.x = box.GetMin().x;
            if (plane.normal.y >= 0) nVertex.y = box.GetMin().y;
            if (plane.normal.z >= 0) nVertex.z = box.GetMin().z;

            // If the positive vertex is behind the plane, box is outside
            if (plane.SignedDistance(pVertex) < 0.0f)
                return IntersectionResult::Outside;

            // If the negative vertex is behind the plane, box is not fully inside
            if (plane.SignedDistance(nVertex) < 0.0f)
                allInside = false;
        }

        return allInside ? IntersectionResult::Inside : IntersectionResult::Intersects;
    }

    /// Quick test if a bounding box is visible (not outside)
    bool IsVisible(const AABB& box) const
    {
        return Intersects(box) != IntersectionResult::Outside;
    }

    /// Test if a bounding sphere intersects the frustum
    IntersectionResult Intersects(const Sphere& sphere) const
    {
        if (!sphere.IsValid()) return IntersectionResult::Outside;

        bool allInside = true;

        for (int i = 0; i < 6; ++i)
        {
            float dist = m_planes[i].SignedDistance(sphere.GetCenter());
            
            if (dist < -sphere.GetRadius())
                return IntersectionResult::Outside;
            
            if (dist < sphere.GetRadius())
                allInside = false;
        }

        return allInside ? IntersectionResult::Inside : IntersectionResult::Intersects;
    }

    /// Quick test if a sphere is visible
    bool IsVisible(const Sphere& sphere) const
    {
        return Intersects(sphere) != IntersectionResult::Outside;
    }

    // =========================================================================
    // Accessors
    // =========================================================================

    const Plane& GetPlane(FrustumPlane index) const
    {
        return m_planes[static_cast<int>(index)];
    }

    const Plane& GetPlane(int index) const
    {
        return m_planes[index];
    }

private:
    std::array<Plane, 6> m_planes;
};

} // namespace RVX
