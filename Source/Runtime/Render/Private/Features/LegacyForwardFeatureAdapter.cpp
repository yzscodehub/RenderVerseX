#include "Features/LegacyForwardFeatureAdapter.h"

#include "Features/DefaultRenderPassBundle.h"

#include "Render/Material/MaterialSystem.h"
#include "Render/Passes/CameraVelocityPass.h"
#include "Render/Passes/ObjectVelocityPass.h"
#include "Render/Passes/OpaquePass.h"
#include "Render/Passes/ParticleFeaturePass.h"
#include "Render/Passes/RayTracedReflectionCompositePass.h"
#include "Render/Passes/RayTracedReflectionDenoisePass.h"
#include "Render/Passes/RayTracedReflectionPass.h"
#include "Render/Passes/RayTracedShadowPass.h"
#include "Render/Passes/ShadowPass.h"
#include "Render/Passes/WaterFeaturePass.h"

#include <algorithm>
#include <array>
#include <utility>

namespace RVX
{
namespace
{
    [[nodiscard]] bool IsHybridRayTracingReflectionPass(
        const IRenderPass& pass) noexcept
    {
        return dynamic_cast<const RayTracedReflectionPass*>(&pass) != nullptr ||
               dynamic_cast<const RayTracedReflectionDenoisePass*>(&pass) !=
                   nullptr ||
               dynamic_cast<const RayTracedReflectionCompositePass*>(&pass) !=
                   nullptr;
    }

    [[nodiscard]] bool IsRasterDirectionalShadowPass(
        const IRenderPass& pass) noexcept
    {
        return dynamic_cast<const RayTracedShadowPass*>(&pass) == nullptr ||
               dynamic_cast<const ShadowPass*>(&pass) != nullptr;
    }

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
                // This snapshot is published through the typed blackboard.
                // Blackboard handles do not create RenderGraph dependencies,
                // so preserve the copy as the producer of the shader-readable
                // frame contract even when no later special surface samples it.
                graphBuilder.SetExportState(
                    data.destination, RHIResourceState::ShaderResource);
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

    struct WaterDepthSnapshotCopyData
    {
        RGTextureHandle source{};
        RGTextureHandle destination{};
        std::shared_ptr<WaterDepthSnapshotCopyReceipt> receipt;
    };

    [[nodiscard]] bool AddWaterDepthSnapshotCopy(
        RenderFeatureGraphBuilder& builder,
        RGTextureHandle source,
        RGTextureHandle destination,
        std::shared_ptr<WaterDepthSnapshotCopyReceipt> receipt)
    {
        if (!receipt)
        {
            return false;
        }
        return builder.AddPass<WaterDepthSnapshotCopyData>(
            "WaterDepthSnapshotCopy", RenderGraphPassType::Graphics,
            [source, destination, receipt = std::move(receipt)](
                RenderGraphBuilder& graphBuilder, WaterDepthSnapshotCopyData& data)
            {
                data.source = graphBuilder.Read(source, RHIResourceState::CopySource);
                data.destination = graphBuilder.Write(destination, RHIResourceState::CopyDest);
                data.receipt = receipt;
            },
            [](const WaterDepthSnapshotCopyData& data, RenderGraphPassContext& context)
            {
                RHITexture* const source = context.GetTexture(data.source);
                RHITexture* const destination = context.GetTexture(data.destination);
                if (source == nullptr || destination == nullptr || source == destination ||
                    source->GetWidth() != destination->GetWidth() ||
                    source->GetHeight() != destination->GetHeight() ||
                    source->GetFormat() != RHIFormat::D32_FLOAT ||
                    destination->GetFormat() != RHIFormat::D32_FLOAT)
                {
                    context.MarkExecutionFailed("Water depth snapshot copy has invalid source/destination provenance");
                    return;
                }
                context.Commands().CopyTexture(source, destination);
                if (data.receipt)
                {
                    data.receipt->copied.store(true, std::memory_order_release);
                }
            });
    }

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
        const DefaultRenderPassBundle* bundle) noexcept
    {
        return bundle != nullptr
            ? bundle->FindPass<ParticleFeaturePass>()
            : nullptr;
    }

    [[nodiscard]] const WaterFeaturePass* FindWaterFeaturePass(
        const DefaultRenderPassBundle* bundle) noexcept
    {
        return bundle != nullptr
            ? bundle->FindPass<WaterFeaturePass>()
            : nullptr;
    }

