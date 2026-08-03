/**
 * @file Bloom.cpp
 * @brief BloomPass implementation
 */

#include "Render/PostProcess/Bloom.h"
#include "Core/Log.h"
#include "Render/Graph/ResourceViewCache.h"
#include "Render/PipelineCache.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace RVX
{

namespace
{
    constexpr uint64 RVX_POST_PROCESS_CONSTANT_BUFFER_ALIGNMENT = 256;
    constexpr uint32 RVX_BLOOM_PYRAMID_LEVEL_COUNT = 3;

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
        float mode = 0.0f;
        float padding[3] = {0.0f, 0.0f, 0.0f};
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
        !m_pipelineCache->GetBloomAdditivePipeline(m_pipelineCache->GetPostProcessIntermediateFormat()) ||
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

    if (m_intensity <= 0.0f)
    {
        AddFullscreenPass(graph,
                          "BloomCopyScene",
                          input,
                          output,
                          PassMode::CopyScene,
                          false,
                          RHILoadOp::DontCare,
                          0.0f,
                          0.0f,
                          0.0f);
        return;
    }

    const RHITextureDesc* inputDesc = graph.GetTextureDesc(input);
    if (!inputDesc)
    {
        RVX_CORE_WARN("Bloom: input texture description is unavailable");
        return;
    }

    std::array<RGTextureHandle, RVX_BLOOM_PYRAMID_LEVEL_COUNT> pyramid{};
    const RHIFormat pyramidFormat = inputDesc->format;
    uint32 levelWidth = std::max(1u, inputDesc->width / 2u);
    uint32 levelHeight = std::max(1u, inputDesc->height / 2u);
    for (uint32 level = 0; level < RVX_BLOOM_PYRAMID_LEVEL_COUNT; ++level)
    {
        RHITextureDesc levelDesc = RHITextureDesc::RenderTarget(levelWidth, levelHeight, pyramidFormat);
        levelDesc.debugName = "BloomPyramidLevel";
        pyramid[level] = graph.CreateTexture(levelDesc);

        levelWidth = std::max(1u, levelWidth / 2u);
        levelHeight = std::max(1u, levelHeight / 2u);
    }

    AddFullscreenPass(graph,
                      "BloomExtract",
                      input,
                      pyramid[0],
                      PassMode::Extract,
                      false,
                      RHILoadOp::DontCare,
                      m_threshold,
                      1.0f,
                      m_radius);
    AddFullscreenPass(graph,
                      "BloomDownsample1",
                      pyramid[0],
                      pyramid[1],
                      PassMode::Downsample,
                      false,
                      RHILoadOp::DontCare,
                      0.0f,
                      1.0f,
                      m_radius);
    AddFullscreenPass(graph,
                      "BloomDownsample2",
                      pyramid[1],
                      pyramid[2],
                      PassMode::Downsample,
                      false,
                      RHILoadOp::DontCare,
                      0.0f,
                      1.0f,
                      m_radius);
    AddFullscreenPass(graph,
                      "BloomCopyScene",
                      input,
                      output,
                      PassMode::CopyScene,
                      false,
                      RHILoadOp::DontCare,
                      0.0f,
                      0.0f,
                      0.0f);

    constexpr float compositeWeights[RVX_BLOOM_PYRAMID_LEVEL_COUNT] = {0.45f, 0.35f, 0.20f};
    AddFullscreenPass(graph,
                      "BloomComposite2",
                      pyramid[2],
                      output,
                      PassMode::CompositeAdditive,
                      true,
                      RHILoadOp::Load,
                      0.0f,
                      m_intensity * compositeWeights[2],
                      m_radius);
    AddFullscreenPass(graph,
                      "BloomComposite1",
                      pyramid[1],
                      output,
                      PassMode::CompositeAdditive,
                      true,
                      RHILoadOp::Load,
                      0.0f,
                      m_intensity * compositeWeights[1],
                      m_radius);
    AddFullscreenPass(graph,
                      "BloomComposite0",
                      pyramid[0],
                      output,
                      PassMode::CompositeAdditive,
                      true,
                      RHILoadOp::Load,
                      0.0f,
                      m_intensity * compositeWeights[0],
                      m_radius);
}

void BloomPass::AddFullscreenPass(RenderGraph& graph,
                                  const char* passName,
                                  RGTextureHandle input,
                                  RGTextureHandle output,
                                  PassMode mode,
                                  bool additive,
                                  RHILoadOp outputLoadOp,
                                  float threshold,
                                  float intensity,
                                  float radius)
{
    struct BloomData
    {
        RGTextureHandle input;
        RGTextureHandle output;
        float threshold;
        float intensity;
        float radius;
        float softKnee;
        PassMode mode;
        bool additive;
        RHILoadOp outputLoadOp;
    };

    graph.AddPass<BloomData>(
        passName,
        RenderGraphPassType::Graphics,
        [input, output, mode, additive, outputLoadOp, threshold, intensity, radius, this](
            RenderGraphBuilder& builder,
            BloomData& data)
        {
            data.input = builder.Read(input, RHIShaderStage::Pixel);
            if (additive)
            {
                data.output = builder.ReadWrite(
                    output,
                    MakeRHIAccessSnapshot(RHIResourceState::RenderTarget,
                                           RHIShaderStage::Pixel));
            }
            else
            {
                data.output = builder.Write(
                    output,
                    RHIResourceState::RenderTarget);
            }
            data.threshold = threshold;
            data.intensity = intensity;
            data.radius = radius;
            data.softKnee = m_softKnee;
            data.mode = mode;
            data.additive = additive;
            data.outputLoadOp = outputLoadOp;
        },
        [this, &graph](const BloomData& data, RHICommandContext& ctx)
        {
            if (!m_pipelineCache || !m_viewCache)
            {
                RVX_CORE_WARN("Bloom: missing resources during execution");
                return;
            }

            RHIFormat outputFormat = RHIFormat::Unknown;
            if (const RHITextureDesc* outputDesc = graph.GetTextureDesc(data.output))
            {
                outputFormat = outputDesc->format;
            }

            RHIPipeline* pipeline = data.additive ?
                m_pipelineCache->GetBloomAdditivePipeline(outputFormat) :
                m_pipelineCache->GetBloomPipeline(outputFormat);
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

            if (!EnsureRuntimeResources())
            {
                return;
            }

            // Every recorded draw needs immutable constants until its GPU submission
            // completes. Reusing one mapped buffer here lets later Bloom passes overwrite
            // constants that earlier command-list entries still reference.
            RHIBufferRef passConstants = CreatePassConstants(inputTexture->GetWidth(),
                                                             inputTexture->GetHeight(),
                                                             data.threshold,
                                                             data.intensity,
                                                             data.radius,
                                                             data.softKnee,
                                                             data.mode);
            if (!passConstants)
            {
                return;
            }
            if (!RetainSubmissionResource(
                    passConstants,
                    AlignPostProcessConstantBufferSize(sizeof(BloomGPUConstants))))
            {
                RVX_CORE_WARN("Bloom: submission ownership rejected pass constants");
                return;
            }

            RHIDescriptorSetDesc descriptorDesc;
            descriptorDesc.layout = setLayout;
            descriptorDesc.debugName = "BloomDescriptorSet";
            descriptorDesc.BindBuffer(0,
                                      passConstants.Get(),
                                      0,
                                      AlignPostProcessConstantBufferSize(sizeof(BloomGPUConstants)));
            descriptorDesc.BindTexture(1, inputView);
            descriptorDesc.BindSampler(2, m_sampler.Get());
            descriptorDesc.BindTexture(3, inputView);

            RHIDescriptorSetRef descriptorSet = device->CreateDescriptorSet(descriptorDesc);
            if (!descriptorSet)
            {
                RVX_CORE_WARN("Bloom: failed to create descriptor set");
                return;
            }
            if (!RetainSubmissionResource(descriptorSet))
            {
                RVX_CORE_WARN("Bloom: submission ownership rejected descriptor set");
                return;
            }

            RHIRenderPassDesc renderPassDesc;
            renderPassDesc.AddColorAttachment(outputView, data.outputLoadOp, RHIStoreOp::Store);
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

RHIBufferRef BloomPass::CreatePassConstants(uint32 width,
                                            uint32 height,
                                            float threshold,
                                            float intensity,
                                            float radius,
                                            float softKnee,
                                            PassMode mode) const
{
    IRHIDevice* device = m_pipelineCache ? m_pipelineCache->GetDevice() : nullptr;
    if (!device)
    {
        return {};
    }

    RHIBufferDesc bufferDesc;
    bufferDesc.size = AlignPostProcessConstantBufferSize(sizeof(BloomGPUConstants));
    bufferDesc.usage = RHIBufferUsage::Constant;
    bufferDesc.memoryType = RHIMemoryType::Upload;
    bufferDesc.debugName = "BloomPassConstants";

    RHIBufferRef constantBuffer = device->CreateBuffer(bufferDesc);
    if (!constantBuffer)
    {
        RVX_CORE_WARN("Bloom: pass constant buffer creation failed");
        return {};
    }

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
    constants.mode = static_cast<float>(static_cast<uint32>(mode));

    void* mapped = constantBuffer->Map();
    if (!mapped)
    {
        RVX_CORE_WARN("Bloom: failed to map pass constants buffer");
        return {};
    }

    std::memcpy(mapped, &constants, sizeof(constants));
    constantBuffer->Unmap();
    return constantBuffer;
}

} // namespace RVX
