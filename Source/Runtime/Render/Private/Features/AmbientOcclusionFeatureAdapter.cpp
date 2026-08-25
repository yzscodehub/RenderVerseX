/**
 * @file AmbientOcclusionFeatureAdapter.cpp
 * @brief Typed composition boundary for the renderer-owned GTAO field.
 */

#include "Features/AmbientOcclusionFeatureAdapter.h"

#include "Core/Diagnostics/ContentHash.h"
#include "Render/PipelineCache.h"

#include <limits>
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

    [[nodiscard]] bool IsRasterRecipe(RenderPipelineRecipe recipe) noexcept
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

    void MixIdentity(uint64& hash, const RenderFrameIdentity& identity) noexcept
    {
        Diagnostics::MixContentHashValue(hash, identity.frameSequence);
        Diagnostics::MixContentHashValue(hash, identity.sceneRevision);
        Diagnostics::MixContentHashValue(hash, identity.surfaceGeneration);
        Diagnostics::MixContentHashValue(hash, identity.configurationRevision);
    }

    void MixIdentity(uint64& hash, const RenderViewIdentity& identity) noexcept
    {
        Diagnostics::MixContentHashValue(hash, identity.sceneRuntimeId);
        Diagnostics::MixContentHashValue(hash, identity.cameraEntityPacked);
        Diagnostics::MixContentHashValue(hash, identity.subViewIndex);
    }
} // namespace

AmbientOcclusionFeatureAdapter::AmbientOcclusionFeatureAdapter(
    PipelineCache* pipelineCache) noexcept
    : m_pipelineCache(pipelineCache)
{
    m_gtao.SetResources(pipelineCache);
}

void AmbientOcclusionFeatureAdapter::SetRecordContext(
    AmbientOcclusionFeatureRecordContext context)
{
    m_recordContext = std::move(context);
    m_hasRecordContext = true;
    m_preparedContext = {};
    m_composedProvenance = {};
    m_composedOutput = {};
    m_velocityFieldIdentity = 0;
    m_lastReceipt = {};
    m_lastExecutionReceipt = {};
    m_readiness = {};
    m_outputPublished = false;
    m_gtao.SetSettings(m_recordContext.settings);
}

void AmbientOcclusionFeatureAdapter::SetPipelineCache(
    PipelineCache* pipelineCache) noexcept
{
    if (m_pipelineCache == pipelineCache)
    {
        return;
    }
    m_pipelineCache = pipelineCache;
    m_gtao.SetResources(pipelineCache);
    m_hasRecordContext = false;
    m_recordContext = {};
    m_preparedContext = {};
    m_composedProvenance = {};
    m_composedOutput = {};
    m_velocityFieldIdentity = 0;
    m_lastReceipt = {};
    m_lastExecutionReceipt = {};
    m_readiness = {};
    m_outputPublished = false;
}

RenderFeatureDescriptor AmbientOcclusionFeatureAdapter::Describe() const
{
    RenderFeatureDescriptor descriptor;
    descriptor.featureId = BuiltinRenderFeatureIds::AmbientOcclusion;
    descriptor.schemaVersion = 1;
    descriptor.name = "AmbientOcclusion";
    descriptor.phase = RenderFeaturePhase::Geometry;
    descriptor.consumedResources = {
        {RenderBlackboardResourceId::SceneTextures,
         SceneTextures::SchemaVersion}};
    descriptor.producedResources = {
        {RenderBlackboardResourceId::AmbientOcclusionOutputs,
         AmbientOcclusionOutputs::SchemaVersion}};
    descriptor.requiredCapabilities =
        ToMask(RenderFeatureCapability::Graphics) |
        ToMask(RenderFeatureCapability::DescriptorBindings);
    descriptor.requiredRecipes =
        ToMask(RenderPipelineRecipe::ForwardStandard) |
        ToMask(RenderPipelineRecipe::DeferredQuality);
    descriptor.qualityTiers = RVX_ALL_RENDER_QUALITY_TIERS;
    descriptor.fallbackPolicy = RenderFeatureFallbackPolicy::None;
    descriptor.shaderPipelineFamily = "GroundTruthAmbientOcclusion";
    return descriptor;
}

