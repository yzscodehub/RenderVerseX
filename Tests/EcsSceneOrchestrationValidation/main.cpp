#include "Scene/ECS/SceneECS.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace
{
    struct BeginBarrierProbe
    {
        RVX::uint32 value = 0;
    };

    struct PrePresentationBarrierProbe
    {
        RVX::uint32 value = 0;
    };

    struct EndFixedBarrierProbe
    {
        RVX::uint32 value = 0;
    };

    struct MutableValueProbe
    {
        RVX::uint32 value = 0;
    };

    struct SubmissionOrderProbe
    {
        RVX::uint32 value = 0;
    };

    RVX::ECS::ProcessorDescriptor MakeProcessor(
        std::string name,
        RVX::ECS::ProcessorPhase phase,
        RVX::ECS::ProcessorStepMode stepMode,
        std::vector<std::string>& execution)
    {
        std::vector<std::string>* const executionTarget = &execution;
        return {
            .name = std::move(name),
            .phase = phase,
            .stepMode = stepMode,
            .runWithContext = [executionTarget](RVX::ECS::ProcessorExecutionContext& context)
            {
                executionTarget->emplace_back(context.group);
            },
        };
    }
} // namespace

TEST(EcsSceneOrchestrationValidation, CommandBuffersBecomeVisibleOnlyAtTheirSelectedBarriers)
{
    RVX::SceneECS::SceneEcsRuntime runtime;
    const RVX::ECS::EntityHandle entity = runtime.CreateEntity();
    ASSERT_TRUE(entity.IsValid());

    RVX::SceneECS::SceneCommandBuffer beginCommands = runtime.CreateCommandBuffer();
    const RVX::SceneECS::SceneCommandReceipt beginCommand =
        beginCommands.Add<BeginBarrierProbe>(entity, {.value = 11});
    const RVX::SceneECS::SceneCommandBufferReceipt beginSubmission =
        runtime.SubmitCommandBuffer(std::move(beginCommands));
    EXPECT_TRUE(beginSubmission.IsQueued());
    EXPECT_TRUE(beginCommand.IsQueued());

    std::vector<std::string> execution;
    std::optional<RVX::SceneECS::SceneCommandBufferReceipt> prePresentationSubmission;
    std::optional<RVX::SceneECS::SceneCommandBufferReceipt> endFixedSubmission;
    ASSERT_TRUE(runtime.RegisterProcessor({
        .name = "begin-observer",
        .phase = RVX::ECS::ProcessorPhase::BeginSimulation,
        .run = [&execution, entity](RVX::ECS::Registry& registry)
        {
            execution.emplace_back(registry.Has<BeginBarrierProbe>(entity) ?
                                       "begin-visible" :
                                       "begin-missing");
        },
    }));
    ASSERT_TRUE(runtime.RegisterProcessor({
        .name = "gameplay-record-prepresentation",
        .phase = RVX::ECS::ProcessorPhase::Gameplay,
        .run = [&runtime, &execution, &prePresentationSubmission, entity](RVX::ECS::Registry&)
        {
            execution.emplace_back("gameplay");
            RVX::SceneECS::SceneCommandBuffer commands = runtime.CreateCommandBuffer();
            EXPECT_TRUE(commands.Add<PrePresentationBarrierProbe>(entity, {.value = 22}).IsQueued());
            prePresentationSubmission = runtime.SubmitCommandBuffer(
                std::move(commands), RVX::SceneECS::SceneCommandBarrier::PrePresentation);
        },
    }));
    ASSERT_TRUE(runtime.RegisterProcessor({
        .name = "fixed-record-end",
        .phase = RVX::ECS::ProcessorPhase::PhysicsSimulation,
        .stepMode = RVX::ECS::ProcessorStepMode::Fixed,
        .run = [&runtime, &endFixedSubmission, entity](RVX::ECS::Registry&)
        {
            RVX::SceneECS::SceneCommandBuffer commands = runtime.CreateCommandBuffer();
            EXPECT_TRUE(commands.Add<EndFixedBarrierProbe>(entity, {.value = 33}).IsQueued());
            endFixedSubmission = runtime.SubmitCommandBuffer(
                std::move(commands), RVX::SceneECS::SceneCommandBarrier::EndFixedStep);
        },
    }));
    ASSERT_TRUE(runtime.RegisterProcessor({
        .name = "end-fixed-observer",
        .phase = RVX::ECS::ProcessorPhase::EndFixedStep,
        .stepMode = RVX::ECS::ProcessorStepMode::Fixed,
        .run = [&execution, entity](RVX::ECS::Registry& registry)
        {
            execution.emplace_back(registry.Has<EndFixedBarrierProbe>(entity) ?
                                       "end-fixed-visible-too-early" :
                                       "end-fixed-not-yet");
        },
    }));
    ASSERT_TRUE(runtime.RegisterProcessor({
        .name = "post-observer",
        .phase = RVX::ECS::ProcessorPhase::PostSimulation,
        .run = [&execution, entity](RVX::ECS::Registry& registry)
        {
            execution.emplace_back(registry.Has<EndFixedBarrierProbe>(entity) ?
                                       "post-end-fixed-visible" :
                                       "post-end-fixed-missing");
        },
    }));
    ASSERT_TRUE(runtime.RegisterProcessor({
        .name = "prepresentation-observer",
        .phase = RVX::ECS::ProcessorPhase::PrePresentation,
        .run = [&execution, entity](RVX::ECS::Registry& registry)
        {
            execution.emplace_back(registry.Has<PrePresentationBarrierProbe>(entity) ?
                                       "prepresentation-visible" :
                                       "prepresentation-missing");
        },
    }));

    const RVX::SceneECS::SceneEcsTickResult tick = runtime.Tick({
        .variableDeltaSeconds = 1.0 / 60.0,
        .fixedDeltaSeconds = 1.0 / 60.0,
        .fixedStepCount = 1,
    });
    EXPECT_TRUE(tick.succeeded);
    EXPECT_EQ(execution,
              (std::vector<std::string>{
                  "begin-visible",
                  "gameplay",
                  "end-fixed-not-yet",
                  "post-end-fixed-visible",
                  "prepresentation-visible",
              }));
    EXPECT_TRUE(beginSubmission.IsApplied());
    EXPECT_TRUE(beginCommand.IsApplied());
    ASSERT_TRUE(prePresentationSubmission.has_value());
    ASSERT_TRUE(endFixedSubmission.has_value());
    EXPECT_TRUE(prePresentationSubmission->IsApplied());
    EXPECT_TRUE(endFixedSubmission->IsApplied());
    EXPECT_EQ(tick.commandBarriers[0].appliedBufferCount, 1u);
    EXPECT_EQ(tick.commandBarriers[2].appliedBufferCount, 1u);
    EXPECT_EQ(tick.commandBarriers[3].appliedBufferCount, 1u);
}

