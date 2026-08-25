#include "Features/LegacyForwardFeatureAdapter.h"

#include "Render/Passes/OpaquePass.h"
#include "Renderer/RenderPassRegistry.h"

#include <array>
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

    [[nodiscard]] bool HasCanonicalVelocityTarget(
        const RenderGraph& graph,
        RGTextureHandle handle,
        RenderOutputExtent extent) noexcept
    {
        const RHITextureDesc* const desc = graph.GetTextureDesc(handle);
        return handle.IsValid() && desc != nullptr &&
               desc->width == extent.width && desc->height == extent.height &&
               desc->depth == 1 && desc->mipLevels == 1 &&
               desc->arraySize == 1 &&
               desc->dimension == RHITextureDimension::Texture2D &&
               desc->format == SceneTextures::VelocityContract.format &&
               desc->sampleCount == SceneTextures::VelocityContract.sampleCount &&
               HasFlag(desc->usage, RHITextureUsage::RenderTarget) &&
               HasFlag(desc->usage, RHITextureUsage::ShaderResource);
    }

    [[nodiscard]] bool AddCanonicalVelocityPasses(
        RenderFeatureGraphBuilder& builder,
        const RenderPassRecordContext& context,
        const VelocityPassBindings& bindings,
        RenderOutputExtent extent,
        LegacyForwardFeaturePassChainStats& stats,
        std::string& outFailure)
    {
        const bool targetAvailable = HasCanonicalVelocityTarget(
            builder.GetGraph(), context.view.velocityTarget, extent);
        if (!bindings.IsComplete())
        {
            if (context.view.velocityTarget.IsValid())
            {
                outFailure =
                    "Canonical velocity target has no complete Camera/Object producer binding";
                return false;
            }
            return true;
        }

        const std::array<IRenderPass*, 2> producers = {
            bindings.camera, bindings.object};
        const std::array<const char*, 2> expectedNames = {
            "CameraVelocityPass", "ObjectVelocityPass"};
        bool anyRequested = false;
        for (size_t index = 0; index < producers.size(); ++index)
        {
            IRenderPass* const producer = producers[index];
            const RenderPassStatus status = producer->GetStatus();
            if (status.name != expectedNames[index])
            {
                outFailure =
                    "Canonical velocity producer binding has an unexpected pass identity";
                return false;
            }
            anyRequested = anyRequested || status.requestedEnabled;
        }

        if (!targetAvailable)
        {
            if (anyRequested || context.view.velocityTarget.IsValid())
            {
                outFailure =
                    "Requested velocity producers require a full-resolution RG16_FLOAT target";
                return false;
            }
            return true;
        }

        // Validate the complete producer set before mutating the graph.  A
        // partially registered velocity chain must never escape when the
        // later Object producer is unavailable.
        for (IRenderPass* const producer : producers)
        {
            const RenderPassStatus status = producer->GetStatus();
            if (!status.requestedEnabled || !status.supported || !status.enabled)
            {
                outFailure = "Required velocity producer '" + status.name +
                    "' is not executable";
                if (!status.supported)
                {
                    stats.unsupportedPassNames.push_back(status.name);
                }
                return false;
            }
        }

        for (IRenderPass* const producer : producers)
        {
            const RenderPassStatus status = producer->GetStatus();
            if (!builder.AddLegacyPass(*producer, context))
            {
                outFailure = "Velocity producer registration failed for '" +
                    status.name + "'";
                return false;
            }
            ++stats.graphPassCount;
            stats.graphPassNames.push_back(status.name);
        }
        return true;
    }
} // namespace

