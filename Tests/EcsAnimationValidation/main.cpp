#include "AnimationSceneAdapters/ECS/AnimationEcsBridge.h"

#include <gtest/gtest.h>

#include <limits>

namespace
{
    using namespace RVX;
    using AnimationSceneAdapters::AnimationEcsEvaluatedPose;

    constexpr uint64 RVX_TEST_SOURCE_MODEL_ASSET_VALUE = 700;
    constexpr int32 RVX_TEST_SOURCE_SKIN_INDEX = 3;

    SceneECS::SceneEcsTickResult TickFixed(SceneECS::SceneEcsRuntime& runtime,
                                           uint32 fixedStepCount = 1)
    {
        return runtime.Tick({
            .variableDeltaSeconds = 1.0 / 60.0,
            .fixedDeltaSeconds = 1.0 / 60.0,
            .fixedStepCount = fixedStepCount,
        });
    }

    AnimationEcsEvaluatedPose MakePose(uint32 boneCount, uint64 rootMotionSequence = 0)
    {
        AnimationEcsEvaluatedPose pose;
        pose.skinningPalette.assign(boneCount, Mat4(1.0f));
        pose.rootMotionSequence = rootMotionSequence;
        return pose;
    }

    ECS::EntityHandle AddAnimatedEntity(SceneECS::SceneEcsRuntime& runtime,
                                        SceneECS::AnimationEvaluationMode mode,
                                        bool rootMotion = false,
                                        bool playing = true)
    {
        const ECS::EntityHandle entity = runtime.CreateEntity();
        if (!entity.IsValid())
        {
            return ECS::EntityHandle::Invalid();
        }

        SceneECS::AnimationSkeletonBinding skeleton;
        skeleton.animationAssetValue = 42;
        skeleton.sourceModelAssetValue = RVX_TEST_SOURCE_MODEL_ASSET_VALUE;
        skeleton.sourceSkinIndex = RVX_TEST_SOURCE_SKIN_INDEX;
        skeleton.boneCount = 2;
        SceneECS::Animator animator;
        animator.evaluationMode = mode;
        animator.playing = playing;
        animator.rootMotionEnabled = rootMotion;
        if (!runtime.AddFragment<SceneECS::AnimationSkeletonBinding>(entity, skeleton) ||
            !runtime.AddFragment<SceneECS::Animator>(entity, animator) ||
            !runtime.AddFragment<SceneECS::AnimationPoseState>(entity) ||
            (rootMotion && !runtime.AddFragment<SceneECS::RootMotionIntent>(entity)))
        {
            return ECS::EntityHandle::Invalid();
        }
        return entity;
    }

    ECS::EntityHandle AddSkinnedMesh(SceneECS::SceneEcsRuntime& runtime,
                                     ECS::EntityHandle poseEntity)
    {
        const ECS::EntityHandle entity = runtime.CreateEntity();
        if (!entity.IsValid())
        {
            return ECS::EntityHandle::Invalid();
        }

        SceneECS::Mesh mesh;
        mesh.meshAssetId = {.value = 101};
        mesh.submeshCount = 1;
        SceneECS::MaterialSlots materials;
        materials.count = 1;
        materials.values[0].materialAssetId = {.value = 201};
        SceneECS::SkinnedMeshBinding binding;
        binding.poseEntity = poseEntity;
        binding.sourceModelAssetValue = RVX_TEST_SOURCE_MODEL_ASSET_VALUE;
        binding.sourceSkinIndex = RVX_TEST_SOURCE_SKIN_INDEX;
        if (!runtime.AddFragment<SceneECS::Mesh>(entity, mesh) ||
            !runtime.AddFragment<SceneECS::MaterialSlots>(entity, materials) ||
            !runtime.AddFragment<SceneECS::Visibility>(entity) ||
            !runtime.AddFragment<SceneECS::Camera>(entity) ||
            !runtime.AddFragment<SceneECS::SkinnedMeshBinding>(entity, binding))
        {
            return ECS::EntityHandle::Invalid();
        }
        return entity;
    }

    [[nodiscard]] const SceneECS::FrozenSceneMesh* FindFrozenMesh(
        const SceneECS::FrozenSceneSnapshot& snapshot,
        ECS::EntityHandle entity)
    {
        for (const SceneECS::FrozenSceneMesh& mesh : snapshot.meshes)
        {
            if (mesh.sourceEntity == entity)
            {
                return &mesh;
            }
        }
        return nullptr;
    }
} // namespace

TEST(EcsAnimationValidation, PausedAnimatorInitializesOnceInVariableGameplayForEveryMode)
{
    for (const SceneECS::AnimationEvaluationMode mode :
         {SceneECS::AnimationEvaluationMode::VariablePrePhysics,
          SceneECS::AnimationEvaluationMode::Fixed})
    {
        SCOPED_TRACE(static_cast<uint32>(mode));
        SceneECS::SceneEcsRuntime runtime;
        uint32 calls = 0;
        AnimationSceneAdapters::AnimationEcsBridge bridge(
            runtime,
            [&calls](const AnimationSceneAdapters::AnimationEcsEvaluationRequest& request)
            {
                ++calls;
                EXPECT_FALSE(request.animator.playing);
                EXPECT_DOUBLE_EQ(request.deltaSeconds, 0.0);
                EXPECT_EQ(request.fixedStepSequence, 0u);
                return MakePose(request.skeleton.boneCount, request.nextPoseSequence);
            });
        ASSERT_EQ(bridge.RegisterProcessors(),
                  AnimationSceneAdapters::AnimationEcsBridgeRegistrationResult::Registered);
        const ECS::EntityHandle entity = AddAnimatedEntity(runtime, mode, true, false);
        ASSERT_TRUE(entity.IsValid());

        ASSERT_TRUE(TickFixed(runtime, 0).succeeded);
        EXPECT_EQ(calls, 1u);
        const auto pose = bridge.GetPoseSnapshot(runtime.GetSceneRuntimeId(), entity);
        ASSERT_TRUE(pose.has_value());
        EXPECT_EQ(pose->poseSequence, 1u);
        EXPECT_EQ(pose->skinningPalette.size(), 2u);
        const auto* poseState =
            runtime.GetRegistry().TryGet<SceneECS::AnimationPoseState>(entity);
        ASSERT_NE(poseState, nullptr);
        EXPECT_NE(poseState->lastVariableFrameSequence, 0u);
        EXPECT_EQ(poseState->lastFixedStepSequence, 0u);
        const auto* intent = runtime.GetRegistry().TryGet<SceneECS::RootMotionIntent>(entity);
        ASSERT_NE(intent, nullptr);
        EXPECT_FALSE(intent->pending);
        EXPECT_FALSE(intent->sequenceRejected);
        const auto firstDiagnostics = bridge.GetDiagnosticsSnapshot();
        EXPECT_EQ(firstDiagnostics.variableEvaluationCount, 1u);
        EXPECT_EQ(firstDiagnostics.fixedEvaluationCount, 0u);
        EXPECT_EQ(firstDiagnostics.rootMotionPublicationCount, 0u);

        ASSERT_TRUE(TickFixed(runtime, 0).succeeded);
        EXPECT_EQ(calls, 1u);
        const auto retained = bridge.GetPoseSnapshot(runtime.GetSceneRuntimeId(), entity);
        ASSERT_TRUE(retained.has_value());
        EXPECT_EQ(retained->poseSequence, 1u);
        const auto retainedDiagnostics = bridge.GetDiagnosticsSnapshot();
        EXPECT_EQ(retainedDiagnostics.variableEvaluationCount, 1u);
        EXPECT_EQ(retainedDiagnostics.fixedEvaluationCount, 0u);
        EXPECT_EQ(retainedDiagnostics.rootMotionPublicationCount, 0u);
    }
}