TEST(EcsSceneOrchestrationValidation, TickRunsVariableAndFixedPhasesInTheDocumentedOrder)
{
    RVX::SceneECS::SceneEcsRuntime runtime;
    std::vector<std::string> execution;

    const std::array variablePhases = {
        RVX::ECS::ProcessorPhase::BeginSimulation,
        RVX::ECS::ProcessorPhase::Gameplay,
        RVX::ECS::ProcessorPhase::PostSimulation,
        RVX::ECS::ProcessorPhase::PrePresentation,
        RVX::ECS::ProcessorPhase::Transform,
        RVX::ECS::ProcessorPhase::Bounds,
        RVX::ECS::ProcessorPhase::Spatial,
        RVX::ECS::ProcessorPhase::Feature,
        RVX::ECS::ProcessorPhase::RenderExtraction,
        RVX::ECS::ProcessorPhase::EndFrameCleanup,
    };
    const std::array fixedPhases = {
        RVX::ECS::ProcessorPhase::BeforeFixedStep,
        RVX::ECS::ProcessorPhase::FixedAnimation,
        RVX::ECS::ProcessorPhase::RootMotion,
        RVX::ECS::ProcessorPhase::SceneToPhysics,
        RVX::ECS::ProcessorPhase::PhysicsSimulation,
        RVX::ECS::ProcessorPhase::PhysicsToScene,
        RVX::ECS::ProcessorPhase::FixedTransformResolve,
        RVX::ECS::ProcessorPhase::EndFixedStep,
    };

    for (const RVX::ECS::ProcessorPhase phase : variablePhases)
    {
        if (phase == RVX::ECS::ProcessorPhase::RenderExtraction)
        {
            ASSERT_TRUE(runtime.RegisterRenderExtractionProcessor(
                "render-extraction-snapshot",
                0,
                [&execution](const RVX::SceneECS::FrozenSceneSnapshot&)
                {
                    execution.emplace_back("RenderExtraction");
                }));
            continue;
        }
        ASSERT_TRUE(runtime.RegisterProcessor(MakeProcessor(
            "variable-" + std::to_string(static_cast<RVX::uint32>(phase)),
            phase,
            RVX::ECS::ProcessorStepMode::Variable,
            execution)));
    }
    for (const RVX::ECS::ProcessorPhase phase : fixedPhases)
    {
        ASSERT_TRUE(runtime.RegisterProcessor(MakeProcessor(
            "fixed-" + std::to_string(static_cast<RVX::uint32>(phase)),
            phase,
            RVX::ECS::ProcessorStepMode::Fixed,
            execution)));
    }

    const RVX::SceneECS::SceneEcsTickResult tick = runtime.Tick({
        .variableDeltaSeconds = 1.0 / 30.0,
        .fixedDeltaSeconds = 1.0 / 60.0,
        .fixedStepCount = 1,
    });
    ASSERT_TRUE(tick.succeeded);
    EXPECT_EQ(tick.frameSequence, 1u);
    EXPECT_EQ(tick.fixedStepsExecuted, 1u);
    EXPECT_EQ(tick.lastFixedStepSequence, 1u);
    EXPECT_EQ(execution,
              (std::vector<std::string>{
                  "BeginSimulation",
                  "Gameplay",
                  "BeforeFixedStep",
                  "FixedAnimation",
                  "RootMotion",
                  "SceneToPhysics",
                  "PhysicsSimulation",
                  "PhysicsToScene",
                  "FixedTransformResolve",
                  "EndFixedStep",
                  "PostSimulation",
                  "PrePresentation",
                  "Transform",
                  "Bounds",
                  "Spatial",
                  "Feature",
                  "RenderExtraction",
                  "EndFrameCleanup",
              }));
}

TEST(EcsSceneOrchestrationValidation, FreezeCopiesPresentationResolvedTransformsAfterVariableMutation)
{
    RVX::SceneECS::RuntimeEntityDesc desc;
    desc.localTransform.translation = {1.0f, 0.0f, 0.0f};
    RVX::SceneECS::SceneEcsRuntime runtime;
    const RVX::ECS::EntityHandle entity = runtime.CreateEntity(desc);
    ASSERT_TRUE(entity.IsValid());

    RVX::SceneECS::Mesh mesh;
    mesh.meshAssetId.value = 10;
    mesh.submeshCount = 1;
    RVX::SceneECS::MaterialSlots materialSlots;
    materialSlots.count = 1;
    materialSlots.values[0].materialAssetId.value = 20;
    ASSERT_TRUE(runtime.AddFragment<RVX::SceneECS::Mesh>(entity, mesh));
    ASSERT_TRUE(runtime.AddFragment<RVX::SceneECS::MaterialSlots>(entity, materialSlots));
    ASSERT_TRUE(runtime.AddFragment<RVX::SceneECS::Visibility>(entity));

    EXPECT_FALSE(runtime.RegisterProcessor({
        .name = "illegal-live-render-extraction",
        .phase = RVX::ECS::ProcessorPhase::RenderExtraction,
        .run = [](RVX::ECS::Registry&) {},
    }));

    ASSERT_TRUE(runtime.RegisterProcessor({
        .name = "gameplay-local-transform-write",
        .phase = RVX::ECS::ProcessorPhase::Gameplay,
        .run = [&runtime, entity](RVX::ECS::Registry&)
        {
            EXPECT_TRUE(runtime.SetLocalTransform(
                entity,
                {.translation = {9.0f, 0.0f, 0.0f}}));
        },
    }));

    float observedExtractionTransformX = 0.0f;
    ASSERT_TRUE(runtime.RegisterRenderExtractionProcessor(
        "snapshot-only-extraction",
        0,
        [&observedExtractionTransformX](const RVX::SceneECS::FrozenSceneSnapshot& snapshot)
        {
            ASSERT_EQ(snapshot.meshes.size(), 1u);
            observedExtractionTransformX = snapshot.meshes.front().worldTransform[3].x;
        }));

    const RVX::SceneECS::SceneEcsTickResult tick = runtime.Tick({
        .variableDeltaSeconds = 1.0 / 60.0,
    });
    ASSERT_TRUE(tick.succeeded);
    EXPECT_EQ(tick.sceneSnapshotRevision, 1u);
    const std::shared_ptr<const RVX::SceneECS::FrozenSceneSnapshot> snapshot =
        runtime.GetLatestFrozenSnapshot();
    ASSERT_NE(snapshot, nullptr);
    ASSERT_EQ(snapshot->revision, tick.sceneSnapshotRevision);
    ASSERT_EQ(snapshot->meshes.size(), 1u);
    EXPECT_EQ(snapshot->meshes.front().id.entity, entity);
    EXPECT_FLOAT_EQ(snapshot->meshes.front().worldTransform[3].x, 9.0f);
    EXPECT_FLOAT_EQ(observedExtractionTransformX, 9.0f);
    EXPECT_GT(snapshot->meshes.front().transformSourceRevision, 0u);
    EXPECT_EQ(runtime.GetSpatialIndex().GetEntryCount(), 1u);

    // A retained extraction snapshot owns its values and is not overwritten by
    // a later frame publication.
    runtime.ClearProcessors();
    ASSERT_TRUE(runtime.SetLocalTransform(entity, {.translation = {13.0f, 0.0f, 0.0f}}));
    ASSERT_TRUE(runtime.Tick().succeeded);
    const std::shared_ptr<const RVX::SceneECS::FrozenSceneSnapshot> nextSnapshot =
        runtime.GetLatestFrozenSnapshot();
    ASSERT_NE(nextSnapshot, nullptr);
    EXPECT_NE(nextSnapshot, snapshot);
    EXPECT_FLOAT_EQ(snapshot->meshes.front().worldTransform[3].x, 9.0f);
    EXPECT_FLOAT_EQ(nextSnapshot->meshes.front().worldTransform[3].x, 13.0f);
}

