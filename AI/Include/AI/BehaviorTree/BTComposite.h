#pragma once

/**
 * @file BTComposite.h
 * @brief Composite nodes - nodes with multiple children
 */

#include "AI/BehaviorTree/BTNode.h"
#include "AI/BehaviorTree/BTService.h"

#include <vector>

namespace RVX::AI
{

/**
 * @brief Base class for composite nodes
 * 
 * Composite nodes have multiple children and define how they are executed.
 * They can also have attached services that run while the composite is active.
 */
class BTComposite : public BTNode
{
public:
    BTComposite(const std::string& name = "Composite");

    BTNodeType GetType() const override { return BTNodeType::Composite; }

    /**
     * @brief Attach a service to this composite
     */
    void AttachService(std::shared_ptr<BTService> service);

    /**
     * @brief Get attached services
     */
    const std::vector<std::shared_ptr<BTService>>& GetServices() const { return m_services; }

    void Reset() override;

protected:
    void OnEnter(BTContext& context) override;
    void OnExit(BTContext& context, BTStatus status) override;

    void TickServices(BTContext& context);

    uint32 m_currentChildIndex = 0;
    std::vector<std::shared_ptr<BTService>> m_services;
};

/**
 * @brief Selector (OR logic) - tries children until one succeeds
 * 
 * Executes children from left to right until one returns Success.
 * Returns Failure only if all children fail.
 * 
 * Behavior:
 * - If child returns Success: Selector returns Success
 * - If child returns Failure: Try next child
 * - If child returns Running: Selector returns Running
 * - If all children fail: Selector returns Failure
 */
class BTSelector : public BTComposite
{
public:
    explicit BTSelector(const std::string& name = "Selector");

    void Reset() override;

protected:
    BTStatus OnTick(BTContext& context) override;
};

/**
 * @brief Sequence (AND logic) - runs children until one fails
 * 
 * Executes children from left to right until one returns Failure.
 * Returns Success only if all children succeed.
 * 
 * Behavior:
 * - If child returns Success: Try next child
 * - If child returns Failure: Sequence returns Failure
 * - If child returns Running: Sequence returns Running
 * - If all children succeed: Sequence returns Success
 */
class BTSequence : public BTComposite
{
public:
    explicit BTSequence(const std::string& name = "Sequence");

    void Reset() override;

protected:
    BTStatus OnTick(BTContext& context) override;
};

/**
 * @brief Parallel node - runs all children simultaneously
 * 
 * Executes all children every tick. The success/failure policy determines
 * when the parallel node completes.
 */
class BTParallel : public BTComposite
{
public:
    /**
     * @brief Policy for determining parallel node result
     */
    enum class Policy : uint8
    {
        RequireOne,     ///< Success if any child succeeds
        RequireAll      ///< Success only if all children succeed
    };

    /**
     * @brief Create a parallel node
     * @param successPolicy Policy for success
     * @param failurePolicy Policy for failure
     */
    BTParallel(const std::string& name = "Parallel",
               Policy successPolicy = Policy::RequireOne,
               Policy failurePolicy = Policy::RequireOne);

    void Reset() override;

protected:
    BTStatus OnTick(BTContext& context) override;

private:
    Policy m_successPolicy;
    Policy m_failurePolicy;
    std::vector<BTStatus> m_childStatuses;
};

/**
 * @brief Random selector - selects a random child to execute
 */
class BTRandomSelector : public BTComposite
{
public:
    explicit BTRandomSelector(const std::string& name = "RandomSelector");

    void Reset() override;

protected:
    void OnEnter(BTContext& context) override;
    BTStatus OnTick(BTContext& context) override;

private:
    std::vector<uint32> m_shuffledIndices;
    uint32 m_currentShuffledIndex = 0;

    void ShuffleIndices();
};

/**
 * @brief Random sequence - executes children in random order
 */
class BTRandomSequence : public BTComposite
{
public:
    explicit BTRandomSequence(const std::string& name = "RandomSequence");

    void Reset() override;

protected:
    void OnEnter(BTContext& context) override;
    BTStatus OnTick(BTContext& context) override;

private:
    std::vector<uint32> m_shuffledIndices;
    uint32 m_currentShuffledIndex = 0;

    void ShuffleIndices();
};

/**
 * @brief Weighted selector - selects child based on weights
 */
class BTWeightedSelector : public BTComposite
{
public:
    explicit BTWeightedSelector(const std::string& name = "WeightedSelector");

    /**
     * @brief Add a child with a weight
     * @param child Child node
     * @param weight Selection weight (higher = more likely)
     */
    void AddWeightedChild(BTNodePtr child, float weight);

    void Reset() override;

protected:
    void OnEnter(BTContext& context) override;
    BTStatus OnTick(BTContext& context) override;

private:
    std::vector<float> m_weights;
    uint32 m_selectedChildIndex = 0;
    bool m_childSelected = false;

    uint32 SelectWeightedChild();
};

} // namespace RVX::AI
