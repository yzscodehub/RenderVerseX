/**
 * @file SSR.cpp
 * @brief SSR diagnostics implementation
 */

#include "Render/PostProcess/SSR.h"
#include "Core/Log.h"

#include <algorithm>
#include <cmath>

namespace RVX
{

namespace
{
    constexpr const char* RVX_SSR_UNSUPPORTED_REASON =
        "SSR HiZ, ray march, resolve, and temporal pipelines are not implemented";
} // namespace

const char* GetSSRImplementationTierName(SSRImplementationTier tier)
{
    switch (tier)
    {
        case SSRImplementationTier::Unsupported: return "Unsupported";
        case SSRImplementationTier::HiZRayMarch: return "HiZRayMarch";
    }
    return "Unknown";
}

SSR::~SSR()
{
    Shutdown();
}

void SSR::Initialize(IRHIDevice* device, uint32 width, uint32 height)
{
    if (!device)
    {
        RVX_CORE_ERROR("SSR: Cannot initialize without an RHI device");
        m_supported = false;
        m_unsupportedReason = "No RHI device";
        return;
    }

    m_device = device;
    m_supported = false;
    m_unsupportedReason = RVX_SSR_UNSUPPORTED_REASON;
    m_width = width;
    m_height = height;

    CreateResources(width, height);
}

void SSR::Shutdown()
{
    m_reflectionResult.Reset();
    m_hitMask.Reset();
    m_rayHitUV.Reset();
    m_history.Reset();
    m_hiZPyramid.Reset();
    m_hiZMips.clear();
    m_hiZPipeline.Reset();
    m_rayMarchPipeline.Reset();
    m_resolvePipeline.Reset();
    m_temporalPipeline.Reset();
    m_constantBuffer.Reset();
    m_device = nullptr;
    m_supported = false;
    m_lastComputeStats = {};
}

void SSR::Resize(uint32 width, uint32 height)
{
    m_width = width;
    m_height = height;
    CreateResources(width, height);
}

void SSR::SetConfig(const SSRConfig& config)
{
    m_config = config;
}

void SSR::CreateResources(uint32 width, uint32 height)
{
    if (!m_device) return;

    uint32 ssrWidth = m_config.halfResolution ? std::max<uint32>(1u, width / 2u) : width;
    uint32 ssrHeight = m_config.halfResolution ? std::max<uint32>(1u, height / 2u) : height;

    // Reflection result
    RHITextureDesc desc;
    desc.width = ssrWidth;
    desc.height = ssrHeight;
    desc.format = RHIFormat::RGBA16_FLOAT;
    desc.usage = RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource;
    m_reflectionResult = m_device->CreateTexture(desc);

    // Hit mask (for debugging and temporal filtering)
    desc.format = RHIFormat::R8_UNORM;
    m_hitMask = m_device->CreateTexture(desc);

    // Ray hit UV (stores where the ray hit in screen space)
    desc.format = RHIFormat::RG16_FLOAT;
    m_rayHitUV = m_device->CreateTexture(desc);

    // History for temporal filtering
    if (m_config.temporalFilter)
    {
        desc.format = RHIFormat::RGBA16_FLOAT;
        m_history = m_device->CreateTexture(desc);
    }

    // HiZ pyramid storage is allocated for diagnostics/resource planning only.
    uint32 hiZWidth = std::max<uint32>(1u, width);
    uint32 hiZHeight = std::max<uint32>(1u, height);
    int mipLevels = static_cast<int>(std::floor(std::log2(std::max(hiZWidth, hiZHeight)))) + 1;

    desc.width = hiZWidth;
    desc.height = hiZHeight;
    desc.mipLevels = mipLevels;
    desc.format = RHIFormat::R32_FLOAT;
    desc.usage = RHITextureUsage::ShaderResource | RHITextureUsage::UnorderedAccess;
    m_hiZPyramid = m_device->CreateTexture(desc);

    // Constant buffer
    RHIBufferDesc bufDesc;
    bufDesc.size = 256;
    bufDesc.usage = RHIBufferUsage::Constant;
    m_constantBuffer = m_device->CreateBuffer(bufDesc);
}

void SSR::Compute(RHICommandContext& ctx,
                  RHITexture* colorTexture,
                  RHITexture* depthTexture,
                  RHITexture* normalTexture,
                  RHITexture* roughnessTexture,
                  const Mat4& viewMatrix,
                  const Mat4& projMatrix)
{
    (void)ctx;
    (void)viewMatrix;
    (void)projMatrix;

    m_lastComputeStats = {};
    m_lastComputeStats.requested = IsRequestedEnabled();
    m_lastComputeStats.supported = IsSupported();
    m_lastComputeStats.scheduled = false;
    m_lastComputeStats.executed = false;
    m_lastComputeStats.colorAvailable = colorTexture != nullptr;
    m_lastComputeStats.depthAvailable = depthTexture != nullptr;
    m_lastComputeStats.normalAvailable = normalTexture != nullptr;
    m_lastComputeStats.roughnessAvailable = roughnessTexture != nullptr;
    m_lastComputeStats.temporalHistoryRequired = m_config.temporalFilter;
    m_lastComputeStats.implementationTier = SSRImplementationTier::Unsupported;

    if (!colorTexture || !depthTexture || !normalTexture || !roughnessTexture)
    {
        m_lastComputeStats.missingInputReason =
            "SSR skipped because color, depth, normal, or roughness input is unavailable";
        m_lastComputeStats.fallbackReason = m_lastComputeStats.missingInputReason;
        if (IsRequestedEnabled())
        {
            RVX_CORE_WARN("SSR: {}", m_lastComputeStats.fallbackReason);
        }
        return;
    }

    if (!m_device)
    {
        m_lastComputeStats.fallbackReason = "SSR skipped because no RHI device is available";
        if (IsRequestedEnabled())
        {
            RVX_CORE_WARN("SSR: {}", m_lastComputeStats.fallbackReason);
        }
        return;
    }

    if (!IsEnabled())
    {
        m_lastComputeStats.fallbackReason = IsSupported() ?
            "SSR disabled by configuration" : GetUnsupportedReason();
        if (IsRequestedEnabled() && !IsSupported())
        {
            RVX_CORE_WARN("SSR: unsupported compute skipped: {}", GetUnsupportedReason());
        }
        return;
    }

    m_lastComputeStats.fallbackReason = RVX_SSR_UNSUPPORTED_REASON;
    RVX_CORE_WARN("SSR: compute path requested but {}", m_lastComputeStats.fallbackReason);
}

void SSR::BuildHiZPyramid(RHICommandContext& ctx, RHITexture* depth)
{
    (void)ctx;
    (void)depth;

    // Reserved for the future HiZ implementation. The public Compute path is capability-gated.
}

void SSR::RayMarch(RHICommandContext& ctx)
{
    (void)ctx;

    // Reserved for the future ray-march implementation. The public Compute path is capability-gated.
}

void SSR::Resolve(RHICommandContext& ctx, RHITexture* color)
{
    (void)ctx;
    (void)color;

    // Reserved for the future resolve implementation. The public Compute path is capability-gated.
}

void SSR::TemporalFilter(RHICommandContext& ctx)
{
    (void)ctx;

    // Reserved for the future temporal filter implementation. The public Compute path is capability-gated.
}

} // namespace RVX