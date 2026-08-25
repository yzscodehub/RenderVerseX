/**
 * @file Exposure.cpp
 * @brief GPU-owned histogram metering and presentation-qualified exposure state.
 */

#include "Render/PostProcess/Exposure.h"

#include "Core/Log.h"
#include "Render/PipelineCache.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>

namespace RVX
{
namespace
{
    constexpr uint64 RVX_EXPOSURE_CONSTANT_BUFFER_ALIGNMENT = 256u;
    constexpr uint32 RVX_EXPOSURE_STATE_VALID = 1u;

    [[nodiscard]] uint64 AlignConstantBufferSize(uint64 size) noexcept
    {
        return (size + RVX_EXPOSURE_CONSTANT_BUFFER_ALIGNMENT - 1u) &
            ~(RVX_EXPOSURE_CONSTANT_BUFFER_ALIGNMENT - 1u);
    }

    [[nodiscard]] RGTextureViewHandle AddComputeTextureSRV(
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
        viewDesc.type = RHITextureViewType::ShaderResource;
        viewDesc.debugName = debugName;
        return builder.Read(
            builder.CreateTextureView(texture, viewDesc),
            MakeRGAccessDesc(RHIResourceState::ShaderResource,
                             RHIShaderStage::Compute));
    }

    [[nodiscard]] uint64 AppendExposureHash(uint64 hash, uint64 value) noexcept
    {
        constexpr uint64 prime = 1099511628211ULL;
        hash ^= value;
        hash *= prime;
        return hash;
    }

    [[nodiscard]] uint64 CalculateExposureSemanticHash(
        uint64 configurationHash,
        const RenderViewHistoryIdentity& identity,
        uint64 frameIndex,
        uint32 stateSlot,
        uint64 priorStateHash) noexcept
    {
        constexpr uint64 offsetBasis = 14695981039346656037ULL;
        uint64 hash = offsetBasis;
        hash = AppendExposureHash(hash, configurationHash);
        hash = AppendExposureHash(hash, identity.previousFrame.frameSequence);
        hash = AppendExposureHash(hash, identity.previousFrame.sceneRevision);
        hash = AppendExposureHash(hash, identity.previousFrame.surfaceGeneration);
        hash = AppendExposureHash(hash, identity.previousFrame.configurationRevision);
        hash = AppendExposureHash(hash, identity.view.sceneRuntimeId);
        hash = AppendExposureHash(hash, identity.view.cameraEntityPacked);
        hash = AppendExposureHash(hash, identity.view.subViewIndex);
        hash = AppendExposureHash(hash, static_cast<uint64>(identity.view.role));
        hash = AppendExposureHash(hash, identity.extent.width);
        hash = AppendExposureHash(hash, identity.extent.height);
        hash = AppendExposureHash(hash, identity.familyId);
        hash = AppendExposureHash(hash, identity.worldRevision);
        hash = AppendExposureHash(hash, identity.temporalEpoch);
        hash = AppendExposureHash(hash, identity.formatRevision);
        hash = AppendExposureHash(hash, identity.surfaceGeneration);
        hash = AppendExposureHash(hash, identity.cameraCutRevision);
        hash = AppendExposureHash(hash, frameIndex);
        hash = AppendExposureHash(hash, stateSlot);
        hash = AppendExposureHash(hash, priorStateHash);
        return hash != 0 ? hash : 1u;
    }
} // namespace

struct alignas(16) ExposureController::GPUState
{
    float exposureValues[4] = {1.0f, 1.0f, 1.0f, 0.0f};
    uint32 targetAndNonce[4] = {0u, 0u, 0u, 0u};
    uint32 stateMeta[4] = {0u, 0u, 0u, RVX_EXPOSURE_STATE_VALID};
};
// GPUState is intentionally 3 x 16-byte vectors; allocation uses the public
// 48-byte ABI constant because this private nested type cannot be named by an
// out-of-class static assertion under C++ access rules.

struct ExposureController::GPUConstants
{
    uint32 renderExtent[2] = {1u, 1u};
    float fixedDeltaSeconds = 0.0f;
    uint32 exposureMode = 0u;

    float manualEV100 = 0.0f;
    float exposureCompensationEV = 0.0f;
    float histogramLowPercentile = 0.5f;
    float histogramHighPercentile = 0.95f;

    float histogramKeyValue = 0.18f;
    float minEV100 = RVX_RENDER_EXPOSURE_MIN_EV100;
    float maxEV100 = RVX_RENDER_EXPOSURE_MAX_EV100;
    float brighteningAdaptationSpeed = 3.0f;

    float darkeningAdaptationSpeed = 1.0f;
    uint32 configurationHashLo = 0u;
    uint32 configurationHashHi = 0u;
    uint32 forceHistoryReset = 1u;

