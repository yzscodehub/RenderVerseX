/**
 * @file GroundTruthAmbientOcclusion.cpp
 * @brief Graph-backed deterministic GTAO primitive.
 */

#include "Render/Lighting/GroundTruthAmbientOcclusion.h"

#include "Render/PipelineCache.h"
#include "Resources/RenderSubmissionTracker.h"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>

namespace RVX
{
    struct GroundTruthAmbientOcclusionExecutionState
    {
        uint32 completedPassCount = 0;
        uint32 pendingHistoryIndex = 0;
        RenderViewHistoryIdentity pendingHistoryIdentity{};
        uint64 pendingHistorySettingsHash = 0;
        RenderViewHistoryIdentity probeIdentity{};
        uint64 probeSettingsHash = 0;
        uint64 probeAmbientOcclusionFieldHash = 0;
        bool failed = false;
        bool probeExpected = false;
        bool probeRecorded = false;
    };

namespace
{
    constexpr uint64 RVX_GTAO_CONSTANT_BUFFER_ALIGNMENT = 256;

    enum class GTAOStage : uint8
    {
        Surface = 0,
        Horizon,
        BilateralHorizontal,
        BilateralVertical,
        Temporal,
    };

    struct GTAOGPUConstants
    {
        float renderSize[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        float depthParams[4] = {0.1f, 1000.0f, 0.0f, 0.0f};
        float horizonParams[4] = {1.0f, 0.1f, 3.0f, 0.15f};
        uint32 samplingParams[4] = {8U, 4U, 2U, 0U};
        float filterParams[4] = {1.5f, 0.8f, 0.9f, 0.0f};
        float temporalParams[4] = {1.0f, 0.05f, 0.85f, 0.0f};
        float cameraPosition[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        float cameraForward[4] = {0.0f, 0.0f, -1.0f, 0.0f};
        Mat4 inverseViewProjection = Mat4Identity();
        Mat4 previousInverseViewProjection = Mat4Identity();
    };

    static_assert(sizeof(Mat4) == 64U);
    static_assert(offsetof(GTAOGPUConstants, samplingParams) == 48U);
    static_assert(offsetof(GTAOGPUConstants, filterParams) == 64U);
    static_assert(offsetof(GTAOGPUConstants, temporalParams) == 80U);
    static_assert(offsetof(GTAOGPUConstants, cameraPosition) == 96U);
    static_assert(offsetof(GTAOGPUConstants, cameraForward) == 112U);
    static_assert(offsetof(GTAOGPUConstants, inverseViewProjection) == 128U);
    static_assert(offsetof(
                      GTAOGPUConstants,
                      previousInverseViewProjection) == 192U);
    static_assert(sizeof(GTAOGPUConstants) == 256U,
                  "GTAO constants must preserve the HLSL cbuffer ABI");

    struct GTAOPassData
    {
        RGTextureHandle deviceDepth;
        RGTextureHandle normalSource;
        RGTextureHandle inputAO;
        RGTextureHandle historyAO;
        RGTextureHandle velocity;
        RGTextureHandle historyDepth;
        RGTextureHandle output;
        RGTextureViewHandle deviceDepthView;
        RGTextureViewHandle normalSourceView;
        RGTextureViewHandle inputAOView;
        RGTextureViewHandle historyAOView;
        RGTextureViewHandle velocityView;
        RGTextureViewHandle historyDepthView;
        RGTextureViewHandle outputView;
        GTAOGPUConstants constants{};
        GTAOStage stage = GTAOStage::Surface;
        IRHIDevice* device = nullptr;
        RHIPipelineRef pipeline;
        RHIPipelineLayoutRef pipelineLayout;
        RHIDescriptorSetLayoutRef setLayout;
        RHISamplerRef sampler;
        std::shared_ptr<GroundTruthAmbientOcclusionExecutionState>
            executionState;
        bool resourcesRetained = false;
    };

    /** One graph-owned identity visibility field when GTAO is not requested. */
    struct GTAONeutralPassData
    {
        RGTextureHandle output;
        RGTextureViewHandle outputView;
    };

    struct GTAOQualificationProbeCopyData
    {
        RGTextureHandle visibility;
        RGBufferHandle readback;
        RHIBufferRef readbackOwner;
        RenderViewHistoryIdentity identity{};
        uint64 settingsHash = 0;
        uint64 ambientOcclusionFieldHash = 0;
        uint32 width = 0;
        uint32 height = 0;
        uint32 rowPitch = 0;
        std::shared_ptr<GroundTruthAmbientOcclusionExecutionState>
            executionState;
    };

    [[nodiscard]] uint64 AlignConstantBufferSize(uint64 size) noexcept
    {
        return (size + RVX_GTAO_CONSTANT_BUFFER_ALIGNMENT - 1) &
               ~(RVX_GTAO_CONSTANT_BUFFER_ALIGNMENT - 1);
    }

    [[nodiscard]] uint32 AlignReadbackRowPitch(
        RHIBackendType backend,
        uint32 width) noexcept
    {
        const uint32 alignment = backend == RHIBackendType::DX12 ||
                                      backend == RHIBackendType::Metal
                                  ? 256U
                                  : 1U;
        const uint64 aligned = (static_cast<uint64>(width) + alignment - 1U) &
            ~static_cast<uint64>(alignment - 1U);
        return aligned != 0 && aligned <= std::numeric_limits<uint32>::max()
            ? static_cast<uint32>(aligned)
            : 0U;
    }

    [[nodiscard]] bool IsExactCurrentGraphicsSubmissionToken(
        const RenderSubmissionTracker& tracker,
        const GPUCompletionToken& token,
        GPUCompletionPoint& outPoint) noexcept
    {
        outPoint = {};
        GPUCompletionToken normalized;
        if (!MergeGPUCompletionToken(normalized, token) || normalized.count == 0 ||
            tracker.Query(normalized) == GPUCompletionStatus::Lost)
        {
            return false;
        }

        bool hasGraphicsPoint = false;
        for (uint32 index = 0; index < normalized.count; ++index)
        {
            const GPUCompletionPoint point = normalized.points[index];
            if (!IsDeclaredGPUQueueDomain(point.domain) || point.value == 0 ||
                point.value != tracker.GetLastSubmittedValue(point.domain))
            {
                return false;
            }
            if (point.domain == GPUQueueDomain::Graphics)
            {
                if (hasGraphicsPoint)
                {
                    return false;
                }
                hasGraphicsPoint = true;
                outPoint = point;
            }
        }
        return hasGraphicsPoint;
    }

    [[nodiscard]] bool IsCompletionSatisfied(
        GPUCompletionStatus status) noexcept
    {
        return status == GPUCompletionStatus::Completed ||
               status == GPUCompletionStatus::CompatibilityWaitIdle;
    }

    [[nodiscard]] RHIBufferRef CreateImmutablePassConstants(
        IRHIDevice& device,
        const GTAOGPUConstants& constants,
        const char* debugName)
    {
        RHIBufferDesc bufferDesc;
        bufferDesc.size = AlignConstantBufferSize(sizeof(GTAOGPUConstants));
        bufferDesc.usage = RHIBufferUsage::Constant;
        bufferDesc.memoryType = RHIMemoryType::Upload;
        bufferDesc.debugName = debugName;

        RHIBufferRef buffer = device.CreateBuffer(bufferDesc);
        if (!buffer)
        {
            return {};
        }

        void* const mapped = buffer->Map();
        if (mapped == nullptr)
        {
            return {};
        }
        std::memcpy(mapped, &constants, sizeof(constants));
        buffer->Unmap();
        return buffer;
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

    [[nodiscard]] uint64 HashAmbientOcclusionField(
        uint64 settingsHash,
        uint64 surfaceFieldHash,
        const RenderViewHistoryIdentity& identity) noexcept
    {
        constexpr uint64 offsetBasis = 14695981039346656037ULL;
        constexpr uint64 prime = 1099511628211ULL;
        uint64 hash = offsetBasis;
        const auto append = [&hash](uint64 value) noexcept
        {
            hash ^= value;
            hash *= prime;
        };
        append(settingsHash);
        append(surfaceFieldHash);
        append(identity.previousFrame.frameSequence);
        append(identity.previousFrame.sceneRevision);
        append(identity.previousFrame.surfaceGeneration);
        append(identity.previousFrame.configurationRevision);
        append(identity.view.sceneRuntimeId);
        append(identity.view.cameraEntityPacked);
        append(identity.extent.width);
        append(identity.extent.height);
        append(identity.familyId);
        append(identity.worldRevision);
        append(identity.temporalEpoch);
        append(identity.formatRevision);
        append(identity.surfaceGeneration);
        append(identity.cameraCutRevision);
        return hash != 0 ? hash : 1;
    }

    [[nodiscard]] bool IsTexture2DCount1(
        const RHITextureDesc& desc,
        uint32 width,
        uint32 height,
        RHIFormat format) noexcept
    {
        return desc.width == width && desc.height == height && desc.depth == 1 &&
               desc.mipLevels == 1 && desc.arraySize == 1 &&
               desc.dimension == RHITextureDimension::Texture2D &&
               desc.sampleCount == RHISampleCount::Count1 && desc.format == format;
    }

    [[nodiscard]] RHIPipeline* ResolvePipeline(
        PipelineCache& cache,
        GTAOStage stage) noexcept
    {
        switch (stage)
        {
            case GTAOStage::Surface: return cache.GetGTAOSurfacePipeline();
            case GTAOStage::Horizon: return cache.GetGTAOHorizonPipeline();
            case GTAOStage::BilateralHorizontal:
                return cache.GetGTAOBilateralHorizontalPipeline();
            case GTAOStage::BilateralVertical:
                return cache.GetGTAOBilateralVerticalPipeline();
            case GTAOStage::Temporal: return cache.GetGTAOTemporalPipeline();
            default: return nullptr;
        }
    }

    [[nodiscard]] const char* GetStageConstantsDebugName(GTAOStage stage) noexcept
    {
        switch (stage)
        {
            case GTAOStage::Surface: return "GTAOSurfacePassConstants";
            case GTAOStage::Horizon: return "GTAOHorizonPassConstants";
            case GTAOStage::BilateralHorizontal:
                return "GTAOBilateralHorizontalPassConstants";
            case GTAOStage::BilateralVertical:
                return "GTAOBilateralVerticalPassConstants";
            case GTAOStage::Temporal: return "GTAOTemporalPassConstants";
            default: return "GTAOPassConstants";
        }
    }

    [[nodiscard]] const char* GetStageDescriptorDebugName(GTAOStage stage) noexcept
    {
        switch (stage)
        {
            case GTAOStage::Surface: return "GTAOSurfaceDescriptorSet";
            case GTAOStage::Horizon: return "GTAOHorizonDescriptorSet";
            case GTAOStage::BilateralHorizontal:
                return "GTAOBilateralHorizontalDescriptorSet";
            case GTAOStage::BilateralVertical:
                return "GTAOBilateralVerticalDescriptorSet";
            case GTAOStage::Temporal: return "GTAOTemporalDescriptorSet";
            default: return "GTAODescriptorSet";
        }
    }
} // namespace

GroundTruthAmbientOcclusion::~GroundTruthAmbientOcclusion()
{
    Shutdown();
}

void GroundTruthAmbientOcclusion::SetResources(PipelineCache* pipelineCache)
{
    IRHIDevice* const nextDevice = pipelineCache ? pipelineCache->GetDevice() : nullptr;
    if (nextDevice != m_device)
    {
        ResetRuntimeResources();
        m_device = nextDevice;
    }
    m_pipelineCache = pipelineCache;
}

void GroundTruthAmbientOcclusion::SetSettings(
    const RenderAmbientOcclusionSettings& settings)
{
    // Invalid settings intentionally remain observable. AddToGraph() rejects
    // them instead of silently clamping a bad authoring policy into a new ABI.
    m_settings = settings;
}

GroundTruthAmbientOcclusionOutput GroundTruthAmbientOcclusion::AddToGraph(
    RenderGraph& graph,
    const ViewData& view,
    const GroundTruthAmbientOcclusionFrameInputs& inputs)
{
    m_lastStats = {};
    m_pendingExecutionState.reset();
    m_lastStats.requested = m_settings.enabled;

    // A disabled optional feature must still close the required lighting data
    // chain. Do not turn this into a fallback: it is the canonical neutral
    // indirect-visibility field and deliberately does not allocate history or
    // report the multi-pass GTAO execution counters.
    if (!m_settings.enabled)
    {
        // Disabling the effect invalidates semantic history. Persistent
        // allocations may remain, but a later re-enable must start from a
        // presentation-qualified candidate generated with current settings.
        ResetHistory();
        if (view.viewportWidth == 0 || view.viewportHeight == 0)
        {
            return {};
        }

        RHITextureDesc neutralDesc = RHITextureDesc::Texture2D(
            view.viewportWidth, view.viewportHeight, RHIFormat::R8_UNORM,
            RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource);
        neutralDesc.SetOptimizedClearColor(
            RHIClearColor{1.0f, 1.0f, 1.0f, 1.0f});
        neutralDesc.debugName = "GTAONeutralVisibility";
        const RGTextureHandle neutralVisibility = graph.CreateTexture(neutralDesc);
        if (!neutralVisibility.IsValid())
        {
            return {};
        }

        graph.AddPass<GTAONeutralPassData>(
            "GTAONeutralVisibility",
            RenderGraphPassType::Graphics,
            [neutralVisibility](RenderGraphBuilder& builder, GTAONeutralPassData& data)
            {
                data.output = neutralVisibility;
                data.outputView = AddTextureRTV(
                    builder, data.output, "GTAONeutralVisibilityRTV");
                builder.SetExportState(data.output, RHIResourceState::ShaderResource);
            },
            [](const GTAONeutralPassData& data, RenderGraphPassContext& context)
            {
                RHITexture* const outputTexture = context.GetTexture(data.output);
                RHITextureView* const outputView = context.GetTextureView(data.outputView);
                if (outputTexture == nullptr || outputView == nullptr)
                {
                    context.MarkExecutionFailed(
                        "GTAO neutral visibility graph resource could not be realized");
                    return;
                }

                RHIRenderPassDesc renderPassDesc;
                renderPassDesc.AddColorAttachment(outputView, RHILoadOp::Clear,
                                                  RHIStoreOp::Store,
                                                  {1.0f, 1.0f, 1.0f, 1.0f});
                renderPassDesc.SetRenderArea(0, 0, outputTexture->GetWidth(),
                                             outputTexture->GetHeight());
                RHICommandContext& commands = context.Commands();
                commands.BeginRenderPass(renderPassDesc);
                commands.EndRenderPass();
            });

        const uint64 calculatedSettingsHash =
            CalculateRenderAmbientOcclusionSettingsHash(m_settings);
        constexpr uint64 RVX_NEUTRAL_AMBIENT_OCCLUSION_SETTINGS_HASH =
            0x4E45555452414C41ULL;
        const uint64 settingsHash = calculatedSettingsHash != 0
            ? calculatedSettingsHash
            : RVX_NEUTRAL_AMBIENT_OCCLUSION_SETTINGS_HASH;
        GroundTruthAmbientOcclusionOutput output;
        output.visibility = neutralVisibility;
        output.metadata.format = RHIFormat::R8_UNORM;
        output.metadata.consumerMask =
            RVX_RENDER_AMBIENT_OCCLUSION_INDIRECT_CONSUMER_MASK;
        output.metadata.settingsHash = settingsHash;
        output.metadata.ambientOcclusionFieldHash = HashAmbientOcclusionField(
            settingsHash, inputs.surfaceFieldHash, inputs.historyIdentity);
        output.metadata.candidateIdentity = inputs.historyIdentity;
        if (m_probeRequested && m_probeExpectedIdentity == inputs.historyIdentity)
        {
            m_lastStats.qualificationProbeRequested = true;
            m_lastStats.qualificationProbeFailure =
                "GTAO qualification probe requires an enabled GTAO candidate";
            ClearQualificationProbeState(true);
        }
        return output;
    }

    m_lastStats.depthAvailable = inputs.deviceDepth.IsValid();
    m_lastStats.normalSourceAvailable = inputs.normalSource.IsValid();
    m_lastStats.normalReconstructed = !inputs.normalSource.IsValid();
    m_lastStats.velocityAvailable = inputs.velocity.IsValid();
    m_lastStats.historyIdentity = inputs.historyIdentity;
    m_lastStats.historyResetReasons = inputs.historyResetReasons;
    m_lastStats.surfaceFieldHash = inputs.surfaceFieldHash;

    if (!IsValidRenderAmbientOcclusionSettings(m_settings))
    {
        RecordUnsupported("GTAO settings are invalid");
        return {};
    }

    const uint64 settingsHash = CalculateRenderAmbientOcclusionSettingsHash(m_settings);
    m_lastStats.settingsHash = settingsHash;
    if (settingsHash == 0 || inputs.surfaceFieldHash == 0 ||
        !inputs.historyIdentity.IsValid())
    {
        RecordUnsupported("GTAO requires valid settings, surface-field hash, and history identity");
        return {};
    }
    if (!HasPipelineResources())
    {
        RecordUnsupported("GTAO cooked pipeline family or descriptor layout is unavailable");
        return {};
    }
    m_lastStats.pipelineReady = true;

    if (!inputs.deviceDepth.IsValid())
    {
        RecordUnsupported("GTAO requires the current graph device-depth texture");
        return {};
    }
    const RHITextureDesc* const depthDesc = graph.GetTextureDesc(inputs.deviceDepth);
    if (depthDesc == nullptr || view.viewportWidth == 0 || view.viewportHeight == 0 ||
        !IsTexture2DCount1(*depthDesc, view.viewportWidth, view.viewportHeight,
                           RHIFormat::D32_FLOAT) ||
        !HasFlag(depthDesc->usage, RHITextureUsage::ShaderResource))
    {
        RecordUnsupported("GTAO requires a full-resolution shader-readable D32 device-depth texture");
        return {};
    }

    if (inputs.normalSource.IsValid())
    {
        const RHITextureDesc* const normalDesc = graph.GetTextureDesc(inputs.normalSource);
        if (normalDesc == nullptr ||
            !IsTexture2DCount1(*normalDesc, view.viewportWidth, view.viewportHeight,
                               RHIFormat::RGBA8_UNORM) ||
            !HasFlag(normalDesc->usage, RHITextureUsage::ShaderResource))
        {
            RecordUnsupported("GTAO normal source must be a shader-readable full-resolution GBuffer1 RGBA8_UNORM texture");
            return {};
        }
    }
    if (!inputs.velocity.IsValid())
    {
        RecordUnsupported(
            "Temporal GTAO requires the current graph velocity texture");
        return {};
    }
    const RHITextureDesc* const velocityDesc =
        graph.GetTextureDesc(inputs.velocity);
    if (velocityDesc == nullptr ||
        !IsTexture2DCount1(*velocityDesc, view.viewportWidth,
                           view.viewportHeight, RHIFormat::RG16_FLOAT) ||
        !HasFlag(velocityDesc->usage, RHITextureUsage::ShaderResource))
    {
        RecordUnsupported(
            "GTAO velocity source must be a shader-readable full-resolution RG16_FLOAT texture");
        return {};
    }
    if (!EnsureHistoryResources(view.viewportWidth, view.viewportHeight))
    {
        RecordUnsupported("GTAO persistent R8 visibility/R32 device-depth history resources are unavailable");
        return {};
    }
    RenderHistoryResetReason effectiveHistoryResetReasons =
        ApplyViewTemporalReset(view, inputs.historyResetReasons);
    const bool historySettingsMismatch = m_historyValid &&
        m_committedHistorySettingsHash != settingsHash;
    if (historySettingsMismatch)
    {
        effectiveHistoryResetReasons |=
            RenderHistoryResetReason::ConfigurationChanged;
        ResetHistory();
    }
    if (inputs.historyAvailable && view.previousViewProjectionValid == 0U)
    {
        effectiveHistoryResetReasons |=
            RenderHistoryResetReason::HistoryUnavailable;
    }
    if (effectiveHistoryResetReasons != RenderHistoryResetReason::None &&
        m_historyValid)
    {
        ResetHistory();
    }
    if (!m_historyValid &&
        effectiveHistoryResetReasons == RenderHistoryResetReason::None)
    {
        effectiveHistoryResetReasons |=
            RenderHistoryResetReason::HistoryUnavailable;
    }
    m_lastStats.historyResetReasons = effectiveHistoryResetReasons;

    AbandonPendingHistory();
    const bool useHistory = CanSampleHistory(
        inputs, view, m_historyValid, m_committedHistorySettingsHash,
        settingsHash, effectiveHistoryResetReasons);
    m_lastStats.historyAvailableBefore = useHistory;
    m_lastStats.historySampled = useHistory;
    const uint32 readHistoryIndex = m_committedHistoryIndex;
    const uint32 writeHistoryIndex = 1U - readHistoryIndex;
    const RHIResourceState historyInitialState = m_historyResourceStatesInitialized
        ? RHIResourceState::ShaderResource
        : RHIResourceState::Undefined;
    const RGTextureHandle historyVisibilityRead = graph.ImportTexture(
        m_historyVisibility[readHistoryIndex],
        historyInitialState,
        {"GTAOHistoryVisibilityRead", RGImportedResourceClass::History});
    const RGTextureHandle historyVisibilityWrite = graph.ImportTexture(
        m_historyVisibility[writeHistoryIndex],
        historyInitialState,
        {"GTAOHistoryVisibilityWrite", RGImportedResourceClass::History});
    const RGTextureHandle historyDepthRead = graph.ImportTexture(
        m_historyDepth[readHistoryIndex],
        historyInitialState,
        {"GTAOHistoryDepthRead", RGImportedResourceClass::History});
    const RGTextureHandle historyDepthWrite = graph.ImportTexture(
        m_historyDepth[writeHistoryIndex],
        historyInitialState,
        {"GTAOHistoryDepthWrite", RGImportedResourceClass::History});
    if (!historyVisibilityRead.IsValid() || !historyVisibilityWrite.IsValid() ||
        !historyDepthRead.IsValid() || !historyDepthWrite.IsValid())
    {
        RecordUnsupported("GTAO persistent history textures could not be imported into the render graph");
        return {};
    }

    RHITextureDesc transientDesc = RHITextureDesc::Texture2D(
        view.viewportWidth, view.viewportHeight, RHIFormat::R8_UNORM,
        RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource);
    transientDesc.debugName = "GTAOHorizon";
    const RGTextureHandle horizon = graph.CreateTexture(transientDesc);
    transientDesc.debugName = "GTAOBilateralHorizontal";
    const RGTextureHandle bilateralHorizontal = graph.CreateTexture(transientDesc);
    transientDesc.debugName = "GTAOBilateralVertical";
    const RGTextureHandle bilateralVertical = graph.CreateTexture(transientDesc);
    if (!horizon.IsValid() || !bilateralHorizontal.IsValid() ||
        !bilateralVertical.IsValid())
    {
        RecordUnsupported("GTAO transient R8 graph resources could not be created");
        return {};
    }

    GTAOGPUConstants constants;
    constants.renderSize[0] = static_cast<float>(view.viewportWidth);
    constants.renderSize[1] = static_cast<float>(view.viewportHeight);
    constants.renderSize[2] = 1.0f / constants.renderSize[0];
    constants.renderSize[3] = 1.0f / constants.renderSize[1];
    constants.depthParams[0] = std::max(view.nearPlane, 1.0e-4f);
    constants.depthParams[1] = std::max(view.farPlane, constants.depthParams[0] + 1.0e-3f);
    constants.depthParams[2] = m_pipelineCache->IsReverseZ() ? 1.0f : 0.0f;
    constants.depthParams[3] = inputs.normalSource.IsValid() ? 1.0f : 0.0f;
    constants.horizonParams[0] = m_settings.radiusMeters;
    constants.horizonParams[1] = m_settings.falloffStartMeters;
    constants.horizonParams[2] = m_settings.falloffEndMeters;
    constants.horizonParams[3] = m_settings.thicknessMeters;
    constants.samplingParams[0] = m_settings.directionCount;
    constants.samplingParams[1] = m_settings.stepCount;
    constants.samplingParams[2] = m_settings.spatialRadius;
    constants.filterParams[0] = m_settings.bilateralDepthSigma;
    constants.filterParams[1] = m_settings.bilateralNormalThreshold;
    constants.filterParams[2] = m_settings.temporalBlend;
    constants.filterParams[3] = useHistory ? 1.0f : 0.0f;
    constants.temporalParams[0] = 1.0f;
    constants.temporalParams[1] =
        m_settings.temporalDepthThresholdMeters;
    constants.temporalParams[2] = m_settings.temporalNormalThreshold;
    constants.temporalParams[3] = 1.0f;
    constants.cameraPosition[0] = view.cameraPosition.x;
    constants.cameraPosition[1] = view.cameraPosition.y;
    constants.cameraPosition[2] = view.cameraPosition.z;
    constants.cameraForward[0] = view.cameraForward.x;
    constants.cameraForward[1] = view.cameraForward.y;
    constants.cameraForward[2] = view.cameraForward.z;
    constants.inverseViewProjection =
        view.inverseJitteredViewProjectionMatrix;
    constants.previousInverseViewProjection = useHistory
        ? inverse(view.previousJitteredViewProjectionMatrix)
        : view.inverseJitteredViewProjectionMatrix;

    const auto executionState =
        std::make_shared<GroundTruthAmbientOcclusionExecutionState>();
    executionState->pendingHistoryIndex = writeHistoryIndex;
    executionState->pendingHistoryIdentity = inputs.historyIdentity;
    executionState->pendingHistorySettingsHash = settingsHash;
    m_pendingExecutionState = executionState;
    const RGTextureHandle normalSource = inputs.normalSource.IsValid()
        ? inputs.normalSource : inputs.deviceDepth;
    const RGTextureHandle velocity = inputs.velocity;
    const auto addStage = [this, &graph, inputs, normalSource, velocity,
                           historyVisibilityRead, historyDepthRead,
                           constants, executionState](
                              const char* passName,
                              GTAOStage stage,
                              RGTextureHandle inputAO,
                              RGTextureHandle output)
    {
        const RHIPipelineRef pipeline(
            ResolvePipeline(*m_pipelineCache, stage));
        const RHIPipelineLayoutRef pipelineLayout(
            m_pipelineCache->GetGTAOLayout());
        const RHIDescriptorSetLayoutRef setLayout(
            m_pipelineCache->GetGTAOSetLayout());
        const RHISamplerRef sampler = m_sampler;
        IRHIDevice* const device = m_device;
        graph.AddPass<GTAOPassData>(
            passName,
            RenderGraphPassType::Graphics,
            [inputs, normalSource, velocity, historyVisibilityRead,
             historyDepthRead, constants, executionState, stage, inputAO,
             output, device, pipeline, pipelineLayout, setLayout,
             sampler](RenderGraphBuilder& builder, GTAOPassData& data)
            {
                data.deviceDepth = builder.Read(
                    inputs.deviceDepth,
                    MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                     RHIShaderStage::Pixel));
                data.normalSource = builder.Read(
                    normalSource,
                    MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                     RHIShaderStage::Pixel));
                data.inputAO = builder.Read(
                    inputAO,
                    MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                     RHIShaderStage::Pixel));
                data.historyAO = builder.Read(
                    historyVisibilityRead,
                    MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                     RHIShaderStage::Pixel));
                data.velocity = builder.Read(
                    velocity,
                    MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                     RHIShaderStage::Pixel));
                data.historyDepth = builder.Read(
                    historyDepthRead,
                    MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                     RHIShaderStage::Pixel));
                data.output = output;
                data.deviceDepthView = AddTextureSRV(builder, data.deviceDepth,
                                                      "GTAODeviceDepthSRV");
                data.normalSourceView = AddTextureSRV(builder, data.normalSource,
                                                       "GTAONormalSourceSRV");
                data.inputAOView = AddTextureSRV(builder, data.inputAO,
                                                  "GTAOInputAOSRV");
                data.historyAOView = AddTextureSRV(builder, data.historyAO,
                                                    "GTAOHistoryAOSRV");
                data.velocityView = AddTextureSRV(builder, data.velocity,
                                                   "GTAOVelocitySRV");
                data.historyDepthView = AddTextureSRV(
                    builder, data.historyDepth, "GTAOHistoryDepthSRV");
                data.outputView = AddTextureRTV(builder, data.output,
                                                 "GTAOOutputRTV");
                data.constants = constants;
                data.stage = stage;
                data.executionState = executionState;
                data.device = device;
                data.pipeline = pipeline;
                data.pipelineLayout = pipelineLayout;
                data.setLayout = setLayout;
                data.sampler = sampler;
                data.resourcesRetained = data.device != nullptr &&
                    data.pipeline && data.pipelineLayout && data.setLayout &&
                    data.sampler && builder.RetainSubmissionResource(
                        Ref<RefCounted>(data.pipeline)) &&
                    builder.RetainSubmissionResource(
                        Ref<RefCounted>(data.pipelineLayout)) &&
                    builder.RetainSubmissionResource(
                        Ref<RefCounted>(data.setLayout)) &&
                    builder.RetainSubmissionResource(
                        Ref<RefCounted>(data.sampler));
                builder.SetExportState(data.output, RHIResourceState::ShaderResource);
            },
            [](const GTAOPassData& data, RenderGraphPassContext& context)
            {
                if (data.executionState->failed || !data.resourcesRetained ||
                    data.device == nullptr || !data.pipeline ||
                    !data.pipelineLayout || !data.setLayout || !data.sampler)
                {
                    data.executionState->failed = true;
                    context.MarkExecutionFailed(
                        "GTAO immutable recording resources are unavailable");
                    return;
                }

                RHITexture* const outputTexture = context.GetTexture(data.output);
                RHITextureView* const deviceDepthView =
                    context.GetTextureView(data.deviceDepthView);
                RHITextureView* const normalSourceView =
                    context.GetTextureView(data.normalSourceView);
                RHITextureView* const inputAOView =
                    context.GetTextureView(data.inputAOView);
                RHITextureView* const historyAOView =
                    context.GetTextureView(data.historyAOView);
                RHITextureView* const velocityView =
                    context.GetTextureView(data.velocityView);
                RHITextureView* const historyDepthView =
                    context.GetTextureView(data.historyDepthView);
                RHITextureView* const outputView = context.GetTextureView(data.outputView);
                if (outputTexture == nullptr || deviceDepthView == nullptr ||
                    normalSourceView == nullptr ||
                    inputAOView == nullptr || historyAOView == nullptr ||
                    velocityView == nullptr || historyDepthView == nullptr ||
                    outputView == nullptr)
                {
                    data.executionState->failed = true;
                    context.MarkExecutionFailed("GTAO graph resources could not be realized");
                    return;
                }

                RHIBufferRef constants = CreateImmutablePassConstants(
                    *data.device, data.constants,
                    GetStageConstantsDebugName(data.stage));
                if (!constants ||
                    !context.RetainSubmissionResource(Ref<RefCounted>(constants)))
                {
                    data.executionState->failed = true;
                    context.MarkExecutionFailed("GTAO immutable constants could not be retained");
                    return;
                }

                RHIDescriptorSetDesc descriptorDesc;
                descriptorDesc.layout = data.setLayout.Get();
                descriptorDesc.debugName = GetStageDescriptorDebugName(data.stage);
                descriptorDesc.BindBuffer(0, constants.Get(), 0,
                                          AlignConstantBufferSize(sizeof(GTAOGPUConstants)));
                descriptorDesc.BindTexture(1, deviceDepthView);
                descriptorDesc.BindTexture(2, normalSourceView);
                descriptorDesc.BindTexture(3, inputAOView);
                descriptorDesc.BindTexture(4, historyAOView);
                descriptorDesc.BindTexture(5, velocityView);
                descriptorDesc.BindTexture(6, historyDepthView);
                descriptorDesc.BindSampler(7, data.sampler.Get());
                RHIDescriptorSetRef descriptorSet =
                    data.device->CreateDescriptorSet(descriptorDesc);
                if (!descriptorSet ||
                    !descriptorSet->IsReadyForBinding(data.setLayout.Get()) ||
                    !context.RetainSubmissionResource(Ref<RefCounted>(descriptorSet)))
                {
                    data.executionState->failed = true;
                    context.MarkExecutionFailed("GTAO descriptor set creation failed");
                    return;
                }

                RHIRenderPassDesc renderPassDesc;
                renderPassDesc.AddColorAttachment(outputView, RHILoadOp::DontCare,
                                                  RHIStoreOp::Store);
                renderPassDesc.SetRenderArea(0, 0, outputTexture->GetWidth(),
                                             outputTexture->GetHeight());
                RHICommandContext& commands = context.Commands();
                commands.BeginRenderPass(renderPassDesc);
                commands.SetPipeline(data.pipeline.Get());
                commands.SetDescriptorSet(0, descriptorSet.Get());
                RHIViewport viewport;
                viewport.width = static_cast<float>(outputTexture->GetWidth());
                viewport.height = static_cast<float>(outputTexture->GetHeight());
                viewport.minDepth = 0.0f;
                viewport.maxDepth = 1.0f;
                commands.SetViewport(viewport);
                RHIRect scissor;
                scissor.width = outputTexture->GetWidth();
                scissor.height = outputTexture->GetHeight();
                commands.SetScissor(scissor);
                commands.Draw(3, 1, 0, 0);
                commands.EndRenderPass();

                ++data.executionState->completedPassCount;
            });
    };

    addStage("GTAOSurfacePreparation", GTAOStage::Surface,
             inputs.deviceDepth, historyDepthWrite);
    addStage("GTAOHorizonSearch", GTAOStage::Horizon,
             historyDepthWrite, horizon);
    addStage("GTAOBilateralHorizontal", GTAOStage::BilateralHorizontal,
             horizon, bilateralHorizontal);
    addStage("GTAOBilateralVertical", GTAOStage::BilateralVertical,
             bilateralHorizontal, bilateralVertical);
    addStage("GTAOTemporalResolve", GTAOStage::Temporal,
             bilateralVertical, historyVisibilityWrite);

    // The only future-visible output is the persistent temporal candidate.
    // It becomes a reusable history resource only at CommitHistoryAfterPresent.
    m_pendingHistoryIndex = writeHistoryIndex;
    m_pendingHistoryIdentity = {};
    m_hasPendingHistory = false;
    m_pendingHistorySubmissionAccepted = false;
    m_lastStats.inputsReady = true;
    m_lastStats.surfacePreparationGraphPassCount = 1;
    m_lastStats.horizonSearchGraphPassCount = 1;
    m_lastStats.bilateralDenoiseGraphPassCount = 2;
    m_lastStats.temporalResolveGraphPassCount = 1;
    m_lastStats.ambientOcclusionFieldHash = HashAmbientOcclusionField(
        settingsHash, inputs.surfaceFieldHash, inputs.historyIdentity);

    GroundTruthAmbientOcclusionOutput output;
    output.visibility = historyVisibilityWrite;
    output.metadata.format = RHIFormat::R8_UNORM;
    output.metadata.consumerMask = m_settings.consumerMask;
    output.metadata.settingsHash = settingsHash;
    output.metadata.ambientOcclusionFieldHash =
        m_lastStats.ambientOcclusionFieldHash;
    output.metadata.candidateIdentity = inputs.historyIdentity;
    output.metadata.normalSourceUsed = inputs.normalSource.IsValid();
    output.metadata.normalReconstructed = !inputs.normalSource.IsValid();

    // A qualification probe is strictly opt-in and reads the actual final R8
    // visibility texture. The separate Copy pass preserves the graph's
    // ShaderResource export for lighting and records an exact Graphics-owned
    // completion point for post-fence CPU observation.
    if (m_probeRequested)
    {
        m_lastStats.qualificationProbeRequested = true;
        if (m_probeExpectedIdentity != inputs.historyIdentity)
        {
            m_lastStats.qualificationProbeFailure =
                "GTAO qualification probe identity does not match the candidate";
            ClearQualificationProbeState(true);
        }
        else if (!EnsureQualificationReadback(view.viewportWidth, view.viewportHeight))
        {
            m_lastStats.qualificationProbeFailure =
                "GTAO qualification readback allocation failed";
            ClearQualificationProbeState(true);
        }
        else
        {
            const RGBufferHandle probeReadback = graph.ImportBuffer(
                RHIBufferRef(m_probeReadback), RHIResourceState::CopyDest);
            if (!probeReadback.IsValid())
            {
                m_lastStats.qualificationProbeFailure =
                    "GTAO qualification readback import failed";
                ClearQualificationProbeState(true);
            }
            else
            {
                executionState->probeExpected = true;
                const RHIBufferRef readbackOwner = m_probeReadback;
                graph.AddPass<GTAOQualificationProbeCopyData>(
                    "GTAOQualificationProbeCopy",
                    RenderGraphPassType::Copy,
                    RenderGraphQueueAffinity::Graphics,
                    [visibility = output.visibility, probeReadback, readbackOwner,
                     identity = inputs.historyIdentity,
                     settingsHash = output.metadata.settingsHash,
                     fieldHash = output.metadata.ambientOcclusionFieldHash,
                     width = view.viewportWidth, height = view.viewportHeight,
                     rowPitch = m_probeRowPitch, executionState](
                        RenderGraphBuilder& builder,
                        GTAOQualificationProbeCopyData& data)
                    {
                        data.visibility = builder.Read(
                            visibility,
                            MakeRGAccessDesc(RHIResourceState::CopySource,
                                             RHIShaderStage::All));
                        data.readback = builder.Write(
                            probeReadback,
                            MakeRGAccessDesc(RHIResourceState::CopyDest,
                                             RHIShaderStage::All));
                        builder.SetExportState(data.visibility,
                                               RHIResourceState::ShaderResource);
                        data.readbackOwner = readbackOwner;
                        data.identity = identity;
                        data.settingsHash = settingsHash;
                        data.ambientOcclusionFieldHash = fieldHash;
                        data.width = width;
                        data.height = height;
                        data.rowPitch = rowPitch;
                        data.executionState = executionState;
                    },
                    [](const GTAOQualificationProbeCopyData& data,
                       RenderGraphPassContext& context)
                    {
                        RHITexture* const visibility =
                            context.GetTexture(data.visibility);
                        RHIBuffer* const readback = context.GetBuffer(data.readback);
                        if (visibility == nullptr || readback == nullptr ||
                            !data.readbackOwner || !data.executionState ||
                            readback != data.readbackOwner.Get() ||
                            !context.RetainSubmissionResource(
                                Ref<RefCounted>(data.readbackOwner)))
                        {
                            context.MarkExecutionFailed(
                                "GTAO qualification readback resources are unavailable");
                            if (data.executionState)
                            {
                                data.executionState->failed = true;
                            }
                            return;
                        }

                        RHIBufferTextureCopyDesc copyDesc;
                        copyDesc.bufferRowPitch = data.rowPitch;
                        copyDesc.bufferImageHeight = data.height;
                        copyDesc.textureRegion = {
                            0, 0, data.width, data.height};
                        context.Commands().CopyTextureToBuffer(
                            visibility, readback, copyDesc);
                        data.executionState->probeRecorded = true;
                        data.executionState->probeIdentity = data.identity;
                        data.executionState->probeSettingsHash =
                            data.settingsHash;
                        data.executionState->probeAmbientOcclusionFieldHash =
                            data.ambientOcclusionFieldHash;
                    });
                m_probeScheduled = true;
            }
        }
    }
    return output;
}

