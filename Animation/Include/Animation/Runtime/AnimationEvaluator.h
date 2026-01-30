/**
 * @file AnimationEvaluator.h
 * @brief Animation clip evaluator - samples animations at a given time
 * 
 * The evaluator is responsible for sampling animation clips at specific
 * times and producing skeleton poses or transform values.
 */

#pragma once

#include "Animation/Core/Types.h"
#include "Animation/Core/Interpolation.h"
#include "Animation/Data/AnimationClip.h"
#include "Animation/Runtime/SkeletonPose.h"
#include <unordered_map>

namespace RVX::Animation
{

/**
 * @brief Options for animation evaluation
 */
struct EvaluationOptions
{
    /// Wrap mode override (nullopt = use clip default)
    std::optional<WrapMode> wrapModeOverride;
    
    /// Playback speed multiplier
    float speed = 1.0f;
    
    /// Root motion extraction mode
    bool extractRootMotion = false;
    
    /// Bone mask (optional, nullptr = all bones)
    const std::vector<float>* boneMask = nullptr;
};

/**
 * @brief Result of animation evaluation
 */
struct EvaluationResult
{
    /// Whether the evaluation was successful
    bool success = false;
    
    /// Whether the animation has finished (for Once mode)
    bool finished = false;
    
    /// Root motion delta (if extracted)
    TransformSample rootMotionDelta;
    
    /// Event triggers at this time (event names)
    std::vector<std::string> triggeredEvents;
};

/**
 * @brief Animation clip evaluator
 * 
 * Samples animation clips at specific times to produce poses.
 * Handles interpolation between keyframes and wrap modes.
 * 
 * Usage:
 * @code
 * AnimationEvaluator evaluator;
 * SkeletonPose pose(skeleton);
 * 
 * // Sample at a specific time
 * evaluator.Evaluate(clip, timeUs, pose);
 * 
 * // With options
 * EvaluationOptions opts;
 * opts.wrapModeOverride = WrapMode::Loop;
 * auto result = evaluator.Evaluate(clip, timeUs, pose, opts);
 * @endcode
 */
class AnimationEvaluator
{
public:
    AnimationEvaluator() = default;

    // =========================================================================
    // Primary Evaluation
    // =========================================================================

    /**
     * @brief Evaluate an animation clip at a given time
     * @param clip The animation clip to evaluate
     * @param time Time in microseconds
     * @param outPose Output pose (will be modified)
     * @param options Evaluation options
     * @return Evaluation result
     */
    EvaluationResult Evaluate(
        const AnimationClip& clip,
        TimeUs time,
        SkeletonPose& outPose,
        const EvaluationOptions& options = {});

    /**
     * @brief Evaluate and blend with existing pose
     * @param clip The animation clip
     * @param time Time in microseconds
     * @param weight Blend weight (0 = keep existing, 1 = full animation)
     * @param pose Pose to blend into
     * @param options Evaluation options
     */
    EvaluationResult EvaluateBlended(
        const AnimationClip& clip,
        TimeUs time,
        float weight,
        SkeletonPose& pose,
        const EvaluationOptions& options = {});

    /**
     * @brief Evaluate additive animation
     * @param clip Additive animation clip
     * @param time Time in microseconds
     * @param weight Additive weight
     * @param basePose Base pose to add to
     * @param options Evaluation options
     */
    EvaluationResult EvaluateAdditive(
        const AnimationClip& clip,
        TimeUs time,
        float weight,
        SkeletonPose& basePose,
        const EvaluationOptions& options = {});

    // =========================================================================
    // Track Evaluation
    // =========================================================================

    /**
     * @brief Evaluate a transform track at a given time
     * @param track The transform track
     * @param time Time in microseconds
     * @return Sampled transform
     */
    TransformSample EvaluateTransformTrack(
        const TransformTrack& track,
        TimeUs time);

    /**
     * @brief Evaluate a blend shape track
     * @param track The blend shape track
     * @param time Time in microseconds
     * @param outWeights Output blend shape weights
     */
    void EvaluateBlendShapeTrack(
        const BlendShapeTrack& track,
        TimeUs time,
        std::vector<float>& outWeights);

    /**
     * @brief Evaluate a property track
     * @param track The property track
     * @param time Time in microseconds
     * @param outValue Output value (as float)
     */
    float EvaluatePropertyTrack(
        const PropertyTrack& track,
        TimeUs time);

    /**
     * @brief Evaluate visibility track
     */
    bool EvaluateVisibilityTrack(
        const VisibilityTrack& track,
        TimeUs time);

    // =========================================================================
    // Caching for Performance
    // =========================================================================

    /**
     * @brief Enable keyframe hint caching for faster sequential playback
     */
    void EnableCaching(bool enable) { m_cachingEnabled = enable; }

    /**
     * @brief Clear cached keyframe hints
     */
    void ClearCache() { m_keyframeHints.clear(); }

    /**
     * @brief Set the cache key for the current evaluation context
     * This allows caching to work across multiple clips
     */
    void SetCacheContext(uint64_t contextId) { m_cacheContext = contextId; }

private:
    // Helper methods
    TimeUs ApplyTimeWrapping(TimeUs time, TimeUs duration, WrapMode mode);
    
    template<typename KeyframeType>
    int GetCachedHint(const std::vector<KeyframeType>& keyframes, uint64_t trackId);
    
    template<typename KeyframeType>
    void SetCachedHint(uint64_t trackId, int hint);

    // Sample individual channels
    Vec3 SampleTranslation(const TransformTrack& track, TimeUs time);
    Quat SampleRotation(const TransformTrack& track, TimeUs time);
    Vec3 SampleScale(const TransformTrack& track, TimeUs time);
    Mat4 SampleMatrix(const TransformTrack& track, TimeUs time);

    bool m_cachingEnabled = true;
    uint64_t m_cacheContext = 0;
    std::unordered_map<uint64_t, int> m_keyframeHints;
};

} // namespace RVX::Animation
