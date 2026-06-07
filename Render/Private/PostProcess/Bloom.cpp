/**
 * @file Bloom.cpp
 * @brief BloomPass implementation
 */

#include "Render/PostProcess/Bloom.h"
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

    struct BloomGPUConstants
    {
        float threshold = 1.0f;
        float softKnee = 0.5f;
        float intensity = 1.0f;
        float radius = 0.5f;
        float textureSize[2] = {1.0f, 1.0f};
        float invTextureSize[2] = {1.0f, 1.0f};
    };
} // namespace

BloomPass::BloomPass()
{
    m_enabled = true;
    MarkUnsupported("Bloom requires fullscreen pipeline resources before it can execute");
}

void BloomPass::Configure(const PostProcessSettings& settings)
{
    m_enabled = settings.enableBloom;
    m_threshold = settings.bloomThreshold;
    m_intensity = settings.bloomIntensity;
    m_radius = settings.bloomRadius;
}

void BloomPass::SetResources(PipelineCache* pipelineCache, ResourceViewCache* viewCache)
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
        MarkUnsupported("Bloom requires an initialized PipelineCache");
        return;
    }

    if (!m_viewCache || !m_viewCache->IsInitialized())
    {
        MarkUnsupported("Bloom requires an initialized ResourceViewCache");
        return;
    }

    if (!m_pipelineCache->GetBloomPipeline() ||
        !m_pipelineCache->GetPostProcessLayout() ||
        !m_pipelineCache->GetPostProcessSetLayout())
    {
        MarkUnsupported("Bloom fullscreen pipeline resources are not available");
        return;
    }

    if (!EnsureRuntimeResources())
    {
        return;
    }

    m_supported = true;
    m_unsupportedReason.clear();
}

void BloomPass::AddToGraph(RenderGraph& graph, RGTextureHandle input, RGTextureHandle output)
{
    if (!IsEnabled())
    {
        if (IsRequestedEnabled() && !IsSupported())
        {
            RVX_CORE_WARN("Bloom: unsupported pass skipped: {}", GetUnsupportedReason());
        }
        return;
    }

    struct BloomData
    {
        RGTextureHandle input;
        RGTextureHandle output;
        float threshold;
        float intensity;
        float radius;
        float softKnee;
    };

    graph.AddPass<BloomData>(
        "Bloom",
        RenderGraphPassType::Graphics,
        [this, input, output](RenderGraphBuilder& builder, BloomData& data)
        {
            data.input = builder.Read(input, RHIShaderStage::Pixel);
            data.output = builder.Write(output, RHIResourceState::RenderTarget);
            data.threshold = m_threshold;
            data.intensity = m_intensity;
            data.radius = m_radius;
            data.softKnee = m_softKnee;
        },
        [this, &graph](const BloomData& data, RHICommandContext& ctx)
        {
            if (!m_pipelineCache || !m_viewCache)
            {
                RVX_CORE_WARN("Bloom: missing resources during execution");
                return;
            }

            RHIPipeline* pipeline = m_pipelineCache->GetBloomPipeline();
            RHIDescriptorSetLayout* setLayout = m_pipelineCache->GetPostProcessSetLayout();
            IRHIDevice* device = m_pipelineCache->GetDevice();
            if (!pipeline || !setLayout || !device)
            {
                RVX_CORE_WARN("Bloom: fullscreen pipeline resources are unavailable");
                return;
            }

            RHITexture* inputTexture = graph.GetTexture(data.input);
            RHITexture* outputTexture = graph.GetTexture(data.output);
            if (!inputTexture || !outputTexture)
            {
                RVX_CORE_WARN("Bloom: input or output texture is unavailable");
                return;
            }

            RHITextureView* inputView = m_viewCache->GetDefaultSRV(inputTexture);
            RHITextureView* outputView = m_viewCache->GetDefaultRTV(outputTexture);
            if (!inputView || !outputView)
            {
                RVX_CORE_WARN("Bloom: failed to resolve input SRV or output RTV");
                return;
            }

            const bool constantsReady = EnsureRuntimeResources() &&
                                        UpdateConstants(inputTexture->GetWidth(),
                                                        inputTexture->GetHeight(),
                                                        data.threshold,
                                                        data.intensity,
                                                        data.radius,
                                                        data.softKnee);
            if (!constantsReady)
            {
                return;
            }

            RHIDescriptorSetDesc descriptorDesc;
            descriptorDesc.layout = setLayout;
            descriptorDesc.debugName = "BloomDescriptorSet";
            descriptorDesc.BindBuffer(0,
                                      m_constantBuffer.Get(),
                                      0,
                                      AlignPostProcessConstantBufferSize(sizeof(BloomGPUConstants)));
            descriptorDesc.BindTexture(1, inputView);
            descriptorDesc.BindSampler(2, m_sampler.Get());

            RHIDescriptorSetRef descriptorSet = device->CreateDescriptorSet(descriptorDesc);
            if (!descriptorSet)
            {
                RVX_CORE_WARN("Bloom: failed to create descriptor set");
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

bool BloomPass::EnsureRuntimeResources()
{
    IRHIDevice* device = m_pipelineCache ? m_pipelineCache->GetDevice() : nullptr;
    if (!device)
    {
        MarkUnsupported("Bloom requires an RHI device");
        return false;
    }

    if (!m_constantBuffer)
    {
        RHIBufferDesc bufferDesc;
        bufferDesc.size = AlignPostProcessConstantBufferSize(sizeof(BloomGPUConstants));
        bufferDesc.usage = RHIBufferUsage::Constant;
        bufferDesc.memoryType = RHIMemoryType::Upload;
        bufferDesc.debugName = "BloomConstants";

        m_constantBuffer = device->CreateBuffer(bufferDesc);
        if (!m_constantBuffer)
        {
            MarkUnsupported("Bloom constant buffer creation failed");
            return false;
        }
    }

    if (!m_sampler)
    {
        RHISamplerDesc samplerDesc = RHISamplerDesc::LinearClamp();
        samplerDesc.debugName = "BloomLinearClampSampler";
        m_sampler = device->CreateSampler(samplerDesc);
        if (!m_sampler)
        {
            MarkUnsupported("Bloom sampler creation failed");
            return false;
        }
    }

    return true;
}

bool BloomPass::UpdateConstants(uint32 width,
                                uint32 height,
                                float threshold,
                                float intensity,
                                float radius,
                                float softKnee)
{
    if (!m_constantBuffer)
        return false;

    const float safeWidth = width > 0 ? static_cast<float>(width) : 1.0f;
    const float safeHeight = height > 0 ? static_cast<float>(height) : 1.0f;

    BloomGPUConstants constants;
    constants.threshold = threshold;
    constants.softKnee = softKnee;
    constants.intensity = intensity;
    constants.radius = radius;
    constants.textureSize[0] = safeWidth;
    constants.textureSize[1] = safeHeight;
    constants.invTextureSize[0] = 1.0f / safeWidth;
    constants.invTextureSize[1] = 1.0f / safeHeight;

    void* mapped = m_constantBuffer->Map();
    if (!mapped)
    {
        RVX_CORE_WARN("Bloom: failed to map constants buffer");
        return false;
    }

    std::memcpy(mapped, &constants, sizeof(constants));
    m_constantBuffer->Unmap();
    return true;
}

} // namespace RVX
