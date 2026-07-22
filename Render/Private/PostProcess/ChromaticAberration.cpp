/**
 * @file ChromaticAberration.cpp
 * @brief Chromatic aberration implementation
 */

#include "Render/PostProcess/ChromaticAberration.h"
#include "Core/Log.h"
#include "Render/Graph/ResourceViewCache.h"
#include "Render/PipelineCache.h"

#include <cstring>

namespace RVX
{

namespace
{
    constexpr uint64 RVX_POST_PROCESS_CONSTANT_BUFFER_ALIGNMENT = 256;

    uint64 AlignPostProcessConstantBufferSize(uint64 size)
    {
        return (size + RVX_POST_PROCESS_CONSTANT_BUFFER_ALIGNMENT - 1) &
               ~(RVX_POST_PROCESS_CONSTANT_BUFFER_ALIGNMENT - 1);
    }

    struct ChromaticAberrationGPUConstants
    {
        float textureSize[2] = {1.0f, 1.0f};
        float invTextureSize[2] = {1.0f, 1.0f};
        float intensity = 0.1f;
        float startOffset = 0.0f;
        float radialFalloff = 1.0f;
        float useSpectral = 0.0f;
        float redOffset[2] = {-1.0f, 0.0f};
        float greenOffset[2] = {0.0f, 0.0f};
        float blueOffset[2] = {1.0f, 0.0f};
        float padding0[2] = {0.0f, 0.0f};
    };
} // namespace

ChromaticAberrationPass::ChromaticAberrationPass()
{
    m_enabled = false;
    MarkUnsupported("ChromaticAberration requires fullscreen pipeline resources before it can execute");
}

void ChromaticAberrationPass::Configure(const PostProcessSettings& settings)
{
    m_enabled = settings.enableChromaticAberration;
    m_config.intensity = settings.chromaticAberrationIntensity;
    RefreshSupportState();
}

void ChromaticAberrationPass::SetResources(PipelineCache* pipelineCache, ResourceViewCache* viewCache)
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

void ChromaticAberrationPass::SetConfig(const ChromaticAberrationConfig& config)
{
    m_config = config;
    RefreshSupportState();
}

void ChromaticAberrationPass::SetSpectralSampling(bool enable)
{
    m_config.useSpectral = enable;
    RefreshSupportState();
}

void ChromaticAberrationPass::AddToGraph(RenderGraph& graph, RGTextureHandle input, RGTextureHandle output)
{
    if (!IsEnabled())
    {
        if (IsRequestedEnabled() && !IsSupported())
        {
            RVX_CORE_WARN("ChromaticAberration: unsupported pass skipped: {}", GetUnsupportedReason());
        }
        return;
    }

    struct ChromaticAberrationData
    {
        RGTextureHandle input;
        RGTextureHandle output;
        ChromaticAberrationConfig config;
    };

    graph.AddPass<ChromaticAberrationData>(
        "ChromaticAberration",
        RenderGraphPassType::Graphics,
        [this, input, output](RenderGraphBuilder& builder, ChromaticAberrationData& data)
        {
            data.input = builder.Read(input);
            data.output = builder.Write(output, RHIResourceState::RenderTarget);
            data.config = m_config;
        },
        [this, &graph](const ChromaticAberrationData& data, RHICommandContext& ctx)
        {
            if (!m_pipelineCache || !m_viewCache)
            {
                RVX_CORE_WARN("ChromaticAberration: missing resources during execution");
                return;
            }

            RHIFormat outputFormat = RHIFormat::Unknown;
            if (const RHITextureDesc* outputDesc = graph.GetTextureDesc(data.output))
            {
                outputFormat = outputDesc->format;
            }

            RHIPipeline* pipeline = m_pipelineCache->GetChromaticAberrationPipeline(outputFormat);
            RHIDescriptorSetLayout* setLayout = m_pipelineCache->GetPostProcessSetLayout();
            IRHIDevice* device = m_pipelineCache->GetDevice();
            if (!pipeline || !setLayout || !device)
            {
                RVX_CORE_WARN("ChromaticAberration: fullscreen pipeline resources are unavailable");
                return;
            }

            RHITexture* inputTexture = graph.GetTexture(data.input);
            RHITexture* outputTexture = graph.GetTexture(data.output);
            if (!inputTexture || !outputTexture)
            {
                RVX_CORE_WARN("ChromaticAberration: input or output texture is unavailable");
                return;
            }

            RHITextureView* inputView = m_viewCache->GetDefaultSRV(inputTexture);
            RHITextureView* outputView = m_viewCache->GetDefaultRTV(outputTexture);
            if (!inputView || !outputView)
            {
                RVX_CORE_WARN("ChromaticAberration: failed to resolve input SRV or output RTV");
                return;
            }

            if (!EnsureRuntimeResources() ||
                !UpdateConstants(inputTexture->GetWidth(), inputTexture->GetHeight(), data.config))
            {
                return;
            }

            RHIDescriptorSetDesc descriptorDesc;
            descriptorDesc.layout = setLayout;
            descriptorDesc.debugName = "ChromaticAberrationDescriptorSet";
            descriptorDesc.BindBuffer(0,
                                      m_constantBuffer.Get(),
                                      0,
                                      AlignPostProcessConstantBufferSize(sizeof(ChromaticAberrationGPUConstants)));
            descriptorDesc.BindTexture(1, inputView);
            descriptorDesc.BindSampler(2, m_sampler.Get());

            RHIDescriptorSetRef descriptorSet = device->CreateDescriptorSet(descriptorDesc);
            if (!descriptorSet)
            {
                RVX_CORE_WARN("ChromaticAberration: failed to create descriptor set");
                return;
            }
            if (!RetainSubmissionResource(descriptorSet))
            {
                RVX_CORE_WARN("ChromaticAberration: submission ownership rejected descriptor set");
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
        });
}

bool ChromaticAberrationPass::EnsureRuntimeResources()
{
    IRHIDevice* device = m_pipelineCache ? m_pipelineCache->GetDevice() : nullptr;
    if (!device)
    {
        MarkUnsupported("ChromaticAberration requires an RHI device");
        return false;
    }

    if (!m_constantBuffer)
    {
        RHIBufferDesc bufferDesc;
        bufferDesc.size = AlignPostProcessConstantBufferSize(sizeof(ChromaticAberrationGPUConstants));
        bufferDesc.usage = RHIBufferUsage::Constant;
        bufferDesc.memoryType = RHIMemoryType::Upload;
        bufferDesc.debugName = "ChromaticAberrationConstants";

        m_constantBuffer = device->CreateBuffer(bufferDesc);
        if (!m_constantBuffer)
        {
            MarkUnsupported("ChromaticAberration constant buffer creation failed");
            return false;
        }
    }

    if (!m_sampler)
    {
        RHISamplerDesc samplerDesc = RHISamplerDesc::LinearClamp();
        samplerDesc.debugName = "ChromaticAberrationLinearClampSampler";
        m_sampler = device->CreateSampler(samplerDesc);
        if (!m_sampler)
        {
            MarkUnsupported("ChromaticAberration sampler creation failed");
            return false;
        }
    }

    return true;
}

bool ChromaticAberrationPass::UpdateConstants(uint32 width,
                                              uint32 height,
                                              const ChromaticAberrationConfig& config)
{
    if (!m_constantBuffer)
    {
        return false;
    }

    ChromaticAberrationGPUConstants constants;
    constants.textureSize[0] = static_cast<float>(width);
    constants.textureSize[1] = static_cast<float>(height);
    constants.invTextureSize[0] = width > 0 ? 1.0f / static_cast<float>(width) : 1.0f;
    constants.invTextureSize[1] = height > 0 ? 1.0f / static_cast<float>(height) : 1.0f;
    constants.intensity = config.intensity;
    constants.startOffset = config.startOffset;
    constants.radialFalloff = config.radialFalloff ? 1.0f : 0.0f;
    constants.useSpectral = config.useSpectral ? 1.0f : 0.0f;
    constants.redOffset[0] = config.redOffset.x;
    constants.redOffset[1] = config.redOffset.y;
    constants.greenOffset[0] = config.greenOffset.x;
    constants.greenOffset[1] = config.greenOffset.y;
    constants.blueOffset[0] = config.blueOffset.x;
    constants.blueOffset[1] = config.blueOffset.y;

    void* mapped = m_constantBuffer->Map();
    if (!mapped)
    {
        RVX_CORE_WARN("ChromaticAberration: failed to map constants");
        return false;
    }

    std::memcpy(mapped, &constants, sizeof(constants));
    m_constantBuffer->Unmap();
    return true;
}

void ChromaticAberrationPass::RefreshSupportState()
{
    if (m_config.useSpectral)
    {
        MarkUnsupported("ChromaticAberration spectral sampling is not implemented");
        return;
    }

    if (!m_pipelineCache || !m_pipelineCache->IsInitialized())
    {
        MarkUnsupported("ChromaticAberration requires an initialized PipelineCache");
        return;
    }

    if (!m_viewCache || !m_viewCache->IsInitialized())
    {
        MarkUnsupported("ChromaticAberration requires an initialized ResourceViewCache");
        return;
    }

    if (!m_pipelineCache->GetChromaticAberrationPipeline() ||
        !m_pipelineCache->GetPostProcessLayout() ||
        !m_pipelineCache->GetPostProcessSetLayout())
    {
        MarkUnsupported("ChromaticAberration fullscreen pipeline resources are not available");
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
