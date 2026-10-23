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
        !m_pipelineCache->GetBloomLayout() ||
        !m_pipelineCache->GetBloomSetLayout())
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
    static_cast<void>(graph);
    static_cast<void>(input);
    static_cast<void>(output);
    m_lastExposureStateContentHash = 0;
    MarkUnsupported("Bloom requires the candidate exposure-state graph input");
}

void BloomPass::AddToGraph(RenderGraph& graph,
                           const PostProcessFrameInputs& frameInputs,
                           RGTextureHandle output)
{
    if (!frameInputs.HasExposureState() ||
        frameInputs.exposureStateContentHash == 0)
    {
        m_lastExposureStateContentHash = 0;
        MarkUnsupported("Bloom requires a non-zero candidate exposure-state identity");
        return;
    }
    AddToGraphWithExposure(graph,
                           frameInputs.sceneColor,
                           frameInputs.exposureState,
                           frameInputs.exposureStateContentHash,
                           output);
}

void BloomPass::AddToGraphWithExposure(RenderGraph& graph,
                                       RGTextureHandle input,
                                       RGBufferHandle exposureState,
                                       uint64 exposureStateContentHash,
                                       RGTextureHandle output)
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
                          exposureState,
                          exposureStateContentHash,
                           output,
                           PassMode::CopyScene,
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
                      exposureState,
                      exposureStateContentHash,
                      pyramid[0],
                      PassMode::Extract,
                       m_threshold,
                      1.0f,
                      m_radius);
    AddFullscreenPass(graph,
                      "BloomDownsample1",
                      pyramid[0],
                      exposureState,
                      exposureStateContentHash,
                      pyramid[1],
                      PassMode::Downsample,
                      0.0f,
                      1.0f,
                      m_radius);
    AddFullscreenPass(graph,
                      "BloomDownsample2",
                      pyramid[1],
                      exposureState,
                      exposureStateContentHash,
                      pyramid[2],
                      PassMode::Downsample,
                      0.0f,
                      1.0f,
                      m_radius);
    AddCompositePyramidPass(graph,
                            input,
                            pyramid,
                            exposureState,
                            exposureStateContentHash,
                            output);
}