TEST(EcsSceneOrchestrationValidation, ConcurrentProducerSubmissionsAreSequencedAndPlayedInOrder)
{
    constexpr RVX::uint32 producerCount = 24;

    RVX::SceneECS::SceneEcsRuntime runtime;
    const RVX::ECS::EntityHandle entity = runtime.CreateEntity();
    ASSERT_TRUE(entity.IsValid());
    ASSERT_TRUE(runtime.AddFragment<SubmissionOrderProbe>(entity, {.value = 0}));

    std::array<RVX::SceneECS::SceneCommandReceipt, producerCount> commandReceipts;
    std::array<RVX::SceneECS::SceneCommandBufferReceipt, producerCount> submissions;
    std::atomic<RVX::uint32> readyProducerCount = 0;
    std::atomic<bool> startSubmitting = false;
    std::vector<std::thread> producers;
    producers.reserve(producerCount);
    for (RVX::uint32 index = 0; index < producerCount; ++index)
    {
        producers.emplace_back([&, index]
        {
            RVX::SceneECS::SceneCommandBuffer commands = runtime.CreateCommandBuffer();
            commandReceipts[index] = commands.Set<SubmissionOrderProbe>(
                entity, {.value = index + 1u});
            readyProducerCount.fetch_add(1u, std::memory_order_release);
            while (!startSubmitting.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
            submissions[index] = runtime.SubmitCommandBuffer(std::move(commands));
        });
    }

    while (readyProducerCount.load(std::memory_order_acquire) != producerCount)
    {
        std::this_thread::yield();
    }
    startSubmitting.store(true, std::memory_order_release);
    for (std::thread& producer : producers)
    {
        producer.join();
    }

    std::vector<std::pair<RVX::uint64, RVX::uint32>> expectedPlaybackOrder;
    expectedPlaybackOrder.reserve(producerCount);
    for (RVX::uint32 index = 0; index < producerCount; ++index)
    {
        EXPECT_TRUE(commandReceipts[index].IsQueued());
        EXPECT_TRUE(submissions[index].IsQueued());
        expectedPlaybackOrder.emplace_back(submissions[index].GetSubmissionSequence(), index + 1u);
    }
    std::sort(expectedPlaybackOrder.begin(), expectedPlaybackOrder.end());
    for (RVX::uint32 index = 0; index < producerCount; ++index)
    {
        EXPECT_EQ(expectedPlaybackOrder[index].first, static_cast<RVX::uint64>(index + 1u));
    }

    const RVX::SceneECS::SceneEcsTickResult tick = runtime.Tick();
    ASSERT_TRUE(tick.succeeded);
    EXPECT_EQ(tick.commandBarriers[0].attemptedBufferCount, producerCount);
    EXPECT_EQ(tick.commandBarriers[0].appliedBufferCount, producerCount);

    ASSERT_NE(runtime.GetRegistry().TryGet<SubmissionOrderProbe>(entity), nullptr);
    EXPECT_EQ(runtime.GetRegistry().TryGet<SubmissionOrderProbe>(entity)->value,
              expectedPlaybackOrder.back().second);
    for (RVX::uint32 index = 0; index < producerCount; ++index)
    {
        EXPECT_TRUE(commandReceipts[index].IsApplied());
        EXPECT_TRUE(submissions[index].IsApplied());
    }

    const RVX::SceneECS::SceneCommandDiagnosticsSnapshot diagnostics =
        runtime.GetCommandDiagnosticsSnapshot();
    EXPECT_EQ(diagnostics.nextSubmissionSequence, static_cast<RVX::uint64>(producerCount + 1u));
    EXPECT_EQ(diagnostics.barriers[0].submittedBufferCount, producerCount);
    EXPECT_EQ(diagnostics.barriers[0].queuedBufferCount, 0u);
    EXPECT_EQ(diagnostics.barriers[0].appliedBufferCount, producerCount);
}

TEST(EcsSceneOrchestrationValidation, RecordingIsBoundToItsCreatorButSubmissionMayMoveAcrossProducers)
{
    RVX::SceneECS::SceneEcsRuntime runtime;
    const RVX::ECS::EntityHandle entity = runtime.CreateEntity();
    ASSERT_TRUE(entity.IsValid());

    RVX::SceneECS::SceneCommandBuffer commands = runtime.CreateCommandBuffer();
    const RVX::SceneECS::SceneCommandReceipt recordedByCreator =
        commands.Add<MutableValueProbe>(entity, {.value = 17});
    RVX::SceneECS::SceneCommandReceipt rejectedForeignRecording;
    RVX::SceneECS::SceneCommandBufferReceipt crossProducerSubmission;
    std::thread producer([&runtime,
                          commands = std::move(commands),
                          &rejectedForeignRecording,
                          &crossProducerSubmission]() mutable
    {
        rejectedForeignRecording = commands.Add<SubmissionOrderProbe>(
            RVX::ECS::EntityHandle::Invalid());
        crossProducerSubmission = runtime.SubmitCommandBuffer(std::move(commands));
    });
    producer.join();

    EXPECT_TRUE(recordedByCreator.IsQueued());
    EXPECT_TRUE(rejectedForeignRecording.IsRejected());
    EXPECT_TRUE(crossProducerSubmission.IsQueued());

    ASSERT_TRUE(runtime.Tick().succeeded);
    EXPECT_TRUE(recordedByCreator.IsApplied());
    EXPECT_TRUE(rejectedForeignRecording.IsRejected());
    EXPECT_TRUE(crossProducerSubmission.IsApplied());
    ASSERT_NE(runtime.GetRegistry().TryGet<MutableValueProbe>(entity), nullptr);
    EXPECT_EQ(runtime.GetRegistry().TryGet<MutableValueProbe>(entity)->value, 17u);
}

TEST(EcsSceneOrchestrationValidation,
     SkinnedMeshPoseOwnerRebindPrevalidatesThenAtomicallyRetargetsEveryMesh)
{
    RVX::SceneECS::SceneEcsRuntime runtime;
    const RVX::ECS::EntityHandle sourcePose = runtime.CreateEntity();
    const RVX::ECS::EntityHandle incompatiblePose = runtime.CreateEntity();
    const RVX::ECS::EntityHandle motionPose = runtime.CreateEntity();
    const RVX::ECS::EntityHandle firstMesh = runtime.CreateEntity();
    const RVX::ECS::EntityHandle secondMesh = runtime.CreateEntity();
    ASSERT_TRUE(sourcePose.IsValid());
    ASSERT_TRUE(incompatiblePose.IsValid());
    ASSERT_TRUE(motionPose.IsValid());
    ASSERT_TRUE(firstMesh.IsValid());
    ASSERT_TRUE(secondMesh.IsValid());

    const auto addPoseBinding = [&runtime](RVX::ECS::EntityHandle entity,
                                           RVX::int32 skinIndex,
                                           RVX::uint64 animationAssetValue)
    {
        return runtime.AddFragment<RVX::SceneECS::AnimationSkeletonBinding>(
            entity,
            {
                .animationAssetValue = animationAssetValue,
                .sourceModelAssetValue = 700,
                .sourceSkinIndex = skinIndex,
                .boneCount = 24,
            });
    };
    ASSERT_TRUE(addPoseBinding(sourcePose, 5, 701));
    ASSERT_TRUE(addPoseBinding(incompatiblePose, 6, 702));
    ASSERT_TRUE(addPoseBinding(motionPose, 5, 703));

    const auto addMeshBinding = [&runtime, sourcePose](RVX::ECS::EntityHandle entity,
                                                        RVX::uint64 meshAssetValue)
    {
        return runtime.AddFragment<RVX::SceneECS::Mesh>(
                   entity,
                   {
                       .meshAssetId = {.value = meshAssetValue},
                       .submeshCount = 1,
                   }) &&
               runtime.AddFragment<RVX::SceneECS::SkinnedMeshBinding>(
                   entity,
                   {
                       .poseEntity = sourcePose,
                       .sourceModelAssetValue = 700,
                       .sourceSkinIndex = 5,
                   });
    };
    ASSERT_TRUE(addMeshBinding(firstMesh, 801));
    ASSERT_TRUE(addMeshBinding(secondMesh, 802));
    ASSERT_TRUE(runtime.SetFragmentEnabled<RVX::SceneECS::SkinnedMeshBinding>(
        secondMesh, false));

    const std::array<RVX::SceneECS::SceneEntityRef, 2> meshes = {
        runtime.GetEntityRef(firstMesh),
        runtime.GetEntityRef(secondMesh),
    };
    const RVX::SceneECS::SceneEntityRef sourcePoseRef = runtime.GetEntityRef(sourcePose);
    const RVX::SceneECS::SceneEntityRef motionPoseRef = runtime.GetEntityRef(motionPose);
    const RVX::SceneECS::SkinnedMeshPoseRebindRequest request{
        .expectedSceneRuntimeId = runtime.GetSceneRuntimeId(),
        .meshMembers = meshes,
        .expectedPoseOwner = sourcePoseRef,
        .newPoseOwner = motionPoseRef,
        .sourceModelAssetValue = 700,
        .sourceSkinIndex = 5,
    };

    RVX::SceneECS::SkinnedMeshPoseRebindRequest invalidRequest = request;
    invalidRequest.newPoseOwner = runtime.GetEntityRef(incompatiblePose);
    const RVX::SceneECS::SkinnedMeshPoseRebindReceipt rejected =
        runtime.RebindSkinnedMeshPoseOwner(invalidRequest);
    EXPECT_EQ(rejected.code, RVX::SceneECS::SkinnedMeshPoseRebindCode::PoseBindingMismatch);
    EXPECT_FALSE(rejected.IsApplied());
    EXPECT_EQ(runtime.GetRegistry().TryGet<RVX::SceneECS::SkinnedMeshBinding>(firstMesh)->poseEntity,
              sourcePose);
    EXPECT_EQ(runtime.GetRegistry().TryGet<RVX::SceneECS::SkinnedMeshBinding>(secondMesh)->poseEntity,
              sourcePose);
    EXPECT_FALSE(runtime.GetRegistry().IsFragmentEnabled<RVX::SceneECS::SkinnedMeshBinding>(
        secondMesh));

    const RVX::SceneECS::SkinnedMeshPoseRebindReceipt applied =
        runtime.RebindSkinnedMeshPoseOwner(request);
    ASSERT_TRUE(applied.IsApplied());
    EXPECT_EQ(applied.meshCount, 2u);
    for (const RVX::ECS::EntityHandle mesh : {firstMesh, secondMesh})
    {
        const auto* binding = runtime.GetRegistry().TryGet<RVX::SceneECS::SkinnedMeshBinding>(mesh);
        ASSERT_NE(binding, nullptr);
        EXPECT_EQ(binding->poseEntity, motionPose);
        EXPECT_EQ(binding->sourceModelAssetValue, 700u);
        EXPECT_EQ(binding->sourceSkinIndex, 5);
    }
    EXPECT_TRUE(runtime.GetRegistry().IsFragmentEnabled<RVX::SceneECS::SkinnedMeshBinding>(
        firstMesh));
    EXPECT_FALSE(runtime.GetRegistry().IsFragmentEnabled<RVX::SceneECS::SkinnedMeshBinding>(
        secondMesh));
}

TEST(EcsSceneOrchestrationValidation, RetainedSubmissionPortRejectsAfterRuntimeDestruction)
{
    RVX::SceneECS::SceneCommandSubmissionPort port;
    RVX::SceneECS::SceneCommandBuffer lateCommands;
    RVX::SceneECS::SceneCommandReceipt lateCommand;
    RVX::SceneECS::SceneCommandReceipt queuedCommand;
    RVX::SceneECS::SceneCommandBufferReceipt queuedSubmission;

    {
        RVX::SceneECS::SceneEcsRuntime runtime;
        const RVX::ECS::EntityHandle entity = runtime.CreateEntity();
        ASSERT_TRUE(entity.IsValid());
        port = runtime.AcquireCommandSubmissionPort();
        ASSERT_TRUE(port.IsOpen());

        lateCommands = port.CreateCommandBuffer();
        lateCommand = lateCommands.Add<MutableValueProbe>(entity, {.value = 5});
        ASSERT_TRUE(lateCommand.IsQueued());

        RVX::SceneECS::SceneCommandBuffer queuedCommands = port.CreateCommandBuffer();
        queuedCommand = queuedCommands.Add<MutableValueProbe>(entity, {.value = 6});
        queuedSubmission = port.SubmitCommandBuffer(std::move(queuedCommands));
        ASSERT_TRUE(queuedCommand.IsQueued());
        ASSERT_TRUE(queuedSubmission.IsQueued());
    }

    EXPECT_FALSE(port.IsOpen());
    EXPECT_TRUE(queuedCommand.IsDiscarded());
    EXPECT_TRUE(queuedSubmission.IsDiscarded());

    const RVX::SceneECS::SceneCommandBufferReceipt lateSubmission =
        port.SubmitCommandBuffer(std::move(lateCommands));
    EXPECT_TRUE(lateCommand.IsRejected());
    EXPECT_TRUE(lateSubmission.IsRejected());

    RVX::SceneECS::SceneCommandBuffer postShutdownCommands = port.CreateCommandBuffer();
    const RVX::SceneECS::SceneCommandReceipt postShutdownCommand =
        postShutdownCommands.Add<MutableValueProbe>(RVX::ECS::EntityHandle::Invalid(), {.value = 7});
    const RVX::SceneECS::SceneCommandBufferReceipt postShutdownSubmission =
        port.SubmitCommandBuffer(std::move(postShutdownCommands));
    EXPECT_TRUE(postShutdownCommand.IsRejected());
    EXPECT_TRUE(postShutdownSubmission.IsRejected());
}

TEST(EcsSceneOrchestrationValidation, CommandReceiptsMayBePolledConcurrentlyWithOwnerPlayback)
{
    constexpr RVX::uint32 producerCount = 12;

    RVX::SceneECS::SceneEcsRuntime runtime;
    const RVX::ECS::EntityHandle entity = runtime.CreateEntity();
    ASSERT_TRUE(entity.IsValid());
    ASSERT_TRUE(runtime.AddFragment<SubmissionOrderProbe>(entity, {.value = 0}));
    const RVX::SceneECS::SceneCommandSubmissionPort port =
        runtime.AcquireCommandSubmissionPort();

    std::array<RVX::SceneECS::SceneCommandBuffer, producerCount> commandBuffers;
    std::array<RVX::SceneECS::SceneCommandReceipt, producerCount> commandReceipts;
    std::array<RVX::SceneECS::SceneCommandBufferReceipt, producerCount> submissions;
    for (RVX::uint32 index = 0; index < producerCount; ++index)
    {
        commandBuffers[index] = port.CreateCommandBuffer();
        commandReceipts[index] = commandBuffers[index].Set<SubmissionOrderProbe>(
            entity, {.value = index + 1u});
        ASSERT_TRUE(commandReceipts[index].IsQueued());
    }

    std::vector<std::thread> producers;
    producers.reserve(producerCount);
    for (RVX::uint32 index = 0; index < producerCount; ++index)
    {
        producers.emplace_back([port, &commandBuffers, &submissions, index]() mutable
        {
            submissions[index] = port.SubmitCommandBuffer(std::move(commandBuffers[index]));
        });
    }
    for (std::thread& producer : producers)
    {
        producer.join();
    }

    std::atomic<bool> stopPolling = false;
    std::atomic<RVX::uint32> pollingPassCount = 0;
    std::thread poller([&commandReceipts, &submissions, &stopPolling, &pollingPassCount]
    {
        while (!stopPolling.load(std::memory_order_acquire))
        {
            for (RVX::uint32 index = 0; index < producerCount; ++index)
            {
                static_cast<void>(commandReceipts[index].GetStatus());
                static_cast<void>(commandReceipts[index].GetError());
                static_cast<void>(submissions[index].GetStatus());
                static_cast<void>(submissions[index].GetSubmissionSequence());
            }
            pollingPassCount.fetch_add(1u, std::memory_order_release);
        }
    });

    while (pollingPassCount.load(std::memory_order_acquire) == 0u)
    {
        std::this_thread::yield();
    }

    const RVX::SceneECS::SceneEcsTickResult tick = runtime.Tick();
    stopPolling.store(true, std::memory_order_release);
    poller.join();

    ASSERT_TRUE(tick.succeeded);
    EXPECT_EQ(tick.commandBarriers[0].attemptedBufferCount, producerCount);
    EXPECT_GT(pollingPassCount.load(std::memory_order_acquire), 0u);
    for (RVX::uint32 index = 0; index < producerCount; ++index)
    {
        EXPECT_TRUE(commandReceipts[index].IsApplied());
        EXPECT_TRUE(submissions[index].IsApplied());
    }
}

TEST(EcsSceneOrchestrationValidation, SubmissionsAfterBarrierBatchDetachesWaitForTheNextEligibleBarrier)
{
    RVX::SceneECS::SceneEcsRuntime runtime;
    const RVX::ECS::EntityHandle entity = runtime.CreateEntity();
    ASSERT_TRUE(entity.IsValid());
    ASSERT_TRUE(runtime.AddFragment<MutableValueProbe>(entity, {.value = 0}));
    const RVX::SceneECS::SceneCommandSubmissionPort port =
        runtime.AcquireCommandSubmissionPort();

    RVX::SceneECS::SceneCommandBuffer firstCommands = port.CreateCommandBuffer();
    const RVX::SceneECS::SceneCommandReceipt firstCommand =
        firstCommands.Set<MutableValueProbe>(entity, {.value = 1});
    const RVX::SceneECS::SceneCommandBufferReceipt firstSubmission =
        port.SubmitCommandBuffer(std::move(firstCommands));

    RVX::SceneECS::SceneCommandReceipt deferredCommand;
    RVX::SceneECS::SceneCommandBufferReceipt deferredSubmission;
    std::mutex synchronizationMutex;
    std::condition_variable synchronization;
    bool producerMaySubmit = false;
    bool producerSubmitted = false;
    std::thread producer([port,
                          &deferredCommand,
                          &deferredSubmission,
                          &synchronizationMutex,
                          &synchronization,
                          &producerMaySubmit,
                          &producerSubmitted,
                          entity]
    {
        {
            std::unique_lock lock(synchronizationMutex);
            synchronization.wait(lock, [&producerMaySubmit] { return producerMaySubmit; });
        }
        RVX::SceneECS::SceneCommandBuffer commands = port.CreateCommandBuffer();
        deferredCommand = commands.Set<MutableValueProbe>(entity, {.value = 2});
        deferredSubmission = port.SubmitCommandBuffer(std::move(commands));
        {
            std::lock_guard lock(synchronizationMutex);
            producerSubmitted = true;
        }
        synchronization.notify_one();
    });

    // BeginSimulation processors run only after the corresponding barrier batch
    // has detached. Holding this processor until the producer queues work gives
    // the deferral assertion an explicit ordering, not a scheduling window.
    ASSERT_TRUE(runtime.RegisterProcessor({
        .name = "submit-after-begin-barrier-detach",
        .phase = RVX::ECS::ProcessorPhase::BeginSimulation,
        .run = [&synchronizationMutex,
                &synchronization,
                &producerMaySubmit,
                &producerSubmitted](RVX::ECS::Registry&)
        {
            std::unique_lock lock(synchronizationMutex);
            producerMaySubmit = true;
            synchronization.notify_one();
            synchronization.wait(lock, [&producerSubmitted] { return producerSubmitted; });
        },
    }));

    const RVX::SceneECS::SceneEcsTickResult firstTick = runtime.Tick();
    producer.join();
    ASSERT_TRUE(firstTick.succeeded);
    EXPECT_EQ(firstTick.commandBarriers[0].attemptedBufferCount, 1u);
    EXPECT_TRUE(firstCommand.IsApplied());
    EXPECT_TRUE(firstSubmission.IsApplied());
    EXPECT_TRUE(deferredCommand.IsQueued());
    EXPECT_TRUE(deferredSubmission.IsQueued());
    EXPECT_EQ(runtime.GetRegistry().TryGet<MutableValueProbe>(entity)->value, 1u);

    const RVX::SceneECS::SceneEcsTickResult secondTick = runtime.Tick();
    ASSERT_TRUE(secondTick.succeeded);
    EXPECT_EQ(secondTick.commandBarriers[0].attemptedBufferCount, 1u);
    EXPECT_TRUE(deferredCommand.IsApplied());
    EXPECT_TRUE(deferredSubmission.IsApplied());
    EXPECT_EQ(runtime.GetRegistry().TryGet<MutableValueProbe>(entity)->value, 2u);
}

TEST(EcsSceneOrchestrationValidation, RejectedCommandBufferIsReportedWithoutAbortingTheFrame)
{
    RVX::SceneECS::SceneEcsRuntime runtime;
    RVX::SceneECS::SceneCommandBuffer commands = runtime.CreateCommandBuffer();
    const RVX::SceneECS::SceneCommandReceipt invalidDestroy =
        commands.RequestDestroy(RVX::ECS::EntityHandle::Invalid());
    const RVX::SceneECS::SceneCommandBufferReceipt submission =
        runtime.SubmitCommandBuffer(std::move(commands));

    const RVX::SceneECS::SceneEcsTickResult tick = runtime.Tick();
    EXPECT_TRUE(tick.succeeded);
    EXPECT_TRUE(submission.IsRejected());
    EXPECT_TRUE(invalidDestroy.IsRejected());
    EXPECT_EQ(tick.commandBarriers[0].rejectedBufferCount, 1u);

    const RVX::SceneECS::SceneCommandDiagnosticsSnapshot diagnostics =
        runtime.GetCommandDiagnosticsSnapshot();
    EXPECT_EQ(diagnostics.barriers[0].rejectedBufferCount, 1u);
    EXPECT_EQ(runtime.GetDiagnosticsSnapshot().rejectedCommandBufferCount, 1u);
}

TEST(EcsSceneOrchestrationValidation, EndFrameCleanupPublishesThenRecyclesAfterAcknowledgement)
{
    RVX::SceneECS::SceneEcsRuntime runtime;
    const RVX::ECS::EntityHandle entity = runtime.CreateEntity();
    ASSERT_TRUE(entity.IsValid());
    ASSERT_EQ(runtime.RequestDestroy(
                  entity,
                  RVX::SceneECS::ToCleanupDomainMask(RVX::SceneECS::CleanupDomain::Simulation)),
              RVX::SceneECS::DestroyRequestResult::Accepted);

    const RVX::SceneECS::SceneEcsTickResult publishTick = runtime.Tick();
    ASSERT_TRUE(publishTick.succeeded);
    EXPECT_EQ(publishTick.publishedCleanupRecordCount, 1u);
    const RVX::SceneECS::EntityLifecycleState* lifecycle =
        runtime.GetRegistry().TryGet<RVX::SceneECS::EntityLifecycleState>(entity);
    ASSERT_NE(lifecycle, nullptr);
    EXPECT_EQ(lifecycle->phase, RVX::SceneECS::EntityLifecyclePhase::CleanupRequired);

    ASSERT_TRUE(runtime.AcknowledgeCleanup(
        entity,
        RVX::SceneECS::ToCleanupDomainMask(RVX::SceneECS::CleanupDomain::Simulation)));
    const RVX::SceneECS::SceneEcsTickResult recycleTick = runtime.Tick();
    ASSERT_TRUE(recycleTick.succeeded);
    EXPECT_EQ(recycleTick.advancedRetirementCount, 1u);
    EXPECT_EQ(recycleTick.recycledEntityCount, 1u);
    EXPECT_FALSE(runtime.GetRegistry().IsAlive(entity));
}

TEST(EcsSceneOrchestrationValidation, DestroyBatchRejectsWithoutPublishingAPrefix)
{
    RVX::SceneECS::SceneEcsRuntime runtime;
    const RVX::ECS::EntityHandle first = runtime.CreateEntity();
    const RVX::ECS::EntityHandle second = runtime.CreateEntity();
    ASSERT_TRUE(first.IsValid());
    ASSERT_TRUE(second.IsValid());

    const std::array invalidBatch{
        first,
        RVX::ECS::EntityHandle::Invalid(),
        second,
    };
    EXPECT_EQ(runtime.RequestDestroyBatch(invalidBatch),
              RVX::SceneECS::DestroyRequestResult::InvalidEntity);
    ASSERT_NE(runtime.GetRegistry().TryGet<RVX::SceneECS::EntityLifecycleState>(first),
              nullptr);
    ASSERT_NE(runtime.GetRegistry().TryGet<RVX::SceneECS::EntityLifecycleState>(second),
              nullptr);
    EXPECT_EQ(runtime.GetRegistry()
                  .TryGet<RVX::SceneECS::EntityLifecycleState>(first)
                  ->phase,
              RVX::SceneECS::EntityLifecyclePhase::Alive);
    EXPECT_EQ(runtime.GetRegistry()
                  .TryGet<RVX::SceneECS::EntityLifecycleState>(second)
                  ->phase,
              RVX::SceneECS::EntityLifecyclePhase::Alive);
    EXPECT_TRUE(runtime.GetRegistry().IsEnabled(first));
    EXPECT_TRUE(runtime.GetRegistry().IsEnabled(second));
    EXPECT_EQ(runtime.PublishPendingDestroyCleanup(), 0u);

    const std::array validBatch{first, second};
    EXPECT_EQ(runtime.RequestDestroyBatch(validBatch),
              RVX::SceneECS::DestroyRequestResult::Accepted);
    EXPECT_EQ(runtime.PublishPendingDestroyCleanup(), 2u);
}

TEST(EcsSceneOrchestrationValidation, ProcessorsCannotBypassStructuralPlaybackGuard)
{
    struct WritableProbe
    {
        RVX::uint32 value = 0;
    };
    struct DirectMutationProbe
    {
        RVX::uint32 value = 0;
    };
    struct DeferredMutationProbe
    {
        RVX::uint32 value = 0;
    };

    RVX::SceneECS::SceneEcsRuntime runtime;
    const RVX::ECS::EntityHandle entity = runtime.CreateEntity();
    ASSERT_TRUE(runtime.AddFragment<WritableProbe>(entity));

    bool directAddAccepted = true;
    bool directDestroyAccepted = true;
    bool rawCommitAccepted = true;
    bool writeAccepted = false;
    bool submittedDeferred = false;
    std::optional<RVX::SceneECS::SceneCommandBufferReceipt> deferredSubmission;
    ASSERT_TRUE(runtime.RegisterProcessor({
        .name = "attempt-structural-bypass",
        .phase = RVX::ECS::ProcessorPhase::Gameplay,
        .run = [&runtime,
                &directAddAccepted,
                &directDestroyAccepted,
                &rawCommitAccepted,
                &writeAccepted,
                &submittedDeferred,
                &deferredSubmission,
                entity](RVX::ECS::Registry& registry)
        {
            directAddAccepted = registry.Add<DirectMutationProbe>(entity);
            directDestroyAccepted = registry.DestroyEntity(entity);
            RVX::ECS::EntityCommandBuffer rawCommands = registry.CreateCommandBuffer();
            EXPECT_TRUE(rawCommands.Add<DirectMutationProbe>(entity).IsQueued());
            rawCommitAccepted = rawCommands.Commit(registry);
            writeAccepted = registry.Write<WritableProbe>(
                entity,
                [](WritableProbe& probe) { probe.value = 7; });

            if (!submittedDeferred)
            {
                RVX::SceneECS::SceneCommandBuffer sceneCommands = runtime.CreateCommandBuffer();
                EXPECT_TRUE(sceneCommands.Add<DeferredMutationProbe>(entity, {.value = 9}).IsQueued());
                deferredSubmission = runtime.SubmitCommandBuffer(std::move(sceneCommands));
                submittedDeferred = true;
            }
        },
    }));

    ASSERT_TRUE(runtime.Tick().succeeded);
    EXPECT_FALSE(directAddAccepted);
    EXPECT_FALSE(directDestroyAccepted);
    EXPECT_FALSE(rawCommitAccepted);
    EXPECT_TRUE(writeAccepted);
    ASSERT_NE(runtime.GetRegistry().TryGet<WritableProbe>(entity), nullptr);
    EXPECT_EQ(runtime.GetRegistry().TryGet<WritableProbe>(entity)->value, 7u);
    EXPECT_FALSE(runtime.GetRegistry().Has<DeferredMutationProbe>(entity));
    ASSERT_TRUE(deferredSubmission.has_value());
    EXPECT_TRUE(deferredSubmission->IsQueued());

    ASSERT_TRUE(runtime.Tick().succeeded);
    EXPECT_TRUE(deferredSubmission->IsApplied());
    EXPECT_TRUE(runtime.GetRegistry().Has<DeferredMutationProbe>(entity));
}

TEST(EcsSceneOrchestrationValidation, SceneCommandBufferBlocksAuthorityFragmentsAndRoutesLifecycleAndHierarchy)
{
    struct OptionalProbe
    {
        RVX::uint32 value = 0;
    };

    RVX::SceneECS::SceneEcsRuntime runtime;
    const RVX::ECS::EntityHandle parent = runtime.CreateEntity();
    const RVX::ECS::EntityHandle child = runtime.CreateEntity();
    const RVX::ECS::EntityHandle retiring = runtime.CreateEntity();
    ASSERT_TRUE(parent.IsValid());
    ASSERT_TRUE(child.IsValid());
    ASSERT_TRUE(retiring.IsValid());

    RVX::SceneECS::SceneCommandBuffer commands = runtime.CreateCommandBuffer();
    EXPECT_TRUE(commands.Add<RVX::SceneECS::ParentRelation>(child, {.parent = parent}).IsRejected());
    EXPECT_TRUE(commands.Add<RVX::SceneECS::LocalTransform>(child).IsRejected());
    EXPECT_TRUE(commands.Add<RVX::SceneECS::EntityLifecycleState>(child).IsRejected());
    const RVX::SceneECS::SceneEntityReceipt created = commands.Create();
    const RVX::SceneECS::SceneCommandReceipt optionalAdd =
        commands.Add<OptionalProbe>(created, {.value = 3});
    const RVX::SceneECS::SceneCommandReceipt firstReparent = commands.Reparent(child, parent);
    const RVX::SceneECS::SceneCommandReceipt cyclicReparent = commands.Reparent(parent, child);
    const RVX::SceneECS::SceneCommandReceipt deferredDestroy = commands.RequestDestroy(
        retiring,
        RVX::SceneECS::ToCleanupDomainMask(RVX::SceneECS::CleanupDomain::Simulation));
    const RVX::SceneECS::SceneCommandBufferReceipt submission =
        runtime.SubmitCommandBuffer(std::move(commands));

    ASSERT_TRUE(runtime.Tick().succeeded);
    EXPECT_TRUE(created.IsApplied());
    EXPECT_TRUE(created.IsResolved());
    EXPECT_TRUE(optionalAdd.IsApplied());
    EXPECT_TRUE(firstReparent.IsApplied());
    EXPECT_TRUE(cyclicReparent.IsRejected());
    EXPECT_TRUE(deferredDestroy.IsRejected());
    EXPECT_TRUE(submission.IsRejected());
    EXPECT_EQ(runtime.GetTransformHierarchy().GetParent(child), parent);
    EXPECT_TRUE(runtime.GetRegistry().IsAlive(created.GetEntity()));
    EXPECT_NE(runtime.GetRegistry().TryGet<RVX::SceneECS::LocalTransform>(created.GetEntity()), nullptr);
    EXPECT_NE(runtime.GetRegistry().TryGet<RVX::SceneECS::EntityLifecycleState>(created.GetEntity()), nullptr);
    EXPECT_TRUE(runtime.GetRegistry().Has<OptionalProbe>(created.GetEntity()));

    // The rejected cyclic command stops non-atomic playback, so lifecycle
    // work must be submitted independently and enter the cleanup gate rather
    // than physically destroying the slot.
    RVX::SceneECS::SceneCommandBuffer destroyCommands = runtime.CreateCommandBuffer();
    const RVX::SceneECS::SceneCommandReceipt acceptedDestroy = destroyCommands.RequestDestroy(
        retiring,
        RVX::SceneECS::ToCleanupDomainMask(RVX::SceneECS::CleanupDomain::Simulation));
    ASSERT_TRUE(runtime.SubmitCommandBuffer(std::move(destroyCommands)).IsQueued());
    ASSERT_TRUE(runtime.Tick().succeeded);
    EXPECT_TRUE(acceptedDestroy.IsApplied());
    const RVX::SceneECS::EntityLifecycleState* lifecycle =
        runtime.GetRegistry().TryGet<RVX::SceneECS::EntityLifecycleState>(retiring);
    ASSERT_NE(lifecycle, nullptr);
    EXPECT_EQ(lifecycle->phase, RVX::SceneECS::EntityLifecyclePhase::CleanupRequired);
    EXPECT_TRUE(runtime.GetRegistry().IsAlive(retiring));
}

TEST(EcsSceneOrchestrationValidation, RejectsProcessorsAssignedToTheWrongClock)
{
    RVX::SceneECS::SceneEcsRuntime runtime;

    RVX::ECS::ProcessorDescriptor variableOnFixed;
    variableOnFixed.name = "VariableOnFixed";
    variableOnFixed.phase = RVX::ECS::ProcessorPhase::FixedAnimation;
    variableOnFixed.stepMode = RVX::ECS::ProcessorStepMode::Variable;
    variableOnFixed.run = [](RVX::ECS::Registry&) {};
    EXPECT_FALSE(runtime.RegisterProcessor(std::move(variableOnFixed)));

    RVX::ECS::ProcessorDescriptor fixedOnVariable;
    fixedOnVariable.name = "FixedOnVariable";
    fixedOnVariable.phase = RVX::ECS::ProcessorPhase::Gameplay;
    fixedOnVariable.stepMode = RVX::ECS::ProcessorStepMode::Fixed;
    fixedOnVariable.run = [](RVX::ECS::Registry&) {};
    EXPECT_FALSE(runtime.RegisterProcessor(std::move(fixedOnVariable)));
}

TEST(EcsSceneOrchestrationValidation, ProcessorBatchRegistrationIsAllOrNothing)
{
    RVX::SceneECS::SceneEcsRuntime runtime;
    std::vector<std::string> executions;

    ASSERT_TRUE(runtime.RegisterProcessor({
        .name = "occupied",
        .phase = RVX::ECS::ProcessorPhase::Gameplay,
        .run = [](RVX::ECS::Registry&) {},
    }));

    std::vector<RVX::ECS::ProcessorDescriptor> rejected;
    rejected.push_back({
        .name = "must-not-leak",
        .phase = RVX::ECS::ProcessorPhase::Gameplay,
        .run = [](RVX::ECS::Registry&) {},
    });
    rejected.push_back({
        .name = "occupied",
        .phase = RVX::ECS::ProcessorPhase::Gameplay,
        .run = [](RVX::ECS::Registry&) {},
    });
    EXPECT_FALSE(runtime.RegisterProcessors(std::move(rejected)));
    EXPECT_TRUE(runtime.RegisterProcessor({
        .name = "must-not-leak",
        .phase = RVX::ECS::ProcessorPhase::Gameplay,
        .run = [](RVX::ECS::Registry&) {},
    }));

    std::vector<RVX::ECS::ProcessorDescriptor> accepted;
    accepted.push_back({
        .name = "batch-a",
        .phase = RVX::ECS::ProcessorPhase::Gameplay,
        .order = 1,
        .run = [&executions](RVX::ECS::Registry&) { executions.emplace_back("a"); },
    });
    accepted.push_back({
        .name = "batch-b",
        .phase = RVX::ECS::ProcessorPhase::Gameplay,
        .order = 2,
        .run = [&executions](RVX::ECS::Registry&) { executions.emplace_back("b"); },
    });
    ASSERT_TRUE(runtime.RegisterProcessors(std::move(accepted)));
    ASSERT_TRUE(runtime.Tick().succeeded);
    EXPECT_EQ(executions, (std::vector<std::string>{"a", "b"}));
}

TEST(EcsSceneOrchestrationValidation, RejectsNonFiniteVariableAndFixedClocks)
{
    RVX::SceneECS::SceneEcsRuntime runtime;

    EXPECT_FALSE(runtime.Tick({
        .variableDeltaSeconds = std::numeric_limits<double>::quiet_NaN(),
    }).succeeded);
    EXPECT_FALSE(runtime.Tick({
        .variableDeltaSeconds = std::numeric_limits<double>::infinity(),
    }).succeeded);
    EXPECT_FALSE(runtime.Tick({
        .fixedDeltaSeconds = std::numeric_limits<double>::quiet_NaN(),
        .fixedStepCount = 1,
    }).succeeded);
    EXPECT_FALSE(runtime.Tick({
        .fixedDeltaSeconds = std::numeric_limits<double>::infinity(),
        .fixedStepCount = 1,
    }).succeeded);
}

TEST(EcsSceneOrchestrationValidation, ValueReplacementDoesNotOpenSceneAuthorityFragments)
{
    RVX::SceneECS::SceneEcsRuntime runtime;
    const RVX::ECS::EntityHandle entity = runtime.CreateEntity();
    ASSERT_TRUE(entity.IsValid());
    ASSERT_TRUE(runtime.AddFragment<MutableValueProbe>(entity, {.value = 1}));
    ASSERT_TRUE(runtime.SetFragment<MutableValueProbe>(entity, {.value = 2}));
    ASSERT_NE(runtime.GetRegistry().TryGet<MutableValueProbe>(entity), nullptr);
    EXPECT_EQ(runtime.GetRegistry().TryGet<MutableValueProbe>(entity)->value, 2u);

    EXPECT_FALSE(runtime.SetFragment<RVX::SceneECS::EntityLifecycleState>(entity, {}));

    RVX::SceneECS::SceneCommandBuffer commands = runtime.CreateCommandBuffer();
    const RVX::SceneECS::SceneCommandReceipt valueSet =
        commands.Set<MutableValueProbe>(entity, {.value = 3});
    const RVX::SceneECS::SceneCommandReceipt authoritySet =
        commands.Set<RVX::SceneECS::ParentRelation>(entity, {});
    ASSERT_TRUE(runtime.SubmitCommandBuffer(std::move(commands)).IsQueued());
    ASSERT_TRUE(runtime.Tick().succeeded);
    EXPECT_TRUE(valueSet.IsApplied());
    EXPECT_TRUE(authoritySet.IsRejected());
    EXPECT_EQ(runtime.GetRegistry().TryGet<MutableValueProbe>(entity)->value, 3u);
}

TEST(EcsSceneOrchestrationValidation, CommandReceiptsNeverRemainQueuedAfterDiscardOrRejectedSubmission)
{
    RVX::SceneECS::SceneCommandReceipt abandonedCommand;
    {
        RVX::SceneECS::SceneEcsRuntime runtime;
        const RVX::ECS::EntityHandle entity = runtime.CreateEntity();
        RVX::SceneECS::SceneCommandBuffer abandoned = runtime.CreateCommandBuffer();
        abandonedCommand = abandoned.Add<MutableValueProbe>(entity, {.value = 1});
        ASSERT_TRUE(abandonedCommand.IsQueued());
    }
    EXPECT_TRUE(abandonedCommand.IsDiscarded());

    RVX::SceneECS::SceneCommandReceipt foreignCommand;
    RVX::SceneECS::SceneCommandBufferReceipt foreignSubmission;
    {
        RVX::SceneECS::SceneEcsRuntime source;
        RVX::SceneECS::SceneEcsRuntime target;
        const RVX::ECS::EntityHandle entity = source.CreateEntity();
        RVX::SceneECS::SceneCommandBuffer foreign = source.CreateCommandBuffer();
        foreignCommand = foreign.Add<MutableValueProbe>(entity, {.value = 2});
        foreignSubmission = target.SubmitCommandBuffer(std::move(foreign));
    }
    EXPECT_TRUE(foreignSubmission.IsRejected());
    EXPECT_TRUE(foreignCommand.IsRejected());

    RVX::SceneECS::SceneCommandReceipt queuedCommand;
    RVX::SceneECS::SceneCommandBufferReceipt queuedSubmission;
    {
        RVX::SceneECS::SceneEcsRuntime runtime;
        const RVX::ECS::EntityHandle entity = runtime.CreateEntity();
        RVX::SceneECS::SceneCommandBuffer queued = runtime.CreateCommandBuffer();
        queuedCommand = queued.Add<MutableValueProbe>(entity, {.value = 3});
        queuedSubmission = runtime.SubmitCommandBuffer(
            std::move(queued), RVX::SceneECS::SceneCommandBarrier::PrePresentation);
        ASSERT_TRUE(queuedCommand.IsQueued());
        ASSERT_TRUE(queuedSubmission.IsQueued());
    }
    EXPECT_TRUE(queuedSubmission.IsDiscarded());
    EXPECT_TRUE(queuedCommand.IsDiscarded());
}

TEST(EcsSceneOrchestrationValidation, ProcessorFailureAbortsFrameWithExactDiagnostic)
{
    RVX::SceneECS::SceneEcsRuntime runtime;
    bool laterProcessorRan = false;

    ASSERT_TRUE(runtime.RegisterProcessor({
        .name = "FailingGameplay",
        .phase = RVX::ECS::ProcessorPhase::Gameplay,
        .order = 1,
        .runWithContext = [](RVX::ECS::ProcessorExecutionContext& context)
        {
            context.ReportFailure("physics-side-table rejected stale body");
            context.ReportFailure("must not overwrite the first reason");
        },
    }));
    ASSERT_TRUE(runtime.RegisterProcessor({
        .name = "LaterGameplay",
        .phase = RVX::ECS::ProcessorPhase::Gameplay,
        .order = 2,
        .run = [&laterProcessorRan](RVX::ECS::Registry&) { laterProcessorRan = true; },
    }));

    const RVX::SceneECS::SceneEcsTickResult tick = runtime.Tick({
        .variableDeltaSeconds = 1.0 / 60.0,
    });
    EXPECT_FALSE(tick.succeeded);
    EXPECT_FALSE(laterProcessorRan);
    ASSERT_TRUE(tick.processorFailure.has_value());
    EXPECT_EQ(tick.processorFailure->processorName, "FailingGameplay");
    EXPECT_EQ(tick.processorFailure->phase, RVX::ECS::ProcessorPhase::Gameplay);
    EXPECT_EQ(tick.processorFailure->stepMode, RVX::ECS::ProcessorStepMode::Variable);
    EXPECT_EQ(tick.processorFailure->frameSequence, tick.frameSequence);
    EXPECT_EQ(tick.processorFailure->fixedStepSequence, 0u);
    EXPECT_EQ(tick.processorFailure->reason, "physics-side-table rejected stale body");
}

TEST(EcsSceneOrchestrationValidation, FixedTransformsResolveBeforeSceneToPhysicsAndAfterPhysicsToScene)
{
    RVX::SceneECS::SceneEcsRuntime runtime;
    const RVX::ECS::EntityHandle entity = runtime.CreateEntity();
    ASSERT_TRUE(entity.IsValid());

    float sceneToPhysicsX = 0.0f;
    float fixedResolveX = 0.0f;
    ASSERT_TRUE(runtime.RegisterProcessor({
        .name = "root-motion-local-write",
        .phase = RVX::ECS::ProcessorPhase::RootMotion,
        .stepMode = RVX::ECS::ProcessorStepMode::Fixed,
        .run = [entity](RVX::ECS::Registry& registry)
        {
            EXPECT_TRUE(registry.Write<RVX::SceneECS::LocalTransform>(
                entity,
                [](RVX::SceneECS::LocalTransform& local) { local.translation.x = 5.0f; }));
        },
    }));
    ASSERT_TRUE(runtime.RegisterProcessor({
        .name = "scene-to-physics-observer",
        .phase = RVX::ECS::ProcessorPhase::SceneToPhysics,
        .stepMode = RVX::ECS::ProcessorStepMode::Fixed,
        .run = [entity, &sceneToPhysicsX](RVX::ECS::Registry& registry)
        {
            const auto* transform = registry.TryGet<RVX::SceneECS::SimulationWorldTransform>(entity);
            ASSERT_NE(transform, nullptr);
            sceneToPhysicsX = transform->matrix[3].x;
        },
    }));
    ASSERT_TRUE(runtime.RegisterProcessor({
        .name = "physics-to-scene-local-write",
        .phase = RVX::ECS::ProcessorPhase::PhysicsToScene,
        .stepMode = RVX::ECS::ProcessorStepMode::Fixed,
        .run = [entity](RVX::ECS::Registry& registry)
        {
            EXPECT_TRUE(registry.Write<RVX::SceneECS::LocalTransform>(
                entity,
                [](RVX::SceneECS::LocalTransform& local) { local.translation.x = 7.0f; }));
        },
    }));
    ASSERT_TRUE(runtime.RegisterProcessor({
        .name = "fixed-transform-observer",
        .phase = RVX::ECS::ProcessorPhase::FixedTransformResolve,
        .stepMode = RVX::ECS::ProcessorStepMode::Fixed,
        .run = [entity, &fixedResolveX](RVX::ECS::Registry& registry)
        {
            const auto* transform = registry.TryGet<RVX::SceneECS::SimulationWorldTransform>(entity);
            ASSERT_NE(transform, nullptr);
            fixedResolveX = transform->matrix[3].x;
        },
    }));

    const RVX::SceneECS::SceneEcsTickResult tick = runtime.Tick({
        .fixedDeltaSeconds = 1.0 / 60.0,
        .fixedStepCount = 1,
    });
    ASSERT_TRUE(tick.succeeded);
    EXPECT_GE(tick.prePhysicsFixedTransforms.resolvedCount, 1u);
    EXPECT_GE(tick.fixedTransforms.resolvedCount, 1u);
    EXPECT_FLOAT_EQ(sceneToPhysicsX, 5.0f);
    EXPECT_FLOAT_EQ(fixedResolveX, 7.0f);
}

TEST(EcsSceneOrchestrationValidation, EndFrameCleanupCanAcknowledgeRecordsPublishedInTheSameFrame)
{
    RVX::SceneECS::SceneEcsRuntime runtime;
    const RVX::ECS::EntityHandle entity = runtime.CreateEntity();
    ASSERT_TRUE(entity.IsValid());
    ASSERT_EQ(runtime.RequestDestroy(
                  entity,
                  RVX::SceneECS::ToCleanupDomainMask(RVX::SceneECS::CleanupDomain::Simulation)),
              RVX::SceneECS::DestroyRequestResult::Accepted);

    bool observedPublishedCleanup = false;
    bool acknowledged = false;
    ASSERT_TRUE(runtime.RegisterProcessor({
        .name = "acknowledge-published-cleanup",
        .phase = RVX::ECS::ProcessorPhase::EndFrameCleanup,
        .run = [&runtime, &observedPublishedCleanup, &acknowledged, entity](RVX::ECS::Registry& registry)
        {
            const auto* lifecycle = registry.TryGet<RVX::SceneECS::EntityLifecycleState>(entity);
            observedPublishedCleanup = lifecycle != nullptr &&
                                      lifecycle->phase ==
                                          RVX::SceneECS::EntityLifecyclePhase::CleanupRequired;
            if (observedPublishedCleanup)
            {
                acknowledged = runtime.AcknowledgeCleanup(
                    entity,
                    RVX::SceneECS::ToCleanupDomainMask(RVX::SceneECS::CleanupDomain::Simulation));
            }
        },
    }));

    const RVX::SceneECS::SceneEcsTickResult tick = runtime.Tick();
    EXPECT_TRUE(tick.succeeded);
    EXPECT_TRUE(observedPublishedCleanup);
    EXPECT_TRUE(acknowledged);
    EXPECT_EQ(tick.publishedCleanupRecordCount, 1u);
    EXPECT_EQ(tick.advancedRetirementCount, 1u);
    EXPECT_EQ(tick.recycledEntityCount, 1u);
    EXPECT_FALSE(runtime.GetRegistry().IsAlive(entity));
}

TEST(EcsSceneOrchestrationValidation,
     BeginShutdownClosesRetainedSubmissionPortsButKeepsCleanupTicksAlive)
{
    RVX::SceneECS::SceneEcsRuntime runtime;
    const RVX::ECS::EntityHandle entity = runtime.CreateEntity();
    ASSERT_TRUE(entity.IsValid());

    RVX::SceneECS::SceneCommandSubmissionPort retainedPort =
        runtime.AcquireCommandSubmissionPort();
    RVX::SceneECS::SceneCommandBuffer queued = retainedPort.CreateCommandBuffer();
    RVX::SceneECS::SceneCommandReceipt queuedCommand =
        queued.Add<MutableValueProbe>(entity, {.value = 9});
    RVX::SceneECS::SceneCommandBufferReceipt queuedSubmission =
        retainedPort.SubmitCommandBuffer(
            std::move(queued),
            RVX::SceneECS::SceneCommandBarrier::PrePresentation);
    ASSERT_TRUE(queuedSubmission.IsQueued());
    ASSERT_TRUE(queuedCommand.IsQueued());

    runtime.BeginShutdown();
    EXPECT_FALSE(runtime.IsAcceptingCommandSubmissions());
    EXPECT_FALSE(retainedPort.IsOpen());
    EXPECT_TRUE(queuedSubmission.IsDiscarded());
    EXPECT_TRUE(queuedCommand.IsDiscarded());

    RVX::SceneECS::SceneCommandBuffer late = retainedPort.CreateCommandBuffer();
    RVX::SceneECS::SceneCommandBufferReceipt lateSubmission =
        retainedPort.SubmitCommandBuffer(std::move(late));
    EXPECT_TRUE(lateSubmission.IsRejected());

    ASSERT_EQ(runtime.RequestDestroy(
                  entity,
                  RVX::SceneECS::ToCleanupDomainMask(
                      RVX::SceneECS::CleanupDomain::None)),
              RVX::SceneECS::DestroyRequestResult::Accepted);
    const RVX::SceneECS::SceneEcsTickResult drainTick = runtime.Tick();
    EXPECT_TRUE(drainTick.succeeded);
    EXPECT_EQ(drainTick.recycledEntityCount, 1U);
    EXPECT_FALSE(runtime.GetRegistry().IsAlive(entity));
}
