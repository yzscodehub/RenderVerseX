/**
 * @file AnimationStateMachine.h
 * @brief Animation state machine for controlling animation flow
 * 
 * The state machine manages states, transitions, and parameters
 * to produce a final animation pose.
 */

#pragma once

#include "Animation/State/AnimationState.h"
#include "Animation/State/StateTransition.h"
#include "Animation/Runtime/SkeletonPose.h"
#include "Animation/Blend/BlendNode.h"
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace RVX::Animation
{

/**
 * @brief Animation state machine
 * 
 * Manages a set of animation states and transitions between them.
 * Produces a final pose based on the current state and transition progress.
 * 
 * Usage:
 * @code
 * AnimationStateMachine fsm(skeleton);
 * 
 * // Create states
 * auto idle = fsm.AddState("Idle");
 * idle->SetMotion(idleClip);
 * 
 * auto walk = fsm.AddState("Walk");
 * walk->SetMotion(walkClip);
 * 
 * // Create transitions
 * auto toWalk = fsm.AddTransition(idle.get(), walk.get());
 * toWalk->AddCondition(TransitionCondition::FloatGreater("Speed", 0.1f));
 * 
 * auto toIdle = fsm.AddTransition(walk.get(), idle.get());
 * toIdle->AddCondition(TransitionCondition::FloatLess("Speed", 0.1f));
 * 
 * fsm.SetDefaultState("Idle");
 * 
 * // Each frame
 * fsm.SetFloat("Speed", currentSpeed);
 * fsm.Update(deltaTime);
 * 
 * const SkeletonPose& pose = fsm.GetOutputPose();
 * @endcode
 */
class AnimationStateMachine
{
public:
    using Ptr = std::shared_ptr<AnimationStateMachine>;

    // =========================================================================
    // Construction
    // =========================================================================

    AnimationStateMachine() = default;
    explicit AnimationStateMachine(Skeleton::ConstPtr skeleton);

    static Ptr Create(Skeleton::ConstPtr skeleton)
    {
        return std::make_shared<AnimationStateMachine>(skeleton);
    }

    // =========================================================================
    // Skeleton
    // =========================================================================

    void SetSkeleton(Skeleton::ConstPtr skeleton);
    Skeleton::ConstPtr GetSkeleton() const { return m_skeleton; }

    // =========================================================================
    // States
    // =========================================================================

    /**
     * @brief Add a new state
     * @param name State name (must be unique)
     * @return Created state
     */
    AnimationState::Ptr AddState(const std::string& name);

    /**
     * @brief Remove a state
     */
    void RemoveState(const std::string& name);

    /**
     * @brief Get a state by name
     */
    AnimationState* GetState(const std::string& name) const;

    /**
     * @brief Check if state exists
     */
    bool HasState(const std::string& name) const;

    /**
     * @brief Get all state names
     */
    std::vector<std::string> GetStateNames() const;

    /**
     * @brief Get state count
     */
    size_t GetStateCount() const { return m_states.size(); }

    // =========================================================================
    // Default State
    // =========================================================================

    /**
     * @brief Set the default/entry state
     */
    void SetDefaultState(const std::string& name);
    void SetDefaultState(AnimationState* state);

    /**
     * @brief Get the default state
     */
    AnimationState* GetDefaultState() const { return m_defaultState; }

    // =========================================================================
    // Transitions
    // =========================================================================

    /**
     * @brief Add a transition between states
     */
    StateTransition::Ptr AddTransition(AnimationState* source, AnimationState* dest);
    StateTransition::Ptr AddTransition(const std::string& sourceName, const std::string& destName);

    /**
     * @brief Add an "Any State" transition
     */
    StateTransition::Ptr AddAnyStateTransition(AnimationState* dest);
    StateTransition::Ptr AddAnyStateTransition(const std::string& destName);

    /**
     * @brief Get transitions from a state
     */
    std::vector<StateTransition*> GetTransitionsFrom(AnimationState* state) const;

    /**
     * @brief Get all transitions
     */
    const std::vector<StateTransition::Ptr>& GetTransitions() const { return m_transitions; }

    // =========================================================================
    // Parameters
    // =========================================================================

    /// Set a float parameter
    void SetFloat(const std::string& name, float value);
    float GetFloat(const std::string& name, float defaultValue = 0.0f) const;

    /// Set a bool parameter
    void SetBool(const std::string& name, bool value);
    bool GetBool(const std::string& name) const;

    /// Set a trigger (auto-reset bool)
    void SetTrigger(const std::string& name);
    void ResetTrigger(const std::string& name);
    bool IsTriggerSet(const std::string& name) const;

    /// Check if parameter exists
    bool HasParameter(const std::string& name) const;

    /// Get all parameter names
    std::vector<std::string> GetParameterNames() const;

    // =========================================================================
    // Current State
    // =========================================================================

    /**
     * @brief Get current state
     */
    AnimationState* GetCurrentState() const { return m_currentState; }

    /**
     * @brief Get next state (during transition)
     */
    AnimationState* GetNextState() const { return m_nextState; }

    /**
     * @brief Is a transition in progress
     */
    bool IsInTransition() const { return m_inTransition; }

    /**
     * @brief Get transition progress (0-1)
     */
    float GetTransitionProgress() const { return m_transitionProgress; }

    /**
     * @brief Force transition to a state (ignoring conditions)
     */
    void ForceState(const std::string& name, float transitionDuration = 0.0f);
    void ForceState(AnimationState* state, float transitionDuration = 0.0f);

    // =========================================================================
    // Update & Evaluate
    // =========================================================================

    /**
     * @brief Update the state machine
     * @param deltaTime Time since last update in seconds
     */
    void Update(float deltaTime);

    /**
     * @brief Advance direct clip states by an exact microsecond delta.
     *
     * Blend-tree states and non-unit effective speeds are intentionally not
     * supported by this deterministic path.
     */
    bool UpdateTimeUs(TimeUs deltaTimeUs);

    /**
     * @brief Check whether an exact microsecond update can complete without
     *        entering or advancing a transition.
     *
     * This is a side-effect-free gate for owners that require an atomic
     * direct-clip evaluation. It includes Any State transitions as well as
     * transitions from the current state.
     */
    bool CanUpdateTimeUsWithoutTransition(TimeUs deltaTimeUs) const;

    /**
     * @brief Check whether an exact update can deterministically enter or
     *        advance a root-motion-free, direct-clip blend.
     *
     * This deliberately supports only unit-speed clip states, non-zero blend
     * durations, and zero transition offsets.  Those constraints make the
     * preflight match the mutation path without speculatively changing either
     * state.  It returns false when no transition is involved.
     */
    bool CanUpdateTimeUsWithRootMotionFreeTransition(TimeUs deltaTimeUs) const;

    /**
     * @brief Check one active 250 ms root-motion-source to ordinary-clip
     *        blend for exact fixed evaluation.
     *
     * The caller suppresses world motion during the blend and removes the
     * qualified source root from the blended pose. This admits no pending,
     * reverse, root-motion destination, offset, speed, or blend-tree case.
     */
    bool CanUpdateTimeUsWithSuppressedRootMotionTransition(TimeUs deltaTimeUs) const;

    /**
     * @brief Check one active 250 ms ordinary-clip to root-motion-clip blend
     *        for exact fixed evaluation.
     *
     * The caller suppresses world motion during the blend and removes the
     * qualified destination root from the blended pose. This admits no
     * pending, reverse, offset, speed, skeleton-mismatch, or blend-tree case.
     */
    bool CanUpdateTimeUsWithIncomingRootMotionTransition(TimeUs deltaTimeUs) const;

    /**
     * @brief Get the output pose
     */
    const SkeletonPose& GetOutputPose() const { return m_outputPose; }
    SkeletonPose& GetOutputPose() { return m_outputPose; }

    // =========================================================================
    // Evaluation Settings
    // =========================================================================

    /// Enable JobSystem-backed transform track evaluation for clip states
    void EnableJobifiedPoseEvaluation(bool enable,
                                      size_t minTransformTrackCount = 32,
                                      size_t batchSize = 0);

    /// Check whether the most recent pose evaluation used the JobSystem path
    bool DidLastEvaluationUseJobifiedPoseEvaluation() const { return m_lastEvaluationUsedJobified; }

    /**
     * @brief Reset to default state
     */
    void Reset();

    /**
     * @brief Start the state machine (enter default state)
     */
    void Start();

    /**
     * @brief Stop the state machine
     */
    void Stop();

    /**
     * @brief Is the state machine running
     */
    bool IsRunning() const { return m_running; }

    // =========================================================================
    // Events
    // =========================================================================

    using StateChangeCallback = std::function<void(AnimationState* from, AnimationState* to)>;
    using AnimationEventCallback = std::function<void(const AnimationEvent& event)>;

    void SetOnStateChange(StateChangeCallback callback) { m_onStateChange = std::move(callback); }
    void SetOnAnimationEvent(AnimationEventCallback callback);

private:
    void BindStateCallbacks(AnimationState* state);
    void ApplyEvaluationSettings(AnimationState* state);
    void CheckTransitions();
    void StartTransition(StateTransition* transition);
    void UpdateTransition(float deltaTime);
    void UpdateTransitionTimeUs(TimeUs deltaTimeUs);
    void CompleteTransition();
    void EvaluatePose();
    void ResetTriggersAfterEval();

    StateTransition* FindValidTransition(AnimationState* fromState);
    StateTransition* FindValidTransition(AnimationState* fromState,
                                         float normalizedTime) const;
    StateTransition* FindValidAnyStateTransition(float normalizedTime) const;
    StateTransition* FindTransitionForNormalizedTime(AnimationState* fromState,
                                                      float normalizedTime) const;

    Skeleton::ConstPtr m_skeleton;

    // States
    std::unordered_map<std::string, AnimationState::Ptr> m_states;
    AnimationState* m_defaultState = nullptr;

    // Transitions
    std::vector<StateTransition::Ptr> m_transitions;
    std::vector<StateTransition::Ptr> m_anyStateTransitions;

    // Current state
    AnimationState* m_currentState = nullptr;
    AnimationState* m_nextState = nullptr;
    StateTransition* m_activeTransition = nullptr;
    bool m_inTransition = false;
    float m_transitionProgress = 0.0f;
    float m_forcedTransitionDuration = 0.0f;
    TimeUs m_transitionElapsedUs = 0;
    TimeUs m_transitionDurationUs = 0;

    // Parameters
    BlendContext m_context;
    std::unordered_set<std::string> m_activeTriggers;

    // Output
    SkeletonPose m_outputPose;
    SkeletonPose m_currentPose;
    SkeletonPose m_nextPose;
    bool m_jobifiedPoseEvaluation = false;
    size_t m_jobifiedMinTransformTrackCount = 32;
    size_t m_jobifiedBatchSize = 0;
    bool m_lastEvaluationUsedJobified = false;

    bool m_running = false;

    StateChangeCallback m_onStateChange;
    AnimationEventCallback m_onAnimationEvent;
};

} // namespace RVX::Animation
