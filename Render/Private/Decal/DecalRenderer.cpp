/**
 * @file DecalRenderer.cpp
 * @brief Decal renderer implementation
 */

#include "Render/Decal/DecalRenderer.h"
#include "Render/Graph/RenderGraph.h"
#include "Core/Log.h"

#include <algorithm>
#include <cmath>

namespace RVX
{

namespace
{
    constexpr const char* RVX_DECAL_UNSUPPORTED_REASON =
        "Decal deferred projection pipeline is not implemented";

    uint32 GetEffectiveMaxDecals(const DecalRendererConfig& config)
    {
        return std::max<uint32>(1u, config.maxDecals);
    }
} // namespace

const char* GetDecalRendererImplementationTierName(DecalRendererImplementationTier tier)
{
    switch (tier)
    {
        case DecalRendererImplementationTier::Unsupported: return "Unsupported";
        case DecalRendererImplementationTier::DeferredProjection: return "DeferredProjection";
    }
    return "Unknown";
}

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

    if (!device)
    {
        m_supported = false;
        m_unsupportedReason = "No RHI device";
        RecordUnsupportedDiagnostics(false, false, false, false, false, m_unsupportedReason);
        RVX_CORE_WARN("DecalRenderer: Cannot initialize without an RHI device");
        return;
    }

    m_device = device;
    m_config = config;
    m_supported = false;
    m_unsupportedReason = RVX_DECAL_UNSUPPORTED_REASON;

    // Reserve space for decals
    m_decals.reserve(config.maxDecals);
    m_sortedIndices.reserve(config.maxDecals);

    // Create GPU buffers for the future projection path and residency diagnostics.
    RHIBufferDesc bufferDesc{};
    bufferDesc.size = sizeof(Mat4) * GetEffectiveMaxDecals(config);
    bufferDesc.usage = RHIBufferUsage::ShaderResource;
    bufferDesc.memoryType = RHIMemoryType::Default;
    m_decalBuffer = device->CreateBuffer(bufferDesc);

    bufferDesc.size = 256;
    bufferDesc.usage = RHIBufferUsage::Constant;
    bufferDesc.memoryType = RHIMemoryType::Upload;
    m_constantBuffer = device->CreateBuffer(bufferDesc);

    RecordUnsupportedDiagnostics(false, false, false, false, false, RVX_DECAL_UNSUPPORTED_REASON);
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
    m_supported = false;
    m_lastDiagnostics = {};

    RVX_CORE_DEBUG("DecalRenderer: Shutdown");
}