void AmbientOcclusionFeatureAdapter::Prepare(
    const RenderFeatureContext& context)
{
    m_preparedContext = context;
    m_readiness = {};
    m_readiness.compiled = true;
    m_readiness.hardwareSupported = HasGraphicsCapability(
        context.hardwareCapabilities);
    m_readiness.implementationSupported = IsRasterRecipe(
        context.resolvedRecipe);
    m_readiness.pipelineReady =
        !m_recordContext.settings.enabled || HasRequiredPipelines();
    m_readiness.inputsReady = HasMatchingRecordContext(context) &&
        IsValidRenderAmbientOcclusionSettings(m_recordContext.settings);

    if (!m_readiness.hardwareSupported)
    {
        m_readiness.diagnostic = "Graphics capability is unavailable";
    }
    else if (!m_readiness.implementationSupported)
    {
        m_readiness.diagnostic =
            "Ambient occlusion only implements raster recipes";
    }
    else if (!m_readiness.pipelineReady)
    {
        m_readiness.diagnostic =
            "Ground-truth ambient-occlusion pipelines are unavailable";
    }
    else if (!m_readiness.inputsReady)
    {
        m_readiness.diagnostic =
            "Ambient-occlusion record context is absent, invalid, stale, or mismatched";
    }
    m_readiness.executed = false;
}

RenderFeatureReadiness
AmbientOcclusionFeatureAdapter::ResolveReadiness() const
{
    return m_readiness;
}

void AmbientOcclusionFeatureAdapter::AddPasses(
    RenderFeatureGraphBuilder& builder,
    RenderGraphBlackboard& blackboard)
{
    m_composedProvenance = {};
    m_composedOutput = {};
    m_velocityFieldIdentity = 0;
    m_lastReceipt = {};
    m_lastExecutionReceipt = {};
    m_readiness.executed = false;
    m_outputPublished = false;
    if (!m_readiness.IsReadyToExecute())
    {
        return;
    }
    if (!HasCurrentGraphProvenance(builder))
    {
        SetCompositionFailure(
            "Ambient-occlusion record context belongs to another graph");
        return;
    }

    RenderGraphBlackboardCode code = RenderGraphBlackboardCode::Success;
    const SceneTextures* const textures = blackboard.Require<SceneTextures>(
        BuiltinRenderFeatureIds::AmbientOcclusion, Describe(), &code);
    if (textures == nullptr || code != RenderGraphBlackboardCode::Success ||
        !HandleMatchesBuilder(textures->deviceDepth, builder) ||
        textures->deviceDepthMetadata.extent !=
            m_preparedContext.outputExtent ||
        textures->deviceDepthMetadata.format != RHIFormat::D32_FLOAT ||
        textures->deviceDepthMetadata.sampleCount != RHISampleCount::Count1)
    {
        SetCompositionFailure(
            "Ambient occlusion requires the current D32 SceneTextures depth field");
        return;
    }

    if (m_recordContext.settings.enabled &&
        (!HandleMatchesBuilder(textures->velocity, builder) ||
         textures->velocityMetadata.extent !=
             m_preparedContext.outputExtent ||
         textures->velocityMetadata.format != RHIFormat::RG16_FLOAT ||
         textures->velocityMetadata.sampleCount != RHISampleCount::Count1))
    {
        SetCompositionFailure(
            "Temporal GTAO requires the current full-resolution RG16_FLOAT velocity field");
        return;
    }

    const bool deferred = m_preparedContext.resolvedRecipe ==
        RenderPipelineRecipe::DeferredQuality;
    if (deferred &&
        (!HandleMatchesBuilder(textures->gBuffer1, builder) ||
         textures->gBuffer1Metadata.extent !=
             m_preparedContext.outputExtent ||
         textures->gBuffer1Metadata.format != RHIFormat::RGBA8_UNORM ||
         textures->gBuffer1Metadata.sampleCount != RHISampleCount::Count1))
    {
        SetCompositionFailure(
            "Deferred ambient occlusion requires the current GBuffer1 normal field");
        return;
    }

    GroundTruthAmbientOcclusionFrameInputs inputs;
    inputs.deviceDepth = textures->deviceDepth;
    inputs.normalSource = deferred ? textures->gBuffer1 : RGTextureHandle{};
    inputs.velocity = textures->velocity;
    inputs.historyIdentity = m_recordContext.historyIdentity;
    inputs.historyResetReasons = m_recordContext.historyResetReasons;
    inputs.surfaceFieldHash = CalculateSurfaceFieldHash(*textures);
    inputs.historyAvailable = m_recordContext.historyAvailable &&
        m_gtao.HasValidHistory();
    m_velocityFieldIdentity = m_recordContext.settings.enabled
        ? CalculateVelocityFieldIdentity(*textures)
        : 0;
    if (m_recordContext.settings.enabled && m_velocityFieldIdentity == 0)
    {
        SetCompositionFailure(
            "Temporal GTAO could not bind the exact velocity field identity");
        return;
    }

    if (m_qualificationProbeRequestedForNextFrame)
    {
        if (!m_recordContext.settings.enabled ||
            !m_gtao.ArmQualificationProbe(inputs.historyIdentity))
        {
            m_gtao.AbandonQualificationProbe();
            m_qualificationProbeRequestedForNextFrame = false;
            SetCompositionFailure(
                "Ambient-occlusion qualification probe could not bind the exact enabled view");
            return;
        }
        m_qualificationProbeRequestedForNextFrame = false;
    }

    bool materialized = false;
    if (!builder.AddGraphCallback(
            "GroundTruthAmbientOcclusion",
            m_recordContext.passRecordContext,
            [this, &materialized, inputs](RenderGraph& graph)
            {
                m_composedOutput = m_gtao.AddToGraph(
                    graph, m_recordContext.passRecordContext.view, inputs);
                materialized = m_composedOutput.IsValid();
            }) ||
        !materialized)
    {
        const std::string& fallbackReason = m_gtao.GetLastStats().fallbackReason;
        SetCompositionFailure(
            fallbackReason.empty()
                ? "Ground-truth ambient occlusion could not materialize its graph"
                : fallbackReason);
        return;
    }

    AmbientOcclusionOutputs outputs;
    outputs.provenance = builder.GetProvenance();
    outputs.frameIdentity = m_preparedContext.frameIdentity;
    outputs.viewIdentity = m_preparedContext.viewIdentity;
    outputs.historyIdentity = m_recordContext.historyIdentity;
    outputs.historyResetReasons =
        m_gtao.GetLastStats().historyResetReasons;
    outputs.settingsHash = m_composedOutput.metadata.settingsHash;
    outputs.consumerMask = m_composedOutput.metadata.consumerMask;
    outputs.ambientOcclusionFieldHash =
        m_composedOutput.metadata.ambientOcclusionFieldHash;
    outputs.ambientOcclusion = m_composedOutput.visibility;
    outputs.ambientOcclusionMetadata = SceneTextures::Metadata{
        m_preparedContext.outputExtent,
        m_composedOutput.metadata.format,
        RHISampleCount::Count1};
    if (blackboard.Publish(BuiltinRenderFeatureIds::AmbientOcclusion,
                           Describe(), std::move(outputs)) !=
        RenderGraphBlackboardCode::Success)
    {
        SetCompositionFailure(
            "Ambient occlusion could not publish AmbientOcclusionOutputs v1");
        return;
    }

    m_composedProvenance = builder.GetProvenance();
    m_outputPublished = true;
}

