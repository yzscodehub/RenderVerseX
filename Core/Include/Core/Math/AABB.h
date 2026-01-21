/**
 * @file AABB.h
 * @brief Axis-Aligned Bounding Box (AABB) for spatial queries
 */

#pragma once

#include "Core/MathTypes.h"
#include <memory>
#include <limits>

namespace RVX
{

/**
 * @brief Axis-Aligned Bounding Box (AABB)
 * 
 * Used for:
 * - Spatial culling and visibility testing
 * - BVH construction (provides SurfaceArea for SAH heuristic)
 * - Mouse picking acceleration
 */
class AABB
{
public:
    using Ptr = std::shared_ptr<AABB>;
    
    /// Default constructor creates an invalid (empty) bounding box
    AABB() 
        : m_min(std::numeric_limits<float>::max())
        , m_max(-std::numeric_limits<float>::max()) 
    {}
    
    /// Construct from min/max corners
    AABB(const Vec3& min, const Vec3& max) 
        : m_min(min), m_max(max) 
    {}

    AABB(const AABB& other) 
        : m_min(other.m_min), m_max(other.m_max) 
    {}
    
    AABB& operator=(const AABB& other)
    {
        if (this != &other)
        {
            m_min = other.m_min;
            m_max = other.m_max;
        }
        return *this;
    }

    // =========================================================================
    // Accessors
    // =========================================================================
    
    const Vec3& GetMin() const { return m_min; }
    const Vec3& GetMax() const { return m_max; }
    Vec3& GetMin() { return m_min; }
    Vec3& GetMax() { return m_max; }
    
    void SetMin(const Vec3& min) { m_min = min; }
    void SetMax(const Vec3& max) { m_max = max; }

    /// Get the center point of the bounding box
    Vec3 GetCenter() const
    {
        return (m_min + m_max) * 0.5f;
    }
    
    /// Get the size (width, height, depth)
    Vec3 GetSize() const
    {
        return m_max - m_min;
    }

    /// Get the half-extents
    Vec3 GetExtent() const
    {
        return 0.5f * (m_max - m_min);
    }

    /// Get the diagonal vector
    Vec3 GetDiagonal() const
    {
        return m_max - m_min;
    }
    
    /// Check if the bounding box is valid (min <= max on all axes)
    bool IsValid() const
    {
        return m_min.x <= m_max.x && 
               m_min.y <= m_max.y && 
               m_min.z <= m_max.z;
    }

    // =========================================================================
    // Modification
    // =========================================================================

    /// Reset to invalid (empty) state
    void Reset()
    {
        m_min = Vec3(std::numeric_limits<float>::max());
        m_max = Vec3(-std::numeric_limits<float>::max());
    }

    /// Expand the bounding box to include a point
    void Expand(const Vec3& p)
    {
        if (!IsValid())
        {
            m_min = m_max = p;
            return;
        }
        m_min = glm::min(m_min, p);
        m_max = glm::max(m_max, p);
    }

    /// Expand the bounding box to include another bounding box
    void Expand(const AABB& b)
    {
        if (!b.IsValid()) return;
        Expand(b.m_min);
        Expand(b.m_max);
    }

    /// Inflate uniformly by delta on all sides
    void Inflate(float delta)
    {
        if (!IsValid()) return;
        m_min -= Vec3(delta);
        m_max += Vec3(delta);
    }

    void Inflate(const Vec3& delta)
    {
        if (!IsValid()) return;
        m_min -= delta;
        m_max += delta;
    }

    void Translate(const Vec3& t)
    {
        if (!IsValid()) return;
        m_min += t;
        m_max += t;
    }

    // =========================================================================
    // Queries
    // =========================================================================

    /// Return the union of this and another box
    AABB Union(const AABB& b) const
    {
        if (!IsValid()) return b;
        if (!b.IsValid()) return *this;
        AABB out = *this;
        out.m_min = glm::min(out.m_min, b.m_min);
        out.m_max = glm::max(out.m_max, b.m_max);
        return out;
    }

    /// Return the intersection with another box (invalid if disjoint)
    AABB Intersection(const AABB& b) const
    {
        if (!IsValid() || !b.IsValid()) return AABB();
        AABB out;
        out.m_min = glm::max(m_min, b.m_min);
        out.m_max = glm::min(m_max, b.m_max);
        if (!out.IsValid())
        {
            out.Reset();
        }
        return out;
    }

    /// Check if a point is contained within the bounding box
    bool Contains(const Vec3& p) const
    {
        if (!IsValid()) return false;
        return p.x >= m_min.x && p.x <= m_max.x &&
               p.y >= m_min.y && p.y <= m_max.y &&
               p.z >= m_min.z && p.z <= m_max.z;
    }

    /// Check if another bounding box is fully contained
    bool Contains(const AABB& b) const
    {
        if (!IsValid() || !b.IsValid()) return false;
        return b.m_min.x >= m_min.x && b.m_max.x <= m_max.x &&
               b.m_min.y >= m_min.y && b.m_max.y <= m_max.y &&
               b.m_min.z >= m_min.z && b.m_max.z <= m_max.z;
    }

    /// Check if two bounding boxes overlap
    bool Overlaps(const AABB& b) const
    {
        if (!IsValid() || !b.IsValid()) return false;
        const bool sep = (m_max.x < b.m_min.x) || (b.m_max.x < m_min.x) ||
                         (m_max.y < b.m_min.y) || (b.m_max.y < m_min.y) ||
                         (m_max.z < b.m_min.z) || (b.m_max.z < m_min.z);
        return !sep;
    }

    // =========================================================================
    // Metrics (used for BVH SAH heuristic)
    // =========================================================================

    /// Calculate the surface area of the bounding box
    /// Used for SAH (Surface Area Heuristic) in BVH construction
    float SurfaceArea() const
    {
        if (!IsValid()) return 0.0f;
        const Vec3 s = GetSize();
        return 2.0f * (s.x * s.y + s.x * s.z + s.y * s.z);
    }

    /// Calculate the volume of the bounding box
    float Volume() const
    {
        if (!IsValid()) return 0.0f;
        const Vec3 s = GetSize();
        return s.x * s.y * s.z;
    }

    /// Return an inflated copy
    AABB Inflated(float delta) const
    {
        if (!IsValid()) return *this;
        AABB out = *this;
        out.Inflate(delta);
        return out;
    }

    AABB Inflated(const Vec3& delta) const
    {
        if (!IsValid()) return *this;
        AABB out = *this;
        out.Inflate(delta);
        return out;
    }

    // =========================================================================
    // Transform
    // =========================================================================

    /// Transform the bounding box by a matrix
    AABB Transformed(const Mat4& transform) const
    {
        if (!IsValid()) return *this;

        // Get the 8 corners and transform them
        Vec3 corners[8] = {
            Vec3(m_min.x, m_min.y, m_min.z),
            Vec3(m_max.x, m_min.y, m_min.z),
            Vec3(m_min.x, m_max.y, m_min.z),
            Vec3(m_max.x, m_max.y, m_min.z),
            Vec3(m_min.x, m_min.y, m_max.z),
            Vec3(m_max.x, m_min.y, m_max.z),
            Vec3(m_min.x, m_max.y, m_max.z),
            Vec3(m_max.x, m_max.y, m_max.z)
        };

        AABB result;
        for (const auto& corner : corners)
        {
            Vec4 transformed = transform * Vec4(corner, 1.0f);
            result.Expand(Vec3(transformed));
        }
        return result;
    }

private:
    Vec3 m_min;
    Vec3 m_max;
};

// Backward compatibility alias
using BoundingBox = AABB;

} // namespace RVX