TEST(EcsAnimationValidation, PausedEvaluationFailureSurvivesBeforeFixedReconcileAndRetries)
{
    SceneECS::SceneEcsRuntime runtime;
    bool reject = true;
    uint32 calls = 0;
    AnimationSceneAdapters::AnimationEcsBridge bridge(
        runtime,
        [&reject, &calls](const AnimationSceneAdapters::AnimationEcsEvaluationRequest& request)
            -> std::optional<AnimationSceneAdapters::AnimationEcsEvaluatedPose>
        {
            ++calls;
            EXPECT_FALSE(request.animator.playing);
            EXPECT_DOUBLE_EQ(request.deltaSeconds, 0.0);
            if (reject)
            {
                return std::nullopt;
            }
            return MakePose(request.skeleton.boneCount, request.nextPoseSequence);
        });
    ASSERT_EQ(bridge.RegisterProcessors(),
              AnimationSceneAdapters::AnimationEcsBridgeRegistrationResult::Registered);
    const ECS::EntityHandle entity = AddAnimatedEntity(
        runtime, SceneECS::AnimationEvaluationMode::Fixed, true, false);
    ASSERT_TRUE(entity.IsValid());

    // Gameplay rejects the one-shot initial pose. BeforeFixed reconciliation
    // in the same tick must not overwrite that failure with a false Active.
    ASSERT_TRUE(TickFixed(runtime, 1).succeeded);
    EXPECT_EQ(calls, 1u);
    const auto* rejected =
        runtime.GetRegistry().TryGet<SceneECS::AnimationPoseState>(entity);
    ASSERT_NE(rejected, nullptr);
    EXPECT_EQ(rejected->status, SceneECS::AnimationBindingStatus::EvaluatorRejected);
    EXPECT_FALSE(bridge.GetPoseSnapshot(runtime.GetSceneRuntimeId(), entity).has_value());

    reject = false;
    ASSERT_TRUE(TickFixed(runtime, 0).succeeded);
    EXPECT_EQ(calls, 2u);
    const auto recovered = bridge.GetPoseSnapshot(runtime.GetSceneRuntimeId(), entity);
    ASSERT_TRUE(recovered.has_value());
    EXPECT_EQ(recovered->poseSequence, 1u);
}

TEST(EcsAnimationValidation, SameBoneCountSkeletonWriteInvalidatesAndRebuildsPausedPalette)
{
    SceneECS::SceneEcsRuntime runtime;
    bool reject = false;
    uint32 calls = 0;
    AnimationSceneAdapters::AnimationEcsBridge bridge(
        runtime,
        [&reject, &calls](const AnimationSceneAdapters::AnimationEcsEvaluationRequest& request)
            -> std::optional<AnimationSceneAdapters::AnimationEcsEvaluatedPose>
        {
            ++calls;
            if (reject)
            {
                return std::nullopt;
            }
            AnimationSceneAdapters::AnimationEcsEvaluatedPose pose =
                MakePose(request.skeleton.boneCount, request.nextPoseSequence);
            pose.skinningPalette.front()[0][0] =
                static_cast<float32>(request.skeleton.animationAssetValue);
            return pose;
        });
    ASSERT_EQ(bridge.RegisterProcessors(),
              AnimationSceneAdapters::AnimationEcsBridgeRegistrationResult::Registered);
    const ECS::EntityHandle entity = AddAnimatedEntity(
        runtime, SceneECS::AnimationEvaluationMode::Fixed, false, false);
    ASSERT_TRUE(entity.IsValid());
    const ECS::EntityHandle mesh = AddSkinnedMesh(runtime, entity);
    ASSERT_TRUE(mesh.IsValid());
    ASSERT_TRUE(TickFixed(runtime, 0).succeeded);
    ASSERT_EQ(calls, 1u);
    EXPECT_EQ(runtime.GetSkinningSnapshotCounts().paletteCount, 1u);
    const auto first = bridge.GetPoseSnapshot(runtime.GetSceneRuntimeId(), entity);
    ASSERT_TRUE(first.has_value());

    const SceneECS::AnimationSkeletonBinding* current =
        runtime.GetRegistry().TryGet<SceneECS::AnimationSkeletonBinding>(entity);
    ASSERT_NE(current, nullptr);
    SceneECS::AnimationSkeletonBinding replacement = *current;
    ++replacement.animationAssetValue;
    ASSERT_TRUE(runtime.SetFragment(entity, replacement));

    // Same bone count is not sufficient identity: the old palette is hidden
    // immediately and cannot escape before the replacement is evaluated.
    EXPECT_FALSE(bridge.GetPoseSnapshot(runtime.GetSceneRuntimeId(), entity).has_value());
    reject = true;
    const SceneECS::SceneEcsTickResult rejectedTick = TickFixed(runtime, 0);
    EXPECT_FALSE(rejectedTick.succeeded);
    EXPECT_EQ(calls, 2u);
    EXPECT_EQ(runtime.GetSkinningSnapshotCounts().paletteCount, 0u);
    EXPECT_FALSE(bridge.GetPoseSnapshot(runtime.GetSceneRuntimeId(), entity).has_value());

    reject = false;
    ASSERT_TRUE(TickFixed(runtime, 0).succeeded);
    EXPECT_EQ(calls, 3u);
    EXPECT_EQ(runtime.GetSkinningSnapshotCounts().paletteCount, 1u);
    const auto rebuilt = bridge.GetPoseSnapshot(runtime.GetSceneRuntimeId(), entity);
    ASSERT_TRUE(rebuilt.has_value());
    EXPECT_EQ(rebuilt->poseSequence, first->poseSequence + 1u);
    EXPECT_FLOAT_EQ(rebuilt->skinningPalette.front()[0][0],
                    static_cast<float32>(replacement.animationAssetValue));

    ASSERT_TRUE(TickFixed(runtime, 0).succeeded);
    EXPECT_EQ(calls, 3u);

    ASSERT_TRUE(runtime.SetFragmentEnabled<SceneECS::Animator>(entity, false));
    const SceneECS::SceneEcsTickResult disabledTick = TickFixed(runtime, 0);
    EXPECT_FALSE(disabledTick.succeeded);
    EXPECT_EQ(runtime.GetSkinningSnapshotCounts().paletteCount, 0u);

    ASSERT_TRUE(runtime.SetFragmentEnabled<SceneECS::Animator>(entity, true));
    ASSERT_TRUE(TickFixed(runtime, 0).succeeded);
    EXPECT_EQ(calls, 4u);
    EXPECT_EQ(runtime.GetSkinningSnapshotCounts().paletteCount, 1u);
}

