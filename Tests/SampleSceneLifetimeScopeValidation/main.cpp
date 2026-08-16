#include "Samples/SampleSceneLifetimeScope.h"

#include "Scene/ECS/RenderFragments.h"

#include <gtest/gtest.h>

#include <array>
#include <vector>

namespace
{
using namespace RVX;

TEST(SampleSceneLifetimeScopeValidation,
     RetainsExactGenerationUntilCleanupRetirementAndRecyclingComplete)
{
    SceneECS::SceneEcsRuntime runtime;
    SampleSceneLifetimeScope scope(runtime);
    const SceneECS::SceneEntityRef entity = scope.CreateAndAdopt();
    ASSERT_TRUE(entity.IsValid());
    ASSERT_TRUE(runtime.AddFragment<SceneECS::Mesh>(entity.entity, {}));

    SampleSceneLifetimeDiagnostics diagnostics = scope.GetDiagnostics();
    EXPECT_EQ(diagnostics.sceneRuntimeId, runtime.GetSceneRuntimeId());
    EXPECT_EQ(diagnostics.ownedEntityCount, 1u);
    EXPECT_EQ(diagnostics.aliveEntityCount, 1u);

    EXPECT_TRUE(scope.RequestDestroyAll());
    EXPECT_TRUE(scope.RequestDestroyAll());
    scope.Collect();
    diagnostics = scope.GetDiagnostics();
    EXPECT_EQ(diagnostics.pendingDestroyEntityCount, 1u);
    EXPECT_TRUE(scope.HasUnresolvedDestroyWork());

    ASSERT_EQ(runtime.PublishPendingDestroyCleanup(), 1u);
    scope.Collect();
    diagnostics = scope.GetDiagnostics();
    EXPECT_EQ(diagnostics.cleanupRequiredEntityCount, 1u);
    EXPECT_TRUE(scope.HasUnresolvedDestroyWork());

    ASSERT_TRUE(runtime.AcknowledgeCleanup(
        entity.entity,
        SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Render)));
    ASSERT_EQ(runtime.AdvanceRetirements(), 1u);
    scope.Collect();
    diagnostics = scope.GetDiagnostics();
    EXPECT_EQ(diagnostics.recyclableEntityCount, 1u);
    EXPECT_TRUE(scope.HasUnresolvedDestroyWork());

    ASSERT_EQ(runtime.RecycleRecyclableEntities(), 1u);
    scope.Collect();
    diagnostics = scope.GetDiagnostics();
    EXPECT_FALSE(scope.HasUnresolvedDestroyWork());
    EXPECT_EQ(diagnostics.ownedEntityCount, 0u);
    EXPECT_EQ(diagnostics.recycledEntityCount, 1u);
    EXPECT_FALSE(runtime.GetEntityRef(entity.entity).IsValid());
}

TEST(SampleSceneLifetimeScopeValidation,
     AtomicallyCreatesAndAdoptsBatchesAndRejectsForeignStaleOrDuplicateRefs)
{
    SceneECS::SceneEcsRuntime runtime;
    SampleSceneLifetimeScope scope(runtime);
    const std::array<SceneECS::RuntimeEntityDesc, 3> descriptions{};
    std::vector<SceneECS::SceneEntityRef> created;
    ASSERT_TRUE(scope.CreateAndAdoptBatch(descriptions, created));
    ASSERT_EQ(created.size(), descriptions.size());
    for (const SceneECS::SceneEntityRef entity : created)
    {
        EXPECT_EQ(entity.sceneRuntimeId, runtime.GetSceneRuntimeId());
        EXPECT_EQ(runtime.GetEntityRef(entity.entity), entity);
    }

    EXPECT_FALSE(scope.AdoptBatch(created));
    EXPECT_EQ(scope.GetDiagnostics().ownedEntityCount, created.size());

    const ECS::EntityHandle direct = runtime.CreateEntity();
    const SceneECS::SceneEntityRef directRef = runtime.GetEntityRef(direct);
    ASSERT_TRUE(directRef.IsValid());
    EXPECT_TRUE(scope.Adopt(directRef));

    const ECS::EntityHandle candidate = runtime.CreateEntity();
    const SceneECS::SceneEntityRef candidateRef = runtime.GetEntityRef(candidate);
    ASSERT_TRUE(candidateRef.IsValid());
    SceneECS::SceneEcsRuntime foreignRuntime;
    const SceneECS::SceneEntityRef foreign =
        foreignRuntime.GetEntityRef(foreignRuntime.CreateEntity());
    ASSERT_TRUE(foreign.IsValid());
    const std::array mixed{candidateRef, foreign};
    EXPECT_FALSE(scope.AdoptBatch(mixed));
    EXPECT_EQ(scope.GetDiagnostics().ownedEntityCount, descriptions.size() + 1u);
    EXPECT_TRUE(scope.Adopt(candidateRef));

    const ECS::EntityHandle staleHandle = runtime.CreateEntity();
    const SceneECS::SceneEntityRef stale = runtime.GetEntityRef(staleHandle);
    ASSERT_TRUE(stale.IsValid());
    ASSERT_EQ(runtime.RequestDestroyBatch(
                  std::span(&staleHandle, 1u),
                  SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::None)),
              SceneECS::DestroyRequestResult::Accepted);
    ASSERT_EQ(runtime.PublishPendingDestroyCleanup(), 1u);
    ASSERT_EQ(runtime.AdvanceRetirements(), 1u);
    ASSERT_EQ(runtime.RecycleRecyclableEntities(), 1u);
    EXPECT_FALSE(scope.Adopt(stale));
    EXPECT_GE(scope.GetDiagnostics().rejectedEntityCount, 3u);

    EXPECT_TRUE(scope.RequestDestroyAll());
    ASSERT_EQ(runtime.PublishPendingDestroyCleanup(), descriptions.size() + 2u);
    ASSERT_EQ(runtime.AdvanceRetirements(), descriptions.size() + 2u);
    ASSERT_EQ(runtime.RecycleRecyclableEntities(), descriptions.size() + 2u);
    scope.Collect();
    EXPECT_FALSE(scope.HasUnresolvedDestroyWork());
    EXPECT_EQ(scope.GetDiagnostics().recycledEntityCount, descriptions.size() + 2u);
}

