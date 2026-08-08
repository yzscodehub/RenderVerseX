#pragma once

/**
 * @file RenderSceneDatabase.h
 * @brief Transactional persistent value database populated by scene updates.
 */

#include "Core/Types.h"
#include "RenderContracts/RenderFramePacket.h"
#include "RenderContracts/RenderFramePacketV5.h"
#include "RenderContracts/RenderSceneUpdate.h"

#include <optional>
#include <unordered_map>

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

    struct RenderSceneShadowComparison
    {
        bool matches = false;
        uint32 missingPrimitiveCount = 0;
        uint32 changedPrimitiveCount = 0;
        uint32 missingLightCount = 0;
        uint32 changedLightCount = 0;
        uint32 featureMismatchCount = 0;
        bool skyMismatch = false;
        bool environmentMismatch = false;
    };

    /**
     * @brief Render-thread-owned persistent scene snapshot database.
     *
     * Apply() builds candidate state and swaps it only after every mutation has
     * succeeded, so an invalid batch cannot partially update the accepted scene.
     */
    class RenderSceneDatabase final
    {
    public:
        [[nodiscard]] RenderSceneUpdateApplyResult Apply(
            const RenderSceneUpdateBatch& batch);
        void Clear();

        [[nodiscard]] RenderSceneShadowComparison Compare(
            const RenderFramePacket& packet) const;
        [[nodiscard]] std::unique_ptr<const RenderFramePacket>
            BuildCompatibilityFrame(const RenderFramePacketV5& frame) const;

        [[nodiscard]] uint64 GetRevision() const noexcept { return m_revision; }
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
        uint64 m_revision = 0;
        std::unordered_map<uint64, RenderPrimitiveSnapshot> m_primitives;
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
    };
} // namespace RVX
