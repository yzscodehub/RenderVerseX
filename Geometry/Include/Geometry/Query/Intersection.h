/**
 * @file Intersection.h
 * @brief Intersection tests for geometry primitives
 * 
 * Extends Core/Math/Intersection.h with additional primitives.
 */

#pragma once

#include "Core/MathTypes.h"
#include "Core/Math/Ray.h"
#include "Core/Math/AABB.h"
#include "Core/Math/Sphere.h"
#include "Core/Math/Plane.h"
#include "Geometry/GeometryFwd.h"
#include "Geometry/Constants.h"
#include "Geometry/Primitives/OBB.h"
#include "Geometry/Primitives/Capsule.h"
#include "Geometry/Primitives/Triangle.h"
#include "Geometry/Primitives/Cylinder.h"
#include "Geometry/Primitives/Cone.h"
#include "Geometry/Primitives/Line.h"
#include <cmath>

namespace RVX::Geometry
{

// ============================================================================
// Ray-OBB Intersection
// ============================================================================

/**
 * @brief Ray-OBB intersection test
 * @param ray The ray to test
 * @param obb The oriented bounding box
 * @param outT Distance to hit point (if hit)
 * @return true if the ray intersects the OBB
 */
inline bool RayOBBIntersect(const Ray& ray, const OBB& obb, float& outT)
{
    // Transform ray to OBB local space
    Quat invOrient = glm::conjugate(obb.orientation);
    Vec3 localOrigin = invOrient * (ray.origin - obb.center);
    Vec3 localDir = invOrient * ray.direction;

    // Now test against axis-aligned box at origin
    Vec3 invDir(
        std::abs(localDir.x) > EPSILON ? 1.0f / localDir.x : 1e8f,
        std::abs(localDir.y) > EPSILON ? 1.0f / localDir.y : 1e8f,
        std::abs(localDir.z) > EPSILON ? 1.0f / localDir.z : 1e8f
    );

    Vec3 t1 = (-obb.halfExtents - localOrigin) * invDir;
    Vec3 t2 = (obb.halfExtents - localOrigin) * invDir;

    Vec3 tNear = glm::min(t1, t2);
    Vec3 tFar = glm::max(t1, t2);

    float tMin = std::max({tNear.x, tNear.y, tNear.z, ray.tMin});
    float tMax = std::min({tFar.x, tFar.y, tFar.z, ray.tMax});

    if (tMin > tMax)
        return false;

    outT = tMin >= ray.tMin ? tMin : tMax;
    return outT >= ray.tMin && outT <= ray.tMax;
}

/**
 * @brief Ray-OBB intersection with hit result
 */
inline bool RayOBBIntersect(const Ray& ray, const OBB& obb, HitResult& hit)
{
    float t;
    if (!RayOBBIntersect(ray, obb, t))
        return false;

    if (t >= hit.distance)
        return false;

    hit.distance = t;
    hit.point = ray.At(t);
    hit.hit = true;

    // Compute normal - find which face was hit
    Vec3 localPoint = obb.WorldToLocal(hit.point);
    Vec3 absLocal = glm::abs(localPoint);
    Vec3 localNormal(0.0f);

    float maxComp = 0.0f;
    for (int i = 0; i < 3; ++i)
    {
        float diff = std::abs(absLocal[i] - obb.halfExtents[i]);
        if (diff < EPSILON && absLocal[i] > maxComp)
        {
            maxComp = absLocal[i];
            localNormal = Vec3(0.0f);
            localNormal[i] = localPoint[i] > 0 ? 1.0f : -1.0f;
        }
    }

    hit.normal = obb.orientation * localNormal;
    return true;
}

// ============================================================================
// Ray-Capsule Intersection
// ============================================================================

/**
 * @brief Ray-Capsule intersection test
 * @param ray The ray to test
 * @param capsule The capsule
 * @param outT Distance to hit point (if hit)
 * @return true if the ray intersects the capsule
 */
inline bool RayCapsuleIntersect(const Ray& ray, const Capsule& capsule, float& outT)
{
    Vec3 d = capsule.b - capsule.a;
    Vec3 m = ray.origin - capsule.a;
    Vec3 n = ray.direction;

    float md = glm::dot(m, d);
    float nd = glm::dot(n, d);
    float dd = glm::dot(d, d);

    // Test if segment is degenerate (capsule becomes a sphere)
    if (dd < EPSILON)
    {
        // Treat as sphere at point a
        Vec3 oc = ray.origin - capsule.a;
        float a = glm::dot(n, n);
        float b = 2.0f * glm::dot(oc, n);
        float c = glm::dot(oc, oc) - capsule.radius * capsule.radius;
        float discriminant = b * b - 4.0f * a * c;

        if (discriminant < 0.0f)
            return false;

        float sqrtD = std::sqrt(discriminant);
        float t1 = (-b - sqrtD) / (2.0f * a);
        if (t1 >= ray.tMin && t1 <= ray.tMax)
        {
            outT = t1;
            return true;
        }

        float t2 = (-b + sqrtD) / (2.0f * a);
        if (t2 >= ray.tMin && t2 <= ray.tMax)
        {
            outT = t2;
            return true;
        }
        return false;
    }

    float mn = glm::dot(m, n);
    float a = dd * glm::dot(n, n) - nd * nd;
    float k = glm::dot(m, m) - capsule.radius * capsule.radius;
    float c = dd * k - md * md;

    if (std::abs(a) < EPSILON)
    {
        // Segment parallel to ray
        if (c > 0.0f) return false;
        
        // Now check both endcap spheres
        float minT = FLT_MAX;
        bool hit = false;

        // Check sphere at a
        {
            Vec3 oc = ray.origin - capsule.a;
            float bSphere = 2.0f * glm::dot(oc, n);
            float cSphere = glm::dot(oc, oc) - capsule.radius * capsule.radius;
            float disc = bSphere * bSphere - 4.0f * cSphere;
            if (disc >= 0.0f)
            {
                float t = (-bSphere - std::sqrt(disc)) * 0.5f;
                if (t >= ray.tMin && t <= ray.tMax && t < minT)
                {
                    minT = t;
                    hit = true;
                }
            }
        }

        // Check sphere at b
        {
            Vec3 oc = ray.origin - capsule.b;
            float bSphere = 2.0f * glm::dot(oc, n);
            float cSphere = glm::dot(oc, oc) - capsule.radius * capsule.radius;
            float disc = bSphere * bSphere - 4.0f * cSphere;
            if (disc >= 0.0f)
            {
                float t = (-bSphere - std::sqrt(disc)) * 0.5f;
                if (t >= ray.tMin && t <= ray.tMax && t < minT)
                {
                    minT = t;
                    hit = true;
                }
            }
        }

        if (hit) outT = minT;
        return hit;
    }

    float b = dd * mn - nd * md;
    float discr = b * b - a * c;

    if (discr < 0.0f)
        return false;

    float sqrtDiscr = std::sqrt(discr);
    float t = (-b - sqrtDiscr) / a;

    if (t < ray.tMin) t = (-b + sqrtDiscr) / a;
    if (t < ray.tMin || t > ray.tMax)
        return false;

    // Check if hit point is within the cylinder portion
    float hitParam = md + t * nd;

    if (hitParam < 0.0f)
    {
        // Check sphere at a
        Vec3 oc = ray.origin - capsule.a;
        float aa = glm::dot(n, n);
        float bb = 2.0f * glm::dot(oc, n);
        float cc = glm::dot(oc, oc) - capsule.radius * capsule.radius;
        float disc = bb * bb - 4.0f * aa * cc;
        if (disc < 0.0f) return false;
        t = (-bb - std::sqrt(disc)) / (2.0f * aa);
        if (t < ray.tMin || t > ray.tMax) return false;
    }
    else if (hitParam > dd)
    {
        // Check sphere at b
        Vec3 oc = ray.origin - capsule.b;
        float aa = glm::dot(n, n);
        float bb = 2.0f * glm::dot(oc, n);
        float cc = glm::dot(oc, oc) - capsule.radius * capsule.radius;
        float disc = bb * bb - 4.0f * aa * cc;
        if (disc < 0.0f) return false;
        t = (-bb - std::sqrt(disc)) / (2.0f * aa);
        if (t < ray.tMin || t > ray.tMax) return false;
    }

    outT = t;
    return true;
}

/**
 * @brief Ray-Capsule intersection with hit result
 */
inline bool RayCapsuleIntersect(const Ray& ray, const Capsule& capsule, HitResult& hit)
{
    float t;
    if (!RayCapsuleIntersect(ray, capsule, t))
        return false;

    if (t >= hit.distance)
        return false;

    hit.distance = t;
    hit.point = ray.At(t);
    hit.hit = true;

    // Compute normal
    Vec3 closest = capsule.ClosestPointOnAxis(hit.point);
    hit.normal = glm::normalize(hit.point - closest);

    return true;
}

// ============================================================================
// Ray-Cylinder Intersection
// ============================================================================

/**
 * @brief Ray-Cylinder intersection test (infinite cylinder)
 */
inline bool RayCylinderIntersect(const Ray& ray, const Cylinder& cylinder, float& outT)
{
    Vec3 d = cylinder.b - cylinder.a;
    Vec3 m = ray.origin - cylinder.a;
    Vec3 n = ray.direction;

    float md = glm::dot(m, d);
    float nd = glm::dot(n, d);
    float dd = glm::dot(d, d);

    if (dd < EPSILON)
        return false;

    float mn = glm::dot(m, n);
    float a = dd * glm::dot(n, n) - nd * nd;
    float k = glm::dot(m, m) - cylinder.radius * cylinder.radius;
    float c = dd * k - md * md;

    float minT = FLT_MAX;
    bool hasHit = false;

    if (std::abs(a) > EPSILON)
    {
        float b = dd * mn - nd * md;
        float discr = b * b - a * c;

        if (discr >= 0.0f)
        {
            float sqrtDiscr = std::sqrt(discr);

            for (int i = 0; i < 2; ++i)
            {
                float t = (-b + (i == 0 ? -sqrtDiscr : sqrtDiscr)) / a;
                if (t >= ray.tMin && t <= ray.tMax)
                {
                    float hitParam = md + t * nd;
                    if (hitParam >= 0.0f && hitParam <= dd && t < minT)
                    {
                        minT = t;
                        hasHit = true;
                    }
                }
            }
        }
    }

    // Check end caps
    Vec3 axis = d / std::sqrt(dd);
    float invNd = std::abs(nd) > EPSILON ? 1.0f / nd : 0.0f;

    if (invNd != 0.0f)
    {
        // Bottom cap at a
        float t = -md * invNd;
        if (t >= ray.tMin && t <= ray.tMax && t < minT)
        {
            Vec3 p = ray.At(t);
            Vec3 toP = p - cylinder.a;
            float radialDistSq = glm::dot(toP, toP) - Square(glm::dot(toP, axis));
            if (radialDistSq <= cylinder.radius * cylinder.radius)
            {
                minT = t;
                hasHit = true;
            }
        }

        // Top cap at b
        t = (dd - md) * invNd;
        if (t >= ray.tMin && t <= ray.tMax && t < minT)
        {
            Vec3 p = ray.At(t);
            Vec3 toP = p - cylinder.b;
            float radialDistSq = glm::dot(toP, toP) - Square(glm::dot(toP, axis));
            if (radialDistSq <= cylinder.radius * cylinder.radius)
            {
                minT = t;
                hasHit = true;
            }
        }
    }

    if (hasHit)
        outT = minT;

    return hasHit;
}

// ============================================================================
// Ray-Triangle Intersection (enhanced version)
// ============================================================================

/**
 * @brief Ray-Triangle intersection using Moller-Trumbore algorithm
 */
inline bool RayTriangleIntersect(
    const Ray& ray,
    const Triangle& tri,
    float& outT,
    float& outU,
    float& outV,
    bool cullBackface = false)
{
    constexpr float kEpsilon = 1e-8f;

    Vec3 edge1 = tri.v1 - tri.v0;
    Vec3 edge2 = tri.v2 - tri.v0;

    Vec3 h = glm::cross(ray.direction, edge2);
    float a = glm::dot(edge1, h);

    if (std::abs(a) < kEpsilon)
        return false;

    if (cullBackface && a < 0.0f)
        return false;

    float f = 1.0f / a;
    Vec3 s = ray.origin - tri.v0;
    outU = f * glm::dot(s, h);

    if (outU < 0.0f || outU > 1.0f)
        return false;

    Vec3 q = glm::cross(s, edge1);
    outV = f * glm::dot(ray.direction, q);

    if (outV < 0.0f || outU + outV > 1.0f)
        return false;

    outT = f * glm::dot(edge2, q);

    return (outT >= ray.tMin && outT <= ray.tMax);
}

/**
 * @brief Ray-Triangle intersection with hit result
 */
inline bool RayTriangleIntersect(const Ray& ray, const Triangle& tri, HitResult& hit, bool cullBackface = false)
{
    float t, u, v;
    if (!RayTriangleIntersect(ray, tri, t, u, v, cullBackface))
        return false;

    if (t >= hit.distance)
        return false;

    hit.distance = t;
    hit.point = ray.At(t);
    hit.uv = Vec2(u, v);
    hit.normal = tri.GetNormal();
    hit.hit = true;

    return true;
}

// ============================================================================
// OBB-OBB Intersection (SAT-based)
// ============================================================================

/**
 * @brief OBB-OBB intersection test using Separating Axis Theorem
 */
inline bool OBBOBBIntersect(const OBB& a, const OBB& b)
{
    Mat3 rotA = a.GetRotationMatrix();
    Mat3 rotB = b.GetRotationMatrix();

    Vec3 axesA[3] = { rotA[0], rotA[1], rotA[2] };
    Vec3 axesB[3] = { rotB[0], rotB[1], rotB[2] };

    // Compute rotation matrix expressing b in a's coordinate frame
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

    // Test axes L = A0, A1, A2
    for (int i = 0; i < 3; ++i)
    {
        float ra = a.halfExtents[i];
        float rb = b.halfExtents[0] * AbsR[i][0] + b.halfExtents[1] * AbsR[i][1] + b.halfExtents[2] * AbsR[i][2];
        if (std::abs(t[i]) > ra + rb) return false;
    }

    // Test axes L = B0, B1, B2
    for (int i = 0; i < 3; ++i)
    {
        float ra = a.halfExtents[0] * AbsR[0][i] + a.halfExtents[1] * AbsR[1][i] + a.halfExtents[2] * AbsR[2][i];
        float rb = b.halfExtents[i];
        float proj = std::abs(t[0] * R[0][i] + t[1] * R[1][i] + t[2] * R[2][i]);
        if (proj > ra + rb) return false;
    }

    // Test axis L = A0 x B0
    {
        float ra = a.halfExtents[1] * AbsR[2][0] + a.halfExtents[2] * AbsR[1][0];
        float rb = b.halfExtents[1] * AbsR[0][2] + b.halfExtents[2] * AbsR[0][1];
        if (std::abs(t[2] * R[1][0] - t[1] * R[2][0]) > ra + rb) return false;
    }

    // Test axis L = A0 x B1
    {
        float ra = a.halfExtents[1] * AbsR[2][1] + a.halfExtents[2] * AbsR[1][1];
        float rb = b.halfExtents[0] * AbsR[0][2] + b.halfExtents[2] * AbsR[0][0];
        if (std::abs(t[2] * R[1][1] - t[1] * R[2][1]) > ra + rb) return false;
    }

    // Test axis L = A0 x B2
    {
        float ra = a.halfExtents[1] * AbsR[2][2] + a.halfExtents[2] * AbsR[1][2];
        float rb = b.halfExtents[0] * AbsR[0][1] + b.halfExtents[1] * AbsR[0][0];
        if (std::abs(t[2] * R[1][2] - t[1] * R[2][2]) > ra + rb) return false;
    }

    // Test axis L = A1 x B0
    {
        float ra = a.halfExtents[0] * AbsR[2][0] + a.halfExtents[2] * AbsR[0][0];
        float rb = b.halfExtents[1] * AbsR[1][2] + b.halfExtents[2] * AbsR[1][1];
        if (std::abs(t[0] * R[2][0] - t[2] * R[0][0]) > ra + rb) return false;
    }

    // Test axis L = A1 x B1
    {
        float ra = a.halfExtents[0] * AbsR[2][1] + a.halfExtents[2] * AbsR[0][1];
        float rb = b.halfExtents[0] * AbsR[1][2] + b.halfExtents[2] * AbsR[1][0];
        if (std::abs(t[0] * R[2][1] - t[2] * R[0][1]) > ra + rb) return false;
    }

    // Test axis L = A1 x B2
    {
        float ra = a.halfExtents[0] * AbsR[2][2] + a.halfExtents[2] * AbsR[0][2];
        float rb = b.halfExtents[0] * AbsR[1][1] + b.halfExtents[1] * AbsR[1][0];
        if (std::abs(t[0] * R[2][2] - t[2] * R[0][2]) > ra + rb) return false;
    }

    // Test axis L = A2 x B0
    {
        float ra = a.halfExtents[0] * AbsR[1][0] + a.halfExtents[1] * AbsR[0][0];
        float rb = b.halfExtents[1] * AbsR[2][2] + b.halfExtents[2] * AbsR[2][1];
        if (std::abs(t[1] * R[0][0] - t[0] * R[1][0]) > ra + rb) return false;
    }

    // Test axis L = A2 x B1
    {
        float ra = a.halfExtents[0] * AbsR[1][1] + a.halfExtents[1] * AbsR[0][1];
        float rb = b.halfExtents[0] * AbsR[2][2] + b.halfExtents[2] * AbsR[2][0];
        if (std::abs(t[1] * R[0][1] - t[0] * R[1][1]) > ra + rb) return false;
    }

    // Test axis L = A2 x B2
    {
        float ra = a.halfExtents[0] * AbsR[1][2] + a.halfExtents[1] * AbsR[0][2];
        float rb = b.halfExtents[0] * AbsR[2][1] + b.halfExtents[1] * AbsR[2][0];
        if (std::abs(t[1] * R[0][2] - t[0] * R[1][2]) > ra + rb) return false;
    }

    return true;
}

// ============================================================================
// OBB-Sphere Intersection
// ============================================================================

/**
 * @brief OBB-Sphere intersection test
 */
inline bool OBBSphereIntersect(const OBB& obb, const Sphere& sphere)
{
    // Find closest point on OBB to sphere center
    Vec3 closest = obb.ClosestPoint(sphere.GetCenter());
    Vec3 diff = sphere.GetCenter() - closest;
    return glm::dot(diff, diff) <= sphere.GetRadius() * sphere.GetRadius();
}

// ============================================================================
// Capsule-Sphere Intersection
// ============================================================================

/**
 * @brief Capsule-Sphere intersection test
 */
inline bool CapsuleSphereIntersect(const Capsule& capsule, const Sphere& sphere)
{
    float distSq = capsule.GetSegment().DistanceSquared(sphere.GetCenter());
    float radiusSum = capsule.radius + sphere.GetRadius();
    return distSq <= radiusSum * radiusSum;
}

// ============================================================================
// Triangle-Triangle Intersection
// ============================================================================

namespace detail
{
    // Project triangle onto axis and get min/max
    inline void ProjectTriangle(const Vec3& axis, const Triangle& tri, float& outMin, float& outMax)
    {
        float d0 = glm::dot(axis, tri.v0);
        float d1 = glm::dot(axis, tri.v1);
        float d2 = glm::dot(axis, tri.v2);
        outMin = std::min({d0, d1, d2});
        outMax = std::max({d0, d1, d2});
    }

