#include "AnimationSceneAdapters/ECS/ResourceAnimationEcsEvaluator.h"

#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <unordered_map>

namespace
{
    using namespace RVX;

    using AnimationSceneAdapters::AnimationEcsEvaluationRequest;
    using AnimationSceneAdapters::ResourceAnimationEcsEvaluator;

    constexpr uint64 AnimationAssetValue = 7001;

    Animation::Skeleton::Ptr MakeSkeleton()
    {
        Animation::Skeleton::Ptr skeleton = Animation::Skeleton::Create();
        Animation::Bone root("root", -1);
        root.localBindPose = Animation::TransformSample::Identity();
        skeleton->AddBone(root);
        skeleton->BuildHierarchy();
        skeleton->ComputeInverseBindPoses();
        return skeleton;
    }

    Animation::AnimationClip::Ptr MakeClip(const Animation::Skeleton::ConstPtr& skeleton,
                                           bool rootMotion)
    {
        constexpr Animation::TimeUs DurationUs = 1'000'000;
        Animation::AnimationClip::Ptr clip = Animation::AnimationClip::Create("walk");
        clip->duration = DurationUs;
        clip->defaultWrapMode = Animation::WrapMode::Loop;
        clip->skeleton = skeleton;
        clip->hasRootMotion = rootMotion;
        if (rootMotion)
        {
            clip->rootMotionBoneName = "root";
        }

        Animation::TransformTrack track;
        track.targetName = "root";
        track.targetType = Animation::TrackTargetType::Bone;
        track.mode = Animation::TransformMode::TRS;
        track.translationKeyframes.emplace_back(0, Vec3(0.0f));
        track.translationKeyframes.emplace_back(DurationUs, Vec3(0.0f, 0.0f, 1.0f));
        track.rotationKeyframes.emplace_back(0, Quat(1.0f, 0.0f, 0.0f, 0.0f));
        track.rotationKeyframes.emplace_back(DurationUs, Quat(1.0f, 0.0f, 0.0f, 0.0f));
        track.scaleKeyframes.emplace_back(0, Vec3(1.0f));
        track.scaleKeyframes.emplace_back(DurationUs, Vec3(1.0f));
        clip->transformTracks.push_back(std::move(track));
        return clip;
    }

    std::shared_ptr<const Resource::AnimationResource> MakeResource(bool rootMotion = false)
    {
        const Animation::Skeleton::Ptr skeleton = MakeSkeleton();
        auto resource = std::make_shared<Resource::AnimationResource>();
        Resource::AnimationResource::AnimationClipMap clips;
        clips.emplace("walk", MakeClip(skeleton, rootMotion));
        EXPECT_TRUE(resource->SetData(skeleton, std::move(clips)));
        return resource;
    }

    AnimationEcsEvaluationRequest MakeRequest(ECS::SceneRuntimeId sceneRuntimeId,
                                              ECS::EntityHandle entity,
                                              SceneECS::AnimationEvaluationMode mode,
                                              uint64 sequence,
                                              double deltaSeconds = 0.0,
                                              bool rootMotion = false)
    {
        AnimationEcsEvaluationRequest request;
        request.sceneRuntimeId = sceneRuntimeId;
        request.entity = entity;
        request.skeleton.animationAssetValue = AnimationAssetValue;
        request.skeleton.animationClipOrdinal = 0;
        request.skeleton.boneCount = 1;
        request.animator.evaluationMode = mode;
        request.animator.playbackRate = 1.0f;
        request.animator.rootMotionEnabled = rootMotion;
        request.deltaSeconds = deltaSeconds;
        request.nextPoseSequence = sequence;
        if (mode == SceneECS::AnimationEvaluationMode::Fixed)
        {
            request.fixedStepSequence = sequence;
        }
        else
        {
            request.frameSequence = sequence;
        }
        return request;
    }
} // namespace

