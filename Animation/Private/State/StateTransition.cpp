/**
 * @file StateTransition.cpp
 * @brief StateTransition implementation
 */

#include "Animation/State/StateTransition.h"
#include "Animation/Blend/BlendNode.h"

namespace RVX::Animation
{

StateTransition::StateTransition(AnimationState* source, AnimationState* destination)
    : m_sourceState(source)
    , m_destinationState(destination)
{
}

void StateTransition::AddCondition(const TransitionCondition& condition)
{
    m_conditions.push_back(condition);
}

void StateTransition::ClearConditions()
{
    m_conditions.clear();
}

bool StateTransition::CheckConditions(const BlendContext& context, float normalizedTime) const
{
    if (!m_enabled)
        return false;

    // All conditions must be true (AND logic)
    for (const auto& condition : m_conditions)
    {
        bool conditionMet = false;

        switch (condition.type)
        {
            case TransitionCondition::Type::Float:
            {
                float value = context.GetParameter(condition.parameterName, 0.0f);
                switch (condition.comparison)
                {
                    case TransitionCondition::Comparison::Greater:
                        conditionMet = value > condition.threshold;
                        break;
                    case TransitionCondition::Comparison::Less:
                        conditionMet = value < condition.threshold;
                        break;
                    case TransitionCondition::Comparison::Equals:
                        conditionMet = std::abs(value - condition.threshold) < 0.0001f;
                        break;
                    case TransitionCondition::Comparison::NotEquals:
                        conditionMet = std::abs(value - condition.threshold) >= 0.0001f;
                        break;
                    case TransitionCondition::Comparison::GreaterOrEqual:
                        conditionMet = value >= condition.threshold;
                        break;
                    case TransitionCondition::Comparison::LessOrEqual:
                        conditionMet = value <= condition.threshold;
                        break;
                }
                break;
            }

            case TransitionCondition::Type::Bool:
            {
                float value = context.GetParameter(condition.parameterName, 0.0f);
                bool boolValue = value > 0.5f;
                bool expected = condition.threshold > 0.5f;
                conditionMet = (boolValue == expected);
                break;
            }

            case TransitionCondition::Type::Trigger:
            {
                float value = context.GetParameter(condition.parameterName, 0.0f);
                conditionMet = value > 0.5f;  // Trigger is set
                break;
            }

            case TransitionCondition::Type::ExitTime:
            {
                conditionMet = normalizedTime >= condition.threshold;
                break;
            }
        }

        if (!conditionMet)
            return false;
    }

    // Check exit time if required
    if (m_hasExitTime && normalizedTime < m_exitTime)
    {
        return false;
    }

    return true;
}

} // namespace RVX::Animation
