#pragma once

/**
 * @file BTDecorator.h
 * @brief Decorator nodes - modify child behavior
 */

#include "AI/BehaviorTree/BTNode.h"

#include <functional>

namespace RVX::AI
{

/**
 * @brief Base class for decorator nodes
 * 
 * Decorators wrap a single child node and modify its behavior.
 * Examples: conditions, inverters, repeaters, timeouts.
 * 
 * Usage:
 * @code
 * // Only attack if has ammo
 * auto condition = std::make_shared<BTCondition>("HasAmmo",
 *     [](BTContext& ctx) { 
 *         return ctx.blackboard->GetValueOr<int>("Ammo", 0) > 0;
 *     });
 * condition->AddChild(attackTask);
 * @endcode
 */
class BTDecorator : public BTNode
{
public:
    BTDecorator(const std::string& name = "Decorator");

    BTNodeType GetType() const override { return BTNodeType::Decorator; }

    /**
     * @brief Get the abort mode
     */
    BTAbortMode GetAbortMode() const { return m_abortMode; }

    /**
     * @brief Set the abort mode
     */
    void SetAbortMode(BTAbortMode mode) { m_abortMode = mode; }

    /**
     * @brief Get the child node
     */
    BTNode* GetChild() const;

protected:
    BTAbortMode m_abortMode = BTAbortMode::None;
};

/**
 * @brief Conditional decorator - only executes child if condition is true
 */
class BTCondition : public BTDecorator
{
public:
    using ConditionFunction = std::function<bool(BTContext&)>;

    BTCondition(const std::string& name, ConditionFunction condition);

    void Reset() override;

protected:
    BTStatus OnTick(BTContext& context) override;

private:
    ConditionFunction m_condition;
};

/**
 * @brief Blackboard condition - checks a blackboard value
 */
class BTBlackboardCondition : public BTDecorator
{
public:
    enum class Comparison : uint8
    {
        Exists,         ///< Key exists
        NotExists,      ///< Key does not exist
        Equals,         ///< Value equals expected
        NotEquals,      ///< Value not equals expected
        LessThan,       ///< Value < expected (numeric)
        LessOrEqual,    ///< Value <= expected (numeric)
        GreaterThan,    ///< Value > expected (numeric)
        GreaterOrEqual  ///< Value >= expected (numeric)
    };

    /**
     * @brief Check if key exists
     */
    BTBlackboardCondition(const std::string& key, Comparison comparison = Comparison::Exists);

    /**
     * @brief Check against a value
     */
    template<typename T>
    BTBlackboardCondition(const std::string& key, Comparison comparison, const T& value)
        : BTDecorator("BlackboardCondition_" + key)
        , m_key(key)
        , m_comparison(comparison)
        , m_expectedValue(value)
    {}

protected:
    BTStatus OnTick(BTContext& context) override;

private:
    std::string m_key;
    Comparison m_comparison;
    BlackboardValue m_expectedValue;

    bool EvaluateCondition(BTContext& context);
};

/**
 * @brief Inverter - inverts child result
 */
class BTInverter : public BTDecorator
{
public:
    BTInverter();

protected:
    BTStatus OnTick(BTContext& context) override;
};

/**
 * @brief Force success - always returns success regardless of child
 */
class BTForceSuccess : public BTDecorator
{
public:
    BTForceSuccess();

protected:
    BTStatus OnTick(BTContext& context) override;
};

/**
 * @brief Force failure - always returns failure regardless of child
 */
class BTForceFailure : public BTDecorator
{
public:
    BTForceFailure();

protected:
    BTStatus OnTick(BTContext& context) override;
};

/**
 * @brief Repeater - repeats child execution
 */
class BTRepeater : public BTDecorator
{
public:
    /**
     * @brief Create repeater
     * @param repeatCount Number of times to repeat (0 = infinite)
     * @param stopOnFailure Stop repeating if child fails
     */
    BTRepeater(uint32 repeatCount = 0, bool stopOnFailure = false);

    void Reset() override;

protected:
    BTStatus OnTick(BTContext& context) override;

private:
    uint32 m_repeatCount;
    uint32 m_currentCount = 0;
    bool m_stopOnFailure;
};

/**
 * @brief Retry - retries failed child
 */
class BTRetry : public BTDecorator
{
public:
    /**
     * @brief Create retry decorator
     * @param maxRetries Maximum number of retries (0 = infinite)
     */
    explicit BTRetry(uint32 maxRetries = 3);

    void Reset() override;

protected:
    BTStatus OnTick(BTContext& context) override;

private:
    uint32 m_maxRetries;
    uint32 m_currentRetries = 0;
};

/**
 * @brief Timeout - fails child if it takes too long
 */
class BTTimeout : public BTDecorator
{
public:
    /**
     * @brief Create timeout decorator
     * @param timeout Timeout duration in seconds
     */
    explicit BTTimeout(float timeout);

    void Reset() override;

protected:
    void OnEnter(BTContext& context) override;
    BTStatus OnTick(BTContext& context) override;

private:
    float m_timeout;
    float m_elapsed = 0.0f;
};

/**
 * @brief Cooldown - prevents re-execution for a duration
 */
class BTCooldown : public BTDecorator
{
public:
    /**
     * @brief Create cooldown decorator
     * @param cooldownDuration Cooldown duration in seconds
     */
    explicit BTCooldown(float cooldownDuration);

    void Reset() override;

protected:
    BTStatus OnTick(BTContext& context) override;
    void OnExit(BTContext& context, BTStatus status) override;

private:
    float m_cooldownDuration;
    float m_remainingCooldown = 0.0f;
    bool m_isOnCooldown = false;
};

} // namespace RVX::AI
