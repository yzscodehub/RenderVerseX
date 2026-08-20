/**
 * @file NavMeshBuilder.cpp
 * @brief Navigation mesh builder implementation
 */

#include "AI/Navigation/NavMeshBuilder.h"
#include "Core/Log.h"

#include <chrono>
#include <algorithm>

#ifdef RVX_AI_RECAST
#include <recastnavigation/Recast.h>
#include <recastnavigation/DetourNavMesh.h>
#include <recastnavigation/DetourNavMeshBuilder.h>
#endif

namespace RVX::AI
{

// =========================================================================
// NavMeshBuildInput
// =========================================================================

void NavMeshBuildInput::AddTriangle(const Vec3& v0, const Vec3& v1, const Vec3& v2,
                                    NavAreaType area)
{
    uint32 baseIdx = static_cast<uint32>(vertices.size());
    vertices.push_back(v0);
    vertices.push_back(v1);
    vertices.push_back(v2);
    
    indices.push_back(baseIdx);
    indices.push_back(baseIdx + 1);
    indices.push_back(baseIdx + 2);
    
    areaTypes.push_back(area);
}

void NavMeshBuildInput::AddMesh(const std::vector<Vec3>& verts,
                                const std::vector<uint32>& inds,
                                NavAreaType area)
{
    uint32 baseIdx = static_cast<uint32>(vertices.size());
    
    // Add vertices
    vertices.insert(vertices.end(), verts.begin(), verts.end());
    
    // Add indices with offset
    for (uint32 idx : inds)
    {
        indices.push_back(baseIdx + idx);
    }
    
    // Add area types for each triangle
    size_t numTris = inds.size() / 3;
    for (size_t i = 0; i < numTris; ++i)
    {
        areaTypes.push_back(area);
    }
}

void NavMeshBuildInput::ComputeBounds()
{
    if (vertices.empty())
    {
        bounds = AABB();
        boundsSet = false;
        return;
    }

    bounds = AABB(vertices[0], vertices[0]);

    for (const auto& v : vertices)
    {
        bounds.Expand(v);
    }

    boundsSet = true;
}

void NavMeshBuildInput::Clear()
{
    vertices.clear();
    indices.clear();
    areaTypes.clear();
    boundsSet = false;
}

// =========================================================================
// NavMeshBuilder
// =========================================================================

bool NavMeshBuilder::IsRecastAvailable()
{
#ifdef RVX_AI_RECAST
    return true;
#else
    return false;
#endif
}

NavMeshBuildResult NavMeshBuilder::Build(const NavMeshBuildInput& input,
                                         const NavMeshBuildSettings& settings,
                                         NavMeshBuildProgress progressCallback)
{
    auto startTime = std::chrono::high_resolution_clock::now();

    NavMeshBuildResult result;

    // Validate input
    if (input.vertices.empty() || input.indices.empty())
    {
        result.errorMessage = "Empty input geometry";
        return result;
    }

    if (input.indices.size() % 3 != 0)
    {
        result.errorMessage = "Invalid index count (must be multiple of 3)";
        return result;
    }

    if (progressCallback)
    {
        progressCallback("Initializing", 0.0f);
    }

#ifdef RVX_AI_RECAST
    result = BuildWithRecast(input, settings, progressCallback);
#else
    result = BuildSimple(input, settings, progressCallback);
#endif

    auto endTime = std::chrono::high_resolution_clock::now();
    result.buildTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();

    if (result.success)
    {
        RVX_CORE_INFO("NavMeshBuilder: Built navmesh in {:.2f}ms ({} polys, {} verts)",
                      result.buildTimeMs, result.polyCount, result.vertexCount);
    }
    else
    {
        RVX_CORE_ERROR("NavMeshBuilder: Failed - {}", result.errorMessage);
    }

    return result;
}

NavMeshBuildResult NavMeshBuilder::BuildTile(const NavMeshBuildInput& input,
                                             const NavMeshBuildSettings& settings,
                                             int tileX, int tileY,
                                             NavMeshBuildProgress progressCallback)
{
    // For tiled navmesh, clip input to tile bounds and build
    (void)tileX;
    (void)tileY;
    
    // Simplified - just use regular build for now
    return Build(input, settings, progressCallback);
}

#ifdef RVX_AI_RECAST
NavMeshBuildResult NavMeshBuilder::BuildWithRecast(const NavMeshBuildInput& input,
                                                   const NavMeshBuildSettings& settings,
                                                   NavMeshBuildProgress progressCallback)
{
    NavMeshBuildResult result;

    // Compute bounds if not set
    AABB bounds = input.bounds;
    if (!input.boundsSet)
    {
        bounds = AABB(input.vertices[0], input.vertices[0]);
        for (const auto& v : input.vertices)
        {
            bounds.Expand(v);
        }
    }

    // Initialize Recast config
    rcConfig cfg;
    memset(&cfg, 0, sizeof(cfg));

    cfg.cs = settings.cellSize;
    cfg.ch = settings.cellHeight;
    cfg.walkableSlopeAngle = settings.agentMaxSlope;
    cfg.walkableHeight = static_cast<int>(ceilf(settings.agentHeight / cfg.ch));
    cfg.walkableClimb = static_cast<int>(floorf(settings.agentMaxClimb / cfg.ch));
    cfg.walkableRadius = static_cast<int>(ceilf(settings.agentRadius / cfg.cs));
    cfg.maxEdgeLen = static_cast<int>(settings.edgeMaxLength / cfg.cs);
    cfg.maxSimplificationError = settings.edgeMaxError;
    cfg.minRegionArea = settings.minRegionArea;
    cfg.mergeRegionArea = settings.mergeRegionArea;
    cfg.maxVertsPerPoly = settings.vertsPerPoly;
    cfg.detailSampleDist = settings.detailSampleDist < 0.9f ? 0 : cfg.cs * settings.detailSampleDist;
    cfg.detailSampleMaxError = cfg.ch * settings.detailSampleMaxError;

    rcVcopy(cfg.bmin, &bounds.GetMin().x);
    rcVcopy(cfg.bmax, &bounds.GetMax().x);
    rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height);