    // Check if intervals overlap
    inline bool IntervalsOverlap(float min1, float max1, float min2, float max2)
    {
        return !(max1 < min2 || max2 < min1);
    }

    // 2D point-in-triangle test for coplanar case
    inline bool PointInTriangle2D(const Vec2& p, const Vec2& a, const Vec2& b, const Vec2& c)
    {
        auto Sign = [](const Vec2& p1, const Vec2& p2, const Vec2& p3) {
            return (p1.x - p3.x) * (p2.y - p3.y) - (p2.x - p3.x) * (p1.y - p3.y);
        };

        float d1 = Sign(p, a, b);
        float d2 = Sign(p, b, c);
        float d3 = Sign(p, c, a);

        bool hasNeg = (d1 < 0) || (d2 < 0) || (d3 < 0);
        bool hasPos = (d1 > 0) || (d2 > 0) || (d3 > 0);

        return !(hasNeg && hasPos);
    }

    // 2D segment intersection
    inline bool SegmentIntersect2D(const Vec2& a1, const Vec2& a2, const Vec2& b1, const Vec2& b2)
    {
        Vec2 d1 = a2 - a1;
        Vec2 d2 = b2 - b1;
        Vec2 d3 = b1 - a1;

        float cross = d1.x * d2.y - d1.y * d2.x;
        if (std::abs(cross) < 1e-8f) return false;  // Parallel

        float t = (d3.x * d2.y - d3.y * d2.x) / cross;
        float u = (d3.x * d1.y - d3.y * d1.x) / cross;

        return t >= 0.0f && t <= 1.0f && u >= 0.0f && u <= 1.0f;
    }

