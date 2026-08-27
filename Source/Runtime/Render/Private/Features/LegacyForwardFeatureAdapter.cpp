#include "Features/LegacyForwardFeatureAdapter.h"

#include "Render/Material/MaterialSystem.h"
#include "Render/Passes/OpaquePass.h"
#include "Render/Passes/ParticleFeaturePass.h"
#include "Renderer/RenderPassRegistry.h"

#include <algorithm>
#include <array>
#include <utility>

namespace RVX
{
namespace
{
    struct OpaqueSceneSnapshotCopyData
    {
        RGTextureHandle source{};
        RGTextureHandle destination{};
        std::shared_ptr<OpaqueSceneSnapshotCopyReceipt> receipt;
    };

    [[nodiscard]] bool AddOpaqueSceneSnapshotCopy(
        RenderFeatureGraphBuilder& builder,
        RGTextureHandle source,
        RGTextureHandle destination,
        std::shared_ptr<OpaqueSceneSnapshotCopyReceipt> receipt)
    {
        if (!receipt)
        {
            return false;
        }
        return builder.AddPass<OpaqueSceneSnapshotCopyData>(
            "OpaqueSceneSnapshotCopy",
            // The snapshot is produced by and immediately consumed on the
            // graphics queue. Keeping this tiny same-frame copy on graphics
            // avoids an otherwise unnecessary pair of queue-family ownership
            // transfers on Vulkan.
            RenderGraphPassType::Graphics,
            [source, destination, receipt = std::move(receipt)](
                RenderGraphBuilder& graphBuilder,
                OpaqueSceneSnapshotCopyData& data)
            {
                data.source = graphBuilder.Read(
                    source, RHIResourceState::CopySource);
                data.destination = graphBuilder.Write(
                    destination, RHIResourceState::CopyDest);
                data.receipt = receipt;
            },
            [](const OpaqueSceneSnapshotCopyData& data,
               RenderGraphPassContext& context)
            {
                RHITexture* const source = context.GetTexture(data.source);
                RHITexture* const destination = context.GetTexture(data.destination);
                if (source == nullptr || destination == nullptr || source == destination)
                {
                    context.MarkExecutionFailed("Opaque HDR snapshot copy has invalid aliased resources");
                    return;
                }
                context.Commands().CopyTexture(source, destination);
                if (data.receipt)
                {
                    data.receipt->copied.store(true, std::memory_order_release);
                }
            });
    }

    struct ParticleSoftDepthSnapshotCopyData
    {
        RGTextureHandle source{};
        RGTextureHandle destination{};
    };

    [[nodiscard]] bool AddParticleSoftDepthSnapshotCopy(
        RenderFeatureGraphBuilder& builder,
        RGTextureHandle source,
        RGTextureHandle destination)
    {
        return builder.AddPass<ParticleSoftDepthSnapshotCopyData>(
            "ParticleSoftDepthSnapshotCopy",
            RenderGraphPassType::Graphics,
            [source, destination](RenderGraphBuilder& graphBuilder,
                                  ParticleSoftDepthSnapshotCopyData& data)
            {
                data.source = graphBuilder.Read(source, RHIResourceState::CopySource);
                data.destination = graphBuilder.Write(destination, RHIResourceState::CopyDest);
            },
            [](const ParticleSoftDepthSnapshotCopyData& data,
               RenderGraphPassContext& context)
            {
                RHITexture* const source = context.GetTexture(data.source);
                RHITexture* const destination = context.GetTexture(data.destination);
                if (source == nullptr || destination == nullptr || source == destination ||
                    source->GetWidth() != destination->GetWidth() ||
                    source->GetHeight() != destination->GetHeight() ||
                    source->GetFormat() != destination->GetFormat())
                {
                    context.MarkExecutionFailed(
                        "Particle soft-depth snapshot copy has invalid source/destination provenance");
                    return;
                }
                context.Commands().CopyTexture(source, destination);
            });
    }

