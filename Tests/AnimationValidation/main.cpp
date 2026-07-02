#include "Animation/Data/AnimationClip.h"
#include "Animation/Data/Skeleton.h"
#include "Animation/Runtime/AnimationPlayer.h"
#include "Animation/State/AnimationStateMachine.h"
#include "Core/Job/JobSystem.h"
#include "Scene/Components/AnimatorComponent.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace RVX;

namespace
{
Animation::Skeleton::Ptr CreateSingleBoneSkeleton()
{
    auto skeleton = Animation::Skeleton::Create();
    skeleton->AddBone("Root");
    skeleton->ComputeInverseBindPoses();
    return skeleton;
}

Animation::Skeleton::Ptr CreateLinearSkeleton(size_t boneCount)
{
    auto skeleton = Animation::Skeleton::Create();
    skeleton->Reserve(boneCount);

    for (size_t i = 0; i < boneCount; ++i)
    {
        skeleton->AddBone("Bone" + std::to_string(i), i == 0 ? -1 : static_cast<int>(i - 1));
    }

    skeleton->ComputeInverseBindPoses();
    return skeleton;
}

Animation::AnimationClip::Ptr CreateEventClip(const std::string& name)
{
    auto clip = Animation::AnimationClip::Create(name);
    clip->SetDurationSeconds(1.0);
    clip->AddEvent("LoopStart", 0.1);
    clip->AddEvent("Footstep", 0.25);
    clip->AddEvent("Land", 0.75);
    return clip;
}

Animation::AnimationClip::Ptr CreateTransformClip(const std::string& name, size_t boneCount)
{
    auto clip = Animation::AnimationClip::Create(name);
    clip->SetDurationSeconds(1.0);

    for (size_t i = 0; i < boneCount; ++i)
    {
        Animation::TransformTrack track;
        track.targetName = "Bone" + std::to_string(i);
        track.targetType = Animation::TrackTargetType::Bone;
        track.translationKeyframes.emplace_back(Animation::SecondsToTimeUs(0.0), Vec3(static_cast<float>(i), 0.0f, 0.0f));
        track.translationKeyframes.emplace_back(Animation::SecondsToTimeUs(1.0), Vec3(static_cast<float>(i) + 10.0f, static_cast<float>(i) * 2.0f, 1.0f));
        clip->AddTransformTrack(std::move(track));
    }

    return clip;
}

class ScopedJobSystem
{
public:
    ScopedJobSystem()
    {
        if (!JobSystem::Get().IsInitialized())
        {
            JobSystem::Get().Initialize(2);
            m_initializedHere = true;
        }
    }

    ~ScopedJobSystem()
    {
        if (m_initializedHere)
        {
            JobSystem::Get().Shutdown();
        }
    }

private:
    bool m_initializedHere = false;
};
} // namespace

TEST(AnimationValidation, AnimationPlayerDispatchesClipEventsDuringPlaybackAndLoop)
{
    Animation::AnimationPlayer player(CreateSingleBoneSkeleton());
    auto clip = CreateEventClip("Looped");

    std::vector<std::string> events;
    player.SetEventCallback([&events](const std::string& eventName) {
        events.push_back(eventName);
    });

    const uint32_t instanceId = player.Play(clip, Animation::WrapMode::Loop);
    ASSERT_NE(0u, instanceId);
    ASSERT_NE(nullptr, player.GetInstance(instanceId));
    EXPECT_TRUE(player.IsPlaying(instanceId));

    player.Update(0.2f);
    EXPECT_EQ((std::vector<std::string>{"LoopStart"}), events);

    player.Update(0.1f);
    EXPECT_EQ((std::vector<std::string>{"LoopStart", "Footstep"}), events);

    player.Update(0.6f);
    EXPECT_EQ((std::vector<std::string>{"LoopStart", "Footstep", "Land"}), events);

    player.Update(0.3f);
    EXPECT_EQ((std::vector<std::string>{"LoopStart", "Footstep", "Land", "LoopStart"}), events);

    events.clear();
    player.SetNormalizedTime(0.0f);
    player.Update(1.0f);
    EXPECT_EQ((std::vector<std::string>{"LoopStart", "Footstep", "Land"}), events);
}

TEST(AnimationValidation, AnimationPlayerUsesInstanceIdsForCallbacksAndLookup)
{
    Animation::AnimationPlayer player(CreateSingleBoneSkeleton());

    auto onceClip = Animation::AnimationClip::Create("Once");
    onceClip->SetDurationSeconds(0.25);

    uint32_t completedInstanceId = 0;
    player.SetCompletionCallback([&completedInstanceId](uint32_t instanceId) {
        completedInstanceId = instanceId;
    });

    const uint32_t instanceId = player.Play(onceClip, Animation::WrapMode::Once);
    ASSERT_NE(0u, instanceId);
    ASSERT_NE(nullptr, player.GetInstance(instanceId));
    EXPECT_EQ(instanceId, player.GetInstance(instanceId)->id);

    const uint32_t additiveId = player.PlayAdditive(onceClip, 0.5f);
    ASSERT_NE(0u, additiveId);
    ASSERT_NE(instanceId, additiveId);
    ASSERT_NE(nullptr, player.GetInstance(additiveId));
    EXPECT_FLOAT_EQ(0.5f, player.GetInstance(additiveId)->weight);

    player.SetSpeed(instanceId, 2.0f);
    EXPECT_FLOAT_EQ(2.0f, player.GetInstance(instanceId)->speed);

    player.Update(0.3f);

    EXPECT_EQ(instanceId, completedInstanceId);
    EXPECT_FALSE(player.IsPlaying(instanceId));
}