    uint32 candidateStateContentHashLo = 0u;
    uint32 candidateStateContentHashHi = 0u;
    uint32 padding[2] = {0u, 0u};
};

struct ExposureController::FrameRecord
{
    ExposureControllerStats stats{};
    RHIBufferRef expectedCommittedState;
    RHIBufferRef candidateState;
    RHIBufferRef histogram;
    uint32 candidateStateIndex = RVX_INVALID_INDEX;
    bool forceHistoryReset = true;
    bool qualificationProbeRequested = false;
};

ExposureController::~ExposureController()
{
    Shutdown();
}

void ExposureController::SetResources(PipelineCache* pipelineCache)
{
    IRHIDevice* const device = pipelineCache ? pipelineCache->GetDevice() : nullptr;
    if (pipelineCache == m_pipelineCache && device == m_device)
    {
        return;
    }

    ResetRuntimeResources();
    m_pipelineCache = pipelineCache;
    m_device = device;
    if (m_device != nullptr && !EnsureRuntimeResources())
    {
        m_lastStats = {};
        m_lastStats.fallbackReason =
            "Exposure controller could not create persistent state buffers";
    }
}

void ExposureController::Shutdown() noexcept
{
    ResetRuntimeResources();
    m_pipelineCache = nullptr;
    m_device = nullptr;
}

bool ExposureController::IsSupported() const noexcept
{
    return m_device != nullptr && m_bootstrapState && m_stateBuffers[0] &&
        m_stateBuffers[1] && m_histogramBuffer && HasPipelineResources();
}

RHIBuffer* ExposureController::GetCommittedStateBuffer() const noexcept
{
    return GetCommittedStateBufferRef().Get();
}

RHIBufferRef ExposureController::GetCommittedStateBufferRef() const noexcept
{
    if (m_committedStateIndex < 2u && m_stateBuffers[m_committedStateIndex])
    {
        return m_stateBuffers[m_committedStateIndex];
    }
    return m_bootstrapState;
}

uint64 ExposureController::GetCommittedConfigurationHash() const noexcept
{
    return m_committedConfigurationHash;
}

const ExposureControllerStats& ExposureController::GetLastStats() const noexcept
{
    return m_lastStats;
}

ExposureFrameInput ExposureController::PrepareFrameInput(
    const RenderExposureSettings& settings,
    const RenderViewHistoryIdentity& historyIdentity,
    bool historyAvailable,
    RenderHistoryResetReason historyResetReasons)
{
    m_preparedFrameInput = {};
    m_hasPreparedFrameInput = false;
    if (!historyIdentity.IsValid() || !IsValidRenderExposureSettings(settings) ||
        !EnsureRuntimeResources())
    {
        return {};
    }

    const uint64 configurationHash = CalculateRenderExposureSettingsHash(settings);
    if (configurationHash == 0)
    {
        return {};
    }

    RenderHistoryResetReason effectiveResetReasons = historyResetReasons;
    if (!historyAvailable)
    {
        effectiveResetReasons |= RenderHistoryResetReason::HistoryUnavailable;
    }
    const bool configurationChanged =
        m_committedConfigurationHash != configurationHash;
    if (configurationChanged)
    {
        effectiveResetReasons |= RenderHistoryResetReason::ConfigurationChanged;
    }
    const bool reset = !historyAvailable ||
        effectiveResetReasons != RenderHistoryResetReason::None;
    RHIBufferRef selected = reset ? m_bootstrapState : GetCommittedStateBufferRef();
    if (!selected)
    {
        return {};
    }

    const uint64 priorStateHash = reset ? 0u : m_committedStateContentHash;
    ExposureFrameInput input;
    input.stateBuffer = std::move(selected);
    input.configurationHash = configurationHash;
    input.contentHash = CalculateExposureSemanticHash(
        configurationHash,
        historyIdentity,
        0u,
        reset ? RVX_INVALID_INDEX : m_committedStateIndex,
        priorStateHash);
    input.historyReset = reset;
    input.resetReasons = effectiveResetReasons;
    m_preparedFrameInput = input;
    m_preparedHistoryIdentity = historyIdentity;
    m_hasPreparedFrameInput = true;
    return input;
}

const ExposureFrameInput* ExposureController::FindPreparedFrameInput(
    const RenderViewHistoryIdentity& historyIdentity,
    uint64 configurationHash) const noexcept
{
    if (!m_hasPreparedFrameInput || !m_preparedFrameInput.IsValid() ||
        m_preparedHistoryIdentity != historyIdentity ||
        m_preparedFrameInput.configurationHash != configurationHash)
    {
        return nullptr;
    }
    return &m_preparedFrameInput;
}

bool ExposureController::HasPipelineResources() const noexcept
{
    return m_pipelineCache != nullptr && m_pipelineCache->IsInitialized() &&
        m_pipelineCache->GetExposureComputeSetLayout() != nullptr &&
        m_pipelineCache->IsExposureComputePipelineReady();
}

bool ExposureController::EnsureRuntimeResources()
{
    static_assert(sizeof(GPUState) == RVX_EXPOSURE_STATE_STRIDE);
    if (m_device == nullptr)
    {
        return false;
    }
    if (m_bootstrapState && m_stateBuffers[0] && m_stateBuffers[1] &&
        m_histogramBuffer)
    {
        return true;
    }

    ResetRuntimeResources();
    RHIBufferDesc bootstrapDesc;
    bootstrapDesc.size = sizeof(GPUState);
    bootstrapDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource;
    bootstrapDesc.memoryType = RHIMemoryType::Upload;
    bootstrapDesc.stride = sizeof(GPUState);
    bootstrapDesc.debugName = "ExposureBootstrapState";
    RHIBufferRef bootstrap = m_device->CreateBuffer(bootstrapDesc);
    if (!bootstrap)
    {
        return false;
    }
    GPUState bootstrapState{};
    // The bootstrap is finite and valid so HDR producers can use it before
    // the first GPU candidate is presented. Its zero hash intentionally makes
    // the first candidate reset temporal adaptation.
    void* mapped = bootstrap->Map();
    if (!mapped)
    {
        return false;
    }
    std::memcpy(mapped, &bootstrapState, sizeof(bootstrapState));
    bootstrap->Unmap();

    RHIBufferDesc stateDesc;
    stateDesc.size = sizeof(GPUState);
    stateDesc.usage = RHIBufferUsage::Structured |
        RHIBufferUsage::ShaderResource | RHIBufferUsage::UnorderedAccess;
    stateDesc.memoryType = RHIMemoryType::Default;
    stateDesc.stride = sizeof(GPUState);
    stateDesc.debugName = "ExposurePersistentState";
    RHIBufferRef state0 = m_device->CreateBuffer(stateDesc);
    RHIBufferRef state1 = m_device->CreateBuffer(stateDesc);

    RHIBufferDesc histogramDesc;
    histogramDesc.size = static_cast<uint64>(RVX_EXPOSURE_HISTOGRAM_BIN_COUNT) *
        sizeof(uint32);
    histogramDesc.usage = RHIBufferUsage::Structured |
        RHIBufferUsage::ShaderResource | RHIBufferUsage::UnorderedAccess;
    histogramDesc.memoryType = RHIMemoryType::Default;
    histogramDesc.stride = sizeof(uint32);
    histogramDesc.debugName = "ExposureHistogram256";
    RHIBufferRef histogram = m_device->CreateBuffer(histogramDesc);
    if (!state0 || !state1 || !histogram)
    {
        return false;
    }

    m_bootstrapState = std::move(bootstrap);
    m_stateBuffers[0] = std::move(state0);
    m_stateBuffers[1] = std::move(state1);
    m_histogramBuffer = std::move(histogram);
    return true;
}

RHIBufferRef ExposureController::CreateImmutableConstants(
    const GPUConstants& constants,
    const char* debugName) const
{
    static_assert(sizeof(GPUConstants) == 80u);
    if (m_device == nullptr)
    {
        return {};
    }
    RHIBufferDesc desc;
    desc.size = AlignConstantBufferSize(sizeof(GPUConstants));
    desc.usage = RHIBufferUsage::Constant;
    desc.memoryType = RHIMemoryType::Upload;
    desc.debugName = debugName;
    RHIBufferRef buffer = m_device->CreateBuffer(desc);
    if (!buffer)
    {
        return {};
    }
    void* mapped = buffer->Map();
    if (!mapped)
    {
        return {};
    }
    std::memcpy(mapped, &constants, sizeof(constants));
    buffer->Unmap();
    return buffer;
}

bool ExposureController::EnsureQualificationReadback()
{
    if (m_probeReadback)
    {
        return true;
    }
    if (m_device == nullptr)
    {
        return false;
    }
    RHIBufferDesc desc;
    desc.size = sizeof(GPUState);
    desc.usage = RHIBufferUsage::CopyDst;
    desc.memoryType = RHIMemoryType::Readback;
    desc.stride = sizeof(GPUState);
    desc.debugName = "ExposureQualificationReadback";
    m_probeReadback = m_device->CreateBuffer(desc);
    return m_probeReadback != nullptr;
}

ExposureGraphOutputs ExposureController::AddToGraph(
    RenderGraph& graph,
    RGTextureHandle sceneColor,
    RGBufferHandle committedState,
    const RenderExposureSettings& settings,
    float32 fixedDeltaSeconds,
    uint64 frameIndex,
    const RenderViewHistoryIdentity& historyIdentity,
    bool historyAvailable,
    RenderHistoryResetReason historyResetReasons)
{
    m_lastStats = {};
    m_lastStats.requested = true;
    m_lastStats.frameIndex = frameIndex;
    m_lastStats.historyIdentity = historyIdentity;
    m_lastStats.historyAvailable = historyAvailable;
    m_lastStats.historyResetReasons = historyResetReasons;

    if (!sceneColor.IsValid() || !committedState.IsValid() ||
        !historyIdentity.IsValid() || !IsValidRenderExposureSettings(settings) ||
        !IsValidRenderExposureDeltaSeconds(fixedDeltaSeconds))
    {
        m_lastStats.fallbackReason = "Exposure graph received invalid frozen frame inputs";
        return {};
    }
    if (m_hasPendingCandidate)
    {
        m_lastStats.fallbackReason =
            "Exposure candidate was not committed or abandoned before the next frame";
        return {};
    }
    if (!EnsureRuntimeResources())
    {
        m_lastStats.fallbackReason = "Exposure persistent GPU resources are unavailable";
        return {};
    }

    // Trigger lazy cooked-program creation before claiming readiness. There is
    // no shader-compiler route here: a missing cooked program is an explicit
    // capability failure.
    if (m_pipelineCache == nullptr ||
        m_pipelineCache->GetExposureAdaptationPipeline() == nullptr ||
        !HasPipelineResources())
    {
        m_lastStats.fallbackReason = "Exposure cooked compute pipelines are unavailable";
        return {};
    }

    const uint64 configurationHash = CalculateRenderExposureSettingsHash(settings);
    if (!m_hasPreparedFrameInput ||
        m_preparedHistoryIdentity != historyIdentity ||
        m_preparedFrameInput.configurationHash != configurationHash)
    {
        m_lastStats.fallbackReason =
            "Exposure frame input was not prepared before HDR graph construction";
        return {};
    }
    const bool forceHistoryReset = m_preparedFrameInput.historyReset;
    m_lastStats.historyResetReasons = m_preparedFrameInput.resetReasons;
    // Exposure's usable history additionally depends on its immutable
    // configuration. The coordinator's resource availability alone is not a
    // valid receipt fact after configuration/reset invalidation.
    m_lastStats.historyAvailable = !forceHistoryReset;
    const uint32 candidateIndex = m_committedStateIndex == 0u ? 1u : 0u;
    RHIBufferRef expectedCommitted = m_preparedFrameInput.stateBuffer;
    if (!expectedCommitted)
    {
        m_lastStats.fallbackReason = "Exposure committed state is unavailable";
        return {};
    }

    const bool useHistogram = settings.mode == RenderExposureMode::HistogramAuto;
    const bool requestProbe = m_probeRequested && !m_probeScheduled &&
        !m_probeRecorded &&
        EnsureQualificationReadback();
    if (m_probeRequested && !m_probeScheduled && !m_probeRecorded &&
        !requestProbe)
    {
        m_lastStats.fallbackReason = "Exposure qualification readback allocation failed";
        return {};
    }

    const uint64 candidateStateContentHash = CalculateExposureSemanticHash(
        configurationHash,
        historyIdentity,
        frameIndex,
        candidateIndex,
        m_preparedFrameInput.contentHash);
    GPUConstants constants{};
    const RHITextureDesc* const sceneDesc = graph.GetTextureDesc(sceneColor);
    if (sceneDesc == nullptr || sceneDesc->width == 0 || sceneDesc->height == 0)
    {
        m_lastStats.fallbackReason = "Exposure scene color has no realizable extent";
        return {};
    }
    constants.renderExtent[0] = sceneDesc->width;
    constants.renderExtent[1] = sceneDesc->height;
    constants.fixedDeltaSeconds = fixedDeltaSeconds;
    constants.exposureMode = static_cast<uint32>(settings.mode);
    constants.manualEV100 = settings.manualEV100;
    constants.exposureCompensationEV = settings.exposureCompensationEV;
    constants.histogramLowPercentile = settings.histogramLowPercentile;
    constants.histogramHighPercentile = settings.histogramHighPercentile;
    constants.histogramKeyValue = settings.histogramKeyValue;
    constants.minEV100 = settings.minEV100;
    constants.maxEV100 = settings.maxEV100;
    constants.brighteningAdaptationSpeed = settings.brighteningAdaptationSpeed;
    constants.darkeningAdaptationSpeed = settings.darkeningAdaptationSpeed;
    constants.configurationHashLo = static_cast<uint32>(configurationHash);
    constants.configurationHashHi = static_cast<uint32>(configurationHash >> 32u);
    constants.forceHistoryReset = forceHistoryReset ? 1u : 0u;
    constants.candidateStateContentHashLo =
        static_cast<uint32>(candidateStateContentHash);
    constants.candidateStateContentHashHi =
        static_cast<uint32>(candidateStateContentHash >> 32u);

    const RGBufferHandle histogram = graph.ImportBuffer(
        RHIBufferRef(m_histogramBuffer), RHIResourceState::UnorderedAccess);
    const RGBufferHandle candidateState = graph.ImportBuffer(
        RHIBufferRef(m_stateBuffers[candidateIndex]), RHIResourceState::ShaderResource);
    auto record = std::make_shared<FrameRecord>();
    record->expectedCommittedState = expectedCommitted;
    record->candidateState = m_stateBuffers[candidateIndex];
    record->histogram = m_histogramBuffer;
    record->candidateStateIndex = candidateIndex;
    record->forceHistoryReset = forceHistoryReset;
    record->qualificationProbeRequested = requestProbe;
    record->stats = m_lastStats;
    record->stats.pipelineReady = true;
    record->stats.inputsReady = true;
    record->stats.configurationHash = configurationHash;
    record->stats.usedCommittedHistory = !forceHistoryReset;
    record->stats.qualificationProbeRequested = requestProbe;
    record->stats.committedStateContentHash =
        m_preparedFrameInput.contentHash;
    record->stats.candidateStateContentHash = candidateStateContentHash;

    struct ComputePassData
    {
        RGTextureHandle sceneColor{};
        RGBufferHandle committedState{};
        RGBufferHandle histogram{};
        RGBufferHandle candidateState{};
        RGTextureViewHandle sceneColorView{};
        RHIBufferRef constants;
        std::shared_ptr<FrameRecord> record;
    };

    const auto addComputePass = [this, &graph, sceneColor, committedState,
                                 histogram, candidateState, constants, record](
                                    const char* passName,
                                    RHIPipeline* pipeline,
                                    bool histogramWrite,
                                    bool histogramReadWrite,
                                    uint32 dispatchX,
                                    uint32 dispatchY,
                                    uint32 dispatchZ,
                                    const std::function<void(FrameRecord&)>& onExecuted)
    {
        graph.AddPass<ComputePassData>(
            passName,
            RenderGraphPassType::Compute,
            RenderGraphQueueAffinity::Graphics,
            [this, sceneColor, committedState, histogram, candidateState, constants,
             record, histogramWrite, histogramReadWrite](
                RenderGraphBuilder& builder, ComputePassData& data)
            {
                data.sceneColor = builder.Read(
                    sceneColor, MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                                  RHIShaderStage::Compute));
                data.committedState = builder.Read(
                    committedState, MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                                      RHIShaderStage::Compute));
                const RGAccessDesc histogramAccess = MakeRGAccessDesc(
                    RHIResourceState::UnorderedAccess, RHIShaderStage::Compute);
                data.histogram = histogramReadWrite
                    ? builder.ReadWrite(histogram, histogramAccess)
                    : (histogramWrite ? builder.Write(histogram, histogramAccess)
                                      : builder.Read(histogram, MakeRGAccessDesc(
                                            RHIResourceState::UnorderedAccess,
                                            RHIShaderStage::Compute)));
                data.candidateState = builder.Write(
                    candidateState, MakeRGAccessDesc(RHIResourceState::UnorderedAccess,
                                                      RHIShaderStage::Compute));
                data.sceneColorView = AddComputeTextureSRV(
                    builder, data.sceneColor, "ExposureSceneColorSRV");
                data.constants = CreateImmutableConstants(
                    constants, "ExposurePassConstants");
                data.record = record;
            },
            [this, pipeline, dispatchX, dispatchY, dispatchZ, onExecuted](
                const ComputePassData& data, RenderGraphPassContext& context)
            {
                if (pipeline == nullptr || data.record == nullptr || !data.constants ||
                    m_pipelineCache == nullptr)
                {
                    context.MarkExecutionFailed("Exposure compute pipeline or constants are unavailable");
                    return;
                }
                RHIDescriptorSetLayout* const setLayout =
                    m_pipelineCache->GetExposureComputeSetLayout();
                IRHIDevice* const device = m_pipelineCache->GetDevice();
                RHIBuffer* const committed = context.GetBuffer(data.committedState);
                RHIBuffer* const histogramBuffer = context.GetBuffer(data.histogram);
                RHIBuffer* const candidate = context.GetBuffer(data.candidateState);
                RHITextureView* const sceneView = context.GetTextureView(data.sceneColorView);
                if (setLayout == nullptr || device == nullptr || committed == nullptr ||
                    histogramBuffer == nullptr || candidate == nullptr || sceneView == nullptr ||
                    committed != data.record->expectedCommittedState.Get())
                {
                    context.MarkExecutionFailed("Exposure graph resources do not match the committed state contract");
                    return;
                }

                RHIDescriptorSetDesc descriptorDesc;
                descriptorDesc.layout = setLayout;
                descriptorDesc.debugName = "ExposureComputeDescriptorSet";
                descriptorDesc.BindBuffer(0, data.constants.Get(), 0,
                                          AlignConstantBufferSize(sizeof(GPUConstants)));
                descriptorDesc.BindTexture(1, sceneView);
                descriptorDesc.BindBuffer(2, committed);
                descriptorDesc.BindBuffer(3, histogramBuffer);
                descriptorDesc.BindBuffer(4, candidate);
                RHIDescriptorSetRef descriptorSet = device->CreateDescriptorSet(descriptorDesc);
                if (!descriptorSet ||
                    !context.RetainSubmissionResource(Ref<RefCounted>(data.constants)) ||
                    !context.RetainSubmissionResource(Ref<RefCounted>(descriptorSet)) ||
                    !context.RetainSubmissionResource(Ref<RefCounted>(data.record->expectedCommittedState)) ||
                    !context.RetainSubmissionResource(Ref<RefCounted>(data.record->candidateState)) ||
                    !context.RetainSubmissionResource(Ref<RefCounted>(data.record->histogram)))
                {
                    context.MarkExecutionFailed("Exposure submission ownership could not be retained");
                    return;
                }

                RHICommandContext& commands = context.Commands();
                commands.SetPipeline(pipeline);
                commands.SetDescriptorSet(0, descriptorSet.Get());
                commands.Dispatch(dispatchX, dispatchY, dispatchZ);
                onExecuted(*data.record);
            });
    };

    if (useHistogram)
    {
        addComputePass(
            "ExposureHistogramClear",
            m_pipelineCache->GetExposureHistogramClearPipeline(),
            true, false,
            (RVX_EXPOSURE_HISTOGRAM_BIN_COUNT + 63u) / 64u, 1u, 1u,
            [](FrameRecord& state)
            {
                state.stats.histogramClearExecuted = true;
                ++state.stats.histogramClearDispatchCount;
            });
        addComputePass(
            "ExposureHistogramBuild",
            m_pipelineCache->GetExposureHistogramBuildPipeline(),
            false, true,
            (constants.renderExtent[0] + 15u) / 16u,
            (constants.renderExtent[1] + 15u) / 16u, 1u,
            [](FrameRecord& state)
            {
                state.stats.histogramBuildExecuted = true;
                ++state.stats.histogramBuildDispatchCount;
            });
    }

    addComputePass(
        "ExposureAdaptation",
        m_pipelineCache->GetExposureAdaptationPipeline(),
        false, false, 1u, 1u, 1u,
        [this, configurationHash, historyIdentity, candidateIndex](
            FrameRecord& state)
        {
            state.stats.adaptationExecuted = true;
            ++state.stats.adaptationDispatchCount;
            state.stats.candidatePrepared = true;
            m_pendingStateIndex = candidateIndex;
            m_pendingConfigurationHash = configurationHash;
            m_pendingStateContentHash = state.stats.candidateStateContentHash;
            m_pendingIdentity = historyIdentity;
            m_hasPendingCandidate = true;
            m_lastStats = state.stats;
        });

    // The qualification copy must be a separate graph pass so its manual
    // CopySrc transition cannot invalidate the graph's state model for the
    // candidate consumed by ToneMapping/Bloom/TAA.
    if (requestProbe)
    {
        const RGBufferHandle probeReadback = graph.ImportBuffer(
            RHIBufferRef(m_probeReadback), RHIResourceState::CopyDest);
        struct ProbeCopyData
        {
            RGBufferHandle candidateState{};
            RGBufferHandle readback{};
            RHIBufferRef readbackOwner;
            std::shared_ptr<FrameRecord> record;
        };
        graph.AddPass<ProbeCopyData>(
            "ExposureQualificationProbeCopy",
            RenderGraphPassType::Copy,
            RenderGraphQueueAffinity::Graphics,
            [candidateState, probeReadback, readbackOwner = RHIBufferRef(m_probeReadback),
             record](RenderGraphBuilder& builder, ProbeCopyData& data)
            {
                data.candidateState = builder.Read(
                    candidateState, MakeRGAccessDesc(RHIResourceState::CopySource,
                                                      RHIShaderStage::All));
                data.readback = builder.Write(
                    probeReadback,
                    MakeRGAccessDesc(RHIResourceState::CopyDest,
                                     RHIShaderStage::All));
                data.readbackOwner = readbackOwner;
                data.record = record;
            },
            [this](const ProbeCopyData& data, RenderGraphPassContext& context)
            {
                RHIBuffer* const candidate = context.GetBuffer(data.candidateState);
                RHIBuffer* const readback = context.GetBuffer(data.readback);
                if (candidate == nullptr || readback == nullptr ||
                    data.record == nullptr || !data.readbackOwner ||
                    readback != data.readbackOwner.Get() ||
                    !context.RetainSubmissionResource(
                        Ref<RefCounted>(data.readbackOwner)))
                {
                    context.MarkExecutionFailed("Exposure qualification readback resources are unavailable");
                    return;
                }
                context.Commands().CopyBuffer(candidate, readback, 0, 0,
                                              sizeof(GPUState));
                m_probeScheduled = false;
                m_probeRecorded = true;
                m_probeIdentity = data.record->stats.historyIdentity;
                m_probeConfigurationHash = data.record->stats.configurationHash;
                data.record->stats.qualificationProbeRecorded = true;
                m_lastStats = data.record->stats;
            });
        m_probeScheduled = true;
        m_probeCandidateStateContentHash = candidateStateContentHash;
        m_probeMode = settings.mode;
        m_probeExposureCompensationEV = settings.exposureCompensationEV;
    }

    m_lastFrameRecord = record;
    m_lastStats = record->stats;
    m_hasPreparedFrameInput = false;
    return ExposureGraphOutputs{
        committedState,
        candidateState,
        histogram,
        configurationHash,
        record->stats.committedStateContentHash,
        record->stats.candidateStateContentHash,
        useHistogram};
}

