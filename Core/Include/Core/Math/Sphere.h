/**
 * @file Sphere.h
 * @brief Bounding sphere for spatial queries
 */

#pragma once

#include "Core/MathTypes.h"
#include "Core/Math/AABB.h"
#include <cmath>

namespace RVX
{

/**
 * @brief Bounding sphere for spatial queries
 * 
 * More efficient for rotation-invariant objects
 * and spherical proximity queries
 */
class Sphere
{
public:
    /// Default constructor creates an invalid sphere
    Sphere() 
        : m_center(0.0f)
        , m_radius(-1.0f) 
    {}
    
    /// Construct from center and radius
    Sphere(const Vec3& center, float radius) 
        : m_center(center)
        , m_radius(radius) 
    {}

    /// Construct from bounding box
    explicit Sphere(const AABB& box)
    {
        if (box.IsValid())
        {
            m_center = box.GetCenter();
            m_radius = glm::length(box.GetExtent());
        }
        else
        {
            m_center = Vec3(0.0f);
            m_radius = -1.0f;
        }
    }

    // =========================================================================
    // Accessors
    // =========================================================================
    
    const Vec3& GetCenter() const { return m_center; }
    float GetRadius() const { return m_radius; }
    
    void SetCenter(const Vec3& center) { m_center = center; }
    void SetRadius(float radius) { m_radius = radius; }
    
    bool IsValid() const { return m_radius >= 0.0f; }

    // =========================================================================
    // Modification
    // =========================================================================

    /// Reset to invalid state
    void Reset()
    {
        m_center = Vec3(0.0f);
        m_radius = -1.0f;
    }

    /// Expand to include a point
    void Expand(const Vec3& point)
    {
        if (!IsValid())
        {
            m_center = point;
            m_radius = 0.0f;
            return;
        }

        Vec3 diff = point - m_center;
        float dist = glm::length(diff);
        
        if (dist > m_radius)
        {
            float newRadius = (m_radius + dist) * 0.5f;
            float ratio = (newRadius - m_radius) / dist;
            m_center += diff * ratio;
            m_radius = newRadius;
        }
    }

    /// Expand to include another sphere
    void Expand(const Sphere& other)
    {
        if (!other.IsValid()) return;
        if (!IsValid())
        {
            *this = other;
            return;
        }

        Vec3 diff = other.m_center - m_center;
        float dist = glm::length(diff);

        // Other sphere is inside this one
        if (dist + other.m_radius <= m_radius) return;

        // This sphere is inside other one
        if (dist + m_radius <= other.m_radius)
        {
            *this = other;
            return;
        }

        // Spheres overlap or are separate
        float newRadius = (dist + m_radius + other.m_radius) * 0.5f;
        if (dist > 0.0f)
        {
            m_center += diff * ((newRadius - m_radius) / dist);
        }
        m_radius = newRadius;
    }

    /// Translate the sphere
    void Translate(const Vec3& t)
    {
        m_center += t;
    }

    // =========================================================================
    // Queries
    // =========================================================================

    /// Check if a point is inside the sphere
    bool Contains(const Vec3& point) const
    {
        if (!IsValid()) return false;
        return glm::length(point - m_center) <= m_radius;
    }

    /// Check if another sphere is fully contained
    bool Contains(const Sphere& other) const
    {
        if (!IsValid() || !other.IsValid()) return false;
        float dist = glm::length(other.m_center - m_center);
        return dist + other.m_radius <= m_radius;
    }

    /// Check if two spheres overlap
    bool Overlaps(const Sphere& other) const
    {
        if (!IsValid() || !other.IsValid()) return false;
        float dist = glm::length(other.m_center - m_center);
        return dist <= m_radius + other.m_radius;
    }

    /// Check if sphere overlaps with a box
    bool Overlaps(const AABB& box) const
    {
        if (!IsValid() || !box.IsValid()) return false;

        // Find closest point on box to sphere center
        Vec3 closest = glm::clamp(m_center, box.GetMin(), box.GetMax());
        float distSq = glm::dot(closest - m_center, closest - m_center);
        return distSq <= m_radius * m_radius;
    }

    /// Get the distance from a point to the sphere surface
    /// Negative if inside the sphere
    float DistanceTo(const Vec3& point) const
    {
        return glm::length(point - m_center) - m_radius;
    }

    // =========================================================================
    // Metrics
    // =========================================================================

    float SurfaceArea() const
    {
        if (!IsValid()) return 0.0f;
        return 4.0f * 3.14159265359f * m_radius * m_radius;
    }

    float Volume() const
    {
        if (!IsValid()) return 0.0f;
        return (4.0f / 3.0f) * 3.14159265359f * m_radius * m_radius * m_radius;
    }

    /// Convert to bounding box
    AABB ToAABB() const
    {
        if (!IsValid()) return AABB();
        Vec3 ext(m_radius);
        return AABB(m_center - ext, m_center + ext);
    }

private:
    Vec3 m_center;
    float m_radius;
};

// Backward compatibility alias
using BoundingSphere = Sphere;

} // namespace RVX