void DecalRenderer::SetConfig(const DecalRendererConfig& config)
{
    const bool maxDecalsChanged = config.maxDecals != m_config.maxDecals;
    m_config = config;

    if (m_decals.size() > m_config.maxDecals)
    {
        m_decals.resize(m_config.maxDecals);
        m_needsSort = true;
    }

    if (maxDecalsChanged && m_device)
    {
        RVX_CORE_DEBUG("DecalRenderer: Max decals changed, recreating buffers");

        RHIBufferDesc bufferDesc{};
        bufferDesc.size = sizeof(Mat4) * GetEffectiveMaxDecals(m_config);
        bufferDesc.usage = RHIBufferUsage::ShaderResource;
        bufferDesc.memoryType = RHIMemoryType::Default;
        m_decalBuffer = m_device->CreateBuffer(bufferDesc);

        bufferDesc.size = 256;
        bufferDesc.usage = RHIBufferUsage::Constant;
        bufferDesc.memoryType = RHIMemoryType::Upload;
        m_constantBuffer = m_device->CreateBuffer(bufferDesc);
    }

    m_supported = false;
    m_unsupportedReason = RVX_DECAL_UNSUPPORTED_REASON;
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

void DecalRenderer::RecordUnsupportedDiagnostics(bool requested,
                                                 bool albedoAvailable,
                                                 bool normalAvailable,
                                                 bool roughnessAvailable,
                                                 bool depthAvailable,
                                                 const std::string& reason)
{
    m_lastDiagnostics.requested = requested;
    m_lastDiagnostics.supported = false;
    m_lastDiagnostics.scheduled = false;
    m_lastDiagnostics.executed = false;
    m_lastDiagnostics.initialized = IsInitialized();
    m_lastDiagnostics.enabled = m_enabled;
    m_lastDiagnostics.gBufferAlbedoAvailable = albedoAvailable;
    m_lastDiagnostics.gBufferNormalAvailable = normalAvailable;
    m_lastDiagnostics.gBufferRoughnessAvailable = roughnessAvailable;
    m_lastDiagnostics.depthAvailable = depthAvailable;
    m_lastDiagnostics.decalCount = static_cast<uint32>(m_decals.size());
    m_lastDiagnostics.implementationTier = DecalRendererImplementationTier::Unsupported;
    m_lastDiagnostics.reason = reason.empty() ? m_unsupportedReason : reason;
}

void DecalRenderer::UploadDecalData(RHICommandContext& ctx)
{
    (void)ctx;
    // Reserved for the future projection path. Public render entry points remain capability-gated.
}

void DecalRenderer::RenderDecalBatch(RHICommandContext& ctx, uint32 startIndex, uint32 count)
{
    (void)ctx;
    (void)startIndex;
    (void)count;
    // Reserved for the future projection path. Public render entry points remain capability-gated.
}

void DecalRenderer::Render(RHICommandContext& ctx,
                           RHITexture* gBufferAlbedo,
                           RHITexture* gBufferNormal,
                           RHITexture* gBufferRoughness,
                           RHITexture* depthBuffer,
                           const Mat4& viewMatrix,
                           const Mat4& projMatrix)
{
    (void)ctx;

    const bool requested = m_enabled && !m_decals.empty();
    const bool albedoAvailable = gBufferAlbedo != nullptr;
    const bool normalAvailable = gBufferNormal != nullptr;
    const bool roughnessAvailable = gBufferRoughness != nullptr;
    const bool depthAvailable = depthBuffer != nullptr;

    m_viewMatrix = viewMatrix;
    m_projMatrix = projMatrix;
    m_invViewProj = glm::inverse(projMatrix * viewMatrix);

    if (!m_enabled)
    {
        RecordUnsupportedDiagnostics(false,
                                     albedoAvailable,
                                     normalAvailable,
                                     roughnessAvailable,
                                     depthAvailable,
                                     "Decal rendering disabled by configuration");
        return;
    }

    if (m_decals.empty())
    {
        RecordUnsupportedDiagnostics(false,
                                     albedoAvailable,
                                     normalAvailable,
                                     roughnessAvailable,
                                     depthAvailable,
                                     "No decals queued");
        return;
    }

    if (!albedoAvailable || !normalAvailable || !roughnessAvailable || !depthAvailable)
    {
        RecordUnsupportedDiagnostics(requested,
                                     albedoAvailable,
                                     normalAvailable,
                                     roughnessAvailable,
                                     depthAvailable,
                                     "Decal rendering skipped because a GBuffer or depth input is unavailable");
        RVX_CORE_WARN("DecalRenderer: {}", m_lastDiagnostics.reason);
        return;
    }

    SortDecals();
    RecordUnsupportedDiagnostics(requested,
                                 albedoAvailable,
                                 normalAvailable,
                                 roughnessAvailable,
                                 depthAvailable,
                                 RVX_DECAL_UNSUPPORTED_REASON);
    RVX_CORE_WARN("DecalRenderer: render requested but {}", m_lastDiagnostics.reason);
}

void DecalRenderer::AddToGraph(RenderGraph& graph,
                                RGTextureHandle gBufferAlbedo,
                                RGTextureHandle gBufferNormal,
                                RGTextureHandle gBufferRoughness,
                                RGTextureHandle depthBuffer,
                                const Mat4& viewMatrix,
                                const Mat4& projMatrix)
{
    (void)graph;

    const bool requested = m_enabled && !m_decals.empty();
    const bool albedoAvailable = gBufferAlbedo.IsValid();
    const bool normalAvailable = gBufferNormal.IsValid();
    const bool roughnessAvailable = gBufferRoughness.IsValid();
    const bool depthAvailable = depthBuffer.IsValid();

    m_viewMatrix = viewMatrix;
    m_projMatrix = projMatrix;
    m_invViewProj = glm::inverse(projMatrix * viewMatrix);

    if (!m_enabled)
    {
        RecordUnsupportedDiagnostics(false,
                                     albedoAvailable,
                                     normalAvailable,
                                     roughnessAvailable,
                                     depthAvailable,
                                     "Decal rendering disabled by configuration");
        return;
    }

    if (m_decals.empty())
    {
        RecordUnsupportedDiagnostics(false,
                                     albedoAvailable,
                                     normalAvailable,
                                     roughnessAvailable,
                                     depthAvailable,
                                     "No decals queued");
        return;
    }

    if (!albedoAvailable || !normalAvailable || !roughnessAvailable || !depthAvailable)
    {
        RecordUnsupportedDiagnostics(requested,
                                     albedoAvailable,
                                     normalAvailable,
                                     roughnessAvailable,
                                     depthAvailable,
                                     "Decal rendering skipped because a GBuffer or depth input is unavailable");
        RVX_CORE_WARN("DecalRenderer: {}", m_lastDiagnostics.reason);
        return;
    }

    SortDecals();
    RecordUnsupportedDiagnostics(requested,
                                 albedoAvailable,
                                 normalAvailable,
                                 roughnessAvailable,
                                 depthAvailable,
                                 RVX_DECAL_UNSUPPORTED_REASON);
    RVX_CORE_WARN("DecalRenderer: graph pass requested but {}", m_lastDiagnostics.reason);
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
