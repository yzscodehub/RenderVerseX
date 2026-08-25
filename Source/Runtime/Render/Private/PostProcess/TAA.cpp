/**
 * @file TAA.cpp
 * @brief Graph-backed temporal anti-aliasing implementation.
 */

#include "Render/PostProcess/TAA.h"

#include "Core/Log.h"
#include "Render/Features/RenderGraphBlackboard.h"
#include "Render/Graph/ResourceViewCache.h"
#include "Render/PipelineCache.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

namespace RVX
{
namespace
{
    constexpr uint64 RVX_TAA_CONSTANT_BUFFER_ALIGNMENT = 256;

    struct TAAGPUConstants
    {
        float renderSize[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        float feedbackParams[4] = {0.88f, 0.97f, 1.0f, 1000.0f};
        float resolveParams[4] = {0.01f, 1.0f, 0.25f, 0.0f};
    };

    struct TAAFrameRecordState
    {
        bool resolveExecuted = false;
    };

    [[nodiscard]] uint64 AlignConstantBufferSize(uint64 size) noexcept
    {
        return (size + RVX_TAA_CONSTANT_BUFFER_ALIGNMENT - 1) &
               ~(RVX_TAA_CONSTANT_BUFFER_ALIGNMENT - 1);
    }

    /**
     * @brief Creates immutable upload constants owned by the recorded submission.
     *
     * A TAA resolve and its history-store pass may overlap with a later frame.
     * The constants therefore cannot be an owner-level upload buffer that each
     * frame maps again. Callers retain the returned buffer through the graph
     * submission before binding it into their descriptor set.
     */
    [[nodiscard]] RHIBufferRef CreateImmutablePassConstants(
        IRHIDevice& device,
        const TAAGPUConstants& constants,
        const char* debugName)
    {
        RHIBufferDesc bufferDesc;
        bufferDesc.size = AlignConstantBufferSize(sizeof(TAAGPUConstants));
        bufferDesc.usage = RHIBufferUsage::Constant;
        bufferDesc.memoryType = RHIMemoryType::Upload;
        bufferDesc.debugName = debugName;

        RHIBufferRef constantBuffer = device.CreateBuffer(bufferDesc);
        if (!constantBuffer)
        {
            return {};
        }

        void* const mapped = constantBuffer->Map();
        if (mapped == nullptr)
        {
            return {};
        }

        std::memcpy(mapped, &constants, sizeof(constants));
        constantBuffer->Unmap();
        return constantBuffer;
    }

    [[nodiscard]] bool IsFiniteNonNegative(float value) noexcept
    {
        return std::isfinite(value) && value >= 0.0f;
    }

    /**
     * The current and immediately preceding raster samples must use distinct
     * deterministic jitter.  A repeated (especially all-zero) pair proves
     * that temporal history would be sampled without the engine's fixed
     * temporal sequence, so refuse that sampled-history path rather than
     * turning it into a spatial blend that happens to be named TAA.
     */
    [[nodiscard]] bool HasDistinctFiniteTemporalJitter(
        const Vec2& current,
        const Vec2& previous) noexcept
    {
        if (!std::isfinite(current.x) || !std::isfinite(current.y) ||
            !std::isfinite(previous.x) || !std::isfinite(previous.y))
        {
            return false;
        }

        constexpr float RVX_TAA_JITTER_EPSILON = 1.0e-6f;
        return std::abs(current.x - previous.x) > RVX_TAA_JITTER_EPSILON ||
               std::abs(current.y - previous.y) > RVX_TAA_JITTER_EPSILON;
    }

    [[nodiscard]] RGTextureViewHandle AddTextureSRV(
        RenderGraphBuilder& builder,
        RGTextureHandle texture,
        const char* debugName)
    {
        const RHITextureDesc* const desc = builder.GetTextureDesc(texture);
        if (desc == nullptr)
        {
            return {};
        }

        RHITextureViewDesc viewDesc;
        viewDesc.format = desc->format;
        viewDesc.dimension = desc->dimension;
        viewDesc.subresourceRange = RHISubresourceRange::All();
        if (IsDepthFormat(viewDesc.format))
        {
            viewDesc.subresourceRange.aspect = RHITextureAspect::Depth;
        }
        viewDesc.type = RHITextureViewType::ShaderResource;
        viewDesc.debugName = debugName;
        return builder.Read(
            builder.CreateTextureView(texture, viewDesc),
            MakeRGAccessDesc(RHIResourceState::ShaderResource,
                             RHIShaderStage::Pixel));
    }

