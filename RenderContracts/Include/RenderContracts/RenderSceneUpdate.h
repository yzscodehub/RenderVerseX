#pragma once

/**
 * @file RenderSceneUpdate.h
 * @brief Reliable revisioned mutations for the persistent render scene
 */

#include "RenderContracts/RenderFrameTypes.h"

#include <array>
#include <optional>
#include <unordered_map>
#include <vector>

namespace RVX
{
    inline constexpr uint32 RVX_RENDER_SCENE_UPDATE_SCHEMA_ID = 0x52565355U;
    inline constexpr uint32 RVX_RENDER_SCENE_UPDATE_SCHEMA_VERSION = 4;

    enum class RenderSceneMutationOperation : uint8
    {
        Upsert = 0,
        Remove
    };

    [[nodiscard]] bool AreRenderPrimitiveSnapshotsEqual(
        const RenderPrimitiveSnapshot& left,
        const RenderPrimitiveSnapshot& right) noexcept;
    [[nodiscard]] bool AreRenderLightSnapshotsEqual(
        const RenderLightSnapshot& left,
        const RenderLightSnapshot& right) noexcept;
    enum class RenderDecalBlendMode : uint8
    {
        Default = 0,
        Stain,
        Emissive,
        Normal,
    };

    struct RenderDecalSnapshot
    {
        uint64 decalId = 0;
        Mat4 worldTransform{1.0f};
        Vec3 halfExtent{0.5f, 0.5f, 0.25f};
        RenderResourceHandle material;
        Vec4 color{1.0f};
        float32 opacity = 1.0f;
        float32 normalStrength = 1.0f;
        float32 angleFade = 0.5f;
        float32 fadeDistance = 10.0f;
        float32 fadeWidth = 2.0f;
        uint32 layerMask = ~0u;
        int32 sortOrder = 0;
        RenderDecalBlendMode blendMode = RenderDecalBlendMode::Default;
    };

    enum class RenderProbeKind : uint8
    {
        Reflection = 0,
        Light,
    };

    enum class RenderProbeShape : uint8
    {
        Point = 0,
        Box,
        Sphere,
    };

    enum class RenderProbeMode : uint8
    {
        Baked = 0,
        Realtime,
        Custom,
    };

    struct RenderProbeSnapshot
    {
        uint64 probeId = 0;
        RenderProbeKind kind = RenderProbeKind::Reflection;
        RenderProbeShape shape = RenderProbeShape::Box;
        RenderProbeMode mode = RenderProbeMode::Baked;
        Mat4 worldTransform{1.0f};
        Vec3 influenceExtent{1.0f};
        float32 blendDistance = 0.0f;
        Vec3 boxProjectionSize{1.0f};
        Vec3 boxProjectionOffset{0.0f};
        RenderResourceHandle texture;
        std::array<Vec4, 7> sphericalHarmonics{};
        uint32 cullingMask = ~0u;
        int32 priority = 0;
        float32 nearClip = 0.1f;
        float32 farClip = 100.0f;
        bool useBoxProjection = false;
        bool useHDR = false;
        bool hasValidData = false;
    };

    [[nodiscard]] bool AreRenderDecalSnapshotsEqual(
        const RenderDecalSnapshot& left,
        const RenderDecalSnapshot& right) noexcept;
    [[nodiscard]] bool AreRenderProbeSnapshotsEqual(
        const RenderProbeSnapshot& left,
        const RenderProbeSnapshot& right) noexcept;
    [[nodiscard]] bool AreRenderSkySnapshotsEqual(
        const RenderSkySnapshot& left,
        const RenderSkySnapshot& right) noexcept;
    [[nodiscard]] bool AreRenderEnvironmentSnapshotsEqual(
        const RenderEnvironmentSnapshot& left,
        const RenderEnvironmentSnapshot& right) noexcept;
    [[nodiscard]] bool AreParticleRenderSnapshotItemsEqual(
        const ParticleRenderSnapshotItem& left,
        const ParticleRenderSnapshotItem& right) noexcept;
    [[nodiscard]] bool AreWaterRenderSnapshotItemsEqual(
        const WaterRenderSnapshotItem& left,
        const WaterRenderSnapshotItem& right) noexcept;
    [[nodiscard]] bool AreTerrainRenderSnapshotItemsEqual(
        const TerrainRenderSnapshotItem& left,
        const TerrainRenderSnapshotItem& right) noexcept;

    struct RenderPrimitiveMutation
    {
        RenderSceneMutationOperation operation = RenderSceneMutationOperation::Upsert;
        uint64 objectId = 0;
        RenderPrimitiveSnapshot state;
    };

    struct RenderLightMutation
    {
        RenderSceneMutationOperation operation = RenderSceneMutationOperation::Upsert;
        uint64 lightId = 0;
        RenderLightSnapshot state;
    };

    struct RenderDecalMutation
    {
        RenderSceneMutationOperation operation = RenderSceneMutationOperation::Upsert;
        uint64 decalId = 0;
        RenderDecalSnapshot state;
    };

    struct RenderProbeMutation
    {
        RenderSceneMutationOperation operation = RenderSceneMutationOperation::Upsert;
        uint64 probeId = 0;
        RenderProbeSnapshot state;
    };

