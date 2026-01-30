/**
 * @file SSAO.cpp
 * @brief SSAO implementation
 */

#include "Render/PostProcess/SSAO.h"
#include <random>
#include <cmath>

namespace RVX
{

SSAO::~SSAO()
{
    Shutdown();
}

void SSAO::Initialize(IRHIDevice* device, uint32 width, uint32 height)
{
    m_device = device;
    m_width = width;
    m_height = height;

    CreateResources(width, height);
    CreateNoiseTexture();
    CreateSampleKernel();
}

void SSAO::Shutdown()
{
    m_aoResult.Reset();
    m_aoBlurred.Reset();
    m_aoHistory.Reset();
    m_noiseTexture.Reset();
    m_sampleKernelBuffer.Reset();
    m_constantBuffer.Reset();
    m_ssaoPipeline.Reset();
    m_blurHPipeline.Reset();
    m_blurVPipeline.Reset();
    m_temporalPipeline.Reset();
    m_device = nullptr;
}

void SSAO::Resize(uint32 width, uint32 height)
{
    m_width = width;
    m_height = height;
    CreateResources(width, height);
}

void SSAO::SetConfig(const SSAOConfig& config)
{
    m_config = config;
    CreateSampleKernel();  // Regenerate samples for new quality
}

void SSAO::CreateResources(uint32 width, uint32 height)
{
    if (!m_device) return;

    uint32 aoWidth = m_config.halfResolution ? width / 2 : width;
    uint32 aoHeight = m_config.halfResolution ? height / 2 : height;

    RHITextureDesc desc;
    desc.width = aoWidth;
    desc.height = aoHeight;
    desc.format = RHIFormat::R8_UNORM;
    desc.usage = RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource;

    m_aoResult = m_device->CreateTexture(desc);
    m_aoBlurred = m_device->CreateTexture(desc);
    
    if (m_config.temporalFilter)
    {
        m_aoHistory = m_device->CreateTexture(desc);
    }

    // Constant buffer
    RHIBufferDesc bufDesc;
    bufDesc.size = 256;
    bufDesc.usage = RHIBufferUsage::Constant;
    m_constantBuffer = m_device->CreateBuffer(bufDesc);
}

void SSAO::CreateNoiseTexture()
{
    if (!m_device) return;

    // 4x4 noise texture with random rotation vectors
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    std::vector<Vec4> noiseData(16);
    for (auto& n : noiseData)
    {
        n = Vec4(dist(gen), dist(gen), 0.0f, 0.0f);
        n = normalize(n);
    }

    RHITextureDesc desc;
    desc.width = 4;
    desc.height = 4;
    desc.format = RHIFormat::RGBA16_FLOAT;
    desc.usage = RHITextureUsage::ShaderResource;
    m_noiseTexture = m_device->CreateTexture(desc);

    // TODO: Upload noise data to texture
}

void SSAO::CreateSampleKernel()
{
    int sampleCount = 8;
    switch (m_config.quality)
    {
        case SSAOQuality::Low: sampleCount = 4; break;
        case SSAOQuality::Medium: sampleCount = 8; break;
        case SSAOQuality::High: sampleCount = 16; break;
        case SSAOQuality::Ultra: sampleCount = 32; break;
    }

    m_sampleKernel.clear();
    m_sampleKernel.reserve(sampleCount);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);

    for (int i = 0; i < sampleCount; ++i)
    {
        // Hemisphere sample in tangent space
        Vec3 sample(
            dist(gen) * 2.0f - 1.0f,
            dist(gen) * 2.0f - 1.0f,
            dist(gen)  // Only positive Z (hemisphere)
        );
        sample = normalize(sample);
        sample *= dist(gen);

        // Scale to favor samples closer to origin
        float scale = static_cast<float>(i) / sampleCount;
        scale = 0.1f + scale * scale * 0.9f;  // Lerp(0.1, 1.0, scale^2)
        sample *= scale;

        m_sampleKernel.push_back(Vec4(sample, 0.0f));
    }

    // Create/update GPU buffer
    if (m_device)
    {
        RHIBufferDesc desc;
        desc.size = m_sampleKernel.size() * sizeof(Vec4);
        desc.usage = RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource;
        desc.memoryType = RHIMemoryType::Upload;
        m_sampleKernelBuffer = m_device->CreateBuffer(desc);
        // Upload sample data
        if (m_sampleKernelBuffer && !m_sampleKernel.empty())
        {
            m_sampleKernelBuffer->Upload(m_sampleKernel.data(), m_sampleKernel.size());
        }
    }
}

void SSAO::Compute(RHICommandContext& ctx,
                   RHITexture* depthTexture,
                   RHITexture* normalTexture,
                   const Mat4& viewMatrix,
                   const Mat4& projMatrix)
{
    if (!m_enabled || !m_device) return;

    ComputeSSAO(ctx, depthTexture, normalTexture);
    
    if (m_config.blurPasses > 0)
    {
        BlurSSAO(ctx, depthTexture);
    }
}

void SSAO::ComputeSSAO(RHICommandContext& ctx, RHITexture* depth, RHITexture* normal)
{
    // TODO: Dispatch SSAO compute shader
    // - Sample depth buffer
    // - Reconstruct view-space position
    // - Sample hemisphere around each point
    // - Accumulate occlusion
}

void SSAO::BlurSSAO(RHICommandContext& ctx, RHITexture* depth)
{
    // TODO: Dispatch bilateral blur passes
    // - Horizontal pass
    // - Vertical pass
    // - Use depth-aware filtering to preserve edges
}

} // namespace RVX
