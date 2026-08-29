/**
 * @file ReflectionFeatureAdapter.cpp
 * @brief Capability-honest IBL-preserving reflection identity resolve.
 */

#include "Features/ReflectionFeatureAdapter.h"

#include "Render/Passes/RayTracedReflectionCompositePass.h"
#include "Render/Passes/RayTracedReflectionDenoisePass.h"
#include "Render/Passes/RayTracedReflectionPass.h"

#include <algorithm>
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
        return IsPresentableRasterRecipe(recipe);
    }
} // namespace

ReflectionFeatureAdapter::ReflectionFeatureAdapter(
    PipelineCache* pipelineCache) noexcept
{
    m_ssr.SetResources(pipelineCache);
}

void ReflectionFeatureAdapter::SetPipelineCache(
    PipelineCache* pipelineCache) noexcept
{
    m_ssr.SetResources(pipelineCache);
}

bool ReflectionFeatureAdapter::ArmSSRQualificationProbe() noexcept
{
    return m_ssr.ArmQualificationProbe();
}

bool ReflectionFeatureAdapter::HasPendingSSRQualificationProbe() const noexcept
{
    return m_ssr.HasPendingQualificationProbe();
}

std::optional<SSRQualificationProbeIdentity>
ReflectionFeatureAdapter::GetPendingSSRQualificationProbeIdentity() const noexcept
{
    return m_ssr.GetPendingQualificationProbeIdentity();
}

bool ReflectionFeatureAdapter::NotifySSRQualificationProbeSubmission(
    const SSRQualificationProbeIdentity& identity,
    const GPUCompletionToken& completion,
    const RenderSubmissionTracker& tracker) noexcept
{
    return m_ssr.NotifyQualificationProbeSubmission(identity, completion, tracker);
}

bool ReflectionFeatureAdapter::MarkSSRQualificationProbePresented(
    const SSRQualificationProbeIdentity& identity) noexcept
{
    return m_ssr.MarkQualificationProbePresented(identity);
}

bool ReflectionFeatureAdapter::PollSSRQualificationProbe(
    const RenderSubmissionTracker& tracker,
    SSRQualificationProbeStats& outStats) noexcept
{
    return m_ssr.PollQualificationProbe(tracker, outStats);
}

void ReflectionFeatureAdapter::AbandonSSRQualificationProbe() noexcept
{
    m_ssr.AbandonQualificationProbe();
}

void ReflectionFeatureAdapter::SetRecordContext(
    RenderPassRecordContext context)
{
    m_recordContext = std::move(context);
    m_reflectionPlan.reset();
    m_probeTextures = {};
    m_probeReady = {};
    m_hybridRayTracingPlan.reset();
    m_rayTracedReflectionPass = nullptr;
    m_rayTracedReflectionDenoisePass = nullptr;
    m_rayTracedReflectionCompositePass = nullptr;
    m_hasRecordContext = true;
    m_requiresReflectionPlan = false;
    m_preparedContext = {};
    m_lastReceipt = {};
    m_readiness = {};
    m_ssrScheduled = false;
    m_ssrStatsObserved = false;
    m_hasFinalizedSSRComputeStats = false;
    m_finalizedSSRComputeStats = {};
    m_rayTracedReflectionScheduled = false;
    m_outputPublished = false;
}

void ReflectionFeatureAdapter::SetRecordContext(
    ReflectionFeatureRecordContext context)
{
    m_recordContext = std::move(context.pass);
    m_reflectionPlan = std::move(context.plan);
    m_probeTextures = context.probeTextures;
    m_probeReady = context.probeReady;
    m_hybridRayTracingPlan = std::move(context.hybridRayTracingPlan);
    m_rayTracedReflectionPass = context.rayTracedReflectionPass;
    m_rayTracedReflectionDenoisePass =
        context.rayTracedReflectionDenoisePass;
    m_rayTracedReflectionCompositePass =
        context.rayTracedReflectionCompositePass;
    m_hasRecordContext = true;
    m_requiresReflectionPlan = true;
    m_preparedContext = {};
    m_lastReceipt = {};
    m_readiness = {};
    m_ssrScheduled = false;
    m_ssrStatsObserved = false;
    m_hasFinalizedSSRComputeStats = false;
    m_finalizedSSRComputeStats = {};
    m_rayTracedReflectionScheduled = false;
    m_outputPublished = false;
}

