/**
 * @file SSR.cpp
 * @brief SSR implementation
 */

#include "Render/PostProcess/SSR.h"

namespace RVX
{

SSR::~SSR()
{
    Shutdown();
}

void SSR::Initialize(IRHIDevice* device, uint32 width, uint32 height)
{
    m_device = device;
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

    uint32 ssrWidth = m_config.halfResolution ? width / 2 : width;
    uint32 ssrHeight = m_config.halfResolution ? height / 2 : height;

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

    // HiZ pyramid for accelerated ray marching
    uint32 hiZWidth = width;
    uint32 hiZHeight = height;
    int mipLevels = static_cast<int>(std::floor(std::log2(std::max(width, height)))) + 1;

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
    if (!m_enabled || !m_device) return;

    // Build HiZ pyramid from depth
    BuildHiZPyramid(ctx, depthTexture);

    // Ray march to find reflection hits
    RayMarch(ctx);

    // Resolve ray hits to color
    Resolve(ctx, colorTexture);

    // Apply temporal filtering
    if (m_config.temporalFilter)
    {
        TemporalFilter(ctx);
    }
}

void SSR::BuildHiZPyramid(RHICommandContext& ctx, RHITexture* depth)
{
    // TODO: Dispatch HiZ pyramid generation
    // - Copy depth to mip 0
    // - For each subsequent mip: take min of 2x2 region
}

void SSR::RayMarch(RHICommandContext& ctx)
{
    // TODO: Dispatch ray marching compute shader
    // - For each pixel: reflect view direction around normal
    // - March ray through HiZ pyramid
    // - Binary search refinement on hit
    // - Store hit UV and mask
}

void SSR::Resolve(RHICommandContext& ctx, RHITexture* color)
{
    // TODO: Sample color at hit UVs
    // - Apply edge fade
    // - Handle misses (use environment map fallback)
}

void SSR::TemporalFilter(RHICommandContext& ctx)
{
    // TODO: Blend with history
    // - Reproject using motion vectors
    // - Apply neighborhood clamping
    // - Update history buffer
}

} // namespace RVX
