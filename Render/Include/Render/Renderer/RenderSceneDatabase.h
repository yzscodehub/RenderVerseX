#pragma once

/**
 * @file RenderSceneDatabase.h
 * @brief Transactional persistent value database populated by scene updates.
 */

#include "Core/Types.h"
#include "RenderContracts/RenderSceneUpdate.h"

#include <deque>
#include <optional>
#include <unordered_map>
#include <vector>

namespace RVX
{
    enum class RenderSceneUpdateApplyCode : uint8
    {
        Applied = 0,
        UnsupportedSchema,
        OutOfOrder,
        RevisionGap,
        InvalidMutation
    };

    struct RenderSceneUpdateApplyResult
    {
        RenderSceneUpdateApplyCode code =
            RenderSceneUpdateApplyCode::InvalidMutation;
        uint64 previousRevision = 0;
        uint64 acceptedRevision = 0;
        bool fullReset = false;
        bool sceneMutated = false;

        [[nodiscard]] bool IsApplied() const noexcept
        {
            return code == RenderSceneUpdateApplyCode::Applied;
        }
    };

    /** @brief Compact IDs changed across a contiguous accepted revision range. */
    struct RenderSceneDatabaseChanges
    {
        uint64 baseRevision = 0;
        uint64 targetRevision = 0;
        bool available = false;
        bool fullReset = false;
        bool skyChanged = false;
        bool environmentChanged = false;
        std::vector<uint64> primitives;
        std::vector<uint64> lights;
        std::vector<uint64> decals;
        std::vector<uint64> probes;
        std::vector<uint64> particles;
        std::vector<uint64> water;
        std::vector<uint64> terrain;

        [[nodiscard]] bool Empty() const noexcept
        {
            return !fullReset && !skyChanged && !environmentChanged &&
                   primitives.empty() && lights.empty() && decals.empty() &&
                   probes.empty() && particles.empty() && water.empty() &&
                   terrain.empty();
        }
    };

    /**
     * @brief Render-thread-owned persistent scene snapshot database.
     *
     * Full resets build replacement tables. Incremental updates prebuild value
     * nodes, reserve growth, and commit only after every fallible copy succeeds,
     * so invalid or allocation-failed batches cannot publish partial scene state.
     */
    class RenderSceneDatabase final
    {
    public:
        RenderSceneDatabase();
        [[nodiscard]] RenderSceneUpdateApplyResult Apply(
            const RenderSceneUpdateBatch& batch);
        void Clear();

        [[nodiscard]] uint64 GetInstanceId() const noexcept
        {
            return m_instanceId;
        }
        [[nodiscard]] uint64 GetRevision() const noexcept { return m_revision; }
        /** @brief Collect O(changed IDs) deltas when retained history is contiguous. */
        [[nodiscard]] RenderSceneDatabaseChanges CollectChangesSince(
            uint64 baseRevision) const;
        /** @brief Release history already consumed by the sole RenderScene reader. */
        void AcknowledgeChangesThrough(uint64 revision) noexcept;
        [[nodiscard]] size_t GetPrimitiveCount() const noexcept
        {
            return m_primitives.size();
        }
        [[nodiscard]] size_t GetLightCount() const noexcept
        {
            return m_lights.size();
        }
        [[nodiscard]] size_t GetDecalCount() const noexcept
        {
            return m_decals.size();
        }
        [[nodiscard]] size_t GetProbeCount() const noexcept
        {
            return m_probes.size();
        }
        [[nodiscard]] const RenderPrimitiveSnapshot* FindPrimitive(
            uint64 objectId) const noexcept;
        [[nodiscard]] uint64 GetPrimitiveRevision(
            uint64 objectId) const noexcept;
        [[nodiscard]] const RenderLightSnapshot* FindLight(
            uint64 lightId) const noexcept;
        [[nodiscard]] const RenderDecalSnapshot* FindDecal(
            uint64 decalId) const noexcept;
        [[nodiscard]] const RenderProbeSnapshot* FindProbe(
            uint64 probeId) const noexcept;

        [[nodiscard]] const auto& GetPrimitives() const noexcept
        {
            return m_primitives;
        }
        [[nodiscard]] const auto& GetLights() const noexcept { return m_lights; }
        [[nodiscard]] const auto& GetDecals() const noexcept { return m_decals; }
        [[nodiscard]] const auto& GetProbes() const noexcept { return m_probes; }
        [[nodiscard]] const auto& GetParticles() const noexcept
        {
            return m_particles;
        }
        [[nodiscard]] const auto& GetWater() const noexcept { return m_water; }
        [[nodiscard]] const auto& GetTerrain() const noexcept { return m_terrain; }
        [[nodiscard]] const std::optional<RenderSkySnapshot>& GetSky() const noexcept
        {
            return m_sky;
        }
        [[nodiscard]] const std::optional<RenderEnvironmentSnapshot>&
            GetEnvironment() const noexcept
        {
            return m_environment;
        }

    private:
        struct ChangeJournalEntry
        {
            uint64 baseRevision = 0;
            uint64 targetRevision = 0;
            bool fullReset = false;
            bool skyChanged = false;
            bool environmentChanged = false;
            std::vector<uint64> primitives;
            std::vector<uint64> lights;
            std::vector<uint64> decals;
            std::vector<uint64> probes;
            std::vector<uint64> particles;
            std::vector<uint64> water;
            std::vector<uint64> terrain;
        };

        [[nodiscard]] static ChangeJournalEntry BuildChangeJournalEntry(
            const RenderSceneUpdateBatch& batch);

        uint64 m_instanceId = 0;
        uint64 m_revision = 0;
        std::unordered_map<uint64, RenderPrimitiveSnapshot> m_primitives;
        std::unordered_map<uint64, uint64> m_primitiveRevisions;
        std::unordered_map<uint64, RenderLightSnapshot> m_lights;
        std::unordered_map<uint64, RenderDecalSnapshot> m_decals;
        std::unordered_map<uint64, RenderProbeSnapshot> m_probes;
        std::unordered_map<uint32, uint32> m_primitiveGenerations;
        std::unordered_map<uint32, uint32> m_lightGenerations;
        std::unordered_map<uint32, uint32> m_decalGenerations;
        std::unordered_map<uint32, uint32> m_probeGenerations;
        std::unordered_map<uint64, ParticleRenderSnapshotItem> m_particles;
        std::unordered_map<uint64, WaterRenderSnapshotItem> m_water;
        std::unordered_map<uint64, TerrainRenderSnapshotItem> m_terrain;
        std::unordered_map<uint32, uint32> m_waterGenerations;
        std::unordered_map<uint32, uint32> m_terrainGenerations;
        std::optional<RenderSkySnapshot> m_sky;
        std::optional<RenderEnvironmentSnapshot> m_environment;
        std::deque<ChangeJournalEntry> m_changeHistory;
    };
} // namespace RVX
