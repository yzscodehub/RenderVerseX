#include "Scene/ECS/FrozenSceneSnapshot.h"

#include "ECS/Query.h"

#include <algorithm>
#include <utility>

namespace RVX::SceneECS
{
namespace
{
    [[nodiscard]] bool IsAliveForSnapshot(const EntityLifecycleState& lifecycle)
    {
        return lifecycle.phase == EntityLifecyclePhase::Alive;
    }

    template<typename T>
    void SortByEntity(std::vector<T>& values)
    {
        std::sort(values.begin(), values.end(), [](const T& lhs, const T& rhs)
        {
            return lhs.id.entity < rhs.id.entity;
        });
    }

    [[nodiscard]] uint32 ClampedMaterialSlotCount(const MaterialSlots& slots)
    {
        return std::min(slots.count, MaterialSlots::MaxSlotCount);
    }
} // namespace

FrozenSceneObjectId MakeFrozenSceneObjectId(
    ECS::SceneRuntimeId sceneRuntimeId,
    ECS::EntityHandle entity,
    FrozenSceneObjectType type,
    uint32 slot)
{
    return {
        .sceneRuntimeId = sceneRuntimeId,
        .entity = entity,
        .type = type,
        .slot = slot,
    };
}

FrozenSceneSnapshot SceneSnapshotBuilder::Build(ECS::Registry& registry)
{
    FrozenSceneSnapshot snapshot;
    snapshot.sceneRuntimeId = registry.GetSceneRuntimeId();
    snapshot.revision = ++m_lastRevision;
    snapshot.structuralJournalSequence =
        registry.GetStructuralJournal().GetNextSequence() - 1u;
    snapshot.renderWorldTransformWriteVersion =
        registry.GetFragmentWriteVersion<RenderWorldTransform>();

    registry.Query<ECS::Read<Camera>,
                   ECS::Read<RenderWorldTransform>,
                   ECS::Read<EntityLifecycleState>>()
        .Each([&snapshot](ECS::EntityHandle entity,
                          const Camera& camera,
                          const RenderWorldTransform& transform,
                          const EntityLifecycleState& lifecycle)
        {
            if (!camera.enabled || !IsAliveForSnapshot(lifecycle))
            {
                return;
            }

            snapshot.cameras.push_back({
                .id = MakeFrozenSceneObjectId(
                    snapshot.sceneRuntimeId, entity, FrozenSceneObjectType::Camera),
                .source = camera,
                .worldTransform = transform.matrix,
                .transformSourceRevision = transform.sourceRevision,
            });
        });

    registry.Query<ECS::Read<Mesh>,
                   ECS::Read<MaterialSlots>,
                   ECS::Read<Visibility>,
                   ECS::Read<Bounds>,
                   ECS::Read<RenderWorldTransform>,
                   ECS::Read<PreviousSimulationWorldTransform>,
                   ECS::Read<EntityLifecycleState>>()
        .Each([&snapshot, &registry](ECS::EntityHandle entity,
                          const Mesh& mesh,
                          const MaterialSlots& materialSlots,
                          const Visibility& visibility,
                          const Bounds& bounds,
                          const RenderWorldTransform& transform,
                          const PreviousSimulationWorldTransform& previousTransform,
                          const EntityLifecycleState& lifecycle)
        {
            if (!IsAliveForSnapshot(lifecycle))
            {
                return;
            }

            FrozenSceneMesh frozenMesh;
            frozenMesh.id = MakeFrozenSceneObjectId(
                snapshot.sceneRuntimeId, entity, FrozenSceneObjectType::Mesh);
            frozenMesh.sourceEntity = entity;
            frozenMesh.source = mesh;
            frozenMesh.materialSlotsSource = materialSlots;
            frozenMesh.visibility = visibility;
            frozenMesh.visibilityWriteVersion =
                registry.GetFragmentWriteVersion<Visibility>(entity);
            frozenMesh.bounds = bounds;
            frozenMesh.worldTransform = transform.matrix;
            frozenMesh.previousSimulationWorldTransform = previousTransform.matrix;
            frozenMesh.transformSourceRevision = transform.sourceRevision;
            frozenMesh.previousSimulationSourceRevision = previousTransform.sourceRevision;
            const uint32 materialCount = ClampedMaterialSlotCount(materialSlots);
            frozenMesh.materialSlots.reserve(materialCount);
            for (uint32 slot = 0; slot < materialCount; ++slot)
            {
                frozenMesh.materialSlots.push_back({
                    .id = MakeFrozenSceneObjectId(
                        snapshot.sceneRuntimeId,
                        entity,
                        FrozenSceneObjectType::MaterialSlot,
                        slot),
                    .source = materialSlots.values[slot],
                });
            }
            if (registry.Has<SkinnedMeshBinding>(entity) &&
                registry.IsFragmentEnabled<SkinnedMeshBinding>(entity))
            {
                const SkinnedMeshBinding* binding =
                    registry.TryGet<SkinnedMeshBinding>(entity);
                if (binding != nullptr)
                {
                    frozenMesh.skinningBinding = *binding;
                }
            }
            snapshot.meshes.push_back(std::move(frozenMesh));
        });

    registry.Query<ECS::Read<Light>,
                   ECS::Read<Visibility>,
                   ECS::Read<RenderWorldTransform>,
                   ECS::Read<EntityLifecycleState>>()
        .Each([&snapshot](ECS::EntityHandle entity,
                          const Light& light,
                          const Visibility& visibility,
                          const RenderWorldTransform& transform,
                          const EntityLifecycleState& lifecycle)
        {
            if (!IsAliveForSnapshot(lifecycle))
            {
                return;
            }

            snapshot.lights.push_back({
                .id = MakeFrozenSceneObjectId(
                    snapshot.sceneRuntimeId, entity, FrozenSceneObjectType::Light),
                .sourceEntity = entity,
                .source = light,
                .visibility = visibility,
                .worldTransform = transform.matrix,
                .transformSourceRevision = transform.sourceRevision,
            });
        });

    registry.Query<ECS::Read<Skybox>, ECS::Read<EntityLifecycleState>>()
        .Each([&snapshot, &registry](ECS::EntityHandle entity,
                                     const Skybox& skybox,
                                     const EntityLifecycleState& lifecycle)
        {
            if (!IsAliveForSnapshot(lifecycle))
            {
                return;
            }

            snapshot.skyboxes.push_back({
                .id = MakeFrozenSceneObjectId(
                    snapshot.sceneRuntimeId, entity, FrozenSceneObjectType::Skybox),
                .sourceEntity = entity,
                .source = skybox,
                .skyboxWriteVersion =
                    registry.GetFragmentWriteVersion<Skybox>(entity),
            });
        });

    SortByEntity(snapshot.cameras);
    SortByEntity(snapshot.meshes);
    SortByEntity(snapshot.lights);
    SortByEntity(snapshot.skyboxes);

    // Camera selection is deterministic: highest priority wins; equal priority
    // resolves to the lowest generation-qualified EntityHandle.
    if (!snapshot.cameras.empty())
    {
        const auto selected = std::min_element(
            snapshot.cameras.begin(),
            snapshot.cameras.end(),
            [](const FrozenSceneCamera& lhs, const FrozenSceneCamera& rhs)
            {
                return lhs.source.priority != rhs.source.priority ?
                           lhs.source.priority > rhs.source.priority :
                           lhs.id.entity < rhs.id.entity;
            });
        snapshot.selectedCamera = selected->id;
    }

    return snapshot;
}
} // namespace RVX::SceneECS