void GroundTruthAmbientOcclusion::FinalizeGraphExecution(
    bool executionAccepted) noexcept
{
    const std::shared_ptr<GroundTruthAmbientOcclusionExecutionState> state =
        std::move(m_pendingExecutionState);
    if (!state)
    {
        if (!executionAccepted && !m_probeSubmissionAccepted &&
            (m_probeRequested || m_probeScheduled || m_probeRecorded))
        {
            m_lastStats.qualificationProbeFailure =
                "GTAO graph execution evidence was not accepted";
            ClearQualificationProbeState(true);
        }
        return;
    }

    const bool probeExecutionComplete =
        !state->probeExpected || state->probeRecorded;
    if (!executionAccepted || state->failed ||
        state->completedPassCount != 5U || !probeExecutionComplete)
    {
        if (!m_probeSubmissionAccepted &&
            (m_probeRequested || m_probeScheduled || m_probeRecorded))
        {
            if (m_lastStats.qualificationProbeFailure.empty())
            {
                m_lastStats.qualificationProbeFailure =
                    !executionAccepted
                    ? "GTAO graph execution evidence was not accepted"
                    : "GTAO qualification graph pass did not complete";
            }
            ClearQualificationProbeState(true);
        }
        m_hasPendingHistory = false;
        m_pendingHistorySubmissionAccepted = false;
        m_pendingHistoryIdentity = {};
        m_pendingHistorySettingsHash = 0;
        m_lastStats.executed = false;
        m_lastStats.historyCandidatePrepared = false;
        return;
    }

    m_pendingHistoryIndex = state->pendingHistoryIndex;
    m_pendingHistoryIdentity = state->pendingHistoryIdentity;
    m_pendingHistorySettingsHash = state->pendingHistorySettingsHash;
    // Recording callbacks only prove that commands were encoded. Persistent
    // import state becomes reusable together with semantic history after the
    // exact submission reaches successful presentation.
    m_hasPendingHistory = true;
    m_pendingHistorySubmissionAccepted = false;
    m_lastStats.executed = true;
    m_lastStats.historyCandidatePrepared = true;

    if (state->probeRecorded)
    {
        m_probeScheduled = false;
        m_probeRecorded = true;
        m_probeIdentity = state->probeIdentity;
        m_probeSettingsHash = state->probeSettingsHash;
        m_probeAmbientOcclusionFieldHash =
            state->probeAmbientOcclusionFieldHash;
        m_lastStats.qualificationProbeRecorded = true;
    }
}

