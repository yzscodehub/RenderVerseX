#include "Scene/ECS/SceneEcsRuntime.h"

#include <gtest/gtest.h>

namespace
{
    struct SpawnProbe
    {
        RVX::uint32 value = 0;
    };

    static_assert(RVX::ECS::Fragment<SpawnProbe>);

    RVX::SceneECS::RuntimeEntityDesc MakeEntityDesc(float x, bool active = true)
    {
        RVX::SceneECS::RuntimeEntityDesc desc;
        desc.localTransform.translation = {x, 0.0f, 0.0f};
        desc.bounds.extents = {1.0f, 2.0f, 3.0f};
        desc.active.value = active;
        desc.layer.value = 7;
        return desc;
    }

    RVX::uint64 NextStructuralSequence(const RVX::SceneECS::SceneEcsRuntime& runtime)
    {
        return runtime.GetRegistry().GetStructuralJournal().GetNextSequence();
    }
} // namespace

TEST(EcsSceneSpawnTransactionValidation, CommitsBaselineFragmentsLocalAndExternalParents)
{
    RVX::SceneECS::SceneEcsRuntime runtime;
    const RVX::ECS::EntityHandle existingParent = runtime.CreateEntity(MakeEntityDesc(40.0f));
    ASSERT_TRUE(existingParent.IsValid());
    const RVX::SceneECS::SceneEntityRef existingParentRef = runtime.GetEntityRef(existingParent);
    ASSERT_TRUE(existingParentRef.IsValid());

    auto transaction = runtime.BeginSpawnTransaction();
    const auto root = transaction.Create(MakeEntityDesc(10.0f));
    const auto child = transaction.Create(MakeEntityDesc(2.0f, false));
    const auto externallyParented = transaction.Create(MakeEntityDesc(3.0f));
    ASSERT_TRUE(root.IsValid());
    ASSERT_TRUE(child.IsValid());
    ASSERT_TRUE(externallyParented.IsValid());
    ASSERT_TRUE(transaction.Add<SpawnProbe>(root, {.value = 11}));
    ASSERT_TRUE(transaction.Add<SpawnProbe>(child, {.value = 22}));
    ASSERT_TRUE(transaction.SetParent(child, root));
    ASSERT_TRUE(transaction.SetParent(externallyParented, existingParentRef));

    const RVX::SceneECS::SceneSpawnCommitResult result = transaction.Commit();
    ASSERT_TRUE(result.IsApplied());
    EXPECT_EQ(result.GetError(), RVX::ECS::CommandError::None);

    const RVX::ECS::EntityHandle rootEntity = result.GetEntity(root);
    const RVX::ECS::EntityHandle childEntity = result.GetEntity(child);
    const RVX::ECS::EntityHandle externallyParentedEntity = result.GetEntity(externallyParented);
    ASSERT_TRUE(rootEntity.IsValid());
    ASSERT_TRUE(childEntity.IsValid());
    ASSERT_TRUE(externallyParentedEntity.IsValid());

    const RVX::SceneECS::SceneEntityRef rootRef = result.GetEntityRef(root);
    ASSERT_TRUE(rootRef.IsValid());
    EXPECT_EQ(rootRef.sceneRuntimeId, runtime.GetSceneRuntimeId());
    EXPECT_EQ(rootRef.entity, rootEntity);
    EXPECT_EQ(runtime.GetEntityRef(rootEntity), rootRef);

    const auto& registry = runtime.GetRegistry();
    ASSERT_NE(registry.TryGet<RVX::SceneECS::LocalTransform>(rootEntity), nullptr);
    ASSERT_NE(registry.TryGet<RVX::SceneECS::SimulationWorldTransform>(rootEntity), nullptr);
    ASSERT_NE(registry.TryGet<RVX::SceneECS::PreviousSimulationWorldTransform>(rootEntity), nullptr);
    ASSERT_NE(registry.TryGet<RVX::SceneECS::RenderWorldTransform>(rootEntity), nullptr);
    ASSERT_NE(registry.TryGet<RVX::SceneECS::Bounds>(rootEntity), nullptr);
    ASSERT_NE(registry.TryGet<RVX::SceneECS::Active>(rootEntity), nullptr);
    ASSERT_NE(registry.TryGet<RVX::SceneECS::Layer>(rootEntity), nullptr);
    ASSERT_NE(registry.TryGet<RVX::SceneECS::EntityLifecycleState>(rootEntity), nullptr);
    ASSERT_NE(registry.TryGet<SpawnProbe>(rootEntity), nullptr);
    EXPECT_EQ(registry.TryGet<SpawnProbe>(rootEntity)->value, 11u);
    ASSERT_NE(registry.TryGet<SpawnProbe>(childEntity), nullptr);
    EXPECT_EQ(registry.TryGet<SpawnProbe>(childEntity)->value, 22u);
    EXPECT_FALSE(registry.IsEnabled(childEntity));
    ASSERT_NE(registry.TryGet<RVX::SceneECS::Active>(childEntity), nullptr);
    EXPECT_FALSE(registry.TryGet<RVX::SceneECS::Active>(childEntity)->value);
    EXPECT_EQ(runtime.GetTransformHierarchy().GetParent(childEntity), rootEntity);
    EXPECT_EQ(runtime.GetTransformHierarchy().GetParent(externallyParentedEntity), existingParent);
}