TEST(AnimationValidation, AnimatorComponentTickDispatchesStateClipEvents)
{
    auto skeleton = CreateSingleBoneSkeleton();
    auto clip = CreateEventClip("Idle");

    auto stateMachine = Animation::AnimationStateMachine::Create(skeleton);
    auto idle = stateMachine->AddState("Idle");
    idle->SetMotion(clip);
    idle->SetLooping(true);
    stateMachine->SetDefaultState("Idle");

    AnimatorComponent animator;
    std::vector<std::string> events;

    animator.SetOnAnimationEvent([&events](const std::string& eventName) {
        events.push_back(eventName);
    });
    animator.SetStateMachine(stateMachine);
    animator.Tick(0.2f);

    EXPECT_EQ((std::vector<std::string>{"LoopStart"}), events);
    EXPECT_EQ("Idle", animator.GetCurrentStateName());
}

TEST(AnimationValidation, JobifiedEvaluatorMatchesSerialPose)
{
    ScopedJobSystem jobSystem;

    constexpr size_t kBoneCount = 48;
    auto skeleton = CreateLinearSkeleton(kBoneCount);
    auto clip = CreateTransformClip("DensePose", kBoneCount);

    Animation::SkeletonPose serialPose(skeleton);
    Animation::SkeletonPose jobifiedPose(skeleton);

    Animation::AnimationEvaluator evaluator;
    Animation::EvaluationResult serialResult =
        evaluator.Evaluate(*clip, Animation::SecondsToTimeUs(0.5), serialPose);

    Animation::EvaluationOptions jobifiedOptions;
    jobifiedOptions.jobifiedTransformEvaluation = true;
    jobifiedOptions.jobifiedMinTransformTrackCount = 1;
    jobifiedOptions.jobifiedBatchSize = 4;

    Animation::EvaluationResult jobifiedResult =
        evaluator.Evaluate(*clip, Animation::SecondsToTimeUs(0.5), jobifiedPose, jobifiedOptions);

    ASSERT_TRUE(serialResult.success);
    ASSERT_TRUE(jobifiedResult.success);
    EXPECT_FALSE(serialResult.usedJobifiedEvaluation);
    EXPECT_TRUE(jobifiedResult.usedJobifiedEvaluation);
    EXPECT_EQ(kBoneCount, jobifiedResult.evaluatedTransformTrackCount);

    for (size_t i = 0; i < kBoneCount; ++i)
    {
        const Vec3& serialTranslation = serialPose.GetLocalTransform(i).translation;
        const Vec3& jobifiedTranslation = jobifiedPose.GetLocalTransform(i).translation;
        EXPECT_NEAR(serialTranslation.x, jobifiedTranslation.x, 0.0001f);
        EXPECT_NEAR(serialTranslation.y, jobifiedTranslation.y, 0.0001f);
        EXPECT_NEAR(serialTranslation.z, jobifiedTranslation.z, 0.0001f);
    }
}

TEST(AnimationValidation, AnimatorComponentTickCanUseJobifiedPoseEvaluation)
{
    ScopedJobSystem jobSystem;

    constexpr size_t kBoneCount = 48;
    auto skeleton = CreateLinearSkeleton(kBoneCount);
    auto clip = CreateTransformClip("AnimatorDensePose", kBoneCount);

    auto stateMachine = Animation::AnimationStateMachine::Create(skeleton);
    auto idle = stateMachine->AddState("Idle");
    idle->SetMotion(clip);
    stateMachine->SetDefaultState("Idle");

    AnimatorComponent animator;
    animator.EnableJobifiedPoseEvaluation(true, 1, 4);
    animator.SetStateMachine(stateMachine);
    animator.Tick(0.5f);

    ASSERT_TRUE(animator.DidLastEvaluationUseJobifiedPoseEvaluation());
    const Animation::SkeletonPose* pose = animator.GetOutputPose();
    ASSERT_NE(nullptr, pose);

    const Vec3& sampledTranslation = pose->GetLocalTransform(10).translation;
    EXPECT_NEAR(15.0f, sampledTranslation.x, 0.0001f);
    EXPECT_NEAR(10.0f, sampledTranslation.y, 0.0001f);
    EXPECT_NEAR(0.5f, sampledTranslation.z, 0.0001f);
}
