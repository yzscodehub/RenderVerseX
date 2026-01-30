/**
 * @file DecalRenderer.cpp
 * @brief Decal renderer implementation
 */

#include "Render/Decal/DecalRenderer.h"
#include "Render/Graph/RenderGraph.h"
#include "Core/Log.h"
#include <algorithm>

namespace RVX
{

DecalRenderer::~DecalRenderer()
{
    Shutdown();
}

void DecalRenderer::Initialize(IRHIDevice* device, const DecalRendererConfig& config)
{
    if (m_device)
    {
        RVX_CORE_WARN("DecalRenderer: Already initialized");
        return;
    }

    m_device = device;
    m_config = config;

    // Reserve space for decals
    m_decals.reserve(config.maxDecals);
    m_sortedIndices.reserve(config.maxDecals);

    // Create GPU buffers
    RHIBufferDesc bufferDesc{};
    bufferDesc.size = sizeof(Mat4) * config.maxDecals;  // Transform matrices
    bufferDesc.usage = RHIBufferUsage::ShaderResource;
    bufferDesc.memoryType = RHIMemoryType::Default;
    m_decalBuffer = device->CreateBuffer(bufferDesc);

    bufferDesc.size = 256;  // Constant buffer
    bufferDesc.usage = RHIBufferUsage::Constant;
    bufferDesc.memoryType = RHIMemoryType::Upload;
    m_constantBuffer = device->CreateBuffer(bufferDesc);

    RVX_CORE_DEBUG("DecalRenderer: Initialized with max {} decals", config.maxDecals);
}

void DecalRenderer::Shutdown()
{
    if (!m_device)
        return;

    ClearDecals();
    m_decalBuffer.Reset();
    m_constantBuffer.Reset();
    m_decalPipeline.Reset();
    m_decalNormalPipeline.Reset();
    m_decalStainPipeline.Reset();
    m_device = nullptr;

    RVX_CORE_DEBUG("DecalRenderer: Shutdown");
}

void DecalRenderer::SetConfig(const DecalRendererConfig& config)
{
    // May need to recreate buffers if max decals changed
    if (config.maxDecals != m_config.maxDecals)
    {
        RVX_CORE_WARN("DecalRenderer: Max decals changed, recreating buffers");
        // TODO: Recreate buffers
    }
    m_config = config;
}

uint32 DecalRenderer::AddDecal(const DecalData& decal)
{
    if (m_decals.size() >= m_config.maxDecals)
    {
        RVX_CORE_WARN("DecalRenderer: Max decals reached");
        return RVX_INVALID_INDEX;
    }

    uint32 index = static_cast<uint32>(m_decals.size());
    m_decals.push_back(decal);
    m_needsSort = true;
    return index;
}

void DecalRenderer::UpdateDecal(uint32 index, const DecalData& decal)
{
    if (index >= m_decals.size())
        return;

    m_decals[index] = decal;
    m_needsSort = true;
}

void DecalRenderer::RemoveDecal(uint32 index)
{
    if (index >= m_decals.size())
        return;

    // Swap with last and pop
    m_decals[index] = m_decals.back();
    m_decals.pop_back();
    m_needsSort = true;
}

void DecalRenderer::ClearDecals()
{
    m_decals.clear();
    m_sortedIndices.clear();
    m_needsSort = false;
}

void DecalRenderer::SortDecals()
{
    if (!m_config.sortDecals || !m_needsSort)
        return;

    m_sortedIndices.resize(m_decals.size());
    for (size_t i = 0; i < m_decals.size(); ++i)
        m_sortedIndices[i] = static_cast<uint32>(i);

    std::sort(m_sortedIndices.begin(), m_sortedIndices.end(),
        [this](uint32 a, uint32 b)
        {
            return m_decals[a].sortOrder < m_decals[b].sortOrder;
        });

    m_needsSort = false;
}

void DecalRenderer::UploadDecalData(RHICommandContext& ctx)
{
    (void)ctx;
    // TODO: Upload decal transforms and properties to GPU buffer
}

void DecalRenderer::RenderDecalBatch(RHICommandContext& ctx, uint32 startIndex, uint32 count)
{
    (void)ctx;
    (void)startIndex;
    (void)count;
    // TODO: Render a batch of decals with the same blend mode
}

void DecalRenderer::Render(RHICommandContext& ctx,
                           RHITexture* gBufferAlbedo,
                           RHITexture* gBufferNormal,
                           RHITexture* gBufferRoughness,
                           RHITexture* depthBuffer,
                           const Mat4& viewMatrix,
                           const Mat4& projMatrix)
{
    if (!m_enabled || m_decals.empty())
        return;

    (void)gBufferAlbedo;
    (void)gBufferNormal;
    (void)gBufferRoughness;
    (void)depthBuffer;

    m_viewMatrix = viewMatrix;
    m_projMatrix = projMatrix;
    m_invViewProj = glm::inverse(projMatrix * viewMatrix);

    SortDecals();
    UploadDecalData(ctx);

    // Group decals by blend mode and render batches
    // TODO: Actual rendering implementation
    // 1. Bind GBuffer textures
    // 2. For each decal:
    //    - Render box mesh with decal shader
    //    - Reconstruct world position from depth
    //    - Project into decal space
    //    - Sample decal textures
    //    - Blend with GBuffer
}

void DecalRenderer::AddToGraph(RenderGraph& graph,
                                RGTextureHandle gBufferAlbedo,
                                RGTextureHandle gBufferNormal,
                                RGTextureHandle gBufferRoughness,
                                RGTextureHandle depthBuffer,
                                const Mat4& viewMatrix,
                                const Mat4& projMatrix)
{
    if (!m_enabled || m_decals.empty())
        return;

    struct DecalPassData
    {
        RGTextureHandle albedo;
        RGTextureHandle normal;
        RGTextureHandle roughness;
        RGTextureHandle depth;
        
        Mat4 invViewProj;
        uint32 decalCount;
    };

    m_viewMatrix = viewMatrix;
    m_projMatrix = projMatrix;
    m_invViewProj = glm::inverse(projMatrix * viewMatrix);

    SortDecals();

    graph.AddPass<DecalPassData>(
        "DecalPass",
        RenderGraphPassType::Graphics,
        [this, gBufferAlbedo, gBufferNormal, gBufferRoughness, depthBuffer]
        (RenderGraphBuilder& builder, DecalPassData& data)
        {
            data.albedo = builder.ReadWrite(gBufferAlbedo);
            data.normal = builder.ReadWrite(gBufferNormal);
            data.roughness = builder.ReadWrite(gBufferRoughness);
            data.depth = builder.Read(depthBuffer);
            
            data.invViewProj = m_invViewProj;
            data.decalCount = static_cast<uint32>(m_decals.size());
        },
        [this](const DecalPassData& data, RHICommandContext& ctx)
        {
            (void)data;
            (void)ctx;
            // TODO: Render decals
        });
}

Mat4 DecalRenderer::CreateDecalTransform(const Vec3& position,
                                          const Quat& rotation,
                                          const Vec3& size)
{
    Mat4 transform = glm::translate(Mat4(1.0f), position);
    transform *= glm::mat4_cast(rotation);
    transform = glm::scale(transform, size);
    return transform;
}

bool DecalRenderer::IsPointInDecal(const Vec3& point, const DecalData& decal)
{
    // Transform point to decal local space
    Mat4 invTransform = glm::inverse(decal.transform);
    Vec4 localPoint = invTransform * Vec4(point, 1.0f);
    
    // Check if within unit box
    return std::abs(localPoint.x) <= 0.5f &&
           std::abs(localPoint.y) <= 0.5f &&
           std::abs(localPoint.z) <= 0.5f;
}

} // namespace RVX
