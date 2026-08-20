/**
 * @file Vignette.cpp
 * @brief Vignette implementation
 */

#include "Render/PostProcess/Vignette.h"
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

    struct VignetteGPUConstants
    {
        float textureSize[2] = {1.0f, 1.0f};
        float invTextureSize[2] = {1.0f, 1.0f};
        float intensity = 0.4f;
        float smoothness = 0.5f;
        float roundness = 1.0f;
        float mode = 0.0f;
        float center[2] = {0.5f, 0.5f};
        float aspectRatio = 1.0f;
        float padding0 = 0.0f;
        float color[3] = {0.0f, 0.0f, 0.0f};
        float padding1 = 0.0f;
    };
} // namespace

VignettePass::VignettePass()
{
    m_enabled = false;  // Disabled by default
    MarkUnsupported("Vignette requires fullscreen pipeline resources before it can execute");
}

void VignettePass::Configure(const PostProcessSettings& settings)
{
    m_enabled = settings.enableVignette;
    m_config.intensity = settings.vignetteIntensity;
    m_config.smoothness = 1.0f - settings.vignetteRadius;  // Convert radius to smoothness
}

void VignettePass::SetResources(PipelineCache* pipelineCache, ResourceViewCache* viewCache)
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
        MarkUnsupported("Vignette requires an initialized PipelineCache");
        return;
    }

    if (!m_viewCache || !m_viewCache->IsInitialized())
    {
        MarkUnsupported("Vignette requires an initialized ResourceViewCache");
        return;
    }

    if (!m_pipelineCache->GetVignettePipeline() ||
        !m_pipelineCache->GetPostProcessLayout() ||
        !m_pipelineCache->GetPostProcessSetLayout())
    {
        MarkUnsupported("Vignette fullscreen pipeline resources are not available");
        return;
    }

    if (!EnsureRuntimeResources())
    {
        return;
    }

    m_supported = true;
    m_unsupportedReason.clear();
}

