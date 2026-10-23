#pragma once

/**
 * @file SceneFeatureSnapshotStore.h
 * @brief Scene-owned value cache contributed to one frozen ECS render snapshot.
 */

#include "Scene/ECS/FrozenSceneSnapshot.h"

#include "RenderContracts/RenderFeatureChannelRegistry.h"

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

        [[nodiscard]] RenderFeatureSourceIdentity ToRenderIdentity() const noexcept
        {
            return {
                .sceneRuntimeId = sceneRuntimeId.GetValue(),
                .entitySlot = entity.GetIndex(),
                .entityGeneration = entity.GetGeneration(),
            };
        }
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
     * FrozenSceneSnapshot. A sealed ParticleCpuPacket remains an owned value
     * of the snapshot payload, preserving the store's value-snapshot contract.
     * Neither the store nor the frozen values retain a
     * live ECS::Registry, Fragment, Actor, Component, Resource, or RHI reference.
     *
     * GFX01 deliberately keeps fixed Particle/Water/Terrain storage. The
     * sealed composition registry validates those typed routes; it does not
     * make this store a dynamic plug-in payload container.
     */
    class SceneFeatureSnapshotStore final
    {
    public:
        explicit SceneFeatureSnapshotStore(ECS::SceneRuntimeId sceneRuntimeId);
        SceneFeatureSnapshotStore(
            ECS::SceneRuntimeId sceneRuntimeId,
            RenderFeatureChannelRegistry channelRegistry);

        [[nodiscard]] ECS::SceneRuntimeId GetSceneRuntimeId() const
        {
            return m_sceneRuntimeId;
        }

        [[nodiscard]] const RenderFeatureChannelRegistry&
        GetChannelRegistry() const noexcept
        {
            return m_channelRegistry;
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

        /** @brief Publish an already-versioned value; invalid or stale headers fail closed. */
        [[nodiscard]] bool PublishParticleChannel(
            const ECS::EntityRef& source,
            ParticleRenderSnapshotItem state);
        [[nodiscard]] bool PublishWaterChannel(
            const ECS::EntityRef& source,
            WaterRenderSnapshotItem state);
        [[nodiscard]] bool PublishTerrainChannel(
            const ECS::EntityRef& source,
            TerrainRenderSnapshotItem state);

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
         * @brief Copy cached feature envelopes into one immutable scene snapshot.
         *
         * The destination must belong to this exact Scene runtime. On failure
         * it remains unchanged, so callers can fail closed rather than publish
         * a partial feature set. Sealed particle packets remain owned values,
         * so each frozen snapshot is independent of later source mutation.
         */
        [[nodiscard]] bool FreezeInto(FrozenSceneSnapshot& destination) const;

        [[nodiscard]] SceneFeatureSnapshotStoreCounts GetCounts() const;

    private:
        [[nodiscard]] bool IsOwnedSource(
            const SceneFeatureSnapshotSource& source) const;

        ECS::SceneRuntimeId m_sceneRuntimeId;
        RenderFeatureChannelRegistry m_channelRegistry;
        std::unordered_map<ECS::EntityHandle, ParticleRenderSnapshotItem>
            m_particles;
        std::unordered_map<ECS::EntityHandle, WaterRenderSnapshotItem>
            m_water;
        std::unordered_map<ECS::EntityHandle, TerrainRenderSnapshotItem>
            m_terrain;
    };
} // namespace RVX::SceneECS
