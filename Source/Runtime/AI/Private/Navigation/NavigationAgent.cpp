/**
 * @file NavigationAgent.cpp
 * @brief Navigation agent implementation
 */

#include "AI/Navigation/NavigationAgent.h"
#include "AI/Navigation/PathFinder.h"
#include "Core/Log.h"

#include <algorithm>

namespace RVX::AI
{

// =========================================================================
// Construction
// =========================================================================

NavigationAgent::NavigationAgent(uint64 entityId, const AgentConfig& config)
    : m_entityId(entityId)
    , m_config(config)
{
}

NavigationAgent::~NavigationAgent() = default;

// =========================================================================
// Position
// =========================================================================

void NavigationAgent::SetPosition(const Vec3& position)
{
    m_position = position;
}

// =========================================================================
// Movement
// =========================================================================

bool NavigationAgent::SetDestination(const Vec3& destination, float acceptanceRadius)
{
    MoveRequest request;
    request.destination = destination;
    request.acceptanceRadius = acceptanceRadius;
    request.filter = m_config.queryFilter;
    return RequestMove(request);
}

bool NavigationAgent::RequestMove(const MoveRequest& request)
{
    m_destination = request.destination;
    m_acceptanceRadius = request.acceptanceRadius;
    m_config.queryFilter = request.filter;
    m_pathPending = true;
    m_state = AgentState::Moving;
    
    return true;
}

void NavigationAgent::Stop()
{
    m_velocity = Vec3(0.0f);
    m_desiredVelocity = Vec3(0.0f);
    m_state = AgentState::Idle;
    m_currentPath.Clear();
    m_pathPending = false;
}

void NavigationAgent::Resume()
{
    if (!m_currentPath.Empty())
    {
        m_state = AgentState::Moving;
    }
}

float NavigationAgent::GetRemainingDistance() const
{
    if (!HasPath() || m_currentWaypointIndex >= m_currentPath.waypoints.size())
    {
        return 0.0f;
    }

    float dist = glm::length(m_currentPath.waypoints[m_currentWaypointIndex] - m_position);
    
    for (size_t i = m_currentWaypointIndex + 1; i < m_currentPath.waypoints.size(); ++i)
    {
        dist += glm::length(m_currentPath.waypoints[i] - m_currentPath.waypoints[i - 1]);
    }
    
    return dist;
}

// =========================================================================
// Update
// =========================================================================

void NavigationAgent::Tick(float deltaTime, PathFinder* pathFinder,
                           const std::vector<NavigationAgent*>& nearbyAgents)
{
    // Handle pending path request
    if (m_pathPending && pathFinder)
    {
        m_pathPending = false;
        
        NavQueryStatus status = pathFinder->FindPath(
            m_position, m_destination, m_currentPath, &m_config.queryFilter);

        if (m_currentPath.IsValid())
        {
            m_currentWaypointIndex = 0;
            m_state = AgentState::Moving;
            
            if (m_onPathFound)
            {
                m_onPathFound(this);
            }
        }
        else
        {
            m_state = AgentState::Idle;
            
            if (m_onPathFailed)
            {
                m_onPathFailed(this);
            }
            
            RVX_CORE_WARN("NavigationAgent {}: Path finding failed (status: {})",
                          m_entityId, static_cast<int>(status));
        }
    }

    // Update based on state
    switch (m_state)
    {
        case AgentState::Moving:
            UpdatePathFollowing(deltaTime);
            m_desiredVelocity = ComputeSteeringForce(nearbyAgents);
            UpdateVelocity(deltaTime);
            break;

        case AgentState::Idle:
        case AgentState::Arrived:
            m_velocity = Vec3(0.0f);
            m_desiredVelocity = Vec3(0.0f);
            break;

        case AgentState::Waiting:
            // Could implement waiting behavior here
            break;

        case AgentState::OffMesh:
            // Off-mesh link traversal would go here
            break;
    }
}

// =========================================================================
// Internal Methods
// =========================================================================

void NavigationAgent::UpdatePathFollowing(float deltaTime)
{
    (void)deltaTime;

    if (!HasPath())
    {
        m_state = AgentState::Idle;
        return;
    }

    // Check if we've reached the current waypoint
    while (m_currentWaypointIndex < m_currentPath.waypoints.size())
    {
        const Vec3& waypoint = m_currentPath.waypoints[m_currentWaypointIndex];
        
        if (IsNearWaypoint(waypoint))
        {
            AdvanceWaypoint();
        }
        else
        {
            break;
        }
    }

    // Check if we've reached the destination
    if (m_currentWaypointIndex >= m_currentPath.waypoints.size())
    {
        float distToGoal = glm::length(m_position - m_destination);
        if (distToGoal <= m_acceptanceRadius)
        {
            m_state = AgentState::Arrived;
            m_currentPath.Clear();
            
            if (m_onArrived)
            {
                m_onArrived(this);
            }
        }
    }
}

void NavigationAgent::UpdateVelocity(float deltaTime)
{
    // Accelerate toward desired velocity
    Vec3 velDiff = m_desiredVelocity - m_velocity;
    float velDiffLen = glm::length(velDiff);
    
    if (velDiffLen > 0.001f)
    {
        float maxDelta = m_config.maxAcceleration * deltaTime;
        if (velDiffLen > maxDelta)
        {
            velDiff = glm::normalize(velDiff) * maxDelta;
        }
        m_velocity += velDiff;
    }

    // Clamp to max speed
    float speed = glm::length(m_velocity);
    if (speed > m_config.maxSpeed)
    {
        m_velocity = glm::normalize(m_velocity) * m_config.maxSpeed;
    }

    // Update position
    m_position += m_velocity * deltaTime;
}

Vec3 NavigationAgent::ComputeSteeringForce(const std::vector<NavigationAgent*>& nearbyAgents)
{
    Vec3 steering(0.0f);

    // Seek toward next waypoint
    Vec3 seekDir = GetNextWaypointDirection();
    if (glm::length(seekDir) > 0.001f)
    {
        steering += glm::normalize(seekDir) * m_config.maxSpeed;
    }

    // Add separation from other agents
    if (m_obstacleAvoidanceEnabled)
    {
        Vec3 separation = ComputeSeparation(nearbyAgents);
        steering += separation * m_config.separationWeight;
    }

    // Limit steering force
    float steeringLen = glm::length(steering);
    if (steeringLen > m_config.maxSpeed)
    {
        steering = glm::normalize(steering) * m_config.maxSpeed;
    }

    return steering;
}

Vec3 NavigationAgent::ComputeSeparation(const std::vector<NavigationAgent*>& nearbyAgents)
{
    Vec3 separation(0.0f);
    int count = 0;

    for (const auto* other : nearbyAgents)
    {
        Vec3 toSelf = m_position - other->GetPosition();
        float dist = glm::length(toSelf);
        
        // Separation radius
        float separationDist = m_config.radius + other->GetConfig().radius + 0.5f;
        
        if (dist < separationDist && dist > 0.001f)
        {
            // Weight by distance (closer = stronger)
            float weight = 1.0f - (dist / separationDist);
            separation += glm::normalize(toSelf) * weight;
            count++;
        }
    }

    if (count > 0)
    {
        separation /= static_cast<float>(count);
    }

    return separation;
}

Vec3 NavigationAgent::GetNextWaypointDirection() const
{
    if (!HasPath() || m_currentWaypointIndex >= m_currentPath.waypoints.size())
    {
        return Vec3(0.0f);
    }

    const Vec3& waypoint = m_currentPath.waypoints[m_currentWaypointIndex];
    Vec3 dir = waypoint - m_position;
    dir.y = 0.0f;  // Keep horizontal
    
    return dir;
}

void NavigationAgent::AdvanceWaypoint()
{
    m_currentWaypointIndex++;
}

bool NavigationAgent::IsNearWaypoint(const Vec3& waypoint) const
{
    Vec3 diff = waypoint - m_position;
    diff.y = 0.0f;  // Ignore height difference
    
    float dist = glm::length(diff);
    
    // Use smaller threshold for intermediate waypoints
    bool isFinalWaypoint = (m_currentWaypointIndex == m_currentPath.waypoints.size() - 1);
    float threshold = isFinalWaypoint ? m_acceptanceRadius : (m_config.radius * 2.0f);
    
    return dist <= threshold;
}

} // namespace RVX::AI