    // Coplanar triangle-triangle intersection
    inline bool CoplanarTriangleIntersect(const Triangle& t1, const Triangle& t2, const Vec3& n)
    {
        // Project to 2D by dropping the largest component of normal
        int dropAxis = 0;
        if (std::abs(n.y) > std::abs(n.x)) dropAxis = 1;
        if (std::abs(n.z) > std::abs(n[dropAxis])) dropAxis = 2;

        auto Project2D = [dropAxis](const Vec3& v) -> Vec2 {
            switch (dropAxis)
            {
                case 0: return Vec2(v.y, v.z);
                case 1: return Vec2(v.x, v.z);
                default: return Vec2(v.x, v.y);
            }
        };

        Vec2 a0 = Project2D(t1.v0), a1 = Project2D(t1.v1), a2 = Project2D(t1.v2);
        Vec2 b0 = Project2D(t2.v0), b1 = Project2D(t2.v1), b2 = Project2D(t2.v2);

        // Check if any vertex of t1 is inside t2
        if (PointInTriangle2D(a0, b0, b1, b2)) return true;
        if (PointInTriangle2D(a1, b0, b1, b2)) return true;
        if (PointInTriangle2D(a2, b0, b1, b2)) return true;

        // Check if any vertex of t2 is inside t1
        if (PointInTriangle2D(b0, a0, a1, a2)) return true;
        if (PointInTriangle2D(b1, a0, a1, a2)) return true;
        if (PointInTriangle2D(b2, a0, a1, a2)) return true;

        // Check edge intersections
        Vec2 edges1[3][2] = {{a0, a1}, {a1, a2}, {a2, a0}};
        Vec2 edges2[3][2] = {{b0, b1}, {b1, b2}, {b2, b0}};

        for (int i = 0; i < 3; ++i)
        {
            for (int j = 0; j < 3; ++j)
            {
                if (SegmentIntersect2D(edges1[i][0], edges1[i][1], edges2[j][0], edges2[j][1]))
                    return true;
            }
        }

        return false;
    }

