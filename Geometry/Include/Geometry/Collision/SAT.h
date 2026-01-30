/**
 * @file SAT.h
 * @brief Separating Axis Theorem for convex shape collision
 */

#pragma once

#include "Core/MathTypes.h"
#include "Geometry/Primitives/OBB.h"
#include "Geometry/Collision/ContactManifold.h"
#include "Geometry/Constants.h"
#include <cmath>
#include <array>
#include <algorithm>
#include <vector>
#include <span>

namespace RVX::Geometry
{

// ============================================================================
// SAT Result Structure
// ============================================================================

/**
 * @brief Result of SAT collision test
 */
struct SATResult
{
    bool separated = true;          ///< True if shapes are separated (no collision)
    Vec3 separatingAxis{0, 1, 0};   ///< The axis of minimum penetration (collision normal)
    float penetration = 0.0f;       ///< Penetration depth (positive when overlapping)
    Vec3 pointOnA{0};               ///< Closest/contact point on shape A
    Vec3 pointOnB{0};               ///< Closest/contact point on shape B

    /// Returns true if collision occurred (shapes are NOT separated)
    bool Colliding() const { return !separated; }

    /// Get collision normal (from A to B)
    Vec3 GetNormal() const { return separatingAxis; }
};

// ============================================================================
// Convex Polyhedron for SAT
// ============================================================================

/**
 * @brief Convex polyhedron representation for SAT testing
 * 
 * Stores vertices and face normals for efficient SAT collision detection.
 */
struct ConvexPolyhedron
{
    std::vector<Vec3> vertices;      ///< Vertices in world space
    std::vector<Vec3> faceNormals;   ///< Face normals (outward facing)
    std::vector<std::vector<int>> faces; ///< Face indices into vertices array

    ConvexPolyhedron() = default;

    /**
     * @brief Construct from vertex array and optional faces
     */
    ConvexPolyhedron(std::span<const Vec3> verts)
        : vertices(verts.begin(), verts.end())
    {
        // If no faces provided, this is just a point cloud
        // SAT will use edges formed from vertices
    }

    /**
     * @brief Get the support point in a given direction
     */
    Vec3 Support(const Vec3& direction) const
    {
        if (vertices.empty()) return Vec3(0);

        float maxDot = glm::dot(vertices[0], direction);
        Vec3 support = vertices[0];

        for (size_t i = 1; i < vertices.size(); ++i)
        {
            float d = glm::dot(vertices[i], direction);
            if (d > maxDot)
            {
                maxDot = d;
                support = vertices[i];
            }
        }

        return support;
    }

    /**
     * @brief Get the center (centroid) of the polyhedron
     */
    Vec3 GetCenter() const
    {
        if (vertices.empty()) return Vec3(0);

        Vec3 sum(0);
        for (const auto& v : vertices)
        {
            sum += v;
        }
        return sum / static_cast<float>(vertices.size());
    }

    /**
     * @brief Project all vertices onto an axis and get min/max
     */
    void ProjectOntoAxis(const Vec3& axis, float& outMin, float& outMax) const
    {
        if (vertices.empty())
        {
            outMin = outMax = 0.0f;
            return;
        }

        outMin = outMax = glm::dot(vertices[0], axis);
        for (size_t i = 1; i < vertices.size(); ++i)
        {
            float proj = glm::dot(vertices[i], axis);
            outMin = std::min(outMin, proj);
            outMax = std::max(outMax, proj);
        }
    }

    /**
     * @brief Get unique edge directions
     */
    std::vector<Vec3> GetEdgeDirections() const
    {
        std::vector<Vec3> edges;
        
        // For each face, compute edge directions
        for (const auto& face : faces)
        {
            for (size_t i = 0; i < face.size(); ++i)
            {
                size_t j = (i + 1) % face.size();
                Vec3 edge = vertices[face[j]] - vertices[face[i]];
                float len = glm::length(edge);
                if (len > EPSILON)
                {
                    edge /= len;
                    // Check if this edge direction (or its negative) is already added
                    bool found = false;
                    for (const auto& e : edges)
                    {
                        if (std::abs(glm::dot(e, edge)) > 1.0f - EPSILON)
                        {
                            found = true;
                            break;
                        }
                    }
                    if (!found)
                    {
                        edges.push_back(edge);
                    }
                }
            }
        }

        return edges;
    }