TEST(SampleSceneLifetimeScopeValidation,
     PreallocatesOwnershipAndOutputStorageBeforePublishingSpawnBatch)
{
    SceneECS::SceneEcsRuntime runtime;
    SampleSceneLifetimeScope scope(runtime);
    const std::array<SceneECS::RuntimeEntityDesc, 3> descriptions{};
    ASSERT_TRUE(scope.ReserveOwnershipCapacity(descriptions.size()));
    const size_t ownershipCapacityBeforeSpawn =
        scope.GetDiagnostics().ownedEntityStorageCapacity;
    ASSERT_GE(ownershipCapacityBeforeSpawn, descriptions.size());

    std::vector<SceneECS::SceneEntityRef> created;
    created.reserve(descriptions.size());
    const size_t outputCapacityBeforeSpawn = created.capacity();

    ASSERT_TRUE(scope.CreateAndAdoptBatch(descriptions, created));
    EXPECT_EQ(scope.GetDiagnostics().ownedEntityStorageCapacity, ownershipCapacityBeforeSpawn);
    EXPECT_EQ(created.capacity(), outputCapacityBeforeSpawn);
    EXPECT_EQ(scope.GetDiagnostics().ownedEntityCount, descriptions.size());
    EXPECT_EQ(created.size(), descriptions.size());
}

TEST(SampleSceneLifetimeScopeValidation,
     AtomicallyCreatesAuthoredFragmentEntitiesWithoutPartialPublication)
{
    SceneECS::SceneEcsRuntime runtime;
    SampleSceneLifetimeScope scope(runtime);

    SceneECS::Mesh mesh;
    mesh.meshAssetId = AssetId{41};
    mesh.submeshCount = 2;
    SceneECS::MaterialSlots materials;
    materials.count = 2;
    materials.values[0].materialAssetId = AssetId{51};
    materials.values[1].materialAssetId = AssetId{52};
    SceneECS::Visibility visibility;
    visibility.layerMask = 7;

    const SceneECS::SceneEntityRef entity =
        scope.CreateAndAdoptWithFragments(
            SceneECS::RuntimeEntityDesc{}, mesh, materials, visibility);
    ASSERT_TRUE(entity.IsValid());
    const ECS::Registry& registry = runtime.GetRegistry();
    const SceneECS::Mesh* storedMesh =
        registry.TryGet<SceneECS::Mesh>(entity.entity);
    const SceneECS::MaterialSlots* storedMaterials =
        registry.TryGet<SceneECS::MaterialSlots>(entity.entity);
    const SceneECS::Visibility* storedVisibility =
        registry.TryGet<SceneECS::Visibility>(entity.entity);
    ASSERT_NE(storedMesh, nullptr);
    ASSERT_NE(storedMaterials, nullptr);
    ASSERT_NE(storedVisibility, nullptr);
    EXPECT_EQ(storedMesh->meshAssetId, mesh.meshAssetId);
    EXPECT_EQ(storedMaterials->values[1].materialAssetId,
              materials.values[1].materialAssetId);
    EXPECT_EQ(storedVisibility->layerMask, visibility.layerMask);

    const uint32 entitiesBeforeRejectedSpawn =
        runtime.GetDiagnosticsSnapshot().entityCount;
    EXPECT_FALSE(scope
                     .CreateAndAdoptWithFragments(
                         SceneECS::RuntimeEntityDesc{},
                         SceneECS::ParentRelation{})
                     .IsValid());
    EXPECT_EQ(runtime.GetDiagnosticsSnapshot().entityCount,
              entitiesBeforeRejectedSpawn);

    ASSERT_TRUE(scope.RequestDestroyAll());
    ASSERT_EQ(runtime.PublishPendingDestroyCleanup(), 1u);
    ASSERT_TRUE(runtime.AcknowledgeCleanup(
        entity.entity,
        SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Render)));
    ASSERT_EQ(runtime.AdvanceRetirements(), 1u);
    ASSERT_EQ(runtime.RecycleRecyclableEntities(), 1u);
    scope.Collect();
    EXPECT_FALSE(scope.HasUnresolvedDestroyWork());
}
} // namespace
