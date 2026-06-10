/**
 * @file ToneMapping.cpp
 * @brief ToneMappingPass implementation
 */

#include "Render/PostProcess/ToneMapping.h"
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

    struct ToneMappingGPUConstants
    {
        float exposure = 1.0f;
        float gamma = 2.2f;
        float whitePoint = 11.2f;
        uint32 operatorType = 0;
        float textureSize[2] = {1.0f, 1.0f};
        float invTextureSize[2] = {1.0f, 1.0f};
    };
} // namespace

ToneMappingPass::ToneMappingPass()
{
    m_enabled = true;
    MarkUnsupported("ToneMapping shader and fullscreen pipeline are not implemented");
}

void ToneMappingPass::Configure(const PostProcessSettings& settings)
{
    m_enabled = settings.enableToneMapping;
    m_exposure = settings.exposure;
    m_gamma = settings.gamma;
    m_operator = settings.toneMappingOperator;
}

void ToneMappingPass::SetResources(PipelineCache* pipelineCache, ResourceViewCache* viewCache)
{
    m_pipelineCache = pipelineCache;
    m_viewCache = viewCache;

    IRHIDevice* device = m_pipelineCache ? m_pipelineCache->GetDevice() : nullptr;
    if (device != m_resourceDevice)
    {
        m_retainedDescriptorSets.clear();
        m_constantBuffer.Reset();
        m_sampler.Reset();
        m_resourceDevice = device;
    }

    if (!m_pipelineCache || !m_pipelineCache->IsInitialized())
    {
        MarkUnsupported("ToneMapping requires an initialized PipelineCache");
        return;
    }

    if (!m_viewCache || !m_viewCache->IsInitialized())
    {
        MarkUnsupported("ToneMapping requires an initialized ResourceViewCache");
        return;
    }

    if (!m_pipelineCache->GetToneMappingPipeline() ||
        !m_pipelineCache->GetPostProcessLayout() ||
        !m_pipelineCache->GetPostProcessSetLayout())
    {
        MarkUnsupported("ToneMapping fullscreen pipeline resources are not available");
        return;
    }

    if (!EnsureRuntimeResources())
    {
        return;
    }

    m_supported = true;
    m_unsupportedReason.clear();
}

