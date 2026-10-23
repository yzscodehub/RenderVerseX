/**
 * @file RootMotion.cpp
 * @brief Root motion extraction implementation
 */

#include "Animation/Runtime/RootMotion.h"
#include "Animation/Core/Interpolation.h"
#include <cmath>

namespace RVX::Animation
{

// ============================================================================
// RootMotionExtractor Implementation
// ============================================================================

RootMotionExtractor::RootMotionExtractor(Skeleton::ConstPtr skeleton)
    : m_skeleton(skeleton)
{
    if (m_skeleton)
    {
        AutoDetectRootBone();
    }
}

void RootMotionExtractor::SetSkeleton(Skeleton::ConstPtr skeleton)
{
    m_skeleton = skeleton;
    m_referenceValid = false;
    
    if (m_skeleton)
    {
        AutoDetectRootBone();
    }
}

void RootMotionExtractor::SetConfig(const RootMotionConfig& config)
{
    m_config = config;
    m_referenceValid = false;
    
    // Resolve bone name to index if needed
    if (!m_config.rootBoneName.empty() && m_skeleton)
    {
        m_config.rootBoneIndex = m_skeleton->FindBoneIndex(m_config.rootBoneName);
    }
}

void RootMotionExtractor::SetRootBone(const std::string& boneName)
{
    m_config.rootBoneName = boneName;
    
    if (m_skeleton)
    {
        m_config.rootBoneIndex = m_skeleton->FindBoneIndex(boneName);
    }
    
    m_referenceValid = false;
}

void RootMotionExtractor::SetRootBone(int boneIndex)
{
    m_config.rootBoneIndex = boneIndex;
    
    if (m_skeleton && boneIndex >= 0 && 
        boneIndex < static_cast<int>(m_skeleton->GetBoneCount()))
    {
        m_config.rootBoneName = m_skeleton->bones[boneIndex].name;
    }
    
    m_referenceValid = false;
}

void RootMotionExtractor::AutoDetectRootBone()
{
    if (!m_skeleton || m_skeleton->IsEmpty())
    {
        m_config.rootBoneIndex = -1;
        return;
    }

    // Use the first root bone
    if (!m_skeleton->rootBoneIndices.empty())
    {
        m_config.rootBoneIndex = m_skeleton->rootBoneIndices[0];
        m_config.rootBoneName = m_skeleton->bones[m_config.rootBoneIndex].name;
    }
    else
    {
        // Fallback to first bone
        m_config.rootBoneIndex = 0;
        m_config.rootBoneName = m_skeleton->bones[0].name;
    }
    
    m_referenceValid = false;
}

RootMotionDelta RootMotionExtractor::ExtractDelta(
    const AnimationClip& clip,
    TimeUs previousTime,
    TimeUs currentTime) const
{
    if (!m_config.enabled)
        return RootMotionDelta();

    // Get root bone transforms at both times
    TransformSample previousRoot = SampleRootTransform(clip, previousTime);
    TransformSample currentRoot = SampleRootTransform(clip, currentTime);

    // Calculate deltas
    Vec3 deltaTranslation = currentRoot.translation - previousRoot.translation;
    
    // For rotation, compute relative rotation: current * inverse(previous)
    Quat previousInverse = glm::conjugate(previousRoot.rotation);
    Quat deltaRotation = normalize(currentRoot.rotation * previousInverse);

    // Apply filtering based on config
    deltaTranslation = FilterTranslation(deltaTranslation);
    deltaRotation = FilterRotation(deltaRotation);

    // Apply scale
    deltaTranslation *= m_config.motionScale;

    RootMotionDelta result;
    result.deltaTranslation = deltaTranslation;
    result.deltaRotation = deltaRotation;
    result.valid = true;

    return result;
}

RootMotionDelta RootMotionExtractor::ExtractAbsolute(
    const AnimationClip& clip,
    TimeUs time) const
{
    if (!m_config.enabled)
        return RootMotionDelta();

    // Get root bone transform at start and current time
    TransformSample startRoot = SampleRootTransform(clip, 0);
    TransformSample currentRoot = SampleRootTransform(clip, time);

    // Calculate deltas from start
    Vec3 deltaTranslation = currentRoot.translation - startRoot.translation;
    
    Quat startInverse = glm::conjugate(startRoot.rotation);
    Quat deltaRotation = normalize(currentRoot.rotation * startInverse);

    // Apply filtering
    deltaTranslation = FilterTranslation(deltaTranslation);
    deltaRotation = FilterRotation(deltaRotation);

    // Apply scale
    deltaTranslation *= m_config.motionScale;

    RootMotionDelta result;
    result.deltaTranslation = deltaTranslation;
    result.deltaRotation = deltaRotation;
    result.valid = true;

    return result;
}

RootMotionDelta RootMotionExtractor::ExtractTotal(const AnimationClip& clip) const
{
    return ExtractAbsolute(clip, clip.duration);
}

RootMotionDelta RootMotionExtractor::ExtractLoopingDelta(
    const AnimationClip& clip,
    TimeUs previousTime,
    TimeUs currentTime) const
{
    Skeleton::ConstPtr qualifiedSkeleton;
    const TransformTrack* rootTrack = nullptr;
    int rootBoneIndex = -1;
    if (!m_config.enabled || previousTime < 0 || currentTime < previousTime ||
        !ResolveQualifiedRootMotion(clip, qualifiedSkeleton, rootBoneIndex, rootTrack))
    {
        return RootMotionDelta();
    }

    const TimeUs duration = clip.duration;
    const TimeUs previousCycle = previousTime / duration;
    const TimeUs currentCycle = currentTime / duration;
    const TimeUs previousOffset = previousTime % duration;
    const TimeUs currentOffset = currentTime % duration;

    const auto makeSegmentDelta = [this, rootTrack](TimeUs start, TimeUs end) {
        const TransformSample startSample = rootTrack->Sample(start);
        const TransformSample endSample = rootTrack->Sample(end);

        RootMotionDelta result;
        result.deltaTranslation = FilterTranslation(endSample.translation - startSample.translation) *
            m_config.motionScale;
        result.deltaRotation = FilterRotation(
            normalize(endSample.rotation * glm::conjugate(startSample.rotation)));
        result.valid = true;
        return result;
    };

    if (previousCycle == currentCycle)
    {
        return makeSegmentDelta(previousOffset, currentOffset);
    }

    RootMotionDelta result = makeSegmentDelta(previousOffset, duration);
    const RootMotionDelta fullCycle = makeSegmentDelta(0, duration);

    uint64_t completeCycles = static_cast<uint64_t>(currentCycle - previousCycle - 1);
    RootMotionDelta repeatedCycle = RootMotionDelta::Identity();
    RootMotionDelta cyclePower = fullCycle;
    while (completeCycles > 0)
    {
        if ((completeCycles & 1u) != 0u)
        {
            repeatedCycle += cyclePower;
        }
        cyclePower += cyclePower;
        completeCycles >>= 1u;
    }

    result += repeatedCycle;
    result += makeSegmentDelta(0, currentOffset);
    return result;
}

TransformSample RootMotionExtractor::SampleRootTransform(
    const AnimationClip& clip,
    TimeUs time) const
{
    int rootIndex = FindRootBoneIndex();
    if (rootIndex < 0)
        return TransformSample::Identity();

    // Find root bone track
    const TransformTrack* rootTrack = nullptr;
    
    if (m_skeleton)
    {
        const Bone* rootBone = m_skeleton->GetBone(rootIndex);
        if (rootBone)
        {
            rootTrack = clip.FindTransformTrack(rootBone->name);
        }
    }
    
    // Fallback: try to find by index
    if (!rootTrack && rootIndex < static_cast<int>(clip.transformTracks.size()))
    {
        for (const auto& track : clip.transformTracks)
        {
            if (track.targetType == TrackTargetType::Bone)
            {
                // Check if this track matches our root
                if (m_skeleton)
                {
                    int trackBoneIndex = m_skeleton->FindBoneIndex(track.targetName);
                    if (trackBoneIndex == rootIndex)
                    {
                        rootTrack = &track;
                        break;
                    }
                }
            }
        }
    }

    if (!rootTrack)
        return TransformSample::Identity();

    // Sample the track
    return rootTrack->Sample(time);
}

Vec3 RootMotionExtractor::FilterTranslation(const Vec3& translation) const
{
    switch (m_config.translationMode)
    {
        case RootMotionTranslationMode::None:
            return Vec3(0.0f);

        case RootMotionTranslationMode::XZ:
            return Vec3(translation.x, 0.0f, translation.z);

        case RootMotionTranslationMode::XYZ:
            return translation;

        case RootMotionTranslationMode::XOnly:
            return Vec3(translation.x, 0.0f, 0.0f);

        case RootMotionTranslationMode::ZOnly:
            return Vec3(0.0f, 0.0f, translation.z);

        default:
            return translation;
    }
}

Quat RootMotionExtractor::FilterRotation(const Quat& rotation) const
{
    switch (m_config.rotationMode)
    {
        case RootMotionRotationMode::None:
            return Quat(1.0f, 0.0f, 0.0f, 0.0f);

        case RootMotionRotationMode::YawOnly:
        {
            // Extract yaw (Y-axis rotation) only
            Vec3 euler = glm::eulerAngles(rotation);
            return Quat(Vec3(0.0f, euler.y, 0.0f));
        }

        case RootMotionRotationMode::Full:
            return rotation;

        default:
            return rotation;
    }
}

int RootMotionExtractor::FindRootBoneIndex() const
{
    // Use configured index if valid
    if (m_config.rootBoneIndex >= 0)
        return m_config.rootBoneIndex;

    // Try to find by name
    if (!m_config.rootBoneName.empty() && m_skeleton)
    {
        return m_skeleton->FindBoneIndex(m_config.rootBoneName);
    }

    // Use first root bone from skeleton
    if (m_skeleton && !m_skeleton->rootBoneIndices.empty())
    {
        return m_skeleton->rootBoneIndices[0];
    }

    return -1;
}

bool RootMotionExtractor::ResolveQualifiedRootMotion(
    const AnimationClip& clip,
    Skeleton::ConstPtr& skeleton,
    int& rootBoneIndex,
    const TransformTrack*& rootTrack) const
{
    skeleton.reset();
    rootBoneIndex = -1;
    rootTrack = nullptr;

    if (!clip.hasRootMotion || clip.rootMotionBoneName.empty() || clip.duration <= 0)
    {
        return false;
    }

    skeleton = clip.skeleton ? clip.skeleton : m_skeleton;
    if (!skeleton || (clip.skeleton && m_skeleton && clip.skeleton.get() != m_skeleton.get()))
    {
        return false;
    }

    rootBoneIndex = skeleton->FindBoneIndex(clip.rootMotionBoneName);
    if (rootBoneIndex < 0)
    {
        return false;
    }

    for (const TransformTrack& track : clip.transformTracks)
    {
        if (track.targetType == TrackTargetType::Bone &&
            track.targetName == clip.rootMotionBoneName && !track.IsEmpty())
        {
            rootTrack = &track;
            return true;
        }
    }

    return false;
}

void RootMotionExtractor::ZeroRootMotion(
    TransformSample& rootTransform,
    bool keepVertical) const
{
    if (!m_config.zeroRootBone)
        return;

    // Zero out translation based on mode
    switch (m_config.translationMode)
    {
        case RootMotionTranslationMode::XZ:
            rootTransform.translation.x = 0.0f;
            rootTransform.translation.z = 0.0f;
            if (!keepVertical)
                rootTransform.translation.y = 0.0f;
            break;

        case RootMotionTranslationMode::XYZ:
            if (keepVertical)
            {
                rootTransform.translation.x = 0.0f;
                rootTransform.translation.z = 0.0f;
            }
            else
            {
                rootTransform.translation = Vec3(0.0f);
            }
            break;

        case RootMotionTranslationMode::XOnly:
            rootTransform.translation.x = 0.0f;
            break;

        case RootMotionTranslationMode::ZOnly:
            rootTransform.translation.z = 0.0f;
            break;

        default:
            break;
    }

    // Zero out rotation based on mode
    switch (m_config.rotationMode)
    {
        case RootMotionRotationMode::YawOnly:
        {
            // Keep pitch and roll, zero yaw
            Vec3 euler = rootTransform.GetEulerAngles();
            euler.y = 0.0f;
            rootTransform.SetEulerAngles(euler);
            break;
        }

        case RootMotionRotationMode::Full:
            rootTransform.rotation = Quat(1.0f, 0.0f, 0.0f, 0.0f);
            break;

        default:
            break;
    }
}

bool RootMotionExtractor::RemoveRootMotionFromPose(
    const AnimationClip& clip,
    SkeletonPose& pose,
    bool keepVertical) const
{
    Skeleton::ConstPtr qualifiedSkeleton;
    const TransformTrack* rootTrack = nullptr;
    int rootBoneIndex = -1;
    if (!ResolveQualifiedRootMotion(clip, qualifiedSkeleton, rootBoneIndex, rootTrack) ||
        !clip.skeleton || pose.GetSkeleton().get() != clip.skeleton.get() ||
        qualifiedSkeleton.get() != pose.GetSkeleton().get() || rootBoneIndex < 0 ||
        static_cast<size_t>(rootBoneIndex) >= pose.GetBoneCount())
    {
        return false;
    }

    const TransformSample evaluatedRoot = pose.GetLocalTransform(static_cast<size_t>(rootBoneIndex));
    TransformSample replacement = qualifiedSkeleton->bones[rootBoneIndex].localBindPose;
    if (keepVertical)
    {
        replacement.translation.y = evaluatedRoot.translation.y;
    }

    pose.SetLocalTransform(static_cast<size_t>(rootBoneIndex), replacement);
    return true;
}

void RootMotionExtractor::ApplyRootMotion(
    TransformSample& rootTransform,
    const RootMotionDelta& delta) const
{
    if (!delta.valid)
        return;

    rootTransform.translation += delta.deltaTranslation;
    rootTransform.rotation = normalize(delta.deltaRotation * rootTransform.rotation);
}

TransformSample RootMotionExtractor::GetRootSampleFromClip(
    const AnimationClip& clip,
    TimeUs time) const
{
    return SampleRootTransform(clip, time);
}

} // namespace RVX::Animation