    // Build context for logging
    rcContext ctx;

    if (progressCallback) progressCallback("Creating heightfield", 0.1f);

    // Create heightfield
    rcHeightfield* solid = rcAllocHeightfield();
    if (!rcCreateHeightfield(&ctx, *solid, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch))
    {
        result.errorMessage = "Failed to create heightfield";
        rcFreeHeightField(solid);
        return result;
    }

    // Convert input to Recast format
    std::vector<float> verts(input.vertices.size() * 3);
    for (size_t i = 0; i < input.vertices.size(); ++i)
    {
        verts[i * 3 + 0] = input.vertices[i].x;
        verts[i * 3 + 1] = input.vertices[i].y;
        verts[i * 3 + 2] = input.vertices[i].z;
    }

    std::vector<int> tris(input.indices.size());
    for (size_t i = 0; i < input.indices.size(); ++i)
    {
        tris[i] = static_cast<int>(input.indices[i]);
    }

    int ntris = static_cast<int>(tris.size() / 3);

    // Mark triangles
    std::vector<unsigned char> areas(ntris);
    memset(areas.data(), 0, areas.size());
    rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, verts.data(), 
                            static_cast<int>(input.vertices.size()),
                            tris.data(), ntris, areas.data());

    if (progressCallback) progressCallback("Rasterizing triangles", 0.2f);

    // Rasterize
    if (!rcRasterizeTriangles(&ctx, verts.data(), static_cast<int>(input.vertices.size()),
                              tris.data(), areas.data(), ntris, *solid, cfg.walkableClimb))
    {
        result.errorMessage = "Failed to rasterize triangles";
        rcFreeHeightField(solid);
        return result;
    }

    if (progressCallback) progressCallback("Filtering walkables", 0.3f);

    // Filter
    rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *solid);
    rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *solid);
    rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *solid);

    if (progressCallback) progressCallback("Building compact heightfield", 0.4f);

    // Compact heightfield
    rcCompactHeightfield* chf = rcAllocCompactHeightfield();
    if (!rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *solid, *chf))
    {
        result.errorMessage = "Failed to build compact heightfield";
        rcFreeHeightField(solid);
        rcFreeCompactHeightfield(chf);
        return result;
    }
    rcFreeHeightField(solid);

    if (progressCallback) progressCallback("Eroding walkable area", 0.5f);

    if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *chf))
    {
        result.errorMessage = "Failed to erode walkable area";
        rcFreeCompactHeightfield(chf);
        return result;
    }

    if (progressCallback) progressCallback("Building regions", 0.6f);

    if (!rcBuildDistanceField(&ctx, *chf))
    {
        result.errorMessage = "Failed to build distance field";
        rcFreeCompactHeightfield(chf);
        return result;
    }

    if (!rcBuildRegions(&ctx, *chf, 0, cfg.minRegionArea, cfg.mergeRegionArea))
    {
        result.errorMessage = "Failed to build regions";
        rcFreeCompactHeightfield(chf);
        return result;
    }

    if (progressCallback) progressCallback("Building contours", 0.7f);

    rcContourSet* cset = rcAllocContourSet();
    if (!rcBuildContours(&ctx, *chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *cset))
    {
        result.errorMessage = "Failed to build contours";
        rcFreeCompactHeightfield(chf);
        rcFreeContourSet(cset);
        return result;
    }

    if (progressCallback) progressCallback("Building polygon mesh", 0.8f);

    rcPolyMesh* pmesh = rcAllocPolyMesh();
    if (!rcBuildPolyMesh(&ctx, *cset, cfg.maxVertsPerPoly, *pmesh))
    {
        result.errorMessage = "Failed to build polygon mesh";
        rcFreeCompactHeightfield(chf);
        rcFreeContourSet(cset);
        rcFreePolyMesh(pmesh);
        return result;
    }

    rcFreeContourSet(cset);
    rcFreeCompactHeightfield(chf);

    if (progressCallback) progressCallback("Creating navmesh", 0.9f);

    // Convert to our NavMesh format
    auto navMesh = std::make_shared<NavMesh>();

    // Add vertices
    for (int i = 0; i < pmesh->nverts; ++i)
    {
        const unsigned short* v = &pmesh->verts[i * 3];
        Vec3 pos;
        pos.x = cfg.bmin[0] + v[0] * cfg.cs;
        pos.y = cfg.bmin[1] + v[1] * cfg.ch;
        pos.z = cfg.bmin[2] + v[2] * cfg.cs;
        navMesh->AddVertex(pos);
    }

    // Add polygons
    for (int i = 0; i < pmesh->npolys; ++i)
    {
        const unsigned short* p = &pmesh->polys[i * pmesh->nvp * 2];
        std::vector<uint32> indices;

        for (int j = 0; j < pmesh->nvp; ++j)
        {
            if (p[j] == RC_MESH_NULL_IDX)
                break;
            indices.push_back(p[j]);
        }

        if (indices.size() >= 3)
        {
            NavAreaType area = static_cast<NavAreaType>(pmesh->areas[i]);
            navMesh->AddPolygon(indices, area);
        }
    }

    rcFreePolyMesh(pmesh);

    navMesh->Finalize();

    result.navMesh = navMesh;
    result.success = true;
    result.vertexCount = static_cast<uint32>(navMesh->GetVertices().size());
    result.polyCount = static_cast<uint32>(navMesh->GetPolygons().size());

    if (progressCallback) progressCallback("Complete", 1.0f);

    return result;
}
#endif

