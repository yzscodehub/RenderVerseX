#pragma once

/**
 * @file BVHIndex.h
 * @brief BVH-based spatial index implementation
 */

#include "Spatial/Index/ISpatialIndex.h"
#include <unordered_map>

namespace RVX::Spatial
{
    /**
     * @brief BVH configuration
     */
    struct BVHConfig
    {
        /// Maximum entities per leaf node
        int maxLeafSize = 4;

        /// Number of buckets for SAH
        int sahBuckets = 12;

        /// Cost ratio for traversal vs intersection
        float traversalCost = 1.0f;
        float intersectionCost = 1.0f;

        /// Whether to use SAH or simple median split
        bool useSAH = true;
    };

    /**
     * @brief BVH-based spatial index
     * 
     * Features:
     * - SAH (Surface Area Heuristic) construction
     * - Efficient frustum culling
     * - Ray intersection queries
     * - Incremental updates
     */
    class BVHIndex : public ISpatialIndex
    {
    public:
        explicit BVHIndex(const BVHConfig& config = {});
        ~BVHIndex() override;

        // =====================================================================
        // ISpatialIndex Implementation
        // =====================================================================

        void Build(std::span<ISpatialEntity*> entities) override;
        void Clear() override;
        void Insert(ISpatialEntity* entity) override;
        void Remove(EntityHandle handle) override;
        void Update(ISpatialEntity* entity) override;
        void Commit() override;

        void QueryFrustum(
            const Frustum& frustum,
            const QueryFilter& filter,
            std::vector<QueryResult>& outResults) const override;

        void QueryBox(
            const AABB& box,
            const QueryFilter& filter,
            std::vector<QueryResult>& outResults) const override;

        void QuerySphere(
            const Vec3& center,
            float radius,
            const QueryFilter& filter,
            std::vector<QueryResult>& outResults) const override;

        bool QueryRay(
            const Ray& ray,
            const QueryFilter& filter,
            QueryResult& outResult) const override;

        void QueryRayAll(
            const Ray& ray,
            const QueryFilter& filter,
            std::vector<QueryResult>& outResults) const override;

        IndexStats GetStats() const override;
        void DebugDraw(IDebugRenderer* renderer, int maxDepth = -1) const override;

        size_t GetEntityCount() const override { return m_entities.size(); }
        AABB GetWorldBounds() const override;

    private:
        struct Node
        {
            AABB bounds;
            int leftChild = -1;   // -1 means leaf
            int rightChild = -1;
            int firstPrimitive = 0;
            int primitiveCount = 0;

            bool IsLeaf() const { return leftChild < 0; }
        };

        BVHConfig m_config;
        std::vector<Node> m_nodes;
        std::vector<ISpatialEntity*> m_entities;
        std::vector<int> m_primitiveIndices;
        std::unordered_map<EntityHandle, size_t> m_entityIndex;

        // Pending updates
        std::vector<ISpatialEntity*> m_pendingInserts;
        std::vector<EntityHandle> m_pendingRemoves;
        bool m_needsRebuild = false;

        // Build helpers
        int BuildRecursive(int start, int end, int depth);
        int FindBestSplit(int start, int end, int& outAxis, float& outPos);
        int Partition(int start, int end, int axis, float splitPos);

        // Query helpers
        void QueryFrustumRecursive(int nodeIdx, const Frustum& frustum,
            const QueryFilter& filter, std::vector<QueryResult>& results) const;
        void QueryBoxRecursive(int nodeIdx, const AABB& box,
            const QueryFilter& filter, std::vector<QueryResult>& results) const;
        void QuerySphereRecursive(int nodeIdx, const Vec3& center, float radius,
            const QueryFilter& filter, std::vector<QueryResult>& results) const;
        bool QueryRayRecursive(int nodeIdx, const Ray& ray,
            const QueryFilter& filter, float& closestT, QueryResult& result) const;
        void QueryRayAllRecursive(int nodeIdx, const Ray& ray,
            const QueryFilter& filter, std::vector<QueryResult>& results) const;

        bool IntersectRayBox(const Ray& ray, const AABB& box, float& tMin, float& tMax) const;
    };

} // namespace RVX::Spatial
