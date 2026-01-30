/**
 * @file AnimationPlayer.h
 * @brief Animation playback controller
 * 
 * The AnimationPlayer manages the playback of animation clips, including:
 * - Time progression
 * - Play/Pause/Stop controls
 * - Speed and direction control
 * - Looping behavior
 * - Crossfade transitions
 */

#pragma once

#include "Animation/Core/Types.h"
#include "Animation/Data/AnimationClip.h"
#include "Animation/Runtime/SkeletonPose.h"
#include "Animation/Runtime/AnimationEvaluator.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace RVX::Animation
{

/**
 * @brief Animation playback instance for a single clip
 */
struct PlaybackInstance
{
    AnimationClip::ConstPtr clip;
    TimeUs currentTime = 0;
    float speed = 1.0f;
    float weight = 1.0f;         // Blend weight
    WrapMode wrapMode = WrapMode::Loop;
    PlaybackState state = PlaybackState::Stopped;
    bool isAdditive = false;
    
    // Transition state
    float fadeInDuration = 0.0f;  // Seconds
    float fadeOutDuration = 0.0f;
    float fadeProgress = 0.0f;    // 0-1, for fade in/out
    bool isFadingIn = false;
    bool isFadingOut = false;
    
    // Events
    std::function<void()> onComplete;
    std::function<void()> onLoop;
    
    bool IsPlaying() const { return state == PlaybackState::Playing; }
    bool IsStopped() const { return state == PlaybackState::Stopped; }
    bool IsPaused() const { return state == PlaybackState::Paused; }
    
    float GetNormalizedTime() const
    {
        if (!clip || clip->duration <= 0) return 0.0f;
        return static_cast<float>(currentTime) / static_cast<float>(clip->duration);
    }
};

/**
 * @brief Animation player for skeleton animations
 * 
 * Manages playback of one or more animation clips on a skeleton.
 * Supports crossfading, layering, and event callbacks.
 * 
 * Usage:
 * @code
 * AnimationPlayer player(skeleton);
 * 
 * // Simple playback
 * player.Play(idleClip);
 * 
 * // Crossfade to another animation
 * player.CrossFade(walkClip, 0.3f); // 0.3 second crossfade
 * 
 * // Update each frame
 * player.Update(deltaTime);
 * 
 * // Get the result pose
 * const SkeletonPose& pose = player.GetPose();
 * @endcode
 */
class AnimationPlayer
{
public:
    using Ptr = std::shared_ptr<AnimationPlayer>;

    // =========================================================================
    // Construction
    // =========================================================================

    AnimationPlayer() = default;
    explicit AnimationPlayer(Skeleton::ConstPtr skeleton);

    static Ptr Create(Skeleton::ConstPtr skeleton)
    {
        return std::make_shared<AnimationPlayer>(skeleton);
    }

    // =========================================================================
    // Skeleton
    // =========================================================================

    void SetSkeleton(Skeleton::ConstPtr skeleton);
    Skeleton::ConstPtr GetSkeleton() const { return m_skeleton; }

    // =========================================================================
    // Playback Control
    // =========================================================================

    /**
     * @brief Play an animation clip
     * @param clip The clip to play
     * @param fadeInTime Fade in duration in seconds (0 = instant)
     * @return Instance ID for this playback
     */
    uint32_t Play(AnimationClip::ConstPtr clip, float fadeInTime = 0.0f);

    /**
     * @brief Play with specific settings
     */
    uint32_t Play(AnimationClip::ConstPtr clip, WrapMode wrapMode, 
                  float speed = 1.0f, float fadeInTime = 0.0f);

    /**
     * @brief Crossfade to a new animation
     * @param clip The new animation
     * @param fadeDuration Duration of the crossfade in seconds
     */
    uint32_t CrossFade(AnimationClip::ConstPtr clip, float fadeDuration);

    /**
     * @brief Stop the current animation
     * @param fadeOutTime Fade out duration (0 = instant)
     */
    void Stop(float fadeOutTime = 0.0f);

    /**
     * @brief Stop a specific playback instance
     */
    void Stop(uint32_t instanceId, float fadeOutTime = 0.0f);

    /**
     * @brief Pause playback
     */
    void Pause();

    /**
     * @brief Resume paused playback
     */
    void Resume();

    /**
     * @brief Stop all animations
     */
    void StopAll(float fadeOutTime = 0.0f);

    // =========================================================================
    // Playback State
    // =========================================================================

    /// Check if any animation is playing
    bool IsPlaying() const;

    /// Check if a specific instance is playing
    bool IsPlaying(uint32_t instanceId) const;

    /// Get the primary playback state
    PlaybackState GetState() const;

    /// Get current playback time
    TimeUs GetCurrentTime() const;

    /// Get normalized time (0-1)
    float GetNormalizedTime() const;

    /// Set normalized time (0-1)
    void SetNormalizedTime(float normalizedTime);

    /// Get current clip
    AnimationClip::ConstPtr GetCurrentClip() const;

    // =========================================================================
    // Speed Control
    // =========================================================================

    /// Set playback speed (1.0 = normal, negative = reverse)
    void SetSpeed(float speed);
    float GetSpeed() const { return m_globalSpeed; }

    /// Set speed for a specific instance
    void SetSpeed(uint32_t instanceId, float speed);

    // =========================================================================
    // Update
    // =========================================================================

    /**
     * @brief Update the player and evaluate animations
     * @param deltaTime Time elapsed since last update in seconds
     */
    void Update(float deltaTime);

    /**
     * @brief Get the current evaluated pose
     */
    const SkeletonPose& GetPose() const { return m_currentPose; }
    SkeletonPose& GetPose() { return m_currentPose; }

    /**
     * @brief Force pose recalculation
     */
    void InvalidatePose() { m_poseDirty = true; }

    // =========================================================================
    // Layers (Additive)
    // =========================================================================

    /**
     * @brief Play an additive animation on top of the base
     * @param clip Additive animation clip
     * @param weight Additive weight
     * @return Instance ID
     */
    uint32_t PlayAdditive(AnimationClip::ConstPtr clip, float weight = 1.0f);

    /**
     * @brief Set additive animation weight
     */
    void SetAdditiveWeight(uint32_t instanceId, float weight);

    // =========================================================================
    // Event Callbacks
    // =========================================================================

    using EventCallback = std::function<void(const std::string& eventName)>;
    using CompletionCallback = std::function<void(uint32_t instanceId)>;

    /// Set callback for animation events
    void SetEventCallback(EventCallback callback) { m_eventCallback = std::move(callback); }

    /// Set callback for animation completion
    void SetCompletionCallback(CompletionCallback callback) { m_completionCallback = std::move(callback); }

    // =========================================================================
    // Instance Access
    // =========================================================================

    /// Get all active playback instances
    const std::vector<PlaybackInstance>& GetInstances() const { return m_instances; }

    /// Get a specific instance
    PlaybackInstance* GetInstance(uint32_t instanceId);
    const PlaybackInstance* GetInstance(uint32_t instanceId) const;

    /// Get number of active instances
    size_t GetInstanceCount() const { return m_instances.size(); }

private:
    void UpdateInstance(PlaybackInstance& instance, float deltaTime);
    void EvaluateAndBlend();
    void CleanupFinishedInstances();
    uint32_t GenerateInstanceId();

    Skeleton::ConstPtr m_skeleton;
    AnimationEvaluator m_evaluator;
    
    std::vector<PlaybackInstance> m_instances;
    std::vector<PlaybackInstance> m_additiveInstances;
    
    SkeletonPose m_currentPose;
    SkeletonPose m_tempPose;      // Temp pose for blending
    
    float m_globalSpeed = 1.0f;
    bool m_poseDirty = true;
    
    uint32_t m_nextInstanceId = 1;
    
    EventCallback m_eventCallback;
    CompletionCallback m_completionCallback;
};

} // namespace RVX::Animation
