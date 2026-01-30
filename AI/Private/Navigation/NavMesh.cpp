/**
 * @file NavMesh.cpp
 * @brief Navigation mesh implementation
 */

#include "AI/Navigation/NavMesh.h"
#include "Core/Log.h"

#include <algorithm>
#include <cmath>

namespace RVX::AI
{

// =========================================================================
// Queries
// =========================================================================

NavPolyRef NavMesh::FindNearestPoly(const Vec3& position, const Vec3& searchExtent) const
{
    if (m_polygons.empty())
    {
        return RVX_NAV_INVALID_POLY;
    }

    // Simple brute force search - use spatial acceleration in production
    NavPolyRef nearest = RVX_NAV_INVALID_POLY;
    float nearestDist = FLT_MAX;

    // Search bounds
    AABB searchBounds(position - searchExtent, position + searchExtent);

    for (const auto& poly : m_polygons)
    {
        // Quick bounds check
        if (poly.center.x < searchBounds.GetMin().x - 5.0f || 
            poly.center.x > searchBounds.GetMax().x + 5.0f ||
            poly.center.z < searchBounds.GetMin().z - 5.0f || 
            poly.center.z > searchBounds.GetMax().z + 5.0f)
        {
            continue;
        }

        // Check if point is inside polygon (XZ plane)
        if (PointInPoly(poly, position))
        {
            // Check height
            float heightDiff = std::abs(position.y - poly.center.y);
            if (heightDiff < searchExtent.y)
            {
                return poly.id;  // Perfect match
            }
        }

        // Otherwise find closest
        Vec3 closest = ClosestPointOnPoly(poly.id, position);
        float dist = glm::length(closest - position);
        if (dist < nearestDist)
        {
            nearestDist = dist;
            nearest = poly.id;
        }
    }

    // Check if nearest is within search extent
    if (nearestDist > glm::length(searchExtent))
    {
        return RVX_NAV_INVALID_POLY;
    }

    return nearest;
}

Vec3 NavMesh::ClosestPointOnPoly(NavPolyRef polyRef, const Vec3& position) const
{
    const NavPoly* poly = GetPoly(polyRef);
    if (!poly || poly->vertexIndices.empty())
    {
        return position;
    }

    // Check if point is inside polygon
    if (PointInPoly(*poly, position))
    {
        // Project to polygon height
        return Vec3(position.x, poly->center.y, position.z);
    }

    // Find closest point on edges
    Vec3 closest = m_vertices[poly->vertexIndices[0]];
    float closestDist = FLT_MAX;

    for (size_t i = 0; i < poly->vertexIndices.size(); ++i)
    {
        size_t j = (i + 1) % poly->vertexIndices.size();
        const Vec3& a = m_vertices[poly->vertexIndices[i]];
        const Vec3& b = m_vertices[poly->vertexIndices[j]];

        // Project onto edge (XZ plane)
        Vec3 ab = b - a;
        Vec3 ap = position - a;

        float t = glm::dot(Vec3(ap.x, 0, ap.z), Vec3(ab.x, 0, ab.z)) / 
                  glm::dot(Vec3(ab.x, 0, ab.z), Vec3(ab.x, 0, ab.z));
        t = glm::clamp(t, 0.0f, 1.0f);

        Vec3 point = a + ab * t;
        float dist = glm::length(point - position);

        if (dist < closestDist)
        {
            closestDist = dist;
            closest = point;
        }
    }

    return closest;
}

const NavPoly* NavMesh::GetPoly(NavPolyRef polyRef) const
{
    // Linear search - use map for large navmeshes
    for (const auto& poly : m_polygons)
    {
        if (poly.id == polyRef)
        {
            return &poly;
        }
    }
    return nullptr;
}

bool NavMesh::GetHeight(const Vec3& position, float& outHeight) const
{
    NavPolyRef polyRef = FindNearestPoly(position, Vec3(1.0f, 10.0f, 1.0f));
    if (polyRef == RVX_NAV_INVALID_POLY)
    {
        return false;
    }

    const NavPoly* poly = GetPoly(polyRef);
    if (!poly)
    {
        return false;
    }

    outHeight = poly->center.y;
    return true;
}

bool NavMesh::IsPointOnNavMesh(const Vec3& position, const Vec3& searchExtent) const
{
    return FindNearestPoly(position, searchExtent) != RVX_NAV_INVALID_POLY;
}

bool NavMesh::Raycast(const Vec3& startPos, const Vec3& endPos,
                      const NavQueryFilter& filter,
                      float& outT, Vec3& outHitNormal) const
{
    (void)filter;  // TODO: Use filter for area costs

    NavPolyRef startPoly = FindNearestPoly(startPos, Vec3(2.0f));
    if (startPoly == RVX_NAV_INVALID_POLY)
    {
        outT = 0.0f;
        return true;  // Not on navmesh
    }

    // Simple raycast - step along ray and check if still on navmesh
    Vec3 dir = endPos - startPos;
    float length = glm::length(dir);
    if (length < 0.001f)
    {
        outT = 1.0f;
        return false;
    }

    dir = glm::normalize(dir);
    float stepSize = 0.5f;
    
    NavPolyRef currentPoly = startPoly;
    Vec3 currentPos = startPos;
    float traveled = 0.0f;

    while (traveled < length)
    {
        currentPos += dir * stepSize;
        traveled += stepSize;

        // Check if still on navmesh
        NavPolyRef newPoly = FindNearestPoly(currentPos, Vec3(1.0f));
        if (newPoly == RVX_NAV_INVALID_POLY)
        {
            outT = traveled / length;
            outHitNormal = -dir;  // Approximate
            return true;
        }

        currentPoly = newPoly;
    }

    outT = 1.0f;
    return false;
}

// =========================================================================
// Building
// =========================================================================

void NavMesh::Clear()
{
    m_vertices.clear();
    m_polygons.clear();
    m_offMeshConnections.clear();
    m_bounds = AABB();
    m_nextPolyRef = 1;
}

uint32 NavMesh::AddVertex(const Vec3& position)
{
    uint32 index = static_cast<uint32>(m_vertices.size());
    m_vertices.push_back(position);
    return index;
}

NavPolyRef NavMesh::AddPolygon(const std::vector<uint32>& indices, NavAreaType areaType)
{
    if (indices.size() < 3)
    {
        RVX_CORE_WARN("NavMesh: Cannot add polygon with less than 3 vertices");
        return RVX_NAV_INVALID_POLY;
    }

    NavPoly poly;
    poly.id = m_nextPolyRef++;
    poly.vertexIndices = indices;
    poly.areaType = areaType;
    poly.neighbors.resize(indices.size(), RVX_NAV_INVALID_POLY);

    m_polygons.push_back(std::move(poly));
    return m_polygons.back().id;
}

void NavMesh::AddOffMeshConnection(const OffMeshConnection& connection)
{
    m_offMeshConnections.push_back(connection);
}

void NavMesh::Finalize()
{
    // Compute polygon centers
    for (auto& poly : m_polygons)
    {
        ComputePolygonCenter(poly);
    }

    // Compute neighbors
    ComputeNeighbors();

    // Compute bounds
    ComputeBounds();

    RVX_CORE_INFO("NavMesh: Finalized with {} vertices, {} polygons, {} off-mesh connections",
                  m_vertices.size(), m_polygons.size(), m_offMeshConnections.size());
}

// =========================================================================
// Internal Methods
// =========================================================================

void NavMesh::ComputePolygonCenter(NavPoly& poly)
{
    if (poly.vertexIndices.empty())
    {
        poly.center = Vec3(0.0f);
        return;
    }

    Vec3 sum(0.0f);
    for (uint32 idx : poly.vertexIndices)
    {
        sum += m_vertices[idx];
    }
    poly.center = sum / static_cast<float>(poly.vertexIndices.size());
}

void NavMesh::ComputeNeighbors()
{
    // Simple O(n^2) neighbor detection - use spatial hashing for large navmeshes
    for (size_t i = 0; i < m_polygons.size(); ++i)
    {
        for (size_t j = i + 1; j < m_polygons.size(); ++j)
        {
            // Find shared edges
            auto& polyA = m_polygons[i];
            auto& polyB = m_polygons[j];

            for (size_t ei = 0; ei < polyA.vertexIndices.size(); ++ei)
            {
                size_t ei_next = (ei + 1) % polyA.vertexIndices.size();
                uint32 a0 = polyA.vertexIndices[ei];
                uint32 a1 = polyA.vertexIndices[ei_next];

                for (size_t ej = 0; ej < polyB.vertexIndices.size(); ++ej)
                {
                    size_t ej_next = (ej + 1) % polyB.vertexIndices.size();
                    uint32 b0 = polyB.vertexIndices[ej];
                    uint32 b1 = polyB.vertexIndices[ej_next];

                    // Check if edges match (in opposite direction)
                    if ((a0 == b1 && a1 == b0) || (a0 == b0 && a1 == b1))
                    {
                        polyA.neighbors[ei] = polyB.id;
                        polyB.neighbors[ej] = polyA.id;
                    }
                }
            }
        }
    }
}

void NavMesh::ComputeBounds()
{
    if (m_vertices.empty())
    {
        m_bounds = AABB();
        return;
    }

    m_bounds = AABB(m_vertices[0], m_vertices[0]);

    for (const auto& v : m_vertices)
    {
        m_bounds.Expand(v);
    }
}

bool NavMesh::PointInPoly(const NavPoly& poly, const Vec3& position) const
{
    if (poly.vertexIndices.size() < 3)
    {
        return false;
    }

    // Point-in-polygon test on XZ plane using cross product
    for (size_t i = 0; i < poly.vertexIndices.size(); ++i)
    {
        size_t j = (i + 1) % poly.vertexIndices.size();
        const Vec3& a = m_vertices[poly.vertexIndices[i]];
        const Vec3& b = m_vertices[poly.vertexIndices[j]];

        // 2D cross product (XZ plane)
        float cross = (b.x - a.x) * (position.z - a.z) - (b.z - a.z) * (position.x - a.x);
        if (cross < 0.0f)
        {
            return false;  // Point is outside this edge
        }
    }

    return true;
}

} // namespace RVX::AI
