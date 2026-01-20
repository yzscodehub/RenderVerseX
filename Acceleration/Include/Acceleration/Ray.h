/**
 * @file Ray.h
 * @brief Ray structure for ray casting and intersection tests
 */

#pragma once

#include "Core/MathTypes.h"
#include <limits>

namespace RVX
{

/**
 * @brief Ray defined by origin and direction
 */
struct Ray
{
    Vec3 origin{0.0f};
    Vec3 direction{0.0f, 0.0f, -1.0f};  // Default: looking down -Z
    float tMin{0.0f};
    float tMax{std::numeric_limits<float>::max()};

    Ray() = default;

    Ray(const Vec3& o, const Vec3& d)
        : origin(o), direction(normalize(d)) {}

    Ray(const Vec3& o, const Vec3& d, float minT, float maxT)
        : origin(o), direction(normalize(d)), tMin(minT), tMax(maxT) {}

    /**
     * @brief Get point along ray at parameter t
     */
    Vec3 At(float t) const
    {
        return origin + direction * t;
    }

    /**
     * @brief Create ray from two points
     */
    static Ray FromPoints(const Vec3& from, const Vec3& to)
    {
        Vec3 dir = to - from;
        float len = length(dir);
        if (len > 0.0f)
        {
            return Ray(from, dir / len, 0.0f, len);
        }
        return Ray(from, Vec3(0.0f, 0.0f, -1.0f));
    }

    /**
     * @brief Transform ray by matrix
     */
    Ray Transform(const Mat4& matrix) const
    {
        Vec3 newOrigin = Vec3(matrix * Vec4(origin, 1.0f));
        Vec3 newDir = normalize(Vec3(matrix * Vec4(direction, 0.0f)));
        return Ray(newOrigin, newDir, tMin, tMax);
    }

    /**
     * @brief Get precomputed inverse direction for slab method
     */
    Vec3 GetInverseDirection() const
    {
        return Vec3(
            1.0f / (std::abs(direction.x) > 1e-8f ? direction.x : 1e-8f),
            1.0f / (std::abs(direction.y) > 1e-8f ? direction.y : 1e-8f),
            1.0f / (std::abs(direction.z) > 1e-8f ? direction.z : 1e-8f)
        );
    }
};

/**
 * @brief Ray hit information
 */
struct RayHit
{
    float t{std::numeric_limits<float>::max()};  // Hit distance
    Vec3 position{0.0f};                          // Hit position
    Vec3 normal{0.0f, 1.0f, 0.0f};               // Surface normal
    Vec2 uv{0.0f};                                // Texture coordinates
    int primitiveIndex{-1};                       // Triangle/primitive index
    int subMeshIndex{-1};                         // SubMesh index
    int meshIndex{-1};                            // Mesh index
    int nodeIndex{-1};                            // Node index

    bool IsValid() const { return t < std::numeric_limits<float>::max(); }
    void Invalidate() { t = std::numeric_limits<float>::max(); }
};

} // namespace RVX
