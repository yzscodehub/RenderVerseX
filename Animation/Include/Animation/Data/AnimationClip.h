/**
 * @file AnimationClip.h
 * @brief Animation clip container
 * 
 * Migrated from found::animation::AnimationClip
 */

#pragma once

#include "Animation/Core/Types.h"
#include "Animation/Core/Keyframe.h"
#include "Animation/Data/AnimationTrack.h"
#include "Animation/Data/Skeleton.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace RVX::Animation
{

/**
 * @brief Source format information
 */
enum class AnimationSourceFormat : uint8_t
{
    Unknown,
    FBX,
    glTF,
    Alembic,
    USD,
    Custom
};

/**
 * @brief Animation clip metadata
 */
struct AnimationClipMetadata
{
    int sourceFps{30};
    int sourceStartFrame{0};
    int sourceEndFrame{0};
    AnimationSourceFormat sourceFormat{AnimationSourceFormat::Unknown};
    std::string sourceFile;
    std::unordered_map<std::string, std::string> customData;
};

/**
 * @brief Animation clip - container for animation data
 */
struct AnimationClip
{
    using Ptr = std::shared_ptr<AnimationClip>;
    using ConstPtr = std::shared_ptr<const AnimationClip>;

    // ========================================================================
    // Identity
    // ========================================================================

    std::string name;
    std::string description;

    // ========================================================================
    // Time Info
    // ========================================================================

    TimeUs duration{0};
    WrapMode defaultWrapMode{WrapMode::Loop};
    float defaultSpeed{1.0f};

    // ========================================================================
    // Metadata
    // ========================================================================

    AnimationClipMetadata metadata;

    // ========================================================================
    // Tracks
    // ========================================================================

    std::vector<TransformTrack> transformTracks;
    std::vector<BlendShapeTrack> blendShapeTracks;
    std::vector<PropertyTrack> propertyTracks;
    std::vector<VisibilityTrack> visibilityTracks;

    // ========================================================================
    // Optional Skeleton Reference
    // ========================================================================

    Skeleton::ConstPtr skeleton;

    // ========================================================================
    // Construction
    // ========================================================================

    AnimationClip() = default;
    explicit AnimationClip(const std::string& clipName) : name(clipName) {}

    static Ptr Create(const std::string& clipName = "")
    {
        return std::make_shared<AnimationClip>(clipName);
    }

    // ========================================================================
    // Track Management
    // ========================================================================

    void AddTransformTrack(TransformTrack track)
    {
        transformTracks.push_back(std::move(track));
        UpdateDuration();
    }

    void AddBlendShapeTrack(BlendShapeTrack track)
    {
        blendShapeTracks.push_back(std::move(track));
        UpdateDuration();
    }

    void AddPropertyTrack(PropertyTrack track)
    {
        propertyTracks.push_back(std::move(track));
        UpdateDuration();
    }

    void AddVisibilityTrack(VisibilityTrack track)
    {
        visibilityTracks.push_back(std::move(track));
        UpdateDuration();
    }

    // ========================================================================
    // Query
    // ========================================================================

    size_t GetTotalTrackCount() const
    {
        return transformTracks.size() + blendShapeTracks.size() +
               propertyTracks.size() + visibilityTracks.size();
    }

    bool IsEmpty() const { return GetTotalTrackCount() == 0; }
    bool HasAnimationData() const { return !IsEmpty() && duration > 0; }

    double GetDurationSeconds() const { return TimeUsToSeconds(duration); }
    void SetDurationSeconds(double seconds) { duration = SecondsToTimeUs(seconds); }

    // ========================================================================
    // Track Lookup
    // ========================================================================

    const TransformTrack* FindTransformTrack(const std::string& targetName) const
    {
        for (const auto& track : transformTracks)
        {
            if (track.targetName == targetName) return &track;
        }
        return nullptr;
    }

    TransformTrack* FindTransformTrack(const std::string& targetName)
    {
        for (auto& track : transformTracks)
        {
            if (track.targetName == targetName) return &track;
        }
        return nullptr;
    }

    // ========================================================================
    // Animation Type Detection
    // ========================================================================

    bool HasSkeletalAnimation() const
    {
        for (const auto& track : transformTracks)
        {
            if (track.targetType == TrackTargetType::Bone) return true;
        }
        return false;
    }

    bool HasNodeAnimation() const
    {
        for (const auto& track : transformTracks)
        {
            if (track.targetType == TrackTargetType::Node) return true;
        }
        return false;
    }

    bool HasBlendShapeAnimation() const { return !blendShapeTracks.empty(); }
    bool HasPropertyAnimation() const { return !propertyTracks.empty(); }
    bool HasVisibilityAnimation() const { return !visibilityTracks.empty(); }

    // ========================================================================
    // Validation
    // ========================================================================

    std::vector<std::string> ValidateAgainstSkeleton(const Skeleton& skel) const
    {
        std::vector<std::string> invalidBones;
        for (const auto& track : transformTracks)
        {
            if (track.targetType == TrackTargetType::Bone)
            {
                if (skel.FindBoneIndex(track.targetName) < 0)
                {
                    invalidBones.push_back(track.targetName);
                }
            }
        }
        return invalidBones;
    }

    // ========================================================================
    // Utility
    // ========================================================================

    void SortAllKeyframes()
    {
        for (auto& track : transformTracks)
        {
            SortKeyframes(track.translationKeyframes);
            SortKeyframes(track.rotationKeyframes);
            SortKeyframes(track.scaleKeyframes);
            SortKeyframes(track.matrixKeyframes);
        }

        for (auto& track : blendShapeTracks)
        {
            for (auto& kfs : track.weightsKeyframes)
            {
                SortKeyframes(kfs);
            }
        }

        for (auto& track : propertyTracks)
        {
            SortKeyframes(track.floatKeyframes);
            SortKeyframes(track.vec3Keyframes);
            SortKeyframes(track.vec4Keyframes);
            SortKeyframes(track.boolKeyframes);
        }

        for (auto& track : visibilityTracks)
        {
            SortKeyframes(track.keyframes);
        }
    }

    void UpdateDuration()
    {
        TimeUs maxTime = 0;

        auto checkRange = [&maxTime](const auto& track) {
            auto [start, end] = track.GetTimeRange();
            maxTime = std::max(maxTime, end);
        };

        for (const auto& track : transformTracks) checkRange(track);
        for (const auto& track : blendShapeTracks) checkRange(track);
        for (const auto& track : propertyTracks) checkRange(track);
        for (const auto& track : visibilityTracks) checkRange(track);

        duration = maxTime;
    }

    Ptr Clone() const { return std::make_shared<AnimationClip>(*this); }
};

// ============================================================================
// Animation Library
// ============================================================================

/**
 * @brief Collection of animation clips
 */
struct AnimationLibrary
{
    using Ptr = std::shared_ptr<AnimationLibrary>;

    std::string name;
    std::vector<AnimationClip::Ptr> clips;
    std::unordered_map<std::string, size_t> clipMap;
    Skeleton::Ptr skeleton;

    void AddClip(AnimationClip::Ptr clip)
    {
        if (!clip) return;
        clipMap[clip->name] = clips.size();
        clips.push_back(std::move(clip));
    }

    AnimationClip::Ptr GetClip(const std::string& clipName) const
    {
        auto it = clipMap.find(clipName);
        if (it != clipMap.end() && it->second < clips.size())
        {
            return clips[it->second];
        }
        return nullptr;
    }

    AnimationClip::Ptr GetClip(size_t index) const
    {
        if (index < clips.size()) return clips[index];
        return nullptr;
    }

    std::vector<std::string> GetClipNames() const
    {
        std::vector<std::string> names;
        names.reserve(clips.size());
        for (const auto& clip : clips) names.push_back(clip->name);
        return names;
    }

    bool HasClip(const std::string& clipName) const
    {
        return clipMap.find(clipName) != clipMap.end();
    }

    size_t GetClipCount() const { return clips.size(); }
    bool IsEmpty() const { return clips.empty(); }

    static Ptr Create(const std::string& libraryName = "")
    {
        auto lib = std::make_shared<AnimationLibrary>();
        lib->name = libraryName;
        return lib;
    }
};

} // namespace RVX::Animation
