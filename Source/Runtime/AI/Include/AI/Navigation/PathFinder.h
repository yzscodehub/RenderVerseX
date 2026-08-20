#pragma once

/**
 * @file PathFinder.h
 * @brief A* pathfinding on navigation mesh
 */

#include "AI/AITypes.h"
#include "AI/Navigation/NavMesh.h"
#include "Core/Types.h"

#include <vector>
#include <queue>
#include <unordered_map>
#include <memory>

namespace RVX::AI
{

/**
 * @brief Pathfinding query settings
 */
struct PathFindQuery
{
    Vec3 startPos;
    Vec3 endPos;
    NavQueryFilter filter;

    /// Maximum number of nodes to search (0 = unlimited)
    uint32 maxNodes = 2048;

    /// Maximum path length
    uint32 maxPathLength = 256;

    /// Heuristic weight (1.0 = A*, 0.0 = Dijkstra)
    float heuristicScale = 1.0f;
};

/**
 * @brief Detailed path result
 */
struct PathFindResult
{
    NavPath path;
    
    /// Polygons traversed
    std::vector<NavPolyRef> polys;

    /// Number of nodes searched
    uint32 nodesSearched = 0;

    /// Number of nodes in open list when finished
    uint32 openListSize = 0;
};

/**
 * @brief A* pathfinder for navigation mesh
 * 
 * Implements A* algorithm on the navmesh polygon graph.
 * Supports:
 * - Area-based cost modifiers
 * - String pulling for smooth paths
 * - Incremental/hierarchical pathfinding (future)
 * 
 * Usage:
 * @code
 * PathFinder pathFinder;
 * pathFinder.SetNavMesh(navMesh);
 * 
 * PathFindQuery query;
 * query.startPos = agentPosition;
 * query.endPos = targetPosition;
 * 
 * PathFindResult result;
 * if (pathFinder.FindPath(query, result))
 * {
 *     // Use result.path.waypoints
 * }
 * @endcode
 */
class PathFinder
{
public:
    // =========================================================================
    // Construction
    // =========================================================================
    PathFinder();
    ~PathFinder();

    PathFinder(const PathFinder&) = delete;
    PathFinder& operator=(const PathFinder&) = delete;

    // =========================================================================
    // Configuration
    // =========================================================================

    /**
     * @brief Set the navigation mesh to use
     */
    void SetNavMesh(NavMesh* navMesh);

    /**
     * @brief Get the current navigation mesh
     */
    NavMesh* GetNavMesh() const { return m_navMesh; }

    // =========================================================================
    // Pathfinding
    // =========================================================================

    /**
     * @brief Find a path between two points
     * @param query Pathfinding query parameters
     * @param outResult Output result
     * @return Query status
     */
    NavQueryStatus FindPath(const PathFindQuery& query, PathFindResult& outResult);

    /**
     * @brief Simplified path finding
     */
    NavQueryStatus FindPath(const Vec3& start, const Vec3& end,
                            NavPath& outPath,
                            const NavQueryFilter* filter = nullptr);

    /**
     * @brief Find the nearest point on the navmesh
     */
    bool FindNearestPoly(const Vec3& position, const Vec3& extents,
                         NavPoint& outPoint);

    // =========================================================================
    // Path Processing
    // =========================================================================

    /**
     * @brief Apply string pulling to simplify a path
     * 
     * String pulling creates the shortest path within the navigation corridor
     * by removing unnecessary waypoints.
     * 
     * @param polys Polygon corridor
     * @param start Starting position
     * @param end End position
     * @param outPath Output simplified path
     */
    void StringPull(const std::vector<NavPolyRef>& polys,
                    const Vec3& start, const Vec3& end,
                    NavPath& outPath);

    /**
     * @brief Raycast along the navmesh
     * @param start Starting position
     * @param end End position
     * @param outHitPoint Point where ray hit obstacle
     * @return True if ray was blocked
     */
    bool Raycast(const Vec3& start, const Vec3& end, Vec3& outHitPoint);

    /**
     * @brief Move along the navmesh (constrained movement)
     * @param startPos Current position (on navmesh)
     * @param endPos Desired end position
     * @param filter Query filter
     * @param outNewPos Actual position after movement
     * @param outPolys Visited polygons
     * @return True if movement succeeded
     */
    bool MoveAlongSurface(const Vec3& startPos, const Vec3& endPos,
                          const NavQueryFilter& filter,
                          Vec3& outNewPos,
                          std::vector<NavPolyRef>& outPolys);

private:
    // A* node
    struct SearchNode
    {
        NavPolyRef polyRef;
        NavPolyRef parentRef;
        float gCost;    // Cost from start
        float hCost;    // Heuristic to goal
        float fCost;    // Total cost

        bool operator>(const SearchNode& other) const
        {
            return fCost > other.fCost;
        }
    };

    NavMesh* m_navMesh = nullptr;

    // Search state (reused for efficiency)
    std::priority_queue<SearchNode, std::vector<SearchNode>, std::greater<SearchNode>> m_openList;
    std::unordered_map<NavPolyRef, SearchNode> m_closedList;
    std::vector<Vec3> m_tempPath;

    void ResetSearchState();
    float Heuristic(const Vec3& a, const Vec3& b) const;
    float GetPortalPoints(const NavPoly& fromPoly, const NavPoly& toPoly,
                          Vec3& left, Vec3& right) const;
};

} // namespace RVX::AI
