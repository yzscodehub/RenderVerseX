/**
 * @file AISubsystem.cpp
 * @brief AI subsystem implementation
 */

#include "AI/AISubsystem.h"
#include "AI/Navigation/NavMesh.h"
#include "AI/Navigation/PathFinder.h"
#include "AI/Navigation/NavigationAgent.h"
#include "AI/BehaviorTree/BehaviorTree.h"
#include "AI/Perception/AIPerception.h"
#include "Core/Log.h"

namespace RVX::AI
{

// =========================================================================
// Construction
// =========================================================================

AISubsystem::AISubsystem()
    : m_pathFinder(std::make_unique<PathFinder>())
{
}

AISubsystem::~AISubsystem() = default;

// =========================================================================
// ISubsystem Interface
// =========================================================================

void AISubsystem::Initialize()
{
    RVX_CORE_INFO("AISubsystem: Initializing");
}

void AISubsystem::Deinitialize()
{
    RVX_CORE_INFO("AISubsystem: Deinitializing");

    m_agents.clear();
    m_behaviorTreeInstances.clear();
    m_blackboards.clear();
    m_perceptionComponents.clear();
    m_navMesh.reset();
}

void AISubsystem::Tick(float deltaTime)
{
    UpdatePerception(deltaTime);
    UpdateBehaviorTrees(deltaTime);
    UpdateAgents(deltaTime);
}

// =========================================================================
// Navigation
// =========================================================================

void AISubsystem::SetNavMesh(NavMeshPtr navMesh)
{
    m_navMesh = std::move(navMesh);
    m_pathFinder->SetNavMesh(m_navMesh.get());
    RVX_CORE_INFO("AISubsystem: NavMesh set with {} polygons", 
                  m_navMesh ? m_navMesh->GetPolygons().size() : 0);
}

NavQueryStatus AISubsystem::FindPath(const Vec3& start, const Vec3& end,
                                     NavPath& outPath,
                                     const NavQueryFilter* filter)
{
    if (!m_pathFinder->GetNavMesh())
    {
        outPath.status = NavQueryStatus::Failed;
        return NavQueryStatus::Failed;
    }

    return m_pathFinder->FindPath(start, end, outPath, filter);
}

bool AISubsystem::FindNearestPoint(const Vec3& position, const Vec3& searchExtent,
                                   NavPoint& outPoint)
{
    if (!m_pathFinder->GetNavMesh())
    {
        return false;
    }

    return m_pathFinder->FindNearestPoly(position, searchExtent, outPoint);
}

bool AISubsystem::Raycast(const Vec3& start, const Vec3& end, Vec3& outHitPoint)
{
    if (!m_pathFinder->GetNavMesh())
    {
        return false;
    }

    return m_pathFinder->Raycast(start, end, outHitPoint);
}

// =========================================================================
// Agent Management
// =========================================================================

NavigationAgent* AISubsystem::RegisterAgent(uint64 entityId, const AgentConfig& config)
{
    // Check if already registered
    auto it = m_agents.find(entityId);
    if (it != m_agents.end())
    {
        RVX_CORE_WARN("AISubsystem: Agent {} already registered", entityId);
        return it->second.get();
    }

    auto agent = std::make_unique<NavigationAgent>(entityId, config);
    NavigationAgent* ptr = agent.get();
    m_agents[entityId] = std::move(agent);

    RVX_CORE_INFO("AISubsystem: Registered agent {}", entityId);
    return ptr;
}

void AISubsystem::UnregisterAgent(uint64 entityId)
{
    if (m_agents.erase(entityId) > 0)
    {
        RVX_CORE_INFO("AISubsystem: Unregistered agent {}", entityId);
    }
}

NavigationAgent* AISubsystem::GetAgent(uint64 entityId)
{
    auto it = m_agents.find(entityId);
    return it != m_agents.end() ? it->second.get() : nullptr;
}

// =========================================================================
// Behavior Trees
// =========================================================================

void AISubsystem::RegisterBehaviorTree(const std::string& name, BehaviorTreePtr tree)
{
    m_behaviorTreeTemplates[name] = std::move(tree);
    RVX_CORE_INFO("AISubsystem: Registered behavior tree '{}'", name);
}

BehaviorTree* AISubsystem::CreateBehaviorTreeInstance(uint64 entityId, 
                                                       const std::string& treeName)
{
    // Find template
    auto templateIt = m_behaviorTreeTemplates.find(treeName);
    if (templateIt == m_behaviorTreeTemplates.end())
    {
        RVX_CORE_ERROR("AISubsystem: Behavior tree '{}' not found", treeName);
        return nullptr;
    }

    // Clone the template
    auto instance = templateIt->second->Clone();
    BehaviorTree* ptr = instance.get();

    // Create blackboard for this entity if not exists
    if (m_blackboards.find(entityId) == m_blackboards.end())
    {
        m_blackboards[entityId] = std::make_unique<Blackboard>();
    }

    m_behaviorTreeInstances[entityId] = std::move(instance);
    RVX_CORE_INFO("AISubsystem: Created behavior tree instance '{}' for entity {}", 
                  treeName, entityId);

    return ptr;
}

BehaviorTree* AISubsystem::GetBehaviorTree(uint64 entityId)
{
    auto it = m_behaviorTreeInstances.find(entityId);
    return it != m_behaviorTreeInstances.end() ? it->second.get() : nullptr;
}

// =========================================================================
// Perception
// =========================================================================

void AISubsystem::RegisterPerception(uint64 entityId, AIPerception* perception)
{
    perception->SetOwnerId(entityId);
    m_perceptionComponents[entityId] = perception;
}

void AISubsystem::UnregisterPerception(uint64 entityId)
{
    m_perceptionComponents.erase(entityId);
}

void AISubsystem::ReportStimulus(const PerceptionStimulus& stimulus, bool excludeSource)
{
    for (auto& [entityId, perception] : m_perceptionComponents)
    {
        if (excludeSource && entityId == stimulus.sourceId)
        {
            continue;
        }
        perception->ProcessStimulus(stimulus);
    }
}

void AISubsystem::ReportNoise(const Vec3& location, float loudness, uint64 sourceId,
                              const std::string& tag)
{
    PerceptionStimulus stimulus;
    stimulus.sense = SenseType::Hearing;
    stimulus.location = location;
    stimulus.strength = loudness;
    stimulus.sourceId = sourceId;
    stimulus.tag = tag;

    ReportStimulus(stimulus, true);
}

// =========================================================================
// Internal Update Methods
// =========================================================================

void AISubsystem::UpdateAgents(float deltaTime)
{
    // Collect all agents for avoidance calculations
    std::vector<NavigationAgent*> allAgents;
    allAgents.reserve(m_agents.size());
    for (auto& [id, agent] : m_agents)
    {
        allAgents.push_back(agent.get());
    }

    // Update each agent
    for (auto& [id, agent] : m_agents)
    {
        // Get nearby agents for avoidance (simplified - use spatial query in real impl)
        std::vector<NavigationAgent*> nearbyAgents;
        const Vec3& pos = agent->GetPosition();
        const float avoidanceRadius = 5.0f;

        for (auto* other : allAgents)
        {
            if (other == agent.get()) continue;
            
            float dist = glm::length(other->GetPosition() - pos);
            if (dist < avoidanceRadius)
            {
                nearbyAgents.push_back(other);
            }
        }

        agent->Tick(deltaTime, m_pathFinder.get(), nearbyAgents);
    }
}

void AISubsystem::UpdateBehaviorTrees(float deltaTime)
{
    for (auto& [entityId, tree] : m_behaviorTreeInstances)
    {
        // Get blackboard for this entity
        Blackboard* blackboard = nullptr;
        auto bbIt = m_blackboards.find(entityId);
        if (bbIt != m_blackboards.end())
        {
            blackboard = bbIt->second.get();
        }

        tree->Tick(entityId, deltaTime, nullptr);
    }
}

void AISubsystem::UpdatePerception(float deltaTime)
{
    for (auto& [entityId, perception] : m_perceptionComponents)
    {
        // In a real implementation, we would get the entity's position and forward
        // from a transform component. For now, we skip the update here.
        // perception->Update(deltaTime, entityPosition, entityForward);
        (void)deltaTime;
        (void)perception;
    }
}

} // namespace RVX::AI
