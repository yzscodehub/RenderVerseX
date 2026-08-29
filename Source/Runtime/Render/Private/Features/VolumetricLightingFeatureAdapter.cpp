/**
 * @file VolumetricLightingFeatureAdapter.cpp
 * @brief Typed composition and presentation-gated history for froxel lighting.
 */

#include "Features/VolumetricLightingFeatureAdapter.h"

#include "Core/Diagnostics/ContentHash.h"

#include <utility>

namespace RVX
{
namespace
{
    [[nodiscard]] bool HasCapability(
        RenderFeatureCapabilityMask capabilities,
        RenderFeatureCapability capability) noexcept
    {
        return (capabilities & ToMask(capability)) != 0;
    }

    [[nodiscard]] uint64 BuildDirectionalShadowIdentity(
        const DirectionalShadowRecordOutput& output) noexcept
    {
        if (!output.enabled || !output.shadowMap.IsValid())
        {
            return 0;
        }
        uint64 hash = Diagnostics::RVX_DIAGNOSTICS_FNV1A64_OFFSET_BASIS;
        Diagnostics::MixContentHashString(
            hash, "RVX.VolumetricDirectionalShadow.v1");
        Diagnostics::MixContentHashValue(hash, output.lightingPlanIdentity);
        Diagnostics::MixContentHashValue(hash, output.shadowMap.index);
        Diagnostics::MixContentHashValue(
            hash, output.shadowMap.recordingGeneration);
        Diagnostics::MixContentHashValue(hash, output.shadowMapSize);
        Diagnostics::MixContentHashValue(
            hash, static_cast<uint64>(output.cascadeViewProjections.size()));
        return hash == 0 ? 1 : hash;
    }

