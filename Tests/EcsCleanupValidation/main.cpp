#include "Scene/ECS/SceneEcsRuntime.h"
#include "Scene/ECS/AnimationFragments.h"
#include "Scene/ECS/AudioFragments.h"
#include "Scene/ECS/PhysicsFragments.h"
#include "Scene/ECS/RenderFragments.h"

#include <gtest/gtest.h>

#include <array>

TEST(EcsCleanupValidation, PendingDestroyPublishesValueCleanupAndGatesRecycling)
{
    RVX::SceneECS::SceneEcsRuntime runtime;
    const RVX::ECS::EntityHandle entity = runtime.CreateEntity();
    ASSERT_TRUE(entity.IsValid());
    const RVX::ECS::SceneRuntimeId runtimeId = runtime.GetSceneRuntimeId();

    EXPECT_EQ(runtime.RequestDestroy(entity), RVX::SceneECS::DestroyRequestResult::Accepted);
    EXPECT_FALSE(runtime.GetRegistry().IsEnabled(entity));
    EXPECT_EQ(runtime.RequestDestroy(entity), RVX::SceneECS::DestroyRequestResult::AlreadyPending);
    EXPECT_EQ(runtime.PublishPendingDestroyCleanup(), 1u);

    RVX::SceneECS::CleanupRecordCursor cursor;
    const RVX::SceneECS::CleanupRecordRead cleanup = runtime.ReadCleanupRecords(cursor);
    ASSERT_EQ(cleanup.records.size(), 1u);
    EXPECT_EQ(cleanup.records.front().sceneRuntimeId, runtimeId);
    EXPECT_EQ(cleanup.records.front().entity, entity);
    EXPECT_EQ(cleanup.records.front().requiredCleanupDomains,
              RVX::SceneECS::ToCleanupDomainMask(RVX::SceneECS::CleanupDomain::All));

    EXPECT_TRUE(runtime.AcknowledgeCleanup(
        entity, RVX::SceneECS::ToCleanupDomainMask(RVX::SceneECS::CleanupDomain::Simulation)));
    EXPECT_TRUE(runtime.AcknowledgeCleanup(
        entity, RVX::SceneECS::ToCleanupDomainMask(RVX::SceneECS::CleanupDomain::Render)));
    EXPECT_EQ(runtime.AdvanceRetirements(), 0u);
    EXPECT_TRUE(runtime.AcknowledgeCleanup(
        entity, RVX::SceneECS::ToCleanupDomainMask(RVX::SceneECS::CleanupDomain::Resources)));
    EXPECT_EQ(runtime.AdvanceRetirements(), 1u);
    EXPECT_TRUE(runtime.AcknowledgeCleanup(
        entity, RVX::SceneECS::ToCleanupDomainMask(RVX::SceneECS::CleanupDomain::Resources)));
    EXPECT_TRUE(runtime.GetRegistry().IsAlive(entity));
    EXPECT_EQ(runtime.RecycleRecyclableEntities(), 1u);
    EXPECT_FALSE(runtime.GetRegistry().IsAlive(entity));

    const RVX::ECS::EntityHandle replacement = runtime.CreateEntity();
    ASSERT_TRUE(replacement.IsValid());
    EXPECT_EQ(replacement.GetIndex(), entity.GetIndex());
    EXPECT_NE(replacement.GetGeneration(), entity.GetGeneration());
}

TEST(EcsCleanupValidation, RecyclingParentDetachesDisabledChildWithKeepWorld)
{
    RVX::SceneECS::SceneEcsRuntime runtime;
    const RVX::ECS::EntityHandle parent = runtime.CreateEntity();
    const RVX::ECS::EntityHandle child = runtime.CreateEntity();
    ASSERT_TRUE(parent.IsValid());
    ASSERT_TRUE(child.IsValid());

    RVX::SceneECS::LocalTransform parentTransform;
    parentTransform.translation = {10.0f, 0.0f, 0.0f};
    RVX::SceneECS::LocalTransform childTransform;
    childTransform.translation = {3.0f, 0.0f, 0.0f};
    ASSERT_TRUE(runtime.SetLocalTransform(parent, parentTransform));
    ASSERT_TRUE(runtime.SetLocalTransform(child, childTransform));
    ASSERT_EQ(runtime.Reparent(child, parent, RVX::SceneECS::ReparentMode::KeepLocal),
              RVX::SceneECS::ReparentResult::Applied);
    ASSERT_TRUE(runtime.SetActive(child, false));

    ASSERT_EQ(runtime.RequestDestroy(parent), RVX::SceneECS::DestroyRequestResult::Accepted);
    ASSERT_EQ(runtime.PublishPendingDestroyCleanup(), 1u);
    ASSERT_TRUE(runtime.AcknowledgeCleanup(
        parent, RVX::SceneECS::ToCleanupDomainMask(RVX::SceneECS::CleanupDomain::All)));
    ASSERT_EQ(runtime.AdvanceRetirements(), 1u);
    ASSERT_EQ(runtime.RecycleRecyclableEntities(), 1u);

    EXPECT_FALSE(runtime.GetRegistry().IsAlive(parent));
    EXPECT_EQ(runtime.GetRegistry().TryGet<RVX::SceneECS::ParentRelation>(child), nullptr);
    const auto* childLocal = runtime.GetRegistry().TryGet<RVX::SceneECS::LocalTransform>(child);
    ASSERT_NE(childLocal, nullptr);
    EXPECT_NEAR(childLocal->translation.x, 13.0f, 0.0001f);
}