TEST(EcsAnimationValidation,
     BridgeGeneratedRootStreamIgnoresPausedPoseRepairAndRetainsPendingIntent)
{
    SceneECS::SceneEcsRuntime runtime;
    uint32 calls = 0;
    AnimationSceneAdapters::AnimationEcsBridge bridge(
        runtime,
        [&calls](const AnimationSceneAdapters::AnimationEcsEvaluationRequest& request)
        {
            ++calls;
            return MakePose(request.skeleton.boneCount, 0u);
        },
        [](ECS::SceneRuntimeId, ECS::EntityHandle, uint64 handle)
        {
            return handle == 77u;
        });
    ASSERT_EQ(bridge.RegisterProcessors(),
              AnimationSceneAdapters::AnimationEcsBridgeRegistrationResult::Registered);
    const ECS::EntityHandle entity = AddAnimatedEntity(
        runtime, SceneECS::AnimationEvaluationMode::Fixed, true, true);
    ASSERT_TRUE(entity.IsValid());

    // Reconcile/evaluate once before Physics binding, then establish the exact
    // body epoch. The bridge-generated transport stream starts at one even
    // though the palette pose sequence has already advanced.
    ASSERT_TRUE(TickFixed(runtime).succeeded);
    ASSERT_EQ(bridge.BindRootMotionPhysicsBody(runtime.GetSceneRuntimeId(), entity, 77u),
              AnimationSceneAdapters::AnimationRootMotionPhysicsBindingResult::Applied);
    ASSERT_TRUE(TickFixed(runtime).succeeded);
    const auto* firstIntent =
        runtime.GetRegistry().TryGet<SceneECS::RootMotionIntent>(entity);
    ASSERT_NE(firstIntent, nullptr);
    ASSERT_TRUE(firstIntent->pending);
    EXPECT_EQ(firstIntent->sourcePoseSequence, 2u);
    EXPECT_EQ(firstIntent->rootMotionSequence, 1u);
    const uint32 callsAfterFirstIntent = calls;

    // A retained Physics retry owns this exact value. Animation must not
    // advance or overwrite it in a later fixed step.
    ASSERT_TRUE(TickFixed(runtime).succeeded);
    EXPECT_EQ(calls, callsAfterFirstIntent);
    const auto* retainedIntent =
        runtime.GetRegistry().TryGet<SceneECS::RootMotionIntent>(entity);
    ASSERT_NE(retainedIntent, nullptr);
    EXPECT_TRUE(retainedIntent->pending);
    EXPECT_EQ(retainedIntent->sourcePoseSequence, 2u);
    EXPECT_EQ(retainedIntent->rootMotionSequence, 1u);

    SceneECS::RootMotionIntent consumed = *retainedIntent;
    consumed.pending = false;
    ASSERT_TRUE(runtime.SetFragment(entity, consumed));
    const SceneECS::Animator* currentAnimator =
        runtime.GetRegistry().TryGet<SceneECS::Animator>(entity);
    const SceneECS::AnimationSkeletonBinding* currentSkeleton =
        runtime.GetRegistry().TryGet<SceneECS::AnimationSkeletonBinding>(entity);
    ASSERT_NE(currentAnimator, nullptr);
    ASSERT_NE(currentSkeleton, nullptr);
    SceneECS::Animator paused = *currentAnimator;
    paused.playing = false;
    SceneECS::AnimationSkeletonBinding replacement = *currentSkeleton;
    ++replacement.animationAssetValue;
    ASSERT_TRUE(runtime.SetFragment(entity, paused));
    ASSERT_TRUE(runtime.SetFragment(entity, replacement));
    ASSERT_TRUE(TickFixed(runtime, 0).succeeded);

    paused.playing = true;
    ASSERT_TRUE(runtime.SetFragment(entity, paused));
    ASSERT_TRUE(TickFixed(runtime).succeeded);
    const auto* secondIntent =
        runtime.GetRegistry().TryGet<SceneECS::RootMotionIntent>(entity);
    ASSERT_NE(secondIntent, nullptr);
    EXPECT_TRUE(secondIntent->pending);
    EXPECT_EQ(secondIntent->sourcePoseSequence, 4u);
    EXPECT_EQ(secondIntent->rootMotionSequence, 2u);
    const auto diagnostics = bridge.GetDiagnosticsSnapshot();
    EXPECT_EQ(diagnostics.rootMotionPublicationCount, 2u);
    EXPECT_EQ(diagnostics.rootMotionReplayCount, 0u);
    EXPECT_EQ(diagnostics.rootMotionGapCount, 0u);
}

