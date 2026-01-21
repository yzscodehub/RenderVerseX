/**
 * @file Intersection.h
 * @brief Geometric intersection and distance tests
 * 
 * Implements:
 * - Ray-AABB intersection (slab method)
 * - Ray-Triangle intersection (Moller-Trumbore)
 * - Ray-Sphere intersection
 * - Ray-Plane intersection
 * - AABB-AABB, Sphere-Sphere, AABB-Sphere tests
 * - Frustum culling tests
 * - Distance functions
 */

#pragma once

#include "Core/MathTypes.h"
#include "Core/Math/Ray.h"
#include "Core/Math/AABB.h"
#include "Core/Math/Sphere.h"
#include "Core/Math/Plane.h"
#include "Core/Math/Frustum.h"
#include <cmath>
#include <algorithm>
#include <limits>

namespace RVX
{

// =============================================================================
// Ray Intersection Tests
// =============================================================================

/**
 * @brief Ray-AABB intersection using slab method
 * 
 * Fast intersection test for bounding box culling in BVH traversal.
 * 
 * @param ray The ray to test
 * @param box The axis-aligned bounding box
 * @param tMin Output: entry distance (if hit)
 * @param tMax Output: exit distance (if hit)
 * @return true if ray intersects box
 */
inline bool RayAABBIntersect(
    const Ray& ray,
    const AABB& box,
    float& tMin,
    float& tMax)
{
    if (!box.IsValid()) return false;

    Vec3 invDir = ray.GetInverseDirection();
    Vec3 boxMin = box.GetMin();
    Vec3 boxMax = box.GetMax();

    // Calculate t values for each slab
    Vec3 t1 = (boxMin - ray.origin) * invDir;
    Vec3 t2 = (boxMax - ray.origin) * invDir;

    // Ensure t1 <= t2 for each axis
    Vec3 tNear = min(t1, t2);
    Vec3 tFar = max(t1, t2);

    // Find largest tNear and smallest tFar
    tMin = std::max({tNear.x, tNear.y, tNear.z, ray.tMin});
    tMax = std::min({tFar.x, tFar.y, tFar.z, ray.tMax});

    return tMin <= tMax;
}

/**
 * @brief Simplified Ray-AABB test (just returns hit/miss)
 */
inline bool RayAABBTest(const Ray& ray, const AABB& box)
{
    float tMin, tMax;
    return RayAABBIntersect(ray, box, tMin, tMax);
}

/**
 * @brief Ray-Triangle intersection using Moller-Trumbore algorithm
 * 
 * @param ray The ray to test
 * @param v0, v1, v2 Triangle vertices (counter-clockwise winding)
 * @param t Output: intersection distance
 * @param u, v Output: barycentric coordinates
 * @param cullBackface If true, skip back-facing triangles
 * @return true if ray intersects triangle
 */
inline bool RayTriangleIntersect(
    const Ray& ray,
    const Vec3& v0,
    const Vec3& v1,
    const Vec3& v2,
    float& t,
    float& u,
    float& v,
    bool cullBackface = false)
{
    constexpr float kEpsilon = 1e-8f;

    Vec3 edge1 = v1 - v0;
    Vec3 edge2 = v2 - v0;

    Vec3 h = cross(ray.direction, edge2);
    float a = dot(edge1, h);

    // Check if ray is parallel to triangle
    if (std::abs(a) < kEpsilon)
        return false;

    // Backface culling
    if (cullBackface && a < 0.0f)
        return false;

    float f = 1.0f / a;
    Vec3 s = ray.origin - v0;
    u = f * dot(s, h);

    // Check if intersection is outside triangle (u)
    if (u < 0.0f || u > 1.0f)
        return false;

    Vec3 q = cross(s, edge1);
    v = f * dot(ray.direction, q);

    // Check if intersection is outside triangle (v, u+v)
    if (v < 0.0f || u + v > 1.0f)
        return false;

    // Calculate t
    t = f * dot(edge2, q);

    // Check if intersection is within ray bounds
    if (t < ray.tMin || t > ray.tMax)
        return false;

    return true;
}

/**
 * @brief Ray-Triangle intersection with hit info output
 */
inline bool RayTriangleIntersect(
    const Ray& ray,
    const Vec3& v0,
    const Vec3& v1,
    const Vec3& v2,
    RayHit& hit,
    bool cullBackface = false)
{
    float t, u, v;
    if (!RayTriangleIntersect(ray, v0, v1, v2, t, u, v, cullBackface))
        return false;

    if (t >= hit.t)
        return false;  // Not closer than existing hit

    hit.t = t;
    hit.position = ray.At(t);
    hit.uv = Vec2(u, v);

    // Compute geometric normal
    Vec3 edge1 = v1 - v0;
    Vec3 edge2 = v2 - v0;
    hit.normal = normalize(cross(edge1, edge2));

    return true;
}

/**
 * @brief Ray-Sphere intersection
 * 
 * @param ray The ray to test
 * @param center Sphere center
 * @param radius Sphere radius
 * @param t Output: intersection distance (nearest)
 * @return true if ray intersects sphere
 */
inline bool RaySphereIntersect(
    const Ray& ray,
    const Vec3& center,
    float radius,
    float& t)
{
    Vec3 oc = ray.origin - center;

    float a = dot(ray.direction, ray.direction);
    float b = 2.0f * dot(oc, ray.direction);
    float c = dot(oc, oc) - radius * radius;

    float discriminant = b * b - 4.0f * a * c;

    if (discriminant < 0.0f)
        return false;

    float sqrtD = std::sqrt(discriminant);
    float t1 = (-b - sqrtD) / (2.0f * a);
    float t2 = (-b + sqrtD) / (2.0f * a);

    // Return nearest valid intersection
    if (t1 >= ray.tMin && t1 <= ray.tMax)
    {
        t = t1;
        return true;
    }

    if (t2 >= ray.tMin && t2 <= ray.tMax)
    {
        t = t2;
        return true;
    }

    return false;
}

/**
 * @brief Ray-Sphere intersection (Sphere object overload)
 */
inline bool RaySphereIntersect(const Ray& ray, const Sphere& sphere, float& t)
{
    if (!sphere.IsValid()) return false;
    return RaySphereIntersect(ray, sphere.GetCenter(), sphere.GetRadius(), t);
}

/**
 * @brief Ray-Plane intersection
 * 
 * @param ray The ray to test
 * @param planeNormal Plane normal (normalized)
 * @param planePoint A point on the plane
 * @param t Output: intersection distance
 * @return true if ray intersects plane
 */
inline bool RayPlaneIntersect(
    const Ray& ray,
    const Vec3& planeNormal,
    const Vec3& planePoint,
    float& t)
{
    constexpr float kEpsilon = 1e-8f;

    float denom = dot(planeNormal, ray.direction);

    if (std::abs(denom) < kEpsilon)
        return false;  // Ray parallel to plane

    Vec3 p0l0 = planePoint - ray.origin;
    t = dot(p0l0, planeNormal) / denom;

    return (t >= ray.tMin && t <= ray.tMax);
}

/**
 * @brief Ray-Plane intersection (Plane object overload)
 */
inline bool RayPlaneIntersect(const Ray& ray, const Plane& plane, float& t)
{
    return RayPlaneIntersect(ray, plane.normal, plane.GetPoint(), t);
}

// =============================================================================
// AABB Tests
// =============================================================================

/// Check if two boxes overlap
inline bool Overlaps(const AABB& a, const AABB& b)
{
    return a.Overlaps(b);
}

/// Check if box A contains box B
inline bool Contains(const AABB& a, const AABB& b)
{
    return a.Contains(b);
}

/// Check if a point is inside a box
inline bool Contains(const AABB& box, const Vec3& point)
{
    return box.Contains(point);
}

// =============================================================================
// Sphere Tests
// =============================================================================

/// Check if two spheres overlap
inline bool Overlaps(const Sphere& a, const Sphere& b)
{
    return a.Overlaps(b);
}

/// Check if sphere A contains sphere B
inline bool Contains(const Sphere& a, const Sphere& b)
{
    return a.Contains(b);
}

/// Check if a point is inside a sphere
inline bool Contains(const Sphere& sphere, const Vec3& point)
{
    return sphere.Contains(point);
}

// =============================================================================
// AABB-Sphere Tests
// =============================================================================

/// Check if a box and sphere overlap
inline bool Overlaps(const AABB& box, const Sphere& sphere)
{
    if (!box.IsValid() || !sphere.IsValid()) return false;

    // Find closest point on box to sphere center
    Vec3 closest = glm::clamp(sphere.GetCenter(), box.GetMin(), box.GetMax());
    float distSq = glm::dot(closest - sphere.GetCenter(), closest - sphere.GetCenter());
    return distSq <= sphere.GetRadius() * sphere.GetRadius();
}

inline bool Overlaps(const Sphere& sphere, const AABB& box)
{
    return Overlaps(box, sphere);
}

// =============================================================================
// Frustum Tests
// =============================================================================

/// Check if frustum intersects box (returns true if visible)
inline bool Overlaps(const Frustum& frustum, const AABB& box)
{
    return frustum.IsVisible(box);
}

inline bool Overlaps(const AABB& box, const Frustum& frustum)
{
    return frustum.IsVisible(box);
}

/// Check if frustum intersects sphere
inline bool Overlaps(const Frustum& frustum, const Sphere& sphere)
{
    return frustum.IsVisible(sphere);
}

inline bool Overlaps(const Sphere& sphere, const Frustum& frustum)
{
    return frustum.IsVisible(sphere);
}

/// Check if a point is inside a frustum
inline bool Contains(const Frustum& frustum, const Vec3& point)
{
    return frustum.Contains(point);
}

// =============================================================================
// Distance Functions
// =============================================================================

/// Squared distance from a point to the nearest point on a box
inline float DistanceSquared(const Vec3& point, const AABB& box)
{
    if (!box.IsValid()) return std::numeric_limits<float>::max();
    
    Vec3 closest = glm::clamp(point, box.GetMin(), box.GetMax());
    return glm::dot(point - closest, point - closest);
}

/// Distance from a point to the nearest point on a box
inline float Distance(const Vec3& point, const AABB& box)
{
    return std::sqrt(DistanceSquared(point, box));
}

/// Squared distance from a point to a sphere surface
inline float DistanceSquared(const Vec3& point, const Sphere& sphere)
{
    if (!sphere.IsValid()) return std::numeric_limits<float>::max();
    
    float dist = glm::length(point - sphere.GetCenter()) - sphere.GetRadius();
    if (dist < 0) return 0.0f;
    return dist * dist;
}

/// Distance from a point to a sphere surface
inline float Distance(const Vec3& point, const Sphere& sphere)
{
    if (!sphere.IsValid()) return std::numeric_limits<float>::max();
    
    float dist = glm::length(point - sphere.GetCenter()) - sphere.GetRadius();
    return dist > 0 ? dist : 0.0f;
}

// =============================================================================
// Barycentric Coordinates
// =============================================================================

/**
 * @brief Compute barycentric coordinates
 */
inline Vec3 ComputeBarycentric(
    const Vec3& p,
    const Vec3& v0,
    const Vec3& v1,
    const Vec3& v2)
{
    Vec3 e0 = v1 - v0;
    Vec3 e1 = v2 - v0;
    Vec3 e2 = p - v0;

    float d00 = dot(e0, e0);
    float d01 = dot(e0, e1);
    float d11 = dot(e1, e1);
    float d20 = dot(e2, e0);
    float d21 = dot(e2, e1);

    float denom = d00 * d11 - d01 * d01;
    if (std::abs(denom) < 1e-8f)
        return Vec3(1.0f, 0.0f, 0.0f);

    float v = (d11 * d20 - d01 * d21) / denom;
    float w = (d00 * d21 - d01 * d20) / denom;
    float u = 1.0f - v - w;

    return Vec3(u, v, w);
}

/**
 * @brief Interpolate vertex attribute using barycentric coordinates
 */
template<typename T>
inline T InterpolateBarycentric(
    const T& a0,
    const T& a1,
    const T& a2,
    const Vec3& bary)
{
    return a0 * bary.x + a1 * bary.y + a2 * bary.z;
}

} // namespace RVX
