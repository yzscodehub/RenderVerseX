/**
 * @file BatchIntersect.h
 * @brief SIMD batch intersection tests
 * 
 * Provides high-performance intersection tests that process
 * multiple primitives in parallel using SIMD instructions.
 */

#pragma once

#include "Geometry/Batch/SIMDTypes.h"
#include "Core/Math/Ray.h"
#include "Core/Math/AABB.h"
#include "Geometry/Primitives/Triangle.h"

namespace RVX::Geometry::SIMD
{

// ============================================================================
// Batch AABB for Ray Testing
// ============================================================================

/**
 * @brief 4 AABBs stored in SOA layout for batch testing
 */
struct BatchAABB4
{
    Float4 minX, minY, minZ;
    Float4 maxX, maxY, maxZ;

    BatchAABB4() = default;

    /// Load from 4 separate AABBs
    void Load(const AABB& b0, const AABB& b1, const AABB& b2, const AABB& b3)
    {
        minX = Float4(b0.GetMin().x, b1.GetMin().x, b2.GetMin().x, b3.GetMin().x);
        minY = Float4(b0.GetMin().y, b1.GetMin().y, b2.GetMin().y, b3.GetMin().y);
        minZ = Float4(b0.GetMin().z, b1.GetMin().z, b2.GetMin().z, b3.GetMin().z);
        maxX = Float4(b0.GetMax().x, b1.GetMax().x, b2.GetMax().x, b3.GetMax().x);
        maxY = Float4(b0.GetMax().y, b1.GetMax().y, b2.GetMax().y, b3.GetMax().y);
        maxZ = Float4(b0.GetMax().z, b1.GetMax().z, b2.GetMax().z, b3.GetMax().z);
    }

    /// Load from 2 AABBs (fill remaining slots with invalid boxes)
    void Load(const AABB& b0, const AABB& b1)
    {
        // Create invalid boxes for slots 2 and 3
        constexpr float INF = 1e30f;
        minX = Float4(b0.GetMin().x, b1.GetMin().x, INF, INF);
        minY = Float4(b0.GetMin().y, b1.GetMin().y, INF, INF);
        minZ = Float4(b0.GetMin().z, b1.GetMin().z, INF, INF);
        maxX = Float4(b0.GetMax().x, b1.GetMax().x, -INF, -INF);
        maxY = Float4(b0.GetMax().y, b1.GetMax().y, -INF, -INF);
        maxZ = Float4(b0.GetMax().z, b1.GetMax().z, -INF, -INF);
    }