TEST(EcsAnimationValidation,
     RebindingRootMotionBodyDoesNotBackpressureTheNewExactBodyEpoch)
{
    SceneECS::SceneEcsRuntime runtime;
    uint32 calls = 0;
    AnimationSceneAdapters::AnimationEcsBridge bridge(
        runtime,
        [&calls](const AnimationSceneAdapters::AnimationEcsEvaluationRequest& request)
        {
            ++calls;
            return MakePose(request.skeleton.boneCount, calls == 1u ? 7u : 41u);
        },
        [](ECS::SceneRuntimeId, ECS::EntityHandle, uint64 handle)
        {
            return handle == 77u || handle == 88u;
        });
    ASSERT_EQ(bridge.RegisterProcessors(),
              AnimationSceneAdapters::AnimationEcsBridgeRegistrationResult::Registered);
    const ECS::EntityHandle entity = AddAnimatedEntity(
        runtime, SceneECS::AnimationEvaluationMode::Fixed, true, true);
    ASSERT_TRUE(entity.IsValid());

    // Reconcile without a fixed evaluation so body A owns the first intent.
    ASSERT_TRUE(TickFixed(runtime, 0).succeeded);
    ASSERT_EQ(bridge.BindRootMotionPhysicsBody(runtime.GetSceneRuntimeId(), entity, 77u),
              AnimationSceneAdapters::AnimationRootMotionPhysicsBindingResult::Applied);
    ASSERT_TRUE(TickFixed(runtime).succeeded);
    const auto* firstIntent = runtime.GetRegistry().TryGet<SceneECS::RootMotionIntent>(entity);
    ASSERT_NE(firstIntent, nullptr);
    ASSERT_TRUE(firstIntent->pending);
    EXPECT_EQ(firstIntent->physicsBodyHandlePacked, 77u);
    EXPECT_EQ(firstIntent->sourcePoseSequence, 1u);
    EXPECT_EQ(firstIntent->rootMotionSequence, 7u);

    // Do not mutate the A-owned pending fragment. Binding B alone must start
    // a new root-motion-stream baseline rather than deadlocking evaluation.
    ASSERT_EQ(bridge.BindRootMotionPhysicsBody(runtime.GetSceneRuntimeId(), entity, 88u),
              AnimationSceneAdapters::AnimationRootMotionPhysicsBindingResult::Applied);
    ASSERT_TRUE(TickFixed(runtime).succeeded);
    const auto* reboundIntent = runtime.GetRegistry().TryGet<SceneECS::RootMotionIntent>(entity);
    ASSERT_NE(reboundIntent, nullptr);
    EXPECT_TRUE(reboundIntent->pending);
    EXPECT_FALSE(reboundIntent->sequenceRejected);
    EXPECT_EQ(reboundIntent->physicsBodyHandlePacked, 88u);
    EXPECT_EQ(reboundIntent->sourcePoseSequence, 2u);
    EXPECT_EQ(reboundIntent->rootMotionSequence, 41u);
    EXPECT_EQ(reboundIntent->fixedStepSequence, 2u);
    const auto diagnostics = bridge.GetDiagnosticsSnapshot();
    EXPECT_EQ(calls, 2u);
    EXPECT_EQ(diagnostics.fixedEvaluationCount, 2u);
    EXPECT_EQ(diagnostics.rootMotionPublicationCount, 2u);
    EXPECT_EQ(diagnostics.rootMotionSupersededEpochCount, 1u);
    EXPECT_EQ(diagnostics.rootMotionReplayCount, 0u);
    EXPECT_EQ(diagnostics.rootMotionGapCount, 0u);
}