    // Compute interval on line for triangle
    inline void ComputeInterval(
        const Vec3& v0, const Vec3& v1, const Vec3& v2,
        float d0, float d1, float d2,
        const Vec3& D, float& t0, float& t1)
    {
        // Find the vertex that is alone on one side
        float proj0 = glm::dot(D, v0);
        float proj1 = glm::dot(D, v1);
        float proj2 = glm::dot(D, v2);

        if (d0 * d1 > 0.0f)
        {
            // v2 is alone
            t0 = proj0 + (proj2 - proj0) * d0 / (d0 - d2);
            t1 = proj1 + (proj2 - proj1) * d1 / (d1 - d2);
        }
        else if (d0 * d2 > 0.0f)
        {
            // v1 is alone
            t0 = proj0 + (proj1 - proj0) * d0 / (d0 - d1);
            t1 = proj2 + (proj1 - proj2) * d2 / (d2 - d1);
        }
        else if (d1 * d2 > 0.0f || d0 != 0.0f)
        {
            // v0 is alone
            t0 = proj1 + (proj0 - proj1) * d1 / (d1 - d0);
            t1 = proj2 + (proj0 - proj2) * d2 / (d2 - d0);
        }
        else if (d1 != 0.0f)
        {
            t0 = proj0 + (proj1 - proj0) * d0 / (d0 - d1);
            t1 = proj2 + (proj1 - proj2) * d2 / (d2 - d1);
        }
        else if (d2 != 0.0f)
        {
            t0 = proj0 + (proj2 - proj0) * d0 / (d0 - d2);
            t1 = proj1 + (proj2 - proj1) * d1 / (d1 - d2);
        }
        else
        {
            // Degenerate
            t0 = t1 = 0.0f;
        }

        if (t0 > t1) std::swap(t0, t1);
    }
}

/**
 * @brief Triangle-Triangle intersection test using Moller's algorithm
 */
inline bool TriangleTriangleIntersect(const Triangle& t1, const Triangle& t2)
{
    // Get plane of t1
    Vec3 n1 = t1.GetNormal();
    float d1 = -glm::dot(n1, t1.v0);

    // Signed distances of t2 vertices to t1's plane
    float d2_0 = glm::dot(n1, t2.v0) + d1;
    float d2_1 = glm::dot(n1, t2.v1) + d1;
    float d2_2 = glm::dot(n1, t2.v2) + d1;

    // Clamp small values to zero
    if (std::abs(d2_0) < EPSILON) d2_0 = 0.0f;
    if (std::abs(d2_1) < EPSILON) d2_1 = 0.0f;
    if (std::abs(d2_2) < EPSILON) d2_2 = 0.0f;

    // All vertices on same side means no intersection
    if (d2_0 * d2_1 > 0.0f && d2_0 * d2_2 > 0.0f)
        return false;

    // Get plane of t2
    Vec3 n2 = t2.GetNormal();
    float d2 = -glm::dot(n2, t2.v0);

    // Signed distances of t1 vertices to t2's plane
    float d1_0 = glm::dot(n2, t1.v0) + d2;
    float d1_1 = glm::dot(n2, t1.v1) + d2;
    float d1_2 = glm::dot(n2, t1.v2) + d2;

    // Clamp small values
    if (std::abs(d1_0) < EPSILON) d1_0 = 0.0f;
    if (std::abs(d1_1) < EPSILON) d1_1 = 0.0f;
    if (std::abs(d1_2) < EPSILON) d1_2 = 0.0f;

    // All vertices on same side means no intersection
    if (d1_0 * d1_1 > 0.0f && d1_0 * d1_2 > 0.0f)
        return false;

    // Compute intersection line direction
    Vec3 D = glm::cross(n1, n2);
    float DDot = glm::dot(D, D);

    if (DDot < EPSILON)
    {
        // Triangles are coplanar
        // Check if they're on the same plane
        float planeDist = std::abs(glm::dot(n1, t2.v0) + d1);
        if (planeDist < EPSILON)
        {
            return detail::CoplanarTriangleIntersect(t1, t2, n1);
        }
        return false;
    }

    // Compute intervals on intersection line
    float t1_min, t1_max, t2_min, t2_max;

    detail::ComputeInterval(t1.v0, t1.v1, t1.v2, d1_0, d1_1, d1_2, D, t1_min, t1_max);
    detail::ComputeInterval(t2.v0, t2.v1, t2.v2, d2_0, d2_1, d2_2, D, t2_min, t2_max);

    // Check if intervals overlap
    return detail::IntervalsOverlap(t1_min, t1_max, t2_min, t2_max);
}

} // namespace RVX::Geometry
