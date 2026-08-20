/**
 * @file AnimationStateMachine.cpp
 * @brief AnimationStateMachine implementation
 */

#include "Animation/State/AnimationStateMachine.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace RVX::Animation
{

AnimationStateMachine::AnimationStateMachine(Skeleton::ConstPtr skeleton)
    : m_skeleton(skeleton)
    , m_outputPose(skeleton)
    , m_currentPose(skeleton)
    , m_nextPose(skeleton)
{
}

void AnimationStateMachine::SetSkeleton(Skeleton::ConstPtr skeleton)
{
    m_skeleton = skeleton;
    m_outputPose.SetSkeleton(skeleton);
    m_currentPose.SetSkeleton(skeleton);
    m_nextPose.SetSkeleton(skeleton);
}

AnimationState::Ptr AnimationStateMachine::AddState(const std::string& name)
{
    if (m_states.find(name) != m_states.end())
    {
        return m_states[name];
    }

    auto state = AnimationState::Create(name);
    state->SetId(static_cast<uint32_t>(m_states.size()));
    BindStateCallbacks(state.get());
    ApplyEvaluationSettings(state.get());
    m_states[name] = state;
    return state;
}

void AnimationStateMachine::SetOnAnimationEvent(AnimationEventCallback callback)
{
    m_onAnimationEvent = std::move(callback);

    for (auto& [name, state] : m_states)
    {
        (void)name;
        BindStateCallbacks(state.get());
    }
}

void AnimationStateMachine::BindStateCallbacks(AnimationState* state)
{
    if (!state)
    {
        return;
    }

    state->SetOnAnimationEvent([this](const AnimationEvent& event) {
        if (m_onAnimationEvent)
        {
            m_onAnimationEvent(event);
        }
    });
}

void AnimationStateMachine::EnableJobifiedPoseEvaluation(bool enable,
                                                         size_t minTransformTrackCount,
                                                         size_t batchSize)
{
    m_jobifiedPoseEvaluation = enable;
    m_jobifiedMinTransformTrackCount = minTransformTrackCount;
    m_jobifiedBatchSize = batchSize;

    for (auto& [name, state] : m_states)
    {
        (void)name;
        ApplyEvaluationSettings(state.get());
    }
}

void AnimationStateMachine::ApplyEvaluationSettings(AnimationState* state)
{
    if (!state)
    {
        return;
    }

    state->EnableJobifiedPoseEvaluation(m_jobifiedPoseEvaluation,
                                        m_jobifiedMinTransformTrackCount,
                                        m_jobifiedBatchSize);
}

void AnimationStateMachine::RemoveState(const std::string& name)
{
    auto it = m_states.find(name);
    if (it != m_states.end())
    {
        AnimationState* statePtr = it->second.get();
        
        // Remove transitions involving this state
        m_transitions.erase(
            std::remove_if(m_transitions.begin(), m_transitions.end(),
                [statePtr](const StateTransition::Ptr& t) {
                    return t->GetSourceState() == statePtr || 
                           t->GetDestinationState() == statePtr;
                }),
            m_transitions.end());

        // Clear current state if it's being removed
        if (m_currentState == statePtr) m_currentState = nullptr;
        if (m_nextState == statePtr) m_nextState = nullptr;
        if (m_defaultState == statePtr) m_defaultState = nullptr;

        m_states.erase(it);
    }
}

AnimationState* AnimationStateMachine::GetState(const std::string& name) const
{
    auto it = m_states.find(name);
    return (it != m_states.end()) ? it->second.get() : nullptr;
}

bool AnimationStateMachine::HasState(const std::string& name) const
{
    return m_states.find(name) != m_states.end();
}

std::vector<std::string> AnimationStateMachine::GetStateNames() const
{
    std::vector<std::string> names;
    names.reserve(m_states.size());
    for (const auto& [name, state] : m_states)
    {
        names.push_back(name);
    }
    return names;
}

void AnimationStateMachine::SetDefaultState(const std::string& name)
{
    m_defaultState = GetState(name);
}

void AnimationStateMachine::SetDefaultState(AnimationState* state)
{
    m_defaultState = state;
}

StateTransition::Ptr AnimationStateMachine::AddTransition(AnimationState* source, AnimationState* dest)
{
    auto transition = StateTransition::Create(source, dest);
    m_transitions.push_back(transition);
    return transition;
}

StateTransition::Ptr AnimationStateMachine::AddTransition(const std::string& sourceName, const std::string& destName)
{
    return AddTransition(GetState(sourceName), GetState(destName));
}

StateTransition::Ptr AnimationStateMachine::AddAnyStateTransition(AnimationState* dest)
{
    auto transition = StateTransition::Create(nullptr, dest);
    m_anyStateTransitions.push_back(transition);
    return transition;
}

StateTransition::Ptr AnimationStateMachine::AddAnyStateTransition(const std::string& destName)
{
    return AddAnyStateTransition(GetState(destName));
}

std::vector<StateTransition*> AnimationStateMachine::GetTransitionsFrom(AnimationState* state) const
{
    std::vector<StateTransition*> result;
    for (const auto& t : m_transitions)
    {
        if (t->GetSourceState() == state)
        {
            result.push_back(t.get());
        }
    }
    return result;
}

void AnimationStateMachine::SetFloat(const std::string& name, float value)
{
    m_context.SetParameter(name, value);
}

float AnimationStateMachine::GetFloat(const std::string& name, float defaultValue) const
{
    return m_context.GetParameter(name, defaultValue);
}

void AnimationStateMachine::SetBool(const std::string& name, bool value)
{
    m_context.SetParameter(name, value ? 1.0f : 0.0f);
}

bool AnimationStateMachine::GetBool(const std::string& name) const
{
    return m_context.GetParameter(name, 0.0f) > 0.5f;
}

void AnimationStateMachine::SetTrigger(const std::string& name)
{
    m_context.SetParameter(name, 1.0f);
    m_activeTriggers.insert(name);
}

void AnimationStateMachine::ResetTrigger(const std::string& name)
{
    m_context.SetParameter(name, 0.0f);
    m_activeTriggers.erase(name);
}

bool AnimationStateMachine::IsTriggerSet(const std::string& name) const
{
    return m_activeTriggers.find(name) != m_activeTriggers.end();
}

bool AnimationStateMachine::HasParameter(const std::string& name) const
{
    return m_context.parameters.find(name) != m_context.parameters.end();
}

std::vector<std::string> AnimationStateMachine::GetParameterNames() const
{
    std::vector<std::string> names;
    names.reserve(m_context.parameters.size());
    for (const auto& [name, value] : m_context.parameters)
    {
        names.push_back(name);
    }
    return names;
}

void AnimationStateMachine::ForceState(const std::string& name, float transitionDuration)
{
    ForceState(GetState(name), transitionDuration);
}

void AnimationStateMachine::ForceState(AnimationState* state, float transitionDuration)
{
    if (!state) return;

    if (transitionDuration <= 0.0f)
    {
        // Instant transition
        if (m_currentState)
        {
            m_currentState->Exit();
        }
        m_currentState = state;
        m_currentState->Enter();
        m_inTransition = false;
        m_nextState = nullptr;
        m_activeTransition = nullptr;
        m_forcedTransitionDuration = 0.0f;
        m_transitionElapsedUs = 0;
        m_transitionDurationUs = 0;
    }
    else
    {
        // Start transition
        m_nextState = state;
        m_inTransition = true;
        m_transitionProgress = 0.0f;
        m_activeTransition = nullptr;  // No formal transition object
        m_forcedTransitionDuration = transitionDuration;
        m_transitionElapsedUs = 0;
        m_transitionDurationUs = SecondsToTimeUs(static_cast<double>(transitionDuration));
        m_nextState->Enter();
    }
}

void AnimationStateMachine::Update(float deltaTime)
{
    if (!m_running || !m_currentState)
        return;

    m_context.deltaTime = deltaTime;

    // Update current state
    m_currentState->Update(m_context, deltaTime);

    // Update transition
    if (m_inTransition)
    {
        UpdateTransition(deltaTime);
    }
    else
    {
        // Check for new transitions
        CheckTransitions();
    }

    // Evaluate final pose
    EvaluatePose();

    // Reset triggers after evaluation
    ResetTriggersAfterEval();
}

bool AnimationStateMachine::UpdateTimeUs(TimeUs deltaTimeUs)
{
    if (deltaTimeUs < 0 || !m_running || !m_currentState ||
        !m_currentState->CanUpdateTimeUs(m_context, deltaTimeUs))
    {
        return false;
    }

    StateTransition* pendingTransition = nullptr;
    if (m_inTransition)
    {
        if (!m_nextState || !m_nextState->CanUpdateTimeUs(m_context, deltaTimeUs) ||
            m_transitionElapsedUs > std::numeric_limits<TimeUs>::max() - deltaTimeUs)
        {
            return false;
        }
    }
    else
    {
        const float projectedNormalizedTime =
            m_currentState->GetNormalizedTimeAfterUpdateUs(deltaTimeUs);
        pendingTransition = FindTransitionForNormalizedTime(m_currentState,
                                                             projectedNormalizedTime);
        if (pendingTransition &&
            (!pendingTransition->GetDestinationState() ||
             !pendingTransition->GetDestinationState()->CanUpdateTimeUs(m_context,
                                                                          deltaTimeUs)))
        {
            return false;
        }
    }

    m_context.deltaTime = static_cast<float>(TimeUsToSeconds(deltaTimeUs));
    if (!m_currentState->UpdateTimeUs(m_context, deltaTimeUs))
    {
        return false;
    }

    if (m_inTransition)
    {
        UpdateTransitionTimeUs(deltaTimeUs);
    }
    else if (pendingTransition)
    {
        StartTransition(pendingTransition);
    }

    EvaluatePose();
    ResetTriggersAfterEval();
    return true;
}

bool AnimationStateMachine::CanUpdateTimeUsWithoutTransition(TimeUs deltaTimeUs) const
{
    if (deltaTimeUs < 0 || !m_running || !m_currentState || m_inTransition ||
        !m_currentState->CanUpdateTimeUs(m_context, deltaTimeUs))
    {
        return false;
    }

    const float projectedNormalizedTime =
        m_currentState->GetNormalizedTimeAfterUpdateUs(deltaTimeUs);
    return FindTransitionForNormalizedTime(m_currentState, projectedNormalizedTime) == nullptr;
}

bool AnimationStateMachine::CanUpdateTimeUsWithRootMotionFreeTransition(
    TimeUs deltaTimeUs) const
{
    const auto supportsRootMotionFreeDirectClip = [this, deltaTimeUs](
                                                   const AnimationState* state) {
        if (!state || state->GetMotionType() != StateMotionType::Clip ||
            !state->GetClip() || state->GetClip()->hasRootMotion ||
            state->HasRootMotion() || state->GetSpeed() != 1.0f ||
            !state->GetSpeedParameter().empty())
        {
            return false;
        }
        return state->CanUpdateTimeUs(m_context, deltaTimeUs);
    };

    if (deltaTimeUs < 0 || !m_running || !supportsRootMotionFreeDirectClip(m_currentState))
    {
        return false;
    }

    if (m_inTransition)
    {
        return m_nextState && m_transitionDurationUs > 0 &&
               m_transitionElapsedUs <= std::numeric_limits<TimeUs>::max() - deltaTimeUs &&
               supportsRootMotionFreeDirectClip(m_nextState);
    }

    const float projectedNormalizedTime =
        m_currentState->GetNormalizedTimeAfterUpdateUs(deltaTimeUs);
    StateTransition* pendingTransition = FindTransitionForNormalizedTime(
        m_currentState,
        projectedNormalizedTime);
    if (!pendingTransition || !pendingTransition->GetDestinationState())
    {
        return false;
    }

    const float duration = pendingTransition->GetDuration();
    if (!std::isfinite(duration) || duration <= 0.0f ||
        pendingTransition->GetOffset() != 0.0f)
    {
        return false;
    }

    const TimeUs durationUs = SecondsToTimeUs(static_cast<double>(duration));
    return durationUs > 0 &&
           supportsRootMotionFreeDirectClip(pendingTransition->GetDestinationState());
}

bool AnimationStateMachine::CanUpdateTimeUsWithSuppressedRootMotionTransition(
    TimeUs deltaTimeUs) const
{
    const auto supportsUnitDirectClip = [this, deltaTimeUs](const AnimationState* state) {
        return state && state->GetMotionType() == StateMotionType::Clip &&
               state->GetClip() && state->GetSpeed() == 1.0f &&
               state->GetSpeedParameter().empty() &&
               state->CanUpdateTimeUs(m_context, deltaTimeUs);
    };

    // This path is intentionally active-transition-only. A newly projected
    // root-motion transition must remain fail-closed so root extraction never
    // changes authority within a fixed step.
    if (deltaTimeUs < 0 || !m_running || !m_inTransition ||
        m_transitionDurationUs != 250'000 || m_transitionElapsedUs >
            std::numeric_limits<TimeUs>::max() - deltaTimeUs ||
        !supportsUnitDirectClip(m_currentState) ||
        !supportsUnitDirectClip(m_nextState))
    {
        return false;
    }

    if (m_activeTransition && m_activeTransition->GetOffset() != 0.0f)
    {
        return false;
    }

    const AnimationClip::ConstPtr sourceClip = m_currentState->GetClip();
    const AnimationClip::ConstPtr destinationClip = m_nextState->GetClip();
    if (!sourceClip->hasRootMotion || !m_currentState->IsLooping() ||
        destinationClip->hasRootMotion || m_nextState->HasRootMotion() ||
        !m_skeleton || !sourceClip->skeleton || !destinationClip->skeleton ||
        sourceClip->skeleton.get() != m_skeleton.get() ||
        destinationClip->skeleton.get() != m_skeleton.get())
    {
        return false;
    }
    return true;
}

bool AnimationStateMachine::CanUpdateTimeUsWithIncomingRootMotionTransition(
    TimeUs deltaTimeUs) const
{
    const auto supportsUnitDirectClip = [this, deltaTimeUs](const AnimationState* state) {
        return state && state->GetMotionType() == StateMotionType::Clip &&
               state->GetClip() && state->GetSpeed() == 1.0f &&
               state->GetSpeedParameter().empty() &&
               state->CanUpdateTimeUs(m_context, deltaTimeUs);
    };
    const auto isQualifiedRootMotionClip = [this](const AnimationClip::ConstPtr& clip) {
        if (!clip || !clip->hasRootMotion || clip->rootMotionBoneName.empty() ||
            clip->duration <= 0 || !m_skeleton || !clip->skeleton ||
            clip->skeleton.get() != m_skeleton.get() ||
            m_skeleton->FindBoneIndex(clip->rootMotionBoneName) < 0)
        {
            return false;
        }

        return std::any_of(clip->transformTracks.begin(), clip->transformTracks.end(),
                           [&clip](const TransformTrack& track) {
            return track.targetType == TrackTargetType::Bone &&
                   track.targetName == clip->rootMotionBoneName && !track.IsEmpty();
        });
    };

    // This path is intentionally active-transition-only. A newly projected
    // ordinary-to-root-motion transition remains fail-closed so root-motion
    // authority cannot change within a fixed step.
    if (deltaTimeUs < 0 || !m_running || !m_inTransition ||
        m_transitionDurationUs != 250'000 || m_transitionElapsedUs >
            std::numeric_limits<TimeUs>::max() - deltaTimeUs ||
        !supportsUnitDirectClip(m_currentState) ||
        !supportsUnitDirectClip(m_nextState))
    {
        return false;
    }

    if (m_activeTransition && m_activeTransition->GetOffset() != 0.0f)
    {
        return false;
    }

    const AnimationClip::ConstPtr sourceClip = m_currentState->GetClip();
    const AnimationClip::ConstPtr destinationClip = m_nextState->GetClip();
    return !sourceClip->hasRootMotion && !m_currentState->HasRootMotion() &&
           sourceClip->skeleton && sourceClip->skeleton.get() == m_skeleton.get() &&
           m_nextState->IsLooping() && isQualifiedRootMotionClip(destinationClip);
}

void AnimationStateMachine::CheckTransitions()
{
    if (!m_currentState) return;

    StateTransition* validTransition = FindTransitionForNormalizedTime(
        m_currentState,
        m_currentState->GetNormalizedTime());
    if (validTransition)
    {
        StartTransition(validTransition);
    }
}

StateTransition* AnimationStateMachine::FindValidTransition(AnimationState* fromState)
{
    return FindValidTransition(fromState, fromState ? fromState->GetNormalizedTime() : 0.0f);
}

StateTransition* AnimationStateMachine::FindValidTransition(AnimationState* fromState,
                                                             float normalizedTime) const
{
    if (!fromState)
    {
        return nullptr;
    }

    std::vector<StateTransition*> candidates;

    for (const auto& t : m_transitions)
    {
        if (t->GetSourceState() == fromState &&
            t->CheckConditions(m_context, normalizedTime))
        {
            candidates.push_back(t.get());
        }
    }

    if (candidates.empty())
        return nullptr;

    // Sort by priority
    std::sort(candidates.begin(), candidates.end(),
        [](StateTransition* a, StateTransition* b) {
            return a->GetPriority() > b->GetPriority();
        });

    return candidates[0];
}

StateTransition* AnimationStateMachine::FindValidAnyStateTransition(float normalizedTime) const
{
    for (const auto& transition : m_anyStateTransitions)
    {
        if (transition->GetDestinationState() != m_currentState &&
            transition->CheckConditions(m_context, normalizedTime))
        {
            return transition.get();
        }
    }
    return nullptr;
}

StateTransition* AnimationStateMachine::FindTransitionForNormalizedTime(
    AnimationState* fromState,
    float normalizedTime) const
{
    if (StateTransition* anyStateTransition = FindValidAnyStateTransition(normalizedTime))
    {
        return anyStateTransition;
    }

    return FindValidTransition(fromState, normalizedTime);
}

void AnimationStateMachine::StartTransition(StateTransition* transition)
{
    if (!transition || !transition->GetDestinationState())
        return;

    m_activeTransition = transition;
    m_nextState = transition->GetDestinationState();
    m_inTransition = true;
    m_transitionProgress = 0.0f;
    m_forcedTransitionDuration = 0.0f;
    m_transitionElapsedUs = 0;
    m_transitionDurationUs = SecondsToTimeUs(static_cast<double>(transition->GetDuration()));

    // Enter next state
    m_nextState->Reset();
    m_nextState->Enter();

    // Apply offset
    if (transition->GetOffset() > 0.0f)
    {
        m_nextState->SetNormalizedTime(transition->GetOffset());
    }

    // Notify callback
    if (m_onStateChange)
    {
        m_onStateChange(m_currentState, m_nextState);
    }
}

void AnimationStateMachine::UpdateTransition(float deltaTime)
{
    if (!m_nextState) return;

    const float duration = m_activeTransition
        ? m_activeTransition->GetDuration()
        : m_forcedTransitionDuration;
    
    if (duration <= 0.0f)
    {
        CompleteTransition();
        return;
    }

    m_transitionProgress += deltaTime / duration;

    // Update next state
    m_nextState->Update(m_context, deltaTime);

    if (m_transitionProgress >= 1.0f)
    {
        CompleteTransition();
    }
}

void AnimationStateMachine::UpdateTransitionTimeUs(TimeUs deltaTimeUs)
{
    if (!m_nextState)
    {
        return;
    }

    if (m_transitionDurationUs <= 0)
    {
        CompleteTransition();
        return;
    }

    m_transitionElapsedUs += deltaTimeUs;
    const TimeUs clampedElapsed = std::min(m_transitionElapsedUs, m_transitionDurationUs);
    m_transitionProgress = static_cast<float>(clampedElapsed) /
        static_cast<float>(m_transitionDurationUs);

    m_nextState->UpdateTimeUs(m_context, deltaTimeUs);

    if (m_transitionElapsedUs >= m_transitionDurationUs)
    {
        CompleteTransition();
    }
}

void AnimationStateMachine::CompleteTransition()
{
    if (m_currentState)
    {
        m_currentState->Exit();
    }

    m_currentState = m_nextState;
    m_nextState = nullptr;
    m_activeTransition = nullptr;
    m_inTransition = false;
    m_transitionProgress = 0.0f;
    m_forcedTransitionDuration = 0.0f;
    m_transitionElapsedUs = 0;
    m_transitionDurationUs = 0;
}

void AnimationStateMachine::EvaluatePose()
{
    m_lastEvaluationUsedJobified = false;

    if (!m_currentState)
    {
        m_outputPose.ResetToBindPose();
        return;
    }

    if (m_inTransition && m_nextState)
    {
        // Blend between current and next state
        m_currentPose.ResetToBindPose();
        m_nextPose.ResetToBindPose();

        m_currentState->Evaluate(m_context, m_currentPose);
        m_lastEvaluationUsedJobified =
            m_lastEvaluationUsedJobified || m_currentState->DidLastEvaluationUseJobifiedPoseEvaluation();

        m_nextState->Evaluate(m_context, m_nextPose);
        m_lastEvaluationUsedJobified =
            m_lastEvaluationUsedJobified || m_nextState->DidLastEvaluationUseJobifiedPoseEvaluation();

        // Apply blend curve
        float t = m_transitionProgress;
        TransitionBlendMode blendMode = m_activeTransition ? 
            m_activeTransition->GetBlendMode() : TransitionBlendMode::Linear;

        switch (blendMode)
        {
            case TransitionBlendMode::Smooth:
                t = t * t * (3.0f - 2.0f * t);  // Smoothstep
                break;
            case TransitionBlendMode::Frozen:
                // Don't update current state pose
                break;
            default:
                break;
        }

        SkeletonPose::Blend(m_currentPose, m_nextPose, t, m_outputPose);
    }
    else
    {
        // Just evaluate current state
        m_outputPose.ResetToBindPose();
        m_currentState->Evaluate(m_context, m_outputPose);
        m_lastEvaluationUsedJobified = m_currentState->DidLastEvaluationUseJobifiedPoseEvaluation();
    }
}

void AnimationStateMachine::ResetTriggersAfterEval()
{
    for (const auto& trigger : m_activeTriggers)
    {
        m_context.SetParameter(trigger, 0.0f);
    }
    m_activeTriggers.clear();
}

void AnimationStateMachine::Reset()
{
    Stop();
    m_currentState = nullptr;
    m_nextState = nullptr;
    m_inTransition = false;
    m_activeTransition = nullptr;
    m_transitionProgress = 0.0f;
    m_forcedTransitionDuration = 0.0f;
    m_transitionElapsedUs = 0;
    m_transitionDurationUs = 0;
    m_outputPose.ResetToBindPose();
}

void AnimationStateMachine::Start()
{
    if (m_defaultState)
    {
        m_currentState = m_defaultState;
        m_currentState->Enter();
    }
    else if (!m_states.empty())
    {
        m_currentState = m_states.begin()->second.get();
        m_currentState->Enter();
    }
    m_running = true;
}

void AnimationStateMachine::Stop()
{
    m_running = false;
    if (m_currentState)
    {
        m_currentState->Exit();
    }
    if (m_nextState)
    {
        m_nextState->Exit();
    }
}

} // namespace RVX::Animation
