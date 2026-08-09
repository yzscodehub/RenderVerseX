/**
 * @file FXAA.cpp
 * @brief FXAAPass implementation
 */

#include "Render/PostProcess/FXAA.h"
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

    struct FXAAGPUConstants
    {
        float textureSize[2] = {1.0f, 1.0f};
        float invTextureSize[2] = {1.0f, 1.0f};
        float edgeThreshold = 0.166f;
        float edgeThresholdMin = 0.0833f;
        float subpixelQuality = 0.75f;
        float padding = 0.0f;
    };
} // namespace

FXAAPass::FXAAPass()
{
    m_enabled = true;
    MarkUnsupported("FXAA requires fullscreen pipeline resources before it can execute");
}

void FXAAPass::Configure(const PostProcessSettings& settings)
{
    m_enabled = settings.enableFXAA;
    m_subpixelQuality = settings.fxaaQuality;
}

void FXAAPass::SetResources(PipelineCache* pipelineCache, ResourceViewCache* viewCache)
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

    if (!m_pipelineCache || !m_pipelineCache->IsInitialized())
    {
        MarkUnsupported("FXAA requires an initialized PipelineCache");
        return;
    }

    if (!m_viewCache || !m_viewCache->IsInitialized())
    {
        MarkUnsupported("FXAA requires an initialized ResourceViewCache");
        return;
    }

    if (!m_pipelineCache->GetFXAAPipeline() ||
        !m_pipelineCache->GetPostProcessLayout() ||
        !m_pipelineCache->GetPostProcessSetLayout())
    {
        MarkUnsupported("FXAA fullscreen pipeline resources are not available");
        return;
    }

    if (!EnsureRuntimeResources())
    {
        return;
    }

    m_supported = true;
    m_unsupportedReason.clear();
}