bool ExposureController::CanCommitAfterPresent(
    const RenderViewHistoryIdentity& identity) const noexcept
{
    return m_hasPendingCandidate && m_pendingStateIndex < 2u &&
        identity.IsValid() && identity == m_pendingIdentity;
}

bool ExposureController::CommitAfterPresent(
    const RenderViewHistoryIdentity& identity) noexcept
{
    if (!CanCommitAfterPresent(identity))
    {
        return false;
    }
    m_committedStateIndex = m_pendingStateIndex;
    m_committedConfigurationHash = m_pendingConfigurationHash;
    m_committedStateContentHash = m_pendingStateContentHash;
    m_pendingStateIndex = RVX_INVALID_INDEX;
    m_pendingConfigurationHash = 0;
    m_pendingStateContentHash = 0;
    m_pendingIdentity = {};
    m_hasPendingCandidate = false;
    m_lastStats.candidateCommitted = true;
    return true;
}

void ExposureController::AbandonPendingCandidate() noexcept
{
    if (!m_hasPendingCandidate)
    {
        return;
    }
    const RenderViewHistoryIdentity abandonedIdentity = m_pendingIdentity;
    const bool abandonedProbeSource =
        m_probeScheduled ||
        (m_probeRecorded && m_probeIdentity == abandonedIdentity);
    m_pendingStateIndex = RVX_INVALID_INDEX;
    m_pendingConfigurationHash = 0;
    m_pendingStateContentHash = 0;
    m_pendingIdentity = {};
    m_hasPendingCandidate = false;
    if (abandonedProbeSource)
    {
        CancelQualificationProbe();
    }
    m_lastStats.candidateAbandoned = true;
}

