#pragma once

/**
 * @file ISpatialIndex.h
 * @brief Abstract interface for spatial indexing structures
 */

#include "Core/Math/Geometry.h"
#include "Spatial/Query/QueryFilter.h"
#include "Spatial/Query/SpatialQuery.h"
#include "Spatial/Index/ISpatialEntity.h"
#include <span>
#include <vector>
#include <memory>

namespace RVX::Spatial
{
    /**
     * @brief Statistics for spatial index
     */
    struct IndexStats
    {
        size_t entityCount = 0;
        size_t nodeCount = 0;
        size_t memoryBytes = 0;
        int maxDepth = 0;
        float avgEntitiesPerLeaf = 0.0f;
        float buildTimeMs = 0.0f;
    };

    /**
     * @brief Debug renderer interface for visualization
     */
    class IDebugRenderer
    {
    public:
        virtual ~IDebugRenderer() = default;
        virtual void DrawBox(const AABB& box, const Vec4& color) = 0;
        virtual void DrawSphere(const Sphere& sphere, const Vec4& color) = 0;
        virtual void DrawLine(const Vec3& start, const Vec3& end, const Vec4& color) = 0;
    };

    /**
     * @brief Abstract interface for spatial indexing structures
     * 
     * Provides unified API for:
     * - BVH (Bounding Volume Hierarchy)
     * - Octree
     * - Uniform Grid
     * - Other spatial acceleration structures
     * 
     * Used by:
     * - Render system (frustum culling)
     * - Physics system (collision detection)
     * - Picking system (ray queries)
     * - AI system (range queries)
     */
    class ISpatialIndex
    {
    public:
        virtual ~ISpatialIndex() = default;

        // =====================================================================
        // Build & Update
        // =====================================================================

        /// Build index from a collection of entities
        virtual void Build(std::span<ISpatialEntity*> entities) = 0;

        /// Clear all entities
        virtual void Clear() = 0;

        /// Insert a single entity
        virtual void Insert(ISpatialEntity* entity) = 0;

        /// Remove an entity by handle
        virtual void Remove(EntityHandle handle) = 0;

        /// Update an entity's position in the index
        virtual void Update(ISpatialEntity* entity) = 0;

        /// Commit pending updates (for batched operations)
        virtual void Commit() = 0;

        // =====================================================================
        // Queries
        // =====================================================================

        /// Query entities visible in a frustum
        virtual void QueryFrustum(
            const Frustum& frustum,
            const QueryFilter& filter,
            std::vector<QueryResult>& outResults) const = 0;

        /// Query entities overlapping a box
        virtual void QueryBox(
            const AABB& box,
            const QueryFilter& filter,
            std::vector<QueryResult>& outResults) const = 0;

        /// Query entities within a sphere
        virtual void QuerySphere(
            const Vec3& center,
            float radius,
            const QueryFilter& filter,
            std::vector<QueryResult>& outResults) const = 0;

        /// Ray query - find nearest intersection
        virtual bool QueryRay(
            const Ray& ray,
            const QueryFilter& filter,
            QueryResult& outResult) const = 0;

        /// Ray query - find all intersections
        virtual void QueryRayAll(
            const Ray& ray,
            const QueryFilter& filter,
            std::vector<QueryResult>& outResults) const = 0;

        // =====================================================================
        // Statistics & Debug
        // =====================================================================

        /// Get index statistics
        virtual IndexStats GetStats() const = 0;

        /// Debug visualization
        virtual void DebugDraw(IDebugRenderer* renderer, int maxDepth = -1) const = 0;

        // =====================================================================
        // Utility
        // =====================================================================

        /// Get total entity count
        virtual size_t GetEntityCount() const = 0;

        /// Check if index is empty
        virtual bool IsEmpty() const { return GetEntityCount() == 0; }

        /// Get world bounds
        virtual AABB GetWorldBounds() const = 0;
    };

    /// Unique pointer type for spatial index
    using SpatialIndexPtr = std::unique_ptr<ISpatialIndex>;

} // namespace RVX::Spatial
