/** @file AnimationResource.cpp  @brief Immutable animation resource implementation. */

#include "Resource/Types/AnimationResource.h"

#include "Resource/Cooked/CookedAnimationArtifact.h"

#include <bit>
#include <cmath>
#include <exception>
#include <utility>

namespace RVX::Resource
{
namespace
{
    template<typename T>
    size_t VectorMemory(const std::vector<T>& values)
    {
        return values.capacity() * sizeof(T);
    }

    bool IsFinite(const Vec3& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) &&
               std::isfinite(value.z);
    }

    bool IsFinite(const Quat& value)
    {
        return std::isfinite(value.w) && std::isfinite(value.x) &&
               std::isfinite(value.y) && std::isfinite(value.z);
    }

    bool IsFinite(const Mat4& value)
    {
        for (uint32 column = 0; column < 4; ++column)
        {
            for (uint32 row = 0; row < 4; ++row)
            {
                if (!std::isfinite(value[column][row]))
                    return false;
            }
        }
        return true;
    }

    uint32 CanonicalFloatBits(float32 value)
    {
        return std::bit_cast<uint32>(value == 0.0f ? 0.0f : value);
    }

    bool HasEqualFiniteValue(float32 source, float32 target)
    {
        return std::isfinite(source) && std::isfinite(target) &&
               CanonicalFloatBits(source) == CanonicalFloatBits(target);
    }

    bool HasEqualFiniteValue(const Vec3& source, const Vec3& target)
    {
        return IsFinite(source) && IsFinite(target) &&
               HasEqualFiniteValue(source.x, target.x) &&
               HasEqualFiniteValue(source.y, target.y) &&
               HasEqualFiniteValue(source.z, target.z);
    }

    bool HasEqualFiniteValue(const Quat& source, const Quat& target)
    {
        return IsFinite(source) && IsFinite(target) &&
               HasEqualFiniteValue(source.w, target.w) &&
               HasEqualFiniteValue(source.x, target.x) &&
               HasEqualFiniteValue(source.y, target.y) &&
               HasEqualFiniteValue(source.z, target.z);
    }

    bool HasEqualFiniteValue(const Mat4& source, const Mat4& target)
    {
        if (!IsFinite(source) || !IsFinite(target))
            return false;

        for (uint32 column = 0; column < 4; ++column)
        {
            for (uint32 row = 0; row < 4; ++row)
            {
                if (!HasEqualFiniteValue(source[column][row], target[column][row]))
                    return false;
            }
        }
        return true;
    }

    /**
     * @brief Match the cooked-animation writer's deterministic quaternion
     *        representation before publishing an immutable runtime payload.
     *
     * SetData validates finiteness and unit length before this is called.  The
     * sign choice below intentionally uses the same w/x/y/z first-nonzero
     * rule as CookedAnimationArtifact::WriteQuat.  Normalize signed zero after
     * choosing the sign as ByteWriter::F32 does on the wire.
     */
    void CanonicalizePublishedQuaternion(Quat& value)
    {
        bool isCanonical = false;
        const float32 components[] = {value.w, value.x, value.y, value.z};
        for (const float32 component : components)
        {
            if (component > 0.0f)
            {
                isCanonical = true;
                break;
            }
            if (component < 0.0f)
                break;
        }

        if (!isCanonical)
        {
            value.w = -value.w;
            value.x = -value.x;
            value.y = -value.y;
            value.z = -value.z;
        }

        if (value.w == 0.0f) value.w = 0.0f;
        if (value.x == 0.0f) value.x = 0.0f;
        if (value.y == 0.0f) value.y = 0.0f;
        if (value.z == 0.0f) value.z = 0.0f;
    }

    bool HasConsistentTopology(const Animation::Skeleton& skeleton)
    {
        if (skeleton.bones.empty() ||
            skeleton.boneNameMap.size() != skeleton.bones.size())
        {
            return false;
        }

        std::vector<std::vector<int>> expectedChildren(skeleton.bones.size());
        std::vector<int> expectedRoots;
        for (size_t index = 0; index < skeleton.bones.size(); ++index)
        {
            const Animation::Bone& bone = skeleton.bones[index];
            if (bone.name.empty() || bone.parentIndex < -1 ||
                bone.parentIndex >= static_cast<int>(index) ||
                skeleton.FindBoneIndex(bone.name) != static_cast<int>(index))
            {
                return false;
            }

            if (bone.parentIndex < 0)
                expectedRoots.push_back(static_cast<int>(index));
            else
                expectedChildren[static_cast<size_t>(bone.parentIndex)].push_back(
                    static_cast<int>(index));
        }

        if (skeleton.rootBoneIndices != expectedRoots)
            return false;

        for (size_t index = 0; index < skeleton.bones.size(); ++index)
        {
            if (skeleton.bones[index].childIndices != expectedChildren[index])
                return false;
        }
        return true;
    }