void BloomPass::AddFullscreenPass(RenderGraph& graph,
                                  const char* passName,
                                  RGTextureHandle input,
                                  RGBufferHandle exposureState,
                                  uint64 exposureStateContentHash,
                                   RGTextureHandle output,
                                   PassMode mode,
                                   float threshold,
                                  float intensity,
                                  float radius)
{
    struct BloomData
    {
        RGTextureHandle input;
        RGBufferHandle exposureState;
        RGTextureHandle output;
        RGTextureViewHandle inputView;
        RGTextureViewHandle outputView;
        RHIFormat outputFormat = RHIFormat::Unknown;
        float threshold;
        float intensity;
        float radius;
        float softKnee;
        PassMode mode;
        uint64 exposureStateContentHash = 0;
    };

    graph.AddPass<BloomData>(
        passName,
        RenderGraphPassType::Graphics,
        [input, exposureState, exposureStateContentHash, output, mode,
         threshold, intensity, radius, this](
            RenderGraphBuilder& builder,
            BloomData& data)
        {
            data.input = input;
            data.exposureState = builder.Read(
                exposureState,
                MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                 RHIShaderStage::Pixel));
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
                viewDesc.debugName = "BloomInputSRV";
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
                viewDesc.debugName = "BloomOutputRTV";
                RGTextureViewHandle outputView =
                    builder.CreateTextureView(output, viewDesc);
                const RGAccessDesc outputAccess = MakeRGAccessDesc(
                    RHIResourceState::RenderTarget,
                    RHIShaderStage::Pixel,
                    RHIDiscardIntent::Discard);
                data.outputView = builder.Write(outputView, outputAccess);
            }
            data.threshold = threshold;
            data.intensity = intensity;
            data.radius = radius;
            data.softKnee = m_softKnee;
            data.mode = mode;
            data.exposureStateContentHash = exposureStateContentHash;
        },
        [this](const BloomData& data, RenderGraphPassContext& context)
        {
            if (!m_pipelineCache)
            {
                RVX_CORE_WARN("Bloom: missing resources during execution");
                context.MarkExecutionFailed("Bloom requires an initialized PipelineCache");
                return;
            }

            RHIPipeline* pipeline =
                m_pipelineCache->GetBloomPipeline(data.outputFormat);
            RHIDescriptorSetLayout* setLayout =
                m_pipelineCache->GetBloomSetLayout();
            IRHIDevice* device = m_pipelineCache->GetDevice();
            if (!pipeline || !setLayout || !device)
            {
                RVX_CORE_WARN("Bloom: fullscreen pipeline resources are unavailable");
                context.MarkExecutionFailed(
                    "Bloom fullscreen pipeline resources are unavailable");
                return;
            }

            RHITexture* inputTexture = context.GetTexture(data.input);
            RHITexture* outputTexture = context.GetTexture(data.output);
            RHIBuffer* exposureState = context.GetBuffer(data.exposureState);
            if (!inputTexture || !outputTexture || !exposureState ||
                data.exposureStateContentHash == 0)
            {
                RVX_CORE_WARN("Bloom: input or output texture is unavailable");
                context.MarkExecutionFailed("Bloom input or output texture is unavailable");
                return;
            }

            RHITextureView* inputView = context.GetTextureView(data.inputView);
            RHITextureView* outputView = context.GetTextureView(data.outputView);
            if (!inputView || !outputView)
            {
                RVX_CORE_WARN("Bloom: failed to resolve input SRV or output RTV");
                context.MarkExecutionFailed("Bloom failed to resolve input SRV or output RTV");
                return;
            }

            if (!EnsureRuntimeResources())
            {
                context.MarkExecutionFailed("Bloom runtime resources are unavailable");
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
                context.MarkExecutionFailed("Bloom failed to allocate immutable pass constants");
                return;
            }
            if (!context.RetainSubmissionResource(
                    Ref<RefCounted>(passConstants)))
            {
                RVX_CORE_WARN("Bloom: submission ownership rejected pass constants");
                context.MarkExecutionFailed(
                    "Bloom submission ownership rejected pass constants");
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
            descriptorDesc.BindTexture(4, inputView);
            descriptorDesc.BindTexture(5, inputView);
            descriptorDesc.BindBuffer(6, exposureState);

            RHIDescriptorSetRef descriptorSet = device->CreateDescriptorSet(descriptorDesc);
            if (!descriptorSet)
            {
                RVX_CORE_WARN("Bloom: failed to create descriptor set");
                context.MarkExecutionFailed("Bloom failed to create descriptor set");
                return;
            }
            if (!context.RetainSubmissionResource(
                    Ref<RefCounted>(descriptorSet)))
            {
                RVX_CORE_WARN("Bloom: submission ownership rejected descriptor set");
                context.MarkExecutionFailed(
                    "Bloom submission ownership rejected descriptor set");
                return;
            }

            RHIRenderPassDesc renderPassDesc;
            renderPassDesc.AddColorAttachment(
                outputView, RHILoadOp::DontCare, RHIStoreOp::Store);
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
            m_lastExposureStateContentHash = data.exposureStateContentHash;
        });
}