bool GroundTruthAmbientOcclusion::CanCommitHistoryAfterPresent(
    const RenderViewHistoryIdentity& identity) const noexcept
{
    return m_hasPendingHistory && m_pendingHistorySubmissionAccepted &&
           identity.IsValid() &&
           identity == m_pendingHistoryIdentity;
}

bool GroundTruthAmbientOcclusion::NotifyHistorySubmission(
    const RenderViewHistoryIdentity& identity) noexcept
{
    if (!m_hasPendingHistory || !identity.IsValid() ||
        identity != m_pendingHistoryIdentity)
    {
        return false;
    }

    // Unlike semantic history validity, physical resource state advances at
    // the exact GPU submission boundary. A later Present failure discards the
    // candidate contents but must not relabel the already submitted barriers.
    m_historyResourceStatesInitialized = true;
    m_pendingHistorySubmissionAccepted = true;
    return true;
}

bool GroundTruthAmbientOcclusion::CommitHistoryAfterPresent(
    const RenderViewHistoryIdentity& identity) noexcept
{
    if (!CanCommitHistoryAfterPresent(identity))
    {
        return false;
    }

    m_committedHistoryIndex = m_pendingHistoryIndex;
    m_committedHistorySettingsHash = m_pendingHistorySettingsHash;
    m_historyResourceStatesInitialized = true;
    m_historyValid = true;
    m_hasPendingHistory = false;
    m_pendingHistorySubmissionAccepted = false;
    m_pendingHistoryIdentity = {};
    m_pendingHistorySettingsHash = 0;
    m_lastStats.historyCommitted = true;
    return true;
}

