/**
 * @file GeometryFwd.h
 * @brief Forward declarations and common types for Geometry module
 */

#pragma once

#include "Core/MathTypes.h"
#include <cstdint>
#include <cfloat>

namespace RVX::Geometry
{

// ============================================================================
// Forward Declarations
// ============================================================================

struct Line;
struct Segment;
struct Triangle;
struct Polygon;
struct OBB;
struct Capsule;
struct Cylinder;
struct Cone;

// ============================================================================
// Shape Type Enumeration
// ============================================================================

/**
 * @brief Shape type enumeration for runtime type identification and dispatch
 */
enum class ShapeType : uint8_t
{
    Point,
    Line,
    Segment,
    Ray,
    Triangle,
    Quad,
    Polygon,
    AABB,
    OBB,
    Sphere,
    Capsule,
    Cylinder,
    Cone,
    Frustum,
    ConvexHull,
    Mesh
};

// ============================================================================
// Query Results
// ============================================================================

/**
 * @brief Result of an intersection query
 */
struct HitResult
{
    float distance = FLT_MAX;       ///< Distance to hit point
    Vec3 point{0.0f};               ///< Hit position in world space
    Vec3 normal{0.0f, 1.0f, 0.0f};  ///< Surface normal at hit point
    Vec2 uv{0.0f};                  ///< Texture/barycentric coordinates
    bool hit = false;               ///< Whether a hit occurred

    /// Check if this is a valid hit
    bool IsValid() const { return hit && distance < FLT_MAX; }

    /// Invalidate this result
    void Invalidate()
    {
        distance = FLT_MAX;
        hit = false;
    }
};

/**
 * @brief Result of a distance query between two shapes
 */
struct DistanceResult
{
    float distance = FLT_MAX;       ///< Distance between shapes (0 if overlapping)
    Vec3 closestPointA{0.0f};       ///< Closest point on shape A
    Vec3 closestPointB{0.0f};       ///< Closest point on shape B

    /// Check if shapes are overlapping
    bool IsOverlapping() const { return distance <= 0.0f; }
};

} // namespace RVX::Geometry