    [[nodiscard]] const ParticleFeaturePass* FindParticleFeaturePass(
        const RenderPassRegistry* registry) noexcept
    {
        if (registry == nullptr)
        {
            return nullptr;
        }
        for (const std::unique_ptr<IRenderPass>& pass : registry->GetPasses())
        {
            if (const auto* const particle =
                    dynamic_cast<const ParticleFeaturePass*>(pass.get()))
            {
                return particle;
            }
        }
        return nullptr;
    }

    [[nodiscard]] bool BuildParticleVfxRecordInput(
        RenderFeatureGraphBuilder& builder,
        const RenderPassRecordContext& context,
        const RenderPassRegistry& registry,
        RenderOutputExtent extent,
        ParticleVfxRecordInput& outInput,
        std::string& outFailure)
    {
        outInput = {};
        const ParticleFeaturePass* const particle = FindParticleFeaturePass(&registry);
        if (particle == nullptr)
        {
            return true;
        }

        const ParticleVfxRequirements requirements =
            particle->GetVfxRequirements();
        if (!requirements.requested)
        {
            return true;
        }

        outInput.identity = context.identity;
        outInput.softDepthRequested = requirements.softDepthRequested;
        outInput.softDepthRequired = requirements.softDepthRequired;
        outInput.reactiveMaskRequested = requirements.reactiveMaskRequested;
        outInput.distortionRequested = requirements.distortionRequested;

        if (requirements.softDepthRequested)
        {
            const RHITextureDesc* const depthDesc = builder.GetGraph().GetTextureDesc(
                context.view.depthTarget);
            const bool validSource = context.view.depthTarget.IsValid() &&
                HasCurrentGraphProvenance(context.view.depthTarget, context.identity) &&
                depthDesc != nullptr && depthDesc->format == RHIFormat::D32_FLOAT &&
                depthDesc->width == extent.width && depthDesc->height == extent.height &&
                depthDesc->sampleCount == RHISampleCount::Count1 &&
                HasFlag(depthDesc->usage, RHITextureUsage::CopySrc);
            if (!validSource)
            {
                if (requirements.softDepthRequired)
                {
                    outFailure =
                        "Particle required soft depth needs a D32 CopySrc depth surface matching the view extent";
                    return false;
                }
            }
            else
            {
                RHITextureDesc softDepthDesc = RHITextureDesc::DepthStencil(
                    extent.width, extent.height, RHIFormat::D32_FLOAT);
                // Explicitly retain sampled-depth capability: this transient
                // is copied once, then bound exclusively as the particle PS
                // SRV; the original depth stays a read-only DSV.
                softDepthDesc.usage = softDepthDesc.usage |
                    RHITextureUsage::ShaderResource | RHITextureUsage::CopyDst;
                softDepthDesc.debugName = "ParticleSoftDepthSnapshot";
                outInput.softDepthSnapshot = builder.CreateTexture(softDepthDesc);
                if (!outInput.softDepthSnapshot.IsValid() ||
                    outInput.softDepthSnapshot.index == context.view.depthTarget.index ||
                    !AddParticleSoftDepthSnapshotCopy(
                        builder, context.view.depthTarget, outInput.softDepthSnapshot))
                {
                    outFailure = "Particle soft-depth snapshot allocation/copy registration failed";
                    return false;
                }
            }
        }

        if (requirements.distortionRequested)
        {
            RHITextureDesc distortionDesc = RHITextureDesc::RenderTarget(
                extent.width, extent.height, RHIFormat::RG16_FLOAT);
            distortionDesc.debugName = "ParticleDistortionVectors";
            outInput.distortionVectors = builder.CreateTexture(distortionDesc);

            RHITextureDesc compositeDesc = RHITextureDesc::RenderTarget(
                extent.width, extent.height, RHIFormat::RGBA16_FLOAT);
            compositeDesc.debugName = "ParticleDistortionCompositeSceneColor";
            outInput.compositedSceneColor = builder.CreateTexture(compositeDesc);
            if (!outInput.distortionVectors.IsValid() ||
                !outInput.compositedSceneColor.IsValid() ||
                outInput.compositedSceneColor.index == context.view.colorTarget.index)
            {
                outFailure = "Particle distortion requires distinct RG16F and HDR graph outputs";
                return false;
            }
        }
        return true;
    }
    [[nodiscard]] uint32 CountEligibleToonOutlineMaterials(
        const MaterialParameterTableSnapshot& table) noexcept
    {
        return static_cast<uint32>(std::count_if(
            table.valueOnlyRowEvidence.begin(),
            table.valueOnlyRowEvidence.end(),
            [](const MaterialParameterTableRowEvidence& row)
            {
                return row.isAsset &&
                       row.shadingModel == MaterialShadingModel::ToonLit &&
                       row.toonParametersValid && row.toonOutlineWidth > 0.0f;
            }));
    }

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
    OpaqueRasterPipelinePreflightResult opaquePipelinePreflight;
    if (m_readiness.hardwareSupported &&
        m_readiness.implementationSupported && m_opaquePass != nullptr)
    {
        const OpaqueRasterOutputFamily outputFamily =
            m_recordContext.view.toonOutlineRequested != 0
                ? OpaqueRasterOutputFamily::ForwardSceneHDRWithSurfaceData
                : OpaqueRasterOutputFamily::ForwardSceneHDR;
        opaquePipelinePreflight = m_opaquePass->PreflightRasterPipelines(
            m_recordContext, outputFamily,
            OpaqueRasterSurfaceRoute::AllOpaque);
    }
    m_readiness.pipelineReady = m_opaquePass != nullptr &&
        m_opaquePass->IsSupported() && opaquePipelinePreflight.Succeeded();
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
        if (m_opaquePass == nullptr)
        {
            m_readiness.diagnostic = "Forward opaque pass is unavailable";
        }
        else if (!opaquePipelinePreflight.diagnostic.empty())
        {
            m_readiness.diagnostic =
                std::move(opaquePipelinePreflight.diagnostic);
        }
        else
        {
            m_readiness.diagnostic = m_opaquePass->GetUnsupportedReason();
        }
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

