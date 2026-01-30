/**
 * @file TerrainLOD.cpp
 * @brief Implementation of terrain LOD system
 */

#include "Terrain/TerrainLOD.h"
#include "Terrain/Heightmap.h"
#include "RHI/RHIDevice.h"
#include "Core/Log.h"

#include <cmath>
#include <algorithm>

namespace RVX
{

bool TerrainLOD::Initialize(const Heightmap* heightmap, const Vec3& terrainSize,
                             const TerrainLODParams& params)
{
    if (!heightmap || !heightmap->IsValid())
    {
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
        RVX_CORE_ERROR("TerrainLOD: Invalid device");
        return false;
    }

    if (m_patchVertices.empty() || m_patchIndices.empty())
    {
        RVX_CORE_ERROR("TerrainLOD: No patch mesh data");
        return false;
    }

    // Create vertex buffer
    RHIBufferDesc vbDesc;
    vbDesc.size = m_patchVertices.size() * sizeof(Vec2);
    vbDesc.usage = RHIBufferUsage::Vertex;
    vbDesc.memoryType = RHIMemoryType::Default;
    vbDesc.debugName = "TerrainPatchVB";

    m_patchVertexBuffer = device->CreateBuffer(vbDesc);
    if (!m_patchVertexBuffer)
    {
        RVX_CORE_ERROR("TerrainLOD: Failed to create vertex buffer");
        return false;
    }

    // Create index buffer
    RHIBufferDesc ibDesc;
    ibDesc.size = m_patchIndices.size() * sizeof(uint32);
    ibDesc.usage = RHIBufferUsage::Index;
    ibDesc.memoryType = RHIMemoryType::Default;
    ibDesc.debugName = "TerrainPatchIB";

    m_patchIndexBuffer = device->CreateBuffer(ibDesc);
    if (!m_patchIndexBuffer)
    {
        RVX_CORE_ERROR("TerrainLOD: Failed to create index buffer");
        return false;
    }

    m_patchIndexCount = static_cast<uint32>(m_patchIndices.size());

    RVX_CORE_INFO("TerrainLOD: Created GPU resources - {} vertices, {} indices",
                  m_patchVertices.size(), m_patchIndices.size());
    return true;
}

void TerrainLOD::BuildQuadTree(const Heightmap* heightmap, const Vec3& terrainSize)
{
    m_quadTree.clear();

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

            // Calculate min/max height for this region
            // (simplified - would sample heightmap in real implementation)
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
        lodNode.lodMask = 0; // TODO: Calculate neighbor LOD mask for crack prevention

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
