#pragma once

/**
 * @file NavigationAgent.h
 * @brief AI agent that can navigate the navmesh
 */

#include "AI/AITypes.h"
#include "Core/Types.h"

#include <vector>
#include <functional>

namespace RVX::AI
{

class PathFinder;
class NavMesh;

/**
 * @brief Configuration for navigation agent
 */
struct AgentConfig
{
    float radius = 0.5f;                ///< Agent radius for collision
    float height = 2.0f;                ///< Agent height
    float maxSpeed = 5.0f;              ///< Maximum movement speed
    float maxAcceleration = 10.0f;      ///< Maximum acceleration
    float separationWeight = 1.0f;      ///< Weight for separation from other agents
    float obstacleAvoidanceWeight = 2.0f;  ///< Weight for obstacle avoidance

    /// Query filter for pathfinding
    NavQueryFilter queryFilter;
};

/**
 * @brief Callback for agent events
 */
using AgentCallback = std::function<void(NavigationAgent*)>;

/**
 * @brief Navigation agent for AI characters
 * 
 * A NavigationAgent handles:
 * - Path following with smooth steering
 * - Obstacle avoidance
 * - Crowd separation
 * - Off-mesh link traversal
 * 
 * Usage:
 * @code
 * AgentConfig config;
 * config.radius = 0.5f;
 * config.maxSpeed = 5.0f;
 * 
 * auto* agent = aiSubsystem.RegisterAgent(entityId, config);
 * agent->SetDestination(targetPosition);
 * 
 * // In update loop, the AISubsystem ticks all agents
 * // Query agent state:
 * if (agent->GetState() == AgentState::Arrived)
 * {
 *     // Reached destination
 * }
 * @endcode
 */
class NavigationAgent
{
public:
    // =========================================================================
    // Construction
    // =========================================================================
    NavigationAgent(uint64 entityId, const AgentConfig& config);
    ~NavigationAgent();

    NavigationAgent(const NavigationAgent&) = delete;
    NavigationAgent& operator=(const NavigationAgent&) = delete;

    // =========================================================================
    // Properties
    // =========================================================================

    /**
     * @brief Get the entity ID
     */
    uint64 GetEntityId() const { return m_entityId; }

    /**
     * @brief Get current state
     */
    AgentState GetState() const { return m_state; }

    /**
     * @brief Get current position
     */
    const Vec3& GetPosition() const { return m_position; }

    /**
     * @brief Set current position (e.g., from physics)
     */
    void SetPosition(const Vec3& position);

    /**
     * @brief Get current velocity
     */
    const Vec3& GetVelocity() const { return m_velocity; }

    /**
     * @brief Get desired velocity (what the agent wants to move)
     */
    const Vec3& GetDesiredVelocity() const { return m_desiredVelocity; }

    /**
     * @brief Get the configuration
     */
    const AgentConfig& GetConfig() const { return m_config; }

    /**
     * @brief Modify configuration
     */
    AgentConfig& GetConfig() { return m_config; }

    // =========================================================================
    // Movement
    // =========================================================================

    /**
     * @brief Request movement to a destination
     * @param destination Target position
     * @param acceptanceRadius How close to get before stopping
     * @return True if path request was initiated
     */
    bool SetDestination(const Vec3& destination, float acceptanceRadius = 0.5f);

    /**
     * @brief Request movement with detailed options
     */
    bool RequestMove(const MoveRequest& request);

    /**
     * @brief Stop movement
     */
    void Stop();

    /**
     * @brief Resume movement after stop
     */
    void Resume();

    /**
     * @brief Get the current destination
     */
    const Vec3& GetDestination() const { return m_destination; }

    /**
     * @brief Get the current path
     */
    const NavPath& GetPath() const { return m_currentPath; }

    /**
     * @brief Get current waypoint index
     */
    uint32 GetCurrentWaypointIndex() const { return m_currentWaypointIndex; }

    /**
     * @brief Get remaining path distance
     */
    float GetRemainingDistance() const;

    /**
     * @brief Check if agent has a valid path
     */
    bool HasPath() const { return m_currentPath.IsValid() && !m_currentPath.Empty(); }

    // =========================================================================
    // Avoidance
    // =========================================================================

    /**
     * @brief Enable/disable obstacle avoidance
     */
    void SetObstacleAvoidanceEnabled(bool enabled) { m_obstacleAvoidanceEnabled = enabled; }
    bool IsObstacleAvoidanceEnabled() const { return m_obstacleAvoidanceEnabled; }

    /**
     * @brief Set avoidance priority (higher = less likely to give way)
     */
    void SetAvoidancePriority(int priority) { m_avoidancePriority = priority; }
    int GetAvoidancePriority() const { return m_avoidancePriority; }

    // =========================================================================
    // Callbacks
    // =========================================================================

    /**
     * @brief Set callback for when destination is reached
     */
    void SetOnArrived(AgentCallback callback) { m_onArrived = std::move(callback); }

    /**
     * @brief Set callback for when path is found
     */
    void SetOnPathFound(AgentCallback callback) { m_onPathFound = std::move(callback); }

    /**
     * @brief Set callback for when path fails
     */
    void SetOnPathFailed(AgentCallback callback) { m_onPathFailed = std::move(callback); }

    // =========================================================================
    // Update
    // =========================================================================

    /**
     * @brief Update the agent (called by AISubsystem)
     */
    void Tick(float deltaTime, PathFinder* pathFinder,
              const std::vector<NavigationAgent*>& nearbyAgents);

private:
    uint64 m_entityId;
    AgentConfig m_config;
    AgentState m_state = AgentState::Idle;

    // Position and movement
    Vec3 m_position{0.0f};
    Vec3 m_velocity{0.0f};
    Vec3 m_desiredVelocity{0.0f};
    NavPolyRef m_currentPoly = RVX_NAV_INVALID_POLY;

    // Path following
    Vec3 m_destination{0.0f};
    float m_acceptanceRadius = 0.5f;
    NavPath m_currentPath;
    uint32 m_currentWaypointIndex = 0;
    bool m_pathPending = false;

    // Avoidance
    bool m_obstacleAvoidanceEnabled = true;
    int m_avoidancePriority = 50;

    // Callbacks
    AgentCallback m_onArrived;
    AgentCallback m_onPathFound;
    AgentCallback m_onPathFailed;

    // Internal methods
    void UpdatePathFollowing(float deltaTime);
    void UpdateVelocity(float deltaTime);
    Vec3 ComputeSteeringForce(const std::vector<NavigationAgent*>& nearbyAgents);
    Vec3 ComputeSeparation(const std::vector<NavigationAgent*>& nearbyAgents);
    Vec3 GetNextWaypointDirection() const;
    void AdvanceWaypoint();
    bool IsNearWaypoint(const Vec3& waypoint) const;
};

} // namespace RVX::AI