void FXAAPass::AddToGraph(RenderGraph& graph, RGTextureHandle input, RGTextureHandle output)
{
    if (!IsEnabled())
    {
        if (IsRequestedEnabled() && !IsSupported())
        {
            RVX_CORE_WARN("FXAA: unsupported pass skipped: {}", GetUnsupportedReason());
        }
        return;
    }

    struct FXAAData
    {
        RGTextureHandle input;
        RGTextureHandle output;
        RGTextureViewHandle inputView;
        RGTextureViewHandle outputView;
        RHIFormat outputFormat = RHIFormat::Unknown;
        float edgeThreshold;
        float edgeThresholdMin;
        float subpixelQuality;
        FXAAQuality quality;
    };

    graph.AddPass<FXAAData>(
        "FXAA",
        RenderGraphPassType::Graphics,
        [this, input, output](RenderGraphBuilder& builder, FXAAData& data)
        {
            data.input = input;
            data.output = output;
            const RHITextureDesc* inputDesc = builder.GetTextureDesc(input);
            const RHITextureDesc* outputDesc = builder.GetTextureDesc(output);
            if (inputDesc)
            {
                RHITextureViewDesc viewDesc;
                viewDesc.format = inputDesc->format;
                viewDesc.dimension = inputDesc->dimension;
                viewDesc.subresourceRange = RHISubresourceRange::All();
                viewDesc.type = RHITextureViewType::ShaderResource;
                viewDesc.debugName = "FXAAInputSRV";
                data.inputView = builder.Read(
                    builder.CreateTextureView(input, viewDesc),
                    MakeRGAccessDesc(
                        RHIResourceState::ShaderResource,
                        RHIShaderStage::Pixel));
            }
            if (outputDesc)
            {
                data.outputFormat = outputDesc->format;
                RHITextureViewDesc viewDesc;
                viewDesc.format = outputDesc->format;
                viewDesc.dimension = outputDesc->dimension;
                viewDesc.subresourceRange = RHISubresourceRange::All();
                viewDesc.type = RHITextureViewType::RenderTarget;
                viewDesc.debugName = "FXAAOutputRTV";
                data.outputView = builder.Write(
                    builder.CreateTextureView(output, viewDesc),
                    MakeRGAccessDesc(
                        RHIResourceState::RenderTarget,
                        RHIShaderStage::Pixel,
                        RHIDiscardIntent::Discard));
            }
            data.edgeThreshold = m_edgeThreshold;
            data.edgeThresholdMin = m_edgeThresholdMin;
            data.subpixelQuality = m_subpixelQuality;
            data.quality = m_quality;
        },
        [this](const FXAAData& data, RenderGraphPassContext& context)
        {
            if (!m_pipelineCache)
            {
                RVX_CORE_WARN("FXAA: missing resources during execution");
                return;
            }

            RHIPipeline* pipeline = m_pipelineCache->GetFXAAPipeline(data.outputFormat);
            RHIDescriptorSetLayout* setLayout = m_pipelineCache->GetPostProcessSetLayout();
            IRHIDevice* device = m_pipelineCache->GetDevice();
            if (!pipeline || !setLayout || !device)
            {
                RVX_CORE_WARN("FXAA: fullscreen pipeline resources are unavailable");
                return;
            }

            RHITexture* inputTexture = context.GetTexture(data.input);
            RHITexture* outputTexture = context.GetTexture(data.output);
            if (!inputTexture || !outputTexture)
            {
                RVX_CORE_WARN("FXAA: input or output texture is unavailable");
                return;
            }

            RHITextureView* inputView = context.GetTextureView(data.inputView);
            RHITextureView* outputView = context.GetTextureView(data.outputView);
            if (!inputView || !outputView)
            {
                RVX_CORE_WARN("FXAA: failed to resolve input SRV or output RTV");
                return;
            }

            if (!EnsureRuntimeResources() ||
                !UpdateConstants(inputTexture->GetWidth(),
                                 inputTexture->GetHeight(),
                                 data.edgeThreshold,
                                 data.edgeThresholdMin,
                                 data.subpixelQuality))
            {
                return;
            }

            RHIDescriptorSetDesc descriptorDesc;
            descriptorDesc.layout = setLayout;
            descriptorDesc.debugName = "FXAADescriptorSet";
            descriptorDesc.BindBuffer(0,
                                      m_constantBuffer.Get(),
                                      0,
                                      AlignPostProcessConstantBufferSize(sizeof(FXAAGPUConstants)));
            descriptorDesc.BindTexture(1, inputView);
            descriptorDesc.BindSampler(2, m_sampler.Get());
            descriptorDesc.BindTexture(3, inputView);

            RHIDescriptorSetRef descriptorSet = device->CreateDescriptorSet(descriptorDesc);
            if (!descriptorSet)
            {
                RVX_CORE_WARN("FXAA: failed to create descriptor set");
                return;
            }
            if (!context.RetainSubmissionResource(
                    Ref<RefCounted>(descriptorSet)))
            {
                RVX_CORE_WARN("FXAA: submission ownership rejected descriptor set");
                return;
            }

            RHIRenderPassDesc renderPassDesc;
            renderPassDesc.AddColorAttachment(outputView, RHILoadOp::DontCare, RHIStoreOp::Store);
            renderPassDesc.SetRenderArea(0, 0, outputTexture->GetWidth(), outputTexture->GetHeight());

            RHICommandContext& ctx = context.Commands();
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

bool FXAAPass::EnsureRuntimeResources()
{
    IRHIDevice* device = m_pipelineCache ? m_pipelineCache->GetDevice() : nullptr;
    if (!device)
    {
        MarkUnsupported("FXAA requires an RHI device");
        return false;
    }

    if (!m_constantBuffer)
    {
        RHIBufferDesc bufferDesc;
        bufferDesc.size = AlignPostProcessConstantBufferSize(sizeof(FXAAGPUConstants));
        bufferDesc.usage = RHIBufferUsage::Constant;
        bufferDesc.memoryType = RHIMemoryType::Upload;
        bufferDesc.debugName = "FXAAConstants";

        m_constantBuffer = device->CreateBuffer(bufferDesc);
        if (!m_constantBuffer)
        {
            MarkUnsupported("FXAA constant buffer creation failed");
            return false;
        }
    }

    if (!m_sampler)
    {
        RHISamplerDesc samplerDesc = RHISamplerDesc::LinearClamp();
        samplerDesc.debugName = "FXAALinearClampSampler";
        m_sampler = device->CreateSampler(samplerDesc);
        if (!m_sampler)
        {
            MarkUnsupported("FXAA sampler creation failed");
            return false;
        }
    }

    return true;
}

bool FXAAPass::UpdateConstants(uint32 width,
                               uint32 height,
                               float edgeThreshold,
                               float edgeThresholdMin,
                               float subpixelQuality)
{
    if (!m_constantBuffer)
    {
        return false;
    }

    FXAAGPUConstants constants;
    constants.textureSize[0] = static_cast<float>(width);
    constants.textureSize[1] = static_cast<float>(height);
    constants.invTextureSize[0] = width > 0 ? 1.0f / static_cast<float>(width) : 1.0f;
    constants.invTextureSize[1] = height > 0 ? 1.0f / static_cast<float>(height) : 1.0f;
    constants.edgeThreshold = edgeThreshold;
    constants.edgeThresholdMin = edgeThresholdMin;
    constants.subpixelQuality = subpixelQuality;

    void* mapped = m_constantBuffer->Map();
    if (!mapped)
    {
        RVX_CORE_WARN("FXAA: failed to map constants");
        return false;
    }

    std::memcpy(mapped, &constants, sizeof(constants));
    m_constantBuffer->Unmap();
    return true;
}

} // namespace RVX
