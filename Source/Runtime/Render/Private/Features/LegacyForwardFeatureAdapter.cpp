#include "Features/LegacyForwardFeatureAdapter.h"

#include "Render/Passes/OpaquePass.h"
#include "Renderer/RenderPassRegistry.h"

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

    [[nodiscard]] bool IsForwardStandard(
        const RenderFeatureContext& context) noexcept
    {
        return context.pipeline.recipe == RenderPipelineRecipe::ForwardStandard &&
               context.resolvedRecipe == RenderPipelineRecipe::ForwardStandard;
    }

    [[nodiscard]] SceneTextures::Metadata MakeMetadata(
        RenderOutputExtent extent,
        SceneTextures::FormatContract contract) noexcept
    {
        return SceneTextures::Metadata{
            extent, contract.format, contract.sampleCount};
    }

    [[nodiscard]] SceneTextures::ExposureStateMetadata
    MakeExposureStateMetadata(const ViewData& view) noexcept
    {
        return SceneTextures::ExposureStateMetadata{
            view.exposureStateSchemaVersion,
            view.exposureStateStride,
            view.exposureStateMode,
            view.exposureStateConfigurationHash};
    }
} // namespace

LegacyForwardFeatureAdapter::LegacyForwardFeatureAdapter(
    RenderPassRegistry* passRegistry,
    OpaquePass* opaquePass) noexcept
    : m_passRegistry(passRegistry)
    , m_opaquePass(opaquePass)
{
}

void LegacyForwardFeatureAdapter::SetRecordContext(
    RenderPassRecordContext context)
{
    m_recordContext = std::move(context);
    m_hasRecordContext = true;
    m_preparedContext = {};
    m_readiness = {};
    m_lastPassChainStats = {};
    m_lastReceipt = {};
}

RenderFeatureDescriptor LegacyForwardFeatureAdapter::Describe() const
{
    RenderFeatureDescriptor descriptor;
    descriptor.featureId = BuiltinRenderFeatureIds::LegacyForwardMainChain;
    descriptor.schemaVersion = 1;
    descriptor.name = "LegacyForwardMainChain";
    descriptor.phase = RenderFeaturePhase::Geometry;
    descriptor.producedResources = {
        {RenderBlackboardResourceId::TranslucencyOutputs,
         TranslucencyOutputs::SchemaVersion}};
    descriptor.requiredCapabilities = ToMask(RenderFeatureCapability::Graphics);
    descriptor.requiredRecipes = ToMask(RenderPipelineRecipe::ForwardStandard);
    descriptor.qualityTiers = RVX_ALL_RENDER_QUALITY_TIERS;
    descriptor.fallbackPolicy = RenderFeatureFallbackPolicy::None;
    descriptor.shaderPipelineFamily = "ForwardStandard";
    return descriptor;
}

void LegacyForwardFeatureAdapter::Prepare(const RenderFeatureContext& context)
{
    m_preparedContext = context;
    m_readiness = {};
    m_readiness.compiled = true;
    m_readiness.hardwareSupported = HasGraphicsCapability(
        context.hardwareCapabilities);
    m_readiness.implementationSupported = IsForwardStandard(context);
    m_readiness.pipelineReady = m_readiness.implementationSupported;
    m_readiness.inputsReady = HasMatchingRecordContext(context);

    if (m_passRegistry == nullptr)
    {
        m_readiness.inputsReady = false;
        m_readiness.diagnostic = "Legacy forward pass registry is unavailable";
    }
    else if (!m_readiness.hardwareSupported)
    {
        m_readiness.diagnostic = "Graphics capability is unavailable";
    }
    else if (!m_readiness.implementationSupported)
    {
        m_readiness.diagnostic =
            "Legacy forward feature only implements ForwardStandard";
    }
    else if (!m_readiness.inputsReady)
    {
        m_readiness.diagnostic =
            "Legacy forward record context is absent, stale, or mismatched";
    }

    // Individual legacy passes such as ray tracing and particles are optional
    // compatibility leaves. Their support never changes this feature's own
    // readiness; AddPasses records their skip evidence separately.
    m_readiness.executed = false;
}

RenderFeatureReadiness LegacyForwardFeatureAdapter::ResolveReadiness() const
{
    return m_readiness;
}

