/**
 * @file SSAO.cpp
 * @brief SSAO implementation
 */

#include "Render/PostProcess/SSAO.h"
#include "Core/Log.h"

#include <algorithm>
#include <cmath>

namespace RVX
{
namespace
{
    constexpr uint64 RVX_SSAO_CONSTANT_BUFFER_ALIGNMENT = 256;

    uint64 AlignSSAOConstantBufferSize(uint64 size)
    {
        return (size + RVX_SSAO_CONSTANT_BUFFER_ALIGNMENT - 1) &
               ~(RVX_SSAO_CONSTANT_BUFFER_ALIGNMENT - 1);
    }

    float Deterministic01(uint32 index, uint32 salt)
    {
        uint32 value = index * 747796405u + salt * 2891336453u + 277803737u;
        value = ((value >> ((value >> 28u) + 4u)) ^ value) * 277803737u;
        value = (value >> 22u) ^ value;
        return static_cast<float>(value & 0x00FFFFFFu) / static_cast<float>(0x01000000u);
    }

    void AppendReason(std::string& reason, const char* text)
    {
        if (!text || !text[0])
        {
            return;
        }

        if (!reason.empty())
        {
            reason += "; ";
        }
        reason += text;
    }

    struct SSAOConstants
    {
        Vec4 targetSize = Vec4(1.0f);
        Vec4 params = Vec4(0.0f);
        Vec4 fallbackParams = Vec4(0.0f);
    };
} // namespace

const char* GetSSAOImplementationTierName(SSAOImplementationTier tier)
{
    switch (tier)
    {
        case SSAOImplementationTier::Unsupported:
            return "Unsupported";
        case SSAOImplementationTier::MinimalNeutralOutput:
            return "MinimalNeutralOutput";
        case SSAOImplementationTier::DepthOnlyLowTier:
            return "DepthOnlyLowTier";
        case SSAOImplementationTier::DepthNormalLowTier:
            return "DepthNormalLowTier";
        default:
            return "Unknown";
    }
}

SSAO::~SSAO()
{
    Shutdown();
}

void SSAO::Initialize(IRHIDevice* device, uint32 width, uint32 height)
{
    if (!device)
    {
        RVX_CORE_ERROR("SSAO: Cannot initialize without an RHI device");
        m_unsupportedReason = "No RHI device";
        return;
    }

    m_device = device;
    m_width = width;
    m_height = height;

    CreateResources(width, height);
    CreateNoiseTexture();
    CreateSampleKernel();
    RefreshSupportState();
}

void SSAO::Shutdown()
{
    m_aoResult.Reset();
    m_aoBlurred.Reset();
    m_aoHistory.Reset();
    m_aoResultRTV.Reset();
    m_aoBlurredRTV.Reset();
    m_noiseTexture.Reset();
    m_sampleKernelBuffer.Reset();
    m_constantBuffer.Reset();
    m_ssaoPipeline.Reset();
    m_blurHPipeline.Reset();
    m_blurVPipeline.Reset();
    m_temporalPipeline.Reset();
    m_device = nullptr;
    m_supported = false;
    m_unsupportedReason = "SSAO resources are not initialized";
    m_lastComputeStats = {};
}

void SSAO::Resize(uint32 width, uint32 height)
{
    m_width = width;
    m_height = height;
    CreateResources(width, height);
    RefreshSupportState();
}

void SSAO::SetConfig(const SSAOConfig& config)
{
    m_config = config;
    if (m_device)
    {
        CreateResources(m_width, m_height);
        CreateNoiseTexture();
    }
    CreateSampleKernel();
    RefreshSupportState();
}

void SSAO::CreateResources(uint32 width, uint32 height)
{
    if (!m_device) return;
    if (width == 0 || height == 0)
    {
        m_supported = false;
        m_unsupportedReason = "SSAO requires a non-zero render extent";
        return;
    }

    uint32 aoWidth = m_config.halfResolution ? std::max<uint32>(1, width / 2) : width;
    uint32 aoHeight = m_config.halfResolution ? std::max<uint32>(1, height / 2) : height;

    m_aoResultRTV.Reset();
    m_aoBlurredRTV.Reset();

    RHITextureDesc desc;
    desc.width = aoWidth;
    desc.height = aoHeight;
    desc.format = RHIFormat::R8_UNORM;
    desc.usage = RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource;
    desc.debugName = "SSAO_Result";

    m_aoResult = m_device->CreateTexture(desc);
    desc.debugName = "SSAO_Blurred";
    m_aoBlurred = m_device->CreateTexture(desc);

    if (m_aoResult)
    {
        RHITextureViewDesc viewDesc;
        viewDesc.type = RHITextureViewType::RenderTarget;
        viewDesc.format = RHIFormat::R8_UNORM;
        viewDesc.debugName = "SSAO_Result_RTV";
        m_aoResultRTV = m_device->CreateTextureView(m_aoResult.Get(), viewDesc);
    }

    if (m_aoBlurred)
    {
        RHITextureViewDesc viewDesc;
        viewDesc.type = RHITextureViewType::RenderTarget;
        viewDesc.format = RHIFormat::R8_UNORM;
        viewDesc.debugName = "SSAO_Blurred_RTV";
        m_aoBlurredRTV = m_device->CreateTextureView(m_aoBlurred.Get(), viewDesc);
    }

    if (m_config.temporalFilter)
    {
        desc.debugName = "SSAO_History";
        m_aoHistory = m_device->CreateTexture(desc);
    }
    else
    {
        m_aoHistory.Reset();
    }

    // Constant buffer
    RHIBufferDesc bufDesc;
    bufDesc.size = AlignSSAOConstantBufferSize(sizeof(SSAOConstants));
    bufDesc.usage = RHIBufferUsage::Constant;
    bufDesc.memoryType = RHIMemoryType::Upload;
    bufDesc.debugName = "SSAO_Constants";
    m_constantBuffer = m_device->CreateBuffer(bufDesc);
}

void SSAO::CreateNoiseTexture()
{
    if (!m_device) return;

    RHITextureDesc desc;
    desc.width = 4;
    desc.height = 4;
    desc.format = RHIFormat::RGBA16_FLOAT;
    desc.usage = RHITextureUsage::ShaderResource;
    desc.debugName = "SSAO_DeterministicNoise";
    m_noiseTexture = m_device->CreateTexture(desc);
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

    for (int i = 0; i < sampleCount; ++i)
    {
        const uint32 sampleIndex = static_cast<uint32>(i);
        Vec3 sample(
            Deterministic01(sampleIndex, 1) * 2.0f - 1.0f,
            Deterministic01(sampleIndex, 2) * 2.0f - 1.0f,
            Deterministic01(sampleIndex, 3)
        );
        sample = normalize(sample);
        sample *= Deterministic01(sampleIndex, 4);

        float scale = static_cast<float>(i) / sampleCount;
        scale = 0.1f + scale * scale * 0.9f;
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
        desc.stride = sizeof(Vec4);
        desc.debugName = "SSAO_SampleKernel";
        m_sampleKernelBuffer = m_device->CreateBuffer(desc);
        if (m_sampleKernelBuffer && !m_sampleKernel.empty())
        {
            m_sampleKernelBuffer->Upload(m_sampleKernel.data(), m_sampleKernel.size());
        }
    }
}

void SSAO::RefreshSupportState()
{
    if (!m_device)
    {
        m_supported = false;
        m_unsupportedReason = "No RHI device";
        return;
    }

    if (m_width == 0 || m_height == 0)
    {
        m_supported = false;
        m_unsupportedReason = "SSAO requires a non-zero render extent";
        return;
    }

    if (!m_aoResult || !m_aoBlurred || !m_aoResultRTV || !m_aoBlurredRTV ||
        !m_noiseTexture || !m_sampleKernelBuffer || !m_constantBuffer)
    {
        m_supported = false;
        m_unsupportedReason = "SSAO low-tier resources could not be created";
        return;
    }

    m_supported = true;
    m_unsupportedReason.clear();
}

void SSAO::Compute(RHICommandContext& ctx,
                   RHITexture* depthTexture,
                   RHITexture* normalTexture,
                   const Mat4& viewMatrix,
                   const Mat4& projMatrix)
{
    (void)viewMatrix;
    (void)projMatrix;

    m_lastComputeStats = {};
    m_lastComputeStats.requested = IsRequestedEnabled();
    m_lastComputeStats.supported = IsSupported();
    m_lastComputeStats.depthAvailable = depthTexture != nullptr;
    m_lastComputeStats.normalAvailable = normalTexture != nullptr;
    m_lastComputeStats.sampleCount = static_cast<uint32>(m_sampleKernel.size());
    m_lastComputeStats.implementationTier = SSAOImplementationTier::Unsupported;

    if (!depthTexture)
    {
        m_lastComputeStats.fallbackReason = "SSAO skipped because depth input is unavailable";
        if (IsRequestedEnabled())
        {
            RVX_CORE_WARN("SSAO: {}", m_lastComputeStats.fallbackReason);
        }
        return;
    }

    if (!IsEnabled() || !m_device)
    {
        if (IsRequestedEnabled() && !IsSupported())
        {
            RVX_CORE_WARN("SSAO: unsupported compute skipped: {}", GetUnsupportedReason());
        }
        m_lastComputeStats.fallbackReason = GetUnsupportedReason();
        return;
    }

    if (m_config.useNormals && !normalTexture)
    {
        m_lastComputeStats.normalFallbackUsed = true;
        AppendReason(m_lastComputeStats.fallbackReason,
                     "SSAO using depth-only low-tier fallback because normal input is unavailable");
    }

    if (m_config.temporalFilter)
    {
        m_lastComputeStats.temporalFallbackUsed = true;
        AppendReason(m_lastComputeStats.fallbackReason,
                     "SSAO temporal accumulation is deferred for the low-tier runtime path");
    }

    ComputeSSAO(ctx, depthTexture, normalTexture);

    if (m_config.blurPasses > 0)
    {
        BlurSSAO(ctx, depthTexture);
    }

    if (m_lastComputeStats.neutralOutputFallbackUsed)
    {
        AppendReason(m_lastComputeStats.fallbackReason,
                     "SSAO emitted a neutral AO target until the depth sampling shader path lands");
    }

    m_lastComputeStats.executed = true;
}

void SSAO::ComputeSSAO(RHICommandContext& ctx, RHITexture* depth, RHITexture* normal)
{
    (void)depth;
    (void)normal;

    if (!m_aoResult || !m_aoResultRTV || !m_constantBuffer)
    {
        return;
    }

    SSAOConstants constants;
    constants.targetSize = Vec4(
        static_cast<float>(m_aoResult->GetWidth()),
        static_cast<float>(m_aoResult->GetHeight()),
        m_aoResult->GetWidth() > 0 ? 1.0f / static_cast<float>(m_aoResult->GetWidth()) : 1.0f,
        m_aoResult->GetHeight() > 0 ? 1.0f / static_cast<float>(m_aoResult->GetHeight()) : 1.0f);
    constants.params = Vec4(m_config.radius, m_config.intensity, m_config.bias, m_config.power);
    constants.fallbackParams = Vec4(
        static_cast<float>(m_sampleKernel.size()),
        m_config.useNormals && !normal ? 1.0f : 0.0f,
        m_config.temporalFilter ? 1.0f : 0.0f,
        1.0f);
    m_constantBuffer->Upload(&constants, 1);

    RHIRenderPassDesc renderPassDesc;
    renderPassDesc.AddColorAttachment(m_aoResultRTV.Get(),
                                      RHILoadOp::Clear,
                                      RHIStoreOp::Store,
                                      RHIClearColor{1.0f, 1.0f, 1.0f, 1.0f});
    renderPassDesc.SetRenderArea(0, 0, m_aoResult->GetWidth(), m_aoResult->GetHeight());

    ctx.BeginRenderPass(renderPassDesc);
    ctx.EndRenderPass();

    m_lastComputeStats.aoPassCount = 1;
    m_lastComputeStats.neutralOutputFallbackUsed = true;
    m_lastComputeStats.implementationTier = SSAOImplementationTier::MinimalNeutralOutput;
}

void SSAO::BlurSSAO(RHICommandContext& ctx, RHITexture* depth)
{
    (void)depth;

    if (!m_aoResult || !m_aoBlurred)
    {
        return;
    }

    ctx.CopyTexture(m_aoResult.Get(), m_aoBlurred.Get());
    m_lastComputeStats.blurPassCount = 1;
}

} // namespace RVX