    RGTextureHandle outlineNormal{};
    RGTextureHandle outlineMaterialIdentity{};
    RGBufferHandle outlineMaterialTable{};
    PostProcessMaterialTableMetadata outlineMaterialTableMetadata{};
    const bool toonOutlineRequested =
        opaqueContext.view.toonOutlineRequested != 0;
    bool opaqueRegistered = false;
    if (toonOutlineRequested)
    {
        const RenderOutputExtent extent = m_preparedContext.outputExtent;
        RHITextureDesc normalDesc = RHITextureDesc::RenderTarget(
            extent.width, extent.height,
            SceneTextures::GBuffer1Contract.format);
        normalDesc.sampleCount = SceneTextures::GBuffer1Contract.sampleCount;
        normalDesc.SetOptimizedClearColor(
            RHIClearColor{0.5f, 0.5f, 1.0f, 0.0f});
        normalDesc.debugName = "ForwardWorldNormal";
        outlineNormal = builder.CreateTexture(normalDesc);

        RHITextureDesc materialIdentityDesc = RHITextureDesc::RenderTarget(
            extent.width, extent.height,
            SceneTextures::GBuffer3Contract.format);
        materialIdentityDesc.sampleCount =
            SceneTextures::GBuffer3Contract.sampleCount;
        materialIdentityDesc.SetOptimizedClearColor(
            RHIClearColor{0.0f, 0.0f, 0.0f, 0.0f});
        materialIdentityDesc.debugName = "ForwardMaterialIdentity";
        outlineMaterialIdentity = builder.CreateTexture(materialIdentityDesc);

        MaterialParameterTableSnapshot materialTable;
        std::string materialTableError;
        if (!outlineNormal.IsValid() || !outlineMaterialIdentity.IsValid() ||
            !m_opaquePass->CreateMaterialClassificationTableSnapshot(
                opaqueContext, OpaqueRasterSurfaceRoute::AllOpaque,
                materialTable, materialTableError))
        {
            SetCompositionFailure(materialTableError.empty()
                ? "Forward Toon outline could not create its typed surface inputs"
                : std::move(materialTableError));
            return;
        }

        outlineMaterialTableMetadata = PostProcessMaterialTableMetadata{
            PostProcessMaterialTableMetadata::SchemaVersion,
            materialTable.stride,
            materialTable.slotCount,
            CountEligibleToonOutlineMaterials(materialTable),
            materialTable.contentHash,
            extent,
            SceneTextures::GBuffer3Contract.sampleCount};
        opaqueContext.opaqueMaterialTable =
            OpaqueMaterialTableExecutionIdentity{
                1,
                materialTable.schemaVersion,
                materialTable.stride,
                materialTable.slotCount,
                materialTable.contentHash};
        outlineMaterialTable = builder.ImportBuffer(
            std::move(materialTable.buffer),
            RHIResourceState::ShaderResource);
        if (!outlineMaterialTable.IsValid() ||
            !outlineMaterialTableMetadata.IsValid())
        {
            SetCompositionFailure(
                "Forward Toon outline could not import its immutable material table");
            return;
        }

        const OpaqueForwardSurfaceTargets targets{
            opaqueContext.view.colorTarget,
            outlineNormal,
            outlineMaterialIdentity,
            opaqueContext.view.depthTarget};
        opaqueRegistered = builder.AddGraphCallback(
            "ForwardSurfaceData",
            opaqueContext,
            [opaque = m_opaquePass,
             context = opaqueContext,
             targets](RenderGraph& graph)
            {
                opaque->AddForwardSurfaceToGraph(graph, context, targets);
            });
    }
    else
    {
        opaqueRegistered = builder.AddLegacyPass(*m_opaquePass, opaqueContext);
    }
    if (!opaqueRegistered)
    {
        SetCompositionFailure("Forward opaque pass registration failed");
        return;
    }
    ++m_lastPassChainStats.graphPassCount;
    m_lastPassChainStats.graphPassNames.push_back(
        toonOutlineRequested ? "ForwardSurfaceData" : m_opaquePass->GetName());

