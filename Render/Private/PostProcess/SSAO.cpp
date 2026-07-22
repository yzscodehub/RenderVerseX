/**
 * @file SSAO.cpp
 * @brief SSAO implementation
 */

#include "Render/PostProcess/SSAO.h"
#include "Core/Log.h"
#include "Render/Graph/ResourceViewCache.h"
#include "Render/PipelineCache.h"

#include <algorithm>
#include <cmath>
#include <cstring>

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

    struct SSAOPassGPUConstants
    {
        float textureSize[2] = {1.0f, 1.0f};
        float invTextureSize[2] = {1.0f, 1.0f};
        float radius = 0.5f;
        float intensity = 1.0f;
        float bias = 0.025f;
        float power = 2.0f;
        float sampleCount = 8.0f;
        float reverseZ = 0.0f;
        float normalFallback = 1.0f;
        float temporalFallback = 0.0f;
        float padding[4] = {};
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

SSAOPass::SSAOPass()
{
    m_config.useNormals = true;
    m_config.temporalFilter = false;
    m_config.blurPasses = 0;
    MarkUnsupported("SSAO requires fullscreen pipeline resources before it can execute");
}

void SSAOPass::Configure(const PostProcessSettings& settings)
{
    m_enabled = settings.enableSSAO;
    m_config.radius = std::max(0.0f, settings.ssaoRadius);
    m_config.intensity = std::max(0.0f, settings.ssaoIntensity);

    switch (settings.visualQualityPreset)
    {
        case RenderVisualQualityPreset::Low:
            m_config.quality = SSAOQuality::Low;
            break;
        case RenderVisualQualityPreset::High:
            m_config.quality = SSAOQuality::High;
            break;
        case RenderVisualQualityPreset::Cinematic:
            m_config.quality = SSAOQuality::Ultra;
            break;
        case RenderVisualQualityPreset::Medium:
        default:
            m_config.quality = SSAOQuality::Medium;
            break;
    }

    RefreshSupportState();
}

void SSAOPass::SetResources(PipelineCache* pipelineCache, ResourceViewCache* viewCache)
{
    m_pipelineCache = pipelineCache;
    m_viewCache = viewCache;

    IRHIDevice* device = m_pipelineCache ? m_pipelineCache->GetDevice() : nullptr;
    if (device != m_resourceDevice)
    {
        m_constantBuffer.Reset();
        m_sampler.Reset();
        m_resourceDevice = device;
    }

    RefreshSupportState();
}

void SSAOPass::SetConfig(const SSAOConfig& config)
{
    m_config = config;
    RefreshSupportState();
}

void SSAOPass::AddToGraph(RenderGraph& graph, RGTextureHandle input, RGTextureHandle output)
{
    PostProcessFrameInputs frameInputs;
    frameInputs.sceneColor = input;
    AddToGraph(graph, frameInputs, output);
}

void SSAOPass::AddToGraph(RenderGraph& graph,
                          const PostProcessFrameInputs& frameInputs,
                          RGTextureHandle output)
{
    m_lastGraphStats = {};
    m_lastGraphStats.requested = IsRequestedEnabled();
    m_lastGraphStats.supported = IsSupported();
    m_lastGraphStats.depthAvailable = frameInputs.depth.IsValid();
    m_lastGraphStats.normalAvailable = frameInputs.normal.IsValid();
    m_lastGraphStats.sampleCount = ResolveSampleCount();
    m_lastGraphStats.normalFallbackUsed = m_config.useNormals && !frameInputs.normal.IsValid();
    m_lastGraphStats.temporalFallbackUsed = m_config.temporalFilter;
    m_lastGraphStats.implementationTier = frameInputs.normal.IsValid()
                                              ? SSAOImplementationTier::DepthNormalLowTier
                                              : SSAOImplementationTier::DepthOnlyLowTier;

    if (!IsEnabled())
    {
        if (IsRequestedEnabled() && !IsSupported())
        {
            RVX_CORE_WARN("SSAO: unsupported fullscreen pass skipped: {}", GetUnsupportedReason());
        }
        m_lastGraphStats.fallbackReason = GetUnsupportedReason();
        m_lastGraphStats.implementationTier = SSAOImplementationTier::Unsupported;
        return;
    }

    if (!frameInputs.depth.IsValid())
    {
        m_lastGraphStats.fallbackReason = "SSAO skipped because depth frame input is unavailable";
        RVX_CORE_WARN("SSAO: {}", m_lastGraphStats.fallbackReason);
        return;
    }

    if (m_lastGraphStats.normalFallbackUsed)
    {
        AppendReason(m_lastGraphStats.fallbackReason,
                     "SSAO using depth-only low-tier fallback because normal input is unavailable");
    }
    if (m_lastGraphStats.temporalFallbackUsed)
    {
        AppendReason(m_lastGraphStats.fallbackReason,
                     "SSAO temporal accumulation is deferred for the low-tier fullscreen path");
    }

    struct SSAOPassData
    {
        RGTextureHandle input;
        RGTextureHandle depth;
        RGTextureHandle output;
        SSAOConfig config;
        bool normalFallbackUsed = false;
        bool temporalFallbackUsed = false;
    };

    graph.AddPass<SSAOPassData>(
        "SSAO",
        RenderGraphPassType::Graphics,
        [this, frameInputs, output](RenderGraphBuilder& builder, SSAOPassData& data)
        {
            data.input = builder.Read(frameInputs.sceneColor);
            data.depth = builder.Read(frameInputs.depth);
            data.output = builder.Write(output, RHIResourceState::RenderTarget);
            data.config = m_config;
            data.normalFallbackUsed = m_lastGraphStats.normalFallbackUsed;
            data.temporalFallbackUsed = m_lastGraphStats.temporalFallbackUsed;
        },
        [this, &graph](const SSAOPassData& data, RHICommandContext& ctx)
        {
            if (!m_pipelineCache || !m_viewCache)
            {
                RVX_CORE_WARN("SSAO: missing resources during execution");
                return;
            }

            RHIFormat outputFormat = RHIFormat::Unknown;
            if (const RHITextureDesc* outputDesc = graph.GetTextureDesc(data.output))
            {
                outputFormat = outputDesc->format;
            }

            RHIPipeline* pipeline = m_pipelineCache->GetSSAOPipeline(outputFormat);
            RHIDescriptorSetLayout* setLayout = m_pipelineCache->GetPostProcessSetLayout();
            IRHIDevice* device = m_pipelineCache->GetDevice();
            if (!pipeline || !setLayout || !device)
            {
                RVX_CORE_WARN("SSAO: fullscreen pipeline resources are unavailable");
                return;
            }

            RHITexture* inputTexture = graph.GetTexture(data.input);
            RHITexture* depthTexture = graph.GetTexture(data.depth);
            RHITexture* outputTexture = graph.GetTexture(data.output);
            if (!inputTexture || !depthTexture || !outputTexture)
            {
                RVX_CORE_WARN("SSAO: input, depth, or output texture is unavailable");
                return;
            }

            RHITextureView* inputView = m_viewCache->GetDefaultSRV(inputTexture);
            RHITextureView* depthView = m_viewCache->GetDefaultSRV(depthTexture);
            RHITextureView* outputView = m_viewCache->GetDefaultRTV(outputTexture);
            if (!inputView || !depthView || !outputView)
            {
                RVX_CORE_WARN("SSAO: failed to resolve input SRV, depth SRV, or output RTV");
                return;
            }

            if (!EnsureRuntimeResources() ||
                !UpdateConstants(inputTexture->GetWidth(),
                                 inputTexture->GetHeight(),
                                 data.config,
                                 data.normalFallbackUsed,
                                 data.temporalFallbackUsed))
            {
                return;
            }

            RHIDescriptorSetDesc descriptorDesc;
            descriptorDesc.layout = setLayout;
            descriptorDesc.debugName = "SSAODescriptorSet";
            descriptorDesc.BindBuffer(0,
                                      m_constantBuffer.Get(),
                                      0,
                                      AlignSSAOConstantBufferSize(sizeof(SSAOPassGPUConstants)));
            descriptorDesc.BindTexture(1, inputView);
            descriptorDesc.BindSampler(2, m_sampler.Get());
            descriptorDesc.BindTexture(3, depthView);

            RHIDescriptorSetRef descriptorSet = device->CreateDescriptorSet(descriptorDesc);
            if (!descriptorSet)
            {
                RVX_CORE_WARN("SSAO: failed to create descriptor set");
                return;
            }
            if (!RetainSubmissionResource(descriptorSet))
            {
                RVX_CORE_WARN("SSAO: submission ownership rejected descriptor set");
                return;
            }

            RHIRenderPassDesc renderPassDesc;
            renderPassDesc.AddColorAttachment(outputView, RHILoadOp::DontCare, RHIStoreOp::Store);
            renderPassDesc.SetRenderArea(0, 0, outputTexture->GetWidth(), outputTexture->GetHeight());

            ctx.BeginRenderPass(renderPassDesc);
            ctx.SetPipeline(pipeline);
            ctx.SetDescriptorSet(0, descriptorSet.Get());

            RHIViewport viewport;
            viewport.width = static_cast<float>(outputTexture->GetWidth());
            viewport.height = static_cast<float>(outputTexture->GetHeight());
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;
            ctx.SetViewport(viewport);

            RHIRect scissor;
            scissor.width = outputTexture->GetWidth();
            scissor.height = outputTexture->GetHeight();
            ctx.SetScissor(scissor);

            ctx.Draw(3, 1, 0, 0);
            ctx.EndRenderPass();

            m_lastGraphStats.executed = true;
            m_lastGraphStats.aoPassCount = 1;
        });
}

bool SSAOPass::EnsureRuntimeResources()
{
    IRHIDevice* device = m_pipelineCache ? m_pipelineCache->GetDevice() : nullptr;
    if (!device)
    {
        MarkUnsupported("SSAO requires an RHI device");
        return false;
    }

    if (!m_constantBuffer)
    {
        RHIBufferDesc bufferDesc;
        bufferDesc.size = AlignSSAOConstantBufferSize(sizeof(SSAOPassGPUConstants));
        bufferDesc.usage = RHIBufferUsage::Constant;
        bufferDesc.memoryType = RHIMemoryType::Upload;
        bufferDesc.debugName = "SSAOPassConstants";

        m_constantBuffer = device->CreateBuffer(bufferDesc);
        if (!m_constantBuffer)
        {
            MarkUnsupported("SSAO constant buffer creation failed");
            return false;
        }
    }

    if (!m_sampler)
    {
        RHISamplerDesc samplerDesc = RHISamplerDesc::LinearClamp();
        samplerDesc.debugName = "SSAOLinearClampSampler";
        m_sampler = device->CreateSampler(samplerDesc);
        if (!m_sampler)
        {
            MarkUnsupported("SSAO sampler creation failed");
            return false;
        }
    }

    return true;
}

bool SSAOPass::UpdateConstants(uint32 width,
                               uint32 height,
                               const SSAOConfig& config,
                               bool normalFallbackUsed,
                               bool temporalFallbackUsed)
{
    if (!m_constantBuffer)
    {
        return false;
    }

    SSAOPassGPUConstants constants;
    constants.textureSize[0] = static_cast<float>(width);
    constants.textureSize[1] = static_cast<float>(height);
    constants.invTextureSize[0] = width > 0 ? 1.0f / static_cast<float>(width) : 1.0f;
    constants.invTextureSize[1] = height > 0 ? 1.0f / static_cast<float>(height) : 1.0f;
    constants.radius = std::max(config.radius, 0.0f);
    constants.intensity = std::max(config.intensity, 0.0f);
    constants.bias = std::max(config.bias, 0.0f);
    constants.power = std::max(config.power, 0.01f);
    constants.sampleCount = static_cast<float>(ResolveSampleCount());
    constants.reverseZ = m_pipelineCache && m_pipelineCache->IsReverseZ() ? 1.0f : 0.0f;
    constants.normalFallback = normalFallbackUsed ? 1.0f : 0.0f;
    constants.temporalFallback = temporalFallbackUsed ? 1.0f : 0.0f;

    void* mapped = m_constantBuffer->Map();
    if (!mapped)
    {
        RVX_CORE_WARN("SSAO: failed to map constants");
        return false;
    }

    std::memcpy(mapped, &constants, sizeof(constants));
    m_constantBuffer->Unmap();
    return true;
}

uint32 SSAOPass::ResolveSampleCount() const
{
    switch (m_config.quality)
    {
        case SSAOQuality::Low:
            return 4;
        case SSAOQuality::High:
            return 12;
        case SSAOQuality::Ultra:
            return 16;
        case SSAOQuality::Medium:
        default:
            return 8;
    }
}

void SSAOPass::RefreshSupportState()
{
    if (!m_pipelineCache || !m_pipelineCache->IsInitialized())
    {
        MarkUnsupported("SSAO requires an initialized PipelineCache");
        return;
    }

    if (!m_viewCache || !m_viewCache->IsInitialized())
    {
        MarkUnsupported("SSAO requires an initialized ResourceViewCache");
        return;
    }

    if (!m_pipelineCache->GetSSAOPipeline() ||
        !m_pipelineCache->GetPostProcessLayout() ||
        !m_pipelineCache->GetPostProcessSetLayout())
    {
        MarkUnsupported("SSAO fullscreen pipeline resources are not available");
        return;
    }

    if (!EnsureRuntimeResources())
    {
        return;
    }

    m_supported = true;
    m_unsupportedReason.clear();
}

} // namespace RVX
