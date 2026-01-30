#pragma once

/**
 * @file BTTask.h
 * @brief Task nodes - leaf nodes that perform actions
 */

#include "AI/BehaviorTree/BTNode.h"

#include <functional>

namespace RVX::AI
{

/**
 * @brief Base class for task (leaf) nodes
 * 
 * Tasks are the leaf nodes of a behavior tree that perform actual actions.
 * They can complete immediately (Success/Failure) or run over multiple ticks (Running).
 * 
 * Usage:
 * @code
 * class MoveToTask : public BTTask
 * {
 * public:
 *     MoveToTask(const std::string& targetKey)
 *         : BTTask("MoveTo"), m_targetKey(targetKey) {}
 *     
 * protected:
 *     BTStatus OnTick(BTContext& ctx) override
 *     {
 *         auto target = GetBlackboardValue<Vec3>(ctx, m_targetKey);
 *         if (!target) return BTStatus::Failure;
 *         
 *         if (ReachedTarget(*target)) return BTStatus::Success;
 *         
 *         MoveToward(*target);
 *         return BTStatus::Running;
 *     }
 * };
 * @endcode
 */
class BTTask : public BTNode
{
public:
    BTTask(const std::string& name = "Task");

    BTNodeType GetType() const override { return BTNodeType::Task; }
};

/**
 * @brief Simple task that executes a lambda function
 */
class BTSimpleTask : public BTTask
{
public:
    using TaskFunction = std::function<BTStatus(BTContext&)>;

    BTSimpleTask(const std::string& name, TaskFunction func);

protected:
    BTStatus OnTick(BTContext& context) override;

private:
    TaskFunction m_function;
};

/**
 * @brief Task that waits for a duration
 */
class BTWaitTask : public BTTask
{
public:
    /**
     * @brief Create a wait task
     * @param duration Wait duration in seconds
     */
    explicit BTWaitTask(float duration);

    /**
     * @brief Create a wait task with duration from blackboard
     * @param durationKey Blackboard key containing duration
     */
    explicit BTWaitTask(const std::string& durationKey);

    void Reset() override;

protected:
    void OnEnter(BTContext& context) override;
    BTStatus OnTick(BTContext& context) override;

private:
    float m_duration = 1.0f;
    std::string m_durationKey;
    float m_elapsed = 0.0f;
};

/**
 * @brief Task that sets a blackboard value
 */
template<typename T>
class BTSetValueTask : public BTTask
{
public:
    BTSetValueTask(const std::string& key, const T& value)
        : BTTask("SetValue_" + key)
        , m_key(key)
        , m_value(value)
    {}

protected:
    BTStatus OnTick(BTContext& context) override
    {
        SetBlackboardValue<T>(context, m_key, m_value);
        return BTStatus::Success;
    }

private:
    std::string m_key;
    T m_value;
};

/**
 * @brief Task that logs a message (for debugging)
 */
class BTLogTask : public BTTask
{
public:
    explicit BTLogTask(const std::string& message);

protected:
    BTStatus OnTick(BTContext& context) override;

private:
    std::string m_message;
};

/**
 * @brief Task that always succeeds
 */
class BTSuccessTask : public BTTask
{
public:
    BTSuccessTask() : BTTask("AlwaysSucceed") {}

protected:
    BTStatus OnTick(BTContext&) override { return BTStatus::Success; }
};

/**
 * @brief Task that always fails
 */
class BTFailTask : public BTTask
{
public:
    BTFailTask() : BTTask("AlwaysFail") {}

protected:
    BTStatus OnTick(BTContext&) override { return BTStatus::Failure; }
};

} // namespace RVX::AI
