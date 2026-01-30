#pragma once

/**
 * @file NavMeshBuilder.h
 * @brief Navigation mesh generation from geometry
 */

#include "AI/AITypes.h"
#include "AI/Navigation/NavMesh.h"
#include "Core/Types.h"

#include <vector>
#include <memory>
#include <functional>

namespace RVX::AI
{

/**
 * @brief Settings for navmesh generation
 */
struct NavMeshBuildSettings
{
    // =========================================================================
    // Agent Properties
    // =========================================================================
    float agentHeight = 2.0f;           ///< Height of the agent
    float agentRadius = 0.5f;           ///< Radius of the agent
    float agentMaxClimb = 0.4f;         ///< Maximum step height
    float agentMaxSlope = 45.0f;        ///< Maximum walkable slope (degrees)

    // =========================================================================
    // Voxelization
    // =========================================================================
    float cellSize = 0.3f;              ///< XZ cell size for voxelization
    float cellHeight = 0.2f;            ///< Y cell height for voxelization

    // =========================================================================
    // Region
    // =========================================================================
    uint32 minRegionArea = 8;           ///< Min cells for a region (filtered)
    uint32 mergeRegionArea = 20;        ///< Min cells to merge regions

    // =========================================================================
    // Polygon Mesh
    // =========================================================================
    float edgeMaxLength = 12.0f;        ///< Maximum edge length
    float edgeMaxError = 1.3f;          ///< Max error for simplified contour
    uint32 vertsPerPoly = 6;            ///< Max vertices per polygon

    // =========================================================================
    // Detail Mesh (for accurate height queries)
    // =========================================================================
    float detailSampleDist = 6.0f;      ///< Detail sampling distance
    float detailSampleMaxError = 1.0f;  ///< Detail max error

    // =========================================================================
    // Tiling (for large worlds)
    // =========================================================================
    bool enableTiling = false;
    float tileSize = 48.0f;             ///< Tile edge length in world units
};

/**
 * @brief Input geometry for navmesh building
 */
struct NavMeshBuildInput
{
    std::vector<Vec3> vertices;
    std::vector<uint32> indices;        ///< Triangle indices (triplets)

    /// Per-triangle area types (optional)
    std::vector<NavAreaType> areaTypes;

    /// Bounding box (computed if not set)
    AABB bounds;
    bool boundsSet = false;

    /**
     * @brief Add a triangle
     */
    void AddTriangle(const Vec3& v0, const Vec3& v1, const Vec3& v2,
                     NavAreaType area = NavAreaType::Ground);

    /**
     * @brief Add indexed triangles
     */
    void AddMesh(const std::vector<Vec3>& verts,
                 const std::vector<uint32>& inds,
                 NavAreaType area = NavAreaType::Ground);

    /**
     * @brief Compute bounding box from vertices
     */
    void ComputeBounds();

    /**
     * @brief Clear all data
     */
    void Clear();
};

/**
 * @brief Progress callback for long builds
 */
using NavMeshBuildProgress = std::function<void(const char* stage, float progress)>;

/**
 * @brief Build result
 */
struct NavMeshBuildResult
{
    NavMeshPtr navMesh;
    bool success = false;
    std::string errorMessage;

    // Statistics
    uint32 vertexCount = 0;
    uint32 polyCount = 0;
    float buildTimeMs = 0.0f;
};

/**
 * @brief Navigation mesh builder
 * 
 * Generates navigation meshes from input geometry using voxelization.
 * Can use RecastNavigation library for high-quality results, or a simpler
 * built-in algorithm when Recast is not available.
 * 
 * Usage:
 * @code
 * NavMeshBuilder builder;
 * 
 * NavMeshBuildInput input;
 * input.AddMesh(terrainVertices, terrainIndices);
 * 
 * NavMeshBuildSettings settings;
 * settings.agentRadius = 0.5f;
 * settings.agentHeight = 2.0f;
 * 
 * NavMeshBuildResult result = builder.Build(input, settings);
 * if (result.success)
 * {
 *     aiSubsystem.SetNavMesh(result.navMesh);
 * }
 * @endcode
 */
class NavMeshBuilder
{
public:
    // =========================================================================
    // Construction
    // =========================================================================
    NavMeshBuilder() = default;
    ~NavMeshBuilder() = default;

    // =========================================================================
    // Building
    // =========================================================================

    /**
     * @brief Build a navigation mesh
     * @param input Input geometry
     * @param settings Build settings
     * @param progressCallback Optional progress callback
     * @return Build result
     */
    NavMeshBuildResult Build(const NavMeshBuildInput& input,
                             const NavMeshBuildSettings& settings,
                             NavMeshBuildProgress progressCallback = nullptr);

    /**
     * @brief Build navmesh for a single tile (for tiled navmesh)
     */
    NavMeshBuildResult BuildTile(const NavMeshBuildInput& input,
                                 const NavMeshBuildSettings& settings,
                                 int tileX, int tileY,
                                 NavMeshBuildProgress progressCallback = nullptr);

    /**
     * @brief Check if RecastNavigation is available
     */
    static bool IsRecastAvailable();

private:
#ifdef RVX_AI_RECAST
    NavMeshBuildResult BuildWithRecast(const NavMeshBuildInput& input,
                                       const NavMeshBuildSettings& settings,
                                       NavMeshBuildProgress progressCallback);
#endif

    NavMeshBuildResult BuildSimple(const NavMeshBuildInput& input,
                                   const NavMeshBuildSettings& settings,
                                   NavMeshBuildProgress progressCallback);
};

} // namespace RVX::AI