LegacyForwardFeatureAdapter::LegacyForwardFeatureAdapter(
    RenderPassRegistry* passRegistry,
    OpaquePass* opaquePass,
    VelocityPassBindings velocityPasses) noexcept
    : m_passRegistry(passRegistry)
    , m_opaquePass(opaquePass)
    , m_velocityPasses(velocityPasses)
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
    descriptor.name = "ForwardSurfaceData";
    descriptor.phase = RenderFeaturePhase::Geometry;
    descriptor.producedResources = {
        {RenderBlackboardResourceId::SceneTextures,
         SceneTextures::SchemaVersion}};
    descriptor.requiredCapabilities = ToMask(RenderFeatureCapability::Graphics);
    descriptor.requiredRecipes = ToMask(RenderPipelineRecipe::ForwardStandard);
    descriptor.qualityTiers = RVX_ALL_RENDER_QUALITY_TIERS;
    descriptor.fallbackPolicy = RenderFeatureFallbackPolicy::None;
    descriptor.shaderPipelineFamily = "ForwardSurfaceData";
    // A scene with no renderable surface work may still publish the current
    // SceneTextures contract. The registry records this as data-only
    // completion, never as GPU execution.
    descriptor.allowsDataOnlyCompletion = true;
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
        m_readiness.diagnostic = "Forward surface-data pass registry is unavailable";
    }
    else if (!m_readiness.hardwareSupported)
    {
        m_readiness.diagnostic = "Graphics capability is unavailable";
    }
    else if (!m_readiness.implementationSupported)
    {
        m_readiness.diagnostic =
            "Forward surface-data feature only implements ForwardStandard";
    }
    else if (!m_readiness.inputsReady)
    {
        m_readiness.diagnostic =
            "Forward surface-data record context is absent, stale, or mismatched";
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

    const int32 firstLightingConsumerPriority = m_opaquePass != nullptr
        ? m_opaquePass->GetPriority()
        : PassPriority::Opaque;
    for (const std::unique_ptr<IRenderPass>& pass : m_passRegistry->GetPasses())
    {
        if (!pass || pass.get() == m_opaquePass ||
            m_velocityPasses.Contains(pass.get()) ||
            pass->GetPriority() >= firstLightingConsumerPriority)
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
        if (!builder.AddLegacyPass(*pass, m_recordContext))
        {
            SetCompositionFailure(
                "Forward surface-data pass registration failed for '" +
                status.name + "'");
            return;
        }
        ++m_lastPassChainStats.graphPassCount;
        m_lastPassChainStats.graphPassNames.push_back(status.name);
    }

    std::string velocityFailure;
    if (!AddCanonicalVelocityPasses(
            builder, m_recordContext, m_velocityPasses,
            m_preparedContext.outputExtent, m_lastPassChainStats,
            velocityFailure))
    {
        SetCompositionFailure(std::move(velocityFailure));
        return;
    }

    if (!m_recordContext.view.colorTarget.IsValid() ||
        !m_recordContext.view.depthTarget.IsValid())
    {
        SetCompositionFailure(
            "ForwardStandard surface data requires canonical HDR color and D32 depth");
        return;
    }

    SceneTextures textures;
    textures.provenance = builder.GetProvenance();
    textures.hdrSceneColor = m_recordContext.view.colorTarget;
    textures.hdrSceneColorMetadata = MakeMetadata(
        m_preparedContext.outputExtent, SceneTextures::HDRSceneColorContract);
    textures.deviceDepth = m_recordContext.view.depthTarget;
    textures.deviceDepthMetadata = MakeMetadata(
        m_preparedContext.outputExtent, SceneTextures::DeviceDepthContract);
    if (m_recordContext.view.velocityTarget.IsValid())
    {
        textures.velocity = m_recordContext.view.velocityTarget;
        textures.velocityMetadata = MakeMetadata(
            m_preparedContext.outputExtent, SceneTextures::VelocityContract);
    }
    textures.exposureState = m_recordContext.view.exposureState;
    textures.exposureStateMetadata = MakeExposureStateMetadata(
        m_recordContext.view);
    if (blackboard.Publish(BuiltinRenderFeatureIds::LegacyForwardMainChain,
                           Describe(), std::move(textures)) !=
        RenderGraphBlackboardCode::Success)
    {
        SetCompositionFailure(
            "ForwardStandard could not publish SceneTextures surface data");
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

ForwardLightingFeatureAdapter::ForwardLightingFeatureAdapter(
    RenderPassRegistry* passRegistry,
    OpaquePass* opaquePass,
    VelocityPassBindings velocityPasses) noexcept
    : m_passRegistry(passRegistry)
    , m_opaquePass(opaquePass)
    , m_velocityPasses(velocityPasses)
{
}

void ForwardLightingFeatureAdapter::SetRecordContext(
    RenderPassRecordContext context)
{
    m_recordContext = std::move(context);
    m_hasRecordContext = true;
    m_preparedContext = {};
    m_readiness = {};
    m_lastPassChainStats = {};
    m_lastReceipt = {};
}

RenderFeatureDescriptor ForwardLightingFeatureAdapter::Describe() const
{
    RenderFeatureDescriptor descriptor;
    descriptor.featureId = BuiltinRenderFeatureIds::ForwardLighting;
    descriptor.schemaVersion = 1;
    descriptor.name = "ForwardLighting";
    descriptor.phase = RenderFeaturePhase::Lighting;
    descriptor.consumedResources = {
        {RenderBlackboardResourceId::AmbientOcclusionOutputs,
         AmbientOcclusionOutputs::SchemaVersion}};
    descriptor.producedResources = {
        {RenderBlackboardResourceId::TranslucencyOutputs,
         TranslucencyOutputs::SchemaVersion}};
    descriptor.requiredCapabilities =
        ToMask(RenderFeatureCapability::Graphics) |
        ToMask(RenderFeatureCapability::DescriptorBindings);
    descriptor.requiredRecipes = ToMask(RenderPipelineRecipe::ForwardStandard);
    descriptor.qualityTiers = RVX_ALL_RENDER_QUALITY_TIERS;
    descriptor.fallbackPolicy = RenderFeatureFallbackPolicy::None;
    descriptor.shaderPipelineFamily = "ForwardLighting";
    return descriptor;
}

void ForwardLightingFeatureAdapter::Prepare(
    const RenderFeatureContext& context)
{
    m_preparedContext = context;
    m_readiness = {};
    m_readiness.compiled = true;
    m_readiness.hardwareSupported = HasGraphicsCapability(
        context.hardwareCapabilities);
    m_readiness.implementationSupported = IsForwardStandard(context);
    m_readiness.pipelineReady = m_opaquePass != nullptr &&
        m_opaquePass->IsSupported();
    m_readiness.inputsReady = HasMatchingRecordContext(context) &&
        m_passRegistry != nullptr && m_opaquePass != nullptr;

    if (!m_readiness.hardwareSupported)
    {
        m_readiness.diagnostic = "Graphics capability is unavailable";
    }
    else if (!m_readiness.implementationSupported)
    {
        m_readiness.diagnostic =
            "Forward lighting feature only implements ForwardStandard";
    }
    else if (!m_readiness.pipelineReady)
    {
        m_readiness.diagnostic = m_opaquePass == nullptr
            ? "Forward opaque pass is unavailable"
            : m_opaquePass->GetUnsupportedReason();
    }
    else if (!m_readiness.inputsReady)
    {
        m_readiness.diagnostic =
            "Forward lighting record context is absent, stale, or mismatched";
    }
    m_readiness.executed = false;
}

RenderFeatureReadiness
ForwardLightingFeatureAdapter::ResolveReadiness() const
{
    return m_readiness;
}

void ForwardLightingFeatureAdapter::AddPasses(
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
            "Forward lighting record context does not match the target graph");
        return;
    }

    RenderGraphBlackboardCode code = RenderGraphBlackboardCode::Success;
    const AmbientOcclusionOutputs* const ambientOcclusion =
        blackboard.Require<AmbientOcclusionOutputs>(
            BuiltinRenderFeatureIds::ForwardLighting, Describe(), &code);
    if (ambientOcclusion == nullptr ||
        code != RenderGraphBlackboardCode::Success ||
        !ambientOcclusion->ambientOcclusion.IsValid() ||
        ambientOcclusion->frameIdentity != m_preparedContext.frameIdentity ||
        ambientOcclusion->viewIdentity != m_preparedContext.viewIdentity)
    {
        SetCompositionFailure(
            "Forward lighting requires the exact typed ambient-occlusion output");
        return;
    }

    RenderPassRecordContext opaqueContext = m_recordContext;
    if (opaqueContext.results == nullptr)
    {
        SetCompositionFailure(
            "Forward lighting requires graph-owned recording results");
        return;
    }
    opaqueContext.directionalShadow =
        opaqueContext.results->directionalShadowOutput;
    if (opaqueContext.directionalShadow.identity != opaqueContext.identity)
    {
        opaqueContext.directionalShadow = {};
        opaqueContext.directionalShadow.identity = opaqueContext.identity;
    }
    opaqueContext.rayTracedShadow =
        opaqueContext.results->rayTracedShadowOutput;
    if (opaqueContext.rayTracedShadow.identity != opaqueContext.identity)
    {
        opaqueContext.rayTracedShadow = {};
        opaqueContext.rayTracedShadow.identity = opaqueContext.identity;
    }
    opaqueContext.ambientOcclusion.identity = opaqueContext.identity;
    opaqueContext.ambientOcclusion.filteredOcclusion =
        ambientOcclusion->ambientOcclusion;
    opaqueContext.ambientOcclusion.extent =
        ambientOcclusion->ambientOcclusionMetadata.extent;
    opaqueContext.ambientOcclusion.format =
        ambientOcclusion->ambientOcclusionMetadata.format;
    opaqueContext.ambientOcclusion.sampleCount =
        ambientOcclusion->ambientOcclusionMetadata.sampleCount;
    opaqueContext.ambientOcclusion.settingsHash =
        ambientOcclusion->settingsHash;
    opaqueContext.ambientOcclusion.fieldContentHash =
        ambientOcclusion->ambientOcclusionFieldHash;
    opaqueContext.ambientOcclusion.requested = true;
    if (!opaqueContext.ambientOcclusion.IsCompatibleWith(
            opaqueContext.identity))
    {
        SetCompositionFailure(
            "Forward lighting rejected an invalid ambient-occlusion field contract");
        return;
    }

    m_lastPassChainStats.frameSequence = m_recordContext.identity.frameSequence;
    m_lastPassChainStats.passStatuses = m_passRegistry->GetPassStatuses();
    m_lastPassChainStats.registeredPassCount =
        m_lastPassChainStats.passStatuses.size();
    if (!builder.AddLegacyPass(*m_opaquePass, opaqueContext))
    {
        SetCompositionFailure("Forward opaque pass registration failed");
        return;
    }
    ++m_lastPassChainStats.graphPassCount;
    m_lastPassChainStats.graphPassNames.push_back(m_opaquePass->GetName());

    const int32 firstLightingConsumerPriority = m_opaquePass->GetPriority();
    for (const std::unique_ptr<IRenderPass>& pass : m_passRegistry->GetPasses())
    {
        if (!pass || pass.get() == m_opaquePass ||
            m_velocityPasses.Contains(pass.get()) ||
            pass->GetPriority() <= firstLightingConsumerPriority)
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

        // Later surfaces intentionally receive no screen AO. They retain the
        // exact graph-owned shadow state, but not the opaque-only AO field.
        RenderPassRecordContext passContext = opaqueContext;
        passContext.ambientOcclusion = {};
        if (!builder.AddLegacyPass(*pass, passContext))
        {
            SetCompositionFailure(
                "Forward post-lighting pass registration failed for '" +
                status.name + "'");
            return;
        }
        ++m_lastPassChainStats.graphPassCount;
        m_lastPassChainStats.graphPassNames.push_back(status.name);
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
    if (blackboard.Publish(BuiltinRenderFeatureIds::ForwardLighting,
                           Describe(), std::move(outputs)) !=
        RenderGraphBlackboardCode::Success)
    {
        SetCompositionFailure(
            "Forward lighting could not publish TranslucencyOutputs");
    }
}

RenderFeatureExecutionReceipt
ForwardLightingFeatureAdapter::CollectReceipt() const
{
    RenderFeatureExecutionReceipt receipt;
    receipt.featureId = BuiltinRenderFeatureIds::ForwardLighting;
    receipt.featureSchemaVersion = 1;
    receipt.phase = RenderFeaturePhase::Lighting;
    receipt.requirement = RenderFeatureRequirement::Required;
    receipt.selected = true;
    receipt.readiness = m_readiness;
    receipt.diagnostic = m_readiness.diagnostic;
    receipt.readiness.executed = false;
    m_lastReceipt = receipt;
    return receipt;
}

bool ForwardLightingFeatureAdapter::HasMatchingRecordContext(
    const RenderFeatureContext& context) const noexcept
{
    return m_hasRecordContext && context.frameIdentity.IsValid() &&
           context.viewIdentity.IsValid() &&
           m_recordContext.featureFrameIdentity == context.frameIdentity &&
           m_recordContext.featureViewIdentity == context.viewIdentity &&
           m_recordContext.IsFrameIdentityValid() &&
           m_recordContext.identity.frameSequence ==
               context.frameIdentity.frameSequence &&
           m_recordContext.identity.viewOrdinal == context.recordingViewOrdinal &&
           m_recordContext.identity.recordEpoch == context.recordEpoch;
}

void ForwardLightingFeatureAdapter::SetCompositionFailure(
    std::string diagnostic)
{
    m_readiness.inputsReady = false;
    m_readiness.executed = false;
    m_readiness.diagnostic = std::move(diagnostic);
}
} // namespace RVX