void LegacyForwardFeatureAdapter::AddPasses(
    RenderFeatureGraphBuilder& builder,
    RenderGraphBlackboard& blackboard)
{
    m_lastPassChainStats = {};
    m_lastReceipt = {};
    m_readiness.executed = false;

    if (!m_readiness.IsReadyToExecute())
    {
        return;
    }
    if (!builder.IsValid() ||
        !m_recordContext.MatchesTargetGraph(builder.GetGraph()))
    {
        SetCompositionFailure(
            "Legacy forward record context does not match the target graph");
        return;
    }
    if (!m_recordContext.view.exposureState.IsValid() ||
        !MakeExposureStateMetadata(m_recordContext.view).IsValid())
    {
        SetCompositionFailure(
            "ForwardStandard requires a valid committed exposure-state contract");
        return;
    }

    m_lastPassChainStats.frameSequence = m_recordContext.identity.frameSequence;
    m_lastPassChainStats.passStatuses = m_passRegistry->GetPassStatuses();
    m_lastPassChainStats.registeredPassCount =
        m_lastPassChainStats.passStatuses.size();

    for (const std::unique_ptr<IRenderPass>& pass : m_passRegistry->GetPasses())
    {
        if (!pass)
        {
            continue;
        }

        const RenderPassStatus status = pass->GetStatus();
        if (!status.requestedEnabled)
        {
            ++m_lastPassChainStats.skippedDisabledPassCount;
            continue;
        }
        if (!status.supported)
        {
            ++m_lastPassChainStats.skippedUnsupportedPassCount;
            m_lastPassChainStats.unsupportedPassNames.push_back(status.name);
            continue;
        }
        if (!status.enabled)
        {
            ++m_lastPassChainStats.skippedDisabledPassCount;
            continue;
        }

        // Each legacy registration receives an independent value copy. Every
        // consumer at or after the opaque phase must see only graph-owned
        // producer results from this exact graph recording. Restricting this
        // refresh to OpaquePass left TransparentPass bound to the pre-graph
        // placeholder shadow outputs even though both paths share one lighting
        // plan.
        RenderPassRecordContext passContext = m_recordContext;
        const int32 firstLightingConsumerPriority = m_opaquePass != nullptr
            ? m_opaquePass->GetPriority()
            : PassPriority::Opaque;
        if (pass->GetPriority() >= firstLightingConsumerPriority)
        {
            if (passContext.results == nullptr)
            {
                SetCompositionFailure(
                    "Main-scene consumer requires graph-owned record results");
                return;
            }

            passContext.directionalShadow =
                passContext.results->directionalShadowOutput;
            if (passContext.directionalShadow.identity !=
                passContext.identity)
            {
                passContext.directionalShadow = {};
                passContext.directionalShadow.identity =
                    passContext.identity;
            }

            passContext.rayTracedShadow =
                passContext.results->rayTracedShadowOutput;
            if (passContext.rayTracedShadow.identity !=
                passContext.identity)
            {
                passContext.rayTracedShadow = {};
                passContext.rayTracedShadow.identity =
                    passContext.identity;
            }
        }

        if (!builder.AddLegacyPass(*pass, passContext))
        {
            SetCompositionFailure(
                "Legacy forward pass registration failed for '" + status.name +
                "'");
            return;
        }

        ++m_lastPassChainStats.graphPassCount;
        m_lastPassChainStats.graphPassNames.push_back(status.name);
    }

    if (!m_recordContext.view.colorTarget.IsValid())
    {
        SetCompositionFailure(
            "ForwardStandard did not produce the canonical HDR scene color surface");
        return;
    }

    TranslucencyOutputs outputs;
    outputs.provenance = builder.GetProvenance();
    outputs.frameIdentity = m_preparedContext.frameIdentity;
    outputs.viewIdentity = m_preparedContext.viewIdentity;
    outputs.hdrSceneColor = m_recordContext.view.colorTarget;
    outputs.hdrSceneColorMetadata = MakeMetadata(
        m_preparedContext.outputExtent, SceneTextures::HDRSceneColorContract);
    outputs.deviceDepth = m_recordContext.view.depthTarget;
    outputs.deviceDepthMetadata = MakeMetadata(
        m_preparedContext.outputExtent, SceneTextures::DeviceDepthContract);
    outputs.velocity = m_recordContext.view.velocityTarget;
    outputs.velocityMetadata = MakeMetadata(
        m_preparedContext.outputExtent, SceneTextures::VelocityContract);
    outputs.reactiveMask = m_recordContext.view.reactiveMaskTarget;
    outputs.reactiveMaskMetadata = MakeMetadata(
        m_preparedContext.outputExtent, SceneTextures::ReactiveMaskContract);
    outputs.exposureState = m_recordContext.view.exposureState;
    outputs.exposureStateMetadata = MakeExposureStateMetadata(
        m_recordContext.view);
    outputs.colorDomain = PostProcessColorDomain::SceneLinearHDR;
    outputs.transfer = PostProcessTransfer::None;
    if (blackboard.Publish(BuiltinRenderFeatureIds::LegacyForwardMainChain,
                           Describe(),
                           std::move(outputs)) !=
        RenderGraphBlackboardCode::Success)
    {
        SetCompositionFailure(
            "ForwardStandard could not publish TranslucencyOutputs v2");
    }
}

RenderFeatureExecutionReceipt LegacyForwardFeatureAdapter::CollectReceipt()
    const
{
    RenderFeatureExecutionReceipt receipt;
    receipt.featureId = BuiltinRenderFeatureIds::LegacyForwardMainChain;
    receipt.featureSchemaVersion = 1;
    receipt.phase = RenderFeaturePhase::Geometry;
    receipt.requirement = RenderFeatureRequirement::Required;
    receipt.selected = true;
    receipt.readiness = m_readiness;
    receipt.diagnostic = m_readiness.diagnostic;

    // Graph pass counts and execution status are registry-owned evidence.
    // Retaining a raw RenderGraph pointer here used to make delayed receipt
    // collection unsafe after a graph replacement. The registry snapshots
    // exact pass diagnostics while the graph is alive and derives the public
    // execution receipt from that immutable evidence.
    receipt.readiness.executed = false;
    m_lastReceipt = receipt;
    return receipt;
}

bool LegacyForwardFeatureAdapter::HasMatchingRecordContext(
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

void LegacyForwardFeatureAdapter::SetCompositionFailure(std::string diagnostic)
{
    m_readiness.inputsReady = false;
    m_readiness.executed = false;
    m_readiness.diagnostic = std::move(diagnostic);
}
} // namespace RVX