TEST(EcsResourceAnimationEvaluatorValidation, RejectsMissingAndStaleResourceBindings)
{
    const ECS::SceneRuntimeId sceneRuntimeId(101);
    const ECS::EntityHandle liveEntity = ECS::EntityHandle::Create(3, 9);
    const std::shared_ptr<const Resource::AnimationResource> resource = MakeResource();

    ResourceAnimationEcsEvaluator evaluator(
        [resource](uint64 assetValue)
        {
            return assetValue == AnimationAssetValue ? resource :
                                                      std::shared_ptr<const Resource::AnimationResource>{};
        },
        [sceneRuntimeId, liveEntity](ECS::SceneRuntimeId candidateScene,
                                     ECS::EntityHandle candidateEntity)
        {
            return candidateScene == sceneRuntimeId && candidateEntity == liveEntity;
        });

    AnimationEcsEvaluationRequest request = MakeRequest(
        sceneRuntimeId, liveEntity, SceneECS::AnimationEvaluationMode::VariablePrePhysics, 1, 0.25);
    ASSERT_TRUE(evaluator.Evaluate(request).has_value());

    request.skeleton.animationAssetValue = AnimationAssetValue + 1;
    EXPECT_FALSE(evaluator.Evaluate(request).has_value());
    request.skeleton.animationAssetValue = AnimationAssetValue;
    request.sceneRuntimeId = ECS::SceneRuntimeId(102);
    EXPECT_FALSE(evaluator.Evaluate(request).has_value());
    request.sceneRuntimeId = sceneRuntimeId;
    request.entity = ECS::EntityHandle::Create(liveEntity.GetIndex(), liveEntity.GetGeneration() + 1u);
    EXPECT_FALSE(evaluator.Evaluate(request).has_value());

    const auto snapshot = evaluator.GetPlaybackSnapshot(sceneRuntimeId, liveEntity);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->unwrappedTimeUs, 250'000);
    EXPECT_EQ(evaluator.GetDiagnosticsSnapshot().activePlaybackCount, 1u);
    EXPECT_EQ(evaluator.GetDiagnosticsSnapshot().rejectedEvaluationCount, 3u);
}

TEST(EcsResourceAnimationEvaluatorValidation, AdvancesVariableAndFixedClocksExactly)
{
    const ECS::SceneRuntimeId sceneRuntimeId(201);
    const ECS::EntityHandle variableEntity = ECS::EntityHandle::Create(1, 1);
    const ECS::EntityHandle fixedEntity = ECS::EntityHandle::Create(2, 1);
    const std::shared_ptr<const Resource::AnimationResource> resource = MakeResource();
    ResourceAnimationEcsEvaluator evaluator(
        [resource](uint64) { return resource; });

    ASSERT_TRUE(evaluator.Evaluate(MakeRequest(
        sceneRuntimeId, variableEntity, SceneECS::AnimationEvaluationMode::VariablePrePhysics,
        1, 0.125)).has_value());
    ASSERT_TRUE(evaluator.Evaluate(MakeRequest(
        sceneRuntimeId, variableEntity, SceneECS::AnimationEvaluationMode::VariablePrePhysics,
        2, 0.125)).has_value());
    const auto variable = evaluator.GetPlaybackSnapshot(sceneRuntimeId, variableEntity);
    ASSERT_TRUE(variable.has_value());
    EXPECT_EQ(variable->unwrappedTimeUs, 250'000);
    EXPECT_EQ(variable->lastVariableFrameSequence, 2u);

    AnimationEcsEvaluationRequest fixedOne = MakeRequest(
        sceneRuntimeId, fixedEntity, SceneECS::AnimationEvaluationMode::Fixed, 1, 123.0);
    AnimationEcsEvaluationRequest fixedTwo = MakeRequest(
        sceneRuntimeId, fixedEntity, SceneECS::AnimationEvaluationMode::Fixed, 2, 0.0001);
    ASSERT_TRUE(evaluator.Evaluate(fixedOne).has_value());
    ASSERT_TRUE(evaluator.Evaluate(fixedTwo).has_value());
    const auto fixed = evaluator.GetPlaybackSnapshot(sceneRuntimeId, fixedEntity);
    ASSERT_TRUE(fixed.has_value());
    EXPECT_EQ(fixed->unwrappedTimeUs,
              Animation::Fixed60HzStepDeltaUs(1) + Animation::Fixed60HzStepDeltaUs(2));
    EXPECT_EQ(fixed->lastFixedStepSequence, 2u);
}

TEST(EcsResourceAnimationEvaluatorValidation, EvaluatesLoopingRootMotionAndRemovesItFromPalette)
{
    const ECS::SceneRuntimeId sceneRuntimeId(301);
    const ECS::EntityHandle entity = ECS::EntityHandle::Create(3, 1);
    const std::shared_ptr<const Resource::AnimationResource> resource = MakeResource(true);
    ResourceAnimationEcsEvaluator evaluator(
        [resource](uint64) { return resource; });

    const auto result = evaluator.Evaluate(MakeRequest(
        sceneRuntimeId, entity, SceneECS::AnimationEvaluationMode::VariablePrePhysics,
        1, 1.25, true));
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->skinningPalette.size(), 1u);
    EXPECT_NEAR(result->rootMotionTranslation.z, 1.25f, 0.0001f);
    EXPECT_FLOAT_EQ(result->rootMotionTranslation.x, 0.0f);
    EXPECT_FLOAT_EQ(result->rootMotionTranslation.y, 0.0f);
    EXPECT_NEAR(result->skinningPalette.front()[3][2], 0.0f, 0.0001f);
    EXPECT_EQ(result->rootMotionSequence, 0u);
}