    [[nodiscard]] bool BuildWaterSurfaceRecordInput(
        RenderFeatureGraphBuilder& builder,
        const RenderPassRecordContext& context,
        const DefaultRenderPassBundle& bundle,
        RenderOutputExtent extent,
        WaterSurfaceRecordInput& outInput,
        std::string& outFailure)
    {
        outInput = {};
        const WaterFeaturePass* const water = FindWaterFeaturePass(&bundle);
        if (water == nullptr || !water->GetRequirements().requested)
        {
            return true;
        }
        const WaterSurfaceRequirements requirements = water->GetRequirements();
        if (!requirements.opaqueSceneSnapshotRequired ||
            !context.opaqueSceneSnapshot.IsCompatibleWith(context.identity))
        {
            outFailure = "Water requires the current graph's immutable HDR opaque snapshot";
            return false;
        }
        if (!requirements.exposureStateRequired ||
            !context.view.exposureState.IsValid() ||
            !HasCurrentGraphProvenance(
                context.view.exposureState, context.identity) ||
            context.view.exposureStateContentHash == 0)
        {
            outFailure =
                "Water requires the exact committed exposure state paired with the HDR opaque snapshot";
            return false;
        }
        const RHITextureDesc* const depthDesc = builder.GetGraph().GetTextureDesc(
            context.view.depthTarget);
        const bool validDepthSource = context.view.depthTarget.IsValid() &&
            HasCurrentGraphProvenance(context.view.depthTarget, context.identity) &&
            depthDesc != nullptr && depthDesc->format == RHIFormat::D32_FLOAT &&
            depthDesc->width == extent.width && depthDesc->height == extent.height &&
            depthDesc->sampleCount == RHISampleCount::Count1 &&
            HasFlag(depthDesc->usage, RHITextureUsage::CopySrc);
        if (!validDepthSource)
        {
            outFailure = "Water requires a D32 CopySrc depth surface matching the view extent";
            return false;
        }
        RHITextureDesc depthSnapshotDesc = RHITextureDesc::DepthStencil(
            extent.width, extent.height, RHIFormat::D32_FLOAT);
        depthSnapshotDesc.usage = depthSnapshotDesc.usage |
            RHITextureUsage::ShaderResource | RHITextureUsage::CopyDst;
        depthSnapshotDesc.debugName = "WaterDepthSnapshot";
        outInput.identity = context.identity;
        outInput.opaqueSceneSnapshot = context.opaqueSceneSnapshot;
        outInput.exposureState = context.view.exposureState;
        outInput.exposureStateContentHash = context.view.exposureStateContentHash;
        outInput.depthSnapshot = builder.CreateTexture(depthSnapshotDesc);
        outInput.depthReceipt = std::make_shared<WaterDepthSnapshotCopyReceipt>();
        if (!outInput.depthSnapshot.IsValid() ||
            outInput.depthSnapshot.index == context.view.depthTarget.index ||
            !AddWaterDepthSnapshotCopy(
                builder, context.view.depthTarget, outInput.depthSnapshot,
                outInput.depthReceipt))
        {
            outFailure = "Water depth snapshot allocation/copy registration failed";
            return false;
        }
        return true;
    }

