#pragma once

/**
 * @file NavMesh.h
 * @brief Navigation mesh data structure for pathfinding
 */

#include "AI/AITypes.h"
#include "Core/Types.h"
#include "Geometry/Geometry.h"

#include <vector>
#include <memory>

namespace RVX::AI
{

/**
 * @brief Navigation mesh polygon
 */
struct NavPoly
{
    NavPolyRef id = RVX_NAV_INVALID_POLY;
    std::vector<uint32> vertexIndices;
    std::vector<NavPolyRef> neighbors;  ///< Adjacent polygons (RVX_NAV_INVALID_POLY for edges)
    NavAreaType areaType = NavAreaType::Ground;
    uint16 flags = 0;

    Vec3 center;
    float height = 0.0f;
};

/**
 * @brief Off-mesh connection (jump, ladder, etc.)
 */
struct OffMeshConnection
{
    Vec3 startPos;
    Vec3 endPos;
    float radius = 0.5f;
    bool bidirectional = true;
    NavAreaType areaType = NavAreaType::Jump;
    uint32 flags = 0;
    uint32 userId = 0;  ///< User-defined identifier
};

/**
 * @brief Navigation mesh data
 * 
 * The NavMesh represents walkable areas decomposed into convex polygons.
 * It supports:
 * - Polygon-based pathfinding
 * - Multiple area types with different costs
 * - Off-mesh connections for special traversal
 * - Height field queries for multi-level navigation
 */
class NavMesh
{
public:
    // =========================================================================
    // Construction
    // =========================================================================
    NavMesh() = default;
    ~NavMesh() = default;

    NavMesh(const NavMesh&) = delete;
    NavMesh& operator=(const NavMesh&) = delete;

    // =========================================================================
    // Data Access
    // =========================================================================

    /**
     * @brief Check if the navmesh contains valid data
     */
    bool IsValid() const { return !m_polygons.empty(); }

    /**
     * @brief Get all polygons
     */
    const std::vector<NavPoly>& GetPolygons() const { return m_polygons; }

    /**
     * @brief Get all vertices
     */
    const std::vector<Vec3>& GetVertices() const { return m_vertices; }

    /**
     * @brief Get off-mesh connections
     */
    const std::vector<OffMeshConnection>& GetOffMeshConnections() const 
    { 
        return m_offMeshConnections; 
    }

    /**
     * @brief Get bounding box
     */
    const AABB& GetBounds() const { return m_bounds; }

    // =========================================================================
    // Queries
    // =========================================================================

    /**
     * @brief Find the polygon containing a point
     * @param position World position
     * @param searchExtent Search box half-extents
     * @return Polygon reference, or RVX_NAV_INVALID_POLY if not found
     */
    NavPolyRef FindNearestPoly(const Vec3& position, const Vec3& searchExtent) const;

    /**
     * @brief Get the closest point on a polygon
     */
    Vec3 ClosestPointOnPoly(NavPolyRef polyRef, const Vec3& position) const;

    /**
     * @brief Get polygon by reference
     */
    const NavPoly* GetPoly(NavPolyRef polyRef) const;

    /**
     * @brief Get the height at a position on the navmesh
     */
    bool GetHeight(const Vec3& position, float& outHeight) const;

    /**
     * @brief Check if a point is on the navmesh
     */
    bool IsPointOnNavMesh(const Vec3& position, const Vec3& searchExtent) const;

    /**
     * @brief Raycast on the navmesh
     * @param startPos Starting position
     * @param endPos End position
     * @param filter Query filter
     * @param outT Parameter along ray where hit occurred (0-1)
     * @param outHitNormal Normal at hit point
     * @return True if ray hit an edge
     */
    bool Raycast(const Vec3& startPos, const Vec3& endPos,
                 const NavQueryFilter& filter,
                 float& outT, Vec3& outHitNormal) const;

    // =========================================================================
    // Building
    // =========================================================================

    /**
     * @brief Clear all data
     */
    void Clear();

    /**
     * @brief Add a vertex
     * @return Index of the added vertex
     */
    uint32 AddVertex(const Vec3& position);

    /**
     * @brief Add a polygon
     * @param indices Vertex indices (must be convex, CCW order)
     * @param areaType Area type for pathfinding costs
     * @return Polygon reference
     */
    NavPolyRef AddPolygon(const std::vector<uint32>& indices, 
                          NavAreaType areaType = NavAreaType::Ground);

    /**
     * @brief Add an off-mesh connection
     */
    void AddOffMeshConnection(const OffMeshConnection& connection);

    /**
     * @brief Finalize the navmesh after building
     * This computes polygon centers, neighbors, and bounding box
     */
    void Finalize();

private:
    std::vector<Vec3> m_vertices;
    std::vector<NavPoly> m_polygons;
    std::vector<OffMeshConnection> m_offMeshConnections;
    AABB m_bounds;

    NavPolyRef m_nextPolyRef = 1;

    void ComputePolygonCenter(NavPoly& poly);
    void ComputeNeighbors();
    void ComputeBounds();
    bool PointInPoly(const NavPoly& poly, const Vec3& position) const;
};

} // namespace RVX::AI