    [[nodiscard]] RGTextureViewHandle AddTextureRTV(
        RenderGraphBuilder& builder,
        RGTextureHandle texture,
        const char* debugName)
    {
        const RHITextureDesc* const desc = builder.GetTextureDesc(texture);
        if (desc == nullptr)
        {
            return {};
        }

        RHITextureViewDesc viewDesc;
        viewDesc.format = desc->format;
        viewDesc.dimension = desc->dimension;
        viewDesc.subresourceRange = RHISubresourceRange::All();
        viewDesc.type = RHITextureViewType::RenderTarget;
        viewDesc.debugName = debugName;
        return builder.Write(
            builder.CreateTextureView(texture, viewDesc),
            MakeRGAccessDesc(RHIResourceState::RenderTarget,
                             RHIShaderStage::Pixel,
                             RHIDiscardIntent::Discard));
    }
} // namespace

TAA::TAA()
{
    MarkUnsupported(
        "TAA requires initialized cooked pipeline and view-cache resources");
}

TAA::~TAA()
{
    Shutdown();
}

PostProcessPassDescriptor TAA::Describe() const
{
    return {PostProcessPassId::TemporalAntiAliasing,
            "TemporalAntiAliasing",
            PostProcessStage::Temporal,
            0,
            PostProcessColorDomain::SceneLinearHDR,
            PostProcessColorDomain::SceneLinearHDR,
            PostProcessTransfer::None};
}

void TAA::Configure(const PostProcessSettings& settings)
{
    m_enabled = settings.antiAliasingMode == RenderAntiAliasingMode::TAA;
}

PostProcessFrameInputRequirements TAA::GetFrameInputRequirements() const
{
    PostProcessFrameInputRequirements requirements;
    requirements.requiresDepth = true;
    requirements.requiresVelocity = true;
    requirements.requiresReactiveMask = true;
    return requirements;
}

void TAA::Initialize(IRHIDevice* device, uint32 width, uint32 height)
{
    if (device == nullptr)
    {
        RVX_CORE_ERROR("TAA: cannot initialize without an RHI device");
        ResetRuntimeResources();
        m_device = nullptr;
        MarkUnsupported("TAA requires an RHI device");
        return;
    }

    if (m_device != device)
    {
        ResetRuntimeResources();
        m_device = device;
    }

    if (!EnsureRuntimeResources(width, height))
    {
        MarkUnsupported("TAA history resources could not be created");
        return;
    }

    // Direct initialization intentionally does not claim an implementation:
    // only the injected cooked pipeline resources make TAA executable.
    if (!HasPipelineResources())
    {
        MarkUnsupported("TAA requires initialized cooked pipeline resources");
    }
}

void TAA::Shutdown()
{
    ResetRuntimeResources();
    m_pipelineCache = nullptr;
    m_viewCache = nullptr;
    m_device = nullptr;
    MarkUnsupported("TAA is not initialized");
    m_lastResolveStats = {};
}

void TAA::Resize(uint32 width, uint32 height)
{
    if (!EnsureRuntimeResources(width, height))
    {
        MarkUnsupported("TAA requires a non-zero render extent");
        return;
    }
    ResetHistory();
}

void TAA::SetResources(PipelineCache* pipelineCache, ResourceViewCache* viewCache)
{
    IRHIDevice* const device = pipelineCache ? pipelineCache->GetDevice() : nullptr;
    if (device != m_device)
    {
        ResetRuntimeResources();
        m_device = device;
    }

    m_pipelineCache = pipelineCache;
    m_viewCache = viewCache;
    if (!HasPipelineResources())
    {
        MarkUnsupported("TAA requires initialized cooked pipeline and view-cache resources");
        return;
    }

    if (m_width != 0 && m_height != 0 &&
        !EnsureRuntimeResources(m_width, m_height))
    {
        MarkUnsupported("TAA history resources could not be recreated");
        return;
    }

    m_supported = true;
    m_unsupportedReason.clear();
}

void TAA::SetConfig(const TAAConfig& config)
{
    m_config = config;
    m_config.feedbackMin = IsFiniteNonNegative(m_config.feedbackMin)
        ? std::clamp(m_config.feedbackMin, 0.0f, 1.0f) : 0.88f;
    m_config.feedbackMax = IsFiniteNonNegative(m_config.feedbackMax)
        ? std::clamp(m_config.feedbackMax, 0.0f, 1.0f) : 0.97f;
    if (m_config.feedbackMin > m_config.feedbackMax)
    {
        std::swap(m_config.feedbackMin, m_config.feedbackMax);
    }
    m_config.motionScale = IsFiniteNonNegative(m_config.motionScale)
        ? m_config.motionScale : 1.0f;
    m_config.velocityWeight = IsFiniteNonNegative(m_config.velocityWeight)
        ? m_config.velocityWeight : 1000.0f;
    m_config.depthRejectScale = IsFiniteNonNegative(m_config.depthRejectScale)
        ? m_config.depthRejectScale : 0.01f;
    m_config.reactiveMaskScale = IsFiniteNonNegative(m_config.reactiveMaskScale)
        ? m_config.reactiveMaskScale : 1.0f;
    m_config.sharpness = IsFiniteNonNegative(m_config.sharpness)
        ? std::clamp(m_config.sharpness, 0.0f, 1.0f) : 0.25f;
}

void TAA::AddToGraph(RenderGraph& graph,
                     RGTextureHandle input,
                     RGTextureHandle output)
{
    static_cast<void>(graph);
    static_cast<void>(input);
    static_cast<void>(output);
    RecordUnsupported("TAA requires the full depth, velocity, reactive-mask, and history contract");
}

void TAA::AddToGraph(RenderGraph& graph,
                     const PostProcessFrameInputs& frameInputs,
                     RGTextureHandle output)
{
    m_lastResolveStats = {};
    m_lastResolveStats.requested = m_enabled;
    m_lastResolveStats.frameIndex = frameInputs.frameIndex;
    m_lastResolveStats.historyIdentity = frameInputs.historyIdentity;
    m_lastResolveStats.historyResetReasons = frameInputs.historyResetReasons;
    m_lastResolveStats.historyAvailableBefore = frameInputs.HasHistory();
    m_lastResolveStats.currentJitterPixels = frameInputs.jitterOffset;
    m_lastResolveStats.previousJitterPixels = m_previousJitterPixels;

    if (!m_enabled)
    {
        return;
    }
    if (!HasPipelineResources())
    {
        RecordUnsupported("TAA cooked resolve/store pipeline resources are unavailable", &frameInputs);
        return;
    }
    m_lastResolveStats.pipelineReady = true;

    if (!frameInputs.sceneColor.IsValid() || !frameInputs.depth.IsValid() ||
        !frameInputs.velocity.IsValid() || !frameInputs.reactiveMask.IsValid() ||
        !frameInputs.exposureState.IsValid() ||
        frameInputs.exposureStateContentHash == 0 || !output.IsValid() ||
        !frameInputs.historyIdentity.IsValid())
    {
        RecordUnsupported("TAA requires valid color, depth, velocity, reactive-mask, candidate exposure, output, and history identity inputs",
                          &frameInputs);
        return;
    }

    const RHITextureDesc* const colorDesc = graph.GetTextureDesc(frameInputs.sceneColor);
    const RHITextureDesc* const outputDesc = graph.GetTextureDesc(output);
    const RHITextureDesc* const depthDesc = graph.GetTextureDesc(frameInputs.depth);
    const RHITextureDesc* const velocityDesc = graph.GetTextureDesc(frameInputs.velocity);
    const RHITextureDesc* const reactiveDesc = graph.GetTextureDesc(frameInputs.reactiveMask);
    if (colorDesc == nullptr || outputDesc == nullptr || depthDesc == nullptr ||
        velocityDesc == nullptr || reactiveDesc == nullptr || colorDesc->width == 0 ||
        colorDesc->height == 0 || outputDesc->width != colorDesc->width ||
        outputDesc->height != colorDesc->height)
    {
        RecordUnsupported("TAA input or output texture descriptions are invalid or incompatible", &frameInputs);
        return;
    }
    const auto matchesContract = [colorDesc](
        const RHITextureDesc& desc,
        const SceneTextures::FormatContract& contract)
    {
        return desc.width == colorDesc->width &&
               desc.height == colorDesc->height &&
               desc.depth == 1 && desc.mipLevels == 1 &&
               desc.arraySize == 1 &&
               desc.dimension == RHITextureDimension::Texture2D &&
               desc.format == contract.format &&
               desc.sampleCount == contract.sampleCount;
    };
    if (!matchesContract(*colorDesc, SceneTextures::HDRSceneColorContract) ||
        !matchesContract(*outputDesc, SceneTextures::HDRSceneColorContract) ||
        !matchesContract(*depthDesc, SceneTextures::DeviceDepthContract) ||
        !matchesContract(*velocityDesc, SceneTextures::VelocityContract) ||
        !matchesContract(*reactiveDesc, SceneTextures::ReactiveMaskContract))
    {
        RecordUnsupported(
            "TAA requires full-resolution Count1 SceneTextures formats",
            &frameInputs);
        return;
    }
    if (!HasFlag(colorDesc->usage, RHITextureUsage::ShaderResource) ||
        !HasFlag(depthDesc->usage, RHITextureUsage::ShaderResource) ||
        !HasFlag(velocityDesc->usage, RHITextureUsage::ShaderResource) ||
        !HasFlag(reactiveDesc->usage, RHITextureUsage::ShaderResource) ||
        !HasFlag(outputDesc->usage, RHITextureUsage::ShaderResource) ||
        !HasFlag(outputDesc->usage, RHITextureUsage::RenderTarget))
    {
        RecordUnsupported(
            "TAA requires shader-readable inputs and a shader-readable render-target output",
            &frameInputs);
        return;
    }
    if (!EnsureRuntimeResources(colorDesc->width, colorDesc->height))
    {
        RecordUnsupported("TAA persistent color/depth history resources are unavailable", &frameInputs);
        return;
    }
    if (frameInputs.HasHistory() && !m_historyValid)
    {
        RecordUnsupported("TAA was offered history without a committed matching resource", &frameInputs);
        return;
    }
    if (frameInputs.HasHistory() && m_historyValid &&
        !HasDistinctFiniteTemporalJitter(
            frameInputs.jitterOffset, m_previousJitterPixels))
    {
        RecordUnsupported(
            "TAA refuses sampled history with identical or invalid temporal jitter",
            &frameInputs);
        return;
    }

    AbandonPendingHistory();
    const bool useHistory = frameInputs.HasHistory() && m_historyValid;
    const uint32 readHistoryIndex = m_committedHistoryIndex;
    const uint32 writeHistoryIndex = 1u - readHistoryIndex;
    const auto frameState = std::make_shared<TAAFrameRecordState>();
    const RHIResourceState historyInitialState =
        m_historyResourceStatesInitialized
            ? RHIResourceState::ShaderResource
            : RHIResourceState::Undefined;
    const RGTextureHandle historyColorRead = graph.ImportTexture(
        m_historyColor[readHistoryIndex], historyInitialState);
    const RGTextureHandle historyDepthRead = graph.ImportTexture(
        m_historyDepth[readHistoryIndex], historyInitialState);
    const RGTextureHandle historyColorWrite = graph.ImportTexture(
        m_historyColor[writeHistoryIndex], historyInitialState);
    const RGTextureHandle historyDepthWrite = graph.ImportTexture(
        m_historyDepth[writeHistoryIndex], historyInitialState);
    if (!historyColorRead.IsValid() || !historyDepthRead.IsValid() ||
        !historyColorWrite.IsValid() || !historyDepthWrite.IsValid())
    {
        RecordUnsupported(
            "TAA persistent history textures could not be imported into the render graph",
            &frameInputs);
        return;
    }

    struct ResolveData
    {
        RGTextureHandle currentColor;
        RGBufferHandle exposureState;
        RGTextureHandle historyColor;
        RGTextureHandle currentDepth;
        RGTextureHandle historyDepth;
        RGTextureHandle velocity;
        RGTextureHandle reactiveMask;
        RGTextureHandle output;
        RGTextureViewHandle currentColorView;
        RGTextureViewHandle historyColorView;
        RGTextureViewHandle currentDepthView;
        RGTextureViewHandle historyDepthView;
        RGTextureViewHandle velocityView;
        RGTextureViewHandle reactiveMaskView;
        RGTextureViewHandle outputView;
        RHIFormat outputFormat = RHIFormat::Unknown;
        bool useHistory = false;
        Vec2 currentJitterPixels = Vec2(0.0f);
        Vec2 previousJitterPixels = Vec2(0.0f);
        uint64 exposureStateContentHash = 0;
        std::shared_ptr<TAAFrameRecordState> state;
    };

    graph.AddPass<ResolveData>(
        "TAAResolve",
        RenderGraphPassType::Graphics,
        [this, frameInputs, output, historyColorRead, historyDepthRead,
         useHistory, frameState](
            RenderGraphBuilder& builder,
            ResolveData& data)
        {
            data.currentColor = builder.Read(
                frameInputs.sceneColor,
                MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                 RHIShaderStage::Pixel));
            data.exposureState = builder.Read(
                frameInputs.exposureState,
                MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                 RHIShaderStage::Pixel));
            data.historyColor = builder.Read(
                historyColorRead,
                MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                 RHIShaderStage::Pixel));
            data.currentDepth = builder.Read(
                frameInputs.depth,
                MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                 RHIShaderStage::Pixel));
            data.historyDepth = builder.Read(
                historyDepthRead,
                MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                 RHIShaderStage::Pixel));
            data.velocity = builder.Read(
                frameInputs.velocity,
                MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                 RHIShaderStage::Pixel));
            data.reactiveMask = builder.Read(
                frameInputs.reactiveMask,
                MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                 RHIShaderStage::Pixel));
            data.output = output;
            data.currentColorView = AddTextureSRV(builder, data.currentColor, "TAACurrentColorSRV");
            data.historyColorView = AddTextureSRV(builder, data.historyColor, "TAAHistoryColorSRV");
            data.currentDepthView = AddTextureSRV(builder, data.currentDepth, "TAACurrentDepthSRV");
            data.historyDepthView = AddTextureSRV(builder, data.historyDepth, "TAAHistoryDepthSRV");
            data.velocityView = AddTextureSRV(builder, data.velocity, "TAAVelocitySRV");
            data.reactiveMaskView = AddTextureSRV(builder, data.reactiveMask, "TAAReactiveMaskSRV");
            data.outputView = AddTextureRTV(builder, output, "TAAResolveRTV");
            const RHITextureDesc* const outputTextureDesc = builder.GetTextureDesc(output);
            data.outputFormat = outputTextureDesc ? outputTextureDesc->format : RHIFormat::Unknown;
            data.useHistory = useHistory;
            data.currentJitterPixels = frameInputs.jitterOffset;
            data.previousJitterPixels = m_previousJitterPixels;
            data.exposureStateContentHash = frameInputs.exposureStateContentHash;
            data.state = frameState;
        },
        [this](const ResolveData& data, RenderGraphPassContext& context)
        {
            RHIPipeline* const pipeline = m_pipelineCache
                ? m_pipelineCache->GetTAAResolvePipeline(data.outputFormat)
                : nullptr;
            RHIDescriptorSetLayout* const setLayout = m_pipelineCache
                ? m_pipelineCache->GetTAASetLayout() : nullptr;
            IRHIDevice* const device = m_pipelineCache ? m_pipelineCache->GetDevice() : nullptr;
            if (pipeline == nullptr || setLayout == nullptr || device == nullptr || !m_sampler)
            {
                m_lastResolveStats.fallbackReason = "TAA resolve pipeline resources disappeared during execution";
                context.MarkExecutionFailed(m_lastResolveStats.fallbackReason.c_str());
                return;
            }

            RHITexture* const currentColor = context.GetTexture(data.currentColor);
            RHIBuffer* const exposureState = context.GetBuffer(data.exposureState);
            RHITexture* const output = context.GetTexture(data.output);
            RHITextureView* const currentColorView = context.GetTextureView(data.currentColorView);
            RHITextureView* const historyColorView = context.GetTextureView(data.historyColorView);
            RHITextureView* const currentDepthView = context.GetTextureView(data.currentDepthView);
            RHITextureView* const historyDepthView = context.GetTextureView(data.historyDepthView);
            RHITextureView* const velocityView = context.GetTextureView(data.velocityView);
            RHITextureView* const reactiveMaskView = context.GetTextureView(data.reactiveMaskView);
            RHITextureView* const outputView = context.GetTextureView(data.outputView);
            if (currentColor == nullptr || output == nullptr || exposureState == nullptr ||
                data.exposureStateContentHash == 0 || currentColorView == nullptr ||
                historyColorView == nullptr || currentDepthView == nullptr ||
                historyDepthView == nullptr || velocityView == nullptr ||
                reactiveMaskView == nullptr || outputView == nullptr)
            {
                m_lastResolveStats.fallbackReason = "TAA resolve resources could not be realized";
                context.MarkExecutionFailed(m_lastResolveStats.fallbackReason.c_str());
                return;
            }

            TAAGPUConstants constants;
            constants.renderSize[0] = static_cast<float>(currentColor->GetWidth());
            constants.renderSize[1] = static_cast<float>(currentColor->GetHeight());
            constants.renderSize[2] = constants.renderSize[0] > 0.0f ?
                1.0f / constants.renderSize[0] : 1.0f;
            constants.renderSize[3] = constants.renderSize[1] > 0.0f ?
                1.0f / constants.renderSize[1] : 1.0f;
            constants.feedbackParams[0] = m_config.feedbackMin;
            constants.feedbackParams[1] = m_config.feedbackMax;
            constants.feedbackParams[2] = m_config.motionScale;
            constants.feedbackParams[3] = m_config.velocityWeight;
            constants.resolveParams[0] = m_config.depthRejectScale;
            constants.resolveParams[1] = m_config.reactiveMaskScale;
            constants.resolveParams[2] = m_config.sharpen ? m_config.sharpness : 0.0f;
            constants.resolveParams[3] = data.useHistory ? 1.0f : 0.0f;
            RHIBufferRef constantBuffer = CreateImmutablePassConstants(
                *device,
                constants,
                "TAAResolvePassConstants");
            if (!constantBuffer ||
                !context.RetainSubmissionResource(Ref<RefCounted>(constantBuffer)))
            {
                m_lastResolveStats.fallbackReason =
                    "TAA resolve immutable constants could not be retained";
                context.MarkExecutionFailed(m_lastResolveStats.fallbackReason.c_str());
                return;
            }

            RHIDescriptorSetDesc descriptorDesc;
            descriptorDesc.layout = setLayout;
            descriptorDesc.debugName = "TAAResolveDescriptorSet";
            descriptorDesc.BindBuffer(0, constantBuffer.Get(), 0,
                                      AlignConstantBufferSize(sizeof(TAAGPUConstants)));
            descriptorDesc.BindTexture(1, currentColorView);
            descriptorDesc.BindTexture(2, historyColorView);
            descriptorDesc.BindTexture(3, currentDepthView);
            descriptorDesc.BindTexture(4, historyDepthView);
            descriptorDesc.BindTexture(5, velocityView);
            descriptorDesc.BindTexture(6, reactiveMaskView);
            descriptorDesc.BindSampler(7, m_sampler.Get());
            descriptorDesc.BindBuffer(8, exposureState);
            RHIDescriptorSetRef descriptorSet = device->CreateDescriptorSet(descriptorDesc);
            if (!descriptorSet || !context.RetainSubmissionResource(Ref<RefCounted>(descriptorSet)))
            {
                m_lastResolveStats.fallbackReason = "TAA resolve descriptor set creation failed";
                context.MarkExecutionFailed(m_lastResolveStats.fallbackReason.c_str());
                return;
            }

            RHIRenderPassDesc renderPassDesc;
            renderPassDesc.AddColorAttachment(outputView, RHILoadOp::DontCare, RHIStoreOp::Store);
            renderPassDesc.SetRenderArea(0, 0, output->GetWidth(), output->GetHeight());
            RHICommandContext& commands = context.Commands();
            commands.BeginRenderPass(renderPassDesc);
            commands.SetPipeline(pipeline);
            commands.SetDescriptorSet(0, descriptorSet.Get());
            RHIViewport viewport;
            viewport.width = static_cast<float>(output->GetWidth());
            viewport.height = static_cast<float>(output->GetHeight());
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;
            commands.SetViewport(viewport);
            RHIRect scissor;
            scissor.width = output->GetWidth();
            scissor.height = output->GetHeight();
            commands.SetScissor(scissor);
            commands.Draw(3, 1, 0, 0);
            commands.EndRenderPass();

            data.state->resolveExecuted = true;
            m_lastResolveStats.executed = true;
            m_lastResolveStats.historySampled = data.useHistory;
            m_lastResolveStats.historyConsumed = data.useHistory;
            m_lastResolveStats.resolveGraphPassCount = 1;
            m_lastResolveStats.exposureStateContentHash =
                data.exposureStateContentHash;
        });

    struct HistoryStoreData
    {
        RGTextureHandle resolvedColor;
        RGBufferHandle exposureState;
        RGTextureHandle historyColorRead;
        RGTextureHandle currentDepth;
        RGTextureHandle historyDepthRead;
        RGTextureHandle velocity;
        RGTextureHandle reactiveMask;
        RGTextureHandle historyColorWrite;
        RGTextureHandle historyDepthWrite;
        RGTextureViewHandle resolvedColorView;
        RGTextureViewHandle historyColorReadView;
        RGTextureViewHandle currentDepthView;
        RGTextureViewHandle historyDepthReadView;
        RGTextureViewHandle velocityView;
        RGTextureViewHandle reactiveMaskView;
        RGTextureViewHandle historyColorWriteView;
        RGTextureViewHandle historyDepthWriteView;
        RenderViewHistoryIdentity historyIdentity{};
        Vec2 currentJitterPixels = Vec2(0.0f);
        uint64 exposureStateContentHash = 0;
        std::shared_ptr<TAAFrameRecordState> state;
    };

    graph.AddPass<HistoryStoreData>(
        "TAAHistoryStore",
        RenderGraphPassType::Graphics,
        [frameInputs, output, historyColorRead, historyDepthRead,
         historyColorWrite, historyDepthWrite, frameState](
            RenderGraphBuilder& builder,
            HistoryStoreData& data)
        {
            data.resolvedColor = builder.Read(
                output,
                MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                 RHIShaderStage::Pixel));
            data.exposureState = builder.Read(
                frameInputs.exposureState,
                MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                 RHIShaderStage::Pixel));
            data.historyColorRead = builder.Read(
                historyColorRead,
                MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                 RHIShaderStage::Pixel));
            data.currentDepth = builder.Read(
                frameInputs.depth,
                MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                 RHIShaderStage::Pixel));
            data.historyDepthRead = builder.Read(
                historyDepthRead,
                MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                 RHIShaderStage::Pixel));
            data.velocity = builder.Read(
                frameInputs.velocity,
                MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                 RHIShaderStage::Pixel));
            data.reactiveMask = builder.Read(
                frameInputs.reactiveMask,
                MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                 RHIShaderStage::Pixel));
            data.historyColorWrite = historyColorWrite;
            data.historyDepthWrite = historyDepthWrite;
            data.resolvedColorView = AddTextureSRV(builder, data.resolvedColor, "TAAStoredColorSRV");
            data.historyColorReadView = AddTextureSRV(builder, data.historyColorRead, "TAAStoredHistoryColorSRV");
            data.currentDepthView = AddTextureSRV(builder, data.currentDepth, "TAAStoredCurrentDepthSRV");
            data.historyDepthReadView = AddTextureSRV(builder, data.historyDepthRead, "TAAStoredHistoryDepthSRV");
            data.velocityView = AddTextureSRV(builder, data.velocity, "TAAStoredVelocitySRV");
            data.reactiveMaskView = AddTextureSRV(builder, data.reactiveMask, "TAAStoredReactiveMaskSRV");
            data.historyColorWriteView = AddTextureRTV(builder, data.historyColorWrite, "TAAHistoryColorRTV");
            data.historyDepthWriteView = AddTextureRTV(builder, data.historyDepthWrite, "TAAHistoryDepthRTV");
            builder.SetExportState(data.historyColorWrite, RHIResourceState::ShaderResource);
            builder.SetExportState(data.historyDepthWrite, RHIResourceState::ShaderResource);
            data.historyIdentity = frameInputs.historyIdentity;
            data.currentJitterPixels = frameInputs.jitterOffset;
            data.exposureStateContentHash = frameInputs.exposureStateContentHash;
            data.state = frameState;
        },
        [this, writeHistoryIndex](const HistoryStoreData& data,
                                  RenderGraphPassContext& context)
        {
            if (!data.state->resolveExecuted)
            {
                m_lastResolveStats.fallbackReason = "TAA history store rejected an unresolved color candidate";
                context.MarkExecutionFailed(m_lastResolveStats.fallbackReason.c_str());
                return;
            }

            RHIPipeline* const pipeline = m_pipelineCache
                ? m_pipelineCache->GetTAAHistoryStorePipeline() : nullptr;
            RHIDescriptorSetLayout* const setLayout = m_pipelineCache
                ? m_pipelineCache->GetTAASetLayout() : nullptr;
            IRHIDevice* const device = m_pipelineCache ? m_pipelineCache->GetDevice() : nullptr;
            if (pipeline == nullptr || setLayout == nullptr || device == nullptr || !m_sampler)
            {
                m_lastResolveStats.fallbackReason = "TAA history-store pipeline resources disappeared during execution";
                context.MarkExecutionFailed(m_lastResolveStats.fallbackReason.c_str());
                return;
            }

            RHITexture* const resolvedColor = context.GetTexture(data.resolvedColor);
            RHIBuffer* const exposureState = context.GetBuffer(data.exposureState);
            RHITexture* const historyColor = context.GetTexture(data.historyColorWrite);
            RHITexture* const historyDepth = context.GetTexture(data.historyDepthWrite);
            RHITextureView* const resolvedColorView = context.GetTextureView(data.resolvedColorView);
            RHITextureView* const historyColorReadView = context.GetTextureView(data.historyColorReadView);
            RHITextureView* const currentDepthView = context.GetTextureView(data.currentDepthView);
            RHITextureView* const historyDepthReadView = context.GetTextureView(data.historyDepthReadView);
            RHITextureView* const velocityView = context.GetTextureView(data.velocityView);
            RHITextureView* const reactiveMaskView = context.GetTextureView(data.reactiveMaskView);
            RHITextureView* const historyColorWriteView = context.GetTextureView(data.historyColorWriteView);
            RHITextureView* const historyDepthWriteView = context.GetTextureView(data.historyDepthWriteView);
            if (resolvedColor == nullptr || historyColor == nullptr || historyDepth == nullptr ||
                exposureState == nullptr || data.exposureStateContentHash == 0 ||
                resolvedColorView == nullptr || historyColorReadView == nullptr ||
                currentDepthView == nullptr || historyDepthReadView == nullptr ||
                velocityView == nullptr || reactiveMaskView == nullptr ||
                historyColorWriteView == nullptr || historyDepthWriteView == nullptr)
            {
                m_lastResolveStats.fallbackReason = "TAA history-store resources could not be realized";
                context.MarkExecutionFailed(m_lastResolveStats.fallbackReason.c_str());
                return;
            }

            TAAGPUConstants constants;
            constants.renderSize[0] = static_cast<float>(resolvedColor->GetWidth());
            constants.renderSize[1] = static_cast<float>(resolvedColor->GetHeight());
            constants.renderSize[2] = constants.renderSize[0] > 0.0f ?
                1.0f / constants.renderSize[0] : 1.0f;
            constants.renderSize[3] = constants.renderSize[1] > 0.0f ?
                1.0f / constants.renderSize[1] : 1.0f;
            RHIBufferRef constantBuffer = CreateImmutablePassConstants(
                *device,
                constants,
                "TAAHistoryStorePassConstants");
            if (!constantBuffer ||
                !context.RetainSubmissionResource(Ref<RefCounted>(constantBuffer)))
            {
                m_lastResolveStats.fallbackReason =
                    "TAA history-store immutable constants could not be retained";
                context.MarkExecutionFailed(m_lastResolveStats.fallbackReason.c_str());
                return;
            }

            RHIDescriptorSetDesc descriptorDesc;
            descriptorDesc.layout = setLayout;
            descriptorDesc.debugName = "TAAHistoryStoreDescriptorSet";
            descriptorDesc.BindBuffer(0, constantBuffer.Get(), 0,
                                      AlignConstantBufferSize(sizeof(TAAGPUConstants)));
            descriptorDesc.BindTexture(1, resolvedColorView);
            descriptorDesc.BindTexture(2, historyColorReadView);
            descriptorDesc.BindTexture(3, currentDepthView);
            descriptorDesc.BindTexture(4, historyDepthReadView);
            descriptorDesc.BindTexture(5, velocityView);
            descriptorDesc.BindTexture(6, reactiveMaskView);
            descriptorDesc.BindSampler(7, m_sampler.Get());
            descriptorDesc.BindBuffer(8, exposureState);
            RHIDescriptorSetRef descriptorSet = device->CreateDescriptorSet(descriptorDesc);
            if (!descriptorSet || !context.RetainSubmissionResource(Ref<RefCounted>(descriptorSet)))
            {
                m_lastResolveStats.fallbackReason = "TAA history-store descriptor set creation failed";
                context.MarkExecutionFailed(m_lastResolveStats.fallbackReason.c_str());
                return;
            }

            RHIRenderPassDesc renderPassDesc;
            renderPassDesc.AddColorAttachment(historyColorWriteView, RHILoadOp::DontCare, RHIStoreOp::Store);
            renderPassDesc.AddColorAttachment(historyDepthWriteView, RHILoadOp::DontCare, RHIStoreOp::Store);
            renderPassDesc.SetRenderArea(0, 0, historyColor->GetWidth(), historyColor->GetHeight());
            RHICommandContext& commands = context.Commands();
            commands.BeginRenderPass(renderPassDesc);
            commands.SetPipeline(pipeline);
            commands.SetDescriptorSet(0, descriptorSet.Get());
            RHIViewport viewport;
            viewport.width = static_cast<float>(historyColor->GetWidth());
            viewport.height = static_cast<float>(historyColor->GetHeight());
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;
            commands.SetViewport(viewport);
            RHIRect scissor;
            scissor.width = historyColor->GetWidth();
            scissor.height = historyColor->GetHeight();
            commands.SetScissor(scissor);
            commands.Draw(3, 1, 0, 0);
            commands.EndRenderPass();

            m_pendingHistoryIndex = writeHistoryIndex;
            m_pendingHistoryIdentity = data.historyIdentity;
            m_pendingJitterPixels = data.currentJitterPixels;
            m_hasPendingHistory = true;
            m_lastResolveStats.historyCandidatePrepared = true;
            m_lastResolveStats.historyStoreGraphPassCount = 1;
        });

    m_lastResolveStats.inputsReady = true;
    m_lastResolveStats.exposureStateContentHash =
        frameInputs.exposureStateContentHash;
}