void GroundTruthAmbientOcclusion::AbandonPendingHistory() noexcept
{
    const bool hadPendingHistory = m_hasPendingHistory;
    m_hasPendingHistory = false;
    m_pendingHistorySubmissionAccepted = false;
    m_pendingHistoryIdentity = {};
    m_pendingHistorySettingsHash = 0;
    if (hadPendingHistory)
    {
        m_lastStats.historyCandidatePrepared = true;
        m_lastStats.historyAbandoned = true;
    }
}

void GroundTruthAmbientOcclusion::ResetHistory() noexcept
{
    m_historyValid = false;
    m_committedHistoryIndex = 0;
    m_pendingHistoryIndex = 0;
    m_committedHistorySettingsHash = 0;
    m_pendingHistorySettingsHash = 0;
    m_hasPendingHistory = false;
    m_pendingHistorySubmissionAccepted = false;
    m_pendingHistoryIdentity = {};
    m_pendingExecutionState.reset();
}

bool GroundTruthAmbientOcclusion::ArmQualificationProbe(
    const RenderViewHistoryIdentity& identity) noexcept
{
    if (!identity.IsValid() || m_probeRequested || m_probeScheduled ||
        m_probeRecorded || m_probeSubmissionAccepted)
    {
        return false;
    }

    m_probeRequested = true;
    m_probeExpectedIdentity = identity;
    m_probeAbandoned = false;
    m_lastStats.qualificationProbeRequested = true;
    m_lastStats.qualificationProbeRecorded = false;
    m_lastStats.qualificationProbeReady = false;
    m_lastStats.qualificationProbeFailure.clear();
    return true;
}