    struct RenderParticleMutation
    {
        RenderSceneMutationOperation operation = RenderSceneMutationOperation::Upsert;
        uint64 instanceId = 0;
        ParticleRenderSnapshotItem state;
    };

    struct RenderWaterMutation
    {
        RenderSceneMutationOperation operation = RenderSceneMutationOperation::Upsert;
        uint64 componentId = 0;
        WaterRenderSnapshotItem state;
    };

    struct RenderTerrainMutation
    {
        RenderSceneMutationOperation operation = RenderSceneMutationOperation::Upsert;
        uint64 componentId = 0;
        TerrainRenderSnapshotItem state;
    };

    template<typename State>
    struct RenderSingletonMutation
    {
        RenderSceneMutationOperation operation = RenderSceneMutationOperation::Upsert;
        State state;
    };

    struct RenderSceneUpdateBatch
    {
        uint32 schemaId = RVX_RENDER_SCENE_UPDATE_SCHEMA_ID;
        uint32 schemaVersion = RVX_RENDER_SCENE_UPDATE_SCHEMA_VERSION;
        uint64 baseSceneRevision = 0;
        uint64 targetSceneRevision = 0;
        bool fullReset = false;
        std::vector<RenderPrimitiveMutation> primitives;
        std::vector<RenderLightMutation> lights;
        std::vector<RenderDecalMutation> decals;
        std::vector<RenderProbeMutation> probes;
        std::optional<RenderSingletonMutation<RenderSkySnapshot>> sky;
        std::optional<RenderSingletonMutation<RenderEnvironmentSnapshot>> environment;
        std::vector<RenderParticleMutation> particles;
        std::vector<RenderWaterMutation> water;
        std::vector<RenderTerrainMutation> terrain;

        [[nodiscard]] bool IsStructurallyValid() const noexcept;
        [[nodiscard]] bool Empty() const noexcept;
    };

    /**
     * @brief Coalesces complete-state mutations without losing remove ordering.
     */
    class RenderSceneMutationAccumulator
    {
    public:
        void Begin(uint64 baseSceneRevision, bool fullReset = false);
        void Clear();

        bool UpsertPrimitive(RenderPrimitiveSnapshot state, bool newlyCreated = false);
        bool RemovePrimitive(uint64 objectId);
        bool UpsertLight(RenderLightSnapshot state, bool newlyCreated = false);
        bool RemoveLight(uint64 lightId);
        bool UpsertDecal(RenderDecalSnapshot state, bool newlyCreated = false);
        bool RemoveDecal(uint64 decalId);
        bool UpsertProbe(RenderProbeSnapshot state, bool newlyCreated = false);
        bool RemoveProbe(uint64 probeId);
        bool UpsertParticle(ParticleRenderSnapshotItem state, bool newlyCreated = false);
        bool RemoveParticle(uint64 instanceId);
        bool UpsertWater(WaterRenderSnapshotItem state, bool newlyCreated = false);
        bool RemoveWater(uint64 componentId);
        bool UpsertTerrain(TerrainRenderSnapshotItem state, bool newlyCreated = false);
        bool RemoveTerrain(uint64 componentId);
        void UpsertSky(RenderSkySnapshot state);
        void RemoveSky();
        void UpsertEnvironment(RenderEnvironmentSnapshot state);
        void RemoveEnvironment();

        [[nodiscard]] RenderSceneUpdateBatch Build(uint64 targetSceneRevision) const;
        [[nodiscard]] uint64 GetBaseSceneRevision() const { return m_baseSceneRevision; }
        [[nodiscard]] bool IsFullReset() const { return m_fullReset; }

    private:
        template<typename State>
        struct PendingState
        {
            RenderSceneMutationOperation operation = RenderSceneMutationOperation::Upsert;
            State state;
            bool newlyCreated = false;
        };

        template<typename State>
        bool Upsert(std::unordered_map<uint64, PendingState<State>>& states,
                    uint64 id,
                    State state,
                    bool newlyCreated);
        template<typename State>
        bool Remove(std::unordered_map<uint64, PendingState<State>>& states,
                    uint64 id);

        uint64 m_baseSceneRevision = 0;
        bool m_fullReset = false;
        std::unordered_map<uint64, PendingState<RenderPrimitiveSnapshot>> m_primitives;
        std::unordered_map<uint64, PendingState<RenderLightSnapshot>> m_lights;
        std::unordered_map<uint64, PendingState<RenderDecalSnapshot>> m_decals;
        std::unordered_map<uint64, PendingState<RenderProbeSnapshot>> m_probes;
        std::unordered_map<uint64, PendingState<ParticleRenderSnapshotItem>> m_particles;
        std::unordered_map<uint64, PendingState<WaterRenderSnapshotItem>> m_water;
        std::unordered_map<uint64, PendingState<TerrainRenderSnapshotItem>> m_terrain;
        std::optional<RenderSingletonMutation<RenderSkySnapshot>> m_sky;
        std::optional<RenderSingletonMutation<RenderEnvironmentSnapshot>> m_environment;
    };
} // namespace RVX
