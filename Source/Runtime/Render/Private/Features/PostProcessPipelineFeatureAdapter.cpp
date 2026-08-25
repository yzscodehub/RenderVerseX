/**
 * @file PostProcessPipelineFeatureAdapter.cpp
 * @brief Typed feature composition for the post-process pipeline.
 */

#include "Features/PostProcessPipelineFeatureAdapter.h"

#include "Core/Diagnostics/ContentHash.h"
#include "Render/PostProcess/Bloom.h"
#include "Render/PostProcess/TAA.h"
#include "Render/PostProcess/ToneMapping.h"

#include <algorithm>
#include <bit>
#include <utility>

namespace RVX
{
namespace
{
    [[nodiscard]] bool HasGraphicsCapability(
        RenderFeatureCapabilityMask capabilities) noexcept
    {
        return (capabilities & ToMask(RenderFeatureCapability::Graphics)) != 0;
    }

    [[nodiscard]] bool IsPresentableRecipe(
        RenderPipelineRecipe recipe) noexcept
    {
        return recipe == RenderPipelineRecipe::ForwardStandard ||
               recipe == RenderPipelineRecipe::DeferredQuality;
    }

    [[nodiscard]] bool HandleMatchesBuilder(
        RGTextureHandle handle,
        const RenderFeatureGraphBuilder& builder) noexcept
    {
        return handle.IsValid() &&
               handle.graphIdentity == builder.GetGraphIdentity() &&
               handle.recordingGeneration == builder.GetRecordingGeneration();
    }

    [[nodiscard]] bool HandleMatchesBuilder(
        RGBufferHandle handle,
        const RenderFeatureGraphBuilder& builder) noexcept
    {
        return handle.IsValid() &&
               handle.graphIdentity == builder.GetGraphIdentity() &&
               handle.recordingGeneration == builder.GetRecordingGeneration();
    }

    [[nodiscard]] bool IsSameBufferHandle(
        const RGBufferHandle& lhs,
        const RGBufferHandle& rhs) noexcept
    {
        return lhs.index == rhs.index && lhs.hasRange == rhs.hasRange &&
               lhs.rangeOffset == rhs.rangeOffset &&
               lhs.rangeSize == rhs.rangeSize &&
               lhs.graphIdentity == rhs.graphIdentity &&
               lhs.recordingGeneration == rhs.recordingGeneration;
    }

    [[nodiscard]] bool UsesHardwareSRGBEncode(RHIFormat format) noexcept
    {
        return IsSRGBFormat(format);
    }

    [[nodiscard]] PostProcessTransfer FindScheduledToneMappingTransfer(
        const PostProcessStackExecuteStats& stats) noexcept
    {
        for (const PostProcessEffectExecutionPlan& effect : stats.effectPlans)
        {
            if (effect.scheduled &&
                effect.passId == PostProcessPassId::ToneMapping)
            {
                return effect.transfer;
            }
        }

        return PostProcessTransfer::None;
    }

    void MixFloat(uint64& hash, float32 value) noexcept
    {
        // Diagnostics must distinguish all temporal jitter samples while
        // treating the two IEEE zero encodings as the same camera state.
        const float32 canonicalValue = value == 0.0f ? 0.0f : value;
        Diagnostics::MixContentHashValue(
            hash,
            static_cast<uint64>(std::bit_cast<uint32>(canonicalValue)));
    }

