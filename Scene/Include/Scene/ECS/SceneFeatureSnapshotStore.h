#pragma once

/**
 * @file SceneFeatureSnapshotStore.h
 * @brief Scene-owned value cache contributed to one frozen ECS render snapshot.
 */

#include "Scene/ECS/FrozenSceneSnapshot.h"

#include <optional>
#include <unordered_map>

namespace RVX::SceneECS
{
    /** @brief Generation-qualified source retained without a Registry reference. */
    struct SceneFeatureSnapshotSource
    {
        ECS::SceneRuntimeId sceneRuntimeId;
        ECS::EntityHandle entity = ECS::EntityHandle::Invalid();

        [[nodiscard]] bool IsValid() const
        {
            return sceneRuntimeId.IsValid() && entity.IsValid();
        }

        bool operator==(const SceneFeatureSnapshotSource&) const = default;
    };

    /** @brief Point-in-time counts for diagnostics and focused validation. */
    struct SceneFeatureSnapshotStoreCounts
    {
        uint32 particleCount = 0;
        uint32 waterCount = 0;
        uint32 terrainCount = 0;
    };

    /**
     * @brief Owner-thread feature output cache subordinate to exactly one Scene runtime.
     *
     * Feature processors validate a live EntityRef only while publishing, then
     * this store retains only value data and a generation-qualified source. At
     * the presentation boundary FreezeInto copies those values into a
     * FrozenSceneSnapshot; neither the store nor the frozen values retain a
     * Registry, Fragment, Actor, Component, Resource, or RHI reference.
     */
    class SceneFeatureSnapshotStore final
    {
    public:
        explicit SceneFeatureSnapshotStore(ECS::SceneRuntimeId sceneRuntimeId);

        [[nodiscard]] ECS::SceneRuntimeId GetSceneRuntimeId() const
        {
            return m_sceneRuntimeId;
        }

        /** @brief Capture a valid source identity before deferred cleanup invalidates its EntityRef. */
        [[nodiscard]] std::optional<SceneFeatureSnapshotSource> MakeSourceIdentity(
            const ECS::EntityRef& source) const;

        [[nodiscard]] bool PublishParticle(const ECS::EntityRef& source,
                                           ParticleRenderSnapshotItem state,
                                           uint64 payloadRevision = 0);
        [[nodiscard]] bool PublishWater(const ECS::EntityRef& source,
                                        WaterRenderSnapshotItem state,
                                        uint64 payloadRevision = 0);
        [[nodiscard]] bool PublishTerrain(const ECS::EntityRef& source,
                                          TerrainRenderSnapshotItem state,
                                          uint64 payloadRevision = 0);

        /**
         * @brief Remove one feature value, or every value, for an exact recorded identity.
         *
         * These intentionally accept a value identity instead of EntityRef so
         * EndFrameCleanup can remove render values after Registry liveness has
         * changed. Generation remains part of every key.
         */
        [[nodiscard]] bool RemoveParticle(const SceneFeatureSnapshotSource& source);
        [[nodiscard]] bool RemoveWater(const SceneFeatureSnapshotSource& source);
        [[nodiscard]] bool RemoveTerrain(const SceneFeatureSnapshotSource& source);
        [[nodiscard]] bool Remove(const SceneFeatureSnapshotSource& source);
        void Clear();

        /**
         * @brief Deep-copy cached feature values into one immutable scene snapshot.
         *
         * The destination must belong to this exact Scene runtime. On failure
         * it remains unchanged, so callers can fail closed rather than publish
         * a partial feature set.
         */
        [[nodiscard]] bool FreezeInto(FrozenSceneSnapshot& destination) const;

        [[nodiscard]] SceneFeatureSnapshotStoreCounts GetCounts() const;

    private:
        template<typename State>
        struct Entry
        {
            State state;
            uint64 payloadRevision = 0;
        };

        [[nodiscard]] bool IsOwnedSource(
            const SceneFeatureSnapshotSource& source) const;

        ECS::SceneRuntimeId m_sceneRuntimeId;
        std::unordered_map<ECS::EntityHandle, Entry<ParticleRenderSnapshotItem>>
            m_particles;
        std::unordered_map<ECS::EntityHandle, Entry<WaterRenderSnapshotItem>>
            m_water;
        std::unordered_map<ECS::EntityHandle, Entry<TerrainRenderSnapshotItem>>
            m_terrain;
    };
} // namespace RVX::SceneECS