NavMeshBuildResult NavMeshBuilder::BuildSimple(const NavMeshBuildInput& input,
                                               const NavMeshBuildSettings& settings,
                                               NavMeshBuildProgress progressCallback)
{
    // Simple fallback builder - just creates polygons from input triangles
    // This is a very basic implementation for when Recast is not available
    
    (void)settings;  // Not used in simple builder

    NavMeshBuildResult result;

    if (progressCallback) progressCallback("Building simple navmesh", 0.5f);

    auto navMesh = std::make_shared<NavMesh>();

    // Add all vertices
    for (const auto& v : input.vertices)
    {
        navMesh->AddVertex(v);
    }

    // Add triangles as polygons
    size_t numTris = input.indices.size() / 3;
    for (size_t i = 0; i < numTris; ++i)
    {
        std::vector<uint32> triIndices = {
            input.indices[i * 3 + 0],
            input.indices[i * 3 + 1],
            input.indices[i * 3 + 2]
        };

        NavAreaType area = NavAreaType::Ground;
        if (i < input.areaTypes.size())
        {
            area = input.areaTypes[i];
        }

        // Check if triangle is walkable (slope check)
        const Vec3& v0 = input.vertices[triIndices[0]];
        const Vec3& v1 = input.vertices[triIndices[1]];
        const Vec3& v2 = input.vertices[triIndices[2]];

        Vec3 e1 = v1 - v0;
        Vec3 e2 = v2 - v0;
        Vec3 normal = glm::normalize(glm::cross(e1, e2));

        float slope = glm::degrees(std::acos(normal.y));
        if (slope <= settings.agentMaxSlope)
        {
            navMesh->AddPolygon(triIndices, area);
        }
    }

    navMesh->Finalize();

    result.navMesh = navMesh;
    result.success = true;
    result.vertexCount = static_cast<uint32>(navMesh->GetVertices().size());
    result.polyCount = static_cast<uint32>(navMesh->GetPolygons().size());

    if (progressCallback) progressCallback("Complete", 1.0f);

    return result;
}

} // namespace RVX::AI
