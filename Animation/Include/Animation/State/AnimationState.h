/**
 * @file AnimationState.h
 * @brief Animation state for state machine
 * 
 * A state represents a single animation behavior (idle, walk, attack, etc.)
 * and can contain a clip, blend tree, or sub-state machine.
 */

#pragma once

#include "Animation/Blend/BlendNode.h"
#include "Animation/Blend/BlendTree.h"
#include "Animation/Runtime/SkeletonPose.h"
#include "Animation/Core/Types.h"
#include <memory>
#include <string>
#include <functional>
#include <vector>

namespace RVX::Animation
{

class AnimationStateMachine;
class StateTransition;

/**
 * @brief Motion source type for a state
 */
enum class StateMotionType
{
    None,           ///< No motion (passthrough)
    Clip,           ///< Single animation clip
    BlendTree,      ///< Blend tree
    SubStateMachine ///< Nested state machine
};

/**
 * @brief Animation state
 * 
 * Represents a state in an animation state machine.
 * Each state produces a pose through its motion source.
 */
class AnimationState
{
public:
    using Ptr = std::shared_ptr<AnimationState>;

    // =========================================================================
    // Construction
    // =========================================================================

    AnimationState() = default;
    explicit AnimationState(const std::string& name);

    static Ptr Create(const std::string& name)
    {
        return std::make_shared<AnimationState>(name);
    }

    // =========================================================================
    // Identity
    // =========================================================================

    const std::string& GetName() const { return m_name; }
    void SetName(const std::string& name) { m_name = name; }

    /// Get unique state ID
    uint32_t GetId() const { return m_id; }
    void SetId(uint32_t id) { m_id = id; }

    // =========================================================================
    // Motion Source
    // =========================================================================

    /// Get motion type
    StateMotionType GetMotionType() const { return m_motionType; }

    /// Set motion as a single clip
    void SetMotion(AnimationClip::ConstPtr clip);

    /// Set motion as a blend tree
    void SetMotion(BlendTree::Ptr blendTree);

    /// Set motion as a blend node
    void SetMotion(BlendNodePtr blendNode);

    /// Get clip (if motion is Clip)
    AnimationClip::ConstPtr GetClip() const { return m_clip; }

    /// Get blend tree (if motion is BlendTree)
    BlendTree::Ptr GetBlendTree() const { return m_blendTree; }

    /// Get blend node
    BlendNodePtr GetBlendNode() const { return m_blendNode; }

    // =========================================================================
    // Speed & Properties
    // =========================================================================

    /// Playback speed multiplier
    float GetSpeed() const { return m_speed; }
    void SetSpeed(float speed) { m_speed = speed; }

    /// Parameter name for speed multiplier (optional)
    void SetSpeedParameter(const std::string& param) { m_speedParameter = param; }
    const std::string& GetSpeedParameter() const { return m_speedParameter; }

    /// Whether this state loops
    bool IsLooping() const { return m_loop; }
    void SetLooping(bool loop) { m_loop = loop; }

    /// Root motion
    bool HasRootMotion() const { return m_hasRootMotion; }
    void SetRootMotion(bool enabled) { m_hasRootMotion = enabled; }

    // =========================================================================
    // State Callbacks
    // =========================================================================

    using StateCallback = std::function<void(AnimationState*)>;

    void SetOnEnter(StateCallback callback) { m_onEnter = std::move(callback); }
    void SetOnExit(StateCallback callback) { m_onExit = std::move(callback); }
    void SetOnUpdate(StateCallback callback) { m_onUpdate = std::move(callback); }

    // =========================================================================
    // Evaluation
    // =========================================================================

    /**
     * @brief Enter this state
     */
    void Enter();

    /**
     * @brief Exit this state
     */
    void Exit();

    /**
     * @brief Update the state
     * @param context Blend context with parameters
     * @param deltaTime Time since last update
     */
    void Update(const BlendContext& context, float deltaTime);

    /**
     * @brief Evaluate and produce output pose
     */
    float Evaluate(const BlendContext& context, SkeletonPose& outPose);

    /**
     * @brief Get normalized time (0-1)
     */
    float GetNormalizedTime() const;

    /**
     * @brief Set normalized time
     */
    void SetNormalizedTime(float t);

    /**
     * @brief Check if state has finished (for non-looping)
     */
    bool IsFinished() const { return m_finished; }

    /**
     * @brief Get the duration/length of this state's motion
     */
    float GetLength() const;

    /**
     * @brief Reset state to beginning
     */
    void Reset();

    // =========================================================================
    // Tags
    // =========================================================================

    /// Add a tag
    void AddTag(const std::string& tag) { m_tags.push_back(tag); }

    /// Check if has tag
    bool HasTag(const std::string& tag) const;

    /// Get all tags
    const std::vector<std::string>& GetTags() const { return m_tags; }

private:
    std::string m_name;
    uint32_t m_id = 0;
    
    StateMotionType m_motionType = StateMotionType::None;
    AnimationClip::ConstPtr m_clip;
    BlendTree::Ptr m_blendTree;
    BlendNodePtr m_blendNode;
    
    float m_speed = 1.0f;
    std::string m_speedParameter;
    bool m_loop = true;
    bool m_hasRootMotion = false;
    bool m_finished = false;
    
    TimeUs m_currentTime = 0;
    
    StateCallback m_onEnter;
    StateCallback m_onExit;
    StateCallback m_onUpdate;
    
    std::vector<std::string> m_tags;
};

} // namespace RVX::Animation
