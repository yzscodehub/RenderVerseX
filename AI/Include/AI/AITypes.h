#pragma once

/**
 * @file AITypes.h
 * @brief Common types and constants for the AI module
 */

#include "Core/Types.h"
#include "Core/MathTypes.h"

#include <functional>
#include <string>
#include <vector>
#include <memory>

namespace RVX::AI
{

// =========================================================================
// Forward Declarations
// =========================================================================
class AISubsystem;
class NavMesh;
class NavMeshBuilder;
class PathFinder;
class NavigationAgent;
class BehaviorTree;
class BTNode;
class Blackboard;
class AIPerception;
class SightSense;
class HearingSense;

// =========================================================================
// Type Aliases
// =========================================================================
using NavMeshPtr = std::shared_ptr<NavMesh>;
using BehaviorTreePtr = std::shared_ptr<BehaviorTree>;
using BlackboardPtr = std::shared_ptr<Blackboard>;

/// Unique identifier for navigation polygons
using NavPolyRef = uint64;

/// Invalid polygon reference
constexpr NavPolyRef RVX_NAV_INVALID_POLY = 0;

// =========================================================================
// Navigation Types
// =========================================================================

/**
 * @brief Result of a navigation query
 */
enum class NavQueryStatus : uint8
{
    Success,            ///< Query completed successfully
    PartialPath,        ///< Path found but incomplete (hit limit or partial obstacle)
    NoPath,             ///< No path exists
    InvalidStart,       ///< Start position is not on navmesh
    InvalidEnd,         ///< End position is not on navmesh
    OutOfNodes,         ///< Query ran out of search nodes
    Failed              ///< General failure
};

/**
 * @brief A point on the navigation mesh
 */
struct NavPoint
{
    Vec3 position;
    NavPolyRef polyRef = RVX_NAV_INVALID_POLY;

    NavPoint() = default;
    NavPoint(const Vec3& pos, NavPolyRef ref = RVX_NAV_INVALID_POLY)
        : position(pos), polyRef(ref) {}

    bool IsValid() const { return polyRef != RVX_NAV_INVALID_POLY; }
};

/**
 * @brief Navigation path result
 */
struct NavPath
{
    std::vector<Vec3> waypoints;
    NavQueryStatus status = NavQueryStatus::NoPath;
    float totalCost = 0.0f;

    bool IsValid() const 
    { 
        return status == NavQueryStatus::Success || 
               status == NavQueryStatus::PartialPath; 
    }

    bool IsComplete() const { return status == NavQueryStatus::Success; }
    size_t Size() const { return waypoints.size(); }
    bool Empty() const { return waypoints.empty(); }

    void Clear()
    {
        waypoints.clear();
        status = NavQueryStatus::NoPath;
        totalCost = 0.0f;
    }
};

/**
 * @brief Navigation area types for pathfinding costs
 */
enum class NavAreaType : uint8
{
    Ground = 0,     ///< Default walkable ground
    Road = 1,       ///< Faster movement (lower cost)
    Grass = 2,      ///< Normal terrain
    Sand = 3,       ///< Slower movement (higher cost)
    Water = 4,      ///< Shallow water (very slow)
    Obstacle = 5,   ///< Not walkable
    Jump = 6,       ///< Requires jump ability
    Custom1 = 7,
    Custom2 = 8,
    
    Count
};

/**
 * @brief Area costs for pathfinding
 */
struct NavQueryFilter
{
    float areaCosts[static_cast<size_t>(NavAreaType::Count)] = {
        1.0f,   // Ground
        0.5f,   // Road (faster)
        1.0f,   // Grass
        2.0f,   // Sand (slower)
        4.0f,   // Water
        FLT_MAX, // Obstacle (unwalkable)
        1.5f,   // Jump
        1.0f,   // Custom1
        1.0f    // Custom2
    };

    /// Ability flags (e.g., can swim, can jump)
    uint32 abilityFlags = 0xFFFFFFFF;

    void SetAreaCost(NavAreaType area, float cost)
    {
        areaCosts[static_cast<size_t>(area)] = cost;
    }

    float GetAreaCost(NavAreaType area) const
    {
        return areaCosts[static_cast<size_t>(area)];
    }
};

// =========================================================================
// Behavior Tree Types
// =========================================================================

/**
 * @brief Result of behavior tree node execution
 */
enum class BTStatus : uint8
{
    Success,    ///< Node completed successfully
    Failure,    ///< Node failed
    Running,    ///< Node is still executing
    Invalid     ///< Node is not initialized
};

/**
 * @brief Type of behavior tree node
 */
enum class BTNodeType : uint8
{
    Root,       ///< Root node of the tree
    Composite,  ///< Has multiple children (Selector, Sequence, etc.)
    Decorator,  ///< Wraps a single child (conditions, loops, etc.)
    Task,       ///< Leaf node that performs actions
    Service     ///< Background task that runs while parent is active
};

/**
 * @brief Abort mode for decorators
 */
enum class BTAbortMode : uint8
{
    None,       ///< Never abort
    Self,       ///< Abort self when condition changes
    LowerPriority, ///< Abort lower priority nodes
    Both        ///< Abort both self and lower priority
};

// =========================================================================
// Perception Types
// =========================================================================

/**
 * @brief Type of perception sense
 */
enum class SenseType : uint8
{
    Sight,
    Hearing,
    Damage,     ///< Taking damage
    Touch,      ///< Physical contact
    Custom
};

/**
 * @brief Affiliation for perception filtering
 */
enum class Affiliation : uint8
{
    Friendly,
    Neutral,
    Hostile
};

/**
 * @brief Stimulus received by perception system
 */
struct PerceptionStimulus
{
    SenseType sense = SenseType::Sight;
    Vec3 location;
    Vec3 direction;
    float strength = 1.0f;
    float age = 0.0f;           ///< Time since stimulus was received
    uint64 sourceId = 0;        ///< Entity ID of the source
    Affiliation affiliation = Affiliation::Neutral;
    bool isActive = true;

    /// Custom tag for filtering
    std::string tag;
};

/**
 * @brief Configuration for sight sense
 */
struct SightConfig
{
    float sightRadius = 20.0f;          ///< Maximum sight distance
    float loseSightRadius = 25.0f;      ///< Distance to lose sight (hysteresis)
    float peripheralVisionAngle = 60.0f; ///< Half-angle of peripheral vision (degrees)
    float sightAngle = 90.0f;           ///< Half-angle of full vision cone (degrees)
    float autoSuccessRange = 2.0f;      ///< Range where sight always succeeds
    bool requireLineOfSight = true;      ///< Check for obstructions
};

/**
 * @brief Configuration for hearing sense
 */
struct HearingConfig
{
    float hearingRange = 30.0f;         ///< Maximum hearing distance
    float loudnessThreshold = 0.1f;     ///< Minimum loudness to detect
    bool hearEnemiesOnly = false;       ///< Only hear hostile targets
};

// =========================================================================
// AI Agent Types
// =========================================================================

/**
 * @brief State of a navigation agent
 */
enum class AgentState : uint8
{
    Idle,           ///< Not moving
    Moving,         ///< Following a path
    Waiting,        ///< Waiting for other agents
    OffMesh,        ///< Traversing off-mesh link
    Arrived         ///< Reached destination
};

/**
 * @brief Movement request for navigation agent
 */
struct MoveRequest
{
    Vec3 destination;
    float acceptanceRadius = 0.5f;
    bool allowPartialPath = false;
    NavQueryFilter filter;
};

} // namespace RVX::AI