bool GroundTruthAmbientOcclusion::NotifyQualificationProbeSubmission(
    const RenderViewHistoryIdentity& identity,
    const GPUCompletionToken& completion,
    const RenderSubmissionTracker& tracker) noexcept
{
    if (!m_probeRecorded || m_probeSubmissionAccepted || !m_probeReadback ||
        !identity.IsValid() || identity != m_probeIdentity)
    {
        return false;
    }

    GPUCompletionPoint graphicsPoint;
    if (!IsExactCurrentGraphicsSubmissionToken(tracker, completion, graphicsPoint))
    {
        m_lastStats.qualificationProbeFailure =
            "GTAO qualification probe submission token is not the exact Graphics completion";
        return false;
    }

    m_probeCompletionValue = graphicsPoint.value;
    m_probeSubmissionAccepted = true;
    return true;
}

bool GroundTruthAmbientOcclusion::PollQualificationProbe(
    const RenderSubmissionTracker& tracker,
    GroundTruthAmbientOcclusionQualificationProbe& outProbe) noexcept
{
    outProbe = {};
    if (!m_probeSubmissionAccepted || !m_probeReadback ||
        !m_probeIdentity.IsValid() || m_probeCompletionValue == 0)
    {
        return false;
    }

    const GPUCompletionPoint completionPoint{
        GPUQueueDomain::Graphics, m_probeCompletionValue};
    const GPUCompletionStatus status = tracker.Query(completionPoint);
    if (status == GPUCompletionStatus::Pending)
    {
        return false;
    }
    if (!IsCompletionSatisfied(status))
    {
        m_lastStats.qualificationProbeFailure =
            "GTAO qualification probe completion was lost";
        ClearQualificationProbeState(true);
        return false;
    }
    if (m_probeAbandoned)
    {
        ClearQualificationProbeState(true);
        return false;
    }

    const uint64 sampleCount = static_cast<uint64>(m_probeWidth) *
        static_cast<uint64>(m_probeHeight);
    const uint64 requiredBytes = static_cast<uint64>(m_probeRowPitch) *
        static_cast<uint64>(m_probeHeight);
    if (sampleCount == 0 || m_probeRowPitch < m_probeWidth ||
        requiredBytes > m_probeReadback->GetSize())
    {
        m_lastStats.qualificationProbeFailure =
            "GTAO qualification readback layout is invalid";
        ClearQualificationProbeState(true);
        return false;
    }

    void* const mapped = m_probeReadback->Map();
    if (mapped == nullptr)
    {
        m_lastStats.qualificationProbeFailure =
            "GTAO qualification readback map failed";
        ClearQualificationProbeState(true);
        return false;
    }

    const auto* const samples = static_cast<const uint8*>(mapped);
    uint64 visibilitySum = 0;
    uint64 occludedCount = 0;
    uint8 minimum = std::numeric_limits<uint8>::max();
    uint8 maximum = 0;
    for (uint32 y = 0; y < m_probeHeight; ++y)
    {
        const uint8* const row = samples + static_cast<uint64>(y) *
            static_cast<uint64>(m_probeRowPitch);
        for (uint32 x = 0; x < m_probeWidth; ++x)
        {
            const uint8 visibility = row[x];
            visibilitySum += visibility;
            minimum = std::min(minimum, visibility);
            maximum = std::max(maximum, visibility);
            if (visibility < std::numeric_limits<uint8>::max())
            {
                ++occludedCount;
            }
        }
    }
    m_probeReadback->Unmap();

    constexpr float32 inverseUNorm8 = 1.0f / 255.0f;
    outProbe.requested = true;
    outProbe.recorded = true;
    outProbe.ready = true;
    outProbe.historyIdentity = m_probeIdentity;
    outProbe.settingsHash = m_probeSettingsHash;
    outProbe.ambientOcclusionFieldHash = m_probeAmbientOcclusionFieldHash;
    outProbe.sampleCount = sampleCount;
    outProbe.occludedCount = occludedCount;
    outProbe.minimumVisibility = static_cast<float32>(minimum) * inverseUNorm8;
    outProbe.meanVisibility = static_cast<float32>(visibilitySum) /
        (static_cast<float32>(sampleCount) * 255.0f);
    outProbe.maximumVisibility = static_cast<float32>(maximum) * inverseUNorm8;
    const bool valid = outProbe.IsValid();
    if (!valid)
    {
        outProbe = {};
        m_lastStats.qualificationProbeFailure =
            "GTAO qualification readback produced invalid visibility statistics";
        ClearQualificationProbeState(true);
        return false;
    }

    m_lastStats.qualificationProbeReady = true;
    ClearQualificationProbeState(true);
    return true;
}

