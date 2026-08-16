#include "Scene/ECS/FrozenSceneSnapshot.h"

#include <gtest/gtest.h>

namespace
{
using namespace RVX;
using namespace RVX::SceneECS;

void AddRenderTransform(ECS::Registry& registry,
                        ECS::EntityHandle entity,
                        uint64 sourceRevision = 1,
                        float translationX = 0.0f)
{
    RenderWorldTransform transform;
    transform.matrix[3].x = translationX;
    transform.sourceRevision = sourceRevision;
    EXPECT_TRUE(registry.Add<RenderWorldTransform>(entity, transform));

    PreviousSimulationWorldTransform previous;
    previous.matrix[3].x = translationX - 1.0f;
    previous.sourceRevision = sourceRevision - 1u;
    EXPECT_TRUE(registry.Add<PreviousSimulationWorldTransform>(entity, previous));
}

void AddLifecycle(ECS::Registry& registry,
                  ECS::EntityHandle entity,
                  EntityLifecyclePhase phase = EntityLifecyclePhase::Alive)
{
    EntityLifecycleState lifecycle;
    lifecycle.phase = phase;
    EXPECT_TRUE(registry.Add<EntityLifecycleState>(entity, lifecycle));
}

void AddMeshSource(ECS::Registry& registry,
                   ECS::EntityHandle entity,
                   uint64 meshAssetId,
                   uint64 materialAssetId,
                   uint64 sourceRevision = 1,
                   float translationX = 0.0f)
{
    Mesh mesh;
    mesh.meshAssetId = {.value = meshAssetId};
    mesh.submeshCount = 1;
    EXPECT_TRUE(registry.Add<Mesh>(entity, mesh));

    MaterialSlots slots;
    slots.count = 1;
    slots.values[0].materialAssetId = {.value = materialAssetId};
    slots.values[0].materialMode = RenderMaterialMode::Masked;
    EXPECT_TRUE(registry.Add<MaterialSlots>(entity, slots));

    Visibility visibility;
    visibility.layerMask = 0x7u;
    EXPECT_TRUE(registry.Add<Visibility>(entity, visibility));

    Bounds bounds;
    bounds.center = {2.0f, 3.0f, 4.0f};
    bounds.extents = {5.0f, 6.0f, 7.0f};
    EXPECT_TRUE(registry.Add<Bounds>(entity, bounds));
    AddRenderTransform(registry, entity, sourceRevision, translationX);
    AddLifecycle(registry, entity);
}
} // namespace

TEST(EcsSceneSnapshotValidation, SortsAllSnapshotCategoriesByGenerationQualifiedHandle)
{
    ECS::Registry registry;
    const ECS::EntityHandle first = registry.CreateEntity();
    const ECS::EntityHandle second = registry.CreateEntity();
    const ECS::EntityHandle third = registry.CreateEntity();

    AddMeshSource(registry, third, 300, 301);
    AddMeshSource(registry, first, 100, 101);
    AddMeshSource(registry, second, 200, 201);

    Light light;
    EXPECT_TRUE(registry.Add<Light>(third, light));
    EXPECT_TRUE(registry.Add<Light>(first, light));
    EXPECT_TRUE(registry.Add<Light>(second, light));

    Skybox skybox;
    EXPECT_TRUE(registry.Add<Skybox>(second, skybox));
    EXPECT_TRUE(registry.Add<Skybox>(first, skybox));
    EXPECT_TRUE(registry.Add<Skybox>(third, skybox));

    SceneSnapshotBuilder builder;
    const FrozenSceneSnapshot snapshot = builder.Build(registry);

    ASSERT_EQ(snapshot.meshes.size(), 3u);
    ASSERT_EQ(snapshot.lights.size(), 3u);
    ASSERT_EQ(snapshot.skyboxes.size(), 3u);
    EXPECT_EQ(snapshot.meshes[0].id.entity, first);
    EXPECT_EQ(snapshot.meshes[1].id.entity, second);
    EXPECT_EQ(snapshot.meshes[2].id.entity, third);
    EXPECT_EQ(snapshot.lights[0].id.entity, first);
    EXPECT_EQ(snapshot.lights[1].id.entity, second);
    EXPECT_EQ(snapshot.lights[2].id.entity, third);
    EXPECT_EQ(snapshot.skyboxes[0].id.entity, first);
    EXPECT_EQ(snapshot.skyboxes[1].id.entity, second);
    EXPECT_EQ(snapshot.skyboxes[2].id.entity, third);
    EXPECT_EQ(snapshot.skyboxes[0].skyboxWriteVersion,
              registry.GetFragmentWriteVersion<Skybox>(first));
    EXPECT_EQ(snapshot.skyboxes[1].skyboxWriteVersion,
              registry.GetFragmentWriteVersion<Skybox>(second));
    EXPECT_EQ(snapshot.skyboxes[2].skyboxWriteVersion,
              registry.GetFragmentWriteVersion<Skybox>(third));
    EXPECT_EQ(snapshot.meshes[0].materialSlots[0].id.type,
              FrozenSceneObjectType::MaterialSlot);
    EXPECT_EQ(snapshot.meshes[0].materialSlots[0].id.slot, 0u);
    EXPECT_EQ(snapshot.revision, 1u);
    EXPECT_EQ(builder.Build(registry).revision, 2u);
}