void AmbientOcclusionFeatureAdapter::OnGraphExecutionFinalized(
    const RenderFeatureGraphExecutionEvidence& evidence)
{
    m_lastExecutionReceipt = {};
    const bool executionAccepted = m_recordContext.settings.enabled &&
        HasMatchingFinalizedEvidence(evidence) &&
        evidence.HasExactExecution();
    m_gtao.FinalizeGraphExecution(executionAccepted);
    if (!executionAccepted)
    {
        return;
    }
    RefreshExecutionReceipt();
}

RenderFeatureExecutionReceipt
AmbientOcclusionFeatureAdapter::CollectReceipt() const
{
    RenderFeatureExecutionReceipt receipt;
    receipt.featureId = BuiltinRenderFeatureIds::AmbientOcclusion;
    receipt.featureSchemaVersion = 1;
    receipt.phase = RenderFeaturePhase::Geometry;
    receipt.requirement = RenderFeatureRequirement::Required;
    receipt.selected = true;
    receipt.readiness = m_readiness;
    receipt.diagnostic = m_readiness.diagnostic;
    receipt.readiness.executed = false;
    m_lastReceipt = receipt;
    return receipt;
}

bool AmbientOcclusionFeatureAdapter::RequiresHistoryCommit() const noexcept
{
    const GroundTruthAmbientOcclusionStats& stats = m_gtao.GetLastStats();
    return m_recordContext.settings.enabled && stats.executed &&
           stats.historyCandidatePrepared && !stats.historyCommitted &&
           !stats.historyAbandoned;
}