RenderFeatureDescriptor ReflectionFeatureAdapter::Describe() const
{
    RenderFeatureDescriptor descriptor;
    descriptor.featureId = BuiltinRenderFeatureIds::ReflectionResolve;
    descriptor.schemaVersion = 1;
    descriptor.name = "ReflectionResolve";
    descriptor.phase = RenderFeaturePhase::Reflection;
    descriptor.consumedResources = {
        {RenderBlackboardResourceId::LightingOutputs,
         LightingOutputs::SchemaVersion}};
    descriptor.producedResources = {
        {RenderBlackboardResourceId::ReflectionOutputs,
         ReflectionOutputs::SchemaVersion}};
    descriptor.requiredCapabilities = ToMask(RenderFeatureCapability::Graphics);
    descriptor.requiredRecipes =
        ToMask(RenderPipelineRecipe::ForwardStandard) |
        ToMask(RenderPipelineRecipe::DeferredQuality) |
        ToMask(RenderPipelineRecipe::HybridRayTracing);
    descriptor.qualityTiers = RVX_ALL_RENDER_QUALITY_TIERS;
    descriptor.fallbackPolicy = RenderFeatureFallbackPolicy::None;
    descriptor.shaderPipelineFamily = "ReflectionResolve";
    // When the current profile does not request SSR and no admitted probe GPU
    // work is needed, the boundary is still a valid typed identity resolve.
    descriptor.allowsDataOnlyCompletion = true;
    return descriptor;
}

void ReflectionFeatureAdapter::Prepare(const RenderFeatureContext& context)
{
    m_preparedContext = context;
    m_readiness = {};
    m_readiness.compiled = true;
    m_readiness.hardwareSupported = HasGraphicsCapability(
        context.hardwareCapabilities);
    m_readiness.implementationSupported =
        IsRasterRecipe(context.pipeline.recipe) &&
        IsRasterRecipe(context.resolvedRecipe) &&
        context.pipeline.recipe == context.resolvedRecipe;
    // SSR and local probes are explicitly optional routes. ReflectionResolve
    // remains required because it preserves the already-lit IBL result when
    // either optional route is unavailable.
    m_readiness.pipelineReady = m_readiness.implementationSupported;
    m_readiness.inputsReady = HasMatchingRecordContext(context) &&
        HasValidLightingPlan(context) && HasValidReflectionPlan(context) &&
        HasValidHybridRayTracingPlan(context);

    if (!m_readiness.hardwareSupported)
    {
        m_readiness.diagnostic = "Graphics capability is unavailable";
    }
    else if (!m_readiness.implementationSupported)
    {
        m_readiness.diagnostic =
            "Reflection identity resolve requires a resolved raster recipe";
    }
    else if (!m_readiness.inputsReady)
    {
        m_readiness.diagnostic = context.resolvedRecipe ==
                RenderPipelineRecipe::HybridRayTracing
            ? "Hybrid reflection resolve requires the exact frame/view reflection and RT plans"
            : "Reflection resolve requires the current frame/view lighting and reflection plans";
    }
    m_readiness.executed = false;
}

RenderFeatureReadiness ReflectionFeatureAdapter::ResolveReadiness() const
{
    return m_readiness;
}