    bool AreSkeletonsCompatible(const Animation::Skeleton& source,
                                const Animation::Skeleton& target)
    {
        if (source.bones.size() != target.bones.size() ||
            !HasConsistentTopology(source) || !HasConsistentTopology(target))
        {
            return false;
        }

        for (size_t index = 0; index < source.bones.size(); ++index)
        {
            const Animation::Bone& sourceBone = source.bones[index];
            const Animation::Bone& targetBone = target.bones[index];
            if (sourceBone.name != targetBone.name ||
                sourceBone.parentIndex != targetBone.parentIndex ||
                !HasEqualFiniteValue(sourceBone.localBindPose.translation,
                                     targetBone.localBindPose.translation) ||
                !HasEqualFiniteValue(sourceBone.localBindPose.rotation,
                                     targetBone.localBindPose.rotation) ||
                !HasEqualFiniteValue(sourceBone.localBindPose.scale,
                                     targetBone.localBindPose.scale) ||
                !HasEqualFiniteValue(sourceBone.inverseBindPose,
                                     targetBone.inverseBindPose))
            {
                return false;
            }
        }
        return true;
    }
} // namespace

bool AnimationResource::SetData(Animation::Skeleton::ConstPtr skeleton,
                                AnimationClipMap clips)
{
    std::string validationError;
    if (!ValidateCookedAnimationPayload(skeleton, clips, validationError))
    {
        return false;
    }

    try
    {
        // A shared_ptr<const T> does not make a T immutable when an importer
        // retains a shared_ptr<T> alias. Copy the payload before publication so
        // callers cannot mutate a resource after its admission validation.
        auto frozenSkeleton = std::make_shared<Animation::Skeleton>();
        frozenSkeleton->Reserve(skeleton->bones.size());
        for (const Animation::Bone& sourceBone : skeleton->bones)
        {
            Animation::Bone frozenBone = sourceBone;
            CanonicalizePublishedQuaternion(frozenBone.localBindPose.rotation);
            frozenBone.childIndices.clear();
            frozenSkeleton->AddBone(frozenBone);
        }

        AnimationClipMap frozenClips;
        for (const auto& [name, sourceClip] : clips)
        {
            Animation::AnimationClip::Ptr frozenClip = sourceClip->Clone();
            frozenClip->skeleton = frozenSkeleton;
            for (Animation::TransformTrack& track : frozenClip->transformTracks)
            {
                for (Animation::KeyframeQuat& keyframe : track.rotationKeyframes)
                    CanonicalizePublishedQuaternion(keyframe.value);
            }
            frozenClips.emplace(name, std::move(frozenClip));
        }

        const Animation::Skeleton::ConstPtr immutableSkeleton = frozenSkeleton;
        if (!ValidateCookedAnimationPayload(
                immutableSkeleton, frozenClips, validationError))
        {
            return false;
        }

        m_skeleton = immutableSkeleton;
        m_clips = std::move(frozenClips);
        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
    catch (...)
    {
        return false;
    }
}

Animation::AnimationClip::ConstPtr
AnimationResource::GetClip(const std::string& name) const
{
    const auto it = m_clips.find(name);
    return it == m_clips.end() ? nullptr : it->second;
}

bool AnimationResource::CloneClipForSkeleton(
    const std::string& name,
    Animation::Skeleton::ConstPtr targetSkeleton,
    Animation::AnimationClip::ConstPtr& outClip) const
{
    outClip.reset();

    const Animation::AnimationClip::ConstPtr sourceClip = GetClip(name);
    if (!sourceClip || !m_skeleton || !targetSkeleton ||
        sourceClip->skeleton != m_skeleton ||
        !AreSkeletonsCompatible(*m_skeleton, *targetSkeleton))
    {
        return false;
    }

    try
    {
        Animation::AnimationClip::Ptr clone = sourceClip->Clone();
        clone->skeleton = std::move(targetSkeleton);
        outClip = std::move(clone);
        return true;
    }
    catch (const std::exception&)
    {
        outClip.reset();
        return false;
    }
    catch (...)
    {
        outClip.reset();
        return false;
    }
}

size_t AnimationResource::GetMemoryUsage() const
{
    size_t total = sizeof(*this);
    if (m_skeleton)
    {
        total += sizeof(Animation::Skeleton);
        total += VectorMemory(m_skeleton->bones);
        total += VectorMemory(m_skeleton->rootBoneIndices);
        for (const Animation::Bone& bone : m_skeleton->bones)
        {
            total += bone.name.capacity();
            total += VectorMemory(bone.childIndices);
        }
    }

    for (const auto& [key, clip] : m_clips)
    {
        total += key.capacity();
        if (!clip)
            continue;

        total += sizeof(Animation::AnimationClip);
        total += clip->name.capacity() + clip->description.capacity();
        total += clip->metadata.sourceFile.capacity();
        for (const auto& [metadataKey, metadataValue] : clip->metadata.customData)
            total += metadataKey.capacity() + metadataValue.capacity();
        total += VectorMemory(clip->transformTracks);
        for (const Animation::TransformTrack& track : clip->transformTracks)
        {
            total += track.targetName.capacity();
            total += VectorMemory(track.translationKeyframes);
            total += VectorMemory(track.rotationKeyframes);
            total += VectorMemory(track.scaleKeyframes);
            total += VectorMemory(track.matrixKeyframes);
        }
        total += VectorMemory(clip->blendShapeTracks);
        total += VectorMemory(clip->propertyTracks);
        total += VectorMemory(clip->visibilityTracks);
        total += VectorMemory(clip->eventTrack.events);
        total += clip->rootMotionBoneName.capacity();
    }
    return total;
}
} // namespace RVX::Resource