bool AmbientOcclusionFeatureAdapter::CanCommitHistoryAfterPresent(
    const RenderViewHistoryIdentity& identity) const noexcept
{
    return !RequiresHistoryCommit() ||
        m_gtao.CanCommitHistoryAfterPresent(identity);
}

bool AmbientOcclusionFeatureAdapter::NotifyHistorySubmission(
    const RenderViewHistoryIdentity& identity) noexcept
{
    return !RequiresHistoryCommit() ||
        m_gtao.NotifyHistorySubmission(identity);
}

bool AmbientOcclusionFeatureAdapter::CommitHistoryAfterPresent(
    const RenderViewHistoryIdentity& identity) noexcept
{
    if (!RequiresHistoryCommit())
    {
        return true;
    }
    const bool committed = m_gtao.CommitHistoryAfterPresent(identity);
    RefreshExecutionReceipt();
    return committed;
}

void AmbientOcclusionFeatureAdapter::AbandonHistoryCandidate() noexcept
{
    // Registry validation can reject a recording before final evidence is
    // delivered. Discard callback-only state before abandoning an accepted
    // presentation candidate.
    m_gtao.FinalizeGraphExecution(false);
    m_gtao.AbandonPendingHistory();
    RefreshExecutionReceipt();
}

void AmbientOcclusionFeatureAdapter::ResetHistory() noexcept
{
    m_gtao.ResetHistory();
    m_lastExecutionReceipt = {};
}

bool AmbientOcclusionFeatureAdapter::RequestQualificationProbe() noexcept
{
    if (m_qualificationProbeRequestedForNextFrame ||
        m_gtao.HasPendingQualificationProbe())
    {
        return false;
    }
    m_qualificationProbeRequestedForNextFrame = true;
    return true;
}

bool AmbientOcclusionFeatureAdapter::NotifyQualificationProbeSubmission(
    const RenderViewHistoryIdentity& identity,
    const GPUCompletionToken& completion,
    const RenderSubmissionTracker& tracker) noexcept
{
    return m_gtao.NotifyQualificationProbeSubmission(
        identity, completion, tracker);
}

bool AmbientOcclusionFeatureAdapter::CompleteQualificationProbe(
    const RenderSubmissionTracker& tracker,
    RenderAmbientOcclusionExecutionReceipt& inOutReceipt) noexcept
{
    GroundTruthAmbientOcclusionQualificationProbe probe;
    if (!m_gtao.PollQualificationProbe(tracker, probe))
    {
        return false;
    }
    if (!probe.IsValid() || !inOutReceipt.requested ||
        !inOutReceipt.executed || !inOutReceipt.historyCommitted ||
        inOutReceipt.settingsHash != probe.settingsHash ||
        inOutReceipt.aoFieldContentHash !=
            probe.ambientOcclusionFieldHash ||
        probe.sampleCount > std::numeric_limits<uint32>::max() ||
        probe.occludedCount > std::numeric_limits<uint32>::max())
    {
        return false;
    }

    inOutReceipt.probeRequested = true;
    inOutReceipt.probeReady = true;
    inOutReceipt.probeSampleCount = static_cast<uint32>(probe.sampleCount);
    inOutReceipt.probeOccludedSampleCount =
        static_cast<uint32>(probe.occludedCount);
    inOutReceipt.probeMinVisibility = probe.minimumVisibility;
    inOutReceipt.probeMeanVisibility = probe.meanVisibility;
    inOutReceipt.probeMaxVisibility = probe.maximumVisibility;
    inOutReceipt.diagnostic.clear();
    return inOutReceipt.IsStructurallyValid();
}

void AmbientOcclusionFeatureAdapter::AbandonQualificationProbe() noexcept
{
    m_qualificationProbeRequestedForNextFrame = false;
    m_gtao.AbandonQualificationProbe();
}

bool AmbientOcclusionFeatureAdapter::HasPendingQualificationProbe()
    const noexcept
{
    return m_qualificationProbeRequestedForNextFrame ||
        m_gtao.HasPendingQualificationProbe();
}