void ToneMappingPass::AddToGraph(RenderGraph& graph, RGTextureHandle input, RGTextureHandle output)
{
    if (!IsEnabled())
    {
        if (IsRequestedEnabled() && !IsSupported())
        {
            RVX_CORE_WARN("ToneMapping: unsupported pass skipped: {}", GetUnsupportedReason());
        }
        return;
    }

    struct ToneMappingData
    {
        RGTextureHandle input;
        RGTextureHandle output;
        ToneMappingOperator op;
        float exposure;
        float gamma;
        float whitePoint;
    };

    graph.AddPass<ToneMappingData>(
        "ToneMapping",
        RenderGraphPassType::Graphics,
        [this, input, output](RenderGraphBuilder& builder, ToneMappingData& data)
        {
            data.input = builder.Read(input, RHIShaderStage::Pixel);
            data.output = builder.Write(output, RHIResourceState::RenderTarget);
            data.op = m_operator;
            data.exposure = m_exposure;
            data.gamma = m_gamma;
            data.whitePoint = m_whitePoint;
        },
        [this, &graph](const ToneMappingData& data, RHICommandContext& ctx)
        {
            if (!m_pipelineCache || !m_viewCache)
            {
                RVX_CORE_WARN("ToneMapping: missing resources during execution");
                return;
            }

            RHIFormat outputFormat = RHIFormat::Unknown;
            if (const RHITextureDesc* outputDesc = graph.GetTextureDesc(data.output))
            {
                outputFormat = outputDesc->format;
            }

            RHIPipeline* pipeline = m_pipelineCache->GetToneMappingPipeline(outputFormat);
            RHIDescriptorSetLayout* setLayout = m_pipelineCache->GetPostProcessSetLayout();
            IRHIDevice* device = m_pipelineCache->GetDevice();
            if (!pipeline || !setLayout || !device)
            {
                RVX_CORE_WARN("ToneMapping: fullscreen pipeline resources are unavailable");
                return;
            }

            RHITexture* inputTexture = graph.GetTexture(data.input);
            RHITexture* outputTexture = graph.GetTexture(data.output);
            if (!inputTexture || !outputTexture)
            {
                RVX_CORE_WARN("ToneMapping: input or output texture is unavailable");
                return;
            }

            RHITextureView* inputView = m_viewCache->GetDefaultSRV(inputTexture);
            RHITextureView* outputView = m_viewCache->GetDefaultRTV(outputTexture);
            if (!inputView || !outputView)
            {
                RVX_CORE_WARN("ToneMapping: failed to resolve input SRV or output RTV");
                return;
            }

            if (!EnsureRuntimeResources() ||
                !UpdateConstants(inputTexture->GetWidth(),
                                 inputTexture->GetHeight(),
                                 data.op,
                                 data.exposure,
                                 data.gamma,
                                 data.whitePoint))
            {
                return;
            }

            RHIDescriptorSetDesc descriptorDesc;
            descriptorDesc.layout = setLayout;
            descriptorDesc.debugName = "ToneMappingDescriptorSet";
            descriptorDesc.BindBuffer(0,
                                      m_constantBuffer.Get(),
                                      0,
                                      AlignPostProcessConstantBufferSize(sizeof(ToneMappingGPUConstants)));
            descriptorDesc.BindTexture(1, inputView);
            descriptorDesc.BindSampler(2, m_sampler.Get());

            RHIDescriptorSetRef descriptorSet = device->CreateDescriptorSet(descriptorDesc);
            if (!descriptorSet)
            {
                RVX_CORE_WARN("ToneMapping: failed to create descriptor set");
                return;
            }
            m_retainedDescriptorSets.push_back(descriptorSet);
            while (m_retainedDescriptorSets.size() > RVX_MAX_FRAME_COUNT + 1)
            {
                m_retainedDescriptorSets.pop_front();
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

bool ToneMappingPass::EnsureRuntimeResources()
{
    IRHIDevice* device = m_pipelineCache ? m_pipelineCache->GetDevice() : nullptr;
    if (!device)
    {
        MarkUnsupported("ToneMapping requires an RHI device");
        return false;
    }

    if (!m_constantBuffer)
    {
        RHIBufferDesc bufferDesc;
        bufferDesc.size = AlignPostProcessConstantBufferSize(sizeof(ToneMappingGPUConstants));
        bufferDesc.usage = RHIBufferUsage::Constant;
        bufferDesc.memoryType = RHIMemoryType::Upload;
        bufferDesc.debugName = "ToneMappingConstants";

        m_constantBuffer = device->CreateBuffer(bufferDesc);
        if (!m_constantBuffer)
        {
            MarkUnsupported("ToneMapping constant buffer creation failed");
            return false;
        }
    }

    if (!m_sampler)
    {
        RHISamplerDesc samplerDesc = RHISamplerDesc::LinearClamp();
        samplerDesc.debugName = "ToneMappingLinearClampSampler";
        m_sampler = device->CreateSampler(samplerDesc);
        if (!m_sampler)
        {
            MarkUnsupported("ToneMapping sampler creation failed");
            return false;
        }
    }

    return true;
}

bool ToneMappingPass::UpdateConstants(uint32 width,
                                      uint32 height,
                                      ToneMappingOperator op,
                                      float exposure,
                                      float gamma,
                                      float whitePoint)
{
    if (!m_constantBuffer)
        return false;

    const float safeWidth = width > 0 ? static_cast<float>(width) : 1.0f;
    const float safeHeight = height > 0 ? static_cast<float>(height) : 1.0f;

    ToneMappingGPUConstants constants;
    constants.exposure = exposure;
    constants.gamma = gamma;
    constants.whitePoint = whitePoint;
    constants.operatorType = static_cast<uint32>(op);
    constants.textureSize[0] = safeWidth;
    constants.textureSize[1] = safeHeight;
    constants.invTextureSize[0] = 1.0f / safeWidth;
    constants.invTextureSize[1] = 1.0f / safeHeight;

    void* mapped = m_constantBuffer->Map();
    if (!mapped)
    {
        RVX_CORE_WARN("ToneMapping: failed to map constants buffer");
        return false;
    }

    std::memcpy(mapped, &constants, sizeof(constants));
    m_constantBuffer->Unmap();
    return true;
}

} // namespace RVX
