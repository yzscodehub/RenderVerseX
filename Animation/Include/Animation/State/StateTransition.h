/**
 * @file StateTransition.h
 * @brief Transition between animation states
 * 
 * Defines how to transition from one state to another,
 * including conditions, duration, and blend settings.
 */

#pragma once

#include "Animation/Core/Types.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace RVX::Animation
{

class AnimationState;
struct BlendContext;

/**
 * @brief Transition blend mode
 */
enum class TransitionBlendMode
{
    Linear,         ///< Linear crossfade
    Smooth,         ///< Smooth (ease in/out)
    Frozen          ///< Freeze source, blend to target
};

/**
 * @brief Transition interrupt behavior
 */
enum class TransitionInterrupt
{
    None,               ///< Cannot be interrupted
    CurrentState,       ///< Can interrupt with transitions from current state
    AnyState,           ///< Can interrupt with any state transitions
    CurrentThenNext     ///< Try current state first, then next state
};

/**
 * @brief Condition for state transition
 */
struct TransitionCondition
{
    enum class Type
    {
        Float,      ///< Compare float parameter
        Bool,       ///< Check bool parameter
        Trigger,    ///< Trigger (auto-reset bool)
        ExitTime    ///< Animation exit time
    };

    enum class Comparison
    {
        Greater,
        Less,
        Equals,
        NotEquals,
        GreaterOrEqual,
        LessOrEqual
    };

    Type type = Type::Float;
    std::string parameterName;
    Comparison comparison = Comparison::Greater;
    float threshold = 0.0f;

    /// Factory methods
    static TransitionCondition FloatGreater(const std::string& param, float value)
    {
        TransitionCondition c;
        c.type = Type::Float;
        c.parameterName = param;
        c.comparison = Comparison::Greater;
        c.threshold = value;
        return c;
    }

    static TransitionCondition FloatLess(const std::string& param, float value)
    {
        TransitionCondition c;
        c.type = Type::Float;
        c.parameterName = param;
        c.comparison = Comparison::Less;
        c.threshold = value;
        return c;
    }

    static TransitionCondition BoolTrue(const std::string& param)
    {
        TransitionCondition c;
        c.type = Type::Bool;
        c.parameterName = param;
        c.threshold = 1.0f;
        return c;
    }

    static TransitionCondition BoolFalse(const std::string& param)
    {
        TransitionCondition c;
        c.type = Type::Bool;
        c.parameterName = param;
        c.threshold = 0.0f;
        return c;
    }

    static TransitionCondition Trigger(const std::string& param)
    {
        TransitionCondition c;
        c.type = Type::Trigger;
        c.parameterName = param;
        return c;
    }

    static TransitionCondition AtExitTime(float normalizedTime = 0.95f)
    {
        TransitionCondition c;
        c.type = Type::ExitTime;
        c.threshold = normalizedTime;
        return c;
    }
};

/**
 * @brief State transition definition
 */
class StateTransition
{
public:
    using Ptr = std::shared_ptr<StateTransition>;

    // =========================================================================
    // Construction
    // =========================================================================

    StateTransition() = default;
    StateTransition(AnimationState* source, AnimationState* destination);

    static Ptr Create(AnimationState* source, AnimationState* destination)
    {
        return std::make_shared<StateTransition>(source, destination);
    }

    // =========================================================================
    // States
    // =========================================================================

    AnimationState* GetSourceState() const { return m_sourceState; }
    AnimationState* GetDestinationState() const { return m_destinationState; }

    void SetSourceState(AnimationState* state) { m_sourceState = state; }
    void SetDestinationState(AnimationState* state) { m_destinationState = state; }

    /// Is this an "Any State" transition
    bool IsAnyState() const { return m_sourceState == nullptr; }

    // =========================================================================
    // Conditions
    // =========================================================================

    /// Add a condition
    void AddCondition(const TransitionCondition& condition);

    /// Remove all conditions
    void ClearConditions();

    /// Get conditions
    const std::vector<TransitionCondition>& GetConditions() const { return m_conditions; }

    /// Check if all conditions are met
    bool CheckConditions(const BlendContext& context, float normalizedTime) const;

    // =========================================================================
    // Transition Settings
    // =========================================================================

    /// Transition duration in seconds
    float GetDuration() const { return m_duration; }
    void SetDuration(float duration) { m_duration = duration; }

    /// Offset into destination animation (0-1)
    float GetOffset() const { return m_offset; }
    void SetOffset(float offset) { m_offset = offset; }

    /// Blend mode
    TransitionBlendMode GetBlendMode() const { return m_blendMode; }
    void SetBlendMode(TransitionBlendMode mode) { m_blendMode = mode; }

    /// Interrupt behavior
    TransitionInterrupt GetInterruptBehavior() const { return m_interruptBehavior; }
    void SetInterruptBehavior(TransitionInterrupt behavior) { m_interruptBehavior = behavior; }

    /// Exit time (when HasExitTime is true)
    bool HasExitTime() const { return m_hasExitTime; }
    void SetHasExitTime(bool has) { m_hasExitTime = has; }

    float GetExitTime() const { return m_exitTime; }
    void SetExitTime(float time) { m_exitTime = time; }

    /// Can this transition be triggered
    bool IsEnabled() const { return m_enabled; }
    void SetEnabled(bool enabled) { m_enabled = enabled; }

    // =========================================================================
    // Priority
    // =========================================================================

    /// Priority when multiple transitions are valid (higher = checked first)
    int GetPriority() const { return m_priority; }
    void SetPriority(int priority) { m_priority = priority; }

private:
    AnimationState* m_sourceState = nullptr;
    AnimationState* m_destinationState = nullptr;

    std::vector<TransitionCondition> m_conditions;

    float m_duration = 0.25f;
    float m_offset = 0.0f;
    float m_exitTime = 0.0f;
    bool m_hasExitTime = false;
    bool m_enabled = true;
    int m_priority = 0;

    TransitionBlendMode m_blendMode = TransitionBlendMode::Linear;
    TransitionInterrupt m_interruptBehavior = TransitionInterrupt::None;
};

} // namespace RVX::Animation
