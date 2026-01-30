/**
 * @file GPUCulling.cpp
 * @brief GPU-driven culling implementation
 */

#include "Render/GPUDriven/GPUCulling.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace RVX
{

// ============================================================================
// GPUCulling
// ============================================================================

GPUCulling::~GPUCulling()
{
    Shutdown();
}

void GPUCulling::Initialize(IRHIDevice* device, const GPUCullingConfig& config)
{
    m_device = device;
    m_config = config;

    m_instances.reserve(config.maxInstances);
    CreateResources();
}

void GPUCulling::Shutdown()
{
    m_instanceBuffer.Reset();
    m_visibleInstanceBuffer.Reset();
    m_indirectBuffer.Reset();
    m_drawCountBuffer.Reset();
    m_cullingConstantsBuffer.Reset();
    m_frustumCullPipeline.Reset();
    m_occlusionCullPipeline.Reset();
    m_compactPipeline.Reset();
    m_statsBuffer.Reset();
    m_device = nullptr;
}

void GPUCulling::SetConfig(const GPUCullingConfig& config)
{
    bool needsResize = config.maxInstances != m_config.maxInstances;
    m_config = config;
    
    if (needsResize)
    {
        CreateResources();
    }
}

void GPUCulling::CreateResources()
{
    if (!m_device) return;

    RHIBufferDesc desc;

    // Instance buffer
    desc.size = m_config.maxInstances * sizeof(GPUInstanceData);
    desc.usage = RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource;
    desc.memoryType = RHIMemoryType::Upload;
    m_instanceBuffer = m_device->CreateBuffer(desc);

    // Visible instance buffer (output)
    desc.memoryType = RHIMemoryType::Default;
    m_visibleInstanceBuffer = m_device->CreateBuffer(desc);

    // Indirect draw buffer
    desc.size = m_config.maxInstances * sizeof(IndirectDrawIndexedCommand);
    desc.usage = RHIBufferUsage::IndirectArgs | RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource;
    m_indirectBuffer = m_device->CreateBuffer(desc);

    // Draw count buffer
    desc.size = sizeof(uint32) * 4;  // count + padding
    desc.memoryType = RHIMemoryType::Upload;
    m_drawCountBuffer = m_device->CreateBuffer(desc);

    // Culling constants
    desc.size = 256;
    desc.usage = RHIBufferUsage::Constant;
    desc.memoryType = RHIMemoryType::Upload;
    m_cullingConstantsBuffer = m_device->CreateBuffer(desc);

    // Statistics buffer (optional)
    if (m_statsEnabled)
    {
        desc.size = sizeof(uint32) * 8;
        desc.usage = RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource;
        desc.memoryType = RHIMemoryType::Readback;
        m_statsBuffer = m_device->CreateBuffer(desc);
    }
}

void GPUCulling::BeginFrame()
{
    m_instances.clear();
    m_instanceCount = 0;
    m_stats = {};
}

uint32 GPUCulling::AddInstance(const GPUInstanceData& instance)
{
    if (m_instanceCount >= m_config.maxInstances)
    {
        return RVX_INVALID_INDEX;
    }

    uint32 index = m_instanceCount++;
    m_instances.push_back(instance);
    return index;
}

void GPUCulling::AddInstances(const GPUInstanceData* instances, uint32 count)
{
    uint32 availableSlots = m_config.maxInstances - m_instanceCount;
    count = std::min(count, availableSlots);

    m_instances.insert(m_instances.end(), instances, instances + count);
    m_instanceCount += count;
}

void GPUCulling::EndFrame()
{
    UploadInstances();
}

void GPUCulling::UploadInstances()
{
    if (m_instances.empty() || !m_instanceBuffer) return;

    // Map and copy instance data
    void* mapped = m_instanceBuffer->Map();
    if (mapped)
    {
        std::memcpy(mapped, m_instances.data(), 
                    m_instances.size() * sizeof(GPUInstanceData));
        m_instanceBuffer->Unmap();
    }

    m_stats.totalInstances = m_instanceCount;
}

void GPUCulling::ExtractFrustumPlanes(const Mat4& viewProj, Vec4* planes)
{
    // Extract 6 frustum planes from view-projection matrix
    // Left, Right, Bottom, Top, Near, Far
    
    // Left plane
    planes[0] = Vec4(
        viewProj[0][3] + viewProj[0][0],
        viewProj[1][3] + viewProj[1][0],
        viewProj[2][3] + viewProj[2][0],
        viewProj[3][3] + viewProj[3][0]
    );
    
    // Right plane
    planes[1] = Vec4(
        viewProj[0][3] - viewProj[0][0],
        viewProj[1][3] - viewProj[1][0],
        viewProj[2][3] - viewProj[2][0],
        viewProj[3][3] - viewProj[3][0]
    );
    
    // Bottom plane
    planes[2] = Vec4(
        viewProj[0][3] + viewProj[0][1],
        viewProj[1][3] + viewProj[1][1],
        viewProj[2][3] + viewProj[2][1],
        viewProj[3][3] + viewProj[3][1]
    );
    
    // Top plane
    planes[3] = Vec4(
        viewProj[0][3] - viewProj[0][1],
        viewProj[1][3] - viewProj[1][1],
        viewProj[2][3] - viewProj[2][1],
        viewProj[3][3] - viewProj[3][1]
    );
    
    // Near plane
    planes[4] = Vec4(
        viewProj[0][2],
        viewProj[1][2],
        viewProj[2][2],
        viewProj[3][2]
    );
    
    // Far plane
    planes[5] = Vec4(
        viewProj[0][3] - viewProj[0][2],
        viewProj[1][3] - viewProj[1][2],
        viewProj[2][3] - viewProj[2][2],
        viewProj[3][3] - viewProj[3][2]
    );

    // Normalize planes
    for (int i = 0; i < 6; ++i)
    {
        float len = std::sqrt(planes[i].x * planes[i].x + 
                              planes[i].y * planes[i].y + 
                              planes[i].z * planes[i].z);
        if (len > 0.0001f)
        {
            planes[i] /= len;
        }
    }
}

void GPUCulling::Cull(RHICommandContext& ctx,
                      const Mat4& viewMatrix,
                      const Mat4& projMatrix,
                      RHITexture* hiZTexture)
{
    if (m_instanceCount == 0) return;

    Mat4 viewProj = projMatrix * viewMatrix;

    // Update culling constants
    struct CullingConstants
    {
        Mat4 viewProj;
        Vec4 frustumPlanes[6];
        Vec4 cameraPosition;
        Vec4 params;  // maxDistance, instanceCount, etc.
    } constants;

    constants.viewProj = viewProj;
    ExtractFrustumPlanes(viewProj, constants.frustumPlanes);
    constants.cameraPosition = Vec4(inverse(viewMatrix)[3]);
    constants.params = Vec4(
        m_config.maxDrawDistance,
        static_cast<float>(m_instanceCount),
        m_config.enableFrustumCulling ? 1.0f : 0.0f,
        m_config.enableOcclusionCulling ? 1.0f : 0.0f
    );

    m_cullingConstantsBuffer->Upload(&constants, 1);

    // Clear draw count
    uint32 zero = 0;
    m_drawCountBuffer->Upload(&zero, 1);

    // Dispatch frustum culling compute shader
    if (m_frustumCullPipeline)
    {
        ctx.SetPipeline(m_frustumCullPipeline.Get());
        // Bind buffers...
        uint32 groupCount = (m_instanceCount + 63) / 64;
        ctx.Dispatch(groupCount, 1, 1);
    }

    // Dispatch occlusion culling if enabled and HiZ available
    if (m_config.enableOcclusionCulling && hiZTexture && m_occlusionCullPipeline)
    {
        ctx.BufferBarrier(m_visibleInstanceBuffer.Get(), 
                          RHIResourceState::UnorderedAccess, RHIResourceState::UnorderedAccess);
        
        ctx.SetPipeline(m_occlusionCullPipeline.Get());
        // Bind HiZ texture and buffers...
        uint32 groupCount = (m_instanceCount + 63) / 64;
        ctx.Dispatch(groupCount, 1, 1);
    }

    // Compact visible instances into draw commands
    if (m_compactPipeline)
    {
        ctx.BufferBarrier(m_indirectBuffer.Get(),
                          RHIResourceState::UnorderedAccess, RHIResourceState::IndirectArgument);
    }
}

// ============================================================================
// MeshletRenderer
// ============================================================================

MeshletRenderer::~MeshletRenderer()
{
    Shutdown();
}

void MeshletRenderer::Initialize(IRHIDevice* device)
{
    m_device = device;
}

void MeshletRenderer::Shutdown()
{
    m_meshletBuffer.Reset();
    m_vertexBuffer.Reset();
    m_indexBuffer.Reset();
    m_visibleMeshletBuffer.Reset();
    m_meshletCullPipeline.Reset();
    m_meshletDrawPipeline.Reset();
    m_device = nullptr;
}

void MeshletRenderer::GenerateMeshlets(
    const Vec3* vertices,
    uint32 vertexCount,
    const uint32* indices,
    uint32 indexCount,
    uint32 maxVertices,
    uint32 maxTriangles,
    std::vector<Meshlet>& outMeshlets)
{
    // Simple meshlet generation algorithm
    // For production, use meshoptimizer or similar

    outMeshlets.clear();

    uint32 triangleCount = indexCount / 3;
    uint32 currentTriangle = 0;

    while (currentTriangle < triangleCount)
    {
        Meshlet meshlet = {};
        meshlet.vertexOffset = 0;  // Would be calculated based on vertex deduplication
        meshlet.triangleOffset = currentTriangle * 3;
        meshlet.vertexCount = 0;
        meshlet.triangleCount = 0;

        // Calculate bounding sphere
        Vec3 center(0.0f);
        float radius = 0.0f;

        // Add triangles to meshlet
        uint32 trianglesToAdd = std::min(maxTriangles, triangleCount - currentTriangle);
        meshlet.triangleCount = trianglesToAdd;

        // Calculate bounding sphere from vertices in this meshlet
        for (uint32 t = 0; t < trianglesToAdd; ++t)
        {
            for (int v = 0; v < 3; ++v)
            {
                uint32 idx = indices[(currentTriangle + t) * 3 + v];
                if (idx < vertexCount)
                {
                    center += vertices[idx];
                    meshlet.vertexCount++;
                }
            }
        }

        if (meshlet.vertexCount > 0)
        {
            center /= static_cast<float>(meshlet.vertexCount);
        }

        // Calculate radius
        for (uint32 t = 0; t < trianglesToAdd; ++t)
        {
            for (int v = 0; v < 3; ++v)
            {
                uint32 idx = indices[(currentTriangle + t) * 3 + v];
                if (idx < vertexCount)
                {
                    float dist = length(vertices[idx] - center);
                    radius = std::max(radius, dist);
                }
            }
        }

        meshlet.boundingSphere = Vec4(center, radius);

        // TODO: Calculate cone for backface culling

        outMeshlets.push_back(meshlet);
        currentTriangle += trianglesToAdd;
    }
}

void MeshletRenderer::Render(RHICommandContext& ctx,
                              const Mat4& viewMatrix,
                              const Mat4& projMatrix)
{
    // TODO: Implement meshlet rendering
    // 1. Cull meshlets using compute shader
    // 2. Generate indirect draw commands
    // 3. Execute mesh shader or indirect draws
}

} // namespace RVX
