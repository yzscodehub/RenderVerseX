#pragma once

/**
 * @file FrozenSceneSnapshot.h
 * @brief Immutable-by-construction value snapshot of one Scene ECS render boundary.
 */

#include "ECS/Registry.h"
#include "RenderContracts/ParticleRenderSnapshot.h"
#include "RenderContracts/TerrainRenderSnapshot.h"
#include "RenderContracts/WaterRenderSnapshot.h"
#include "Scene/ECS/AnimationFragments.h"
#include "Scene/ECS/Fragments.h"
#include "Scene/ECS/RenderFragments.h"

#include <optional>
#include <vector>

namespace RVX::SceneECS
{
    /** @brief Explicit source category used to derive a stable frozen-object identity. */
    enum class FrozenSceneObjectType : uint8
    {
        Invalid = 0,
        Camera,
        Mesh,
        MaterialSlot,
        Light,
        Skybox,
        Particle,
        Water,
        Terrain,
        /** @brief Palette provider identity derived from its mesh source identity. */
        SkinningPalette,
    };

    /**
     * @brief Generation-qualified identity for one source value in a frozen Scene.
     *
     * A material binding has its own identity through MaterialSlot plus its
     * explicit slot index. IDs are never derived from fragment addresses.
     */
    struct FrozenSceneObjectId
    {
        ECS::SceneRuntimeId sceneRuntimeId;
        ECS::EntityHandle entity = ECS::EntityHandle::Invalid();
        FrozenSceneObjectType type = FrozenSceneObjectType::Invalid;
        uint32 slot = 0;

        [[nodiscard]] bool IsValid() const
        {
            return sceneRuntimeId.IsValid() && entity.IsValid() &&
                   type != FrozenSceneObjectType::Invalid;
        }

        bool operator==(const FrozenSceneObjectId&) const = default;
    };

    /** @brief Construct an identity from the only permitted source identity inputs. */
    [[nodiscard]] FrozenSceneObjectId MakeFrozenSceneObjectId(
        ECS::SceneRuntimeId sceneRuntimeId,
        ECS::EntityHandle entity,
        FrozenSceneObjectType type,
        uint32 slot = 0);

    /** @brief Snapshot value for one camera and its render-resolved transform source. */
    struct FrozenSceneCamera
    {
        FrozenSceneObjectId id;
        Camera source;
        Mat4 worldTransform{1.0f};
        uint64 transformSourceRevision = 0;
    };

    /** @brief Snapshot value for one material binding within a frozen mesh source. */
    struct FrozenSceneMaterialSlot
    {
        FrozenSceneObjectId id;
        MaterialSlot source;
    };

    /** @brief Frozen value palette owned by its mesh's skinning snapshot entry. */
    struct FrozenSceneSkinningPalette
    {
        FrozenSceneObjectId id;
        ECS::EntityHandle poseEntity = ECS::EntityHandle::Invalid();
        uint64 sourceModelAssetValue = 0;
        int32 sourceSkinIndex = -1;
        uint64 poseSequence = 0;
        uint64 paletteRevision = 0;
        uint32 paletteBoneCount = 0;
        std::vector<Mat4> matrices;
    };

    /** @brief Snapshot value for one visible mesh source and every copied slot. */
    struct FrozenSceneMesh
    {
        FrozenSceneObjectId id;
        /**
         * @brief Explicit generation-qualified source retained for downstream
         * presentation proof. It must match `id.entity` when supplied.
         */
        ECS::EntityHandle sourceEntity = ECS::EntityHandle::Invalid();
        Mesh source;
        MaterialSlots materialSlotsSource;
        Visibility visibility;
        /** @brief Entity-local Visibility write version captured with `visibility`. */
        uint64 visibilityWriteVersion = 0;
        Bounds bounds;
        Mat4 worldTransform{1.0f};
        Mat4 previousSimulationWorldTransform{1.0f};
        uint64 transformSourceRevision = 0;
        uint64 previousSimulationSourceRevision = 0;
        std::vector<FrozenSceneMaterialSlot> materialSlots;
        /** @brief Exact source relation expected by the stored value palette. */
        std::optional<SkinnedMeshBinding> skinningBinding;
        /** @brief Complete copied palette; absent only when this mesh is not skinned. */
        std::optional<FrozenSceneSkinningPalette> skinningPalette;
    };

    /** @brief Snapshot value for one light source and its render-resolved transform source. */
    struct FrozenSceneLight
    {
        FrozenSceneObjectId id;
        ECS::EntityHandle sourceEntity = ECS::EntityHandle::Invalid();
        Light source;
        Visibility visibility;
        Mat4 worldTransform{1.0f};
        uint64 transformSourceRevision = 0;
    };

    /** @brief Snapshot value for one sky source. */
    struct FrozenSceneSkybox
    {
        FrozenSceneObjectId id;
        ECS::EntityHandle sourceEntity = ECS::EntityHandle::Invalid();
        Skybox source;
        /** @brief Entity-local Skybox fragment write version captured with source. */
        uint64 skyboxWriteVersion = 0;
    };

    /** @brief Immutable value contribution from one particle feature source. */
    struct FrozenSceneParticle
    {
        FrozenSceneObjectId id;
        ECS::EntityHandle sourceEntity = ECS::EntityHandle::Invalid();
        ParticleRenderSnapshotItem state;
        uint64 payloadRevision = 0;
    };

    /** @brief Immutable value contribution from one water feature source. */
    struct FrozenSceneWater
    {
        FrozenSceneObjectId id;
        ECS::EntityHandle sourceEntity = ECS::EntityHandle::Invalid();
        WaterRenderSnapshotItem state;
        uint64 payloadRevision = 0;
    };

    /** @brief Immutable value contribution from one terrain feature source. */
    struct FrozenSceneTerrain
    {
        FrozenSceneObjectId id;
        ECS::EntityHandle sourceEntity = ECS::EntityHandle::Invalid();
        TerrainRenderSnapshotItem state;
        uint64 payloadRevision = 0;
    };

    /**
     * @brief Complete immutable extraction input for one ECS scene boundary.
     *
     * The snapshot owns every value and vector it exposes. It intentionally has
     * no Registry, fragment, Resource, RHI, or Actor pointer/reference member.
     */
    struct FrozenSceneSnapshot
    {
        ECS::SceneRuntimeId sceneRuntimeId;
        uint64 revision = 0;
        uint64 structuralJournalSequence = 0;
        uint64 renderWorldTransformWriteVersion = 0;
        std::vector<FrozenSceneCamera> cameras;
        std::vector<FrozenSceneMesh> meshes;
        std::vector<FrozenSceneLight> lights;
        std::vector<FrozenSceneSkybox> skyboxes;
        std::vector<FrozenSceneParticle> particles;
        std::vector<FrozenSceneWater> water;
        std::vector<FrozenSceneTerrain> terrain;
        std::optional<FrozenSceneObjectId> selectedCamera;
    };

    /**
     * @brief Owner-thread service that copies a Registry at the render extraction boundary.
     *
     * Call Build only after the owning Scene has completed its selected command
     * barrier and transform synchronization. Registry mutation is not allowed
     * concurrently with Build; the resulting FrozenSceneSnapshot itself is
     * independent of any later Registry changes.
     */
    class SceneSnapshotBuilder
    {
    public:
        SceneSnapshotBuilder() = default;

        [[nodiscard]] FrozenSceneSnapshot Build(ECS::Registry& registry);
        [[nodiscard]] uint64 GetLastRevision() const { return m_lastRevision; }

    private:
        uint64 m_lastRevision = 0;
    };
} // namespace RVX::SceneECS
