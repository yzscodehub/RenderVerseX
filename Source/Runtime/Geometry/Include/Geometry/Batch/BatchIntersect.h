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
#include "Core/Math/Sphere.h"
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

// ============================================================================
// Batch Sphere Structures
// ============================================================================

/**
 * @brief 4 spheres in SOA layout for batch testing
 */
struct BatchSphere4
{
    Float4 centerX, centerY, centerZ;
    Float4 radius;

    BatchSphere4() = default;

    /// Load from 4 separate spheres
    void Load(const Sphere& s0, const Sphere& s1, const Sphere& s2, const Sphere& s3)
    {
        centerX = Float4(s0.GetCenter().x, s1.GetCenter().x, s2.GetCenter().x, s3.GetCenter().x);
        centerY = Float4(s0.GetCenter().y, s1.GetCenter().y, s2.GetCenter().y, s3.GetCenter().y);
        centerZ = Float4(s0.GetCenter().z, s1.GetCenter().z, s2.GetCenter().z, s3.GetCenter().z);
        radius = Float4(s0.GetRadius(), s1.GetRadius(), s2.GetRadius(), s3.GetRadius());
    }

    /// Load from array of spheres
    void Load(const Sphere* spheres, int count)
    {
        float cx[4] = {0, 0, 0, 0};
        float cy[4] = {0, 0, 0, 0};
        float cz[4] = {0, 0, 0, 0};
        float r[4] = {0, 0, 0, 0};

        for (int i = 0; i < count && i < 4; ++i)
        {
            cx[i] = spheres[i].GetCenter().x;
            cy[i] = spheres[i].GetCenter().y;
            cz[i] = spheres[i].GetCenter().z;
            r[i] = spheres[i].GetRadius();
        }

        centerX = Float4(cx[0], cx[1], cx[2], cx[3]);
        centerY = Float4(cy[0], cy[1], cy[2], cy[3]);
        centerZ = Float4(cz[0], cz[1], cz[2], cz[3]);
        radius = Float4(r[0], r[1], r[2], r[3]);
    }

    /// Splat a single sphere to all 4 slots
    void Splat(const Sphere& s)
    {
        centerX = Float4::Splat(s.GetCenter().x);
        centerY = Float4::Splat(s.GetCenter().y);
        centerZ = Float4::Splat(s.GetCenter().z);
        radius = Float4::Splat(s.GetRadius());
    }
};

// ============================================================================
// Sphere-Sphere Batch Intersection
// ============================================================================

/**
 * @brief Test 1 sphere against 4 spheres
 * 
 * @param sphere The single sphere to test
 * @param batch 4 spheres in SOA layout
 * @param outDistSq Output: squared distances between sphere centers
 * @return Bitmask of hits (bit i set if sphere i intersects)
 */
inline uint32_t SphereBatchSphereIntersect(
    const Sphere& sphere,
    const BatchSphere4& batch,
    Float4& outDistSq)
{
    Vec3 center = sphere.GetCenter();
    float rad = sphere.GetRadius();

    // Compute delta vectors
    Float4 dx = batch.centerX - Float4::Splat(center.x);
    Float4 dy = batch.centerY - Float4::Splat(center.y);
    Float4 dz = batch.centerZ - Float4::Splat(center.z);

    // Squared distance between centers
    Float4 distSq = dx * dx + dy * dy + dz * dz;
    outDistSq = distSq;

    // Sum of radii
    Float4 radiusSum = batch.radius + Float4::Splat(rad);
    Float4 radiusSumSq = radiusSum * radiusSum;

    // Intersection if distSq <= radiusSumSq
    Float4 hitMask = distSq <= radiusSumSq;
    return static_cast<uint32_t>(hitMask.MoveMask()) & 0xF;
}

/**
 * @brief Test 1 sphere against 4 spheres (simplified version)
 * @return Bitmask of hits
 */
inline uint32_t SphereBatchSphereTest(const Sphere& sphere, const BatchSphere4& batch)
{
    Float4 distSq;
    return SphereBatchSphereIntersect(sphere, batch, distSq);
}

/**
 * @brief Test 4 spheres against 4 spheres (pairwise)
 * 
 * Tests spheres[i] from A against spheres[i] from B.
 * This is NOT a full N*M test, just 4 pairwise tests.
 * 
 * @param spheresA First batch of 4 spheres
 * @param spheresB Second batch of 4 spheres
 * @param outDistSq Output: squared distances
 * @return Bitmask of hits (bit i set if pair i intersects)
 */
inline uint32_t BatchSpherePairwiseIntersect(
    const BatchSphere4& spheresA,
    const BatchSphere4& spheresB,
    Float4& outDistSq)
{
    Float4 dx = spheresB.centerX - spheresA.centerX;
    Float4 dy = spheresB.centerY - spheresA.centerY;
    Float4 dz = spheresB.centerZ - spheresA.centerZ;

    Float4 distSq = dx * dx + dy * dy + dz * dz;
    outDistSq = distSq;

    Float4 radiusSum = spheresA.radius + spheresB.radius;
    Float4 radiusSumSq = radiusSum * radiusSum;

    Float4 hitMask = distSq <= radiusSumSq;
    return static_cast<uint32_t>(hitMask.MoveMask()) & 0xF;
}