void GroundTruthAmbientOcclusion::AbandonQualificationProbe() noexcept
{
    if (m_probeSubmissionAccepted)
    {
        // A submitted GPU copy may still access this buffer. Retain it until
        // PollQualificationProbe observes the exact completion point.
        m_probeAbandoned = true;
        return;
    }
    ClearQualificationProbeState(true);
}

bool GroundTruthAmbientOcclusion::HasPendingQualificationProbe() const noexcept
{
    return m_probeRequested || m_probeScheduled || m_probeRecorded ||
           m_probeSubmissionAccepted;
}

void GroundTruthAmbientOcclusion::Shutdown() noexcept
{
    ResetRuntimeResources();
    m_pipelineCache = nullptr;
    m_device = nullptr;
    m_lastStats = {};
}

RenderHistoryResetReason GroundTruthAmbientOcclusion::ApplyViewTemporalReset(
    const ViewData& view,
    RenderHistoryResetReason resetReasons) noexcept
{
    return view.resetTemporalHistory
        ? resetReasons | RenderHistoryResetReason::ExplicitDiscontinuity
        : resetReasons;
}

bool GroundTruthAmbientOcclusion::CanSampleHistory(
    const GroundTruthAmbientOcclusionFrameInputs& inputs,
    const ViewData& view,
    bool historyValid,
    uint64 committedHistorySettingsHash,
    uint64 settingsHash,
    RenderHistoryResetReason effectiveResetReasons) noexcept
{
    return inputs.historyAvailable && historyValid &&
           inputs.velocity.IsValid() &&
           !view.resetTemporalHistory &&
           view.previousViewProjectionValid != 0U &&
           committedHistorySettingsHash == settingsHash &&
           effectiveResetReasons == RenderHistoryResetReason::None;
}

