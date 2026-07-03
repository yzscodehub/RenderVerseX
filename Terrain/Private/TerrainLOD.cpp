/**
 * @file TerrainLOD.cpp
 * @brief Implementation of terrain LOD system
 */

#include "Terrain/TerrainLOD.h"
#include "Core/Log.h"
#include "RHI/RHIDevice.h"
#include "Terrain/Heightmap.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace RVX
{
namespace
{
    bool UploadMappedTerrainBuffer(RHIBuffer* buffer,
                                   const void* data,
                                   uint64 size,
                                   const char* label,
                                   std::string& outDiagnostic)
    {
        if (!buffer)
        {
            outDiagnostic = std::string("Terrain LOD patch ") + label + " buffer is missing.";
            return false;
        }

        if (!data || size == 0)
        {
            outDiagnostic = std::string("Terrain LOD patch ") + label + " upload has no data.";
            return false;
        }

        void* mapped = buffer->Map();
        if (!mapped)
        {
            outDiagnostic = std::string("Terrain LOD failed to map patch ") + label + " buffer.";
            return false;
        }

        std::memcpy(mapped, data, static_cast<size_t>(size));
        buffer->Unmap();
        return true;
    }
} // namespace

bool TerrainLOD::Initialize(const Heightmap* heightmap, const Vec3& terrainSize,
                             const TerrainLODParams& params)
{
    if (!heightmap || !heightmap->IsValid())
    {
        m_usesConservativeHeightBounds = false;
        m_heightBoundsDiagnostic = "Terrain LOD initialization failed: invalid heightmap.";
        RVX_CORE_ERROR("TerrainLOD: Invalid heightmap");
        return false;
    }

    m_params = params;
    m_terrainSize = terrainSize;

    BuildQuadTree(heightmap, terrainSize);
    CreatePatchMesh(params.patchSize);

    RVX_CORE_INFO("TerrainLOD: Initialized with {} quadtree nodes, {} LOD levels",
                  m_quadTree.size(), params.maxLODLevels);
    return true;
}

void TerrainLOD::SetParams(const TerrainLODParams& params)
{
    m_params = params;
}

void TerrainLOD::SelectLOD(const Vec3& cameraPosition, const Vec4* frustumPlanes,
                            TerrainLODSelection& outSelection)
{
    outSelection.nodes.clear();
    outSelection.totalPatches = 0;
    outSelection.totalTriangles = 0;

    m_stats = Statistics{};

    if (m_quadTree.empty())
        return;

    SelectLODRecursive(0, cameraPosition, frustumPlanes, outSelection);

    outSelection.totalPatches = static_cast<uint32>(outSelection.nodes.size());
    outSelection.totalTriangles = outSelection.totalPatches * (m_params.patchSize - 1) * 
                                   (m_params.patchSize - 1) * 2;

    m_stats.patchesRendered = outSelection.totalPatches;
    m_stats.trianglesRendered = outSelection.totalTriangles;
}

uint8 TerrainLOD::GetLODLevel(float distance) const
{
    if (distance <= 0) return 0;

    float adjustedDistance = distance * std::exp2(m_params.lodBias);
    uint8 level = static_cast<uint8>(std::log2(adjustedDistance / m_params.lodDistance));

    return std::min(level, static_cast<uint8>(m_params.maxLODLevels - 1));
}

float TerrainLOD::GetMorphFactor(float distance, uint8 lodLevel) const
{
    float lodStart = m_params.lodDistance * std::exp2(static_cast<float>(lodLevel));
    float lodEnd = lodStart * 2.0f;
    float morphStart = lodEnd - (lodEnd - lodStart) * m_params.morphRange;

    if (distance < morphStart) return 0.0f;
    if (distance >= lodEnd) return 1.0f;

    return (distance - morphStart) / (lodEnd - morphStart);
}

bool TerrainLOD::CreateGPUResources(IRHIDevice* device)
{
    if (!device)
    {
        m_patchMeshDataUploaded = false;
        m_patchMeshDiagnostic = "Terrain LOD patch mesh upload failed: invalid device.";
        RVX_CORE_ERROR("TerrainLOD: Invalid device");
        return false;
    }

    if (m_patchVertices.empty() || m_patchIndices.empty())
    {
        m_patchMeshDataUploaded = false;
        m_patchMeshDiagnostic = "Terrain LOD patch mesh upload failed: no patch mesh data.";
        RVX_CORE_ERROR("TerrainLOD: No patch mesh data");
        return false;
    }

    RHIBufferDesc vbDesc;
    vbDesc.size = static_cast<uint64>(m_patchVertices.size() * sizeof(Vec2));
    vbDesc.usage = RHIBufferUsage::Vertex;
    vbDesc.memoryType = RHIMemoryType::Upload;
    vbDesc.stride = sizeof(Vec2);
    vbDesc.debugName = "TerrainPatchVB";

    m_patchVertexBuffer = device->CreateBuffer(vbDesc);
    if (!m_patchVertexBuffer)
    {
        m_patchVertexBuffer.Reset();
        m_patchIndexBuffer.Reset();
        m_patchMeshDataUploaded = false;
        m_patchMeshDiagnostic = "Terrain LOD failed to create patch vertex buffer.";
        RVX_CORE_ERROR("TerrainLOD: {}", m_patchMeshDiagnostic);
        return false;
    }

    if (!UploadMappedTerrainBuffer(m_patchVertexBuffer.Get(),
                                   m_patchVertices.data(),
                                   vbDesc.size,
                                   "vertex",
                                   m_patchMeshDiagnostic))
    {
        m_patchVertexBuffer.Reset();
        m_patchIndexBuffer.Reset();
        m_patchMeshDataUploaded = false;
        RVX_CORE_ERROR("TerrainLOD: {}", m_patchMeshDiagnostic);
        return false;
    }

    RHIBufferDesc ibDesc;
    ibDesc.size = static_cast<uint64>(m_patchIndices.size() * sizeof(uint32));
    ibDesc.usage = RHIBufferUsage::Index;
    ibDesc.memoryType = RHIMemoryType::Upload;
    ibDesc.stride = sizeof(uint32);
    ibDesc.debugName = "TerrainPatchIB";

    m_patchIndexBuffer = device->CreateBuffer(ibDesc);
    if (!m_patchIndexBuffer)
    {
        m_patchVertexBuffer.Reset();
        m_patchIndexBuffer.Reset();
        m_patchMeshDataUploaded = false;
        m_patchMeshDiagnostic = "Terrain LOD failed to create patch index buffer.";
        RVX_CORE_ERROR("TerrainLOD: {}", m_patchMeshDiagnostic);
        return false;
    }

    if (!UploadMappedTerrainBuffer(m_patchIndexBuffer.Get(),
                                   m_patchIndices.data(),
                                   ibDesc.size,
                                   "index",
                                   m_patchMeshDiagnostic))
    {
        m_patchVertexBuffer.Reset();
        m_patchIndexBuffer.Reset();
        m_patchMeshDataUploaded = false;
        RVX_CORE_ERROR("TerrainLOD: {}", m_patchMeshDiagnostic);
        return false;
    }

    m_patchIndexCount = static_cast<uint32>(m_patchIndices.size());
    m_patchMeshDataUploaded = true;
    m_patchMeshDiagnostic =
        "Terrain LOD patch mesh uploaded with RHI upload-memory buffers; "
        "GPU-only staged patch buffers are not used by this Terrain-local path.";

    RVX_CORE_INFO("TerrainLOD: Created GPU resources - {} vertices, {} indices",
                  m_patchVertices.size(), m_patchIndices.size());
    return true;
}

void TerrainLOD::BuildQuadTree(const Heightmap* heightmap, const Vec3& terrainSize)
{
    (void)heightmap;

    m_quadTree.clear();
    m_usesConservativeHeightBounds = true;
    m_heightBoundsDiagnostic =
        "Terrain LOD quadtree uses full terrain vertical bounds as a conservative fallback; "
        "per-node sampled height ranges are not generated yet.";

    // Calculate number of levels based on terrain size and patch size
    float minNodeSize = terrainSize.x / std::exp2(static_cast<float>(m_params.maxLODLevels - 1));
    
    // Create root node
    QuadTreeNode root;
    root.min = Vec2(-terrainSize.x * 0.5f, -terrainSize.z * 0.5f);
    root.max = Vec2(terrainSize.x * 0.5f, terrainSize.z * 0.5f);
    root.minHeight = 0.0f;
    root.maxHeight = terrainSize.y;
    root.level = 0;
    root.children[0] = root.children[1] = root.children[2] = root.children[3] = 0;

    m_quadTree.push_back(root);

    // Build tree recursively (breadth-first for better cache locality)
    std::vector<uint32> nodesToProcess;
    nodesToProcess.push_back(0);

    while (!nodesToProcess.empty())
    {
        uint32 nodeIndex = nodesToProcess.back();
        nodesToProcess.pop_back();

        QuadTreeNode& node = m_quadTree[nodeIndex];

        // Check if we should subdivide
        float nodeSize = node.max.x - node.min.x;
        if (nodeSize <= minNodeSize || node.level >= m_params.maxLODLevels - 1)
            continue;

        // Create four children
        Vec2 center = (node.min + node.max) * 0.5f;

        for (int i = 0; i < 4; ++i)
        {
            QuadTreeNode child;
            child.level = node.level + 1;

            // Determine child bounds
            switch (i)
            {
            case 0: // Bottom-left
                child.min = node.min;
                child.max = center;
                break;
            case 1: // Bottom-right
                child.min = Vec2(center.x, node.min.y);
                child.max = Vec2(node.max.x, center.y);
                break;
            case 2: // Top-left
                child.min = Vec2(node.min.x, center.y);
                child.max = Vec2(center.x, node.max.y);
                break;
            case 3: // Top-right
                child.min = center;
                child.max = node.max;
                break;
            }

            // Conservative fallback: keep parent vertical bounds until per-node
            // height sampling is implemented, so culling cannot hide real peaks.
            child.minHeight = node.minHeight;
            child.maxHeight = node.maxHeight;
            child.children[0] = child.children[1] = child.children[2] = child.children[3] = 0;

            uint32 childIndex = static_cast<uint32>(m_quadTree.size());
            node.children[i] = childIndex;
            m_quadTree.push_back(child);
            nodesToProcess.push_back(childIndex);
        }
    }
}

void TerrainLOD::SelectLODRecursive(uint32 nodeIndex, const Vec3& cameraPos,
                                     const Vec4* frustumPlanes, TerrainLODSelection& selection)
{
    if (nodeIndex >= m_quadTree.size())
        return;

    const QuadTreeNode& node = m_quadTree[nodeIndex];
    m_stats.nodesTraversed++;

    // Frustum culling
    if (frustumPlanes && !IsNodeInFrustum(node, frustumPlanes))
    {
        m_stats.nodesCulled++;
        return;
    }

    // Calculate distance to node center
    Vec2 nodeCenter = (node.min + node.max) * 0.5f;
    float nodeCenterHeight = (node.minHeight + node.maxHeight) * 0.5f;
    Vec3 nodeCenterWorld(nodeCenter.x, nodeCenterHeight, nodeCenter.y);
    float distance = length(cameraPos - nodeCenterWorld);

    // Get LOD level for this distance
    uint8 desiredLOD = GetLODLevel(distance);

    // Check if this node should be rendered or subdivided
    bool hasChildren = node.children[0] != 0;
    bool shouldSubdivide = hasChildren && desiredLOD < node.level;

    if (shouldSubdivide)
    {
        // Recurse into children
        for (int i = 0; i < 4; ++i)
        {
            if (node.children[i] != 0)
            {
                SelectLODRecursive(node.children[i], cameraPos, frustumPlanes, selection);
            }
        }
    }
    else
    {
        // Render this node as a patch
        TerrainLODNode lodNode;
        lodNode.position = nodeCenter;
        lodNode.size = node.max.x - node.min.x;
        lodNode.level = node.level;
        lodNode.morphFactor = static_cast<uint8>(GetMorphFactor(distance, node.level) * 255.0f);
        lodNode.isLeaf = true;
        lodNode.lodMask = RVX_TERRAIN_LOD_MASK_UNGENERATED;

        selection.nodes.push_back(lodNode);
    }
}

bool TerrainLOD::IsNodeInFrustum(const QuadTreeNode& node, const Vec4* frustumPlanes) const
{
    if (!frustumPlanes)
        return true;

    // Create AABB for the node
    Vec3 nodeMin(node.min.x, node.minHeight, node.min.y);
    Vec3 nodeMax(node.max.x, node.maxHeight, node.max.y);

    // Test against each frustum plane
    for (int i = 0; i < 6; ++i)
    {
        Vec3 planeNormal(frustumPlanes[i].x, frustumPlanes[i].y, frustumPlanes[i].z);
        float planeD = frustumPlanes[i].w;

        // Find the positive vertex (furthest along plane normal)
        Vec3 pVertex;
        pVertex.x = (planeNormal.x >= 0) ? nodeMax.x : nodeMin.x;
        pVertex.y = (planeNormal.y >= 0) ? nodeMax.y : nodeMin.y;
        pVertex.z = (planeNormal.z >= 0) ? nodeMax.z : nodeMin.z;

        if (dot(planeNormal, pVertex) + planeD < 0)
            return false;
    }

    return true;
}

void TerrainLOD::CreatePatchMesh(uint32 patchSize)
{
    m_patchVertices.clear();
    m_patchIndices.clear();
    m_patchVertexBuffer.Reset();
    m_patchIndexBuffer.Reset();
    m_patchIndexCount = 0;
    m_patchMeshDataUploaded = false;
    m_patchMeshDiagnostic = "Terrain patch mesh CPU data has not been uploaded to GPU buffers.";

    // Create grid of vertices
    for (uint32 y = 0; y < patchSize; ++y)
    {
        for (uint32 x = 0; x < patchSize; ++x)
        {
            float u = static_cast<float>(x) / (patchSize - 1);
            float v = static_cast<float>(y) / (patchSize - 1);
            m_patchVertices.push_back(Vec2(u, v));
        }
    }

    // Create triangle indices
    for (uint32 y = 0; y < patchSize - 1; ++y)
    {
        for (uint32 x = 0; x < patchSize - 1; ++x)
        {
            uint32 i00 = y * patchSize + x;
            uint32 i10 = i00 + 1;
            uint32 i01 = i00 + patchSize;
            uint32 i11 = i01 + 1;

            // First triangle
            m_patchIndices.push_back(i00);
            m_patchIndices.push_back(i01);
            m_patchIndices.push_back(i10);

            // Second triangle
            m_patchIndices.push_back(i10);
            m_patchIndices.push_back(i01);
            m_patchIndices.push_back(i11);
        }
    }

    m_patchIndexCount = static_cast<uint32>(m_patchIndices.size());
}

} // namespace RVX