    [[nodiscard]] uint64 BuildLocalShadowIdentity(
        const LocalShadowOutputs& output) noexcept
    {
        if (!output.requested || !output.depthAtlas.IsValid())
        {
            return 0;
        }
        uint64 hash = Diagnostics::RVX_DIAGNOSTICS_FNV1A64_OFFSET_BASIS;
        Diagnostics::MixContentHashString(
            hash, "RVX.VolumetricLocalShadow.v1");
        Diagnostics::MixContentHashValue(hash, output.lightingPlanIdentity);
        Diagnostics::MixContentHashValue(hash, output.localShadowPlanIdentity);
        Diagnostics::MixContentHashValue(hash, output.atlasPlanIdentity);
        Diagnostics::MixContentHashValue(hash, output.atlasGeneration);
        Diagnostics::MixContentHashValue(hash, output.depthAtlas.index);
        return hash == 0 ? 1 : hash;
    }
} // namespace

VolumetricLightingFeatureAdapter::VolumetricLightingFeatureAdapter(
    PipelineCache* pipelineCache) noexcept
    : m_pipelineCache(pipelineCache)
{
    m_froxelLighting.SetPipelineCache(pipelineCache);
}

void VolumetricLightingFeatureAdapter::SetPipelineCache(
    PipelineCache* pipelineCache) noexcept
{
    m_pipelineCache = pipelineCache;
    m_froxelLighting.SetPipelineCache(pipelineCache);
}

void VolumetricLightingFeatureAdapter::SetRecordContext(
    VolumetricLightingFeatureRecordContext context)
{
    m_recordContext = std::move(context);
    m_preparedContext = {};
    m_composedProvenance = {};
    m_lastReceipt = {};
    m_readiness = {};
    m_pendingHistoryCandidate = {};
    m_pendingPlanIdentity = 0;
    m_hasRecordContext = true;
    m_outputPublished = false;
    m_identityRoute = true;
}

RenderFeatureDescriptor VolumetricLightingFeatureAdapter::Describe() const
{
    RenderFeatureDescriptor descriptor;
    descriptor.featureId = BuiltinRenderFeatureIds::VolumetricLighting;
    descriptor.schemaVersion = 1;
    descriptor.name = "VolumetricLighting";
    descriptor.phase = RenderFeaturePhase::Sky;
    descriptor.consumedResources = {
        {RenderBlackboardResourceId::ReflectionOutputs,
         ReflectionOutputs::SchemaVersion},
        {RenderBlackboardResourceId::LocalShadowOutputs,
         LocalShadowOutputs::SchemaVersion}};
    descriptor.producedResources = {
        {RenderBlackboardResourceId::VolumetricLightingOutputs,
         VolumetricLightingOutputs::SchemaVersion}};
    // Compute is required only for a non-identity request.  Keeping it out
    // of the static descriptor lets the registry select the exact data-only
    // identity route on compatibility hardware where compute is absent.
    // Prepare() applies the strict compute requirement before any enabled
    // path can publish a graph resource.
    descriptor.requiredCapabilities =
        ToMask(RenderFeatureCapability::Graphics) |
        ToMask(RenderFeatureCapability::DescriptorBindings);
    descriptor.requiredRecipes =
        ToMask(RenderPipelineRecipe::ForwardStandard) |
        ToMask(RenderPipelineRecipe::DeferredQuality) |
        ToMask(RenderPipelineRecipe::HybridRayTracing);
    descriptor.qualityTiers = RVX_ALL_RENDER_QUALITY_TIERS;
    descriptor.fallbackPolicy = RenderFeatureFallbackPolicy::None;
    descriptor.shaderPipelineFamily = "FroxelVolumetricLighting";
    descriptor.allowsDataOnlyCompletion = true;
    return descriptor;
}

void VolumetricLightingFeatureAdapter::Prepare(
    const RenderFeatureContext& context)
{
    m_preparedContext = context;
    m_readiness = {};
    m_readiness.compiled = true;
    const bool hasGraphicsAndDescriptors =
        HasCapability(context.hardwareCapabilities,
                      RenderFeatureCapability::Graphics) &&
        HasCapability(context.hardwareCapabilities,
                      RenderFeatureCapability::DescriptorBindings);
    const bool hasCompute = HasCapability(
        context.hardwareCapabilities, RenderFeatureCapability::Compute);
    const bool identityRequested = !m_recordContext.settings.enabled ||
        m_recordContext.settings.density == 0.0f;
    m_readiness.hardwareSupported = hasGraphicsAndDescriptors &&
        (identityRequested || hasCompute);
    m_readiness.implementationSupported =
        IsPresentableRasterRecipe(context.resolvedRecipe) &&
        context.pipeline.recipe == context.resolvedRecipe;
    m_readiness.pipelineReady = identityRequested ||
        (m_pipelineCache != nullptr && m_pipelineCache->IsInitialized());
    m_readiness.inputsReady = HasMatchingRecordContext(context);
    if (!m_readiness.hardwareSupported)
    {
        m_readiness.diagnostic = identityRequested
            ? "Froxel volumetric identity route requires graphics and descriptor bindings"
            : "Froxel volumetric lighting requires graphics, compute, and descriptor bindings";
    }
    else if (!m_readiness.implementationSupported)
    {
        m_readiness.diagnostic =
            "Froxel volumetric lighting requires the selected presentable raster recipe";
    }
    else if (!m_readiness.pipelineReady)
    {
        m_readiness.diagnostic =
            "Froxel volumetric lighting has no renderer pipeline cache";
    }
    else if (!m_readiness.inputsReady)
    {
        m_readiness.diagnostic =
            "Froxel volumetric lighting record context is stale or incomplete";
    }
    m_readiness.executed = false;
}

RenderFeatureReadiness
VolumetricLightingFeatureAdapter::ResolveReadiness() const
{
    return m_readiness;
}

void VolumetricLightingFeatureAdapter::AddPasses(
    RenderFeatureGraphBuilder& builder,
    RenderGraphBlackboard& blackboard)
{
    m_lastReceipt = {};
    m_outputPublished = false;
    m_pendingHistoryCandidate = {};
    m_pendingPlanIdentity = 0;
    m_readiness.executed = false;
    if (!m_readiness.IsReadyToExecute())
    {
        return;
    }
    if (!builder.IsValid() ||
        !m_recordContext.passRecordContext.MatchesTargetGraph(
            builder.GetGraph()))
    {
        SetCompositionFailure(
            "Froxel volumetric lighting does not match the target graph");
        return;
    }

    RenderGraphBlackboardCode code = RenderGraphBlackboardCode::Success;
    const ReflectionOutputs* const reflection =
        blackboard.Require<ReflectionOutputs>(
            BuiltinRenderFeatureIds::VolumetricLighting, Describe(), &code);
    if (reflection == nullptr || code != RenderGraphBlackboardCode::Success)
    {
        SetCompositionFailure(
            "Froxel volumetric lighting requires exact ReflectionOutputs v3");
        return;
    }
    const LocalShadowOutputs* const localShadows =
        blackboard.Require<LocalShadowOutputs>(
            BuiltinRenderFeatureIds::VolumetricLighting, Describe(), &code);
    if (localShadows == nullptr ||
        code != RenderGraphBlackboardCode::Success)
    {
        SetCompositionFailure(
            "Froxel volumetric lighting requires exact LocalShadowOutputs v1");
        return;
    }

    VolumetricLightingFramePlan plan;
    FroxelVolumetricLightingInputs inputs;
    std::string diagnostic;
    if (!BuildPlanAndInputs(
            *reflection, *localShadows, plan, inputs, diagnostic))
    {
        SetCompositionFailure(std::move(diagnostic));
        return;
    }

    const FroxelVolumetricLightingOutput gpuOutput =
        m_froxelLighting.AddToGraph(
            builder, m_recordContext.passRecordContext.view, plan, inputs);
    const bool validIdentityOutput =
        plan.IsIdentityOutput() && gpuOutput.IsIdentityOutput();
    const bool validGpuOutput = !plan.IsIdentityOutput() &&
        gpuOutput.sourceSceneColor.IsValid() &&
        gpuOutput.resolvedSceneColor.IsValid() &&
        gpuOutput.integratedScatteringTransmittance.IsValid() &&
        gpuOutput.volumetricPlanIdentity == plan.GetIdentityHash() &&
        gpuOutput.lightingPlanIdentity == plan.GetLightingPlanIdentity() &&
        gpuOutput.froxelGrid == plan.GetFroxelGrid() &&
        gpuOutput.historyCandidate == m_recordContext.historyCandidate;
    if (!validIdentityOutput && !validGpuOutput)
    {
        const std::string& unsupported =
            m_froxelLighting.GetUnsupportedReason();
        SetCompositionFailure(
            unsupported.empty()
                ? "Froxel volumetric lighting failed to register its exact GPU chain"
                : unsupported);
        return;
    }

    VolumetricLightingOutputs outputs;
    outputs.provenance = builder.GetProvenance();
    outputs.frameIdentity = reflection->frameIdentity;
    outputs.viewIdentity = reflection->viewIdentity;
    outputs.historyIdentity = m_recordContext.historyCandidate;
    outputs.historyResetReasons = m_recordContext.historyResetReasons;
    outputs.lightingPlanIdentity = reflection->lightingPlanIdentity;
    outputs.volumetricPlanIdentity = plan.GetIdentityHash();
    outputs.settingsHash = plan.GetSettingsHash();
    outputs.froxelGrid = plan.GetFroxelGrid();
    outputs.sourceSceneColor = reflection->resolvedSceneColor;
    outputs.sourceSceneColorMetadata =
        reflection->resolvedSceneColorMetadata;
    outputs.resolvedSceneColor = gpuOutput.resolvedSceneColor;
    outputs.resolvedSceneColorMetadata =
        reflection->resolvedSceneColorMetadata;
    outputs.integratedScatteringTransmittance =
        gpuOutput.integratedScatteringTransmittance;
    if (!plan.IsIdentityOutput())
    {
        outputs.integratedScatteringTransmittanceMetadata.dimension =
            RHITextureDimension::Texture3D;
        outputs.integratedScatteringTransmittanceMetadata.format =
            RHIFormat::RGBA16_FLOAT;
        outputs.integratedScatteringTransmittanceMetadata.sampleCount =
            RHISampleCount::Count1;
        outputs.integratedScatteringTransmittanceMetadata.width =
            plan.GetFroxelGrid().froxelWidth;
        outputs.integratedScatteringTransmittanceMetadata.height =
            plan.GetFroxelGrid().froxelHeight;
        outputs.integratedScatteringTransmittanceMetadata.depth =
            plan.GetFroxelGrid().depthSliceCount;
    }
    outputs.worldNormal = reflection->worldNormal;
    outputs.worldNormalMetadata = reflection->worldNormalMetadata;
    outputs.materialIdentity = reflection->materialIdentity;
    outputs.materialIdentityMetadata =
        reflection->materialIdentityMetadata;
    outputs.materialParameterTable = reflection->materialParameterTable;
    outputs.materialParameterTableMetadata =
        reflection->materialParameterTableMetadata;
    outputs.identityRoute = plan.IsIdentityOutput();
    outputs.temporalHistoryUsed = plan.UsesTemporalHistory();

    const RenderGraphBlackboardCode publishCode = blackboard.Publish(
        BuiltinRenderFeatureIds::VolumetricLighting,
        Describe(),
        std::move(outputs));
    if (publishCode != RenderGraphBlackboardCode::Success)
    {
        SetCompositionFailure(
            "Froxel volumetric lighting could not publish VolumetricLightingOutputs v1");
        return;
    }

    m_composedProvenance = builder.GetProvenance();
    m_pendingHistoryCandidate = m_recordContext.historyCandidate;
    m_pendingPlanIdentity = plan.GetIdentityHash();
    m_identityRoute = plan.IsIdentityOutput();
    m_outputPublished = true;
    m_readiness.executed = m_identityRoute;
}

void VolumetricLightingFeatureAdapter::OnGraphExecutionFinalized(
    const RenderFeatureGraphExecutionEvidence& evidence)
{
    const bool evidenceMatches = HasMatchingFinalizedEvidence(evidence);
    if (m_identityRoute)
    {
        m_readiness.executed = evidenceMatches &&
            evidence.semanticExecutionSucceeded &&
            evidence.contractCompletedWithoutGraphWork;
        return;
    }
    const bool executionAccepted = evidenceMatches && evidence.HasExactExecution();
    m_froxelLighting.FinalizeGraphExecution(executionAccepted);
    m_readiness.executed = executionAccepted;
    if (!executionAccepted && evidenceMatches && !evidence.diagnostic.empty())
    {
        m_readiness.diagnostic = evidence.diagnostic;
    }
}

RenderFeatureExecutionReceipt
VolumetricLightingFeatureAdapter::CollectReceipt() const
{
    RenderFeatureExecutionReceipt receipt;
    receipt.featureId = BuiltinRenderFeatureIds::VolumetricLighting;
    receipt.featureSchemaVersion = 1;
    receipt.phase = RenderFeaturePhase::Sky;
    receipt.requirement = RenderFeatureRequirement::Required;
    receipt.selected = true;
    receipt.readiness = m_readiness;
    receipt.diagnostic = m_readiness.diagnostic;
    receipt.readiness.executed = false;
    m_lastReceipt = receipt;
    return receipt;
}

bool VolumetricLightingFeatureAdapter::RequiresHistoryCommit() const noexcept
{
    const FroxelVolumetricLightingStats& stats =
        m_froxelLighting.GetLastStats();
    return !m_identityRoute && stats.historyCandidatePrepared &&
        !stats.historyCommitted && !stats.historyAbandoned &&
        m_pendingPlanIdentity != 0 && m_pendingHistoryCandidate.IsValid();
}

bool VolumetricLightingFeatureAdapter::NotifyHistorySubmission(
    const RenderViewHistoryIdentity& identity) noexcept
{
    return !RequiresHistoryCommit() ||
        (identity == m_pendingHistoryCandidate &&
         m_froxelLighting.NotifyHistorySubmission(
             identity, m_pendingPlanIdentity));
}

bool VolumetricLightingFeatureAdapter::CanCommitHistoryAfterPresent(
    const RenderViewHistoryIdentity& identity) const noexcept
{
    return !RequiresHistoryCommit() ||
        (identity == m_pendingHistoryCandidate &&
         m_froxelLighting.CanCommitHistoryAfterPresent(
             identity, m_pendingPlanIdentity));
}

bool VolumetricLightingFeatureAdapter::CommitHistoryAfterPresent(
    const RenderViewHistoryIdentity& identity) noexcept
{
    if (!RequiresHistoryCommit())
    {
        return true;
    }
    const bool committed = identity == m_pendingHistoryCandidate &&
        m_froxelLighting.CommitHistoryAfterPresent(
            identity, m_pendingPlanIdentity);
    if (committed)
    {
        m_pendingHistoryCandidate = {};
        m_pendingPlanIdentity = 0;
    }
    return committed;
}

void VolumetricLightingFeatureAdapter::AbandonHistoryCandidate() noexcept
{
    if (m_pendingHistoryCandidate.IsValid() && m_pendingPlanIdentity != 0)
    {
        static_cast<void>(m_froxelLighting.AbandonPendingHistory(
            m_pendingHistoryCandidate, m_pendingPlanIdentity));
    }
    else
    {
        m_froxelLighting.FinalizeGraphExecution(false);
    }
    m_pendingHistoryCandidate = {};
    m_pendingPlanIdentity = 0;
}

void VolumetricLightingFeatureAdapter::ResetHistory() noexcept
{
    m_froxelLighting.ResetHistory();
    m_pendingHistoryCandidate = {};
    m_pendingPlanIdentity = 0;
}

void VolumetricLightingFeatureAdapter::Shutdown() noexcept
{
    m_froxelLighting.Shutdown();
    m_pendingHistoryCandidate = {};
    m_pendingPlanIdentity = 0;
}

bool VolumetricLightingFeatureAdapter::HasMatchingRecordContext(
    const RenderFeatureContext& context) const noexcept
{
    const RenderPassRecordContext& pass = m_recordContext.passRecordContext;
    return m_hasRecordContext && pass.IsFrameIdentityValid() &&
        pass.featureFrameIdentity == context.frameIdentity &&
        pass.featureViewIdentity == context.viewIdentity &&
        pass.identity.viewOrdinal == context.recordingViewOrdinal &&
        pass.identity.recordEpoch == context.recordEpoch &&
        pass.lightingPlan != nullptr && pass.lightingPlan->IsValid() &&
        pass.lightingPlan->GetIdentityHash() != 0 &&
        pass.lightingPlan->GetFrameIdentity() == context.frameIdentity &&
        pass.lightingPlan->GetViewIdentity() == context.viewIdentity &&
        pass.lightingResources != nullptr &&
        pass.lightingResources->IsComplete() &&
        m_recordContext.historyCandidate.IsValid() &&
        m_recordContext.historyCandidate.previousFrame == context.frameIdentity &&
        m_recordContext.historyCandidate.view == context.viewIdentity &&
        m_recordContext.historyCandidate.extent == context.outputExtent;
}

bool VolumetricLightingFeatureAdapter::HasMatchingFinalizedEvidence(
    const RenderFeatureGraphExecutionEvidence& evidence) const noexcept
{
    return evidence.featureId == BuiltinRenderFeatureIds::VolumetricLighting &&
        evidence.graphIdentity == m_composedProvenance.graphIdentity &&
        evidence.recordingGeneration ==
            m_composedProvenance.recordingGeneration &&
        evidence.frameIdentity == m_preparedContext.frameIdentity &&
        evidence.viewIdentity == m_preparedContext.viewIdentity &&
        m_outputPublished;
}

bool VolumetricLightingFeatureAdapter::BuildPlanAndInputs(
    const ReflectionOutputs& reflection,
    const LocalShadowOutputs& localShadows,
    VolumetricLightingFramePlan& outPlan,
    FroxelVolumetricLightingInputs& outInputs,
    std::string& outDiagnostic) const
{
    outDiagnostic.clear();
    const RenderPassRecordContext& pass = m_recordContext.passRecordContext;
    const uint64 lightingPlanIdentity = pass.lightingPlan != nullptr
        ? pass.lightingPlan->GetIdentityHash()
        : 0;
    if (reflection.frameIdentity != m_preparedContext.frameIdentity ||
        reflection.viewIdentity != m_preparedContext.viewIdentity ||
        reflection.lightingPlanIdentity != lightingPlanIdentity ||
        localShadows.frameIdentity != m_preparedContext.frameIdentity ||
        localShadows.viewIdentity != m_preparedContext.viewIdentity ||
        localShadows.lightingPlanIdentity != lightingPlanIdentity ||
        !reflection.resolvedSceneColor.IsValid() ||
        !pass.view.depthTarget.IsValid() || pass.results == nullptr ||
        pass.results->identity != pass.identity)
    {
        outDiagnostic =
            "Froxel volumetric lighting received stale reflection, shadow, or depth inputs";
        return false;
    }

    const DirectionalShadowRecordOutput& directional =
        pass.results->directionalShadowOutput;
    if (!directional.IsCompatibleWith(pass.identity, lightingPlanIdentity))
    {
        outDiagnostic =
            "Froxel volumetric lighting directional-shadow output is incompatible";
        return false;
    }

    VolumetricLightingFramePlanRequest request;
    request.frameIdentity = m_preparedContext.frameIdentity;
    request.viewIdentity = m_preparedContext.viewIdentity;
    request.outputExtent = m_preparedContext.outputExtent;
    request.quality = m_preparedContext.resolvedQuality;
    request.settings = m_recordContext.settings;
    request.expectedLightingPlanIdentity = lightingPlanIdentity;
    // The ViewHistoryCoordinator proves that a prior *frame* presented.  It
    // does not prove this feature has ever produced or committed its own
    // Texture3D.  Reuse is legal only at the intersection of those facts and
    // must name the exact froxel history identity.  This makes a later
    // disabled -> enabled transition initialize history instead of sampling a
    // nonexistent allocation.
    const bool hasCompatibleFroxelHistory =
        m_recordContext.historySource.IsValid() &&
        m_froxelLighting.HasCompatibleCommittedHistory(
            m_recordContext.historySource);
    const bool canReuseTemporalHistory =
        request.settings.enableTemporalReprojection &&
        m_recordContext.historyAvailable && hasCompatibleFroxelHistory &&
        m_recordContext.historyResetReasons == RenderHistoryResetReason::None;
    if (canReuseTemporalHistory)
    {
        request.historyIdentity = m_recordContext.historySource;
        request.historyResetReasons = RenderHistoryResetReason::None;
    }
    else if (request.settings.enableTemporalReprojection)
    {
        request.historyResetReasons =
            m_recordContext.historyResetReasons == RenderHistoryResetReason::None
                ? RenderHistoryResetReason::HistoryUnavailable
                : m_recordContext.historyResetReasons;
    }
    else
    {
        request.historyResetReasons = RenderHistoryResetReason::None;
    }
    request.directionalShadowReady = directional.enabled;
    request.directionalShadowIdentityHash =
        BuildDirectionalShadowIdentity(directional);
    request.localShadowReady = localShadows.requested;
    request.localShadowIdentityHash = BuildLocalShadowIdentity(localShadows);

    const VolumetricLightingFramePlanBuildResult built =
        VolumetricLightingFramePlanBuilder{}.Build(
            *pass.lightingPlan, request);
    if (!built.Succeeded())
    {
        outDiagnostic = built.diagnostic.empty()
            ? GetVolumetricLightingFramePlanCodeName(built.code)
            : built.diagnostic;
        return false;
    }
    outPlan = built.plan;

    const RenderLightingFrameResourceSnapshot& resources =
        *pass.lightingResources;
    outInputs.sourceSceneColor = reflection.resolvedSceneColor;
    outInputs.deviceDepth = pass.view.depthTarget;
    outInputs.lightingPlanIdentity = lightingPlanIdentity;
    outInputs.frozenLightResources = resources.frozenLightResources;
    outInputs.lightConstants = resources.lightConstants;
    outInputs.pointLights = resources.pointLights;
    outInputs.spotLights = resources.spotLights;
    outInputs.clusterConstants = resources.clusterConstants;
    outInputs.clusterData = resources.clusterData;
    outInputs.clusterLightIndices = resources.clusterLightIndices;
    outInputs.directionalShadowEnabled = directional.enabled;
    outInputs.directionalShadowMap = directional.shadowMap;
    outInputs.localShadowEnabled = localShadows.requested;
    outInputs.localShadowAtlas = localShadows.depthAtlas;
    outInputs.historyCandidate = m_recordContext.historyCandidate;
    outInputs.historyAvailable = built.plan.UsesTemporalHistory();
    if (!outInputs.HasCompleteLightingResources())
    {
        outDiagnostic =
            "Froxel volumetric lighting graph resources are incomplete";
        return false;
    }
    return true;
}

void VolumetricLightingFeatureAdapter::SetCompositionFailure(
    std::string diagnostic)
{
    m_readiness.inputsReady = false;
    m_readiness.executed = false;
    m_readiness.diagnostic = std::move(diagnostic);
    m_froxelLighting.FinalizeGraphExecution(false);
}
} // namespace RVX