void AmbientOcclusionFeatureAdapter::Shutdown() noexcept
{
    m_qualificationProbeRequestedForNextFrame = false;
    m_gtao.Shutdown();
    m_pipelineCache = nullptr;
    m_hasRecordContext = false;
    m_recordContext = {};
    m_preparedContext = {};
    m_composedProvenance = {};
    m_composedOutput = {};
    m_lastReceipt = {};
    m_lastExecutionReceipt = {};
    m_readiness = {};
    m_outputPublished = false;
}

bool AmbientOcclusionFeatureAdapter::HasMatchingRecordContext(
    const RenderFeatureContext& context) const noexcept
{
    const RenderPassRecordContext& pass = m_recordContext.passRecordContext;
    return m_hasRecordContext && context.frameIdentity.IsValid() &&
           context.viewIdentity.IsValid() &&
           pass.featureFrameIdentity == context.frameIdentity &&
           pass.featureViewIdentity == context.viewIdentity &&
           pass.IsFrameIdentityValid() &&
           pass.identity.frameSequence == context.frameIdentity.frameSequence &&
           pass.identity.viewOrdinal == context.recordingViewOrdinal &&
           pass.identity.recordEpoch == context.recordEpoch &&
           m_recordContext.historyIdentity.IsValid() &&
           m_recordContext.historyIdentity.view == context.viewIdentity;
}

bool AmbientOcclusionFeatureAdapter::HasCurrentGraphProvenance(
    const RenderFeatureGraphBuilder& builder) const noexcept
{
    return builder.IsValid() &&
           m_recordContext.passRecordContext.MatchesTargetGraph(
               builder.GetGraph());
}

bool AmbientOcclusionFeatureAdapter::HasMatchingFinalizedEvidence(
    const RenderFeatureGraphExecutionEvidence& evidence) const noexcept
{
    return evidence.featureId == BuiltinRenderFeatureIds::AmbientOcclusion &&
           evidence.graphIdentity == m_composedProvenance.graphIdentity &&
           evidence.recordingGeneration ==
               m_composedProvenance.recordingGeneration &&
           evidence.frameIdentity == m_preparedContext.frameIdentity &&
           evidence.viewIdentity == m_preparedContext.viewIdentity &&
           m_outputPublished;
}

bool AmbientOcclusionFeatureAdapter::HasRequiredPipelines() const noexcept
{
    return m_pipelineCache != nullptr &&
           m_pipelineCache->GetGTAOSurfacePipeline() != nullptr &&
           m_pipelineCache->GetGTAOHorizonPipeline() != nullptr &&
           m_pipelineCache->GetGTAOBilateralHorizontalPipeline() != nullptr &&
           m_pipelineCache->GetGTAOBilateralVerticalPipeline() != nullptr &&
           m_pipelineCache->GetGTAOTemporalPipeline() != nullptr &&
           m_pipelineCache->GetGTAOLayout() != nullptr &&
           m_pipelineCache->GetGTAOSetLayout() != nullptr;
}

bool AmbientOcclusionFeatureAdapter::HasExactVelocityProducerExecution()
    const noexcept
{
    const RenderPassRecordContext& pass =
        m_recordContext.passRecordContext;
    if (pass.results == nullptr || pass.results->identity != pass.identity ||
        !pass.identity.IsValid() || !pass.view.velocityTarget.IsValid())
    {
        return false;
    }

    const CameraVelocityPassStats& camera =
        pass.results->cameraVelocityStats;
    const bool historySampled = m_gtao.GetLastStats().historySampled;
    const bool cameraExecutionValid =
        camera.requested && camera.supported && camera.depthAvailable &&
        camera.velocityTargetAvailable && camera.outputDeclared &&
        camera.constantsUploaded && camera.velocityRecorded &&
        (!historySampled || camera.previousViewProjectionAvailable) &&
        camera.width == m_preparedContext.outputExtent.width &&
        camera.height == m_preparedContext.outputExtent.height &&
        camera.outputFormat == RHIFormat::RG16_FLOAT &&
        camera.MatchesVelocityTarget(pass.view.velocityTarget, pass.identity);
    if (!cameraExecutionValid)
    {
        return false;
    }

    // CameraVelocity establishes the full-screen field. ObjectVelocity then
    // overlays every drawable that has valid transform/deformation history.
    // A newly visible object may legitimately have no history, but missing
    // resources, UVs, material bindings, or a stale target must invalidate the
    // producer chain instead of silently degrading temporal GTAO.
    const ObjectVelocityPassStats& object =
        pass.results->objectVelocityStats;
    const bool objectEvaluationValid =
        object.requested && object.supported &&
        object.velocityTargetAvailable && object.depthAvailable &&
        (!historySampled || object.previousViewProjectionAvailable) &&
        object.width == m_preparedContext.outputExtent.width &&
        object.height == m_preparedContext.outputExtent.height &&
        object.outputFormat == RHIFormat::RG16_FLOAT &&
        object.MatchesVelocityTarget(pass.view.velocityTarget, pass.identity) &&
        object.skippedMissingResourceCount == 0 &&
        object.skippedMissingUVCount == 0 &&
        object.skippedMaterialBindingCount == 0;
    if (!objectEvaluationValid)
    {
        return false;
    }

    if (!object.previousViewProjectionAvailable ||
        !object.drawItemsAvailable)
    {
        return !object.outputDeclared && !object.velocityRecorded &&
               object.drawCount == 0;
    }

    const bool everyDrawItemAccountedFor =
        object.drawCount + object.skippedNoHistoryCount ==
        object.drawItemCount;
    if (object.drawCount == 0)
    {
        return everyDrawItemAccountedFor && !object.outputDeclared &&
               !object.velocityRecorded;
    }
    return everyDrawItemAccountedFor && object.outputDeclared &&
           object.velocityRecorded &&
           object.objectsWithHistory == object.drawCount;
}

