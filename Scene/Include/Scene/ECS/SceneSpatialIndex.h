#pragma once

/**
 * @file SceneSpatialIndex.h
 * @brief Handle-only derived spatial index for the pure runtime Scene ECS.
 */

#include "Core/Math/AABB.h"
#include "ECS/Entity.h"

#include <unordered_map>
#include <vector>

namespace RVX::ECS
{
    class Registry;
}

namespace RVX::SceneECS
{
    /** @brief Value identity returned by spatial queries; never owns ECS state. */
    struct SpatialEntityId
    {
        ECS::SceneRuntimeId sceneRuntimeId;
        ECS::EntityHandle entity = ECS::EntityHandle::Invalid();

        [[nodiscard]] bool IsValid() const
        {
            return sceneRuntimeId.IsValid() && entity.IsValid();
        }

        friend bool operator==(const SpatialEntityId&, const SpatialEntityId&) = default;
    };

    /** @brief One immutable value result from a spatial query. */
    struct SpatialQueryResult
    {
        SpatialEntityId id;
        AABB worldBounds;
        uint32 layer = 0;
    };

    /** @brief Diagnostics for one authoritative ECS-to-index synchronization. */
    struct SpatialSynchronizeStats
    {
        uint32 inserted = 0;
        uint32 updated = 0;
        uint32 removed = 0;
        uint32 unchanged = 0;
        uint32 indexed = 0;
    };

    /**
     * @brief Derived broad-phase view over ECS Bounds and resolved render transforms.
     *
     * Registry fragments remain authoritative. Synchronize rebuilds membership from
     * enabled, active, Alive entities and retains no fragment or object pointers.
     * The initial implementation uses a flat value table; a BVH can replace the
     * internal representation without changing this handle-only API.
     */
    class SceneSpatialIndex
    {
    public:
        explicit SceneSpatialIndex(ECS::Registry& registry);

        SceneSpatialIndex(const SceneSpatialIndex&) = delete;
        SceneSpatialIndex& operator=(const SceneSpatialIndex&) = delete;

        [[nodiscard]] SpatialSynchronizeStats Synchronize();
        [[nodiscard]] std::vector<SpatialQueryResult> QueryAabb(
            const AABB& bounds,
            uint32 layerMask = ~0u) const;
        [[nodiscard]] std::vector<SpatialQueryResult> QueryPoint(
            const Vec3& point,
            uint32 layerMask = ~0u) const;

        [[nodiscard]] uint32 GetEntryCount() const
        {
            return static_cast<uint32>(m_entries.size());
        }

    private:
        struct Entry
        {
            SpatialQueryResult value;
            uint64 boundsWriteVersion = 0;
            uint64 transformWriteVersion = 0;
            uint64 activeWriteVersion = 0;
            uint64 layerWriteVersion = 0;
            uint64 lifecycleWriteVersion = 0;
        };

        [[nodiscard]] static bool IsLayerSelected(uint32 layer, uint32 mask);
        [[nodiscard]] static AABB MakeWorldBounds(const Vec3& center,
                                                  const Vec3& extents,
                                                  const Mat4& worldTransform);

        ECS::Registry* m_registry = nullptr;
        ECS::SceneRuntimeId m_sceneRuntimeId;
        std::unordered_map<ECS::EntityHandle, Entry> m_entries;
    };
} // namespace RVX::SceneECS