bool TAA::CanCommitHistoryAfterPresent(
    const RenderViewHistoryIdentity& identity) const noexcept
{
    return m_hasPendingHistory && identity.IsValid() &&
           identity == m_pendingHistoryIdentity;
}

bool TAA::CommitHistoryAfterPresent(
    const RenderViewHistoryIdentity& identity) noexcept
{
    if (!CanCommitHistoryAfterPresent(identity))
        return false;

    m_committedHistoryIndex = m_pendingHistoryIndex;
    m_previousJitterPixels = m_pendingJitterPixels;
    // Only a successfully presented candidate proves that the graph work was
    // submitted and that every persistent history ended in its exported SRV
    // state. Failed or abandoned frames remain safe to re-import as Undefined.
    m_historyResourceStatesInitialized = true;
    m_historyValid = true;
    m_hasPendingHistory = false;
    m_pendingHistoryIdentity = {};
    m_lastResolveStats.historyCommitted = true;
    return true;
}

void TAA::AbandonPendingHistory() noexcept
{
    if (!m_hasPendingHistory)
    {
        return;
    }

    m_hasPendingHistory = false;
    m_pendingHistoryIdentity = {};
    m_pendingJitterPixels = Vec2(0.0f);
    // A candidate is immutable evidence once the store pass produced it.
    // Abandoning may discard its eligibility for a future frame, but must not
    // rewrite the receipt as if production never happened.
    m_lastResolveStats.historyCandidatePrepared = true;
    m_lastResolveStats.historyAbandoned = true;
}