void VignettePass::AddToGraph(RenderGraph& graph, RGTextureHandle input, RGTextureHandle output)
{
    if (!IsEnabled())
    {
        if (IsRequestedEnabled() && !IsSupported())
        {
            RVX_CORE_WARN("Vignette: unsupported pass skipped: {}", GetUnsupportedReason());
        }
        return;
    }

    struct VignetteData
    {
        RGTextureHandle input;
        RGTextureHandle output;
        RGTextureViewHandle inputView;
        RGTextureViewHandle outputView;
        RHIFormat outputFormat = RHIFormat::Unknown;

        float intensity;
        float smoothness;
        float roundness;
        Vec2 center;
        Vec3 color;
        uint32 mode;
    };

    graph.AddPass<VignetteData>(
        "Vignette",
        RenderGraphPassType::Graphics,
        [this, input, output](RenderGraphBuilder& builder, VignetteData& data)
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
                viewDesc.debugName = "VignetteInputSRV";
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
                viewDesc.debugName = "VignetteOutputRTV";
                data.outputView = builder.Write(
                    builder.CreateTextureView(output, viewDesc),
                    MakeRGAccessDesc(
                        RHIResourceState::RenderTarget,
                        RHIShaderStage::Pixel,
                        RHIDiscardIntent::Discard));
            }

            data.intensity = m_config.intensity;
            data.smoothness = m_config.smoothness;
            data.roundness = m_config.roundness;
            data.center = m_config.center;
            data.color = m_config.color;
            data.mode = static_cast<uint32>(m_config.mode);
        },
        [this](const VignetteData& data, RenderGraphPassContext& context)
        {
            if (!m_pipelineCache)
            {
                RVX_CORE_WARN("Vignette: missing resources during execution");
                return;
            }

            RHIPipeline* pipeline = m_pipelineCache->GetVignettePipeline(data.outputFormat);
            RHIDescriptorSetLayout* setLayout = m_pipelineCache->GetPostProcessSetLayout();
            IRHIDevice* device = m_pipelineCache->GetDevice();
            if (!pipeline || !setLayout || !device)
            {
                RVX_CORE_WARN("Vignette: fullscreen pipeline resources are unavailable");
                return;
            }

            RHITexture* inputTexture = context.GetTexture(data.input);
            RHITexture* outputTexture = context.GetTexture(data.output);
            if (!inputTexture || !outputTexture)
            {
                RVX_CORE_WARN("Vignette: input or output texture is unavailable");
                return;
            }

            RHITextureView* inputView = context.GetTextureView(data.inputView);
            RHITextureView* outputView = context.GetTextureView(data.outputView);
            if (!inputView || !outputView)
            {
                RVX_CORE_WARN("Vignette: failed to resolve input SRV or output RTV");
                return;
            }

            if (!EnsureRuntimeResources() ||
                !UpdateConstants(inputTexture->GetWidth(),
                                 inputTexture->GetHeight(),
                                 data.intensity,
                                 data.smoothness,
                                 data.roundness,
                                 data.center,
                                 data.color,
                                 data.mode))
            {
                return;
            }

            RHIDescriptorSetDesc descriptorDesc;
            descriptorDesc.layout = setLayout;
            descriptorDesc.debugName = "VignetteDescriptorSet";
            descriptorDesc.BindBuffer(0,
                                      m_constantBuffer.Get(),
                                      0,
                                      AlignPostProcessConstantBufferSize(sizeof(VignetteGPUConstants)));
            descriptorDesc.BindTexture(1, inputView);
            descriptorDesc.BindSampler(2, m_sampler.Get());
            descriptorDesc.BindTexture(3, inputView);

            RHIDescriptorSetRef descriptorSet = device->CreateDescriptorSet(descriptorDesc);
            if (!descriptorSet)
            {
                RVX_CORE_WARN("Vignette: failed to create descriptor set");
                return;
            }
            if (!context.RetainSubmissionResource(
                    Ref<RefCounted>(descriptorSet)))
            {
                RVX_CORE_WARN("Vignette: submission ownership rejected descriptor set");
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

bool VignettePass::EnsureRuntimeResources()
{
    IRHIDevice* device = m_pipelineCache ? m_pipelineCache->GetDevice() : nullptr;
    if (!device)
    {
        MarkUnsupported("Vignette requires an RHI device");
        return false;
    }

    if (!m_constantBuffer)
    {
        RHIBufferDesc bufferDesc;
        bufferDesc.size = AlignPostProcessConstantBufferSize(sizeof(VignetteGPUConstants));
        bufferDesc.usage = RHIBufferUsage::Constant;
        bufferDesc.memoryType = RHIMemoryType::Upload;
        bufferDesc.debugName = "VignetteConstants";

        m_constantBuffer = device->CreateBuffer(bufferDesc);
        if (!m_constantBuffer)
        {
            MarkUnsupported("Vignette constant buffer creation failed");
            return false;
        }
    }

    if (!m_sampler)
    {
        RHISamplerDesc samplerDesc = RHISamplerDesc::LinearClamp();
        samplerDesc.debugName = "VignetteLinearClampSampler";
        m_sampler = device->CreateSampler(samplerDesc);
        if (!m_sampler)
        {
            MarkUnsupported("Vignette sampler creation failed");
            return false;
        }
    }

    return true;
}

bool VignettePass::UpdateConstants(uint32 width,
                                   uint32 height,
                                   float intensity,
                                   float smoothness,
                                   float roundness,
                                   const Vec2& center,
                                   const Vec3& color,
                                   uint32 mode)
{
    if (!m_constantBuffer)
    {
        return false;
    }

    VignetteGPUConstants constants;
    constants.textureSize[0] = static_cast<float>(width);
    constants.textureSize[1] = static_cast<float>(height);
    constants.invTextureSize[0] = width > 0 ? 1.0f / static_cast<float>(width) : 1.0f;
    constants.invTextureSize[1] = height > 0 ? 1.0f / static_cast<float>(height) : 1.0f;
    constants.intensity = intensity;
    constants.smoothness = smoothness;
    constants.roundness = roundness;
    constants.mode = static_cast<float>(mode);
    constants.center[0] = center.x;
    constants.center[1] = center.y;
    constants.aspectRatio = height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
    constants.color[0] = color.x;
    constants.color[1] = color.y;
    constants.color[2] = color.z;

    void* mapped = m_constantBuffer->Map();
    if (!mapped)
    {
        RVX_CORE_WARN("Vignette: failed to map constants");
        return false;
    }

    std::memcpy(mapped, &constants, sizeof(constants));
    m_constantBuffer->Unmap();
    return true;
}

} // namespace RVX