TEST(EcsSceneSnapshotValidation,
     CarriesTheEntityLocalSkyboxWriteVersionAcrossFrozenSnapshots)
{
    ECS::Registry registry;
    const ECS::EntityHandle entity = registry.CreateEntity();
    AddLifecycle(registry, entity);
    EXPECT_TRUE(registry.Add<Skybox>(entity, {}));

    SceneSnapshotBuilder builder;
    const FrozenSceneSnapshot first = builder.Build(registry);
    ASSERT_EQ(first.skyboxes.size(), 1U);
    const uint64 firstWriteVersion = registry.GetFragmentWriteVersion<Skybox>(entity);
    EXPECT_EQ(first.skyboxes.front().sourceEntity, entity);
    EXPECT_EQ(first.skyboxes.front().skyboxWriteVersion, firstWriteVersion);

    ASSERT_TRUE(registry.Write<Skybox>(entity,
                                       [](Skybox& skybox)
                                       { skybox.exposure = 2.0f; }));
    const FrozenSceneSnapshot second = builder.Build(registry);
    ASSERT_EQ(second.skyboxes.size(), 1U);
    EXPECT_GT(second.skyboxes.front().skyboxWriteVersion, firstWriteVersion);
    EXPECT_EQ(second.skyboxes.front().skyboxWriteVersion,
              registry.GetFragmentWriteVersion<Skybox>(entity));
}

TEST(EcsSceneSnapshotValidation, ExcludesRegistryDisabledAndNonAliveLifecycleEntities)
{
    ECS::Registry registry;
    const ECS::EntityHandle alive = registry.CreateEntity();
    const ECS::EntityHandle disabled = registry.CreateEntity();
    const ECS::EntityHandle pending = registry.CreateEntity();
    AddMeshSource(registry, alive, 1, 11);
    AddMeshSource(registry, disabled, 2, 22);
    AddMeshSource(registry, pending, 3, 33);
    ASSERT_TRUE(registry.Disable(disabled));
    ASSERT_TRUE(registry.Write<EntityLifecycleState>(
        pending,
        [](EntityLifecycleState& lifecycle)
        {
            lifecycle.phase = EntityLifecyclePhase::PendingDestroy;
        }));

    SceneSnapshotBuilder builder;
    const FrozenSceneSnapshot snapshot = builder.Build(registry);
    ASSERT_EQ(snapshot.meshes.size(), 1u);
    EXPECT_EQ(snapshot.meshes.front().id.entity, alive);
}