/**
 * @brief Test 4 spheres against 4 spheres (all-pairs: 16 tests)
 * 
 * Returns a 16-bit mask where bit (i*4 + j) indicates if spheres_a[i]
 * intersects with spheres_b[j].
 * 
 * @param spheresA First batch of 4 spheres
 * @param spheresB Second batch of 4 spheres
 * @return 16-bit mask of intersections
 */
inline uint32_t BatchSphereAllPairsTest(
    const BatchSphere4& spheresA,
    const BatchSphere4& spheresB)
{
    uint32_t result = 0;

    // For each sphere in A, test against all 4 in B
    for (int i = 0; i < 4; ++i)
    {
        // Extract sphere i from A
        Sphere sA(
            Vec3(spheresA.centerX[i], spheresA.centerY[i], spheresA.centerZ[i]),
            spheresA.radius[i]
        );

        // Test against all of B
        uint32_t mask = SphereBatchSphereTest(sA, spheresB);
        result |= (mask << (i * 4));
    }

    return result;
}

// ============================================================================
// Ray-Sphere Batch Intersection
// ============================================================================

/**
 * @brief Test 1 ray against 4 spheres
 * 
 * @param ray The ray to test
 * @param spheres 4 spheres in SOA layout
 * @param outT Output: hit distances (for closest intersection point)
 * @return Bitmask of hits
 */
inline uint32_t RayBatchSphereIntersect(
    const Ray& ray,
    const BatchSphere4& spheres,
    Float4& outT)
{
    // Vector from ray origin to sphere centers
    Float4 ocX = Float4::Splat(ray.origin.x) - spheres.centerX;
    Float4 ocY = Float4::Splat(ray.origin.y) - spheres.centerY;
    Float4 ocZ = Float4::Splat(ray.origin.z) - spheres.centerZ;

    // Ray direction (already normalized)
    Float4 dirX = Float4::Splat(ray.direction.x);
    Float4 dirY = Float4::Splat(ray.direction.y);
    Float4 dirZ = Float4::Splat(ray.direction.z);

    // Quadratic coefficients: at^2 + bt + c = 0
    // a = dot(d, d) = 1 (assuming normalized direction)
    Float4 b = (ocX * dirX + ocY * dirY + ocZ * dirZ) * Float4::Splat(2.0f);
    Float4 c = ocX * ocX + ocY * ocY + ocZ * ocZ - spheres.radius * spheres.radius;

    // Discriminant
    Float4 disc = b * b - Float4::Splat(4.0f) * c;

    // Check for real solutions
    Float4 validMask = disc >= Float4::Zero();

    // Compute t = (-b - sqrt(disc)) / 2
    Float4 sqrtDisc = disc.Max(Float4::Zero()).Sqrt();
    Float4 t = ((-b) - sqrtDisc) * Float4::Splat(0.5f);

    // If t < tMin, try the far intersection
    Float4 tMin = Float4::Splat(ray.tMin);
    Float4 tMax = Float4::Splat(ray.tMax);

    Float4 tFar = ((-b) + sqrtDisc) * Float4::Splat(0.5f);
    Float4 useNear = t >= tMin;
    t = useNear.Select(t, tFar);

    // Check bounds
    validMask = validMask.And(t >= tMin).And(t <= tMax);

    outT = t;
    return static_cast<uint32_t>(validMask.MoveMask()) & 0xF;
}

// ============================================================================
// Sphere-AABB Batch Intersection
// ============================================================================

/**
 * @brief Test 1 sphere against 4 AABBs
 * 
 * @param sphere The sphere to test
 * @param boxes 4 AABBs in SOA layout
 * @return Bitmask of hits
 */
inline uint32_t SphereBatchAABBTest(const Sphere& sphere, const BatchAABB4& boxes)
{
    Vec3 center = sphere.GetCenter();
    float rad = sphere.GetRadius();

    // Find closest point on each AABB to sphere center
    Float4 closestX = Float4::Splat(center.x).Max(boxes.minX).Min(boxes.maxX);
    Float4 closestY = Float4::Splat(center.y).Max(boxes.minY).Min(boxes.maxY);
    Float4 closestZ = Float4::Splat(center.z).Max(boxes.minZ).Min(boxes.maxZ);

    // Distance from closest point to sphere center
    Float4 dx = closestX - Float4::Splat(center.x);
    Float4 dy = closestY - Float4::Splat(center.y);
    Float4 dz = closestZ - Float4::Splat(center.z);

    Float4 distSq = dx * dx + dy * dy + dz * dz;
    Float4 radiusSq = Float4::Splat(rad * rad);

    Float4 hitMask = distSq <= radiusSq;
    return static_cast<uint32_t>(hitMask.MoveMask()) & 0xF;
}

} // namespace RVX::Geometry::SIMD