void ReflectionFeatureAdapter::AddPasses(
    RenderFeatureGraphBuilder& builder,
    RenderGraphBlackboard& blackboard)
{
    m_lastReceipt = {};
    m_readiness.executed = false;
    m_ssrScheduled = false;
    m_ssrStatsObserved = false;
    m_hasFinalizedSSRComputeStats = false;
    m_finalizedSSRComputeStats = {};
    m_rayTracedReflectionScheduled = false;
    m_outputPublished = false;
    if (!m_readiness.IsReadyToExecute())
    {
        return;
    }
    if (!builder.IsValid() ||
        !m_recordContext.MatchesTargetGraph(builder.GetGraph()))
    {
        SetCompositionFailure(
            "Reflection identity resolve record context does not match the target graph");
        return;
    }

    RenderGraphBlackboardCode code = RenderGraphBlackboardCode::Success;
    const LightingOutputs* const lighting = blackboard.Require<LightingOutputs>(
        BuiltinRenderFeatureIds::ReflectionResolve, Describe(), &code);
    const uint64 expectedLightingPlanIdentity =
        m_recordContext.lightingPlan != nullptr
            ? m_recordContext.lightingPlan->GetIdentityHash()
            : 0;
    const bool hasAnyReflectionSurfaceData = lighting != nullptr &&
        (lighting->baseIndirectSpecular.IsValid() ||
         lighting->specularResponse.IsValid() ||
         lighting->exposureState.IsValid() ||
         lighting->baseIndirectSpecularMetadata.extent.IsValid() ||
         lighting->specularResponseMetadata.extent.IsValid() ||
         lighting->exposureStateMetadata.IsValid() ||
         lighting->exposureStateContentHash != 0);
    const bool hasCompleteReflectionSurfaceData = lighting != nullptr &&
        lighting->baseIndirectSpecular.IsValid() &&
        lighting->specularResponse.IsValid() &&
        lighting->exposureState.IsValid() &&
        lighting->baseIndirectSpecularMetadata.extent ==
            m_preparedContext.outputExtent &&
        lighting->specularResponseMetadata.extent ==
            m_preparedContext.outputExtent &&
        lighting->exposureStateMetadata.IsValid() &&
        lighting->exposureStateContentHash != 0;
    const bool reflectionSurfaceDataValid =
        !hasAnyReflectionSurfaceData || hasCompleteReflectionSurfaceData;
    const bool reflectionSurfaceDataRequired =
        m_recordContext.requiresReflectionSurfaceData;
    if (lighting == nullptr || code != RenderGraphBlackboardCode::Success ||
        lighting->frameIdentity != m_preparedContext.frameIdentity ||
        lighting->viewIdentity != m_preparedContext.viewIdentity ||
        !lighting->IsCompatibleWithLightingPlan(
            expectedLightingPlanIdentity) ||
        !lighting->hdrSceneColor.IsValid() ||
        lighting->hdrSceneColorMetadata.extent !=
            m_preparedContext.outputExtent ||
        !reflectionSurfaceDataValid ||
        (reflectionSurfaceDataRequired && !hasCompleteReflectionSurfaceData))
    {
        SetCompositionFailure(
            "Reflection identity resolve requires the exact typed LightingOutputs v5");
        return;
    }

    ReflectionOutputs outputs;
    outputs.provenance = builder.GetProvenance();
    outputs.frameIdentity = lighting->frameIdentity;
    outputs.viewIdentity = lighting->viewIdentity;
    outputs.lightingPlanIdentity = lighting->lightingPlanIdentity;
    outputs.resolvedSceneColor = lighting->hdrSceneColor;
    outputs.resolvedSceneColorMetadata = lighting->hdrSceneColorMetadata;
    outputs.worldNormal = lighting->worldNormal;
    outputs.worldNormalMetadata = lighting->worldNormalMetadata;
    outputs.materialIdentity = lighting->materialIdentity;
    outputs.materialIdentityMetadata = lighting->materialIdentityMetadata;
    outputs.materialParameterTable = lighting->materialParameterTable;
    outputs.materialParameterTableMetadata =
        lighting->materialParameterTableMetadata;
    outputs.screenSpaceReflectionsApplied = false;
    outputs.reflectionProbesApplied = false;
    if (m_hybridRayTracingPlan.has_value())
    {
        outputs.hybridRayTracingPlanIdentity =
            m_hybridRayTracingPlan->GetIdentityHash();
    }
    outputs.imageBasedLightingPreserved = true;

    const bool scheduleScreenSpaceReflection =
        !UsesHybridRayTracedReflectionRoute() &&
        (!m_hybridRayTracingPlan.has_value() ||
         UsesHybridScreenSpaceReflectionRoute());
    if (scheduleScreenSpaceReflection && m_reflectionPlan != nullptr &&
        (m_reflectionPlan->GetSSRIdentity().requested ||
         !m_reflectionPlan->GetSelectedProbes().empty()))
    {
        const ReflectionSSRIdentity& ssrIdentity =
            m_reflectionPlan->GetSSRIdentity();
        SSRFrameInputs inputs;
        inputs.frameIdentity = m_preparedContext.frameIdentity;
        inputs.viewIdentity = m_preparedContext.viewIdentity;
        inputs.reflectionPlanIdentity = m_reflectionPlan->GetIdentityHash();
        inputs.lightingPlanIdentity = lighting->lightingPlanIdentity;
        inputs.depthPyramidIdentity =
            ssrIdentity.depthPyramidIdentityHash;
        inputs.screenSpaceRequested = ssrIdentity.requested;
        inputs.sceneColor = lighting->hdrSceneColor;
        inputs.deviceDepth = m_recordContext.view.depthTarget;
        inputs.normalRoughness = lighting->worldNormal;
        // The v5 lighting producer owns the exact pre-exposed IBL baseline,
        // material response, and exposure state. SSR must consume this whole
        // bundle so its replacement route cannot subtract a different
        // environment source or apply response/exposure twice.
        inputs.baseIndirectSpecular = lighting->baseIndirectSpecular;
        inputs.specularResponse = lighting->specularResponse;
        inputs.exposureState = lighting->exposureState;
        inputs.exposureStateContentHash =
            lighting->exposureStateContentHash;

        const std::vector<ReflectionProbeSelection>& probes =
            m_reflectionPlan->GetSelectedProbes();
        for (size_t probeIndex = 0;
             probeIndex < probes.size();
             ++probeIndex)
        {
            const RenderProbeSnapshot& probe = probes[probeIndex].snapshot;
            inputs.probeTextures[probeIndex] =
                m_probeTextures[probeIndex];
            inputs.probeReady[probeIndex] =
                m_probeReady[probeIndex];
            inputs.probePositions[probeIndex] = Vec3(
                probe.worldTransform[3]);
            inputs.probeExtents[probeIndex] = probe.influenceExtent;
            inputs.probeWorldTransforms[probeIndex] = probe.worldTransform;
            inputs.probeInverseWorldTransforms[probeIndex] =
                inverse(probe.worldTransform);
            inputs.probeBoxProjectionSizes[probeIndex] =
                probe.boxProjectionSize;
            inputs.probeBoxProjectionOffsets[probeIndex] =
                probe.boxProjectionOffset;
            const float32 minimumInfluenceExtent = std::min(
                probe.influenceExtent.x,
                std::min(probe.influenceExtent.y, probe.influenceExtent.z));
            inputs.probeNormalizedBlendDistances[probeIndex] =
                probe.blendDistance / minimumInfluenceExtent;
            inputs.probeShapes[probeIndex] =
                probe.shape == RenderProbeShape::Box
                    ? SSRProbeInfluenceShape::Box
                    : SSRProbeInfluenceShape::Sphere;
            inputs.probeUsesBoxProjection[probeIndex] =
                probe.useBoxProjection;
            inputs.probePriorities[probeIndex] = probe.priority;
            inputs.probeWeights[probeIndex] =
                probes[probeIndex].influenceWeight;
        }

        SSROutput ssrOutput;
        const bool graphRegistered = builder.AddGraphCallback(
            "HiZScreenSpaceReflection",
            m_recordContext,
            [this, &ssrOutput, &inputs](RenderGraph& graph)
            {
                ssrOutput = m_ssr.AddToGraph(
                    graph, m_recordContext.view, inputs);
            });
        m_ssrStatsObserved = graphRegistered;
        const bool outputMatchesPlan = graphRegistered &&
            ssrOutput.IsValid() &&
            ssrOutput.reflectionPlanIdentity ==
                m_reflectionPlan->GetIdentityHash() &&
            ssrOutput.lightingPlanIdentity == lighting->lightingPlanIdentity &&
            (!ssrIdentity.requested ||
             ssrOutput.depthPyramidIdentity ==
                 ssrIdentity.depthPyramidIdentityHash);
        if (outputMatchesPlan)
        {
            outputs.resolvedSceneColor = ssrOutput.resolvedSceneColor;
            outputs.rawReflection = ssrOutput.rawReflection;
            outputs.confidence = ssrOutput.confidence;
            // Graph composition only proves that ReflectionOutputs owns these
            // resources.  Route-mask qualification completes after submit and
            // presentation, so it alone may claim pixels were actually routed
            // through SSR or probes.
            outputs.screenSpaceReflectionsApplied = false;
            outputs.reflectionProbesApplied = false;
            m_ssrScheduled = true;
        }
        // A requested but unavailable SSR/probe route falls back to the typed
        // LightingOutputs identity, which already preserves the resolved IBL
        // contribution. It must never invalidate the required Reflection
        // feature merely because an optional enhancement was unavailable.
    }
    if (UsesHybridScreenSpaceReflectionRoute() && !m_ssrScheduled)
    {
        SetCompositionFailure(
            "Hybrid reflection plan selected SSR but no exact SSR route could be registered");
        return;
    }

    if (UsesHybridRayTracedReflectionRoute())
    {
        if (!HasReadyRayTracedReflectionPasses() ||
            !builder.AddLegacyPass(*m_rayTracedReflectionPass,
                                   m_recordContext) ||
            !builder.AddLegacyPass(*m_rayTracedReflectionDenoisePass,
                                   m_recordContext) ||
            !builder.AddLegacyPass(*m_rayTracedReflectionCompositePass,
                                   m_recordContext))
        {
            SetCompositionFailure(
                "Hybrid reflection plan selected RT but the exact reflection pass chain could not be registered");
            return;
        }

        const RGTextureHandle rawReflection =
            m_rayTracedReflectionPass->GetReflectionHandle();
        const RGTextureHandle denoisedReflection =
            m_rayTracedReflectionDenoisePass->GetDenoisedReflectionHandle();
        // Composite writes the canonical LightingOutputs HDR target in place.
        // When the concrete passes declare their graph outputs, retain the
        // producer handles for downstream diagnostics. Exact graph execution
        // still validates the passes themselves, so a test double may model
        // the route without manufacturing private pass-owned resources.
        if (rawReflection.IsValid() && denoisedReflection.IsValid())
        {
            outputs.rawReflection = rawReflection;
            outputs.denoisedReflection = denoisedReflection;
        }
        outputs.rayTracedReflectionsScheduled = true;
        outputs.rayTracedReflectionsApplied = false;
        m_rayTracedReflectionScheduled = true;
    }
    if (blackboard.Publish(BuiltinRenderFeatureIds::ReflectionResolve,
                           Describe(), std::move(outputs)) !=
        RenderGraphBlackboardCode::Success)
    {
        SetCompositionFailure(
            "Reflection identity resolve could not publish ReflectionOutputs v3");
        return;
    }
    m_outputPublished = true;
}

