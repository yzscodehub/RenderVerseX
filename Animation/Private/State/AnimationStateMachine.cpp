/**
 * @file AnimationStateMachine.cpp
 * @brief AnimationStateMachine implementation
 */

#include "Animation/State/AnimationStateMachine.h"
#include <algorithm>

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
    m_states[name] = state;
    return state;
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
    }
    else
    {
        // Start transition
        m_nextState = state;
        m_inTransition = true;
        m_transitionProgress = 0.0f;
        m_activeTransition = nullptr;  // No formal transition object
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

void AnimationStateMachine::CheckTransitions()
{
    if (!m_currentState) return;

    float normalizedTime = m_currentState->GetNormalizedTime();

    // Check any-state transitions first (higher priority)
    for (const auto& t : m_anyStateTransitions)
    {
        if (t->GetDestinationState() != m_currentState &&
            t->CheckConditions(m_context, normalizedTime))
        {
            StartTransition(t.get());
            return;
        }
    }

    // Find valid transition from current state
    StateTransition* validTransition = FindValidTransition(m_currentState);
    if (validTransition)
    {
        StartTransition(validTransition);
    }
}

StateTransition* AnimationStateMachine::FindValidTransition(AnimationState* fromState)
{
    std::vector<StateTransition*> candidates;
    float normalizedTime = fromState->GetNormalizedTime();

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

void AnimationStateMachine::StartTransition(StateTransition* transition)
{
    if (!transition || !transition->GetDestinationState())
        return;

    m_activeTransition = transition;
    m_nextState = transition->GetDestinationState();
    m_inTransition = true;
    m_transitionProgress = 0.0f;

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

    float duration = m_activeTransition ? m_activeTransition->GetDuration() : 0.25f;
    
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
}

void AnimationStateMachine::EvaluatePose()
{
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
        m_nextState->Evaluate(m_context, m_nextPose);

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
