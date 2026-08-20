#pragma once

/**
 * @file EcsFrozenSceneBridge.h
 * @brief Pure ECS frozen-scene to render-source value adapter.
 */

#include "RenderContracts/RenderFrameTypes.h"
#include "RenderContracts/RenderProxy.h"
#include "Scene/ECS/FrozenSceneSnapshot.h"

#include <optional>
#include <vector>

namespace RVX
{
    /** @brief Failure reason for a complete frozen-scene bridge build. */
    enum class EcsFrozenSceneBridgeCode : uint8
    {
        Complete = 0,
        InvalidSceneRuntimeId,
        InvalidSourceIdentity,
        SourceIdentityMismatch,
        UnexpectedSourceType,
        DuplicateSourceIdentity,
        InvalidDerivedRenderId,
        RenderIdCollision,
        InvalidMaterialSlotCount,
        SubmeshMaterialSlotMismatch,
        InvalidSkinningPalette,
        MultipleSkyboxes,
        SelectedCameraMissing,
    };

    /**
     * @brief One unresolved material binding retained beside RenderProxySnapshot.
     *
     * RenderProxySnapshot intentionally has no identity slot per material binding.
     * This companion value supplies that identity without creating a Resource or
     * RHI dependency in the ECS extraction boundary.
     */
    struct EcsFrozenSceneMaterialBindingValue
    {
        uint64 id = 0;
        RenderProxyId primitiveId;
        uint32 slot = 0;
        AssetId materialAssetId;
        RenderMaterialMode materialMode = RenderMaterialMode::Opaque;
    };

    /**
     * @brief Temporal source data that RenderProxySnapshot cannot represent yet.
     *
     * The common ECS payload is the sole source for Direct and GPU-driven
     * consumers. A future frame extractor can copy this previous transform into
     * RenderPrimitiveSnapshot without consulting Scene, World, or Resource.
     */
    struct EcsFrozenScenePrimitiveTemporalValue
    {
        RenderProxyId primitiveId;
        uint64 ownerId = 0;
        /** @brief Exact ECS identity; never inferred from an address. */
        ECS::EntityHandle sourceEntity = ECS::EntityHandle::Invalid();
        /** @brief Entity-local Visibility version copied with this primitive source. */
        uint64 visibilityWriteVersion = 0;
        Mat4 previousWorldTransform{1.0f};
        uint64 worldTransformSourceRevision = 0;
        uint64 previousWorldTransformSourceRevision = 0;
    };

    /**
     * @brief Light source values retained where RenderLightProxy has no layer or visibility fields.
     *
     * `proxy` always mirrors the light's renderable fields. Invisible sources
     * are kept here for full snapshot fidelity but are deliberately omitted
     * from RenderProxySnapshot because that legacy contract cannot carry an
     * enabled/visible bit for lights.
     */
    struct EcsFrozenSceneLightValue
    {
        uint64 id = 0;
        ECS::EntityHandle sourceEntity = ECS::EntityHandle::Invalid();
        RenderLightProxy proxy;
        uint32 layerMask = ~0u;
        bool visible = true;
        uint64 transformSourceRevision = 0;
    };

    /** @brief View source copied from a frozen ECS camera without viewport allocation policy. */
    struct EcsFrozenSceneCameraValue
    {
        uint64 id = 0;
        RenderViewSnapshot view;
        Vec4 normalizedViewport{0.0f, 0.0f, 1.0f, 1.0f};
        SceneECS::CameraClearPolicy clearPolicy =
            SceneECS::CameraClearPolicy::Skybox;
        Vec4 clearColor{0.0f, 0.0f, 0.0f, 1.0f};
        uint32 cullingMask = ~0u;
        int32 priority = 0;
        uint64 transformSourceRevision = 0;
    };

    /**
     * @brief Unresolved sky and IBL asset source for a future frame extractor.
     *
     * AssetId values remain deliberately unresolved. Resource residency and
     * RenderResourceHandle construction happen only in the downstream resource
     * stage.
     */
    struct EcsFrozenSceneSkyboxValue
    {
        uint64 id = 0;
        ECS::EntityHandle sourceEntity = ECS::EntityHandle::Invalid();
        /** @brief Entity-local Skybox write version carried for activation proof. */
        uint64 skyboxWriteVersion = 0;
        RenderSkyMode mode = RenderSkyMode::Disabled;
        AssetId environmentAssetId;
        AssetId prefilteredEnvironmentAssetId;
        AssetId irradianceAssetId;
        AssetId brdfLutAssetId;
        Vec3 solidColor{0.1f, 0.1f, 0.15f};
        Vec3 sunDirection{0.0f, 1.0f, 0.0f};
        Vec3 sunColor{1.0f, 0.95f, 0.9f};
        Vec3 zenithColor{0.2f, 0.4f, 0.8f};
        Vec3 horizonColor{0.7f, 0.8f, 0.9f};
        Vec3 groundColor{0.3f, 0.25f, 0.2f};
        float32 exposure = 1.0f;
        float32 rotationRadians = 0.0f;
        float32 blur = 0.0f;
        float32 scatteringIntensity = 1.0f;
        bool contributesToLighting = true;
    };

