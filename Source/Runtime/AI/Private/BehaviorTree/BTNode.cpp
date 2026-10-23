/**
 * @file BTNode.cpp
 * @brief Behavior tree node implementation
 */

#include "AI/BehaviorTree/BTNode.h"
#include "AI/BehaviorTree/BTTask.h"
#include "AI/BehaviorTree/BTDecorator.h"
#include "AI/BehaviorTree/BTService.h"
#include "AI/BehaviorTree/BTComposite.h"
#include "Core/Log.h"

#include <algorithm>
#include <random>

namespace RVX::AI
{

// Static member
uint32 BTNode::s_nextNodeId = 1;

// =========================================================================
// BTNode
// =========================================================================

BTNode::BTNode(const std::string& name)
    : m_name(name)
    , m_nodeId(s_nextNodeId++)
{
}

void BTNode::AddChild(BTNodePtr child)
{
    if (child)
    {
        child->m_parent = this;
        m_children.push_back(std::move(child));
    }
}

void BTNode::RemoveChild(BTNode* child)
{
    auto it = std::find_if(m_children.begin(), m_children.end(),
        [child](const BTNodePtr& ptr) { return ptr.get() == child; });

    if (it != m_children.end())
    {
        (*it)->m_parent = nullptr;
        m_children.erase(it);
    }
}

void BTNode::ClearChildren()
{
    for (auto& child : m_children)
    {
        child->m_parent = nullptr;
    }
    m_children.clear();
}

BTStatus BTNode::Tick(BTContext& context)
{
    // First tick - enter
    if (!m_wasRunning)
    {
        OnEnter(context);
        m_wasRunning = true;
    }

    // Execute
    m_status = OnTick(context);

    // Finished - exit
    if (m_status != BTStatus::Running)
    {
        OnExit(context, m_status);
        m_wasRunning = false;
    }

    return m_status;
}

void BTNode::Abort(BTContext& context)
{
    OnAbort(context);
    
    for (auto& child : m_children)
    {
        if (child->IsRunning())
        {
            child->Abort(context);
        }
    }

    m_wasRunning = false;
    m_status = BTStatus::Invalid;
}

void BTNode::Reset()
{
    m_status = BTStatus::Invalid;
    m_wasRunning = false;

    for (auto& child : m_children)
    {
        child->Reset();
    }
}

// =========================================================================
// BTTask
// =========================================================================

BTTask::BTTask(const std::string& name)
    : BTNode(name)
{
}

// =========================================================================
// BTSimpleTask
// =========================================================================

BTSimpleTask::BTSimpleTask(const std::string& name, TaskFunction func)
    : BTTask(name)
    , m_function(std::move(func))
{
}

BTStatus BTSimpleTask::OnTick(BTContext& context)
{
    if (m_function)
    {
        return m_function(context);
    }
    return BTStatus::Failure;
}

// =========================================================================
// BTWaitTask
// =========================================================================

BTWaitTask::BTWaitTask(float duration)
    : BTTask("Wait")
    , m_duration(duration)
{
}

BTWaitTask::BTWaitTask(const std::string& durationKey)
    : BTTask("Wait")
    , m_duration(0.0f)
    , m_durationKey(durationKey)
{
}

void BTWaitTask::Reset()
{
    BTTask::Reset();
    m_elapsed = 0.0f;
}

void BTWaitTask::OnEnter(BTContext& context)
{
    m_elapsed = 0.0f;

    if (!m_durationKey.empty())
    {
        auto duration = GetBlackboardValue<float>(context, m_durationKey);
        if (duration.has_value())
        {
            m_duration = *duration;
        }
    }
}

BTStatus BTWaitTask::OnTick(BTContext& context)
{
    m_elapsed += context.deltaTime;

    if (m_elapsed >= m_duration)
    {
        return BTStatus::Success;
    }

    return BTStatus::Running;
}

// =========================================================================
// BTLogTask
// =========================================================================

BTLogTask::BTLogTask(const std::string& message)
    : BTTask("Log")
    , m_message(message)
{
}

BTStatus BTLogTask::OnTick(BTContext& context)
{
    (void)context;
    RVX_CORE_INFO("BTLogTask: {}", m_message);
    return BTStatus::Success;
}

// =========================================================================
// BTDecorator
// =========================================================================

BTDecorator::BTDecorator(const std::string& name)
    : BTNode(name)
{
}

BTNode* BTDecorator::GetChild() const
{
    return m_children.empty() ? nullptr : m_children[0].get();
}

// =========================================================================
// BTCondition
// =========================================================================

BTCondition::BTCondition(const std::string& name, ConditionFunction condition)
    : BTDecorator(name)
    , m_condition(std::move(condition))
{
}

void BTCondition::Reset()
{
    BTDecorator::Reset();
}

BTStatus BTCondition::OnTick(BTContext& context)
{
    if (!m_condition || !m_condition(context))
    {
        return BTStatus::Failure;
    }

    BTNode* child = GetChild();
    if (child)
    {
        return child->Tick(context);
    }

    return BTStatus::Success;
}

// =========================================================================
// BTBlackboardCondition
// =========================================================================

BTBlackboardCondition::BTBlackboardCondition(const std::string& key, Comparison comparison)
    : BTDecorator("BlackboardCondition_" + key)
    , m_key(key)
    , m_comparison(comparison)
{
}

BTStatus BTBlackboardCondition::OnTick(BTContext& context)
{
    if (!EvaluateCondition(context))
    {
        return BTStatus::Failure;
    }

    BTNode* child = GetChild();
    if (child)
    {
        return child->Tick(context);
    }

    return BTStatus::Success;
}

bool BTBlackboardCondition::EvaluateCondition(BTContext& context)
{
    if (!context.blackboard)
    {
        return false;
    }

    bool hasKey = context.blackboard->HasKey(m_key);

    switch (m_comparison)
    {
        case Comparison::Exists:
            return hasKey;
        
        case Comparison::NotExists:
            return !hasKey;
        
        case Comparison::Equals:
        case Comparison::NotEquals:
        case Comparison::LessThan:
        case Comparison::LessOrEqual:
        case Comparison::GreaterThan:
        case Comparison::GreaterOrEqual:
            // Would need type-specific comparison logic
            // For now, just check if key exists
            return hasKey;
    }

    return false;
}

// =========================================================================
// BTInverter
// =========================================================================

BTInverter::BTInverter()
    : BTDecorator("Inverter")
{
}

BTStatus BTInverter::OnTick(BTContext& context)
{
    BTNode* child = GetChild();
    if (!child)
    {
        return BTStatus::Failure;
    }

    BTStatus status = child->Tick(context);

    switch (status)
    {
        case BTStatus::Success:
            return BTStatus::Failure;
        case BTStatus::Failure:
            return BTStatus::Success;
        default:
            return status;
    }
}

// =========================================================================
// BTForceSuccess
// =========================================================================

BTForceSuccess::BTForceSuccess()
    : BTDecorator("ForceSuccess")
{
}

BTStatus BTForceSuccess::OnTick(BTContext& context)
{
    BTNode* child = GetChild();
    if (child)
    {
        BTStatus status = child->Tick(context);
        if (status == BTStatus::Running)
        {
            return BTStatus::Running;
        }
    }
    return BTStatus::Success;
}

// =========================================================================
// BTForceFailure
// =========================================================================

BTForceFailure::BTForceFailure()
    : BTDecorator("ForceFailure")
{
}

BTStatus BTForceFailure::OnTick(BTContext& context)
{
    BTNode* child = GetChild();
    if (child)
    {
        BTStatus status = child->Tick(context);
        if (status == BTStatus::Running)
        {
            return BTStatus::Running;
        }
    }
    return BTStatus::Failure;
}

// =========================================================================
// BTRepeater
// =========================================================================

BTRepeater::BTRepeater(uint32 repeatCount, bool stopOnFailure)
    : BTDecorator("Repeater")
    , m_repeatCount(repeatCount)
    , m_stopOnFailure(stopOnFailure)
{
}

void BTRepeater::Reset()
{
    BTDecorator::Reset();
    m_currentCount = 0;
}

BTStatus BTRepeater::OnTick(BTContext& context)
{
    BTNode* child = GetChild();
    if (!child)
    {
        return BTStatus::Failure;
    }

    BTStatus status = child->Tick(context);

    if (status == BTStatus::Running)
    {
        return BTStatus::Running;
    }

    if (status == BTStatus::Failure && m_stopOnFailure)
    {
        return BTStatus::Failure;
    }

    m_currentCount++;
    child->Reset();

    // Check if done repeating
    if (m_repeatCount > 0 && m_currentCount >= m_repeatCount)
    {
        return BTStatus::Success;
    }

    // Continue repeating
    return BTStatus::Running;
}

// =========================================================================
// BTRetry
// =========================================================================

BTRetry::BTRetry(uint32 maxRetries)
    : BTDecorator("Retry")
    , m_maxRetries(maxRetries)
{
}

void BTRetry::Reset()
{
    BTDecorator::Reset();
    m_currentRetries = 0;
}

BTStatus BTRetry::OnTick(BTContext& context)
{
    BTNode* child = GetChild();
    if (!child)
    {
        return BTStatus::Failure;
    }

    BTStatus status = child->Tick(context);

    if (status == BTStatus::Success)
    {
        return BTStatus::Success;
    }

    if (status == BTStatus::Running)
    {
        return BTStatus::Running;
    }

    // Failed - retry
    m_currentRetries++;
    child->Reset();

    if (m_maxRetries > 0 && m_currentRetries >= m_maxRetries)
    {
        return BTStatus::Failure;
    }

    return BTStatus::Running;
}

// =========================================================================
// BTTimeout
// =========================================================================

BTTimeout::BTTimeout(float timeout)
    : BTDecorator("Timeout")
    , m_timeout(timeout)
{
}

void BTTimeout::Reset()
{
    BTDecorator::Reset();
    m_elapsed = 0.0f;
}

void BTTimeout::OnEnter(BTContext& context)
{
    (void)context;
    m_elapsed = 0.0f;
}

BTStatus BTTimeout::OnTick(BTContext& context)
{
    m_elapsed += context.deltaTime;

    if (m_elapsed >= m_timeout)
    {
        BTNode* child = GetChild();
        if (child && child->IsRunning())
        {
            child->Abort(context);
        }
        return BTStatus::Failure;
    }

    BTNode* child = GetChild();
    if (child)
    {
        return child->Tick(context);
    }

    return BTStatus::Failure;
}

// =========================================================================
// BTCooldown
// =========================================================================

BTCooldown::BTCooldown(float cooldownDuration)
    : BTDecorator("Cooldown")
    , m_cooldownDuration(cooldownDuration)
{
}

void BTCooldown::Reset()
{
    BTDecorator::Reset();
    m_remainingCooldown = 0.0f;
    m_isOnCooldown = false;
}

BTStatus BTCooldown::OnTick(BTContext& context)
{
    // Update cooldown
    if (m_isOnCooldown)
    {
        m_remainingCooldown -= context.deltaTime;
        if (m_remainingCooldown <= 0.0f)
        {
            m_isOnCooldown = false;
        }
        else
        {
            return BTStatus::Failure;
        }
    }

    BTNode* child = GetChild();
    if (child)
    {
        return child->Tick(context);
    }

    return BTStatus::Failure;
}

void BTCooldown::OnExit(BTContext& context, BTStatus status)
{
    (void)context;
    
    if (status == BTStatus::Success || status == BTStatus::Failure)
    {
        m_isOnCooldown = true;
        m_remainingCooldown = m_cooldownDuration;
    }
}

// =========================================================================
// BTService
// =========================================================================

BTService::BTService(const std::string& name, float interval, float randomDeviation)
    : BTNode(name)
    , m_interval(interval)
    , m_randomDeviation(randomDeviation)
{
    ComputeNextInterval();
}

void BTService::Reset()
{
    BTNode::Reset();
    m_timeSinceLastTick = 0.0f;
    m_wasActive = false;
    ComputeNextInterval();
}

void BTService::TickService(BTContext& context)
{
    if (!m_wasActive)
    {
        OnServiceActivate(context);
        m_wasActive = true;
        m_timeSinceLastTick = m_currentInterval;  // Tick immediately on first activation
    }

    m_timeSinceLastTick += context.deltaTime;

    if (m_timeSinceLastTick >= m_currentInterval)
    {
        OnService(context);
        m_timeSinceLastTick = 0.0f;
        ComputeNextInterval();
    }
}

BTStatus BTService::OnTick(BTContext& context)
{
    // Services are ticked separately by composites
    (void)context;
    return BTStatus::Success;
}

void BTService::ComputeNextInterval()
{
    if (m_randomDeviation > 0.0f)
    {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        std::uniform_real_distribution<float> dist(-m_randomDeviation, m_randomDeviation);
        m_currentInterval = m_interval + dist(gen);
        m_currentInterval = std::max(0.01f, m_currentInterval);
    }
    else
    {
        m_currentInterval = m_interval;
    }
}

// =========================================================================
// BTSimpleService
// =========================================================================

BTSimpleService::BTSimpleService(const std::string& name, float interval, ServiceFunction func)
    : BTService(name, interval)
    , m_function(std::move(func))
{
}

void BTSimpleService::OnService(BTContext& context)
{
    if (m_function)
    {
        m_function(context);
    }
}

// =========================================================================
// BTComposite
// =========================================================================

BTComposite::BTComposite(const std::string& name)
    : BTNode(name)
{
}

void BTComposite::AttachService(std::shared_ptr<BTService> service)
{
    if (service)
    {
        m_services.push_back(std::move(service));
    }
}

void BTComposite::Reset()
{
    BTNode::Reset();
    m_currentChildIndex = 0;

    for (auto& service : m_services)
    {
        service->Reset();
    }
}

void BTComposite::OnEnter(BTContext& context)
{
    (void)context;
    m_currentChildIndex = 0;
}

void BTComposite::OnExit(BTContext& context, BTStatus status)
{
    (void)status;

    // Deactivate services
    for (auto& service : m_services)
    {
        service->Reset();
    }
    (void)context;
}

void BTComposite::TickServices(BTContext& context)
{
    for (auto& service : m_services)
    {
        service->TickService(context);
    }
}

// =========================================================================
// BTSelector
// =========================================================================

BTSelector::BTSelector(const std::string& name)
    : BTComposite(name)
{
}

void BTSelector::Reset()
{
    BTComposite::Reset();
}

BTStatus BTSelector::OnTick(BTContext& context)
{
    TickServices(context);

    while (m_currentChildIndex < m_children.size())
    {
        BTStatus status = m_children[m_currentChildIndex]->Tick(context);

        if (status == BTStatus::Running)
        {
            return BTStatus::Running;
        }

        if (status == BTStatus::Success)
        {
            return BTStatus::Success;
        }

        // Failed - try next child
        m_currentChildIndex++;
    }

    // All children failed
    return BTStatus::Failure;
}

// =========================================================================
// BTSequence
// =========================================================================

BTSequence::BTSequence(const std::string& name)
    : BTComposite(name)
{
}

void BTSequence::Reset()
{
    BTComposite::Reset();
}

BTStatus BTSequence::OnTick(BTContext& context)
{
    TickServices(context);

    while (m_currentChildIndex < m_children.size())
    {
        BTStatus status = m_children[m_currentChildIndex]->Tick(context);

        if (status == BTStatus::Running)
        {
            return BTStatus::Running;
        }

        if (status == BTStatus::Failure)
        {
            return BTStatus::Failure;
        }

        // Succeeded - continue to next child
        m_currentChildIndex++;
    }

    // All children succeeded
    return BTStatus::Success;
}

// =========================================================================
// BTParallel
// =========================================================================

BTParallel::BTParallel(const std::string& name, Policy successPolicy, Policy failurePolicy)
    : BTComposite(name)
    , m_successPolicy(successPolicy)
    , m_failurePolicy(failurePolicy)
{
}

void BTParallel::Reset()
{
    BTComposite::Reset();
    m_childStatuses.clear();
}

BTStatus BTParallel::OnTick(BTContext& context)
{
    TickServices(context);

    // Initialize statuses if needed
    if (m_childStatuses.size() != m_children.size())
    {
        m_childStatuses.resize(m_children.size(), BTStatus::Invalid);
    }

    uint32 successCount = 0;
    uint32 failureCount = 0;
    uint32 runningCount = 0;

    // Tick all children
    for (size_t i = 0; i < m_children.size(); ++i)
    {
        if (m_childStatuses[i] == BTStatus::Running || 
            m_childStatuses[i] == BTStatus::Invalid)
        {
            m_childStatuses[i] = m_children[i]->Tick(context);
        }

        switch (m_childStatuses[i])
        {
            case BTStatus::Success:
                successCount++;
                break;
            case BTStatus::Failure:
                failureCount++;
                break;
            case BTStatus::Running:
                runningCount++;
                break;
            default:
                break;
        }
    }

    // Check success condition
    if (m_successPolicy == Policy::RequireOne && successCount > 0)
    {
        return BTStatus::Success;
    }
    if (m_successPolicy == Policy::RequireAll && successCount == m_children.size())
    {
        return BTStatus::Success;
    }

    // Check failure condition
    if (m_failurePolicy == Policy::RequireOne && failureCount > 0)
    {
        return BTStatus::Failure;
    }
    if (m_failurePolicy == Policy::RequireAll && failureCount == m_children.size())
    {
        return BTStatus::Failure;
    }

    // Still running
    if (runningCount > 0)
    {
        return BTStatus::Running;
    }

    // All finished but didn't hit success/failure conditions
    return BTStatus::Failure;
}

// =========================================================================
// BTRandomSelector
// =========================================================================

BTRandomSelector::BTRandomSelector(const std::string& name)
    : BTComposite(name)
{
}

void BTRandomSelector::Reset()
{
    BTComposite::Reset();
    m_shuffledIndices.clear();
    m_currentShuffledIndex = 0;
}

void BTRandomSelector::OnEnter(BTContext& context)
{
    BTComposite::OnEnter(context);
    ShuffleIndices();
    m_currentShuffledIndex = 0;
}

BTStatus BTRandomSelector::OnTick(BTContext& context)
{
    TickServices(context);

    while (m_currentShuffledIndex < m_shuffledIndices.size())
    {
        uint32 childIdx = m_shuffledIndices[m_currentShuffledIndex];
        BTStatus status = m_children[childIdx]->Tick(context);

        if (status == BTStatus::Running)
        {
            return BTStatus::Running;
        }

        if (status == BTStatus::Success)
        {
            return BTStatus::Success;
        }

        m_currentShuffledIndex++;
    }

    return BTStatus::Failure;
}

void BTRandomSelector::ShuffleIndices()
{
    m_shuffledIndices.resize(m_children.size());
    for (size_t i = 0; i < m_children.size(); ++i)
    {
        m_shuffledIndices[i] = static_cast<uint32>(i);
    }

    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::shuffle(m_shuffledIndices.begin(), m_shuffledIndices.end(), gen);
}

// =========================================================================
// BTRandomSequence
// =========================================================================

BTRandomSequence::BTRandomSequence(const std::string& name)
    : BTComposite(name)
{
}

void BTRandomSequence::Reset()
{
    BTComposite::Reset();
    m_shuffledIndices.clear();
    m_currentShuffledIndex = 0;
}

void BTRandomSequence::OnEnter(BTContext& context)
{
    BTComposite::OnEnter(context);
    ShuffleIndices();
    m_currentShuffledIndex = 0;
}

BTStatus BTRandomSequence::OnTick(BTContext& context)
{
    TickServices(context);

    while (m_currentShuffledIndex < m_shuffledIndices.size())
    {
        uint32 childIdx = m_shuffledIndices[m_currentShuffledIndex];
        BTStatus status = m_children[childIdx]->Tick(context);

        if (status == BTStatus::Running)
        {
            return BTStatus::Running;
        }

        if (status == BTStatus::Failure)
        {
            return BTStatus::Failure;
        }

        m_currentShuffledIndex++;
    }

    return BTStatus::Success;
}

void BTRandomSequence::ShuffleIndices()
{
    m_shuffledIndices.resize(m_children.size());
    for (size_t i = 0; i < m_children.size(); ++i)
    {
        m_shuffledIndices[i] = static_cast<uint32>(i);
    }

    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::shuffle(m_shuffledIndices.begin(), m_shuffledIndices.end(), gen);
}

// =========================================================================
// BTWeightedSelector
// =========================================================================

BTWeightedSelector::BTWeightedSelector(const std::string& name)
    : BTComposite(name)
{
}

void BTWeightedSelector::AddWeightedChild(BTNodePtr child, float weight)
{
    AddChild(std::move(child));
    m_weights.push_back(weight);
}

void BTWeightedSelector::Reset()
{
    BTComposite::Reset();
    m_selectedChildIndex = 0;
    m_childSelected = false;
}

void BTWeightedSelector::OnEnter(BTContext& context)
{
    BTComposite::OnEnter(context);
    m_selectedChildIndex = SelectWeightedChild();
    m_childSelected = true;
}

BTStatus BTWeightedSelector::OnTick(BTContext& context)
{
    TickServices(context);

    if (!m_childSelected || m_selectedChildIndex >= m_children.size())
    {
        return BTStatus::Failure;
    }

    return m_children[m_selectedChildIndex]->Tick(context);
}

uint32 BTWeightedSelector::SelectWeightedChild()
{
    if (m_children.empty() || m_weights.empty())
    {
        return 0;
    }

    // Calculate total weight
    float totalWeight = 0.0f;
    for (size_t i = 0; i < m_children.size() && i < m_weights.size(); ++i)
    {
        totalWeight += m_weights[i];
    }

    if (totalWeight <= 0.0f)
    {
        return 0;
    }

    // Random selection
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_real_distribution<float> dist(0.0f, totalWeight);
    float random = dist(gen);

    float cumulative = 0.0f;
    for (size_t i = 0; i < m_children.size() && i < m_weights.size(); ++i)
    {
        cumulative += m_weights[i];
        if (random <= cumulative)
        {
            return static_cast<uint32>(i);
        }
    }

    return static_cast<uint32>(m_children.size() - 1);
}

} // namespace RVX::AI
