#pragma once

/**
 * @file BTService.h
 * @brief Service nodes - background tasks that run while parent is active
 */

#include "AI/BehaviorTree/BTNode.h"

#include <functional>

namespace RVX::AI
{

/**
 * @brief Base class for service nodes
 * 
 * Services are background tasks attached to composite nodes that execute
 * at a regular interval while the composite or its children are active.
 * 
 * Common uses:
 * - Update blackboard values
 * - Check for targets
 * - Update perception
 * 
 * Usage:
 * @code
 * class UpdateTargetService : public BTService
 * {
 * public:
 *     UpdateTargetService() : BTService("UpdateTarget", 0.5f) {}
 *     
 * protected:
 *     void OnService(BTContext& ctx) override
 *     {
 *         auto* enemy = FindNearestEnemy();
 *         if (enemy)
 *         {
 *             SetBlackboardValue<uint64>(ctx, "TargetEnemy", enemy->GetId());
 *             SetBlackboardValue<Vec3>(ctx, "TargetLocation", enemy->GetPosition());
 *         }
 *         else
 *         {
 *             ctx.blackboard->RemoveKey("TargetEnemy");
 *         }
 *     }
 * };
 * @endcode
 */
class BTService : public BTNode
{
public:
    /**
     * @brief Create a service
     * @param name Service name
     * @param interval Update interval in seconds
     * @param randomDeviation Random deviation added to interval
     */
    BTService(const std::string& name = "Service", 
              float interval = 0.5f, 
              float randomDeviation = 0.0f);

    BTNodeType GetType() const override { return BTNodeType::Service; }

    /**
     * @brief Get the update interval
     */
    float GetInterval() const { return m_interval; }

    /**
     * @brief Set the update interval
     */
    void SetInterval(float interval) { m_interval = interval; }

    /**
     * @brief Get the random deviation
     */
    float GetRandomDeviation() const { return m_randomDeviation; }

    /**
     * @brief Set the random deviation
     */
    void SetRandomDeviation(float deviation) { m_randomDeviation = deviation; }

    /**
     * @brief Called to update the service
     */
    void TickService(BTContext& context);

    void Reset() override;

protected:
    // Services don't use OnTick like regular nodes
    BTStatus OnTick(BTContext& context) override;

    /**
     * @brief Called when service becomes active
     */
    virtual void OnServiceActivate(BTContext& context) { (void)context; }

    /**
     * @brief Called when service becomes inactive
     */
    virtual void OnServiceDeactivate(BTContext& context) { (void)context; }

    /**
     * @brief Called at the service interval
     */
    virtual void OnService(BTContext& context) = 0;

private:
    float m_interval;
    float m_randomDeviation;
    float m_timeSinceLastTick = 0.0f;
    float m_currentInterval = 0.0f;
    bool m_wasActive = false;

    void ComputeNextInterval();
};

/**
 * @brief Simple service that executes a lambda function
 */
class BTSimpleService : public BTService
{
public:
    using ServiceFunction = std::function<void(BTContext&)>;

    BTSimpleService(const std::string& name, float interval, ServiceFunction func);

protected:
    void OnService(BTContext& context) override;

private:
    ServiceFunction m_function;
};

/**
 * @brief Service that updates a blackboard value from a function
 */
template<typename T>
class BTUpdateValueService : public BTService
{
public:
    using ValueFunction = std::function<T(BTContext&)>;

    BTUpdateValueService(const std::string& key, float interval, ValueFunction func)
        : BTService("UpdateValue_" + key, interval)
        , m_key(key)
        , m_function(std::move(func))
    {}

protected:
    void OnService(BTContext& context) override
    {
        T value = m_function(context);
        SetBlackboardValue<T>(context, m_key, value);
    }

private:
    std::string m_key;
    ValueFunction m_function;
};

} // namespace RVX::AI