TEST(EcsSceneSpawnTransactionValidation, RejectsInvalidParentAndLocalCycleWithoutPublication)
{
    RVX::SceneECS::SceneEcsRuntime runtime;
    const RVX::uint32 entityCountBefore = runtime.GetRegistry().GetEntityCount();
    const RVX::uint64 journalBefore = NextStructuralSequence(runtime);

    auto invalidParentTransaction = runtime.BeginSpawnTransaction();
    const auto invalidParentChild = invalidParentTransaction.Create();
    ASSERT_TRUE(invalidParentChild.IsValid());
    ASSERT_TRUE(invalidParentTransaction.SetParent(
        invalidParentChild, RVX::ECS::EntityHandle::Create(999u, 0u)));
    const RVX::SceneECS::SceneSpawnCommitResult invalidParentResult =
        invalidParentTransaction.Commit();
    EXPECT_TRUE(invalidParentResult.IsRejected());
    EXPECT_FALSE(invalidParentResult.GetEntity(invalidParentChild).IsValid());
    EXPECT_FALSE(invalidParentResult.GetEntityRef(invalidParentChild).IsValid());
    EXPECT_EQ(runtime.GetRegistry().GetEntityCount(), entityCountBefore);
    EXPECT_EQ(NextStructuralSequence(runtime), journalBefore);

    auto cycleTransaction = runtime.BeginSpawnTransaction();
    const auto first = cycleTransaction.Create();
    const auto second = cycleTransaction.Create();
    ASSERT_TRUE(cycleTransaction.SetParent(first, second));
    ASSERT_TRUE(cycleTransaction.SetParent(second, first));
    const RVX::SceneECS::SceneSpawnCommitResult cycleResult = cycleTransaction.Commit();
    EXPECT_TRUE(cycleResult.IsRejected());
    EXPECT_FALSE(cycleResult.GetEntity(first).IsValid());
    EXPECT_FALSE(cycleResult.GetEntity(second).IsValid());
    EXPECT_EQ(runtime.GetRegistry().GetEntityCount(), entityCountBefore);
    EXPECT_EQ(NextStructuralSequence(runtime), journalBefore);
}

TEST(EcsSceneSpawnTransactionValidation, DuplicateAndAuthorityFragmentsRejectWithoutJournalPrefix)
{
    RVX::SceneECS::SceneEcsRuntime runtime;
    const RVX::uint32 entityCountBefore = runtime.GetRegistry().GetEntityCount();
    const RVX::uint64 journalBefore = NextStructuralSequence(runtime);

    auto duplicateTransaction = runtime.BeginSpawnTransaction();
    const auto duplicateEntity = duplicateTransaction.Create();
    ASSERT_TRUE(duplicateTransaction.Add<SpawnProbe>(duplicateEntity, {.value = 1}));
    ASSERT_TRUE(duplicateTransaction.Add<SpawnProbe>(duplicateEntity, {.value = 2}));
    const RVX::SceneECS::SceneSpawnCommitResult duplicateResult = duplicateTransaction.Commit();
    EXPECT_TRUE(duplicateResult.IsRejected());
    EXPECT_FALSE(duplicateResult.GetEntity(duplicateEntity).IsValid());
    EXPECT_FALSE(duplicateResult.GetEntityRef(duplicateEntity).IsValid());
    EXPECT_EQ(runtime.GetRegistry().GetEntityCount(), entityCountBefore);
    EXPECT_EQ(NextStructuralSequence(runtime), journalBefore);

    auto authorityTransaction = runtime.BeginSpawnTransaction();
    const auto authorityEntity = authorityTransaction.Create();
    EXPECT_FALSE(authorityTransaction.Add<RVX::SceneECS::Active>(authorityEntity, {.value = false}));
    const RVX::SceneECS::SceneSpawnCommitResult authorityResult = authorityTransaction.Commit();
    EXPECT_TRUE(authorityResult.IsRejected());
    EXPECT_FALSE(authorityResult.GetEntity(authorityEntity).IsValid());
    EXPECT_EQ(runtime.GetRegistry().GetEntityCount(), entityCountBefore);
    EXPECT_EQ(NextStructuralSequence(runtime), journalBefore);
}

TEST(EcsSceneSpawnTransactionValidation, SceneEntityRefsCarryExactSceneAndGenerationIdentity)
{
    RVX::SceneECS::SceneEcsRuntime runtime;
    const RVX::ECS::EntityHandle entity = runtime.CreateEntity();
    ASSERT_TRUE(entity.IsValid());

    const RVX::SceneECS::SceneEcsRuntime& constRuntime = runtime;
    const RVX::SceneECS::SceneEntityRef reference = constRuntime.GetEntityRef(entity);
    ASSERT_TRUE(reference.IsValid());
    EXPECT_EQ(reference.sceneRuntimeId, runtime.GetSceneRuntimeId());
    EXPECT_EQ(reference.entity, entity);

    const RVX::ECS::EntityHandle stale = RVX::ECS::EntityHandle::Create(
        entity.GetIndex(), entity.GetGeneration() + 1u);
    EXPECT_FALSE(constRuntime.GetEntityRef(stale).IsValid());

    RVX::SceneECS::SceneEcsRuntime foreignRuntime;
    const RVX::SceneECS::SceneEntityRef foreignReference =
        foreignRuntime.GetEntityRef(foreignRuntime.CreateEntity());
    ASSERT_TRUE(foreignReference.IsValid());
    auto transaction = runtime.BeginSpawnTransaction();
    const auto child = transaction.Create();
    EXPECT_FALSE(transaction.SetParent(child, foreignReference));
    EXPECT_TRUE(transaction.Commit().IsRejected());
}