bool GroundTruthAmbientOcclusion::HasPipelineResources() const noexcept
{
    return m_pipelineCache != nullptr && m_pipelineCache->IsInitialized() &&
           m_device != nullptr && m_pipelineCache->GetGTAOSurfacePipeline() != nullptr &&
           m_pipelineCache->GetGTAOHorizonPipeline() != nullptr &&
           m_pipelineCache->GetGTAOBilateralHorizontalPipeline() != nullptr &&
           m_pipelineCache->GetGTAOBilateralVerticalPipeline() != nullptr &&
           m_pipelineCache->GetGTAOTemporalPipeline() != nullptr &&
           m_pipelineCache->GetGTAOLayout() != nullptr &&
           m_pipelineCache->GetGTAOSetLayout() != nullptr;
}

bool GroundTruthAmbientOcclusion::EnsureHistoryResources(uint32 width, uint32 height)
{
    if (m_device == nullptr || width == 0 || height == 0)
    {
        return false;
    }
    if (m_width == width && m_height == height && m_historyVisibility[0] &&
        m_historyVisibility[1] && m_historyDepth[0] && m_historyDepth[1] &&
        m_sampler)
    {
        return true;
    }

    ResetRuntimeResources();
    m_width = width;
    m_height = height;
    RHITextureDesc desc = RHITextureDesc::Texture2D(
        width, height, RHIFormat::R8_UNORM,
        RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource);
    desc.debugName = "GTAOHistoryVisibility";
    m_historyVisibility[0] = m_device->CreateTexture(desc);
    m_historyVisibility[1] = m_device->CreateTexture(desc);
    desc.format = RHIFormat::R32_FLOAT;
    desc.debugName = "GTAOHistoryDepth";
    m_historyDepth[0] = m_device->CreateTexture(desc);
    m_historyDepth[1] = m_device->CreateTexture(desc);
    RHISamplerDesc samplerDesc = RHISamplerDesc::LinearClamp();
    samplerDesc.debugName = "GTAOLinearClampSampler";
    m_sampler = m_device->CreateSampler(samplerDesc);
    if (!m_historyVisibility[0] || !m_historyVisibility[1] ||
        !m_historyDepth[0] || !m_historyDepth[1] || !m_sampler)
    {
        ResetRuntimeResources();
        return false;
    }
    return true;
}