TEST(EcsAnimationValidation,
     PausedPoseOwnerUnblocksSnapshotAndBindingEpochKeepsRootMotionStrict)
{
    SceneECS::SceneEcsRuntime runtime;
    ECS::EntityHandle motionDriver = ECS::EntityHandle::Invalid();
    uint32 pausedDriverCalls = 0;
    uint32 playingDriverCalls = 0;
    AnimationSceneAdapters::AnimationEcsBridge bridge(
        runtime,
        [&motionDriver, &pausedDriverCalls, &playingDriverCalls](
            const AnimationSceneAdapters::AnimationEcsEvaluationRequest& request)
        {
            if (request.entity != motionDriver)
            {
                return MakePose(request.skeleton.boneCount, request.nextPoseSequence);
            }
            if (!request.animator.playing)
            {
                ++pausedDriverCalls;
                EXPECT_DOUBLE_EQ(request.deltaSeconds, 0.0);
                EXPECT_EQ(request.fixedStepSequence, 0u);
                return MakePose(request.skeleton.boneCount, request.nextPoseSequence);
            }

            const uint32 sequenceIndex = playingDriverCalls++;
            if (sequenceIndex == 0u || sequenceIndex == 1u)
            {
                return MakePose(request.skeleton.boneCount, 7u);
            }
            if (sequenceIndex == 2u)
            {
                return MakePose(request.skeleton.boneCount, 9u);
            }
            return MakePose(request.skeleton.boneCount, 10u + sequenceIndex);
        },
        [](ECS::SceneRuntimeId, ECS::EntityHandle, uint64 physicsBodyHandlePacked)
        {
            return physicsBodyHandlePacked == 77u;
        });
    ASSERT_EQ(bridge.RegisterProcessors(),
              AnimationSceneAdapters::AnimationEcsBridgeRegistrationResult::Registered);

    const ECS::EntityHandle modelPoseOwner = AddAnimatedEntity(
        runtime, SceneECS::AnimationEvaluationMode::VariablePrePhysics);
    const ECS::EntityHandle mesh = AddSkinnedMesh(runtime, modelPoseOwner);
    ASSERT_TRUE(modelPoseOwner.IsValid());
    ASSERT_TRUE(mesh.IsValid());
    ASSERT_TRUE(TickFixed(runtime, 0).succeeded);
    ASSERT_NE(runtime.GetLatestFrozenSnapshot(), nullptr);
    const SceneECS::FrozenSceneMesh* initialMesh =
        FindFrozenMesh(*runtime.GetLatestFrozenSnapshot(), mesh);
    ASSERT_NE(initialMesh, nullptr);
    ASSERT_TRUE(initialMesh->skinningPalette.has_value());
    EXPECT_EQ(initialMesh->skinningPalette->poseEntity, modelPoseOwner);

    const SceneECS::Animator* const modelAnimator =
        runtime.GetRegistry().TryGet<SceneECS::Animator>(modelPoseOwner);
    ASSERT_NE(modelAnimator, nullptr);
    SceneECS::Animator pausedModelAnimator = *modelAnimator;
    pausedModelAnimator.playing = false;
    ASSERT_TRUE(runtime.SetFragment(modelPoseOwner, pausedModelAnimator));
    motionDriver = AddAnimatedEntity(
        runtime, SceneECS::AnimationEvaluationMode::Fixed, true, false);
    ASSERT_TRUE(motionDriver.IsValid());
    const SceneECS::SceneEntityRef meshMembers[] = {runtime.GetEntityRef(mesh)};
    const SceneECS::SkinnedMeshPoseRebindReceipt rebind =
        runtime.RebindSkinnedMeshPoseOwner({
            .expectedSceneRuntimeId = runtime.GetSceneRuntimeId(),
            .meshMembers = meshMembers,
            .expectedPoseOwner = runtime.GetEntityRef(modelPoseOwner),
            .newPoseOwner = runtime.GetEntityRef(motionDriver),
            .sourceModelAssetValue = RVX_TEST_SOURCE_MODEL_ASSET_VALUE,
            .sourceSkinIndex = RVX_TEST_SOURCE_SKIN_INDEX,
        });
    ASSERT_TRUE(rebind.IsApplied());
    ASSERT_EQ(rebind.meshCount, 1u);

    ASSERT_TRUE(TickFixed(runtime, 0).succeeded);
    EXPECT_EQ(pausedDriverCalls, 1u);
    EXPECT_EQ(playingDriverCalls, 0u);
    ASSERT_NE(runtime.GetLatestFrozenSnapshot(), nullptr);
    const SceneECS::FrozenSceneMesh* reboundMesh =
        FindFrozenMesh(*runtime.GetLatestFrozenSnapshot(), mesh);
    ASSERT_NE(reboundMesh, nullptr);
    ASSERT_TRUE(reboundMesh->skinningPalette.has_value());
    EXPECT_EQ(reboundMesh->skinningPalette->poseEntity, motionDriver);
    const auto* initialPoseState =
        runtime.GetRegistry().TryGet<SceneECS::AnimationPoseState>(motionDriver);
    ASSERT_NE(initialPoseState, nullptr);
    EXPECT_EQ(initialPoseState->poseSequence, 1u);
    EXPECT_EQ(initialPoseState->lastFixedStepSequence, 0u);
    const auto* initialIntent =
        runtime.GetRegistry().TryGet<SceneECS::RootMotionIntent>(motionDriver);
    ASSERT_NE(initialIntent, nullptr);
    EXPECT_FALSE(initialIntent->pending);
    EXPECT_EQ(bridge.GetDiagnosticsSnapshot().fixedEvaluationCount, 0u);

    ASSERT_TRUE(TickFixed(runtime, 0).succeeded);
    EXPECT_EQ(pausedDriverCalls, 1u);
    const auto pausedPose = bridge.GetPoseSnapshot(runtime.GetSceneRuntimeId(), motionDriver);
    ASSERT_TRUE(pausedPose.has_value());
    EXPECT_EQ(pausedPose->poseSequence, 1u);

    ASSERT_EQ(bridge.BindRootMotionPhysicsBody(runtime.GetSceneRuntimeId(), motionDriver, 77u),
              AnimationSceneAdapters::AnimationRootMotionPhysicsBindingResult::Applied);
    const SceneECS::Animator* const currentAnimator =
        runtime.GetRegistry().TryGet<SceneECS::Animator>(motionDriver);
    ASSERT_NE(currentAnimator, nullptr);
    SceneECS::Animator playingAnimator = *currentAnimator;
    playingAnimator.playing = true;
    ASSERT_TRUE(runtime.SetFragment(motionDriver, playingAnimator));

    ASSERT_TRUE(TickFixed(runtime).succeeded);
    EXPECT_EQ(playingDriverCalls, 1u);
    const auto* firstIntent =
        runtime.GetRegistry().TryGet<SceneECS::RootMotionIntent>(motionDriver);
    ASSERT_NE(firstIntent, nullptr);
    EXPECT_TRUE(firstIntent->pending);
    EXPECT_FALSE(firstIntent->sequenceRejected);
    EXPECT_EQ(firstIntent->sourcePoseSequence, 2u);
    EXPECT_EQ(firstIntent->rootMotionSequence, 7u);
    EXPECT_EQ(bridge.GetDiagnosticsSnapshot().rootMotionPublicationCount, 1u);

    SceneECS::RootMotionIntent consumedFirst = *firstIntent;
    consumedFirst.pending = false;
    ASSERT_TRUE(runtime.SetFragment(motionDriver, consumedFirst));

    ASSERT_EQ(bridge.BindRootMotionPhysicsBody(runtime.GetSceneRuntimeId(), motionDriver, 77u),
              AnimationSceneAdapters::AnimationRootMotionPhysicsBindingResult::Applied);
    ASSERT_TRUE(TickFixed(runtime).succeeded);
    const auto* replay = runtime.GetRegistry().TryGet<SceneECS::RootMotionIntent>(motionDriver);
    ASSERT_NE(replay, nullptr);
    EXPECT_FALSE(replay->pending);
    EXPECT_TRUE(replay->sequenceRejected);
    EXPECT_EQ(replay->sourcePoseSequence, 3u);
    EXPECT_EQ(replay->rootMotionSequence, 7u);

    ASSERT_TRUE(TickFixed(runtime).succeeded);
    const auto* gap = runtime.GetRegistry().TryGet<SceneECS::RootMotionIntent>(motionDriver);
    ASSERT_NE(gap, nullptr);
    EXPECT_FALSE(gap->pending);
    EXPECT_TRUE(gap->sequenceRejected);
    EXPECT_EQ(gap->sourcePoseSequence, 4u);
    EXPECT_EQ(gap->rootMotionSequence, 9u);
    const auto diagnostics = bridge.GetDiagnosticsSnapshot();
    EXPECT_EQ(diagnostics.fixedEvaluationCount, 3u);
    EXPECT_EQ(diagnostics.rootMotionPublicationCount, 1u);
    EXPECT_EQ(diagnostics.rootMotionReplayCount, 1u);
    EXPECT_EQ(diagnostics.rootMotionGapCount, 1u);
}

