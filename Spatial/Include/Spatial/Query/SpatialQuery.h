#pragma once

/**
 * @file SpatialQuery.h
 * @brief Query result types for spatial queries
 */

#include "Core/MathTypes.h"
#include <cstdint>
#include <vector>

namespace RVX::Spatial
{
    /// Entity handle type
    using EntityHandle = uint32_t;
    static constexpr EntityHandle InvalidEntityHandle = ~0u;

    /**
     * @brief Result of a spatial query
     */
    struct QueryResult
    {
        /// Handle to the entity
        EntityHandle handle = InvalidEntityHandle;

        /// Distance from query origin (for ray/point queries)
        float distance = 0.0f;

        /// Sort key (can be distance or custom)
        float sortKey = 0.0f;

        /// User data pointer (optional)
        void* userData = nullptr;

        bool IsValid() const { return handle != InvalidEntityHandle; }

        bool operator<(const QueryResult& other) const
        {
            return sortKey < other.sortKey;
        }
    };

    /**
     * @brief Extended query result with hit information
     */
    struct RaycastResult : QueryResult
    {
        /// World-space hit point
        Vec3 hitPoint{0.0f};

        /// Surface normal at hit point
        Vec3 hitNormal{0.0f, 1.0f, 0.0f};

        /// UV coordinates at hit (if available)
        Vec2 hitUV{0.0f};

        /// Triangle/primitive index (if applicable)
        int primitiveIndex = -1;
    };

    /**
     * @brief Query type enumeration
     */
    enum class QueryType
    {
        Frustum,    // Frustum culling query
        Box,        // Box overlap query
        Sphere,     // Sphere overlap query
        Ray,        // Ray intersection query
        Point       // Point containment query
    };

} // namespace RVX::Spatial