    RHITextureDesc opaqueSnapshotDesc = RHITextureDesc::Texture2D(
        m_preparedContext.outputExtent.width,
        m_preparedContext.outputExtent.height,
        RHIFormat::RGBA16_FLOAT,
        RHITextureUsage::ShaderResource | RHITextureUsage::CopyDst);
    opaqueSnapshotDesc.debugName = "ForwardOpaqueSceneSnapshot";
    const RGTextureHandle opaqueSceneSnapshot = builder.CreateTexture(
        opaqueSnapshotDesc);
    if (!opaqueSceneSnapshot.IsValid() ||
        opaqueSceneSnapshot.index == opaqueContext.view.colorTarget.index)
    {
        SetCompositionFailure("Forward lighting could not allocate a distinct HDR opaque snapshot");
        return;
    }
    const std::shared_ptr<OpaqueSceneSnapshotCopyReceipt> opaqueSnapshotReceipt =
        std::make_shared<OpaqueSceneSnapshotCopyReceipt>();
    bool opaqueSnapshotRegistered = false;

    ParticleVfxRecordInput particleVfx;
    std::string particleVfxFailure;
    if (!BuildParticleVfxRecordInput(
            builder,
            opaqueContext,
            *m_passRegistry,
            m_preparedContext.outputExtent,
            particleVfx,
            particleVfxFailure))
    {
        SetCompositionFailure(std::move(particleVfxFailure));
        return;
    }
    RGTextureHandle activeSceneColor = opaqueContext.view.colorTarget;

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
        passContext.view.colorTarget = activeSceneColor;
        passContext.particleVfx = particleVfx;
        passContext.ambientOcclusion = {};
        if (!opaqueSnapshotRegistered &&
            pass->GetPriority() >= PassPriority::Transparent)
        {
            if (!AddOpaqueSceneSnapshotCopy(
                    builder,
                    activeSceneColor,
                    opaqueSceneSnapshot,
                    opaqueSnapshotReceipt))
            {
                SetCompositionFailure("Forward lighting could not register the HDR opaque snapshot copy");
                return;
            }
            ++m_lastPassChainStats.graphPassCount;
            m_lastPassChainStats.graphPassNames.push_back("OpaqueSceneSnapshotCopy");
            opaqueSnapshotRegistered = true;
        }
        if (opaqueSnapshotRegistered)
        {
            passContext.opaqueSceneSnapshot = OpaqueSceneSnapshotRecordInput{
                opaqueContext.identity,
                opaqueSceneSnapshot,
                opaqueSnapshotReceipt};
        }
        if (!builder.AddLegacyPass(*pass, passContext))
        {
            SetCompositionFailure(
                "Forward post-lighting pass registration failed for '" +
                status.name + "'");
            return;
        }
        if (const auto* const particle =
                dynamic_cast<const ParticleFeaturePass*>(pass.get());
            particle != nullptr && particleVfx.compositedSceneColor.IsValid() &&
            particle->HasRegisteredDistortionComposite())
        {
            activeSceneColor = particleVfx.compositedSceneColor;
        }
        ++m_lastPassChainStats.graphPassCount;
        m_lastPassChainStats.graphPassNames.push_back(status.name);
    }