TEST(EcsAnimationValidation, FixedAnimationEvaluatesExactlyOncePerFixedStep)
{
    SceneECS::SceneEcsRuntime runtime;
    uint32 fixedCalls = 0;
    uint32 variableCalls = 0;
    AnimationSceneAdapters::AnimationEcsBridge bridge(
        runtime,
        [&fixedCalls, &variableCalls](const AnimationSceneAdapters::AnimationEcsEvaluationRequest& request)
        {
            if (request.fixedStepSequence != 0)
            {
                ++fixedCalls;
            }
            else
            {
                ++variableCalls;
            }
            return MakePose(request.skeleton.boneCount, request.nextPoseSequence);
        });
    ASSERT_EQ(bridge.RegisterProcessors(),
              AnimationSceneAdapters::AnimationEcsBridgeRegistrationResult::Registered);
    const ECS::EntityHandle entity = AddAnimatedEntity(
        runtime, SceneECS::AnimationEvaluationMode::Fixed);
    ASSERT_TRUE(entity.IsValid());

    ASSERT_TRUE(runtime.Tick().succeeded);
    EXPECT_EQ(variableCalls, 0u);
    EXPECT_EQ(fixedCalls, 0u);

    ASSERT_TRUE(TickFixed(runtime, 2).succeeded);
    EXPECT_EQ(fixedCalls, 2u);
    const auto pose = bridge.GetPoseSnapshot(runtime.GetSceneRuntimeId(), entity);
    ASSERT_TRUE(pose.has_value());
    EXPECT_EQ(pose->poseSequence, 2u);
    EXPECT_EQ(pose->skinningPalette.size(), 2u);

    ASSERT_TRUE(runtime.Tick().succeeded);
    EXPECT_EQ(fixedCalls, 2u);
    EXPECT_EQ(bridge.GetDiagnosticsSnapshot().fixedEvaluationCount, 2u);
}

TEST(EcsAnimationValidation, FailedPoseCommitRetainsLastAtomicPalettePublication)
{
    SceneECS::SceneEcsRuntime runtime;
    uint32 calls = 0;
    AnimationSceneAdapters::AnimationEcsBridge bridge(
        runtime,
        [&calls](const AnimationSceneAdapters::AnimationEcsEvaluationRequest& request)
        {
            ++calls;
            AnimationEcsEvaluatedPose pose = MakePose(
                request.skeleton.boneCount, request.nextPoseSequence);
            if (calls == 2)
            {
                pose.skinningPalette[0][0][0] = std::numeric_limits<float>::quiet_NaN();
            }
            return pose;
        });
    ASSERT_EQ(bridge.RegisterProcessors(),
              AnimationSceneAdapters::AnimationEcsBridgeRegistrationResult::Registered);
    const ECS::EntityHandle entity = AddAnimatedEntity(
        runtime, SceneECS::AnimationEvaluationMode::Fixed);

    ASSERT_TRUE(TickFixed(runtime).succeeded);
    const auto first = bridge.GetPoseSnapshot(runtime.GetSceneRuntimeId(), entity);
    ASSERT_TRUE(first.has_value());
    ASSERT_EQ(first->poseSequence, 1u);
    ASSERT_EQ(first->skinningPalette.size(), 2u);
    const Mat4 firstMatrix = first->skinningPalette.front();

    ASSERT_TRUE(TickFixed(runtime).succeeded);
    const auto retained = bridge.GetPoseSnapshot(runtime.GetSceneRuntimeId(), entity);
    ASSERT_TRUE(retained.has_value());
    EXPECT_EQ(retained->poseSequence, 1u);
    EXPECT_EQ(retained->paletteRevision, first->paletteRevision);
    EXPECT_FLOAT_EQ(retained->skinningPalette.front()[0][0], firstMatrix[0][0]);
    const auto* state = runtime.GetRegistry().TryGet<SceneECS::AnimationPoseState>(entity);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->status, SceneECS::AnimationBindingStatus::EvaluatorRejected);
    EXPECT_EQ(bridge.GetDiagnosticsSnapshot().rejectedEvaluationCount, 1u);
}

TEST(EcsAnimationValidation, ForeignStaleEntityAndExactPhysicsBindingAreRejected)
{
    SceneECS::SceneEcsRuntime runtime;
    bool physicsBindingAlive = true;
    AnimationSceneAdapters::AnimationEcsBridge bridge(
        runtime,
        {},
        [&physicsBindingAlive](ECS::SceneRuntimeId,
                               ECS::EntityHandle,
                               uint64 physicsBodyHandlePacked)
        {
            return physicsBindingAlive && physicsBodyHandlePacked == 100;
        });
    ASSERT_EQ(bridge.RegisterProcessors(),
              AnimationSceneAdapters::AnimationEcsBridgeRegistrationResult::Registered);
    const ECS::EntityHandle entity = AddAnimatedEntity(
        runtime, SceneECS::AnimationEvaluationMode::Fixed, true);
    ASSERT_TRUE(runtime.Tick().succeeded);

    EXPECT_EQ(bridge.BindRootMotionPhysicsBody(ECS::SceneRuntimeId(999), entity, 10),
              AnimationSceneAdapters::AnimationRootMotionPhysicsBindingResult::ForeignScene);
    EXPECT_EQ(bridge.BindRootMotionPhysicsBody(runtime.GetSceneRuntimeId(),
                                               ECS::EntityHandle::Invalid(), 10),
              AnimationSceneAdapters::AnimationRootMotionPhysicsBindingResult::InvalidEntity);
    EXPECT_EQ(bridge.BindRootMotionPhysicsBody(runtime.GetSceneRuntimeId(), entity, 0),
              AnimationSceneAdapters::AnimationRootMotionPhysicsBindingResult::InvalidPhysicsHandle);
    ASSERT_TRUE(runtime.SetFragmentEnabled<SceneECS::Animator>(entity, false));
    EXPECT_EQ(bridge.BindRootMotionPhysicsBody(runtime.GetSceneRuntimeId(), entity, 100),
              AnimationSceneAdapters::AnimationRootMotionPhysicsBindingResult::AnimationEntityNotBound);
    ASSERT_TRUE(runtime.SetFragmentEnabled<SceneECS::Animator>(entity, true));
    ASSERT_EQ(bridge.BindRootMotionPhysicsBody(runtime.GetSceneRuntimeId(), entity, 100),
              AnimationSceneAdapters::AnimationRootMotionPhysicsBindingResult::Applied);
    EXPECT_EQ(bridge.UnbindRootMotionPhysicsBody(runtime.GetSceneRuntimeId(), entity, 101),
              AnimationSceneAdapters::AnimationRootMotionPhysicsBindingResult::BindingMismatch);
    physicsBindingAlive = false;
    ASSERT_TRUE(TickFixed(runtime).succeeded);
    const auto* staleIntent = runtime.GetRegistry().TryGet<SceneECS::RootMotionIntent>(entity);
    ASSERT_NE(staleIntent, nullptr);
    EXPECT_FALSE(staleIntent->pending);
    const auto* staleState = runtime.GetRegistry().TryGet<SceneECS::AnimationPoseState>(entity);
    ASSERT_NE(staleState, nullptr);
    EXPECT_EQ(staleState->status, SceneECS::AnimationBindingStatus::InvalidPhysicsBinding);
    EXPECT_EQ(bridge.UnbindRootMotionPhysicsBody(runtime.GetSceneRuntimeId(), entity, 100),
              AnimationSceneAdapters::AnimationRootMotionPhysicsBindingResult::Applied);

    ASSERT_EQ(runtime.RequestDestroy(
                  entity, SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Animation)),
              SceneECS::DestroyRequestResult::Accepted);
    ASSERT_TRUE(runtime.GetRegistry().IsAlive(entity));
    EXPECT_FALSE(bridge.GetPoseSnapshot(runtime.GetSceneRuntimeId(), entity).has_value());
    EXPECT_EQ(bridge.BindRootMotionPhysicsBody(runtime.GetSceneRuntimeId(), entity, 100),
              AnimationSceneAdapters::AnimationRootMotionPhysicsBindingResult::InvalidEntity);
    ASSERT_TRUE(runtime.Tick().succeeded);
    EXPECT_FALSE(runtime.GetRegistry().IsAlive(entity));
    EXPECT_FALSE(bridge.GetPoseSnapshot(runtime.GetSceneRuntimeId(), entity).has_value());
    EXPECT_EQ(bridge.BindRootMotionPhysicsBody(runtime.GetSceneRuntimeId(), entity, 100),
              AnimationSceneAdapters::AnimationRootMotionPhysicsBindingResult::InvalidEntity);
}