void TAA::ResetHistory()
{
    m_historyValid = false;
    m_committedHistoryIndex = 0;
    m_previousJitterPixels = Vec2(0.0f);
    m_hasPendingHistory = false;
    m_pendingHistoryIndex = 0;
    m_pendingHistoryIdentity = {};
    m_pendingJitterPixels = Vec2(0.0f);
}

bool TAA::EnsureRuntimeResources(uint32 width, uint32 height)
{
    if (m_device == nullptr || width == 0 || height == 0)
    {
        return false;
    }
    if (m_width == width && m_height == height && m_historyColor[0] &&
        m_historyColor[1] && m_historyDepth[0] && m_historyDepth[1] && m_sampler)
    {
        return true;
    }

    ResetRuntimeResources();
    m_width = width;
    m_height = height;

    RHITextureDesc colorDesc;
    colorDesc.width = width;
    colorDesc.height = height;
    colorDesc.format = RHIFormat::RGBA16_FLOAT;
    colorDesc.usage = RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource;
    colorDesc.debugName = "TAAHistoryColor";
    m_historyColor[0] = m_device->CreateTexture(colorDesc);
    m_historyColor[1] = m_device->CreateTexture(colorDesc);

    RHITextureDesc depthDesc = colorDesc;
    depthDesc.format = RHIFormat::R32_FLOAT;
    depthDesc.debugName = "TAAHistoryDepth";
    m_historyDepth[0] = m_device->CreateTexture(depthDesc);
    m_historyDepth[1] = m_device->CreateTexture(depthDesc);

    RHISamplerDesc samplerDesc = RHISamplerDesc::LinearClamp();
    samplerDesc.debugName = "TAALinearClampSampler";
    m_sampler = m_device->CreateSampler(samplerDesc);

    if (!m_historyColor[0] || !m_historyColor[1] || !m_historyDepth[0] ||
        !m_historyDepth[1] || !m_sampler)
    {
        ResetRuntimeResources();
        return false;
    }

    return true;
}