    if (!opaqueSnapshotRegistered)
    {
        if (!AddOpaqueSceneSnapshotCopy(
                builder,
                activeSceneColor,
                opaqueSceneSnapshot,
                opaqueSnapshotReceipt))
        {
            SetCompositionFailure("Forward lighting could not register the HDR opaque snapshot copy");
            return;
        }
        ++m_lastPassChainStats.graphPassCount;
        m_lastPassChainStats.graphPassNames.push_back("OpaqueSceneSnapshotCopy");
    }

    TranslucencyOutputs outputs;
    outputs.provenance = builder.GetProvenance();
    outputs.frameIdentity = m_preparedContext.frameIdentity;
    outputs.viewIdentity = m_preparedContext.viewIdentity;
    outputs.hdrSceneColor = activeSceneColor;
    outputs.hdrSceneColorMetadata = MakeMetadata(
        m_preparedContext.outputExtent, SceneTextures::HDRSceneColorContract);
    outputs.deviceDepth = m_recordContext.view.depthTarget;
    outputs.deviceDepthMetadata = MakeMetadata(
        m_preparedContext.outputExtent, SceneTextures::DeviceDepthContract);
    if (toonOutlineRequested)
    {
        outputs.worldNormal = outlineNormal;
        outputs.worldNormalMetadata = MakeMetadata(
            m_preparedContext.outputExtent,
            SceneTextures::GBuffer1Contract);
        outputs.materialIdentity = outlineMaterialIdentity;
        outputs.materialIdentityMetadata = MakeMetadata(
            m_preparedContext.outputExtent,
            SceneTextures::GBuffer3Contract);
        outputs.materialParameterTable = outlineMaterialTable;
        outputs.materialParameterTableMetadata =
            outlineMaterialTableMetadata;
    }
    outputs.velocity = m_recordContext.view.velocityTarget;
    outputs.velocityMetadata = MakeMetadata(
        m_preparedContext.outputExtent, SceneTextures::VelocityContract);
    outputs.reactiveMask = m_recordContext.view.reactiveMaskTarget;
    outputs.reactiveMaskMetadata = MakeMetadata(
        m_preparedContext.outputExtent, SceneTextures::ReactiveMaskContract);
    outputs.distortionVectors = particleVfx.distortionVectors;
    if (outputs.distortionVectors.IsValid())
    {
        outputs.distortionVectorsMetadata = MakeMetadata(
            m_preparedContext.outputExtent,
            SceneTextures::FormatContract{
                RHIFormat::RG16_FLOAT, RHISampleCount::Count1});
    }
    outputs.opaqueSceneSnapshot = opaqueSceneSnapshot;
    outputs.opaqueSceneSnapshotMetadata = MakeMetadata(
        m_preparedContext.outputExtent, SceneTextures::HDRSceneColorContract);
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