TEST(EcsCleanupValidation, IndependentCursorFailsClosedAfterBoundedHistoryLoss)
{
    RVX::SceneECS::SceneEcsRuntime runtime(2);
    RVX::SceneECS::CleanupRecordCursor staleCursor;

    for (int index = 0; index < 3; ++index)
    {
        const auto entity = runtime.CreateEntity();
        ASSERT_TRUE(entity.IsValid());
        ASSERT_EQ(runtime.RequestDestroy(
                      entity,
                      RVX::SceneECS::ToCleanupDomainMask(
                          RVX::SceneECS::CleanupDomain::None)),
                  RVX::SceneECS::DestroyRequestResult::Accepted);
        ASSERT_EQ(runtime.PublishPendingDestroyCleanup(), 1u);
    }

    const auto lost = runtime.ReadCleanupRecords(staleCursor);
    EXPECT_EQ(lost.continuity, RVX::SceneECS::CleanupRecordContinuity::Lost);
    EXPECT_TRUE(lost.records.empty());
    EXPECT_EQ(staleCursor.nextSequence, lost.nextSequence);
    EXPECT_EQ(runtime.GetDiagnosticsSnapshot().cleanupContinuityLossCount, 1u);

    RVX::SceneECS::CleanupRecordCursor retained{
        .nextSequence = runtime.GetDiagnosticsSnapshot().firstCleanupSequence,
    };
    const auto current = runtime.ReadCleanupRecords(retained);
    EXPECT_EQ(current.continuity, RVX::SceneECS::CleanupRecordContinuity::Continuous);
    ASSERT_EQ(current.records.size(), 2u);
    EXPECT_LT(current.records[0].sequence, current.records[1].sequence);
}

TEST(EcsCleanupValidation, DestroyBatchInfersEveryBoundSubsystemDomainForTheWholeInstance)
{
    using namespace RVX;

    SceneECS::SceneEcsRuntime runtime;
    const ECS::EntityHandle root = runtime.CreateEntity();
    const ECS::EntityHandle simulatedMember = runtime.CreateEntity();
    const ECS::EntityHandle audibleMember = runtime.CreateEntity();
    ASSERT_TRUE(root.IsValid());
    ASSERT_TRUE(simulatedMember.IsValid());
    ASSERT_TRUE(audibleMember.IsValid());

    ASSERT_TRUE(runtime.AddFragment<SceneECS::Mesh>(root, {}));
    ASSERT_TRUE(runtime.AddFragment<SceneECS::RigidBody>(simulatedMember, {}));
    ASSERT_TRUE(runtime.AddFragment<SceneECS::Animator>(simulatedMember, {}));
    ASSERT_TRUE(runtime.AddFragment<SceneECS::AudioEmitter>(audibleMember, {}));

    const std::array members{root, simulatedMember, audibleMember};
    ASSERT_EQ(runtime.RequestDestroyBatch(
                  members,
                  SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Resources)),
              SceneECS::DestroyRequestResult::Accepted);
    ASSERT_EQ(runtime.PublishPendingDestroyCleanup(), members.size());

    const SceneECS::CleanupDomainMask expected =
        SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Physics) |
        SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Animation) |
        SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Audio) |
        SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Render) |
        SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Resources);

    SceneECS::CleanupRecordCursor cursor;
    const SceneECS::CleanupRecordRead cleanup = runtime.ReadCleanupRecords(cursor);
    ASSERT_EQ(cleanup.records.size(), members.size());
    for (const SceneECS::CleanupRecord& record : cleanup.records)
    {
        EXPECT_EQ(record.requiredCleanupDomains, expected);
        EXPECT_TRUE(runtime.AcknowledgeCleanup(
            record.entity,
            SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Render) |
                SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Resources)));
    }
    EXPECT_EQ(runtime.AdvanceRetirements(), 0u);

    for (const ECS::EntityHandle entity : members)
    {
        EXPECT_TRUE(runtime.AcknowledgeCleanup(
            entity,
            SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Physics) |
                SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Animation) |
                SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Audio)));
    }
    EXPECT_EQ(runtime.AdvanceRetirements(), members.size());
    EXPECT_EQ(runtime.RecycleRecyclableEntities(), members.size());
}