uint64 AmbientOcclusionFeatureAdapter::CalculateSurfaceFieldHash(
    const SceneTextures& textures) const noexcept
{
    uint64 hash = Diagnostics::RVX_DIAGNOSTICS_FNV1A64_OFFSET_BASIS;
    Diagnostics::MixContentHashValue(hash, 0x4754414F53555246ull);
    MixIdentity(hash, m_preparedContext.frameIdentity);
    MixIdentity(hash, m_preparedContext.viewIdentity);
    Diagnostics::MixContentHashValue(hash, textures.deviceDepth.index);
    Diagnostics::MixContentHashValue(hash, textures.deviceDepth.graphIdentity);
    Diagnostics::MixContentHashValue(
        hash, textures.deviceDepth.recordingGeneration);
    Diagnostics::MixContentHashValue(hash, textures.gBuffer1.index);
    Diagnostics::MixContentHashValue(hash, textures.velocity.index);
    Diagnostics::MixContentHashValue(hash, textures.velocity.graphIdentity);
    Diagnostics::MixContentHashValue(
        hash, textures.velocity.recordingGeneration);
    Diagnostics::MixContentHashValue(
        hash, static_cast<uint64>(m_preparedContext.resolvedRecipe));
    return hash != 0 ? hash : 1u;
}

uint64 AmbientOcclusionFeatureAdapter::CalculateVelocityFieldIdentity(
    const SceneTextures& textures) const noexcept
{
    if (!textures.velocity.IsValid() ||
        textures.velocityMetadata.extent != m_preparedContext.outputExtent ||
        textures.velocityMetadata.format != RHIFormat::RG16_FLOAT ||
        textures.velocityMetadata.sampleCount != RHISampleCount::Count1)
    {
        return 0;
    }

    uint64 hash = Diagnostics::RVX_DIAGNOSTICS_FNV1A64_OFFSET_BASIS;
    Diagnostics::MixContentHashValue(hash, 0x4754414F56454C4FULL);
    MixIdentity(hash, m_preparedContext.frameIdentity);
    MixIdentity(hash, m_preparedContext.viewIdentity);
    Diagnostics::MixContentHashValue(hash, textures.velocity.index);
    Diagnostics::MixContentHashValue(hash, textures.velocity.graphIdentity);
    Diagnostics::MixContentHashValue(
        hash, textures.velocity.recordingGeneration);
    Diagnostics::MixContentHashValue(
        hash, textures.velocityMetadata.extent.width);
    Diagnostics::MixContentHashValue(
        hash, textures.velocityMetadata.extent.height);
    Diagnostics::MixContentHashValue(
        hash, static_cast<uint64>(textures.velocityMetadata.format));
    Diagnostics::MixContentHashValue(
        hash, static_cast<uint64>(textures.velocityMetadata.sampleCount));
    return hash != 0 ? hash : 1u;
}