TEST(EcsAnimationValidation, RootMotionRejectsReplayAndGapWithoutPublishingThem)
{
    SceneECS::SceneEcsRuntime runtime;
    uint32 calls = 0;
    AnimationSceneAdapters::AnimationEcsBridge bridge(
        runtime,
        [&calls](const AnimationSceneAdapters::AnimationEcsEvaluationRequest& request)
        {
            ++calls;
            const uint64 sourceSequence = calls == 1 ? 1u : (calls == 2 ? 1u : 3u);
            return MakePose(request.skeleton.boneCount, sourceSequence);
        },
        [](ECS::SceneRuntimeId, ECS::EntityHandle, uint64 physicsBodyHandlePacked)
        {
            return physicsBodyHandlePacked == 77;
        });
    ASSERT_EQ(bridge.RegisterProcessors(),
              AnimationSceneAdapters::AnimationEcsBridgeRegistrationResult::Registered);
    const ECS::EntityHandle entity = AddAnimatedEntity(
        runtime, SceneECS::AnimationEvaluationMode::Fixed, true);
    ASSERT_TRUE(runtime.Tick().succeeded);
    ASSERT_EQ(bridge.BindRootMotionPhysicsBody(runtime.GetSceneRuntimeId(), entity, 77),
              AnimationSceneAdapters::AnimationRootMotionPhysicsBindingResult::Applied);

    ASSERT_TRUE(TickFixed(runtime).succeeded);
    const auto* firstIntent = runtime.GetRegistry().TryGet<SceneECS::RootMotionIntent>(entity);
    ASSERT_NE(firstIntent, nullptr);
    EXPECT_TRUE(firstIntent->pending);
    EXPECT_FALSE(firstIntent->sequenceRejected);
    EXPECT_EQ(firstIntent->sourcePoseSequence, 1u);
    EXPECT_EQ(firstIntent->rootMotionSequence, 1u);
    EXPECT_EQ(firstIntent->sceneRuntimeIdValue, runtime.GetSceneRuntimeId().GetValue());
    EXPECT_EQ(firstIntent->targetEntity, entity);
    EXPECT_EQ(firstIntent->physicsBodyHandlePacked, 77u);
    EXPECT_EQ(firstIntent->fixedStepSequence, 1u);

    SceneECS::RootMotionIntent consumedFirst = *firstIntent;
    consumedFirst.pending = false;
    ASSERT_TRUE(runtime.SetFragment(entity, consumedFirst));

    ASSERT_TRUE(TickFixed(runtime).succeeded);
    const auto* replay = runtime.GetRegistry().TryGet<SceneECS::RootMotionIntent>(entity);
    ASSERT_NE(replay, nullptr);
    EXPECT_FALSE(replay->pending);
    EXPECT_TRUE(replay->sequenceRejected);
    EXPECT_EQ(replay->sourcePoseSequence, 2u);
    EXPECT_EQ(replay->rootMotionSequence, 1u);

    ASSERT_TRUE(TickFixed(runtime).succeeded);
    const auto* gap = runtime.GetRegistry().TryGet<SceneECS::RootMotionIntent>(entity);
    ASSERT_NE(gap, nullptr);
    EXPECT_FALSE(gap->pending);
    EXPECT_TRUE(gap->sequenceRejected);
    EXPECT_EQ(gap->sourcePoseSequence, 3u);
    EXPECT_EQ(gap->rootMotionSequence, 3u);
    const auto diagnostics = bridge.GetDiagnosticsSnapshot();
    EXPECT_EQ(diagnostics.rootMotionPublicationCount, 1u);
    EXPECT_EQ(diagnostics.rootMotionReplayCount, 1u);
    EXPECT_EQ(diagnostics.rootMotionGapCount, 1u);
}

TEST(EcsAnimationValidation, StructuralJournalLossRebuildsAllEligibleBindings)
{
    SceneECS::SceneEcsRuntime runtime(8192, 1);
    AnimationSceneAdapters::AnimationEcsBridge bridge(runtime);
    ASSERT_EQ(bridge.RegisterProcessors(),
              AnimationSceneAdapters::AnimationEcsBridgeRegistrationResult::Registered);
    const ECS::EntityHandle first = AddAnimatedEntity(
        runtime, SceneECS::AnimationEvaluationMode::Fixed);
    const ECS::EntityHandle second = AddAnimatedEntity(
        runtime, SceneECS::AnimationEvaluationMode::Fixed);
    ASSERT_TRUE(first.IsValid());
    ASSERT_TRUE(second.IsValid());

    ASSERT_TRUE(TickFixed(runtime).succeeded);
    const auto diagnostics = bridge.GetDiagnosticsSnapshot();
    EXPECT_GE(diagnostics.structuralContinuityLossCount, 1u);
    EXPECT_GE(diagnostics.authoritativeReconcileCount, 1u);
    EXPECT_EQ(diagnostics.activeBindingCount, 2u);
    EXPECT_TRUE(bridge.GetPoseSnapshot(runtime.GetSceneRuntimeId(), first).has_value());
    EXPECT_TRUE(bridge.GetPoseSnapshot(runtime.GetSceneRuntimeId(), second).has_value());
}