    /// Load from array of AABBs
    void Load(const AABB* boxes, int count)
    {
        constexpr float INF = 1e30f;
        float mins[4][3] = {{INF, INF, INF}, {INF, INF, INF}, {INF, INF, INF}, {INF, INF, INF}};
        float maxs[4][3] = {{-INF, -INF, -INF}, {-INF, -INF, -INF}, {-INF, -INF, -INF}, {-INF, -INF, -INF}};

        for (int i = 0; i < count && i < 4; ++i)
        {
            mins[i][0] = boxes[i].GetMin().x;
            mins[i][1] = boxes[i].GetMin().y;
            mins[i][2] = boxes[i].GetMin().z;
            maxs[i][0] = boxes[i].GetMax().x;
            maxs[i][1] = boxes[i].GetMax().y;
            maxs[i][2] = boxes[i].GetMax().z;
        }

        minX = Float4(mins[0][0], mins[1][0], mins[2][0], mins[3][0]);
        minY = Float4(mins[0][1], mins[1][1], mins[2][1], mins[3][1]);
        minZ = Float4(mins[0][2], mins[1][2], mins[2][2], mins[3][2]);
        maxX = Float4(maxs[0][0], maxs[1][0], maxs[2][0], maxs[3][0]);
        maxY = Float4(maxs[0][1], maxs[1][1], maxs[2][1], maxs[3][1]);
        maxZ = Float4(maxs[0][2], maxs[1][2], maxs[2][2], maxs[3][2]);
    }
};

// ============================================================================
// Ray-AABB Batch Intersection
// ============================================================================

/**
 * @brief Test 1 ray against 4 AABBs
 * 
 * Uses the slab method with precomputed inverse direction.
 * 
 * @param ray The ray to test
 * @param boxes 4 AABBs in SOA layout
 * @param outTMin Output: entry distances for each box
 * @param outTMax Output: exit distances for each box
 * @return Bitmask of hits (bit i set if box i was hit)
 */
inline uint32_t RayBatchAABBIntersect(
    const Ray& ray,
    const BatchAABB4& boxes,
    Float4& outTMin,
    Float4& outTMax)
{
    // Precompute inverse direction
    Vec3 invDir = ray.GetInverseDirection();

    // Splat ray origin and inverse direction
    Float4 origX = Float4::Splat(ray.origin.x);
    Float4 origY = Float4::Splat(ray.origin.y);
    Float4 origZ = Float4::Splat(ray.origin.z);

    Float4 invDirX = Float4::Splat(invDir.x);
    Float4 invDirY = Float4::Splat(invDir.y);
    Float4 invDirZ = Float4::Splat(invDir.z);

    // Compute t values for each slab
    Float4 t1x = (boxes.minX - origX) * invDirX;
    Float4 t2x = (boxes.maxX - origX) * invDirX;
    Float4 t1y = (boxes.minY - origY) * invDirY;
    Float4 t2y = (boxes.maxY - origY) * invDirY;
    Float4 t1z = (boxes.minZ - origZ) * invDirZ;
    Float4 t2z = (boxes.maxZ - origZ) * invDirZ;

    // Ensure t1 <= t2 for each axis
    Float4 tNearX = t1x.Min(t2x);
    Float4 tFarX = t1x.Max(t2x);
    Float4 tNearY = t1y.Min(t2y);
    Float4 tFarY = t1y.Max(t2y);
    Float4 tNearZ = t1z.Min(t2z);
    Float4 tFarZ = t1z.Max(t2z);

    // Find largest tNear and smallest tFar
    Float4 tNear = tNearX.Max(tNearY).Max(tNearZ);
    Float4 tFar = tFarX.Min(tFarY).Min(tFarZ);

    // Apply ray bounds
    Float4 rayTMin = Float4::Splat(ray.tMin);
    Float4 rayTMax = Float4::Splat(ray.tMax);
    tNear = tNear.Max(rayTMin);
    tFar = tFar.Min(rayTMax);

    outTMin = tNear;
    outTMax = tFar;

    // Hit if tMin <= tMax
    Float4 hitMask = tNear <= tFar;
    return static_cast<uint32_t>(hitMask.MoveMask()) & 0xF;
}

/**
 * @brief Test 1 ray against 4 AABBs (simplified version)
 * @return Bitmask of hits
 */
inline uint32_t RayBatchAABBTest(const Ray& ray, const BatchAABB4& boxes)
{
    Float4 tMin, tMax;
    return RayBatchAABBIntersect(ray, boxes, tMin, tMax);
}

// ============================================================================
// Batch Rays
// ============================================================================

/**
 * @brief 4 rays in SOA layout
 */
struct BatchRay4
{
    Vec3x4 origins;
    Vec3x4 directions;
    Vec3x4 invDirections;  // Precomputed 1/direction
    Float4 tMin;
    Float4 tMax;

    BatchRay4() = default;

    /// Load from 4 separate rays
    void Load(const Ray& r0, const Ray& r1, const Ray& r2, const Ray& r3)
    {
        origins = Vec3x4::Load(r0.origin, r1.origin, r2.origin, r3.origin);
        directions = Vec3x4::Load(r0.direction, r1.direction, r2.direction, r3.direction);
        invDirections = Vec3x4::Load(
            r0.GetInverseDirection(),
            r1.GetInverseDirection(),
            r2.GetInverseDirection(),
            r3.GetInverseDirection());
        tMin = Float4(r0.tMin, r1.tMin, r2.tMin, r3.tMin);
        tMax = Float4(r0.tMax, r1.tMax, r2.tMax, r3.tMax);
    }

    /// Splat a single ray to all 4 slots
    void Splat(const Ray& r)
    {
        origins = Vec3x4::Splat(r.origin);
        directions = Vec3x4::Splat(r.direction);
        invDirections = Vec3x4::Splat(r.GetInverseDirection());
        tMin = Float4::Splat(r.tMin);
        tMax = Float4::Splat(r.tMax);
    }
};

/**
 * @brief Test 4 rays against 1 AABB
 * 
 * @param rays 4 rays in SOA layout
 * @param box Single AABB to test
 * @param outTMin Output: entry distances for each ray
 * @param outTMax Output: exit distances for each ray
 * @return Bitmask of hits (bit i set if ray i hit the box)
 */
inline uint32_t BatchRayAABBIntersect(
    const BatchRay4& rays,
    const AABB& box,
    Float4& outTMin,
    Float4& outTMax)
{
    // Splat box bounds
    Float4 boxMinX = Float4::Splat(box.GetMin().x);
    Float4 boxMinY = Float4::Splat(box.GetMin().y);
    Float4 boxMinZ = Float4::Splat(box.GetMin().z);
    Float4 boxMaxX = Float4::Splat(box.GetMax().x);
    Float4 boxMaxY = Float4::Splat(box.GetMax().y);
    Float4 boxMaxZ = Float4::Splat(box.GetMax().z);

    // Compute t values
    Float4 t1x = (boxMinX - rays.origins.x) * rays.invDirections.x;
    Float4 t2x = (boxMaxX - rays.origins.x) * rays.invDirections.x;
    Float4 t1y = (boxMinY - rays.origins.y) * rays.invDirections.y;
    Float4 t2y = (boxMaxY - rays.origins.y) * rays.invDirections.y;
    Float4 t1z = (boxMinZ - rays.origins.z) * rays.invDirections.z;
    Float4 t2z = (boxMaxZ - rays.origins.z) * rays.invDirections.z;

    Float4 tNearX = t1x.Min(t2x);
    Float4 tFarX = t1x.Max(t2x);
    Float4 tNearY = t1y.Min(t2y);
    Float4 tFarY = t1y.Max(t2y);
    Float4 tNearZ = t1z.Min(t2z);
    Float4 tFarZ = t1z.Max(t2z);

    Float4 tNear = tNearX.Max(tNearY).Max(tNearZ).Max(rays.tMin);
    Float4 tFar = tFarX.Min(tFarY).Min(tFarZ).Min(rays.tMax);

    outTMin = tNear;
    outTMax = tFar;

    Float4 hitMask = tNear <= tFar;
    return static_cast<uint32_t>(hitMask.MoveMask()) & 0xF;
}

// ============================================================================
// Batch Triangle Intersection (Moller-Trumbore x4)
// ============================================================================

/**
 * @brief 4 triangles in SOA layout
 */
struct BatchTriangle4
{
    Vec3x4 v0, v1, v2;