void ReflectionFeatureAdapter::OnGraphExecutionFinalized(
    const RenderFeatureGraphExecutionEvidence& evidence)
{
    const bool accepted = m_outputPublished &&
        evidence.featureId == BuiltinRenderFeatureIds::ReflectionResolve &&
        evidence.frameIdentity == m_preparedContext.frameIdentity &&
        evidence.viewIdentity == m_preparedContext.viewIdentity &&
        evidence.HasExactExecution();
    if (m_ssrScheduled)
    {
        m_ssr.FinalizeGraphExecution(accepted);
    }
    if (m_ssrStatsObserved)
    {
        m_finalizedSSRComputeStats = m_ssr.GetLastComputeStats();
        if (!m_ssrScheduled)
        {
            // The Reflection feature itself did execute and publish the IBL
            // preserving identity result. SSR may still have declined its
            // optional route before scheduling any graph pass.
            m_finalizedSSRComputeStats.executed = false;
            m_finalizedSSRComputeStats.hiZBuilt = false;
            m_finalizedSSRComputeStats.screenSpaceApplied = false;
            m_finalizedSSRComputeStats.probesApplied = false;
            m_finalizedSSRComputeStats.fallbackChainResolved =
                accepted && m_outputPublished;
            if (!accepted &&
                m_finalizedSSRComputeStats.fallbackReason.empty())
            {
                m_finalizedSSRComputeStats.fallbackReason =
                    "Reflection fallback identity did not receive exact graph execution evidence";
            }
        }
        m_hasFinalizedSSRComputeStats = true;
    }
}