void BloomPass::AddCompositePyramidPass(
    RenderGraph& graph,
    RGTextureHandle source,
    const std::array<RGTextureHandle, 3>& pyramid,
    RGBufferHandle exposureState,
    uint64 exposureStateContentHash,
    RGTextureHandle output)
{
    struct BloomCompositeData
    {
        RGTextureHandle source{};
        std::array<RGTextureHandle, RVX_BLOOM_PYRAMID_LEVEL_COUNT> pyramid{};
        RGBufferHandle exposureState{};
        RGTextureHandle output{};
        RGTextureViewHandle sourceView{};
        std::array<RGTextureViewHandle, RVX_BLOOM_PYRAMID_LEVEL_COUNT>
            pyramidViews{};
        RGTextureViewHandle outputView{};
        RHIFormat outputFormat = RHIFormat::Unknown;
        uint64 exposureStateContentHash = 0;
    };

    graph.AddPass<BloomCompositeData>(
        "BloomCompositePyramid",
        RenderGraphPassType::Graphics,
        [source, pyramid, exposureState, exposureStateContentHash, output](
            RenderGraphBuilder& builder,
            BloomCompositeData& data)
        {
            data.source = source;
            data.pyramid = pyramid;
            data.exposureState = builder.Read(
                exposureState,
                MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                 RHIShaderStage::Pixel));
            data.output = output;

            const auto createReadView = [&builder](
                RGTextureHandle texture,
                const char* debugName) -> RGTextureViewHandle
            {
                const RHITextureDesc* const description =
                    builder.GetTextureDesc(texture);
                if (description == nullptr)
                {
                    return {};
                }

                RHITextureViewDesc viewDesc;
                viewDesc.format = description->format;
                viewDesc.dimension = description->dimension;
                viewDesc.subresourceRange = RHISubresourceRange::All();
                viewDesc.type = RHITextureViewType::ShaderResource;
                viewDesc.debugName = debugName;
                return builder.Read(
                    builder.CreateTextureView(texture, viewDesc),
                    MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                     RHIShaderStage::Pixel));
            };

            data.sourceView = createReadView(source, "BloomSourceSRV");
            for (uint32 level = 0; level < RVX_BLOOM_PYRAMID_LEVEL_COUNT;
                 ++level)
            {
                data.pyramidViews[level] = createReadView(
                    pyramid[level], "BloomPyramidSRV");
            }

            if (const RHITextureDesc* const outputDesc =
                    builder.GetTextureDesc(output))
            {
                data.outputFormat = outputDesc->format;
                RHITextureViewDesc outputViewDesc;
                outputViewDesc.format = outputDesc->format;
                outputViewDesc.dimension = outputDesc->dimension;
                outputViewDesc.subresourceRange = RHISubresourceRange::All();
                outputViewDesc.type = RHITextureViewType::RenderTarget;
                outputViewDesc.debugName = "BloomCompositePyramidRTV";
                data.outputView = builder.Write(
                    builder.CreateTextureView(output, outputViewDesc),
                    MakeRGAccessDesc(RHIResourceState::RenderTarget,
                                     RHIShaderStage::Pixel,
                                     RHIDiscardIntent::Discard));
            }
            data.exposureStateContentHash = exposureStateContentHash;
        },
        [this](const BloomCompositeData& data,
               RenderGraphPassContext& context)
        {
            if (m_pipelineCache == nullptr)
            {
                context.MarkExecutionFailed(
                    "Bloom requires an initialized PipelineCache");
                return;
            }

            RHIPipeline* const pipeline =
                m_pipelineCache->GetBloomPipeline(data.outputFormat);
            RHIDescriptorSetLayout* const setLayout =
                m_pipelineCache->GetBloomSetLayout();
            IRHIDevice* const device = m_pipelineCache->GetDevice();
            if (pipeline == nullptr || setLayout == nullptr || device == nullptr)
            {
                context.MarkExecutionFailed(
                    "Bloom composite fullscreen pipeline resources are unavailable");
                return;
            }

            RHITexture* const sourceTexture = context.GetTexture(data.source);
            RHITexture* const outputTexture = context.GetTexture(data.output);
            RHIBuffer* const candidateExposure =
                context.GetBuffer(data.exposureState);
            if (sourceTexture == nullptr || outputTexture == nullptr ||
                candidateExposure == nullptr ||
                data.exposureStateContentHash == 0)
            {
                context.MarkExecutionFailed(
                    "Bloom composite source, output, or exposure state is unavailable");
                return;
            }

            RHITextureView* const sourceView =
                context.GetTextureView(data.sourceView);
            RHITextureView* const outputView =
                context.GetTextureView(data.outputView);
            std::array<RHITextureView*, RVX_BLOOM_PYRAMID_LEVEL_COUNT>
                pyramidViews{};
            for (uint32 level = 0; level < RVX_BLOOM_PYRAMID_LEVEL_COUNT;
                 ++level)
            {
                pyramidViews[level] = context.GetTextureView(
                    data.pyramidViews[level]);
            }
            if (sourceView == nullptr || outputView == nullptr ||
                std::any_of(pyramidViews.begin(), pyramidViews.end(),
                            [](const RHITextureView* view)
                            {
                                return view == nullptr;
                            }))
            {
                context.MarkExecutionFailed(
                    "Bloom composite failed to resolve source or pyramid views");
                return;
            }

            if (!EnsureRuntimeResources())
            {
                context.MarkExecutionFailed(
                    "Bloom runtime resources are unavailable");
                return;
            }

            RHIBufferRef passConstants = CreatePassConstants(
                sourceTexture->GetWidth(),
                sourceTexture->GetHeight(),
                0.0f,
                m_intensity,
                m_radius,
                m_softKnee,
                PassMode::CompositePyramid);
            if (!passConstants ||
                !context.RetainSubmissionResource(
                    Ref<RefCounted>(passConstants)))
            {
                context.MarkExecutionFailed(
                    "Bloom composite failed to retain immutable pass constants");
                return;
            }

            RHIDescriptorSetDesc descriptorDesc;
            descriptorDesc.layout = setLayout;
            descriptorDesc.debugName = "BloomCompositePyramidDescriptorSet";
            descriptorDesc.BindBuffer(
                0,
                passConstants.Get(),
                0,
                AlignPostProcessConstantBufferSize(sizeof(BloomGPUConstants)));
            descriptorDesc.BindTexture(1, sourceView);
            descriptorDesc.BindSampler(2, m_sampler.Get());
            descriptorDesc.BindTexture(3, pyramidViews[0]);
            descriptorDesc.BindTexture(4, pyramidViews[1]);
            descriptorDesc.BindTexture(5, pyramidViews[2]);
            descriptorDesc.BindBuffer(6, candidateExposure);

            RHIDescriptorSetRef descriptorSet =
                device->CreateDescriptorSet(descriptorDesc);
            if (!descriptorSet ||
                !context.RetainSubmissionResource(
                    Ref<RefCounted>(descriptorSet)))
            {
                context.MarkExecutionFailed(
                    "Bloom composite failed to create or retain its descriptor set");
                return;
            }

            RHIRenderPassDesc renderPassDesc;
            renderPassDesc.AddColorAttachment(
                outputView, RHILoadOp::DontCare, RHIStoreOp::Store);
            renderPassDesc.SetRenderArea(
                0, 0, outputTexture->GetWidth(), outputTexture->GetHeight());

            RHICommandContext& commandContext = context.Commands();
            commandContext.BeginRenderPass(renderPassDesc);
            commandContext.SetPipeline(pipeline);
            commandContext.SetDescriptorSet(0, descriptorSet.Get());

            RHIViewport viewport;
            viewport.width = static_cast<float>(outputTexture->GetWidth());
            viewport.height = static_cast<float>(outputTexture->GetHeight());
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;
            commandContext.SetViewport(viewport);

            RHIRect scissor;
            scissor.width = outputTexture->GetWidth();
            scissor.height = outputTexture->GetHeight();
            commandContext.SetScissor(scissor);

            commandContext.Draw(3, 1, 0, 0);
            commandContext.EndRenderPass();
            m_lastExposureStateContentHash = data.exposureStateContentHash;
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
