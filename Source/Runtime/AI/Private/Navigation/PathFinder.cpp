/**
 * @file PathFinder.cpp
 * @brief A* pathfinding implementation
 */

#include "AI/Navigation/PathFinder.h"
#include "Core/Log.h"

#include <algorithm>
#include <cmath>

namespace RVX::AI
{

// =========================================================================
// Construction
// =========================================================================

PathFinder::PathFinder() = default;
PathFinder::~PathFinder() = default;

// =========================================================================
// Configuration
// =========================================================================

void PathFinder::SetNavMesh(NavMesh* navMesh)
{
    m_navMesh = navMesh;
}

// =========================================================================
// Pathfinding
// =========================================================================

NavQueryStatus PathFinder::FindPath(const PathFindQuery& query, PathFindResult& outResult)
{
    outResult = PathFindResult();

    if (!m_navMesh || !m_navMesh->IsValid())
    {
        outResult.path.status = NavQueryStatus::Failed;
        return NavQueryStatus::Failed;
    }

    // Find start and end polygons
    Vec3 searchExtent(2.0f, 4.0f, 2.0f);
    NavPolyRef startPoly = m_navMesh->FindNearestPoly(query.startPos, searchExtent);
    NavPolyRef endPoly = m_navMesh->FindNearestPoly(query.endPos, searchExtent);

    if (startPoly == RVX_NAV_INVALID_POLY)
    {
        outResult.path.status = NavQueryStatus::InvalidStart;
        return NavQueryStatus::InvalidStart;
    }

    if (endPoly == RVX_NAV_INVALID_POLY)
    {
        outResult.path.status = NavQueryStatus::InvalidEnd;
        return NavQueryStatus::InvalidEnd;
    }

    // Same polygon - direct path
    if (startPoly == endPoly)
    {
        outResult.path.waypoints.push_back(query.startPos);
        outResult.path.waypoints.push_back(query.endPos);
        outResult.path.status = NavQueryStatus::Success;
        outResult.polys.push_back(startPoly);
        return NavQueryStatus::Success;
    }

    // A* search
    ResetSearchState();

    SearchNode startNode;
    startNode.polyRef = startPoly;
    startNode.parentRef = RVX_NAV_INVALID_POLY;
    startNode.gCost = 0.0f;
    startNode.hCost = Heuristic(m_navMesh->GetPoly(startPoly)->center, query.endPos);
    startNode.fCost = startNode.gCost + startNode.hCost * query.heuristicScale;

    m_openList.push(startNode);

    uint32 nodesSearched = 0;
    NavPolyRef bestPoly = RVX_NAV_INVALID_POLY;
    float bestH = FLT_MAX;

    while (!m_openList.empty())
    {
        SearchNode current = m_openList.top();
        m_openList.pop();

        // Skip if already in closed list with better cost
        auto closedIt = m_closedList.find(current.polyRef);
        if (closedIt != m_closedList.end())
        {
            continue;
        }

        // Add to closed list
        m_closedList[current.polyRef] = current;
        nodesSearched++;

        // Track best node in case we don't reach the goal
        if (current.hCost < bestH)
        {
            bestH = current.hCost;
            bestPoly = current.polyRef;
        }

        // Reached goal
        if (current.polyRef == endPoly)
        {
            break;
        }

        // Check node limit
        if (query.maxNodes > 0 && nodesSearched >= query.maxNodes)
        {
            break;
        }

        // Expand neighbors
        const NavPoly* poly = m_navMesh->GetPoly(current.polyRef);
        if (!poly) continue;

        for (size_t i = 0; i < poly->neighbors.size(); ++i)
        {
            NavPolyRef neighborRef = poly->neighbors[i];
            if (neighborRef == RVX_NAV_INVALID_POLY)
            {
                continue;
            }

            // Skip if in closed list
            if (m_closedList.find(neighborRef) != m_closedList.end())
            {
                continue;
            }

            const NavPoly* neighborPoly = m_navMesh->GetPoly(neighborRef);
            if (!neighborPoly)
            {
                continue;
            }

            // Calculate cost
            float areaCost = query.filter.GetAreaCost(neighborPoly->areaType);
            if (areaCost >= FLT_MAX)
            {
                continue;  // Unwalkable
            }

            float edgeCost = glm::length(neighborPoly->center - poly->center);
            float gCost = current.gCost + edgeCost * areaCost;
            float hCost = Heuristic(neighborPoly->center, query.endPos);
            float fCost = gCost + hCost * query.heuristicScale;

            SearchNode neighborNode;
            neighborNode.polyRef = neighborRef;
            neighborNode.parentRef = current.polyRef;
            neighborNode.gCost = gCost;
            neighborNode.hCost = hCost;
            neighborNode.fCost = fCost;

            m_openList.push(neighborNode);
        }
    }

    outResult.nodesSearched = nodesSearched;
    outResult.openListSize = static_cast<uint32>(m_openList.size());

    // Reconstruct path
    NavPolyRef traceRef = (m_closedList.find(endPoly) != m_closedList.end()) ? endPoly : bestPoly;
    
    if (traceRef == RVX_NAV_INVALID_POLY)
    {
        outResult.path.status = NavQueryStatus::NoPath;
        return NavQueryStatus::NoPath;
    }

    // Build polygon path
    std::vector<NavPolyRef> polyPath;
    while (traceRef != RVX_NAV_INVALID_POLY)
    {
        polyPath.push_back(traceRef);
        auto it = m_closedList.find(traceRef);
        if (it == m_closedList.end())
        {
            break;
        }
        traceRef = it->second.parentRef;

        if (polyPath.size() > query.maxPathLength)
        {
            break;
        }
    }

    std::reverse(polyPath.begin(), polyPath.end());
    outResult.polys = polyPath;

    // String pull to get smooth path
    StringPull(polyPath, query.startPos, query.endPos, outResult.path);

    // Determine status
    if (polyPath.back() == endPoly)
    {
        outResult.path.status = NavQueryStatus::Success;
    }
    else
    {
        outResult.path.status = NavQueryStatus::PartialPath;
    }

    return outResult.path.status;
}

NavQueryStatus PathFinder::FindPath(const Vec3& start, const Vec3& end,
                                    NavPath& outPath,
                                    const NavQueryFilter* filter)
{
    PathFindQuery query;
    query.startPos = start;
    query.endPos = end;
    if (filter)
    {
        query.filter = *filter;
    }

    PathFindResult result;
    NavQueryStatus status = FindPath(query, result);
    outPath = std::move(result.path);
    return status;
}

bool PathFinder::FindNearestPoly(const Vec3& position, const Vec3& extents,
                                 NavPoint& outPoint)
{
    if (!m_navMesh)
    {
        return false;
    }

    NavPolyRef polyRef = m_navMesh->FindNearestPoly(position, extents);
    if (polyRef == RVX_NAV_INVALID_POLY)
    {
        return false;
    }

    outPoint.polyRef = polyRef;
    outPoint.position = m_navMesh->ClosestPointOnPoly(polyRef, position);
    return true;
}

// =========================================================================
// Path Processing
// =========================================================================

void PathFinder::StringPull(const std::vector<NavPolyRef>& polys,
                            const Vec3& start, const Vec3& end,
                            NavPath& outPath)
{
    outPath.Clear();

    if (polys.empty())
    {
        return;
    }

    if (polys.size() == 1)
    {
        outPath.waypoints.push_back(start);
        outPath.waypoints.push_back(end);
        return;
    }

    // Simple string pulling implementation
    // For production, use the funnel algorithm
    
    outPath.waypoints.push_back(start);

    // Add polygon centers as waypoints (simplified)
    for (size_t i = 1; i < polys.size() - 1; ++i)
    {
        const NavPoly* poly = m_navMesh->GetPoly(polys[i]);
        if (poly)
        {
            // Get portal between this poly and previous
            const NavPoly* prevPoly = m_navMesh->GetPoly(polys[i - 1]);
            if (prevPoly)
            {
                Vec3 left, right;
                GetPortalPoints(*prevPoly, *poly, left, right);
                Vec3 portalMid = (left + right) * 0.5f;
                outPath.waypoints.push_back(portalMid);
            }
        }
    }

    outPath.waypoints.push_back(end);

    // Calculate total cost
    for (size_t i = 1; i < outPath.waypoints.size(); ++i)
    {
        outPath.totalCost += glm::length(outPath.waypoints[i] - outPath.waypoints[i - 1]);
    }
}

bool PathFinder::Raycast(const Vec3& start, const Vec3& end, Vec3& outHitPoint)
{
    if (!m_navMesh)
    {
        return false;
    }

    NavQueryFilter filter;
    float t;
    Vec3 hitNormal;

    if (m_navMesh->Raycast(start, end, filter, t, hitNormal))
    {
        outHitPoint = start + (end - start) * t;
        return true;
    }

    return false;
}

bool PathFinder::MoveAlongSurface(const Vec3& startPos, const Vec3& endPos,
                                  const NavQueryFilter& filter,
                                  Vec3& outNewPos,
                                  std::vector<NavPolyRef>& outPolys)
{
    (void)filter;
    outPolys.clear();

    if (!m_navMesh)
    {
        outNewPos = startPos;
        return false;
    }

    // Find start polygon
    Vec3 searchExtent(2.0f, 4.0f, 2.0f);
    NavPolyRef startPoly = m_navMesh->FindNearestPoly(startPos, searchExtent);
    if (startPoly == RVX_NAV_INVALID_POLY)
    {
        outNewPos = startPos;
        return false;
    }

    outPolys.push_back(startPoly);

    // Simple implementation - raycast to see how far we can go
    Vec3 hitPoint;
    if (Raycast(startPos, endPos, hitPoint))
    {
        outNewPos = hitPoint;
    }
    else
    {
        // Can reach end directly
        outNewPos = endPos;
    }

    // Snap to navmesh
    float height;
    if (m_navMesh->GetHeight(outNewPos, height))
    {
        outNewPos.y = height;
    }

    return true;
}

// =========================================================================
// Internal Methods
// =========================================================================

void PathFinder::ResetSearchState()
{
    while (!m_openList.empty())
    {
        m_openList.pop();
    }
    m_closedList.clear();
    m_tempPath.clear();
}

float PathFinder::Heuristic(const Vec3& a, const Vec3& b) const
{
    // Euclidean distance
    return glm::length(b - a);
}

float PathFinder::GetPortalPoints(const NavPoly& fromPoly, const NavPoly& toPoly,
                                  Vec3& left, Vec3& right) const
{
    // Find shared edge between polygons
    for (size_t i = 0; i < fromPoly.neighbors.size(); ++i)
    {
        if (fromPoly.neighbors[i] == toPoly.id)
        {
            size_t j = (i + 1) % fromPoly.vertexIndices.size();
            left = m_navMesh->GetVertices()[fromPoly.vertexIndices[i]];
            right = m_navMesh->GetVertices()[fromPoly.vertexIndices[j]];
            return glm::length(right - left);
        }
    }

    // Fallback to polygon centers
    left = fromPoly.center;
    right = toPoly.center;
    return glm::length(right - left);
}

} // namespace RVX::AI