TEST(EcsCleanupValidation, DestroyAllIncludesDisabledEntitiesAndKeepsPerEntityCleanupDomains)
{
    using namespace RVX;

    SceneECS::SceneEcsRuntime runtime;
    const ECS::EntityHandle renderEntity = runtime.CreateEntity();
    const ECS::EntityHandle audioEntity = runtime.CreateEntity();
    const ECS::EntityHandle valueOnlyEntity = runtime.CreateEntity();
    ASSERT_TRUE(runtime.AddFragment<SceneECS::Mesh>(renderEntity, {}));
    ASSERT_TRUE(runtime.AddFragment<SceneECS::AudioEmitter>(audioEntity, {}));
    ASSERT_TRUE(runtime.SetActive(audioEntity, false));

    const SceneECS::DestroyAllRequestResult requested = runtime.RequestDestroyAll(
        SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Resources));
    ASSERT_TRUE(requested.IsAccepted());
    ASSERT_EQ(requested.requestedEntityCount, 3u);
    ASSERT_EQ(runtime.PublishPendingDestroyCleanup(), 3u);

    SceneECS::CleanupRecordCursor cursor;
    const SceneECS::CleanupRecordRead cleanup = runtime.ReadCleanupRecords(cursor);
    ASSERT_EQ(cleanup.records.size(), 3u);
    for (const SceneECS::CleanupRecord& record : cleanup.records)
    {
        SceneECS::CleanupDomainMask expected =
            SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Resources);
        if (record.entity == renderEntity)
        {
            expected |= SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Render);
        }
        else if (record.entity == audioEntity)
        {
            expected |= SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Audio);
        }
        else
        {
            EXPECT_EQ(record.entity, valueOnlyEntity);
        }
        EXPECT_EQ(record.requiredCleanupDomains, expected);
        EXPECT_TRUE(runtime.AcknowledgeCleanup(record.entity, expected));
    }
    EXPECT_EQ(runtime.AdvanceRetirements(), 3u);
    EXPECT_EQ(runtime.RecycleRecyclableEntities(), 3u);
}

TEST(EcsCleanupValidation, DestroyAllExceptRetainsExactAliveGenerationForPresentation)
{
    using namespace RVX;

    SceneECS::SceneEcsRuntime runtime;
    const ECS::EntityHandle retainedCamera = runtime.CreateEntity();
    const ECS::EntityHandle renderEntity = runtime.CreateEntity();
    const ECS::EntityHandle valueOnlyEntity = runtime.CreateEntity();
    ASSERT_TRUE(runtime.AddFragment<SceneECS::Camera>(retainedCamera, {}));
    ASSERT_TRUE(runtime.AddFragment<SceneECS::Mesh>(renderEntity, {}));

    const std::array exclusions{retainedCamera};
    const SceneECS::DestroyAllRequestResult requested =
        runtime.RequestDestroyAllExcept(exclusions);
    ASSERT_TRUE(requested.IsAccepted());
    EXPECT_EQ(requested.requestedEntityCount, 2u);

    const auto* retainedLifecycle =
        runtime.GetRegistry().TryGet<SceneECS::EntityLifecycleState>(retainedCamera);
    const auto* renderLifecycle =
        runtime.GetRegistry().TryGet<SceneECS::EntityLifecycleState>(renderEntity);
    const auto* valueLifecycle =
        runtime.GetRegistry().TryGet<SceneECS::EntityLifecycleState>(valueOnlyEntity);
    ASSERT_NE(retainedLifecycle, nullptr);
    ASSERT_NE(renderLifecycle, nullptr);
    ASSERT_NE(valueLifecycle, nullptr);
    EXPECT_EQ(retainedLifecycle->phase, SceneECS::EntityLifecyclePhase::Alive);
    EXPECT_EQ(renderLifecycle->phase, SceneECS::EntityLifecyclePhase::PendingDestroy);
    EXPECT_EQ(valueLifecycle->phase, SceneECS::EntityLifecyclePhase::PendingDestroy);

    const ECS::EntityHandle stale = ECS::EntityHandle::Create(
        retainedCamera.GetIndex(), retainedCamera.GetGeneration() + 1u);
    const std::array staleExclusion{stale};
    const SceneECS::DestroyAllRequestResult rejected =
        runtime.RequestDestroyAllExcept(staleExclusion);
    EXPECT_EQ(rejected.result, SceneECS::DestroyRequestResult::InvalidEntity);
    EXPECT_EQ(rejected.requestedEntityCount, 0u);
    EXPECT_EQ(retainedLifecycle->phase, SceneECS::EntityLifecyclePhase::Alive);
}