TEST(EcsResourceAnimationEvaluatorValidation,
     FixedEvaluationAllowsBridgeBackpressureGapButStillRejectsReplay)
{
    const ECS::SceneRuntimeId sceneRuntimeId(302);
    const ECS::EntityHandle entity = ECS::EntityHandle::Create(3, 2);
    const std::shared_ptr<const Resource::AnimationResource> resource = MakeResource(true);
    ResourceAnimationEcsEvaluator evaluator(
        [resource](uint64) { return resource; });

    ASSERT_TRUE(evaluator.Evaluate(MakeRequest(
        sceneRuntimeId, entity, SceneECS::AnimationEvaluationMode::Fixed,
        1, 0.0, true)).has_value());
    ASSERT_TRUE(evaluator.Evaluate(MakeRequest(
        sceneRuntimeId, entity, SceneECS::AnimationEvaluationMode::Fixed,
        3, 0.0, true)).has_value());
    EXPECT_FALSE(evaluator.Evaluate(MakeRequest(
        sceneRuntimeId, entity, SceneECS::AnimationEvaluationMode::Fixed,
        3, 0.0, true)).has_value());

    const auto playback = evaluator.GetPlaybackSnapshot(sceneRuntimeId, entity);
    ASSERT_TRUE(playback.has_value());
    EXPECT_EQ(playback->lastFixedStepSequence, 3u);
    EXPECT_EQ(playback->unwrappedTimeUs,
              Animation::Fixed60HzStepDeltaUs(1) +
                  Animation::Fixed60HzStepDeltaUs(3));
}

TEST(EcsResourceAnimationEvaluatorValidation,
     FixedPausedVariablePaletteInitializerProducesBaselineWithoutConsumingSimulationTime)
{
    const ECS::SceneRuntimeId sceneRuntimeId(303);
    const ECS::EntityHandle entity = ECS::EntityHandle::Create(3, 3);
    const std::shared_ptr<const Resource::AnimationResource> resource = MakeResource(true);
    ResourceAnimationEcsEvaluator evaluator(
        [resource](uint64) { return resource; });

    AnimationEcsEvaluationRequest initializer = MakeRequest(
        sceneRuntimeId, entity, SceneECS::AnimationEvaluationMode::Fixed,
        1, 0.0, true);
    initializer.animator.playing = false;
    initializer.frameSequence = 1;
    initializer.fixedStepSequence = 0;

    const auto baseline = evaluator.Evaluate(initializer);
    ASSERT_TRUE(baseline.has_value());
    ASSERT_EQ(baseline->skinningPalette.size(), 1u);
    EXPECT_FLOAT_EQ(baseline->rootMotionTranslation.x, 0.0f);
    EXPECT_FLOAT_EQ(baseline->rootMotionTranslation.y, 0.0f);
    EXPECT_FLOAT_EQ(baseline->rootMotionTranslation.z, 0.0f);
    EXPECT_EQ(baseline->rootMotionSequence, 0u);

    const auto initializedPlayback = evaluator.GetPlaybackSnapshot(sceneRuntimeId, entity);
    ASSERT_TRUE(initializedPlayback.has_value());
    EXPECT_EQ(initializedPlayback->unwrappedTimeUs, 0);
    EXPECT_EQ(initializedPlayback->lastVariableFrameSequence, 0u);
    EXPECT_EQ(initializedPlayback->lastFixedStepSequence, 0u);

    AnimationEcsEvaluationRequest fixedPlaying = initializer;
    fixedPlaying.animator.playing = true;
    fixedPlaying.fixedStepSequence = 1;
    fixedPlaying.frameSequence = 2;
    fixedPlaying.nextPoseSequence = 2;

    const auto firstFixedStep = evaluator.Evaluate(fixedPlaying);
    ASSERT_TRUE(firstFixedStep.has_value());
    EXPECT_NEAR(firstFixedStep->rootMotionTranslation.z,
                static_cast<float>(Animation::Fixed60HzStepDeltaUs(1)) /
                    static_cast<float>(Animation::kMicrosecondsPerSecond),
                0.0001f);

    const auto playedBack = evaluator.GetPlaybackSnapshot(sceneRuntimeId, entity);
    ASSERT_TRUE(playedBack.has_value());
    EXPECT_EQ(playedBack->unwrappedTimeUs, Animation::Fixed60HzStepDeltaUs(1));
    EXPECT_EQ(playedBack->lastFixedStepSequence, 1u);

    EXPECT_FALSE(evaluator.Evaluate(fixedPlaying).has_value());
    const auto replayRejectedPlayback = evaluator.GetPlaybackSnapshot(sceneRuntimeId, entity);
    ASSERT_TRUE(replayRejectedPlayback.has_value());
    EXPECT_EQ(replayRejectedPlayback->unwrappedTimeUs, playedBack->unwrappedTimeUs);
    EXPECT_EQ(replayRejectedPlayback->lastFixedStepSequence,
              playedBack->lastFixedStepSequence);
}