bool TAA::HasPipelineResources() const noexcept
{
    return m_pipelineCache != nullptr && m_pipelineCache->IsInitialized() &&
           m_pipelineCache->GetDevice() != nullptr && m_viewCache != nullptr &&
           m_viewCache->IsInitialized() && m_pipelineCache->GetTAAResolvePipeline() != nullptr &&
           m_pipelineCache->GetTAAHistoryStorePipeline() != nullptr &&
           m_pipelineCache->GetTAALayout() != nullptr &&
           m_pipelineCache->GetTAASetLayout() != nullptr;
}

void TAA::ResetRuntimeResources()
{
    m_historyColor[0].Reset();
    m_historyColor[1].Reset();
    m_historyDepth[0].Reset();
    m_historyDepth[1].Reset();
    m_sampler.Reset();
    m_width = 0;
    m_height = 0;
    m_historyResourceStatesInitialized = false;
    ResetHistory();
}

void TAA::RecordUnsupported(const char* reason,
                            const PostProcessFrameInputs* frameInputs)
{
    m_lastResolveStats.pipelineReady = HasPipelineResources();
    m_lastResolveStats.inputsReady = false;
    m_lastResolveStats.executed = false;
    m_lastResolveStats.fallbackReason = reason ? reason : "TAA is unavailable";
    if (frameInputs != nullptr)
    {
        m_lastResolveStats.frameIndex = frameInputs->frameIndex;
        m_lastResolveStats.historyIdentity = frameInputs->historyIdentity;
        m_lastResolveStats.historyResetReasons = frameInputs->historyResetReasons;
        m_lastResolveStats.historyAvailableBefore = frameInputs->HasHistory();
        m_lastResolveStats.currentJitterPixels = frameInputs->jitterOffset;
        m_lastResolveStats.previousJitterPixels = m_previousJitterPixels;
    }
    RVX_CORE_WARN("TAA: {}", m_lastResolveStats.fallbackReason);
}

} // namespace RVX