    /** @brief Particle value with a stable ECS-derived RenderScene identity. */
    struct EcsFrozenSceneParticleValue
    {
        uint64 id = 0;
        ECS::EntityHandle sourceEntity = ECS::EntityHandle::Invalid();
        uint64 payloadRevision = 0;
        ParticleRenderSnapshotItem state;
    };

    /** @brief Water value with a stable ECS-derived RenderScene identity. */
    struct EcsFrozenSceneWaterValue
    {
        uint64 id = 0;
        ECS::EntityHandle sourceEntity = ECS::EntityHandle::Invalid();
        uint64 payloadRevision = 0;
        WaterRenderSnapshotItem state;
    };

    /** @brief Terrain value with a stable ECS-derived RenderScene identity. */
    struct EcsFrozenSceneTerrainValue
    {
        uint64 id = 0;
        ECS::EntityHandle sourceEntity = ECS::EntityHandle::Invalid();
        uint64 payloadRevision = 0;
        TerrainRenderSnapshotItem state;
    };

    /** @brief Full owned output of one non-incremental frozen ECS extraction. */
    struct EcsFrozenSceneBridgeOutput
    {
        RenderProxySnapshot renderProxies;
        std::vector<EcsFrozenSceneMaterialBindingValue> materialBindings;
        std::vector<EcsFrozenScenePrimitiveTemporalValue> primitiveTemporalValues;
        std::vector<EcsFrozenSceneLightValue> lightValues;
        std::vector<EcsFrozenSceneCameraValue> cameras;
        std::vector<EcsFrozenSceneSkyboxValue> skyboxes;
        std::vector<EcsFrozenSceneParticleValue> particles;
        std::vector<EcsFrozenSceneWaterValue> water;
        std::vector<EcsFrozenSceneTerrainValue> terrain;
        std::optional<uint64> selectedCameraId;
        ECS::SceneRuntimeId sceneRuntimeId;
        uint64 snapshotRevision = 0;
        uint64 structuralJournalSequence = 0;
        uint64 renderWorldTransformWriteVersion = 0;
    };

    /** @brief Build-level diagnostics. A false Build result never leaves partial output. */
    struct EcsFrozenSceneBridgeResult
    {
        EcsFrozenSceneBridgeCode code = EcsFrozenSceneBridgeCode::Complete;
        uint32 primitiveCount = 0;
        uint32 lightCount = 0;
        uint32 materialBindingCount = 0;
        uint32 cameraCount = 0;
        uint32 skyboxCount = 0;
        uint32 particleCount = 0;
        uint32 waterCount = 0;
        uint32 terrainCount = 0;
    };

    /** @brief Test seam for collision verification; production uses DeriveRenderId. */
    using EcsFrozenSceneBridgeIdDeriver = uint64 (*) (
        const SceneECS::FrozenSceneObjectId& id) noexcept;

    /**
     * @brief Converts one immutable ECS snapshot into common Direct/GPU render source values.
     *
     * This bridge is intentionally stateless and consumes only value-owned
     * FrozenSceneSnapshot data. It does not include or access legacy Scene,
     * World, Actor, Resource, or RHI objects.
     */
    class EcsFrozenSceneBridge final
    {
    public:
        /** @brief Deterministically hash a generation-qualified frozen source identity. */
        [[nodiscard]] static uint64 DeriveRenderId(
            const SceneECS::FrozenSceneObjectId& id) noexcept;

        explicit EcsFrozenSceneBridge(
            EcsFrozenSceneBridgeIdDeriver idDeriver = &DeriveRenderId) noexcept;

        /**
         * @brief Build a full snapshot payload and fail closed on invalid or colliding IDs.
         *
         * `outOutput` is cleared before every attempt. On failure, its render
         * proxy snapshot is marked incomplete and every companion collection is
         * empty, preventing publication of a partial ECS scene.
         */
        [[nodiscard]] bool Build(
            const SceneECS::FrozenSceneSnapshot& snapshot,
            EcsFrozenSceneBridgeOutput& outOutput,
            EcsFrozenSceneBridgeResult* outResult = nullptr) const;

    private:
        EcsFrozenSceneBridgeIdDeriver m_idDeriver = &DeriveRenderId;
    };

    static_assert(static_cast<uint8>(EcsFrozenSceneBridgeCode::Complete) == 0);
} // namespace RVX