TEST(EcsResourceAnimationEvaluatorValidation, FailureDoesNotAdvancePlaybackState)
{
    const ECS::SceneRuntimeId sceneRuntimeId(401);
    const ECS::EntityHandle entity = ECS::EntityHandle::Create(4, 1);
    const std::shared_ptr<const Resource::AnimationResource> resource = MakeResource();
    ResourceAnimationEcsEvaluator evaluator(
        [resource](uint64) { return resource; });

    AnimationEcsEvaluationRequest request = MakeRequest(
        sceneRuntimeId, entity, SceneECS::AnimationEvaluationMode::VariablePrePhysics, 1, 0.3);
    ASSERT_TRUE(evaluator.Evaluate(request).has_value());
    const auto accepted = evaluator.GetPlaybackSnapshot(sceneRuntimeId, entity);
    ASSERT_TRUE(accepted.has_value());

    request.frameSequence = 2;
    request.skeleton.animationClipOrdinal = 1;
    EXPECT_FALSE(evaluator.Evaluate(request).has_value());
    const auto retained = evaluator.GetPlaybackSnapshot(sceneRuntimeId, entity);
    ASSERT_TRUE(retained.has_value());
    EXPECT_EQ(retained->unwrappedTimeUs, accepted->unwrappedTimeUs);
    EXPECT_EQ(retained->committedEvaluationCount, accepted->committedEvaluationCount);

    request.skeleton.animationClipOrdinal = 0;
    ASSERT_TRUE(evaluator.Evaluate(request).has_value());
    const auto recovered = evaluator.GetPlaybackSnapshot(sceneRuntimeId, entity);
    ASSERT_TRUE(recovered.has_value());
    EXPECT_EQ(recovered->unwrappedTimeUs, 600'000);
    EXPECT_EQ(recovered->committedEvaluationCount, 2u);
}

TEST(EcsResourceAnimationEvaluatorValidation, RebindsGenerationsAndDrainsOnShutdown)
{
    const ECS::SceneRuntimeId sceneRuntimeId(501);
    const ECS::EntityHandle staleEntity = ECS::EntityHandle::Create(7, 2);
    const ECS::EntityHandle reboundEntity = ECS::EntityHandle::Create(7, 3);
    const std::shared_ptr<const Resource::AnimationResource> resource = MakeResource();
    ResourceAnimationEcsEvaluator evaluator(
        [resource](uint64) { return resource; });

    ASSERT_TRUE(evaluator.Evaluate(MakeRequest(
        sceneRuntimeId, staleEntity, SceneECS::AnimationEvaluationMode::VariablePrePhysics,
        1, 0.1)).has_value());
    ASSERT_TRUE(evaluator.Evaluate(MakeRequest(
        sceneRuntimeId, reboundEntity, SceneECS::AnimationEvaluationMode::VariablePrePhysics,
        1, 0.2)).has_value());
    EXPECT_FALSE(evaluator.GetPlaybackSnapshot(sceneRuntimeId, staleEntity).has_value());
    const auto rebound = evaluator.GetPlaybackSnapshot(sceneRuntimeId, reboundEntity);
    ASSERT_TRUE(rebound.has_value());
    EXPECT_EQ(rebound->unwrappedTimeUs, 200'000);

    EXPECT_FALSE(evaluator.RemoveBinding(ECS::SceneRuntimeId(502), reboundEntity));
    EXPECT_EQ(evaluator.RemoveScene(sceneRuntimeId), 1u);
    EXPECT_EQ(evaluator.GetDiagnosticsSnapshot().activePlaybackCount, 0u);

    const AnimationSceneAdapters::AnimationEcsPoseEvaluator callback =
        evaluator.CreatePoseEvaluator();
    ASSERT_TRUE(callback(MakeRequest(
        sceneRuntimeId, reboundEntity, SceneECS::AnimationEvaluationMode::VariablePrePhysics,
        1, 0.2)).has_value());
    evaluator.Shutdown();
    EXPECT_TRUE(evaluator.IsShutdown());
    EXPECT_FALSE(callback(MakeRequest(
        sceneRuntimeId, reboundEntity, SceneECS::AnimationEvaluationMode::VariablePrePhysics,
        2, 0.2)).has_value());
    EXPECT_EQ(evaluator.GetDiagnosticsSnapshot().activePlaybackCount, 0u);
}

TEST(EcsResourceAnimationEvaluatorValidation, SceneScopedDiagnosticsExcludeOtherWorldPlaybacks)
{
    const ECS::SceneRuntimeId firstScene(601);
    const ECS::SceneRuntimeId secondScene(602);
    const ECS::EntityHandle firstEntity = ECS::EntityHandle::Create(8, 1);
    const ECS::EntityHandle secondEntity = ECS::EntityHandle::Create(9, 1);
    const std::shared_ptr<const Resource::AnimationResource> resource = MakeResource();
    ResourceAnimationEcsEvaluator evaluator([resource](uint64) { return resource; });

    ASSERT_TRUE(evaluator.Evaluate(MakeRequest(
        firstScene, firstEntity, SceneECS::AnimationEvaluationMode::VariablePrePhysics,
        1, 0.1)).has_value());
    ASSERT_TRUE(evaluator.Evaluate(MakeRequest(
        secondScene, secondEntity, SceneECS::AnimationEvaluationMode::VariablePrePhysics,
        1, 0.1)).has_value());
    EXPECT_EQ(evaluator.GetDiagnosticsSnapshot().activePlaybackCount, 2u);
    EXPECT_EQ(evaluator.GetSceneDiagnosticsSnapshot(firstScene).activePlaybackCount, 1u);
    EXPECT_EQ(evaluator.GetSceneDiagnosticsSnapshot(secondScene).activePlaybackCount, 1u);

    EXPECT_EQ(evaluator.RemoveScene(firstScene), 1u);
    EXPECT_EQ(evaluator.GetSceneDiagnosticsSnapshot(firstScene).activePlaybackCount, 0u);
    EXPECT_EQ(evaluator.GetSceneDiagnosticsSnapshot(secondScene).activePlaybackCount, 1u);
}

TEST(EcsResourceAnimationEvaluatorValidation, BridgeFragmentRemovalDrainsEvaluatorSideTable)
{
    SceneECS::SceneEcsRuntime runtime;
    const std::shared_ptr<const Resource::AnimationResource> resource = MakeResource();
    ResourceAnimationEcsEvaluator evaluator(
        [resource](uint64 assetValue)
        {
            return assetValue == AnimationAssetValue ? resource :
                                                      std::shared_ptr<const Resource::AnimationResource>{};
        });
    AnimationSceneAdapters::AnimationEcsBridge bridge(
        runtime,
        evaluator.CreatePoseEvaluator(),
        {},
        evaluator.CreateCleanupCallback());
    ASSERT_EQ(bridge.RegisterProcessors(),
              AnimationSceneAdapters::AnimationEcsBridgeRegistrationResult::Registered);

    const ECS::EntityHandle entity = runtime.CreateEntity();
    ASSERT_TRUE(entity.IsValid());
    SceneECS::AnimationSkeletonBinding skeleton;
    skeleton.animationAssetValue = AnimationAssetValue;
    skeleton.animationClipOrdinal = 0;
    skeleton.boneCount = 1;
    SceneECS::Animator animator;
    animator.evaluationMode = SceneECS::AnimationEvaluationMode::VariablePrePhysics;
    ASSERT_TRUE(runtime.AddFragment<SceneECS::AnimationSkeletonBinding>(entity, skeleton));
    ASSERT_TRUE(runtime.AddFragment<SceneECS::Animator>(entity, animator));
    ASSERT_TRUE(runtime.AddFragment<SceneECS::AnimationPoseState>(entity));

    ASSERT_TRUE(runtime.Tick({.variableDeltaSeconds = 0.2}).succeeded);
    ASSERT_TRUE(evaluator.GetPlaybackSnapshot(runtime.GetSceneRuntimeId(), entity).has_value());
    ASSERT_TRUE(runtime.RemoveFragment<SceneECS::Animator>(entity));
    ASSERT_TRUE(runtime.Tick({.variableDeltaSeconds = 0.2}).succeeded);
    EXPECT_FALSE(evaluator.GetPlaybackSnapshot(runtime.GetSceneRuntimeId(), entity).has_value());
    EXPECT_TRUE(bridge.GetDiagnosticsSnapshot().bindings.empty());
}