RenderFeatureExecutionReceipt ReflectionFeatureAdapter::CollectReceipt() const
{
    RenderFeatureExecutionReceipt receipt;
    receipt.featureId = BuiltinRenderFeatureIds::ReflectionResolve;
    receipt.featureSchemaVersion = 1;
    receipt.phase = RenderFeaturePhase::Reflection;
    receipt.requirement = RenderFeatureRequirement::Required;
    receipt.selected = true;
    receipt.readiness = m_readiness;
    receipt.diagnostic = m_readiness.diagnostic;
    receipt.readiness.executed = false;
    m_lastReceipt = receipt;
    return receipt;
}

bool ReflectionFeatureAdapter::HasMatchingRecordContext(
    const RenderFeatureContext& context) const noexcept
{
    return m_hasRecordContext && context.frameIdentity.IsValid() &&
           context.viewIdentity.IsValid() &&
           m_recordContext.featureFrameIdentity == context.frameIdentity &&
           m_recordContext.featureViewIdentity == context.viewIdentity &&
           m_recordContext.IsFrameIdentityValid() &&
           m_recordContext.identity.frameSequence ==
               context.frameIdentity.frameSequence &&
           m_recordContext.identity.viewOrdinal ==
               context.recordingViewOrdinal &&
           m_recordContext.identity.recordEpoch == context.recordEpoch;
}