bool GroundTruthAmbientOcclusion::EnsureQualificationReadback(
    uint32 width,
    uint32 height)
{
    if (m_device == nullptr || width == 0 || height == 0)
    {
        return false;
    }

    const uint32 rowPitch = AlignReadbackRowPitch(
        m_device->GetBackendType(), width);
    if (rowPitch == 0 || m_probeSubmissionAccepted ||
        static_cast<uint64>(height) >
            std::numeric_limits<uint64>::max() / static_cast<uint64>(rowPitch))
    {
        return false;
    }
    const uint64 size = static_cast<uint64>(rowPitch) * static_cast<uint64>(height);
    if (size == 0)
    {
        return false;
    }
    if (m_probeReadback && m_probeWidth == width && m_probeHeight == height &&
        m_probeRowPitch == rowPitch && m_probeReadback->GetSize() == size)
    {
        return true;
    }

    RHIBufferDesc bufferDesc;
    bufferDesc.size = size;
    bufferDesc.usage = RHIBufferUsage::CopyDst;
    bufferDesc.memoryType = RHIMemoryType::Readback;
    bufferDesc.debugName = "GTAOQualificationVisibilityReadback";
    RHIBufferRef readback = m_device->CreateBuffer(bufferDesc);
    if (!readback || readback->GetSize() != size)
    {
        return false;
    }

    m_probeReadback = std::move(readback);
    m_probeWidth = width;
    m_probeHeight = height;
    m_probeRowPitch = rowPitch;
    return true;
}

void GroundTruthAmbientOcclusion::ClearQualificationProbeState(
    bool retainReadback) noexcept
{
    m_probeExpectedIdentity = {};
    m_probeIdentity = {};
    m_probeSettingsHash = 0;
    m_probeAmbientOcclusionFieldHash = 0;
    m_probeCompletionValue = 0;
    m_probeRequested = false;
    m_probeScheduled = false;
    m_probeRecorded = false;
    m_probeSubmissionAccepted = false;
    m_probeAbandoned = false;
    if (!retainReadback)
    {
        m_probeReadback.Reset();
        m_probeWidth = 0;
        m_probeHeight = 0;
        m_probeRowPitch = 0;
    }
}

void GroundTruthAmbientOcclusion::ResetRuntimeResources() noexcept
{
    m_historyVisibility[0].Reset();
    m_historyVisibility[1].Reset();
    m_historyDepth[0].Reset();
    m_historyDepth[1].Reset();
    m_sampler.Reset();
    ClearQualificationProbeState(false);
    m_width = 0;
    m_height = 0;
    m_historyResourceStatesInitialized = false;
    ResetHistory();
}

void GroundTruthAmbientOcclusion::RecordUnsupported(const char* reason) noexcept
{
    m_lastStats.pipelineReady = HasPipelineResources();
    m_lastStats.inputsReady = false;
    m_lastStats.executed = false;
    m_lastStats.fallbackReason = reason ? reason : "GTAO is unavailable";
}
} // namespace RVX