    BatchTriangle4() = default;

    void Load(const Triangle& t0, const Triangle& t1, const Triangle& t2, const Triangle& t3)
    {
        v0 = Vec3x4::Load(t0.v0, t1.v0, t2.v0, t3.v0);
        v1 = Vec3x4::Load(t0.v1, t1.v1, t2.v1, t3.v1);
        v2 = Vec3x4::Load(t0.v2, t1.v2, t2.v2, t3.v2);
    }

    void Load(const Vec3* vertices, const uint32_t* indices, int triCount)
    {
        Vec3 verts[4][3];
        for (int i = 0; i < 4; ++i)
        {
            if (i < triCount)
            {
                verts[i][0] = vertices[indices[i * 3 + 0]];
                verts[i][1] = vertices[indices[i * 3 + 1]];
                verts[i][2] = vertices[indices[i * 3 + 2]];
            }
            else
            {
                // Degenerate triangle that won't be hit
                verts[i][0] = verts[i][1] = verts[i][2] = Vec3(0);
            }
        }

        v0 = Vec3x4::Load(verts[0][0], verts[1][0], verts[2][0], verts[3][0]);
        v1 = Vec3x4::Load(verts[0][1], verts[1][1], verts[2][1], verts[3][1]);
        v2 = Vec3x4::Load(verts[0][2], verts[1][2], verts[2][2], verts[3][2]);
    }
};

/**
 * @brief Test 1 ray against 4 triangles using Moller-Trumbore
 * 
 * @param ray The ray to test
 * @param tris 4 triangles in SOA layout
 * @param outT Output: hit distances
 * @param outU Output: barycentric U coordinates
 * @param outV Output: barycentric V coordinates
 * @param cullBackface If true, skip back-facing triangles
 * @return Bitmask of hits
 */
inline uint32_t RayBatchTriangleIntersect(
    const Ray& ray,
    const BatchTriangle4& tris,
    Float4& outT,
    Float4& outU,
    Float4& outV,
    bool cullBackface = false)
{
    constexpr float TRI_EPSILON = 1e-8f;

    Vec3x4 origin = Vec3x4::Splat(ray.origin);
    Vec3x4 dir = Vec3x4::Splat(ray.direction);

    Vec3x4 edge1 = tris.v1 - tris.v0;
    Vec3x4 edge2 = tris.v2 - tris.v0;

    Vec3x4 h = dir.Cross(edge2);
    Float4 a = edge1.Dot(h);

    Float4 eps = Float4::Splat(TRI_EPSILON);
    Float4 absA = a.Abs();

    // Check for parallel rays
    Float4 validMask = absA > eps;

    if (cullBackface)
    {
        validMask = validMask.And(a > Float4::Zero());
    }

    Float4 f = Float4::Splat(1.0f) / a;
    Vec3x4 s = origin - tris.v0;
    Float4 u = f * s.Dot(h);

    // Check u bounds [0, 1]
    validMask = validMask.And(u >= Float4::Zero()).And(u <= Float4::Splat(1.0f));

    Vec3x4 q = s.Cross(edge1);
    Float4 v = f * dir.Dot(q);

    // Check v bounds and u+v <= 1
    validMask = validMask.And(v >= Float4::Zero()).And((u + v) <= Float4::Splat(1.0f));

    Float4 t = f * edge2.Dot(q);

    // Check t bounds
    Float4 tMin = Float4::Splat(ray.tMin);
    Float4 tMax = Float4::Splat(ray.tMax);
    validMask = validMask.And(t >= tMin).And(t <= tMax);

    outT = t;
    outU = u;
    outV = v;

    return static_cast<uint32_t>(validMask.MoveMask()) & 0xF;
}

} // namespace RVX::Geometry::SIMD