    [[nodiscard]] uint64 ExtendExposureProbeContentHash(
        uint64 sourceHash,
        const ExposureQualificationProbe& probe) noexcept
    {
        uint64 hash = Diagnostics::RVX_DIAGNOSTICS_FNV1A64_OFFSET_BASIS;
        Diagnostics::MixContentHashValue(hash, sourceHash);
        // Domain-separate a post-fence qualification augmentation from the
        // graph-finalized source receipt. The source hash already binds the
        // exact graph, frame, view, plan, and pre-Present temporal state.
        Diagnostics::MixContentHashValue(hash, 0x4558504F53555245ull);
        Diagnostics::MixContentHashValue(hash, probe.frameIndex);
        Diagnostics::MixContentHashValue(hash, probe.configurationHash);
        Diagnostics::MixContentHashValue(
            hash, probe.candidateStateContentHash);
        MixFloat(hash, probe.targetEV100);
        MixFloat(hash, probe.adaptedEV100);
        MixFloat(hash, probe.exposureScale);
        MixFloat(hash, probe.preExposure);
        MixFloat(hash, probe.previousPreExposure);
        return hash != 0 ? hash : 1u;
    }
} // namespace

PostProcessPipelineFeatureAdapter::PostProcessPipelineFeatureAdapter(
    PostProcessStack* stack) noexcept
    : m_stack(stack)
{
}

void PostProcessPipelineFeatureAdapter::SetRecordContext(
    PostProcessPipelineRecordContext context)
{
    m_recordContext = std::move(context);
    m_hasRecordContext = true;
    m_preparedContext = {};
    m_lastPlanStats = {};
    m_composedProvenance = {};
    m_lastReceipt = {};
    m_lastExecutionReceipt = {};
    m_readiness = {};
    m_hasComposedPresentationPlan = false;
    m_hardwareSRGBEncodeBoundary = false;
}

void PostProcessPipelineFeatureAdapter::SetStack(PostProcessStack* stack) noexcept
{
    if (m_stack == stack)
    {
        return;
    }

    m_stack = stack;
    m_hasRecordContext = false;
    m_recordContext = {};
    m_preparedContext = {};
    m_lastPlanStats = {};
    m_composedProvenance = {};
    m_lastReceipt = {};
    m_lastExecutionReceipt = {};
    m_readiness = {};
    m_hasComposedPresentationPlan = false;
    m_hardwareSRGBEncodeBoundary = false;
    m_lastExposureProbe = {};
    ClearExposureProbeSourceReceipt();
}

RenderFeatureDescriptor PostProcessPipelineFeatureAdapter::Describe() const
{
    RenderFeatureDescriptor descriptor;
    descriptor.featureId = BuiltinRenderFeatureIds::PostProcessPipeline;
    descriptor.schemaVersion = 1;
    descriptor.name = "PostProcessPipeline";
    descriptor.phase = RenderFeaturePhase::HDRPost;
    descriptor.consumedResources = {
        {RenderBlackboardResourceId::TranslucencyOutputs,
         TranslucencyOutputs::SchemaVersion}};
    descriptor.producedResources = {
        {RenderBlackboardResourceId::PostProcessOutputs,
         PostProcessOutputs::SchemaVersion}};
    descriptor.requiredCapabilities = ToMask(RenderFeatureCapability::Graphics);
    descriptor.requiredRecipes =
        ToMask(RenderPipelineRecipe::ForwardStandard) |
        ToMask(RenderPipelineRecipe::DeferredQuality);
    descriptor.qualityTiers = RVX_ALL_RENDER_QUALITY_TIERS;
    descriptor.fallbackPolicy = RenderFeatureFallbackPolicy::None;
    descriptor.shaderPipelineFamily = "PostProcessPipeline";
    return descriptor;
}

void PostProcessPipelineFeatureAdapter::Prepare(
    const RenderFeatureContext& context)
{
    m_preparedContext = context;
    m_readiness = {};
    m_readiness.compiled = true;
    m_readiness.hardwareSupported = HasGraphicsCapability(
        context.hardwareCapabilities);
    m_readiness.implementationSupported = IsPresentableRecipe(
        context.resolvedRecipe);
    m_readiness.pipelineReady = m_stack != nullptr && m_stack->IsInitialized();
    m_readiness.inputsReady = HasMatchingRecordContext(context);

    if (!m_readiness.hardwareSupported)
    {
        m_readiness.diagnostic = "Graphics capability is unavailable";
    }
    else if (!m_readiness.implementationSupported)
    {
        m_readiness.diagnostic =
            "Post-process pipeline is unavailable for the selected recipe";
    }
    else if (m_stack == nullptr || !m_stack->IsInitialized())
    {
        m_readiness.pipelineReady = false;
        m_readiness.diagnostic = "Post-process stack is unavailable";
    }
    else if (!m_readiness.inputsReady)
    {
        m_readiness.diagnostic =
            "Post-process record context is absent, stale, or mismatched";
    }
    m_readiness.executed = false;
}

RenderFeatureReadiness
PostProcessPipelineFeatureAdapter::ResolveReadiness() const
{
    return m_readiness;
}

void PostProcessPipelineFeatureAdapter::AddPasses(
    RenderFeatureGraphBuilder& builder,
    RenderGraphBlackboard& blackboard)
{
    m_lastPlanStats = {};
    m_composedProvenance = {};
    m_lastReceipt = {};
    m_lastExecutionReceipt = {};
    m_readiness.executed = false;
    m_hasComposedPresentationPlan = false;
    m_hardwareSRGBEncodeBoundary = false;
    if (!m_readiness.IsReadyToExecute())
    {
        return;
    }
    if (!HasCurrentGraphProvenance(builder))
    {
        SetCompositionFailure(
            "Post-process presentation surface is stale or belongs to another graph");
        return;
    }

    RenderGraphBlackboardCode code = RenderGraphBlackboardCode::Success;
    const TranslucencyOutputs* const translucency =
        blackboard.Require<TranslucencyOutputs>(
            BuiltinRenderFeatureIds::PostProcessPipeline, Describe(), &code);
    if (translucency == nullptr || code != RenderGraphBlackboardCode::Success)
    {
        SetCompositionFailure("Post-process requires TranslucencyOutputs v3");
        return;
    }
    if (translucency->frameIdentity != m_preparedContext.frameIdentity ||
        translucency->viewIdentity != m_preparedContext.viewIdentity ||
        translucency->colorDomain != PostProcessColorDomain::SceneLinearHDR ||
        translucency->transfer != PostProcessTransfer::None ||
        !HandleMatchesBuilder(translucency->hdrSceneColor, builder) ||
        !HandleMatchesBuilder(translucency->exposureState, builder) ||
        !translucency->exposureStateMetadata.IsValid() ||
        !IsSameBufferHandle(
            translucency->exposureState,
            m_recordContext.frameInputs.exposureState) ||
        m_recordContext.frameInputs.exposureStateContentHash == 0 ||
        m_stack == nullptr ||
        translucency->exposureStateMetadata.mode !=
            m_stack->GetSettings().exposureSettings.mode ||
        translucency->exposureStateMetadata.configurationHash !=
            CalculateRenderExposureSettingsHash(
                m_stack->GetSettings().exposureSettings))
    {
        SetCompositionFailure(
            "Post-process received mismatched HDR scene-color provenance or domain");
        return;
    }

    PostProcessFrameInputs inputs = m_recordContext.frameInputs;
    inputs.sceneColor = translucency->hdrSceneColor;
    inputs.depth = translucency->deviceDepth;
    inputs.normal = translucency->worldNormal;
    inputs.velocity = translucency->velocity;
    inputs.reactiveMask = translucency->reactiveMask;
    inputs.exposureState = translucency->exposureState;
    inputs.sceneColorDomain = translucency->colorDomain;
    const bool hardwareSRGBEncode = UsesHardwareSRGBEncode(
        m_recordContext.presentableColorMetadata.format);
    inputs.outputColorDomain = hardwareSRGBEncode
        ? PostProcessColorDomain::DisplayLinearLDR
        : PostProcessColorDomain::DisplayEncodedLDR;
    inputs.outputFormat = m_recordContext.presentableColorMetadata.format;

    if (!m_stack->BuildPlan(
            inputs,
            m_recordContext.presentableColorMetadata.format,
            PostProcessPlanContract::Presentation,
            m_lastPlanStats) ||
        !m_lastPlanStats.planValid)
    {
        SetCompositionFailure(
            m_lastPlanStats.planFailureReason.empty()
                ? "Post-process typed plan is invalid"
                : m_lastPlanStats.planFailureReason);
        return;
    }
    if (m_lastPlanStats.enabledToneMappingEffectCount != 1 ||
        m_lastPlanStats.fallbackCopyApplied ||
        m_lastPlanStats.noEffectNoWork)
    {
        SetCompositionFailure(
            "Post-process requires exactly one real ToneMapping plan without fallback copy");
        return;
    }
    bool planMaterialized = false;
    if (!builder.AddGraphCallback(
            "PostProcessPipeline",
            m_recordContext.passRecordContext,
            [this, &planMaterialized, inputs, plan = m_lastPlanStats](RenderGraph& graph)
            {
                const bool executed = m_stack != nullptr &&
                    m_stack->ExecutePlan(
                        graph,
                        inputs,
                        m_recordContext.presentableColor,
                        plan);
                planMaterialized = executed;
            }) ||
        !planMaterialized)
    {
        SetCompositionFailure("Post-process typed plan could not be materialized");
        return;
    }

    m_lastPlanStats = m_stack->GetLastExecuteStats();
    if (!m_lastPlanStats.planValid ||
        m_lastPlanStats.planContract !=
            PostProcessPlanContract::Presentation ||
        m_lastPlanStats.planFingerprint == 0 ||
        m_lastPlanStats.scheduledToneMappingEffectCount != 1 ||
        m_lastPlanStats.fallbackCopyApplied ||
        m_lastPlanStats.outputColorDomain != inputs.outputColorDomain)
    {
        SetCompositionFailure(
            "Post-process materialization did not preserve the required presentation boundary");
        return;
    }

    const PostProcessTransfer toneMappingTransfer =
        FindScheduledToneMappingTransfer(m_lastPlanStats);
    const PostProcessTransfer expectedToneMappingTransfer = hardwareSRGBEncode
        ? PostProcessTransfer::SceneLinearToDisplayLinear
        : PostProcessTransfer::SceneLinearToDisplayEncoded;
    if (toneMappingTransfer != expectedToneMappingTransfer)
    {
        SetCompositionFailure(
            "Post-process ToneMapping transfer does not match the presentation surface");
        return;
    }

    PostProcessOutputs outputs;
    outputs.provenance = builder.GetProvenance();
    outputs.frameIdentity = m_preparedContext.frameIdentity;
    outputs.viewIdentity = m_preparedContext.viewIdentity;
    outputs.presentableColor = m_recordContext.presentableColor;
    outputs.presentableColorMetadata =
        m_recordContext.presentableColorMetadata;
    // An sRGB attachment receives display-linear shader output and performs
    // the final encode in hardware. The blackboard describes the stored
    // presentable resource, not the shader-visible target domain.
    outputs.colorDomain = PostProcessColorDomain::DisplayEncodedLDR;
    outputs.transfer = hardwareSRGBEncode
        ? PostProcessTransfer::DisplayLinearToEncoded
        : toneMappingTransfer;
    if (blackboard.Publish(BuiltinRenderFeatureIds::PostProcessPipeline,
                           Describe(),
                           std::move(outputs)) !=
        RenderGraphBlackboardCode::Success)
    {
        SetCompositionFailure("Post-process could not publish PostProcessOutputs v2");
        return;
    }

    m_composedProvenance = builder.GetProvenance();
    m_hasComposedPresentationPlan = true;
    m_hardwareSRGBEncodeBoundary = hardwareSRGBEncode;
}

void PostProcessPipelineFeatureAdapter::OnGraphExecutionFinalized(
    const RenderFeatureGraphExecutionEvidence& evidence)
{
    m_lastExecutionReceipt = {};
    if (!HasMatchingFinalizedEvidence(evidence) ||
        !evidence.HasExactExecution())
    {
        return;
    }

    RefreshExecutionReceipt();
    SnapshotExposureProbeSourceReceipt();
}

bool PostProcessPipelineFeatureAdapter::CommitTemporalHistoryAfterPresent(
    const RenderViewHistoryIdentity& identity) noexcept
{
    if (!CanCommitTemporalHistoryAfterPresent(identity))
    {
        return false;
    }

    const bool requiresTAA =
        m_lastExecutionReceipt.taaRequested &&
        m_lastExecutionReceipt.taaExecuted &&
        m_lastExecutionReceipt.historyCandidateProduced &&
        !m_lastExecutionReceipt.historyCommitted;
    const bool requiresExposure =
        m_lastExecutionReceipt.exposureRequested &&
        m_lastExecutionReceipt.exposureExecuted &&
        m_lastExecutionReceipt.exposureHistoryCandidateProduced &&
        !m_lastExecutionReceipt.exposureHistoryCommitted;

    if (requiresTAA)
    {
        TAA* const taa = m_stack->GetEffect<TAA>();
        if (taa == nullptr || !taa->CommitHistoryAfterPresent(identity))
            return false;
    }
    if (requiresExposure &&
        !m_stack->GetExposureController().CommitAfterPresent(identity))
    {
        return false;
    }

    RefreshExecutionReceipt();
    SnapshotExposureProbeSourceReceipt();
    return (!requiresTAA || m_lastExecutionReceipt.historyCommitted) &&
           (!requiresExposure ||
            m_lastExecutionReceipt.exposureHistoryCommitted);
}

bool PostProcessPipelineFeatureAdapter::CanCommitTemporalHistoryAfterPresent(
    const RenderViewHistoryIdentity& identity) noexcept
{
    if (!RequiresTemporalHistoryCommit())
        return true;

    if (m_stack == nullptr || !identity.IsValid())
        return false;

    const bool requiresTAA =
        m_lastExecutionReceipt.taaRequested &&
        m_lastExecutionReceipt.taaExecuted &&
        m_lastExecutionReceipt.historyCandidateProduced &&
        !m_lastExecutionReceipt.historyCommitted;
    const bool requiresExposure =
        m_lastExecutionReceipt.exposureRequested &&
        m_lastExecutionReceipt.exposureExecuted &&
        m_lastExecutionReceipt.exposureHistoryCandidateProduced &&
        !m_lastExecutionReceipt.exposureHistoryCommitted;
    const TAA* const taa = requiresTAA ? m_stack->GetEffect<TAA>() : nullptr;
    return (!requiresTAA ||
            (taa != nullptr &&
             taa->CanCommitHistoryAfterPresent(identity))) &&
           (!requiresExposure ||
            m_stack->GetExposureController().CanCommitAfterPresent(identity));
}

void PostProcessPipelineFeatureAdapter::AbandonTemporalHistoryCandidate() noexcept
{
    if (m_stack == nullptr)
    {
        return;
    }

    const RenderViewHistoryIdentity abandonedIdentity =
        m_lastPlanStats.frameInputs.historyIdentity;
    if (TAA* const taa = m_stack->GetEffect<TAA>(); taa != nullptr)
    {
        taa->AbandonPendingHistory();
        if (m_lastExecutionReceipt.taaRequested)
        {
            RefreshExecutionReceipt();
        }
    }
    m_stack->GetExposureController().AbandonPendingCandidate();
    if (m_lastExecutionReceipt.exposureRequested)
    {
        RefreshExecutionReceipt();
    }
    if (m_exposureProbeSourceIdentity.has_value() &&
        *m_exposureProbeSourceIdentity == abandonedIdentity &&
        (!m_exposureProbeSourceReceipt.has_value() ||
         !m_exposureProbeSourceReceipt->exposureHistoryCommitted))
    {
        ClearExposureProbeSourceReceipt();
    }
}

bool PostProcessPipelineFeatureAdapter::RequiresTemporalHistoryCommit() const noexcept
{
    const bool taaRequiresCommit =
        m_lastExecutionReceipt.taaRequested &&
        m_lastExecutionReceipt.taaExecuted &&
        m_lastExecutionReceipt.historyCandidateProduced &&
        !m_lastExecutionReceipt.historyCommitted;
    const bool exposureRequiresCommit =
        m_lastExecutionReceipt.exposureRequested &&
        m_lastExecutionReceipt.exposureExecuted &&
        m_lastExecutionReceipt.exposureHistoryCandidateProduced &&
        !m_lastExecutionReceipt.exposureHistoryCommitted;
    return taaRequiresCommit || exposureRequiresCommit;
}

bool PostProcessPipelineFeatureAdapter::RequestExposureQualificationProbe()
    noexcept
{
    if (m_stack == nullptr)
    {
        return false;
    }
    if (!m_stack->GetExposureController().RequestQualificationProbe())
    {
        return false;
    }
    m_lastExposureProbe = {};
    ClearExposureProbeSourceReceipt();
    return true;
}

bool PostProcessPipelineFeatureAdapter::CompleteExposureQualificationProbe(
    const RenderViewHistoryIdentity& identity,
    RenderPostProcessExecutionReceipt& outReceipt) noexcept
{
    outReceipt = {};
    if (m_stack == nullptr || !identity.IsValid() ||
        !m_exposureProbeSourceIdentity.has_value() ||
        !m_exposureProbeSourceReceipt.has_value() ||
        *m_exposureProbeSourceIdentity != identity)
    {
        return false;
    }
    ExposureQualificationProbe probe;
    if (!m_stack->GetExposureController().CompleteQualificationProbe(
            identity, probe))
    {
        return false;
    }
    m_lastExposureProbe = probe;
    RenderPostProcessExecutionReceipt completed =
        *m_exposureProbeSourceReceipt;
    if (!completed.exposureProbeRequested ||
        !completed.exposureHistoryCommitted ||
        completed.exposureProbeReady || !m_lastExposureProbe.ready ||
        m_lastExposureProbe.historyIdentity != identity ||
        m_lastExposureProbe.configurationHash == 0 ||
        m_lastExposureProbe.candidateStateContentHash == 0 ||
        m_lastExposureProbe.candidateStateContentHash !=
            completed.exposureStateContentHash)
    {
        ClearExposureProbeSourceReceipt();
        return false;
    }

    completed.exposureProbeReady = true;
    completed.targetEV100 = m_lastExposureProbe.targetEV100;
    completed.adaptedEV100 = m_lastExposureProbe.adaptedEV100;
    completed.exposureScale = m_lastExposureProbe.exposureScale;
    completed.preExposure = m_lastExposureProbe.preExposure;
    completed.previousPreExposure =
        m_lastExposureProbe.previousPreExposure;
    completed.postProcessContentHash = ExtendExposureProbeContentHash(
        completed.postProcessContentHash, m_lastExposureProbe);
    if (!completed.IsStructurallyValid())
    {
        ClearExposureProbeSourceReceipt();
        return false;
    }

    outReceipt = std::move(completed);
    ClearExposureProbeSourceReceipt();
    return true;
}

void PostProcessPipelineFeatureAdapter::SnapshotExposureProbeSourceReceipt()
    noexcept
{
    if (!m_lastExecutionReceipt.IsStructurallyValid() ||
        !m_lastExecutionReceipt.exposureProbeRequested ||
        m_lastExecutionReceipt.exposureProbeReady ||
        !m_lastPlanStats.frameInputs.historyIdentity.IsValid())
    {
        return;
    }

    const RenderViewHistoryIdentity& identity =
        m_lastPlanStats.frameInputs.historyIdentity;
    if (m_exposureProbeSourceIdentity.has_value() &&
        *m_exposureProbeSourceIdentity != identity)
    {
        return;
    }
    m_exposureProbeSourceIdentity = identity;
    m_exposureProbeSourceReceipt = m_lastExecutionReceipt;
}

void PostProcessPipelineFeatureAdapter::ClearExposureProbeSourceReceipt()
    noexcept
{
    m_exposureProbeSourceIdentity.reset();
    m_exposureProbeSourceReceipt.reset();
}

RenderFeatureExecutionReceipt
PostProcessPipelineFeatureAdapter::CollectReceipt() const
{
    RenderFeatureExecutionReceipt receipt;
    receipt.featureId = BuiltinRenderFeatureIds::PostProcessPipeline;
    receipt.featureSchemaVersion = 1;
    receipt.phase = RenderFeaturePhase::HDRPost;
    receipt.requirement = RenderFeatureRequirement::Required;
    receipt.selected = true;
    receipt.readiness = m_readiness;
    receipt.diagnostic = m_readiness.diagnostic;
    receipt.readiness.executed = false;
    m_lastReceipt = receipt;
    return receipt;
}

bool PostProcessPipelineFeatureAdapter::HasMatchingRecordContext(
    const RenderFeatureContext& context) const noexcept
{
    return m_hasRecordContext && context.frameIdentity.IsValid() &&
           context.viewIdentity.IsValid() &&
           m_recordContext.frameIdentity == context.frameIdentity &&
           m_recordContext.viewIdentity == context.viewIdentity &&
           m_recordContext.presentableColor.IsValid() &&
           m_recordContext.presentableColorMetadata.extent ==
               context.outputExtent &&
           m_recordContext.presentableColorMetadata.format !=
               RHIFormat::Unknown &&
           m_recordContext.presentableColorMetadata.sampleCount ==
               RHISampleCount::Count1;
}

bool PostProcessPipelineFeatureAdapter::HasCurrentGraphProvenance(
    const RenderFeatureGraphBuilder& builder) const noexcept
{
    return HandleMatchesBuilder(m_recordContext.presentableColor, builder);
}

bool PostProcessPipelineFeatureAdapter::HasMatchingFinalizedEvidence(
    const RenderFeatureGraphExecutionEvidence& evidence) const noexcept
{
    return m_hasComposedPresentationPlan &&
           evidence.featureId == BuiltinRenderFeatureIds::PostProcessPipeline &&
           evidence.graphIdentity == m_composedProvenance.graphIdentity &&
           evidence.recordingGeneration ==
               m_composedProvenance.recordingGeneration &&
           evidence.frameIdentity == m_preparedContext.frameIdentity &&
           evidence.viewIdentity == m_preparedContext.viewIdentity;
}

void PostProcessPipelineFeatureAdapter::SetCompositionFailure(
    std::string diagnostic)
{
    m_readiness.inputsReady = false;
    m_readiness.executed = false;
    m_readiness.diagnostic = diagnostic.empty()
        ? "Post-process graph composition failed"
        : std::move(diagnostic);
    m_composedProvenance = {};
    m_hasComposedPresentationPlan = false;
    m_hardwareSRGBEncodeBoundary = false;
}

void PostProcessPipelineFeatureAdapter::RefreshExecutionReceipt()
{
    if (!m_hasComposedPresentationPlan || !m_lastPlanStats.planValid ||
        m_lastPlanStats.planContract !=
            PostProcessPlanContract::Presentation ||
        m_lastPlanStats.planFingerprint == 0 ||
        m_lastPlanStats.scheduledToneMappingEffectCount != 1 ||
        m_lastPlanStats.fallbackCopyApplied)
    {
        m_lastExecutionReceipt = {};
        return;
    }

    RenderPostProcessExecutionReceipt receipt;
    receipt.inputColorDomain = m_lastPlanStats.inputColorDomain;
    receipt.plannedOutputColorDomain = m_lastPlanStats.outputColorDomain;
    receipt.storedOutputColorDomain =
        PostProcessColorDomain::DisplayEncodedLDR;
    receipt.finalOutputFormat = m_lastPlanStats.finalOutputFormat;
    receipt.toneMappingTransfer =
        FindScheduledToneMappingTransfer(m_lastPlanStats);
    receipt.presentationTransfer = m_hardwareSRGBEncodeBoundary
        ? PostProcessTransfer::DisplayLinearToEncoded
        : receipt.toneMappingTransfer;
    receipt.antiAliasingMode = m_stack != nullptr
        ? m_stack->GetSettings().antiAliasingMode
        : RenderAntiAliasingMode::None;

    if (m_stack == nullptr || !m_lastPlanStats.exposureOutputs.IsValid())
    {
        m_lastExecutionReceipt = {};
        return;
    }
    const ExposureControllerStats& exposureStats =
        m_stack->GetExposureController().GetLastStats();
    receipt.exposureMode = m_stack->GetSettings().exposureSettings.mode;
    receipt.exposureStateContentHash =
        m_lastPlanStats.exposureOutputs.candidateStateContentHash;
    receipt.exposureRequested = exposureStats.requested;
    receipt.exposurePipelineReady = exposureStats.pipelineReady;
    receipt.exposureInputsReady = exposureStats.inputsReady;
    receipt.exposureExecuted = exposureStats.adaptationExecuted &&
        (receipt.exposureMode == RenderExposureMode::ManualEV100 ||
         (exposureStats.histogramClearExecuted &&
          exposureStats.histogramBuildExecuted));
    receipt.exposureHistogramPassCount =
        exposureStats.histogramClearDispatchCount +
        exposureStats.histogramBuildDispatchCount;
    receipt.exposureAdaptationPassCount =
        exposureStats.adaptationDispatchCount;
    receipt.exposureHistoryAvailableBefore =
        exposureStats.usedCommittedHistory;
    receipt.exposureHistorySampled = exposureStats.usedCommittedHistory;
    receipt.exposureHistoryCandidateProduced =
        exposureStats.candidatePrepared || exposureStats.candidateAbandoned;
    receipt.exposureHistoryCommitted = exposureStats.candidateCommitted;
    receipt.exposureHistoryAbandoned = exposureStats.candidateAbandoned;
    receipt.exposureHistoryResetReasons = exposureStats.historyResetReasons;
    receipt.exposureProbeRequested =
        exposureStats.qualificationProbeRequested;
    const bool matchingExposureProbe = m_lastExposureProbe.ready &&
        m_lastExposureProbe.historyIdentity ==
            m_lastPlanStats.frameInputs.historyIdentity &&
        m_lastExposureProbe.configurationHash ==
            m_lastPlanStats.exposureOutputs.configurationHash;
    receipt.exposureProbeReady = matchingExposureProbe;
    if (matchingExposureProbe)
    {
        receipt.targetEV100 = m_lastExposureProbe.targetEV100;
        receipt.adaptedEV100 = m_lastExposureProbe.adaptedEV100;
        receipt.exposureScale = m_lastExposureProbe.exposureScale;
        receipt.preExposure = m_lastExposureProbe.preExposure;
        receipt.previousPreExposure =
            m_lastExposureProbe.previousPreExposure;
    }

    uint64 hash = Diagnostics::RVX_DIAGNOSTICS_FNV1A64_OFFSET_BASIS;
    Diagnostics::MixContentHashValue(hash, m_lastPlanStats.planFingerprint);
    Diagnostics::MixContentHashValue(hash, m_composedProvenance.graphIdentity);
    Diagnostics::MixContentHashValue(
        hash, m_composedProvenance.recordingGeneration);
    Diagnostics::MixContentHashValue(
        hash, m_preparedContext.frameIdentity.frameSequence);
    Diagnostics::MixContentHashValue(
        hash, m_preparedContext.viewIdentity.sceneRuntimeId);
    Diagnostics::MixContentHashValue(
        hash, m_preparedContext.viewIdentity.cameraEntityPacked);
    Diagnostics::MixContentHashValue(
        hash, static_cast<uint64>(m_preparedContext.viewIdentity.subViewIndex));
    Diagnostics::MixContentHashValue(
        hash, static_cast<uint64>(m_preparedContext.viewIdentity.role));
    Diagnostics::MixContentHashValue(
        hash, static_cast<uint64>(receipt.inputColorDomain));
    Diagnostics::MixContentHashValue(
        hash, static_cast<uint64>(receipt.plannedOutputColorDomain));
    Diagnostics::MixContentHashValue(
        hash, static_cast<uint64>(receipt.finalOutputFormat));
    Diagnostics::MixContentHashValue(
        hash, static_cast<uint64>(receipt.storedOutputColorDomain));
    Diagnostics::MixContentHashValue(
        hash, static_cast<uint64>(receipt.toneMappingTransfer));
    Diagnostics::MixContentHashValue(
        hash, static_cast<uint64>(receipt.presentationTransfer));
    Diagnostics::MixContentHashValue(
        hash, static_cast<uint64>(receipt.antiAliasingMode));

    for (const PostProcessEffectExecutionPlan& effect :
         m_lastPlanStats.effectPlans)
    {
        if (!effect.scheduled)
        {
            continue;
        }
        Diagnostics::MixContentHashValue(
            hash, static_cast<uint64>(effect.passId));
        Diagnostics::MixContentHashValue(
            hash, static_cast<uint64>(effect.stage));
        Diagnostics::MixContentHashValue(
            hash, static_cast<uint64>(effect.order));
        Diagnostics::MixContentHashValue(
            hash, static_cast<uint64>(effect.inputDomain));
        Diagnostics::MixContentHashValue(
            hash, static_cast<uint64>(effect.outputDomain));
        Diagnostics::MixContentHashValue(
            hash, static_cast<uint64>(effect.transfer));

        switch (effect.passId)
        {
            case PostProcessPassId::ToneMapping:
                ++receipt.toneMapPassCount;
                break;
            case PostProcessPassId::Bloom:
                ++receipt.bloomPassCount;
                break;
            case PostProcessPassId::ColorGrading:
                ++receipt.colorGradingPassCount;
                break;
            case PostProcessPassId::FXAA:
                ++receipt.fxaaPassCount;
                break;
            case PostProcessPassId::TemporalAntiAliasing:
                ++receipt.taaPassCount;
                break;
            default: break;
        }
    }

    if (receipt.antiAliasingMode == RenderAntiAliasingMode::TAA)
    {
        const TAA* const taa = m_stack != nullptr
            ? m_stack->GetEffect<TAA>()
            : nullptr;
        if (taa == nullptr)
        {
            m_lastExecutionReceipt = {};
            return;
        }

        const TAAResolveStats& taaStats = taa->GetLastResolveStats();
        receipt.taaRequested = taaStats.requested;
        receipt.taaPipelineReady = taaStats.pipelineReady;
        receipt.taaInputsReady = taaStats.inputsReady;
        receipt.taaExecuted = taaStats.executed;
        receipt.historyAvailableBefore = taaStats.historyAvailableBefore;
        receipt.historySampled = taaStats.historySampled;
        // An abandoned candidate was still physically produced by the exact
        // graph; retain that fact in the receipt even though TAA clears its
        // mutable pending slot while abandoning it.
        receipt.historyCandidateProduced =
            taaStats.historyCandidatePrepared || taaStats.historyAbandoned;
        receipt.historyCommitted = taaStats.historyCommitted;
        receipt.historyAbandoned = taaStats.historyAbandoned;
        receipt.historyResetReasons = taaStats.historyResetReasons;
        receipt.currentJitterPixels = taaStats.currentJitterPixels;
        receipt.previousJitterPixels = taaStats.previousJitterPixels;
        receipt.taaResolveGraphPassCount = taaStats.resolveGraphPassCount;
        receipt.taaHistoryStoreGraphPassCount = taaStats.historyStoreGraphPassCount;
    }

    const ToneMappingPass* const toneMapping =
        m_stack->GetEffect<ToneMappingPass>();
    receipt.toneMapExposureStateHash = toneMapping != nullptr
        ? toneMapping->GetLastExposureStateContentHash()
        : 0;
    const BloomPass* const bloom = m_stack->GetEffect<BloomPass>();
    receipt.bloomExposureStateHash = receipt.bloomPassCount != 0 && bloom != nullptr
        ? bloom->GetLastExposureStateContentHash()
        : 0;
    const TAA* const exposureTAA =
        receipt.antiAliasingMode == RenderAntiAliasingMode::TAA
            ? m_stack->GetEffect<TAA>()
            : nullptr;
    receipt.taaExposureStateHash = exposureTAA != nullptr
        ? exposureTAA->GetLastResolveStats().exposureStateContentHash
        : 0;

    // Temporal state is filled from the actual graph-backed TAA execution,
    // rather than from settings.  Bind it into the receipt fingerprint only
    // after that evidence is available, including the post-Present state.
    Diagnostics::MixContentHashValue(hash, receipt.taaRequested ? 1u : 0u);
    Diagnostics::MixContentHashValue(hash, receipt.taaPipelineReady ? 1u : 0u);
    Diagnostics::MixContentHashValue(hash, receipt.taaInputsReady ? 1u : 0u);
    Diagnostics::MixContentHashValue(hash, receipt.taaExecuted ? 1u : 0u);
    Diagnostics::MixContentHashValue(
        hash, receipt.historyAvailableBefore ? 1u : 0u);
    Diagnostics::MixContentHashValue(hash, receipt.historySampled ? 1u : 0u);
    Diagnostics::MixContentHashValue(
        hash, receipt.historyCandidateProduced ? 1u : 0u);
    Diagnostics::MixContentHashValue(hash, receipt.historyCommitted ? 1u : 0u);
    Diagnostics::MixContentHashValue(hash, receipt.historyAbandoned ? 1u : 0u);
    Diagnostics::MixContentHashValue(
        hash, static_cast<uint64>(receipt.historyResetReasons));
    MixFloat(hash, receipt.currentJitterPixels.x);
    MixFloat(hash, receipt.currentJitterPixels.y);
    MixFloat(hash, receipt.previousJitterPixels.x);
    MixFloat(hash, receipt.previousJitterPixels.y);
    Diagnostics::MixContentHashValue(hash, receipt.taaResolveGraphPassCount);
    Diagnostics::MixContentHashValue(hash, receipt.taaHistoryStoreGraphPassCount);
    Diagnostics::MixContentHashValue(hash, receipt.exposureStateContentHash);
    Diagnostics::MixContentHashValue(hash, receipt.toneMapExposureStateHash);
    Diagnostics::MixContentHashValue(hash, receipt.bloomExposureStateHash);
    Diagnostics::MixContentHashValue(hash, receipt.taaExposureStateHash);
    Diagnostics::MixContentHashValue(
        hash, static_cast<uint64>(receipt.exposureMode));
    Diagnostics::MixContentHashValue(hash, receipt.exposureRequested ? 1u : 0u);
    Diagnostics::MixContentHashValue(hash, receipt.exposurePipelineReady ? 1u : 0u);
    Diagnostics::MixContentHashValue(hash, receipt.exposureInputsReady ? 1u : 0u);
    Diagnostics::MixContentHashValue(hash, receipt.exposureExecuted ? 1u : 0u);
    Diagnostics::MixContentHashValue(hash, receipt.exposureHistogramPassCount);
    Diagnostics::MixContentHashValue(hash, receipt.exposureAdaptationPassCount);
    Diagnostics::MixContentHashValue(
        hash, receipt.exposureHistoryAvailableBefore ? 1u : 0u);
    Diagnostics::MixContentHashValue(
        hash, receipt.exposureHistorySampled ? 1u : 0u);
    Diagnostics::MixContentHashValue(
        hash, receipt.exposureHistoryCandidateProduced ? 1u : 0u);
    Diagnostics::MixContentHashValue(
        hash, receipt.exposureHistoryCommitted ? 1u : 0u);
    Diagnostics::MixContentHashValue(
        hash, receipt.exposureHistoryAbandoned ? 1u : 0u);
    Diagnostics::MixContentHashValue(
        hash, static_cast<uint64>(receipt.exposureHistoryResetReasons));
    Diagnostics::MixContentHashValue(
        hash, receipt.exposureProbeRequested ? 1u : 0u);
    Diagnostics::MixContentHashValue(hash, receipt.exposureProbeReady ? 1u : 0u);
    if (receipt.exposureProbeReady)
    {
        MixFloat(hash, receipt.targetEV100);
        MixFloat(hash, receipt.adaptedEV100);
        MixFloat(hash, receipt.exposureScale);
        MixFloat(hash, receipt.preExposure);
        MixFloat(hash, receipt.previousPreExposure);
    }

    receipt.postProcessContentHash = hash;
    receipt.hdrInputReady =
        m_lastPlanStats.planValid &&
        m_lastPlanStats.frameInputs.sceneColor.IsValid() &&
        m_lastPlanStats.inputColorDomain ==
            PostProcessColorDomain::SceneLinearHDR;
    receipt.displayLinearOutputReady = std::any_of(
        m_lastPlanStats.effectPlans.begin(),
        m_lastPlanStats.effectPlans.end(),
        [](const PostProcessEffectExecutionPlan& effect)
        {
            return effect.scheduled && effect.outputDomain ==
                PostProcessColorDomain::DisplayLinearLDR;
        });
    receipt.encodedOutputReady =
        m_hardwareSRGBEncodeBoundary ||
        m_lastPlanStats.outputColorDomain ==
            PostProcessColorDomain::DisplayEncodedLDR;
    receipt.fallbackUsed = m_lastPlanStats.fallbackCopyApplied;
    m_lastExecutionReceipt = receipt.IsStructurallyValid()
        ? std::move(receipt)
        : RenderPostProcessExecutionReceipt{};
}
} // namespace RVX