bool ReflectionFeatureAdapter::HasValidLightingPlan(
    const RenderFeatureContext& context) const noexcept
{
    return m_hasRecordContext && m_recordContext.lightingPlan != nullptr &&
           m_recordContext.lightingPlan->IsValid() &&
           m_recordContext.lightingPlan->GetIdentityHash() != 0 &&
           m_recordContext.lightingPlan->GetFrameIdentity() ==
               context.frameIdentity &&
           m_recordContext.lightingPlan->GetViewIdentity() ==
               context.viewIdentity;
}

bool ReflectionFeatureAdapter::HasValidReflectionPlan(
    const RenderFeatureContext& context) const noexcept
{
    // Focused blackboard tests deliberately exercise the data-only boundary
    // without a renderer-owned probe database.
    if (m_reflectionPlan == nullptr)
    {
        return !m_requiresReflectionPlan;
    }
    if (!m_reflectionPlan->IsValid() ||
        m_reflectionPlan->GetFrameIdentity() != context.frameIdentity ||
        m_reflectionPlan->GetViewIdentity() != context.viewIdentity ||
        m_recordContext.lightingPlan == nullptr ||
        m_reflectionPlan->GetIBLIdentity().lightingPlanIdentityHash !=
            m_recordContext.lightingPlan->GetIdentityHash())
    {
        return false;
    }
    if (m_reflectionPlan->GetSelectedProbes().size() > m_probeReady.size())
    {
        return false;
    }
    // Probe residency and SSR input readiness are optional-route state. The
    // final IBL identity remains the required contract when either is absent.
    return true;
}

bool ReflectionFeatureAdapter::HasValidHybridRayTracingPlan(
    const RenderFeatureContext& context) const noexcept
{
    if (context.resolvedRecipe != RenderPipelineRecipe::HybridRayTracing)
    {
        // A non-hybrid recipe must not accidentally inherit an RT route from
        // a previous record context.
        return !m_hybridRayTracingPlan.has_value();
    }

    if (!m_hybridRayTracingPlan.has_value())
    {
        return false;
    }

    const HybridRayTracingFramePlan& plan = *m_hybridRayTracingPlan;
    if (!plan.IsValid() || plan.GetRecipe() != context.resolvedRecipe ||
        plan.GetFrameIdentity() != context.frameIdentity ||
        plan.GetViewIdentity() != context.viewIdentity ||
        plan.GetIdentityHash() == 0)
    {
        return false;
    }

    if (plan.GetReflectionRoute() ==
        HybridRayTracingReflectionRoute::RayTraced)
    {
        return HasReadyRayTracedReflectionPasses();
    }
    return true;
}

bool ReflectionFeatureAdapter::HasReadyRayTracedReflectionPasses() const
    noexcept
{
    return m_rayTracedReflectionPass != nullptr &&
           m_rayTracedReflectionDenoisePass != nullptr &&
           m_rayTracedReflectionCompositePass != nullptr &&
           m_rayTracedReflectionPass->IsEnabled() &&
           m_rayTracedReflectionDenoisePass->IsEnabled() &&
           m_rayTracedReflectionCompositePass->IsEnabled();
}

bool ReflectionFeatureAdapter::UsesHybridRayTracedReflectionRoute() const
    noexcept
{
    return m_preparedContext.resolvedRecipe ==
               RenderPipelineRecipe::HybridRayTracing &&
           m_hybridRayTracingPlan.has_value() &&
           m_hybridRayTracingPlan->GetReflectionRoute() ==
               HybridRayTracingReflectionRoute::RayTraced;
}

bool ReflectionFeatureAdapter::UsesHybridScreenSpaceReflectionRoute() const
    noexcept
{
    return m_preparedContext.resolvedRecipe ==
               RenderPipelineRecipe::HybridRayTracing &&
           m_hybridRayTracingPlan.has_value() &&
           m_hybridRayTracingPlan->GetReflectionRoute() ==
               HybridRayTracingReflectionRoute::ScreenSpaceReflection;
}

void ReflectionFeatureAdapter::SetCompositionFailure(std::string diagnostic)
{
    m_readiness.inputsReady = false;
    m_readiness.executed = false;
    m_readiness.diagnostic = std::move(diagnostic);
}
} // namespace RVX