uint64 AmbientOcclusionFeatureAdapter::CalculatePlanContentHash(
    const GroundTruthAmbientOcclusionStats& stats) const noexcept
{
    uint64 hash = Diagnostics::RVX_DIAGNOSTICS_FNV1A64_OFFSET_BASIS;
    Diagnostics::MixContentHashValue(hash, 0x4754414F504C414Eull);
    MixIdentity(hash, m_preparedContext.frameIdentity);
    MixIdentity(hash, m_preparedContext.viewIdentity);
    Diagnostics::MixContentHashValue(hash, stats.settingsHash);
    Diagnostics::MixContentHashValue(hash, stats.surfaceFieldHash);
    Diagnostics::MixContentHashValue(hash, stats.ambientOcclusionFieldHash);
    Diagnostics::MixContentHashValue(
        hash, static_cast<uint64>(stats.historyResetReasons));
    return hash != 0 ? hash : 1u;
}

void AmbientOcclusionFeatureAdapter::RefreshExecutionReceipt()
{
    if (!m_recordContext.settings.enabled)
    {
        m_lastExecutionReceipt = {};
        return;
    }

    const GroundTruthAmbientOcclusionStats& stats = m_gtao.GetLastStats();
    RenderAmbientOcclusionExecutionReceipt receipt;
    receipt.requested = stats.requested;
    receipt.linearDepthInputReady = stats.depthAvailable;
    receipt.worldNormalInputReady =
        stats.normalSourceAvailable || stats.normalReconstructed;
    receipt.velocityInputReady = stats.velocityAvailable;
    receipt.velocityProducerExecuted =
        HasExactVelocityProducerExecution();
    receipt.pipelineReady = stats.pipelineReady;
    receipt.inputsReady = stats.inputsReady;
    receipt.outputPublished = m_outputPublished && stats.executed;
    receipt.executed = stats.executed;
    receipt.historyAvailableBefore = stats.historyAvailableBefore;
    receipt.historySampled = stats.historySampled;
    receipt.motionReprojectionUsed =
        stats.historySampled && stats.velocityAvailable &&
        receipt.velocityProducerExecuted;
    receipt.viewProjectionMotionObserved =
        receipt.motionReprojectionUsed &&
        m_recordContext.passRecordContext.results != nullptr &&
        m_recordContext.passRecordContext.results->identity ==
            m_recordContext.passRecordContext.identity &&
        m_recordContext.passRecordContext.results->cameraVelocityStats
            .viewProjectionMotionObserved;
    receipt.historyCandidateProduced = stats.historyCandidatePrepared;
    receipt.historyCommitted = stats.historyCommitted;
    receipt.historyAbandoned = stats.historyAbandoned;
    receipt.historyResetReasons = stats.historyResetReasons;
    receipt.consumerMask = m_recordContext.settings.consumerMask;
    receipt.outputFormat = stats.executed ? RHIFormat::R8_UNORM
                                          : RHIFormat::Unknown;
    receipt.horizonSearchGraphPassCount =
        stats.horizonSearchGraphPassCount;
    receipt.bilateralDenoiseGraphPassCount =
        stats.bilateralDenoiseGraphPassCount;
    receipt.temporalResolveGraphPassCount =
        stats.temporalResolveGraphPassCount;
    receipt.planContentHash = CalculatePlanContentHash(stats);
    receipt.settingsHash = stats.settingsHash;
    receipt.aoFieldContentHash = stats.ambientOcclusionFieldHash;
    receipt.velocityFieldIdentity = m_velocityFieldIdentity;
    receipt.probeRequested = stats.qualificationProbeRequested;
    receipt.probeReady = stats.qualificationProbeReady;
    receipt.diagnostic = !stats.qualificationProbeFailure.empty()
        ? stats.qualificationProbeFailure
        : stats.fallbackReason;
    m_lastExecutionReceipt = std::move(receipt);
}

void AmbientOcclusionFeatureAdapter::SetCompositionFailure(
    std::string diagnostic)
{
    m_qualificationProbeRequestedForNextFrame = false;
    m_gtao.FinalizeGraphExecution(false);
    m_readiness.inputsReady = false;
    m_readiness.executed = false;
    m_readiness.diagnostic = diagnostic.empty()
        ? "Ambient-occlusion graph composition failed"
        : std::move(diagnostic);
    m_outputPublished = false;
    m_lastExecutionReceipt = {};
}
} // namespace RVX
