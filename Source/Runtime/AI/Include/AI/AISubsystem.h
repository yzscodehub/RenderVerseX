#pragma once

/**
 * @file AISubsystem.h
 * @brief World-level AI subsystem managing navigation, behavior trees, and perception
 */

#include "Core/Subsystem/WorldSubsystem.h"
#include "AI/AITypes.h"

#include <vector>
#include <memory>
#include <unordered_map>

namespace RVX::AI
{

/**
 * @brief World-level subsystem for AI management
 * 
 * The AISubsystem coordinates all AI functionality for a world:
 * - Navigation mesh and pathfinding
 * - Behavior tree execution
 * - Perception updates
 * - Agent crowd simulation
 * 
 * Usage:
 * @code
 * // In your world setup
 * auto& aiSystem = world->GetSubsystem<AISubsystem>();
 * 
 * // Build navigation mesh
 * NavMeshBuildSettings settings;
 * aiSystem.BuildNavMesh(meshData, settings);
 * 
 * // Register an AI agent
 * aiSystem.RegisterAgent(entityId, agentConfig);
 * 
 * // Request pathfinding
 * NavPath path;
 * aiSystem.FindPath(startPos, endPos, path);
 * @endcode
 */
class AISubsystem : public WorldSubsystem
{
public:
    // =========================================================================
    // Construction
    // =========================================================================
    AISubsystem();
    ~AISubsystem() override;

    // =========================================================================
    // ISubsystem Interface
    // =========================================================================
    const char* GetName() const override { return "AISubsystem"; }
    void Initialize() override;
    void Deinitialize() override;
    void Tick(float deltaTime) override;
    bool ShouldTick() const override { return true; }
    TickPhase GetTickPhase() const override { return TickPhase::Update; }

    // =========================================================================
    // Navigation
    // =========================================================================

    /**
     * @brief Get the active navigation mesh
     */
    NavMesh* GetNavMesh() const { return m_navMesh.get(); }

    /**
     * @brief Set the navigation mesh
     */
    void SetNavMesh(NavMeshPtr navMesh);

    /**
     * @brief Find a path between two points
     * @param start Starting position
     * @param end Destination position
     * @param outPath Output path result
     * @param filter Optional query filter for area costs
     * @return Query status
     */
    NavQueryStatus FindPath(const Vec3& start, const Vec3& end, 
                            NavPath& outPath,
                            const NavQueryFilter* filter = nullptr);

    /**
     * @brief Find the nearest point on the navmesh
     * @param position World position
     * @param searchExtent Search box half-extents
     * @param outPoint Output navmesh point
     * @return True if a point was found
     */
    bool FindNearestPoint(const Vec3& position, const Vec3& searchExtent,
                          NavPoint& outPoint);

    /**
     * @brief Raycast along the navmesh
     * @param start Starting point
     * @param end End point
     * @param outHitPoint Point where ray hit obstacle
     * @return True if ray was blocked
     */
    bool Raycast(const Vec3& start, const Vec3& end, Vec3& outHitPoint);

    // =========================================================================
    // Agent Management
    // =========================================================================

    /**
     * @brief Register a navigation agent
     * @param entityId Entity ID for the agent
     * @param config Agent configuration
     * @return Pointer to the created agent, or nullptr on failure
     */
    NavigationAgent* RegisterAgent(uint64 entityId, const struct AgentConfig& config);

    /**
     * @brief Unregister a navigation agent
     */
    void UnregisterAgent(uint64 entityId);

    /**
     * @brief Get an agent by entity ID
     */
    NavigationAgent* GetAgent(uint64 entityId);

    // =========================================================================
    // Behavior Trees
    // =========================================================================

    /**
     * @brief Register a behavior tree template
     */
    void RegisterBehaviorTree(const std::string& name, BehaviorTreePtr tree);

    /**
     * @brief Create a behavior tree instance for an entity
     */
    BehaviorTree* CreateBehaviorTreeInstance(uint64 entityId, const std::string& treeName);

    /**
     * @brief Get the behavior tree for an entity
     */
    BehaviorTree* GetBehaviorTree(uint64 entityId);

    // =========================================================================
    // Perception
    // =========================================================================

    /**
     * @brief Register a perception component
     */
    void RegisterPerception(uint64 entityId, AIPerception* perception);

    /**
     * @brief Unregister a perception component
     */
    void UnregisterPerception(uint64 entityId);

    /**
     * @brief Report a stimulus to the perception system
     * @param stimulus The stimulus to report
     * @param excludeSource If true, don't report to the source entity
     */
    void ReportStimulus(const PerceptionStimulus& stimulus, bool excludeSource = true);

    /**
     * @brief Report a noise at a location
     */
    void ReportNoise(const Vec3& location, float loudness, uint64 sourceId,
                     const std::string& tag = "");

    // =========================================================================
    // Debug
    // =========================================================================

    /**
     * @brief Enable/disable debug drawing
     */
    void SetDebugDrawEnabled(bool enabled) { m_debugDrawEnabled = enabled; }
    bool IsDebugDrawEnabled() const { return m_debugDrawEnabled; }

private:
    // Navigation
    NavMeshPtr m_navMesh;
    std::unique_ptr<PathFinder> m_pathFinder;
    std::unordered_map<uint64, std::unique_ptr<NavigationAgent>> m_agents;

    // Behavior Trees
    std::unordered_map<std::string, BehaviorTreePtr> m_behaviorTreeTemplates;
    std::unordered_map<uint64, std::unique_ptr<BehaviorTree>> m_behaviorTreeInstances;
    std::unordered_map<uint64, std::unique_ptr<Blackboard>> m_blackboards;

    // Perception
    std::unordered_map<uint64, AIPerception*> m_perceptionComponents;

    // Debug
    bool m_debugDrawEnabled = false;

    // Internal methods
    void UpdateAgents(float deltaTime);
    void UpdateBehaviorTrees(float deltaTime);
    void UpdatePerception(float deltaTime);
};

} // namespace RVX::AI