void ExposureController::ResetHistory() noexcept
{
    m_committedStateIndex = RVX_INVALID_INDEX;
    m_committedConfigurationHash = 0;
    m_committedStateContentHash = 0;
    AbandonPendingCandidate();
    CancelQualificationProbe();
}

bool ExposureController::RequestQualificationProbe() noexcept
{
    if (m_probeRequested || m_probeScheduled || m_probeRecorded)
    {
        return false;
    }
    m_probeRequested = true;
    return true;
}

void ExposureController::CancelQualificationProbe() noexcept
{
    m_probeRequested = false;
    m_probeScheduled = false;
    m_probeRecorded = false;
    m_probeIdentity = {};
    m_probeConfigurationHash = 0;
    m_probeCandidateStateContentHash = 0;
    m_probeMode = static_cast<RenderExposureMode>(255);
    m_probeExposureCompensationEV = 0.0f;
    m_lastStats.qualificationProbeRequested = false;
    m_lastStats.qualificationProbeRecorded = false;
    m_lastStats.qualificationProbeReady = false;
}

bool ExposureController::CompleteQualificationProbe(
    const RenderViewHistoryIdentity& identity,
    ExposureQualificationProbe& outProbe) noexcept
{
    outProbe = {};
    if (!m_probeRecorded || !m_probeReadback || !identity.IsValid() ||
        identity != m_probeIdentity)
    {
        return false;
    }

    void* mapped = m_probeReadback->Map();
    if (mapped == nullptr)
    {
        CancelQualificationProbe();
        return false;
    }
    GPUState state{};
    std::memcpy(&state, mapped, sizeof(state));
    m_probeReadback->Unmap();
    float32 expectedExposureScale = 0.0f;
    const bool physicalScaleValid = TryCalculateRenderExposureScale(
        state.exposureValues[3], m_probeExposureCompensationEV,
        expectedExposureScale);
    const float32 targetEV100 =
        std::bit_cast<float32>(state.targetAndNonce[0]);
    const uint64 observedCandidateStateContentHash =
        static_cast<uint64>(state.targetAndNonce[1]) |
        (static_cast<uint64>(state.targetAndNonce[2]) << 32u);
    const bool valid = state.stateMeta[3] == RVX_EXPOSURE_STATE_VALID &&
        state.stateMeta[2] == static_cast<uint32>(m_probeMode) &&
        IsValidRenderExposureScale(state.exposureValues[0]) &&
        IsValidRenderExposureScale(state.exposureValues[1]) &&
        IsValidRenderExposureScale(state.exposureValues[2]) &&
        std::isfinite(state.exposureValues[3]) &&
        state.exposureValues[3] >= RVX_RENDER_EXPOSURE_MIN_EV100 &&
        state.exposureValues[3] <= RVX_RENDER_EXPOSURE_MAX_EV100 &&
        std::isfinite(targetEV100) &&
        targetEV100 >= RVX_RENDER_EXPOSURE_MIN_EV100 &&
        targetEV100 <= RVX_RENDER_EXPOSURE_MAX_EV100 &&
        observedCandidateStateContentHash != 0 &&
        observedCandidateStateContentHash ==
            m_probeCandidateStateContentHash &&
        physicalScaleValid &&
        AreRenderExposureScalesEquivalent(
            state.exposureValues[0], expectedExposureScale);
    if (!valid)
    {
        CancelQualificationProbe();
        return false;
    }

    outProbe.requested = true;
    outProbe.recorded = true;
    outProbe.ready = true;
    outProbe.frameIndex = identity.previousFrame.frameSequence;
    outProbe.historyIdentity = identity;
    outProbe.configurationHash =
        static_cast<uint64>(state.stateMeta[0]) |
        (static_cast<uint64>(state.stateMeta[1]) << 32u);
    outProbe.candidateStateContentHash =
        observedCandidateStateContentHash;
    outProbe.targetEV100 = targetEV100;
    outProbe.adaptedEV100 = state.exposureValues[3];
    outProbe.exposureScale = state.exposureValues[0];
    outProbe.preExposure = state.exposureValues[1];
    outProbe.previousPreExposure = state.exposureValues[2];
    if (outProbe.configurationHash != m_probeConfigurationHash)
    {
        CancelQualificationProbe();
        return false;
    }

    m_probeRequested = false;
    m_probeScheduled = false;
    m_probeRecorded = false;
    m_probeIdentity = {};
    m_probeConfigurationHash = 0;
    m_probeCandidateStateContentHash = 0;
    m_probeMode = static_cast<RenderExposureMode>(255);
    m_probeExposureCompensationEV = 0.0f;
    m_lastStats.qualificationProbeReady = true;
    return true;
}

void ExposureController::ResetRuntimeResources() noexcept
{
    m_lastFrameRecord.reset();
    m_probeReadback.Reset();
    m_histogramBuffer.Reset();
    m_stateBuffers[0].Reset();
    m_stateBuffers[1].Reset();
    m_bootstrapState.Reset();
    m_committedStateIndex = RVX_INVALID_INDEX;
    m_pendingStateIndex = RVX_INVALID_INDEX;
    m_committedConfigurationHash = 0;
    m_committedStateContentHash = 0;
    m_pendingConfigurationHash = 0;
    m_pendingStateContentHash = 0;
    m_preparedFrameInput = {};
    m_preparedHistoryIdentity = {};
    m_hasPreparedFrameInput = false;
    m_hasPendingCandidate = false;
    m_pendingIdentity = {};
    m_probeRequested = false;
    m_probeScheduled = false;
    m_probeRecorded = false;
    m_probeIdentity = {};
    m_probeConfigurationHash = 0;
    m_probeCandidateStateContentHash = 0;
    m_probeMode = static_cast<RenderExposureMode>(255);
    m_probeExposureCompensationEV = 0.0f;
}
} // namespace RVX