TEST(EcsSceneSnapshotValidation, OwnsFullValuesAfterRegistryMutation)
{
    ECS::Registry registry;
    const ECS::EntityHandle entity = registry.CreateEntity();
    AddMeshSource(registry, entity, 101, 202, 77, 12.0f);

    SceneSnapshotBuilder builder;
    const FrozenSceneSnapshot snapshot = builder.Build(registry);
    ASSERT_EQ(snapshot.meshes.size(), 1u);
    const FrozenSceneMesh& frozen = snapshot.meshes.front();

    ASSERT_TRUE(registry.Write<Mesh>(entity, [](Mesh& mesh) { mesh.meshAssetId.value = 999; }));
    ASSERT_TRUE(registry.Write<MaterialSlots>(
        entity,
        [](MaterialSlots& slots)
        {
            slots.count = 0;
            slots.values[0].materialAssetId.value = 888;
        }));
    ASSERT_TRUE(registry.Write<Visibility>(
        entity,
        [](Visibility& visibility) { visibility.visible = false; }));
    ASSERT_TRUE(registry.Write<RenderWorldTransform>(
        entity,
        [](RenderWorldTransform& transform)
        {
            transform.matrix[3].x = 99.0f;
            transform.sourceRevision = 88;
        }));

    EXPECT_EQ(frozen.source.meshAssetId.value, 101u);
    EXPECT_EQ(frozen.materialSlotsSource.count, 1u);
    ASSERT_EQ(frozen.materialSlots.size(), 1u);
    EXPECT_EQ(frozen.materialSlots[0].source.materialAssetId.value, 202u);
    EXPECT_TRUE(frozen.visibility.visible);
    EXPECT_FLOAT_EQ(frozen.worldTransform[3].x, 12.0f);
    EXPECT_EQ(frozen.transformSourceRevision, 77u);
}

TEST(EcsSceneSnapshotValidation, IdentitiesCarrySceneAndGenerationAndTransformsCarrySourceRevision)
{
    ECS::Registry registry;
    const ECS::EntityHandle retired = registry.CreateEntity();
    AddMeshSource(registry, retired, 10, 20, 9, 4.0f);
    ASSERT_TRUE(registry.DestroyEntity(retired));

    const ECS::EntityHandle recycled = registry.CreateEntity();
    ASSERT_EQ(recycled.GetIndex(), retired.GetIndex());
    ASSERT_NE(recycled.GetGeneration(), retired.GetGeneration());
    AddMeshSource(registry, recycled, 30, 40, 123, 8.0f);

    SceneSnapshotBuilder builder;
    const FrozenSceneSnapshot snapshot = builder.Build(registry);
    ASSERT_EQ(snapshot.meshes.size(), 1u);
    const FrozenSceneMesh& frozen = snapshot.meshes.front();
    EXPECT_EQ(frozen.id.sceneRuntimeId, registry.GetSceneRuntimeId());
    EXPECT_EQ(frozen.id.entity, recycled);
    EXPECT_NE(frozen.id.entity, retired);
    EXPECT_EQ(frozen.id.type, FrozenSceneObjectType::Mesh);
    EXPECT_EQ(frozen.id.slot, 0u);
    EXPECT_EQ(frozen.sourceEntity, recycled);
    EXPECT_EQ(frozen.visibilityWriteVersion,
              registry.GetFragmentWriteVersion<Visibility>(recycled));
    EXPECT_EQ(frozen.transformSourceRevision, 123u);
    EXPECT_FLOAT_EQ(frozen.worldTransform[3].x, 8.0f);
    EXPECT_EQ(frozen.previousSimulationSourceRevision, 122u);
}

TEST(EcsSceneSnapshotValidation, SelectsHighestPriorityCameraThenLowestHandle)
{
    ECS::Registry registry;
    const ECS::EntityHandle first = registry.CreateEntity();
    const ECS::EntityHandle second = registry.CreateEntity();
    const ECS::EntityHandle third = registry.CreateEntity();

    for (const ECS::EntityHandle entity : {first, second, third})
    {
        AddRenderTransform(registry, entity);
        AddLifecycle(registry, entity);
    }

    Camera firstCamera;
    firstCamera.priority = 20;
    Camera secondCamera;
    secondCamera.priority = 20;
    Camera disabledCamera;
    disabledCamera.priority = 100;
    disabledCamera.enabled = false;
    EXPECT_TRUE(registry.Add<Camera>(first, firstCamera));
    EXPECT_TRUE(registry.Add<Camera>(second, secondCamera));
    EXPECT_TRUE(registry.Add<Camera>(third, disabledCamera));

    SceneSnapshotBuilder builder;
    const FrozenSceneSnapshot snapshot = builder.Build(registry);
    ASSERT_EQ(snapshot.cameras.size(), 2u);
    ASSERT_TRUE(snapshot.selectedCamera.has_value());
    EXPECT_EQ(snapshot.selectedCamera->entity, first);
    EXPECT_EQ(snapshot.selectedCamera->type, FrozenSceneObjectType::Camera);
}