    [[nodiscard]] bool BuildParticleVfxRecordInput(
        RenderFeatureGraphBuilder& builder,
        const RenderPassRecordContext& context,
        const DefaultRenderPassBundle& bundle,
        RenderOutputExtent extent,
        ParticleVfxRecordInput& outInput,
        std::string& outFailure)
    {
        outInput = {};
        const ParticleFeaturePass* const particle = FindParticleFeaturePass(&bundle);
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
        if (!requirements.exposureStateRequired ||
            !context.view.exposureState.IsValid() ||
            !HasCurrentGraphProvenance(
                context.view.exposureState, context.identity) ||
            context.view.exposureStateContentHash == 0)
        {
            outFailure =
                "Particle VFX requires the exact committed exposure state for authored HDR radiance";
            return false;
        }

        outInput.identity = context.identity;
        outInput.exposureState = context.view.exposureState;
        outInput.exposureStateContentHash = context.view.exposureStateContentHash;
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

    [[nodiscard]] bool BuildLocalShadowOutputs(
        const RenderPassRecordContext& context,
        RenderGraphBlackboardProvenance provenance,
        LocalShadowOutputs& outOutputs,
        std::string& outDiagnostic)
    {
        outOutputs = {};
        outDiagnostic.clear();
        if (context.results == nullptr || context.lightingPlan == nullptr ||
            context.localShadowAtlasPlan == nullptr)
        {
            outDiagnostic =
                "Local-shadow publication requires plan-owned graph results";
            return false;
        }

        const LocalShadowAtlasPlan& atlasPlan =
            *context.localShadowAtlasPlan;
        const uint64 lightingPlanIdentity =
            context.lightingPlan->GetIdentityHash();
        const LocalShadowRecordOutput& recorded =
            context.results->localShadowOutput;
        const bool requested = !atlasPlan.GetPages().empty();
        if (!atlasPlan.IsValid() ||
            atlasPlan.GetFrameIdentity() != context.featureFrameIdentity ||
            atlasPlan.GetViewIdentity() != context.featureViewIdentity ||
            atlasPlan.GetLightingPlanIdentity() != lightingPlanIdentity ||
            !recorded.IsCompatibleWith(
                context.identity, context.featureFrameIdentity,
                context.featureViewIdentity, lightingPlanIdentity) ||
            recorded.enabled != requested)
        {
            outDiagnostic =
                "Local-shadow graph output is stale, incomplete, or plan-mismatched";
            return false;
        }
        if (requested && !recorded.MatchesAtlasPlan(atlasPlan))
        {
            outDiagnostic =
                "Local-shadow graph output does not match the sealed atlas pages";
            return false;
        }

        outOutputs.provenance = provenance;
        outOutputs.frameIdentity = atlasPlan.GetFrameIdentity();
        outOutputs.viewIdentity = atlasPlan.GetViewIdentity();
        outOutputs.lightingPlanIdentity = lightingPlanIdentity;
        outOutputs.localShadowPlanIdentity =
            atlasPlan.GetLocalShadowPlanIdentity();
        outOutputs.atlasPlanIdentity = atlasPlan.GetIdentityHash();
        outOutputs.atlasGeneration = atlasPlan.GetAtlasGeneration();
        outOutputs.requested = requested;
        if (requested)
        {
            outOutputs.depthAtlas = recorded.depthAtlas;
            outOutputs.pageCount = recorded.pageCount;
            outOutputs.pageResolution = recorded.pageResolution;
        }
        if (!outOutputs.IsValid())
        {
            outDiagnostic = "LocalShadowOutputs v1 validation failed";
            return false;
        }
        return true;
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
        DefaultRenderPassBundle& bundle,
        RenderOutputExtent extent,
        LegacyForwardFeaturePassChainStats& stats,
        std::string& outFailure)
    {
        const bool targetAvailable = HasCanonicalVelocityTarget(
            builder.GetGraph(), context.view.velocityTarget, extent);
        CameraVelocityPass* const camera =
            bundle.FindPass<CameraVelocityPass>();
        ObjectVelocityPass* const object =
            bundle.FindPass<ObjectVelocityPass>();
        if (camera == nullptr || object == nullptr ||
            static_cast<IRenderPass*>(camera) ==
                static_cast<IRenderPass*>(object))
        {
            if (context.view.velocityTarget.IsValid())
            {
                outFailure =
                    "Canonical velocity target has no complete Camera/Object producer binding";
                return false;
            }
            return true;
        }

        const std::array<IRenderPass*, 2> producers = {camera, object};
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
    DefaultRenderPassBundle* passBundle) noexcept
    : m_passBundle(passBundle)
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
         SceneTextures::SchemaVersion},
        {RenderBlackboardResourceId::LocalShadowOutputs,
         LocalShadowOutputs::SchemaVersion}};
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

    if (m_passBundle == nullptr)
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
    m_lastPassChainStats.passStatuses =
        m_passBundle->GetRegistry().GetPassStatuses();
    m_lastPassChainStats.registeredPassCount =
        m_lastPassChainStats.passStatuses.size();

    OpaquePass* const opaquePass = m_passBundle->FindPass<OpaquePass>();
    const int32 firstLightingConsumerPriority = opaquePass != nullptr
        ? opaquePass->GetPriority()
        : PassPriority::Opaque;
    for (const std::unique_ptr<IRenderPass>& pass :
         m_passBundle->GetRegistry().GetPasses())
    {
        if (!pass || pass.get() == opaquePass ||
            pass.get() == m_passBundle->FindPass<CameraVelocityPass>() ||
            pass.get() == m_passBundle->FindPass<ObjectVelocityPass>() ||
            pass->GetPriority() >= firstLightingConsumerPriority ||
            !IsRasterDirectionalShadowPass(*pass))
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
            builder, m_recordContext, *m_passBundle,
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
    LocalShadowOutputs localShadows;
    std::string localShadowFailure;
    if (!BuildLocalShadowOutputs(
            m_recordContext, builder.GetProvenance(), localShadows,
            localShadowFailure))
    {
        SetCompositionFailure(std::move(localShadowFailure));
        return;
    }
    if (blackboard.Publish(
            BuiltinRenderFeatureIds::LegacyForwardMainChain,
            Describe(), std::move(localShadows)) !=
        RenderGraphBlackboardCode::Success)
    {
        SetCompositionFailure(
            "ForwardStandard could not publish LocalShadowOutputs v1");
        return;
    }
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
    DefaultRenderPassBundle* passBundle) noexcept
    : m_passBundle(passBundle)
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
         AmbientOcclusionOutputs::SchemaVersion},
        {RenderBlackboardResourceId::LocalShadowOutputs,
         LocalShadowOutputs::SchemaVersion}};
    descriptor.producedResources = {
        {RenderBlackboardResourceId::LightingOutputs,
         LightingOutputs::SchemaVersion}};
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
    const bool hasValidLightingPlan = HasValidLightingPlan(context);
    OpaquePass* const opaquePass = m_passBundle != nullptr
        ? m_passBundle->FindPass<OpaquePass>()
        : nullptr;
    OpaqueRasterPipelinePreflightResult opaquePipelinePreflight;
    if (m_readiness.hardwareSupported &&
        m_readiness.implementationSupported && hasValidLightingPlan &&
        opaquePass != nullptr)
    {
        const OpaqueRasterOutputFamily outputFamily =
            (m_recordContext.view.toonOutlineRequested != 0 ||
             m_recordContext.requiresReflectionSurfaceData)
                ? OpaqueRasterOutputFamily::ForwardSceneHDRWithSurfaceData
                : OpaqueRasterOutputFamily::ForwardSceneHDR;
        opaquePipelinePreflight = opaquePass->PreflightRasterPipelines(
            m_recordContext, outputFamily,
            OpaqueRasterSurfaceRoute::AllOpaque);
    }
    m_readiness.pipelineReady = opaquePass != nullptr &&
        opaquePass->IsSupported() && opaquePipelinePreflight.Succeeded();
    m_readiness.inputsReady = HasMatchingRecordContext(context) &&
        hasValidLightingPlan && m_passBundle != nullptr &&
        opaquePass != nullptr && m_recordContext.lightingResources != nullptr &&
        m_recordContext.lightingResources->IsComplete() &&
        m_recordContext.lightingResources->HasCurrentGraphResources(
            m_recordContext.identity) &&
        (!(m_recordContext.requiresReflectionSurfaceData ||
           m_recordContext.view.toonOutlineRequested != 0) ||
         m_recordContext.view.exposureStateContentHash != 0);

    if (!m_readiness.hardwareSupported)
    {
        m_readiness.diagnostic = "Graphics capability is unavailable";
    }
    else if (!m_readiness.implementationSupported)
    {
        m_readiness.diagnostic =
            "Forward lighting feature only implements ForwardStandard";
    }
    else if (!hasValidLightingPlan)
    {
        m_readiness.diagnostic =
            "Forward lighting requires the current frame/view LightingFramePlan";
    }
    else if (!m_readiness.pipelineReady)
    {
        if (opaquePass == nullptr)
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
            m_readiness.diagnostic = opaquePass->GetUnsupportedReason();
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
    OpaquePass* const opaquePass = m_passBundle != nullptr
        ? m_passBundle->FindPass<OpaquePass>()
        : nullptr;
    if (opaquePass == nullptr)
    {
        SetCompositionFailure("Forward opaque pass is unavailable");
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

    const LocalShadowOutputs* const localShadows =
        blackboard.Require<LocalShadowOutputs>(
            BuiltinRenderFeatureIds::ForwardLighting, Describe(), &code);
    const LocalShadowAtlasPlan* const localShadowAtlasPlan =
        m_recordContext.localShadowAtlasPlan.get();
    if (localShadows == nullptr ||
        code != RenderGraphBlackboardCode::Success ||
        localShadowAtlasPlan == nullptr || !localShadows->IsValid() ||
        localShadows->frameIdentity != m_preparedContext.frameIdentity ||
        localShadows->viewIdentity != m_preparedContext.viewIdentity ||
        localShadows->lightingPlanIdentity !=
            m_recordContext.lightingPlan->GetIdentityHash() ||
        localShadows->localShadowPlanIdentity !=
            localShadowAtlasPlan->GetLocalShadowPlanIdentity() ||
        localShadows->atlasPlanIdentity !=
            localShadowAtlasPlan->GetIdentityHash() ||
        localShadows->atlasGeneration !=
            localShadowAtlasPlan->GetAtlasGeneration() ||
        localShadows->requested !=
            !localShadowAtlasPlan->GetPages().empty())
    {
        SetCompositionFailure(
            "Forward lighting requires the exact typed local-shadow output");
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
    m_lastPassChainStats.passStatuses =
        m_passBundle->GetRegistry().GetPassStatuses();
    m_lastPassChainStats.registeredPassCount =
        m_lastPassChainStats.passStatuses.size();

    RGTextureHandle outlineNormal{};
    RGTextureHandle outlineMaterialIdentity{};
    RGTextureHandle baseIndirectSpecular{};
    RGTextureHandle specularResponse{};
    RGBufferHandle outlineMaterialTable{};
    PostProcessMaterialTableMetadata outlineMaterialTableMetadata{};
    const bool toonOutlineRequested =
        opaqueContext.view.toonOutlineRequested != 0;
    const bool needsSurfaceData = toonOutlineRequested ||
        opaqueContext.requiresReflectionSurfaceData;
    bool opaqueRegistered = false;
    if (needsSurfaceData)
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
            !opaquePass->CreateMaterialClassificationTableSnapshot(
                opaqueContext, OpaqueRasterSurfaceRoute::AllOpaque,
                materialTable, materialTableError))
        {
            SetCompositionFailure(materialTableError.empty()
                ? "Forward surface-data could not create its typed inputs"
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
                "Forward surface-data could not import its immutable material table");
            return;
        }

        RHITextureDesc baseIndirectSpecularDesc = RHITextureDesc::RenderTarget(
            extent.width, extent.height,
            SceneTextures::BaseIndirectSpecularContract.format);
        baseIndirectSpecularDesc.sampleCount =
            SceneTextures::BaseIndirectSpecularContract.sampleCount;
        baseIndirectSpecularDesc.debugName = "ForwardBaseIndirectSpecular";
        baseIndirectSpecular = builder.CreateTexture(baseIndirectSpecularDesc);

        RHITextureDesc specularResponseDesc = RHITextureDesc::RenderTarget(
            extent.width, extent.height,
            SceneTextures::SpecularResponseContract.format);
        specularResponseDesc.sampleCount =
            SceneTextures::SpecularResponseContract.sampleCount;
        specularResponseDesc.debugName = "ForwardSpecularResponse";
        specularResponse = builder.CreateTexture(specularResponseDesc);
        if (!baseIndirectSpecular.IsValid() || !specularResponse.IsValid())
        {
            SetCompositionFailure(
                "Forward surface-data could not create its reflection-replacement targets");
            return;
        }

        const OpaqueForwardSurfaceTargets targets{
            opaqueContext.view.colorTarget,
            outlineNormal,
            outlineMaterialIdentity,
            baseIndirectSpecular,
            specularResponse,
            opaqueContext.view.depthTarget};
        opaqueRegistered = builder.AddGraphCallback(
            "ForwardSurfaceData",
            opaqueContext,
            [opaque = opaquePass,
             context = opaqueContext,
             targets](RenderGraph& graph)
            {
                opaque->AddForwardSurfaceToGraph(graph, context, targets);
            });
    }
    else
    {
        opaqueRegistered = builder.AddLegacyPass(*opaquePass, opaqueContext);
    }
    if (!opaqueRegistered)
    {
        SetCompositionFailure("Forward opaque pass registration failed");
        return;
    }
    ++m_lastPassChainStats.graphPassCount;
    m_lastPassChainStats.graphPassNames.push_back(
        needsSurfaceData ? "ForwardSurfaceData" : opaquePass->GetName());

    const RenderLightingFrameResourceSnapshot* const lightingResources =
        m_recordContext.lightingResources.get();
    if (lightingResources == nullptr || !lightingResources->IsComplete() ||
        !lightingResources->HasCurrentGraphResources(m_recordContext.identity))
    {
        SetCompositionFailure(
            "Forward lighting lost the current LightingFramePlan resources");
        return;
    }

    LightingOutputs outputs;
    outputs.provenance = builder.GetProvenance();
    outputs.frameIdentity = m_preparedContext.frameIdentity;
    outputs.viewIdentity = m_preparedContext.viewIdentity;
    outputs.lightingPlanIdentity =
        m_recordContext.lightingPlan->GetIdentityHash();
    outputs.hdrSceneColor = opaqueContext.view.colorTarget;
    outputs.hdrSceneColorMetadata = MakeMetadata(
        m_preparedContext.outputExtent, SceneTextures::HDRSceneColorContract);
    outputs.clusteredLights = lightingResources->clusterData;
    if (needsSurfaceData)
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
        outputs.baseIndirectSpecular = baseIndirectSpecular;
        outputs.baseIndirectSpecularMetadata = MakeMetadata(
            m_preparedContext.outputExtent,
            SceneTextures::BaseIndirectSpecularContract);
        outputs.specularResponse = specularResponse;
        outputs.specularResponseMetadata = MakeMetadata(
            m_preparedContext.outputExtent,
            SceneTextures::SpecularResponseContract);
        outputs.exposureState = opaqueContext.view.exposureState;
        outputs.exposureStateMetadata = MakeExposureStateMetadata(
            opaqueContext.view);
        outputs.exposureStateContentHash =
            opaqueContext.view.exposureStateContentHash;
    }
    if (blackboard.Publish(BuiltinRenderFeatureIds::ForwardLighting,
                           Describe(), std::move(outputs)) !=
        RenderGraphBlackboardCode::Success)
    {
        SetCompositionFailure(
            "Forward lighting could not publish LightingOutputs v5");
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

bool ForwardLightingFeatureAdapter::HasValidLightingPlan(
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

void ForwardLightingFeatureAdapter::SetCompositionFailure(
    std::string diagnostic)
{
    m_readiness.inputsReady = false;
    m_readiness.executed = false;
    m_readiness.diagnostic = std::move(diagnostic);
}

ForwardSpecialFeatureAdapter::ForwardSpecialFeatureAdapter(
    DefaultRenderPassBundle* passBundle) noexcept
    : m_passBundle(passBundle)
{
}

void ForwardSpecialFeatureAdapter::SetRecordContext(
    RenderPassRecordContext context)
{
    m_recordContext = std::move(context);
    m_hasRecordContext = true;
    m_preparedContext = {};
    m_readiness = {};
    m_lastPassChainStats = {};
    m_lastReceipt = {};
}

RenderFeatureDescriptor ForwardSpecialFeatureAdapter::Describe() const
{
    RenderFeatureDescriptor descriptor;
    descriptor.featureId = BuiltinRenderFeatureIds::ForwardSpecialChain;
    descriptor.schemaVersion = 1;
    descriptor.name = "ForwardSpecialChain";
    descriptor.phase = RenderFeaturePhase::Sky;
    descriptor.consumedResources = {
        {RenderBlackboardResourceId::VolumetricLightingOutputs,
         VolumetricLightingOutputs::SchemaVersion},
        {RenderBlackboardResourceId::LocalShadowOutputs,
         LocalShadowOutputs::SchemaVersion}};
    descriptor.producedResources = {
        {RenderBlackboardResourceId::PreTranslucencyOutputs,
         PreTranslucencyOutputs::SchemaVersion}};
    descriptor.requiredCapabilities = ToMask(RenderFeatureCapability::Graphics);
    descriptor.requiredRecipes = ToMask(RenderPipelineRecipe::ForwardStandard);
    descriptor.qualityTiers = RVX_ALL_RENDER_QUALITY_TIERS;
    descriptor.fallbackPolicy = RenderFeatureFallbackPolicy::None;
    descriptor.shaderPipelineFamily = "ForwardSpecialSurfaces";
    return descriptor;
}

void ForwardSpecialFeatureAdapter::Prepare(
    const RenderFeatureContext& context)
{
    m_preparedContext = context;
    m_readiness = {};
    m_readiness.compiled = true;
    m_readiness.hardwareSupported = HasGraphicsCapability(
        context.hardwareCapabilities);
    m_readiness.implementationSupported = IsForwardStandard(context);
    const bool hasValidLightingPlan = HasValidLightingPlan(context);
    m_readiness.pipelineReady = m_passBundle != nullptr &&
        m_passBundle->FindPass<OpaquePass>() != nullptr;
    m_readiness.inputsReady = HasMatchingRecordContext(context) &&
        hasValidLightingPlan && m_readiness.pipelineReady;

    if (!m_readiness.hardwareSupported)
    {
        m_readiness.diagnostic = "Graphics capability is unavailable";
    }
    else if (!m_readiness.implementationSupported)
    {
        m_readiness.diagnostic =
            "Forward special-surface feature only implements ForwardStandard";
    }
    else if (!hasValidLightingPlan)
    {
        m_readiness.diagnostic =
            "Forward special-surface feature requires the current LightingFramePlan";
    }
    else if (!m_readiness.pipelineReady)
    {
        m_readiness.diagnostic =
            "Forward special-surface composition dependencies are unavailable";
    }
    else if (!HasMatchingRecordContext(context))
    {
        m_readiness.diagnostic =
            "Forward special-surface record context is absent, stale, or mismatched";
    }
    m_readiness.executed = false;
}

RenderFeatureReadiness
ForwardSpecialFeatureAdapter::ResolveReadiness() const
{
    return m_readiness;
}

void ForwardSpecialFeatureAdapter::AddPasses(
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
            "Forward special-surface record context does not match the target graph");
        return;
    }
    OpaquePass* const opaquePass = m_passBundle != nullptr
        ? m_passBundle->FindPass<OpaquePass>()
        : nullptr;
    if (opaquePass == nullptr)
    {
        SetCompositionFailure("Forward opaque pass is unavailable");
        return;
    }

    RenderGraphBlackboardCode code = RenderGraphBlackboardCode::Success;
    const VolumetricLightingOutputs* const volumetricLighting =
        blackboard.Require<VolumetricLightingOutputs>(
            BuiltinRenderFeatureIds::ForwardSpecialChain,
            Describe(), &code);
    const uint64 expectedLightingPlanIdentity =
        m_recordContext.lightingPlan != nullptr
            ? m_recordContext.lightingPlan->GetIdentityHash()
            : 0;
    if (volumetricLighting == nullptr ||
        code != RenderGraphBlackboardCode::Success ||
        volumetricLighting->frameIdentity != m_preparedContext.frameIdentity ||
        volumetricLighting->viewIdentity != m_preparedContext.viewIdentity ||
        volumetricLighting->lightingPlanIdentity !=
            expectedLightingPlanIdentity ||
        !volumetricLighting->resolvedSceneColor.IsValid() ||
        volumetricLighting->resolvedSceneColorMetadata.extent !=
            m_preparedContext.outputExtent)
    {
        SetCompositionFailure(
            "Forward special-surface chain requires the exact VolumetricLightingOutputs v1");
        return;
    }
    code = RenderGraphBlackboardCode::Success;
    const LocalShadowOutputs* const localShadows =
        blackboard.Require<LocalShadowOutputs>(
            BuiltinRenderFeatureIds::ForwardSpecialChain,
            Describe(), &code);
    const LocalShadowAtlasPlan* const localShadowAtlasPlan =
        m_recordContext.localShadowAtlasPlan.get();
    if (localShadows == nullptr ||
        code != RenderGraphBlackboardCode::Success ||
        localShadowAtlasPlan == nullptr || !localShadows->IsValid() ||
        localShadows->frameIdentity != m_preparedContext.frameIdentity ||
        localShadows->viewIdentity != m_preparedContext.viewIdentity ||
        localShadows->lightingPlanIdentity != expectedLightingPlanIdentity ||
        localShadows->localShadowPlanIdentity !=
            localShadowAtlasPlan->GetLocalShadowPlanIdentity() ||
        localShadows->atlasPlanIdentity !=
            localShadowAtlasPlan->GetIdentityHash() ||
        localShadows->atlasGeneration !=
            localShadowAtlasPlan->GetAtlasGeneration() ||
        localShadows->requested !=
            !localShadowAtlasPlan->GetPages().empty())
    {
        SetCompositionFailure(
            "Forward special-surface chain requires the exact LocalShadowOutputs v1");
        return;
    }
    if (m_recordContext.results == nullptr)
    {
        SetCompositionFailure(
            "Forward special-surface chain requires graph-owned recording results");
        return;
    }

    RenderPassRecordContext postLightingContext = m_recordContext;
    postLightingContext.view.colorTarget =
        volumetricLighting->resolvedSceneColor;
    postLightingContext.volumetricLighting = VolumetricLightingRecordInput{
        postLightingContext.identity,
        volumetricLighting->volumetricPlanIdentity,
        volumetricLighting->froxelGrid,
        volumetricLighting->integratedScatteringTransmittance,
        volumetricLighting->identityRoute};
    postLightingContext.directionalShadow =
        postLightingContext.results->directionalShadowOutput;
    if (postLightingContext.directionalShadow.identity !=
        postLightingContext.identity)
    {
        postLightingContext.directionalShadow = {};
        postLightingContext.directionalShadow.identity =
            postLightingContext.identity;
    }
    postLightingContext.rayTracedShadow =
        postLightingContext.results->rayTracedShadowOutput;
    if (postLightingContext.rayTracedShadow.identity !=
        postLightingContext.identity)
    {
        postLightingContext.rayTracedShadow = {};
        postLightingContext.rayTracedShadow.identity =
            postLightingContext.identity;
    }
    // AO is an opaque-lighting input only. Later surfaces retain shadows and
    // the resolved HDR scene, but never inherit the opaque AO field.
    postLightingContext.ambientOcclusion = {};

    m_lastPassChainStats.frameSequence =
        m_recordContext.identity.frameSequence;
    m_lastPassChainStats.passStatuses =
        m_passBundle->GetRegistry().GetPassStatuses();
    m_lastPassChainStats.registeredPassCount =
        m_lastPassChainStats.passStatuses.size();

    RHITextureDesc opaqueSnapshotDesc = RHITextureDesc::Texture2D(
        m_preparedContext.outputExtent.width,
        m_preparedContext.outputExtent.height,
        RHIFormat::RGBA16_FLOAT,
        RHITextureUsage::ShaderResource | RHITextureUsage::CopyDst);
    opaqueSnapshotDesc.debugName = "ForwardOpaqueSceneSnapshot";
    const RGTextureHandle opaqueSceneSnapshot =
        builder.CreateTexture(opaqueSnapshotDesc);
    if (!opaqueSceneSnapshot.IsValid() ||
        opaqueSceneSnapshot.index ==
            postLightingContext.view.colorTarget.index)
    {
        SetCompositionFailure(
            "Forward special-surface chain could not allocate a distinct HDR opaque snapshot");
        return;
    }
    const std::shared_ptr<OpaqueSceneSnapshotCopyReceipt>
        opaqueSnapshotReceipt =
            std::make_shared<OpaqueSceneSnapshotCopyReceipt>();
    bool opaqueSnapshotRegistered = false;

    RGTextureHandle activeSceneColor =
        volumetricLighting->resolvedSceneColor;
    WaterSurfaceRecordInput waterSurface;

    const int32 firstLightingConsumerPriority = opaquePass->GetPriority();
    for (const std::unique_ptr<IRenderPass>& pass :
         m_passBundle->GetRegistry().GetPasses())
    {
        if (!pass || pass.get() == opaquePass ||
            pass.get() == m_passBundle->FindPass<CameraVelocityPass>() ||
            pass.get() == m_passBundle->FindPass<ObjectVelocityPass>() ||
            pass->GetPriority() <= firstLightingConsumerPriority ||
            pass->GetPriority() >= PassPriority::Transparent ||
            IsHybridRayTracingReflectionPass(*pass))
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

        RenderPassRecordContext passContext = postLightingContext;
        passContext.view.colorTarget = activeSceneColor;
        if (!opaqueSnapshotRegistered &&
            pass->GetPriority() >= PassPriority::Water)
        {
            if (!AddOpaqueSceneSnapshotCopy(
                    builder,
                    activeSceneColor,
                    opaqueSceneSnapshot,
                    opaqueSnapshotReceipt))
            {
                SetCompositionFailure(
                    "Forward special-surface chain could not register the HDR opaque snapshot copy");
                return;
            }
            ++m_lastPassChainStats.graphPassCount;
            m_lastPassChainStats.graphPassNames.push_back(
                "OpaqueSceneSnapshotCopy");
            opaqueSnapshotRegistered = true;
            passContext.opaqueSceneSnapshot = OpaqueSceneSnapshotRecordInput{
                postLightingContext.identity,
                opaqueSceneSnapshot,
                opaqueSnapshotReceipt};
            std::string waterSurfaceFailure;
            if (!BuildWaterSurfaceRecordInput(
                    builder,
                    passContext,
                    *m_passBundle,
                    m_preparedContext.outputExtent,
                    waterSurface,
                    waterSurfaceFailure))
            {
                SetCompositionFailure(std::move(waterSurfaceFailure));
                return;
            }
        }
        if (opaqueSnapshotRegistered)
        {
            passContext.opaqueSceneSnapshot = OpaqueSceneSnapshotRecordInput{
                postLightingContext.identity,
                opaqueSceneSnapshot,
                opaqueSnapshotReceipt};
        }
        passContext.waterSurface = waterSurface;
        if (!builder.AddLegacyPass(*pass, passContext))
        {
            SetCompositionFailure(
                "Forward special-surface pass registration failed for '" +
                status.name + "'");
            return;
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
            SetCompositionFailure(
                "Forward special-surface chain could not register the HDR opaque snapshot copy");
            return;
        }
        ++m_lastPassChainStats.graphPassCount;
        m_lastPassChainStats.graphPassNames.push_back(
            "OpaqueSceneSnapshotCopy");
    }

    PreTranslucencyOutputs outputs;
    outputs.provenance = builder.GetProvenance();
    outputs.frameIdentity = m_preparedContext.frameIdentity;
    outputs.viewIdentity = m_preparedContext.viewIdentity;
    outputs.lightingPlanIdentity = expectedLightingPlanIdentity;
    outputs.hdrSceneColor = activeSceneColor;
    outputs.hdrSceneColorMetadata =
        volumetricLighting->resolvedSceneColorMetadata;
    outputs.deviceDepth = m_recordContext.view.depthTarget;
    outputs.deviceDepthMetadata = MakeMetadata(
        m_preparedContext.outputExtent,
        SceneTextures::DeviceDepthContract);
    outputs.worldNormal = volumetricLighting->worldNormal;
    outputs.worldNormalMetadata = volumetricLighting->worldNormalMetadata;
    outputs.materialIdentity = volumetricLighting->materialIdentity;
    outputs.materialIdentityMetadata =
        volumetricLighting->materialIdentityMetadata;
    outputs.materialParameterTable =
        volumetricLighting->materialParameterTable;
    outputs.materialParameterTableMetadata =
        volumetricLighting->materialParameterTableMetadata;
    outputs.velocity = m_recordContext.view.velocityTarget;
    outputs.velocityMetadata = MakeMetadata(
        m_preparedContext.outputExtent, SceneTextures::VelocityContract);
    outputs.reactiveMask = m_recordContext.view.reactiveMaskTarget;
    outputs.reactiveMaskMetadata = MakeMetadata(
        m_preparedContext.outputExtent, SceneTextures::ReactiveMaskContract);
    outputs.opaqueSceneSnapshot = opaqueSceneSnapshot;
    outputs.opaqueSceneSnapshotMetadata = MakeMetadata(
        m_preparedContext.outputExtent, SceneTextures::HDRSceneColorContract);
    outputs.opaqueSceneSnapshotReceipt = opaqueSnapshotReceipt;
    outputs.exposureState = m_recordContext.view.exposureState;
    outputs.exposureStateMetadata = MakeExposureStateMetadata(
        m_recordContext.view);
    outputs.volumetricPlanIdentity =
        volumetricLighting->volumetricPlanIdentity;
    outputs.froxelGrid = volumetricLighting->froxelGrid;
    outputs.volumetricHistoryIdentity =
        volumetricLighting->historyIdentity;
    outputs.volumetricHistoryResetReasons =
        volumetricLighting->historyResetReasons;
    outputs.integratedScatteringTransmittance =
        volumetricLighting->integratedScatteringTransmittance;
    outputs.integratedScatteringTransmittanceMetadata =
        volumetricLighting->integratedScatteringTransmittanceMetadata;
    outputs.volumetricIdentityRoute =
        volumetricLighting->identityRoute;
    outputs.volumetricTemporalHistoryUsed =
        volumetricLighting->temporalHistoryUsed;
    outputs.colorDomain = PostProcessColorDomain::SceneLinearHDR;
    outputs.transfer = PostProcessTransfer::None;
    if (blackboard.Publish(BuiltinRenderFeatureIds::ForwardSpecialChain,
                           Describe(), std::move(outputs)) !=
        RenderGraphBlackboardCode::Success)
    {
        SetCompositionFailure(
            "Forward special-surface chain could not publish PreTranslucencyOutputs v2");
    }
}

RenderFeatureExecutionReceipt
ForwardSpecialFeatureAdapter::CollectReceipt() const
{
    RenderFeatureExecutionReceipt receipt;
    receipt.featureId = BuiltinRenderFeatureIds::ForwardSpecialChain;
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

bool ForwardSpecialFeatureAdapter::HasMatchingRecordContext(
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

bool ForwardSpecialFeatureAdapter::HasValidLightingPlan(
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

void ForwardSpecialFeatureAdapter::SetCompositionFailure(
    std::string diagnostic)
{
    m_readiness.inputsReady = false;
    m_readiness.executed = false;
    m_readiness.diagnostic = std::move(diagnostic);
}
} // namespace RVX