    /**
     * @brief Create from OBB
     */
    static ConvexPolyhedron FromOBB(const OBB& obb)
    {
        ConvexPolyhedron poly;
        auto corners = obb.GetCorners();
        poly.vertices.assign(corners.begin(), corners.end());

        // Add face normals
        Mat3 rot = obb.GetRotationMatrix();
        poly.faceNormals = {
             rot[0],  // +X
            -rot[0],  // -X
             rot[1],  // +Y
            -rot[1],  // -Y
             rot[2],  // +Z
            -rot[2]   // -Z
        };

        // Add faces (vertex indices for each face)
        // Based on corner indexing: bit0=x, bit1=y, bit2=z
        poly.faces = {
            {1, 3, 7, 5}, // +X
            {0, 4, 6, 2}, // -X
            {2, 3, 7, 6}, // +Y
            {0, 1, 5, 4}, // -Y
            {4, 5, 7, 6}, // +Z
            {0, 2, 3, 1}  // -Z
        };

        return poly;
    }
};

// ============================================================================
// SAT Helper Functions
// ============================================================================

namespace detail
{

/**
 * @brief Test overlap on a single axis
 * @return False if separated, true if overlapping
 */
inline bool TestAxisOverlap(
    const ConvexPolyhedron& a,
    const ConvexPolyhedron& b,
    const Vec3& axis,
    float& outPenetration)
{
    float axisLenSq = glm::dot(axis, axis);
    if (axisLenSq < EPSILON * EPSILON)
    {
        // Degenerate axis
        outPenetration = std::numeric_limits<float>::max();
        return true;
    }

    Vec3 normAxis = axis / std::sqrt(axisLenSq);

    float minA, maxA, minB, maxB;
    a.ProjectOntoAxis(normAxis, minA, maxA);
    b.ProjectOntoAxis(normAxis, minB, maxB);

    // Check for separation
    if (maxA < minB || maxB < minA)
    {
        outPenetration = 0.0f;
        return false;
    }

    // Compute overlap
    float overlap1 = maxA - minB;
    float overlap2 = maxB - minA;
    outPenetration = std::min(overlap1, overlap2);

    return true;
}

} // namespace detail

// ============================================================================
// SAT for Convex Polyhedra
// ============================================================================

/**
 * @brief SAT collision test between two convex polyhedra
 * 
 * Tests all potential separating axes:
 * 1. Face normals of polyhedron A
 * 2. Face normals of polyhedron B
 * 3. Cross products of edges from A and B
 * 
 * @param a First polyhedron
 * @param b Second polyhedron
 * @return SATResult with collision information
 */
inline SATResult SATTestConvex(const ConvexPolyhedron& a, const ConvexPolyhedron& b)
{
    SATResult result;
    result.separated = false;
    float minPenetration = std::numeric_limits<float>::max();
    Vec3 minAxis(0, 1, 0);

    // Test face normals of A
    for (const auto& normal : a.faceNormals)
    {
        float penetration;
        if (!detail::TestAxisOverlap(a, b, normal, penetration))
        {
            result.separated = true;
            result.separatingAxis = glm::normalize(normal);
            result.penetration = 0.0f;
            return result;
        }
        if (penetration < minPenetration)
        {
            minPenetration = penetration;
            minAxis = normal;
        }
    }

    // Test face normals of B
    for (const auto& normal : b.faceNormals)
    {
        float penetration;
        if (!detail::TestAxisOverlap(a, b, normal, penetration))
        {
            result.separated = true;
            result.separatingAxis = glm::normalize(normal);
            result.penetration = 0.0f;
            return result;
        }
        if (penetration < minPenetration)
        {
            minPenetration = penetration;
            minAxis = normal;
        }
    }

    // Test cross products of edge directions
    auto edgesA = a.GetEdgeDirections();
    auto edgesB = b.GetEdgeDirections();

    for (const auto& edgeA : edgesA)
    {
        for (const auto& edgeB : edgesB)
        {
            Vec3 axis = glm::cross(edgeA, edgeB);
            float axisLenSq = glm::dot(axis, axis);
            if (axisLenSq < EPSILON * EPSILON)
                continue;  // Parallel edges

            float penetration;
            if (!detail::TestAxisOverlap(a, b, axis, penetration))
            {
                result.separated = true;
                result.separatingAxis = glm::normalize(axis);
                result.penetration = 0.0f;
                return result;
            }
            if (penetration < minPenetration)
            {
                minPenetration = penetration;
                minAxis = axis;
            }
        }
    }

    // No separating axis found - collision detected
    result.separated = false;
    result.separatingAxis = glm::normalize(minAxis);
    result.penetration = minPenetration;

    // Ensure normal points from A to B
    Vec3 centerDiff = b.GetCenter() - a.GetCenter();
    if (glm::dot(result.separatingAxis, centerDiff) < 0)
    {
        result.separatingAxis = -result.separatingAxis;
    }

    // Compute approximate contact points
    result.pointOnA = a.Support(result.separatingAxis);
    result.pointOnB = b.Support(-result.separatingAxis);

    return result;
}

/**
 * @brief SAT collision test between two OBBs with SATResult
 */
inline SATResult SAT_OBB_OBB(const OBB& a, const OBB& b)
{
    auto polyA = ConvexPolyhedron::FromOBB(a);
    auto polyB = ConvexPolyhedron::FromOBB(b);
    return SATTestConvex(polyA, polyB);
}

/**
 * @brief SAT collision test between two OBBs
 * 
 * Uses the Separating Axis Theorem to test collision between
 * two oriented bounding boxes. Can optionally generate contact manifold.
 * 
 * @param a First OBB
 * @param b Second OBB
 * @param outManifold Optional contact manifold output
 * @return true if the OBBs intersect
 */
inline bool SATTestOBB(
    const OBB& a,
    const OBB& b,
    ContactManifold* outManifold = nullptr)
{
    Mat3 rotA = a.GetRotationMatrix();
    Mat3 rotB = b.GetRotationMatrix();

    Vec3 axesA[3] = { rotA[0], rotA[1], rotA[2] };
    Vec3 axesB[3] = { rotB[0], rotB[1], rotB[2] };

    // Compute rotation matrix expressing B in A's coordinate frame
    Mat3 R, AbsR;
    for (int i = 0; i < 3; ++i)
    {
        for (int j = 0; j < 3; ++j)
        {
            R[i][j] = glm::dot(axesA[i], axesB[j]);
            AbsR[i][j] = std::abs(R[i][j]) + EPSILON;
        }
    }

    Vec3 t = b.center - a.center;
    t = Vec3(glm::dot(t, axesA[0]), glm::dot(t, axesA[1]), glm::dot(t, axesA[2]));

    float minPenetration = FLT_MAX;
    Vec3 minAxis;
    int minAxisType = -1;

    // Helper to test an axis
    auto TestAxis = [&](const Vec3& axis, float ra, float rb, int axisType) -> bool
    {
        float separation = std::abs(glm::dot(t, axis)) - (ra + rb);
        if (separation > 0) return false;  // Separating axis found

        float penetration = -(separation);
        if (penetration < minPenetration)
        {
            minPenetration = penetration;
            minAxis = axis;
            minAxisType = axisType;
        }
        return true;
    };

    // Test axes L = A0, A1, A2
    for (int i = 0; i < 3; ++i)
    {
        float ra = a.halfExtents[i];
        float rb = b.halfExtents[0] * AbsR[i][0] + 
                   b.halfExtents[1] * AbsR[i][1] + 
                   b.halfExtents[2] * AbsR[i][2];
        Vec3 localAxis = Vec3(0);
        localAxis[i] = 1.0f;
        if (!TestAxis(localAxis, ra, rb, i)) return false;
    }

    // Test axes L = B0, B1, B2
    for (int i = 0; i < 3; ++i)
    {
        float ra = a.halfExtents[0] * AbsR[0][i] + 
                   a.halfExtents[1] * AbsR[1][i] + 
                   a.halfExtents[2] * AbsR[2][i];
        float rb = b.halfExtents[i];
        Vec3 localAxis(R[0][i], R[1][i], R[2][i]);
        if (!TestAxis(localAxis, ra, rb, 3 + i)) return false;
    }

    // Test cross product axes (9 axes)
    // A0 x B0
    {
        float ra = a.halfExtents[1] * AbsR[2][0] + a.halfExtents[2] * AbsR[1][0];
        float rb = b.halfExtents[1] * AbsR[0][2] + b.halfExtents[2] * AbsR[0][1];
        Vec3 localAxis(0, -R[2][0], R[1][0]);
        if (glm::dot(localAxis, localAxis) > EPSILON)
        {
            localAxis = glm::normalize(localAxis);
            if (!TestAxis(localAxis, ra, rb, 6)) return false;
        }
    }

    // A0 x B1
    {
        float ra = a.halfExtents[1] * AbsR[2][1] + a.halfExtents[2] * AbsR[1][1];
        float rb = b.halfExtents[0] * AbsR[0][2] + b.halfExtents[2] * AbsR[0][0];
        Vec3 localAxis(0, -R[2][1], R[1][1]);
        if (glm::dot(localAxis, localAxis) > EPSILON)
        {
            localAxis = glm::normalize(localAxis);
            if (!TestAxis(localAxis, ra, rb, 7)) return false;
        }
    }

    // A0 x B2
    {
        float ra = a.halfExtents[1] * AbsR[2][2] + a.halfExtents[2] * AbsR[1][2];
        float rb = b.halfExtents[0] * AbsR[0][1] + b.halfExtents[1] * AbsR[0][0];
        Vec3 localAxis(0, -R[2][2], R[1][2]);
        if (glm::dot(localAxis, localAxis) > EPSILON)
        {
            localAxis = glm::normalize(localAxis);
            if (!TestAxis(localAxis, ra, rb, 8)) return false;
        }
    }

    // A1 x B0
    {
        float ra = a.halfExtents[0] * AbsR[2][0] + a.halfExtents[2] * AbsR[0][0];
        float rb = b.halfExtents[1] * AbsR[1][2] + b.halfExtents[2] * AbsR[1][1];
        Vec3 localAxis(R[2][0], 0, -R[0][0]);
        if (glm::dot(localAxis, localAxis) > EPSILON)
        {
            localAxis = glm::normalize(localAxis);
            if (!TestAxis(localAxis, ra, rb, 9)) return false;
        }
    }

    // A1 x B1
    {
        float ra = a.halfExtents[0] * AbsR[2][1] + a.halfExtents[2] * AbsR[0][1];
        float rb = b.halfExtents[0] * AbsR[1][2] + b.halfExtents[2] * AbsR[1][0];
        Vec3 localAxis(R[2][1], 0, -R[0][1]);
        if (glm::dot(localAxis, localAxis) > EPSILON)
        {
            localAxis = glm::normalize(localAxis);
            if (!TestAxis(localAxis, ra, rb, 10)) return false;
        }
    }

    // A1 x B2
    {
        float ra = a.halfExtents[0] * AbsR[2][2] + a.halfExtents[2] * AbsR[0][2];
        float rb = b.halfExtents[0] * AbsR[1][1] + b.halfExtents[1] * AbsR[1][0];
        Vec3 localAxis(R[2][2], 0, -R[0][2]);
        if (glm::dot(localAxis, localAxis) > EPSILON)
        {
            localAxis = glm::normalize(localAxis);
            if (!TestAxis(localAxis, ra, rb, 11)) return false;
        }
    }

    // A2 x B0
    {
        float ra = a.halfExtents[0] * AbsR[1][0] + a.halfExtents[1] * AbsR[0][0];
        float rb = b.halfExtents[1] * AbsR[2][2] + b.halfExtents[2] * AbsR[2][1];
        Vec3 localAxis(-R[1][0], R[0][0], 0);
        if (glm::dot(localAxis, localAxis) > EPSILON)
        {
            localAxis = glm::normalize(localAxis);
            if (!TestAxis(localAxis, ra, rb, 12)) return false;
        }
    }

    // A2 x B1
    {
        float ra = a.halfExtents[0] * AbsR[1][1] + a.halfExtents[1] * AbsR[0][1];
        float rb = b.halfExtents[0] * AbsR[2][2] + b.halfExtents[2] * AbsR[2][0];
        Vec3 localAxis(-R[1][1], R[0][1], 0);
        if (glm::dot(localAxis, localAxis) > EPSILON)
        {
            localAxis = glm::normalize(localAxis);
            if (!TestAxis(localAxis, ra, rb, 13)) return false;
        }
    }

    // A2 x B2
    {
        float ra = a.halfExtents[0] * AbsR[1][2] + a.halfExtents[1] * AbsR[0][2];
        float rb = b.halfExtents[0] * AbsR[2][1] + b.halfExtents[1] * AbsR[2][0];
        Vec3 localAxis(-R[1][2], R[0][2], 0);
        if (glm::dot(localAxis, localAxis) > EPSILON)
        {
            localAxis = glm::normalize(localAxis);
            if (!TestAxis(localAxis, ra, rb, 14)) return false;
        }
    }

    // No separating axis found - boxes intersect
    if (outManifold)
    {
        outManifold->Clear();

        // Convert minAxis to world space
        Vec3 worldAxis;
        if (minAxisType < 3)
        {
            worldAxis = axesA[minAxisType];
        }
        else if (minAxisType < 6)
        {
            worldAxis = axesB[minAxisType - 3];
        }
        else
        {
            // Cross product axis - already normalized
            worldAxis = axesA[0] * minAxis.x + axesA[1] * minAxis.y + axesA[2] * minAxis.z;
        }

        // Ensure normal points from A to B
        Vec3 centerDiff = b.center - a.center;
        if (glm::dot(worldAxis, centerDiff) < 0)
        {
            worldAxis = -worldAxis;
        }

        outManifold->normal = worldAxis;

        // Generate contact points (simplified - just use closest points)
        Vec3 pointOnA = a.ClosestPoint(b.center);
        Vec3 pointOnB = b.ClosestPoint(a.center);

        outManifold->Add(pointOnA, pointOnB, worldAxis, minPenetration);
    }

    return true;
}

/**
 * @brief SAT test for OBB vs Sphere (simplified)
 */
inline bool SATTestOBBSphere(
    const OBB& obb,
    const Vec3& sphereCenter,
    float sphereRadius,
    ContactManifold* outManifold = nullptr)
{
    Vec3 closest = obb.ClosestPoint(sphereCenter);
    Vec3 diff = sphereCenter - closest;
    float distSq = glm::dot(diff, diff);
    float radiusSq = sphereRadius * sphereRadius;

    if (distSq > radiusSq)
        return false;

    if (outManifold)
    {
        outManifold->Clear();

        float dist = std::sqrt(distSq);
        Vec3 normal = dist > EPSILON ? diff / dist : Vec3(0, 1, 0);

        Vec3 pointOnOBB = closest;
        Vec3 pointOnSphere = sphereCenter - normal * sphereRadius;
        float penetration = sphereRadius - dist;

        outManifold->normal = normal;
        outManifold->Add(pointOnOBB, pointOnSphere, normal, penetration);
    }

    return true;
}

/**
 * @brief SAT test for Capsule vs Capsule
 */
inline bool SATTestCapsule(
    const Capsule& a,
    const Capsule& b,
    ContactManifold* outManifold = nullptr)
{
    float s, t;
    float distSq = SegmentSegmentDistanceSquared(a.GetSegment(), b.GetSegment(), s, t);
    float radiusSum = a.radius + b.radius;

    if (distSq > radiusSum * radiusSum)
        return false;

    if (outManifold)
    {
        outManifold->Clear();

        Vec3 closestA = a.GetSegment().PointAt(s);
        Vec3 closestB = b.GetSegment().PointAt(t);

        Vec3 diff = closestB - closestA;
        float dist = std::sqrt(distSq);
        Vec3 normal = dist > EPSILON ? diff / dist : Vec3(0, 1, 0);

        Vec3 pointOnA = closestA + normal * a.radius;
        Vec3 pointOnB = closestB - normal * b.radius;
        float penetration = radiusSum - dist;

        outManifold->normal = normal;
        outManifold->Add(pointOnA, pointOnB, normal, penetration);
    }

    return true;
}

} // namespace RVX::Geometry
