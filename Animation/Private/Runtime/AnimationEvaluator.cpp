/**
 * @file AnimationEvaluator.cpp
 * @brief AnimationEvaluator implementation
 */

#include "Animation/Runtime/AnimationEvaluator.h"
#include "Animation/Core/Interpolation.h"

namespace RVX::Animation
{

EvaluationResult AnimationEvaluator::Evaluate(
    const AnimationClip& clip,
    TimeUs time,
    SkeletonPose& outPose,
    const EvaluationOptions& options)
{
    EvaluationResult result;
    result.success = true;

    // Apply wrap mode
    WrapMode wrapMode = options.wrapModeOverride.value_or(clip.defaultWrapMode);
    TimeUs wrappedTime = ApplyTimeWrapping(time, clip.duration, wrapMode);
    
    // Check if finished
    result.finished = IsAnimationFinished(time, clip.duration, wrapMode);

    // Evaluate transform tracks
    for (const auto& track : clip.transformTracks)
    {
        TransformSample sample = EvaluateTransformTrack(track, wrappedTime);

        // Find bone index
        int boneIndex = -1;
        auto skeleton = outPose.GetSkeleton();
        if (skeleton)
        {
            boneIndex = skeleton->FindBoneIndex(track.targetName);
        }

        if (boneIndex >= 0)
        {
            outPose.SetLocalTransform(static_cast<size_t>(boneIndex), sample);
        }
    }

    return result;
}

EvaluationResult AnimationEvaluator::EvaluateBlended(
    const AnimationClip& clip,
    TimeUs time,
    float weight,
    SkeletonPose& pose,
    const EvaluationOptions& options)
{
    if (weight <= 0.001f)
    {
        EvaluationResult result;
        result.success = true;
        return result;
    }

    if (weight >= 0.999f)
    {
        return Evaluate(clip, time, pose, options);
    }

    // Evaluate to temp pose and blend
    SkeletonPose tempPose(pose.GetSkeleton());
    tempPose.ResetToBindPose();

    auto result = Evaluate(clip, time, tempPose, options);
    
    if (result.success)
    {
        pose.BlendWith(tempPose, weight);
    }

    return result;
}

EvaluationResult AnimationEvaluator::EvaluateAdditive(
    const AnimationClip& clip,
    TimeUs time,
    float weight,
    SkeletonPose& basePose,
    const EvaluationOptions& options)
{
    if (weight <= 0.001f)
    {
        EvaluationResult result;
        result.success = true;
        return result;
    }

    SkeletonPose additivePose(basePose.GetSkeleton());
    additivePose.ResetToIdentity();  // Additive base is identity

    auto result = Evaluate(clip, time, additivePose, options);
    
    if (result.success)
    {
        basePose.AdditivelBlend(additivePose, weight);
    }

    return result;
}

TransformSample AnimationEvaluator::EvaluateTransformTrack(
    const TransformTrack& track,
    TimeUs time)
{
    TransformSample sample;

    // Check for matrix keyframes first (takes precedence)
    if (!track.matrixKeyframes.empty())
    {
        Mat4 matrix = SampleMatrix(track, time);
        return TransformSample::FromMatrix(matrix);
    }

    // Sample individual TRS components
    if (!track.translationKeyframes.empty())
    {
        sample.translation = SampleTranslation(track, time);
    }

    if (!track.rotationKeyframes.empty())
    {
        sample.rotation = SampleRotation(track, time);
    }

    if (!track.scaleKeyframes.empty())
    {
        sample.scale = SampleScale(track, time);
    }

    return sample;
}

Vec3 AnimationEvaluator::SampleTranslation(const TransformTrack& track, TimeUs time)
{
    const auto& keyframes = track.translationKeyframes;
    
    if (keyframes.empty())
        return Vec3(0.0f);
    
    if (keyframes.size() == 1)
        return keyframes[0].value;

    int indexA, indexB;
    float t;
    
    if (!FindKeyframePair(keyframes, time, indexA, indexB, t))
        return Vec3(0.0f);

    if (indexA == indexB)
        return keyframes[indexA].value;

    return Interpolate(keyframes[indexA], keyframes[indexB], t);
}

Quat AnimationEvaluator::SampleRotation(const TransformTrack& track, TimeUs time)
{
    const auto& keyframes = track.rotationKeyframes;
    
    if (keyframes.empty())
        return Quat(1.0f, 0.0f, 0.0f, 0.0f);
    
    if (keyframes.size() == 1)
        return keyframes[0].value;

    int indexA, indexB;
    float t;
    
    if (!FindKeyframePair(keyframes, time, indexA, indexB, t))
        return Quat(1.0f, 0.0f, 0.0f, 0.0f);

    if (indexA == indexB)
        return keyframes[indexA].value;

    return Interpolate(keyframes[indexA], keyframes[indexB], t);
}

Vec3 AnimationEvaluator::SampleScale(const TransformTrack& track, TimeUs time)
{
    const auto& keyframes = track.scaleKeyframes;
    
    if (keyframes.empty())
        return Vec3(1.0f);
    
    if (keyframes.size() == 1)
        return keyframes[0].value;

    int indexA, indexB;
    float t;
    
    if (!FindKeyframePair(keyframes, time, indexA, indexB, t))
        return Vec3(1.0f);

    if (indexA == indexB)
        return keyframes[indexA].value;

    return Interpolate(keyframes[indexA], keyframes[indexB], t);
}

Mat4 AnimationEvaluator::SampleMatrix(const TransformTrack& track, TimeUs time)
{
    const auto& keyframes = track.matrixKeyframes;
    
    if (keyframes.empty())
        return Mat4(1.0f);
    
    if (keyframes.size() == 1)
        return keyframes[0].value;

    int indexA, indexB;
    float t;
    
    if (!FindKeyframePair(keyframes, time, indexA, indexB, t))
        return Mat4(1.0f);

    if (indexA == indexB)
        return keyframes[indexA].value;

    return Interpolate(keyframes[indexA], keyframes[indexB], t);
}

void AnimationEvaluator::EvaluateBlendShapeTrack(
    const BlendShapeTrack& track,
    TimeUs time,
    std::vector<float>& outWeights)
{
    outWeights.clear();
    outWeights.reserve(track.weightsKeyframes.size());

    for (size_t i = 0; i < track.weightsKeyframes.size(); ++i)
    {
        if (i < track.weightsKeyframes.size() && !track.weightsKeyframes[i].empty())
        {
            const auto& keyframes = track.weightsKeyframes[i];
            
            if (keyframes.size() == 1)
            {
                outWeights.push_back(keyframes[0].value);
                continue;
            }

            int indexA, indexB;
            float t;
            
            if (FindKeyframePair(keyframes, time, indexA, indexB, t))
            {
                float value = Interpolate(keyframes[indexA], keyframes[indexB], t);
                outWeights.push_back(value);
            }
            else
            {
                outWeights.push_back(0.0f);
            }
        }
        else
        {
            outWeights.push_back(0.0f);
        }
    }
}

float AnimationEvaluator::EvaluatePropertyTrack(const PropertyTrack& track, TimeUs time)
{
    const auto& keyframes = track.floatKeyframes;
    
    if (keyframes.empty())
        return 0.0f;
    
    if (keyframes.size() == 1)
        return keyframes[0].value;

    int indexA, indexB;
    float t;
    
    if (!FindKeyframePair(keyframes, time, indexA, indexB, t))
        return 0.0f;

    if (indexA == indexB)
        return keyframes[indexA].value;

    return Interpolate(keyframes[indexA], keyframes[indexB], t);
}

bool AnimationEvaluator::EvaluateVisibilityTrack(const VisibilityTrack& track, TimeUs time)
{
    const auto& keyframes = track.keyframes;
    
    if (keyframes.empty())
        return true;

    // Find the keyframe at or before time
    int index = FindKeyframeIndex(keyframes, time);
    
    if (index < 0)
        return keyframes[0].value;  // Before first keyframe

    return keyframes[index].value;
}

TimeUs AnimationEvaluator::ApplyTimeWrapping(TimeUs time, TimeUs duration, WrapMode mode)
{
    return Animation::ApplyWrapMode(time, duration, mode);
}

} // namespace RVX::Animation