TEST(EcsAnimationValidation, CleanupCursorLossStillAcknowledgesEveryAnimationBinding)
{
    SceneECS::SceneEcsRuntime runtime(1, 1);
    AnimationSceneAdapters::AnimationEcsBridge bridge(runtime);
    ASSERT_EQ(bridge.RegisterProcessors(),
              AnimationSceneAdapters::AnimationEcsBridgeRegistrationResult::Registered);
    const ECS::EntityHandle first = AddAnimatedEntity(
        runtime, SceneECS::AnimationEvaluationMode::Fixed);
    const ECS::EntityHandle second = AddAnimatedEntity(
        runtime, SceneECS::AnimationEvaluationMode::Fixed);
    ASSERT_TRUE(runtime.Tick().succeeded);
    ASSERT_EQ(runtime.RequestDestroy(
                  first, SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Animation)),
              SceneECS::DestroyRequestResult::Accepted);
    ASSERT_EQ(runtime.RequestDestroy(
                  second, SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Animation)),
              SceneECS::DestroyRequestResult::Accepted);
    ASSERT_EQ(runtime.PublishPendingDestroyCleanup(), 2u);

    ASSERT_TRUE(runtime.Tick().succeeded);
    EXPECT_FALSE(runtime.GetRegistry().IsAlive(first));
    EXPECT_FALSE(runtime.GetRegistry().IsAlive(second));
    const auto diagnostics = bridge.GetDiagnosticsSnapshot();
    EXPECT_GE(diagnostics.cleanupContinuityLossCount, 1u);
    EXPECT_EQ(diagnostics.pendingCleanupCount, 0u);
    EXPECT_TRUE(diagnostics.bindings.empty());
}

TEST(EcsAnimationValidation, CleanupCallbackRunsBeforeAnimationAcknowledgement)
{
    SceneECS::SceneEcsRuntime runtime;
    uint32 cleanupCalls = 0;
    bool observedCleanupRequired = false;
    AnimationSceneAdapters::AnimationEcsBridge bridge(
        runtime,
        {},
        {},
        [&runtime, &cleanupCalls, &observedCleanupRequired](ECS::SceneRuntimeId sceneRuntimeId,
                                                            ECS::EntityHandle entity)
        {
            ++cleanupCalls;
            const SceneECS::EntityLifecycleState* lifecycle =
                runtime.GetRegistry().TryGet<SceneECS::EntityLifecycleState>(entity);
            observedCleanupRequired = sceneRuntimeId == runtime.GetSceneRuntimeId() &&
                                      lifecycle != nullptr &&
                                      lifecycle->phase ==
                                          SceneECS::EntityLifecyclePhase::CleanupRequired &&
                                      (lifecycle->acknowledgedCleanupDomains &
                                       SceneECS::ToCleanupDomainMask(
                                           SceneECS::CleanupDomain::Animation)) == 0;
            return observedCleanupRequired;
        });
    ASSERT_EQ(bridge.RegisterProcessors(),
              AnimationSceneAdapters::AnimationEcsBridgeRegistrationResult::Registered);
    const ECS::EntityHandle entity = AddAnimatedEntity(
        runtime, SceneECS::AnimationEvaluationMode::Fixed);
    ASSERT_TRUE(runtime.Tick().succeeded);
    ASSERT_EQ(runtime.RequestDestroy(
                  entity, SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Animation)),
              SceneECS::DestroyRequestResult::Accepted);
    ASSERT_EQ(runtime.PublishPendingDestroyCleanup(), 1u);

    ASSERT_TRUE(runtime.Tick().succeeded);
    EXPECT_EQ(cleanupCalls, 1u);
    EXPECT_TRUE(observedCleanupRequired);
    EXPECT_FALSE(runtime.GetRegistry().IsAlive(entity));
    EXPECT_TRUE(bridge.GetDiagnosticsSnapshot().bindings.empty());
}

TEST(EcsAnimationValidation, FailedCleanupCallbackRetainsGateAndRetries)
{
    SceneECS::SceneEcsRuntime runtime;
    uint32 cleanupCalls = 0;
    AnimationSceneAdapters::AnimationEcsBridge bridge(
        runtime,
        {},
        {},
        [&cleanupCalls](ECS::SceneRuntimeId, ECS::EntityHandle)
        {
            ++cleanupCalls;
            return cleanupCalls > 1u;
        });
    ASSERT_EQ(bridge.RegisterProcessors(),
              AnimationSceneAdapters::AnimationEcsBridgeRegistrationResult::Registered);
    const ECS::EntityHandle entity = AddAnimatedEntity(
        runtime, SceneECS::AnimationEvaluationMode::Fixed);
    ASSERT_TRUE(runtime.Tick().succeeded);
    ASSERT_EQ(runtime.RequestDestroy(
                  entity, SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Animation)),
              SceneECS::DestroyRequestResult::Accepted);
    ASSERT_EQ(runtime.PublishPendingDestroyCleanup(), 1u);

    EXPECT_FALSE(runtime.Tick().succeeded);
    const SceneECS::EntityLifecycleState* failedLifecycle =
        runtime.GetRegistry().TryGet<SceneECS::EntityLifecycleState>(entity);
    ASSERT_NE(failedLifecycle, nullptr);
    EXPECT_EQ(failedLifecycle->phase, SceneECS::EntityLifecyclePhase::CleanupRequired);
    EXPECT_EQ(failedLifecycle->acknowledgedCleanupDomains &
                  SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Animation),
              0u);
    EXPECT_EQ(bridge.GetDiagnosticsSnapshot().pendingCleanupCount, 1u);

    ASSERT_TRUE(runtime.Tick().succeeded);
    EXPECT_EQ(cleanupCalls, 2u);
    EXPECT_FALSE(runtime.GetRegistry().IsAlive(entity));
    EXPECT_TRUE(bridge.GetDiagnosticsSnapshot().bindings.empty());
}
