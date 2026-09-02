/**
 * @file DeferredRecipeFeatureAdapter.cpp
 * @brief DeferredQuality composition over graph-owned GBuffer resources.
 */

#include "Features/DeferredRecipeFeatureAdapter.h"

#include "Features/DefaultRenderPassBundle.h"
#include "Features/OpaqueSceneColorPyramidBuilder.h"
#include "Features/SceneDepthSnapshotBuilder.h"

#include "Render/Material/MaterialSystem.h"
#include "Render/Passes/CameraVelocityPass.h"
#include "Render/Passes/DeferredLightingPass.h"
#include "Render/Passes/OpaquePass.h"
#include "Render/Passes/ObjectVelocityPass.h"
#include "Render/Passes/ParticleFeaturePass.h"
#include "Render/Passes/RayTracedReflectionCompositePass.h"
#include "Render/Passes/RayTracedReflectionDenoisePass.h"
#include "Render/Passes/RayTracedReflectionPass.h"
#include "Render/Passes/RayTracedShadowPass.h"
#include "Render/Passes/ShadowPass.h"
#include "Render/Passes/WaterFeaturePass.h"
#include "Render/PipelineCache.h"

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

    [[nodiscard]] bool IsSelectedDirectionalShadowPass(
        const IRenderPass& pass,
        RenderPipelineRecipe resolvedRecipe) noexcept
    {
        if (dynamic_cast<const RayTracedShadowPass*>(&pass) != nullptr)
        {
            return resolvedRecipe == RenderPipelineRecipe::HybridRayTracing;
        }
        if (dynamic_cast<const ShadowPass*>(&pass) != nullptr)
        {
            return resolvedRecipe != RenderPipelineRecipe::HybridRayTracing;
        }
        return true;
    }

    [[nodiscard]] const WaterFeaturePass* FindWaterFeaturePass(
        const DefaultRenderPassBundle& bundle) noexcept
    {
        return bundle.FindPass<WaterFeaturePass>();
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
        const WaterFeaturePass* const water = FindWaterFeaturePass(bundle);
        if (water == nullptr || !water->GetRequirements().requested)
        {
            return true;
        }
        const WaterSurfaceRequirements requirements = water->GetRequirements();
        if (!requirements.sceneColorPyramidRequired ||
            !context.opaqueSceneColorPyramid.IsCompatibleWith(context.identity))
        {
            outFailure =
                "Water requires the current graph's completed opaque-scene color pyramid";
            return false;
        }
        if (!requirements.exposureStateRequired ||
            !context.view.exposureState.IsValid() ||
            !HasCurrentGraphProvenance(
                context.view.exposureState, context.identity) ||
            context.view.exposureStateContentHash == 0)
        {
            outFailure =
                "Water requires the exact committed exposure state paired with the opaque-scene color pyramid";
            return false;
        }
        const RHITextureDesc* const depthSnapshotDesc =
            builder.GetGraph().GetTextureDesc(context.sceneDepthSnapshot.texture);
        const bool validDepthSnapshot =
            context.sceneDepthSnapshot.IsCompatibleWith(context.identity, extent) &&
            depthSnapshotDesc != nullptr &&
            depthSnapshotDesc->format == RHIFormat::D32_FLOAT &&
            depthSnapshotDesc->width == extent.width &&
            depthSnapshotDesc->height == extent.height &&
            depthSnapshotDesc->sampleCount == RHISampleCount::Count1 &&
            HasFlag(depthSnapshotDesc->usage, RHITextureUsage::ShaderResource);
        if (!validDepthSnapshot)
        {
            outFailure =
                "Water requires the current SceneDepthSnapshot D32 surface at the exact view extent";
            return false;
        }
        outInput.identity = context.identity;
        outInput.sceneColorPyramid = context.opaqueSceneColorPyramid;
        outInput.exposureState = context.view.exposureState;
        outInput.exposureStateContentHash = context.view.exposureStateContentHash;
        outInput.depthSnapshot = context.sceneDepthSnapshot;
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

    [[nodiscard]] bool UsesDeferredOpaqueComposition(
        const RenderFeatureContext& context) noexcept
    {
        return context.pipeline.recipe == context.resolvedRecipe &&
               UsesDeferredOpaquePath(context.resolvedRecipe);
    }

    [[nodiscard]] bool IsNativeDeferredBackend(RHIBackendType backend) noexcept
    {
        switch (backend)
        {
            case RHIBackendType::DX12:
            case RHIBackendType::Vulkan:
            case RHIBackendType::Metal: return true;
            case RHIBackendType::None:
            case RHIBackendType::Auto:
            case RHIBackendType::DX11:
            case RHIBackendType::OpenGL: return false;
            default: return false;
        }
    }

    [[nodiscard]] bool IsValidDeferredTexture(
        const RenderGraph& graph,
        RGTextureHandle handle,
        RenderOutputExtent extent,
        SceneTextures::FormatContract contract,
        RHITextureUsage requiredUsage) noexcept
    {
        const RHITextureDesc* const desc = graph.GetTextureDesc(handle);
        return handle.IsValid() && desc != nullptr &&
               desc->width == extent.width && desc->height == extent.height &&
               desc->depth == 1 && desc->mipLevels == 1 &&
               desc->arraySize == 1 && desc->dimension == RHITextureDimension::Texture2D &&
               desc->format == contract.format &&
               desc->sampleCount == contract.sampleCount &&
               HasFlag(desc->usage, requiredUsage);
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

    [[nodiscard]] bool HasDeferredOpaqueRouteInputs(
        const std::vector<RenderDrawItem>* drawItems) noexcept
    {
        if (drawItems == nullptr)
        {
            return true;
        }
        for (const RenderDrawItem& item : *drawItems)
        {
            const RenderDrawPacket& packet = item.packet;
            if (packet.pass != RenderPassKind::Opaque ||
                packet.pipelineKey.materialVariant ==
                    MaterialPipelineVariant::Transparent ||
                !packet.pipelineKey.materialPolicyValid ||
                (packet.pipelineKey.surfacePathEligibility !=
                     SurfacePathEligibility::DeferredOpaque &&
                 packet.pipelineKey.surfacePathEligibility !=
                     SurfacePathEligibility::ForwardSpecialOpaque))
            {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] RenderPassRecordContext MakeOpaqueRecordContext(
        const RenderPassRecordContext& source,
        bool& outValid) noexcept
    {
        RenderPassRecordContext result = source;
        outValid = result.results != nullptr;
        if (!outValid)
        {
            return result;
        }

        result.directionalShadow = result.results->directionalShadowOutput;
        if (result.directionalShadow.identity != result.identity)
        {
            result.directionalShadow = {};
            result.directionalShadow.identity = result.identity;
        }
        result.rayTracedShadow = result.results->rayTracedShadowOutput;
        if (result.rayTracedShadow.identity != result.identity)
        {
            result.rayTracedShadow = {};
            result.rayTracedShadow.identity = result.identity;
        }
        return result;
    }

    template<typename IncludePredicate>
    [[nodiscard]] bool AddLegacyPassRange(
        RenderFeatureGraphBuilder& builder,
        RenderPassRecordContext context,
        DefaultRenderPassBundle& bundle,
        const OpaquePass* opaquePass,
        IncludePredicate include,
        DeferredRecipeFeaturePassChainStats& stats,
        std::string& outFailure,
        RGTextureHandle* outFinalSceneColor = nullptr)
    {
        stats.frameSequence = context.identity.frameSequence;
        auto& registry = bundle.GetRegistry();
        stats.passStatuses = registry.GetPassStatuses();
        stats.registeredPassCount = stats.passStatuses.size();

        for (const std::unique_ptr<IRenderPass>& pass : registry.GetPasses())
        {
            if (!pass || pass.get() == opaquePass || !include(*pass))
            {
                continue;
            }

            const RenderPassStatus status = pass->GetStatus();
            if (!status.requestedEnabled)
            {
                ++stats.skippedDisabledPassCount;
                continue;
            }
            if (!status.supported)
            {
                ++stats.skippedUnsupportedPassCount;
                stats.unsupportedPassNames.push_back(status.name);
                continue;
            }
            if (!status.enabled)
            {
                ++stats.skippedDisabledPassCount;
                continue;
            }
            if (!builder.AddLegacyPass(*pass, context))
            {
                outFailure = "Deferred recipe legacy pass registration failed for '" +
                             status.name + "'";
                return false;
            }
            if (const auto* const particle =
                    dynamic_cast<const ParticleFeaturePass*>(pass.get());
                particle != nullptr &&
                context.particleVfx.compositedSceneColor.IsValid() &&
                particle->HasRegisteredDistortionComposite())
            {
                context.view.colorTarget =
                    context.particleVfx.compositedSceneColor;
            }
            ++stats.graphPassCount;
            stats.graphPassNames.push_back(status.name);
        }
        if (outFinalSceneColor != nullptr)
        {
            *outFinalSceneColor = context.view.colorTarget;
        }
        return true;
    }

    [[nodiscard]] SceneTextures::Metadata MakeMetadata(
        RenderOutputExtent extent,
        SceneTextures::FormatContract contract) noexcept
    {
        return SceneTextures::Metadata{extent, contract.format,
                                       contract.sampleCount};
    }

    [[nodiscard]] RHITextureDesc MakeGBufferDesc(
        RenderOutputExtent extent,
        SceneTextures::FormatContract contract,
        const char* name)
    {
        RHITextureDesc desc = RHITextureDesc::RenderTarget(
            extent.width, extent.height, contract.format);
        desc.sampleCount = contract.sampleCount;
        desc.debugName = name;
        return desc;
    }

    [[nodiscard]] bool AddCanonicalVelocityPasses(
        RenderFeatureGraphBuilder& builder,
        const RenderPassRecordContext& context,
        DefaultRenderPassBundle& bundle,
        RenderOutputExtent extent,
        DeferredRecipeFeaturePassChainStats& stats,
        std::string& outFailure)
    {
        const bool targetAvailable = IsValidDeferredTexture(
            builder.GetGraph(), context.view.velocityTarget, extent,
            SceneTextures::VelocityContract,
            RHITextureUsage::ShaderResource) &&
            HasFlag(builder.GetGraph()
                        .GetTextureDesc(context.view.velocityTarget)->usage,
                    RHITextureUsage::RenderTarget);
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
            const RenderPassStatus status = producers[index]->GetStatus();
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

        // Validate the whole pair before recording either producer so a
        // failed Object pass cannot leave a partial velocity chain behind.
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

DeferredGeometryFeatureAdapter::DeferredGeometryFeatureAdapter(
    DefaultRenderPassBundle* passBundle,
    PipelineCache* pipelineCache) noexcept
    : m_passBundle(passBundle)
    , m_pipelineCache(pipelineCache)
{
}

void DeferredGeometryFeatureAdapter::SetRecordContext(
    RenderPassRecordContext context)
{
    m_recordContext = std::move(context);
    m_hasRecordContext = true;
    m_preparedContext = {};
    m_readiness = {};
    m_lastPassChainStats = {};
    m_lastReceipt = {};
}

RenderFeatureDescriptor DeferredGeometryFeatureAdapter::Describe() const
{
    RenderFeatureDescriptor descriptor;
    descriptor.featureId = BuiltinRenderFeatureIds::DeferredGeometry;
    descriptor.schemaVersion = 1;
    descriptor.name = "DeferredGeometry";
    descriptor.phase = RenderFeaturePhase::Geometry;
    descriptor.producedResources = {
        {RenderBlackboardResourceId::SceneTextures, SceneTextures::SchemaVersion},
        {RenderBlackboardResourceId::LocalShadowOutputs,
         LocalShadowOutputs::SchemaVersion}};
    descriptor.requiredCapabilities =
        ToMask(RenderFeatureCapability::Graphics) |
        ToMask(RenderFeatureCapability::MultipleRenderTargets);
    descriptor.requiredRecipes =
        ToMask(RenderPipelineRecipe::DeferredQuality) |
        ToMask(RenderPipelineRecipe::HybridRayTracing);
    descriptor.qualityTiers = RVX_ALL_RENDER_QUALITY_TIERS;
    descriptor.fallbackPolicy = RenderFeatureFallbackPolicy::None;
    descriptor.shaderPipelineFamily = "DeferredGBuffer";
    return descriptor;
}

void DeferredGeometryFeatureAdapter::Prepare(const RenderFeatureContext& context)
{
    m_preparedContext = context;
    m_readiness = {};
    m_readiness.compiled = true;
    m_readiness.hardwareSupported = HasGraphicsCapability(
        context.hardwareCapabilities) && IsNativeDeferredBackend(context.backend);
    m_readiness.implementationSupported = UsesDeferredOpaqueComposition(context);
    OpaquePass* const opaquePass = m_passBundle != nullptr
        ? m_passBundle->FindPass<OpaquePass>()
        : nullptr;
    OpaqueRasterPipelinePreflightResult opaquePipelinePreflight;
    if (m_readiness.hardwareSupported &&
        m_readiness.implementationSupported && opaquePass != nullptr &&
        m_pipelineCache != nullptr)
    {
        // A DeferredQuality frame may be selected only after the shared
        // opaque recorder has resolved every actual frozen DeferredOpaque
        // route request. This deliberately does not synthesize a rigid
        // baseline: masked, skinned, and sealed GPU variants are all
        // preflighted from the current frame plan before graph registration.
        opaquePipelinePreflight = opaquePass->PreflightRasterPipelines(
            m_recordContext,
            OpaqueRasterOutputFamily::DeferredGBuffer,
            OpaqueRasterSurfaceRoute::DeferredOpaque);
    }
    m_readiness.pipelineReady = opaquePipelinePreflight.Succeeded();
    m_readiness.inputsReady = HasMatchingRecordContext(context) &&
        HasDeferredTargets() && opaquePass != nullptr &&
        m_passBundle != nullptr;

    if (!m_readiness.hardwareSupported)
    {
        m_readiness.diagnostic =
            "DeferredQuality requires the DX12, Vulkan, or Metal renderer path";
    }
    else if (!m_readiness.implementationSupported)
    {
        m_readiness.diagnostic =
            "Deferred geometry requires a deferred-opaque recipe";
    }
    else if (opaquePass == nullptr || m_passBundle == nullptr ||
             m_pipelineCache == nullptr)
    {
        m_readiness.diagnostic =
            "Deferred geometry composition dependencies are unavailable";
    }
    else if (!m_readiness.pipelineReady)
    {
        m_readiness.diagnostic = opaquePipelinePreflight.diagnostic.empty()
            ? "Deferred opaque frozen-plan pipeline preflight failed"
            : std::move(opaquePipelinePreflight.diagnostic);
    }
    else if (!HasMatchingRecordContext(context))
    {
        m_readiness.diagnostic =
            "Deferred geometry record context is absent, stale, or mismatched";
    }
    else if (!HasDeferredTargets())
    {
        m_readiness.diagnostic =
            "DeferredQuality requires RGBA16F HDR and D32 depth targets";
    }
    else if (!HasDeferredOpaqueRouteInputs(m_recordContext.opaqueDrawItems) ||
             !HasDeferredOpaqueRouteInputs(m_recordContext.maskedDrawItems))
    {
        m_readiness.inputsReady = false;
        m_readiness.diagnostic =
            "DeferredQuality encountered an unsupported or wrong-list opaque surface";
    }
    m_readiness.executed = false;
}

RenderFeatureReadiness DeferredGeometryFeatureAdapter::ResolveReadiness() const
{
    return m_readiness;
}

void DeferredGeometryFeatureAdapter::AddPasses(
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
    if (!builder.IsValid() || !m_recordContext.MatchesTargetGraph(builder.GetGraph()))
    {
        SetCompositionFailure(
            "Deferred geometry record context does not match the target graph");
        return;
    }
    OpaquePass* const opaquePass = m_passBundle != nullptr
        ? m_passBundle->FindPass<OpaquePass>()
        : nullptr;
    if (opaquePass == nullptr)
    {
        SetCompositionFailure("Deferred geometry opaque pass is unavailable");
        return;
    }

    std::string failure;
    if (!AddLegacyPassRange(
            builder,
            m_recordContext,
            *m_passBundle,
            opaquePass,
            [resolvedRecipe = m_preparedContext.resolvedRecipe](
                const IRenderPass& pass)
            {
                return pass.GetPriority() < PassPriority::Opaque &&
                       IsSelectedDirectionalShadowPass(pass, resolvedRecipe);
            },
            m_lastPassChainStats,
            failure))
    {
        SetCompositionFailure(std::move(failure));
        return;
    }

    const RenderOutputExtent extent = m_preparedContext.outputExtent;
    SceneTextures textures;
    textures.provenance = builder.GetProvenance();
    textures.hdrSceneColor = m_recordContext.view.colorTarget;
    textures.hdrSceneColorMetadata = MakeMetadata(
        extent, SceneTextures::HDRSceneColorContract);
    textures.deviceDepth = m_recordContext.view.depthTarget;
    textures.deviceDepthMetadata = MakeMetadata(
        extent, SceneTextures::DeviceDepthContract);
    textures.exposureState = m_recordContext.view.exposureState;
    textures.exposureStateMetadata = MakeExposureStateMetadata(
        m_recordContext.view);
    if (m_recordContext.view.velocityTarget.IsValid())
    {
        textures.velocity = m_recordContext.view.velocityTarget;
        textures.velocityMetadata = MakeMetadata(
            extent, SceneTextures::VelocityContract);
    }
    textures.gBuffer0 = builder.CreateTexture(MakeGBufferDesc(
        extent, SceneTextures::GBuffer0Contract, "DeferredGBuffer0"));
    textures.gBuffer0Metadata = MakeMetadata(extent, SceneTextures::GBuffer0Contract);
    textures.gBuffer1 = builder.CreateTexture(MakeGBufferDesc(
        extent, SceneTextures::GBuffer1Contract, "DeferredGBuffer1"));
    textures.gBuffer1Metadata = MakeMetadata(extent, SceneTextures::GBuffer1Contract);
    textures.gBuffer2 = builder.CreateTexture(MakeGBufferDesc(
        extent, SceneTextures::GBuffer2Contract, "DeferredGBuffer2"));
    textures.gBuffer2Metadata = MakeMetadata(extent, SceneTextures::GBuffer2Contract);
    textures.gBuffer3 = builder.CreateTexture(MakeGBufferDesc(
        extent, SceneTextures::GBuffer3Contract, "DeferredGBuffer3"));
    textures.gBuffer3Metadata = MakeMetadata(extent, SceneTextures::GBuffer3Contract);

    // GFX14 publishes the exact stable-slot material ABI consumed by both
    // deferred lighting and the screen-space Toon outline. It is initialized
    // before graph import so GBuffer3 can never address undefined rows.
    MaterialParameterTableSnapshot materialTable;
    std::string materialTableError;
    if (!opaquePass->CreateMaterialClassificationTableSnapshot(
            m_recordContext,
            OpaqueRasterSurfaceRoute::DeferredOpaque,
            materialTable,
            materialTableError))
    {
        SetCompositionFailure(materialTableError.empty()
            ? "Deferred geometry could not create its immutable material table"
            : std::move(materialTableError));
        return;
    }
    textures.materialTableMetadata = PostProcessMaterialTableMetadata{
        PostProcessMaterialTableMetadata::SchemaVersion,
        materialTable.stride,
        materialTable.slotCount,
        CountEligibleToonOutlineMaterials(materialTable),
        materialTable.contentHash,
        extent,
        SceneTextures::GBuffer3Contract.sampleCount};
    textures.materialTable = builder.ImportBuffer(
        std::move(materialTable.buffer), RHIResourceState::ShaderResource);

    if (ValidateDeferredSceneTextures(textures) !=
        DeferredSceneTexturesValidationCode::Success)
    {
        SetCompositionFailure("Deferred scene texture contract is invalid");
        return;
    }

    bool opaqueContextValid = false;
    RenderPassRecordContext opaqueContext = MakeOpaqueRecordContext(
        m_recordContext, opaqueContextValid);
    opaqueContext.opaqueMaterialTable = OpaqueMaterialTableExecutionIdentity{
        1,
        materialTable.schemaVersion,
        materialTable.stride,
        materialTable.slotCount,
        materialTable.contentHash};
    if (!opaqueContextValid ||
        !builder.AddGraphCallback(
            "DeferredGBuffer",
            opaqueContext,
            [opaque = opaquePass,
             context = opaqueContext,
             targets = OpaqueDeferredGBufferTargets{
                 {textures.gBuffer0, textures.gBuffer1, textures.gBuffer2,
                  textures.gBuffer3},
                 textures.deviceDepth}](RenderGraph& graph)
            {
                opaque->AddDeferredToGraph(graph, context, targets);
            }))
    {
        SetCompositionFailure(
            "Deferred GBuffer registration failed for the shared opaque submission");
        return;
    }
    ++m_lastPassChainStats.graphPassCount;
    m_lastPassChainStats.graphPassNames.push_back("DeferredGBuffer");

    if (!AddCanonicalVelocityPasses(
            builder, m_recordContext, *m_passBundle, extent,
            m_lastPassChainStats, failure))
    {
        SetCompositionFailure(std::move(failure));
        return;
    }

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
            BuiltinRenderFeatureIds::DeferredGeometry,
            Describe(), std::move(localShadows)) !=
        RenderGraphBlackboardCode::Success)
    {
        SetCompositionFailure(
            "Deferred geometry could not publish LocalShadowOutputs v1");
        return;
    }

    if (blackboard.Publish(BuiltinRenderFeatureIds::DeferredGeometry,
                           Describe(), std::move(textures)) !=
        RenderGraphBlackboardCode::Success)
    {
        SetCompositionFailure("Deferred geometry could not publish SceneTextures");
    }
}

RenderFeatureExecutionReceipt
DeferredGeometryFeatureAdapter::CollectReceipt() const
{
    RenderFeatureExecutionReceipt receipt;
    receipt.featureId = BuiltinRenderFeatureIds::DeferredGeometry;
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

bool DeferredGeometryFeatureAdapter::HasMatchingRecordContext(
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

bool DeferredGeometryFeatureAdapter::HasDeferredTargets() const noexcept
{
    if (m_recordContext.identity.graph == nullptr)
    {
        return false;
    }
    const RenderGraph& graph = *m_recordContext.identity.graph;
    const RenderOutputExtent extent = m_preparedContext.outputExtent;
    return IsValidDeferredTexture(
               graph,
               m_recordContext.view.colorTarget,
               extent,
               SceneTextures::HDRSceneColorContract,
               RHITextureUsage::UnorderedAccess) &&
           IsValidDeferredTexture(
               graph,
               m_recordContext.view.depthTarget,
               extent,
               SceneTextures::DeviceDepthContract,
               RHITextureUsage::DepthStencil);
}

void DeferredGeometryFeatureAdapter::SetCompositionFailure(std::string diagnostic)
{
    m_readiness.inputsReady = false;
    m_readiness.executed = false;
    m_readiness.diagnostic = std::move(diagnostic);
}

DeferredLightingFeatureAdapter::DeferredLightingFeatureAdapter(
    DefaultRenderPassBundle* passBundle,
    PipelineCache* pipelineCache) noexcept
    : m_passBundle(passBundle)
    , m_pipelineCache(pipelineCache)
{
}

void DeferredLightingFeatureAdapter::SetRecordContext(
    RenderPassRecordContext context)
{
    m_recordContext = std::move(context);
    m_hasRecordContext = true;
    m_preparedContext = {};
    m_readiness = {};
    m_lastReceipt = {};
}

RenderFeatureDescriptor DeferredLightingFeatureAdapter::Describe() const
{
    RenderFeatureDescriptor descriptor;
    descriptor.featureId = BuiltinRenderFeatureIds::DeferredLighting;
    descriptor.schemaVersion = 1;
    descriptor.name = "DeferredLighting";
    descriptor.phase = RenderFeaturePhase::Lighting;
    descriptor.consumedResources = {
        {RenderBlackboardResourceId::SceneTextures,
         SceneTextures::SchemaVersion},
        {RenderBlackboardResourceId::AmbientOcclusionOutputs,
         AmbientOcclusionOutputs::SchemaVersion},
        {RenderBlackboardResourceId::LocalShadowOutputs,
         LocalShadowOutputs::SchemaVersion}};
    descriptor.producedResources = {
        {RenderBlackboardResourceId::LightingOutputs,
         LightingOutputs::SchemaVersion}};
    descriptor.requiredCapabilities =
        ToMask(RenderFeatureCapability::Graphics) |
        ToMask(RenderFeatureCapability::Compute) |
        ToMask(RenderFeatureCapability::DescriptorBindings) |
        ToMask(RenderFeatureCapability::StorageTexture);
    descriptor.requiredRecipes =
        ToMask(RenderPipelineRecipe::DeferredQuality) |
        ToMask(RenderPipelineRecipe::HybridRayTracing);
    descriptor.qualityTiers = RVX_ALL_RENDER_QUALITY_TIERS;
    descriptor.fallbackPolicy = RenderFeatureFallbackPolicy::None;
    descriptor.shaderPipelineFamily = "DeferredLighting";
    return descriptor;
}

void DeferredLightingFeatureAdapter::Prepare(const RenderFeatureContext& context)
{
    m_preparedContext = context;
    m_readiness = {};
    m_readiness.compiled = true;
    m_readiness.hardwareSupported = HasGraphicsCapability(
        context.hardwareCapabilities) && IsNativeDeferredBackend(context.backend);
    m_readiness.implementationSupported = UsesDeferredOpaqueComposition(context);
    const bool hasValidLightingPlan = HasValidLightingPlan(context);
    DeferredLightingPass* const lightingPass = m_passBundle != nullptr
        ? m_passBundle->FindPass<DeferredLightingPass>()
        : nullptr;
    const bool createdPipeline = hasValidLightingPlan && lightingPass != nullptr &&
        lightingPass->EnsurePipelineReady();
    m_readiness.pipelineReady = createdPipeline && m_pipelineCache != nullptr &&
        m_pipelineCache->IsDeferredLightingPipelineReady();
    m_readiness.inputsReady = HasMatchingRecordContext(context) &&
        hasValidLightingPlan && lightingPass != nullptr;

    if (!m_readiness.hardwareSupported)
    {
        m_readiness.diagnostic =
            "DeferredQuality requires the DX12, Vulkan, or Metal renderer path";
    }
    else if (!m_readiness.implementationSupported)
    {
        m_readiness.diagnostic =
            "Deferred lighting requires a deferred-opaque recipe";
    }
    else if (!hasValidLightingPlan)
    {
        m_readiness.diagnostic =
            "Deferred lighting requires a valid LightingFramePlan for the current frame and view";
    }
    else if (lightingPass == nullptr || m_pipelineCache == nullptr)
    {
        m_readiness.diagnostic = "Deferred lighting composition dependencies are unavailable";
    }
    else if (!m_readiness.pipelineReady)
    {
        m_readiness.diagnostic = lightingPass->GetUnsupportedReason();
    }
    else if (!lightingPass->IsSupported())
    {
        m_readiness.pipelineReady = false;
        m_readiness.diagnostic = lightingPass->GetUnsupportedReason();
    }
    else if (!HasMatchingRecordContext(context))
    {
        m_readiness.diagnostic =
            "Deferred lighting record context is absent, stale, or mismatched";
    }
    m_readiness.executed = false;
}

RenderFeatureReadiness DeferredLightingFeatureAdapter::ResolveReadiness() const
{
    return m_readiness;
}

void DeferredLightingFeatureAdapter::AddPasses(
    RenderFeatureGraphBuilder& builder,
    RenderGraphBlackboard& blackboard)
{
    m_lastReceipt = {};
    m_readiness.executed = false;
    if (!m_readiness.IsReadyToExecute())
    {
        return;
    }
    DeferredLightingPass* const lightingPass = m_passBundle != nullptr
        ? m_passBundle->FindPass<DeferredLightingPass>()
        : nullptr;
    if (lightingPass == nullptr)
    {
        SetCompositionFailure("Deferred lighting pass is unavailable");
        return;
    }
    if (!HasValidLightingPlan(m_preparedContext))
    {
        SetCompositionFailure(
            "Deferred lighting record context lost its valid LightingFramePlan");
        return;
    }
    RenderGraphBlackboardCode code = RenderGraphBlackboardCode::Success;
    const SceneTextures* const sceneTextures = blackboard.Require<SceneTextures>(
        BuiltinRenderFeatureIds::DeferredLighting, Describe(), &code);
    if (sceneTextures == nullptr || code != RenderGraphBlackboardCode::Success ||
        ValidateDeferredSceneTextures(*sceneTextures) !=
            DeferredSceneTexturesValidationCode::Success)
    {
        SetCompositionFailure("Deferred lighting requires a valid SceneTextures v5 entry");
        return;
    }
    const AmbientOcclusionOutputs* const ambientOcclusion =
        blackboard.Require<AmbientOcclusionOutputs>(
            BuiltinRenderFeatureIds::DeferredLighting, Describe(), &code);
    if (ambientOcclusion == nullptr ||
        code != RenderGraphBlackboardCode::Success ||
        !ambientOcclusion->ambientOcclusion.IsValid() ||
        ambientOcclusion->frameIdentity != m_preparedContext.frameIdentity ||
        ambientOcclusion->viewIdentity != m_preparedContext.viewIdentity ||
        ambientOcclusion->ambientOcclusionMetadata.extent !=
            m_preparedContext.outputExtent ||
        ambientOcclusion->ambientOcclusionMetadata.format !=
            RHIFormat::R8_UNORM ||
        ambientOcclusion->ambientOcclusionMetadata.sampleCount !=
            RHISampleCount::Count1 ||
        ambientOcclusion->settingsHash == 0 ||
        ambientOcclusion->ambientOcclusionFieldHash == 0 ||
        !IsValidRenderAmbientOcclusionConsumerMask(
            ambientOcclusion->consumerMask))
    {
        SetCompositionFailure(
            "Deferred lighting requires the exact typed ambient-occlusion output");
        return;
    }
    const LocalShadowOutputs* const localShadows =
        blackboard.Require<LocalShadowOutputs>(
            BuiltinRenderFeatureIds::DeferredLighting, Describe(), &code);
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
            !localShadowAtlasPlan->GetPages().empty() ||
        (localShadows->requested &&
         (localShadows->pageCount !=
              localShadowAtlasPlan->GetPages().size() ||
          localShadows->pageResolution !=
              localShadowAtlasPlan->GetTextureContract().pageResolution ||
          localShadowAtlasPlan->GetTextureContract().pageCapacity == 0 ||
          localShadows->pageCount >
              localShadowAtlasPlan->GetTextureContract().pageCapacity)))
    {
        SetCompositionFailure(
            "Deferred lighting requires the exact typed local-shadow output");
        return;
    }
    if (!builder.IsValid() || !m_recordContext.MatchesTargetGraph(builder.GetGraph()))
    {
        SetCompositionFailure(
            "Deferred lighting record context does not match the target graph");
        return;
    }

    const uint64 lightingPlanIdentity =
        m_recordContext.lightingPlan->GetIdentityHash();
    const std::shared_ptr<const RenderLightingFrameResourceSnapshot>&
        lightingResources = m_recordContext.lightingResources;
    if (!lightingResources || !lightingResources->IsComplete() ||
        lightingResources->lightingPlanIdentity != lightingPlanIdentity ||
        !lightingResources->frozenLightResources.IsComplete() ||
        !HasCurrentGraphProvenance(
            lightingResources->lightConstants, m_recordContext.identity) ||
        !HasCurrentGraphProvenance(
            lightingResources->pointLights, m_recordContext.identity) ||
        !HasCurrentGraphProvenance(
            lightingResources->spotLights, m_recordContext.identity) ||
        !HasCurrentGraphProvenance(
            lightingResources->clusterConstants, m_recordContext.identity) ||
        !HasCurrentGraphProvenance(
            lightingResources->clusterData, m_recordContext.identity) ||
        !HasCurrentGraphProvenance(
            lightingResources->clusterLightIndices, m_recordContext.identity))
    {
        SetCompositionFailure(
            "Deferred lighting requires complete frame resources for its LightingFramePlan");
        return;
    }
    if (m_recordContext.results == nullptr)
    {
        SetCompositionFailure(
            "Deferred lighting requires the graph-produced directional-shadow result");
        return;
    }

    const DirectionalShadowRecordOutput& directionalShadow =
        m_recordContext.results->directionalShadowOutput;
    const LightingCSMRequest& csmRequest =
        m_recordContext.lightingPlan->GetCSMRequest();
    if (csmRequest.inputsReady != directionalShadow.enabled ||
        !directionalShadow.IsCompatibleWith(
            m_recordContext.identity, lightingPlanIdentity))
    {
        SetCompositionFailure(
            "Deferred lighting CSM readiness does not match its LightingFramePlan");
        return;
    }

    ViewData lightingView = m_recordContext.view;
    if (!ResolveDirectionalShadowViewData(
            lightingView, directionalShadow, m_recordContext.identity,
            lightingPlanIdentity))
    {
        SetCompositionFailure(
            "Deferred lighting could not resolve its graph-produced CSM view data");
        return;
    }

    const LightingIBLRequest& iblRequest =
        m_recordContext.lightingPlan->GetIBLRequest();
    if (iblRequest.inputsReady != lightingResources->textureIBLEnabled)
    {
        SetCompositionFailure(
            "Deferred lighting texture-IBL readiness does not match its LightingFramePlan");
        return;
    }
    if (lightingResources->textureIBLEnabled &&
        (!HasCurrentGraphProvenance(
             lightingResources->environmentIBLIrradiance,
             m_recordContext.identity) ||
         !HasCurrentGraphProvenance(
             lightingResources->environmentIBLPrefiltered,
             m_recordContext.identity) ||
         !HasCurrentGraphProvenance(
             lightingResources->environmentIBLBRDFLUT,
             m_recordContext.identity)))
    {
        SetCompositionFailure(
            "Deferred lighting texture-IBL graph resources are stale or missing");
        return;
    }

    DeferredLightingInputs inputs;
    inputs.gBuffer0 = sceneTextures->gBuffer0;
    inputs.gBuffer1 = sceneTextures->gBuffer1;
    inputs.gBuffer2 = sceneTextures->gBuffer2;
    inputs.gBuffer3 = sceneTextures->gBuffer3;
    inputs.deviceDepth = sceneTextures->deviceDepth;
    inputs.hdrSceneColor = sceneTextures->hdrSceneColor;
    RHITextureDesc baseIndirectSpecularDesc = RHITextureDesc::RenderTarget(
        m_preparedContext.outputExtent.width,
        m_preparedContext.outputExtent.height,
        SceneTextures::BaseIndirectSpecularContract.format);
    baseIndirectSpecularDesc.sampleCount =
        SceneTextures::BaseIndirectSpecularContract.sampleCount;
    baseIndirectSpecularDesc.usage =
        baseIndirectSpecularDesc.usage | RHITextureUsage::UnorderedAccess;
    baseIndirectSpecularDesc.debugName =
        "DeferredBaseIndirectSpecular";
    inputs.baseIndirectSpecular = builder.CreateTexture(
        baseIndirectSpecularDesc);

    RHITextureDesc specularResponseDesc = RHITextureDesc::RenderTarget(
        m_preparedContext.outputExtent.width,
        m_preparedContext.outputExtent.height,
        SceneTextures::SpecularResponseContract.format);
    specularResponseDesc.sampleCount =
        SceneTextures::SpecularResponseContract.sampleCount;
    specularResponseDesc.usage =
        specularResponseDesc.usage | RHITextureUsage::UnorderedAccess;
    specularResponseDesc.debugName = "DeferredSpecularResponse";
    inputs.specularResponse = builder.CreateTexture(specularResponseDesc);
    inputs.materialTable = sceneTextures->materialTable;
    inputs.materialTableMetadata = sceneTextures->materialTableMetadata;
    inputs.exposureState = sceneTextures->exposureState;
    inputs.ambientOcclusion.identity = m_recordContext.identity;
    inputs.ambientOcclusion.filteredOcclusion =
        ambientOcclusion->ambientOcclusion;
    inputs.ambientOcclusion.extent =
        ambientOcclusion->ambientOcclusionMetadata.extent;
    inputs.ambientOcclusion.format =
        ambientOcclusion->ambientOcclusionMetadata.format;
    inputs.ambientOcclusion.sampleCount =
        ambientOcclusion->ambientOcclusionMetadata.sampleCount;
    inputs.ambientOcclusion.settingsHash = ambientOcclusion->settingsHash;
    inputs.ambientOcclusion.fieldContentHash =
        ambientOcclusion->ambientOcclusionFieldHash;
    inputs.ambientOcclusion.requested = true;
    inputs.expectedLightingPlanIdentity = lightingPlanIdentity;
    inputs.lighting.lightingPlanIdentity = lightingPlanIdentity;
    inputs.lighting.frozenLightResources =
        lightingResources->frozenLightResources;
    inputs.lighting.lightConstants = lightingResources->lightConstants;
    inputs.lighting.pointLights = lightingResources->pointLights;
    inputs.lighting.spotLights = lightingResources->spotLights;
    inputs.lighting.clusterConstants = lightingResources->clusterConstants;
    inputs.lighting.clusterData = lightingResources->clusterData;
    inputs.lighting.clusterLightIndices = lightingResources->clusterLightIndices;
    inputs.lighting.directionalShadowEnabled = directionalShadow.enabled;
    if (directionalShadow.enabled)
    {
        inputs.lighting.directionalShadowMap = directionalShadow.shadowMap;
    }
    inputs.lighting.localShadowEnabled = localShadows->requested;
    if (localShadows->requested)
    {
        inputs.lighting.localShadowAtlas = localShadows->depthAtlas;
        inputs.lighting.localShadowPlanIdentity =
            localShadows->localShadowPlanIdentity;
        inputs.lighting.localShadowAtlasPlanIdentity =
            localShadows->atlasPlanIdentity;
        inputs.lighting.localShadowAtlasGeneration =
            localShadows->atlasGeneration;
        inputs.lighting.localShadowPageCount = localShadows->pageCount;
        inputs.lighting.localShadowPageResolution =
            localShadows->pageResolution;
        inputs.lighting.localShadowPageCapacity =
            localShadowAtlasPlan->GetTextureContract().pageCapacity;
    }
    inputs.lighting.textureIBLEnabled = lightingResources->textureIBLEnabled;
    inputs.lighting.recordResults = m_recordContext.results;
    if (lightingResources->textureIBLEnabled)
    {
        inputs.lighting.irradiance =
            lightingResources->environmentIBLIrradiance;
        inputs.lighting.prefilteredEnvironment =
            lightingResources->environmentIBLPrefiltered;
        inputs.lighting.brdfLUT = lightingResources->environmentIBLBRDFLUT;
        inputs.lighting.expectedIrradianceView =
            lightingResources->environmentIBLIrradianceView;
        inputs.lighting.expectedPrefilteredEnvironmentView =
            lightingResources->environmentIBLPrefilteredView;
        inputs.lighting.expectedBRDFLUTView =
            lightingResources->environmentIBLBRDFLUTView;
    }
    if (!inputs.IsComplete() ||
        !lightingPass->AddToGraph(builder, lightingView, inputs))
    {
        SetCompositionFailure(lightingPass->GetUnsupportedReason());
        return;
    }

    LightingOutputs lighting;
    lighting.provenance = builder.GetProvenance();
    lighting.frameIdentity = m_preparedContext.frameIdentity;
    lighting.viewIdentity = m_preparedContext.viewIdentity;
    lighting.lightingPlanIdentity =
        m_recordContext.lightingPlan->GetIdentityHash();
    lighting.hdrSceneColor = sceneTextures->hdrSceneColor;
    lighting.hdrSceneColorMetadata = sceneTextures->hdrSceneColorMetadata;
    lighting.clusteredLights = inputs.lighting.clusterData;
    lighting.baseIndirectSpecular = inputs.baseIndirectSpecular;
    lighting.baseIndirectSpecularMetadata = MakeMetadata(
        m_preparedContext.outputExtent,
        SceneTextures::BaseIndirectSpecularContract);
    lighting.specularResponse = inputs.specularResponse;
    lighting.specularResponseMetadata = MakeMetadata(
        m_preparedContext.outputExtent,
        SceneTextures::SpecularResponseContract);
    lighting.exposureState = sceneTextures->exposureState;
    lighting.exposureStateMetadata = sceneTextures->exposureStateMetadata;
    lighting.exposureStateContentHash =
        m_recordContext.view.exposureStateContentHash;
    lighting.worldNormal = sceneTextures->gBuffer1;
    lighting.worldNormalMetadata = sceneTextures->gBuffer1Metadata;
    lighting.materialIdentity = sceneTextures->gBuffer3;
    lighting.materialIdentityMetadata = sceneTextures->gBuffer3Metadata;
    lighting.materialParameterTable = sceneTextures->materialTable;
    lighting.materialParameterTableMetadata =
        sceneTextures->materialTableMetadata;
    if (!lighting.clusteredLights.IsValid() ||
        !lighting.baseIndirectSpecular.IsValid() ||
        !lighting.specularResponse.IsValid() ||
        !lighting.exposureState.IsValid() ||
        !lighting.exposureStateMetadata.IsValid() ||
        lighting.exposureStateContentHash == 0 ||
        !HasCurrentGraphProvenance(
            lighting.baseIndirectSpecular, m_recordContext.identity) ||
        !HasCurrentGraphProvenance(
            lighting.specularResponse, m_recordContext.identity) ||
        !HasCurrentGraphProvenance(
            lighting.exposureState, m_recordContext.identity) ||
        !HasCurrentGraphProvenance(
            lighting.clusteredLights, m_recordContext.identity))
    {
        SetCompositionFailure(
            "Deferred lighting could not publish the current LightingFramePlan cluster output");
        return;
    }
    if (blackboard.Publish(BuiltinRenderFeatureIds::DeferredLighting,
                           Describe(), std::move(lighting)) !=
        RenderGraphBlackboardCode::Success)
    {
        SetCompositionFailure("Deferred lighting could not publish LightingOutputs");
    }
}

RenderFeatureExecutionReceipt
DeferredLightingFeatureAdapter::CollectReceipt() const
{
    RenderFeatureExecutionReceipt receipt;
    receipt.featureId = BuiltinRenderFeatureIds::DeferredLighting;
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

bool DeferredLightingFeatureAdapter::HasMatchingRecordContext(
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

bool DeferredLightingFeatureAdapter::HasValidLightingPlan(
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

void DeferredLightingFeatureAdapter::SetCompositionFailure(std::string diagnostic)
{
    m_readiness.inputsReady = false;
    m_readiness.executed = false;
    m_readiness.diagnostic = diagnostic.empty()
        ? "Deferred lighting graph composition failed"
        : std::move(diagnostic);
}

DeferredForwardSpecialFeatureAdapter::DeferredForwardSpecialFeatureAdapter(
    DefaultRenderPassBundle* passBundle,
    PipelineCache* pipelineCache) noexcept
    : m_passBundle(passBundle)
    , m_pipelineCache(pipelineCache)
{
}

void DeferredForwardSpecialFeatureAdapter::SetRecordContext(
    RenderPassRecordContext context)
{
    m_recordContext = std::move(context);
    m_hasRecordContext = true;
    m_preparedContext = {};
    m_readiness = {};
    m_lastPassChainStats = {};
    m_lastReceipt = {};
}

RenderFeatureDescriptor DeferredForwardSpecialFeatureAdapter::Describe() const
{
    RenderFeatureDescriptor descriptor;
    descriptor.featureId = BuiltinRenderFeatureIds::DeferredForwardSpecialChain;
    descriptor.schemaVersion = 1;
    descriptor.name = "DeferredForwardSpecialChain";
    descriptor.phase = RenderFeaturePhase::Sky;
    descriptor.consumedResources = {
        {RenderBlackboardResourceId::SceneTextures,
         SceneTextures::SchemaVersion},
        {RenderBlackboardResourceId::VolumetricLightingOutputs,
         VolumetricLightingOutputs::SchemaVersion},
        {RenderBlackboardResourceId::LocalShadowOutputs,
         LocalShadowOutputs::SchemaVersion}};
    descriptor.producedResources = {
        {RenderBlackboardResourceId::SceneDepthSnapshot,
         SceneDepthSnapshot::SchemaVersion},
        {RenderBlackboardResourceId::PreTranslucencyOutputs,
         PreTranslucencyOutputs::SchemaVersion}};
    descriptor.requiredCapabilities = ToMask(RenderFeatureCapability::Graphics);
    descriptor.requiredRecipes =
        ToMask(RenderPipelineRecipe::DeferredQuality) |
        ToMask(RenderPipelineRecipe::HybridRayTracing);
    descriptor.qualityTiers = RVX_ALL_RENDER_QUALITY_TIERS;
    descriptor.fallbackPolicy = RenderFeatureFallbackPolicy::None;
    descriptor.shaderPipelineFamily = "ForwardSpecialSurfaces";
    return descriptor;
}

void DeferredForwardSpecialFeatureAdapter::Prepare(
    const RenderFeatureContext& context)
{
    m_preparedContext = context;
    m_readiness = {};
    m_readiness.compiled = true;
    m_readiness.hardwareSupported = HasGraphicsCapability(
        context.hardwareCapabilities) && IsNativeDeferredBackend(context.backend);
    m_readiness.implementationSupported = UsesDeferredOpaqueComposition(context);
    const bool hasValidLightingPlan = HasValidLightingPlan(context);
    OpaquePass* const opaquePass = m_passBundle != nullptr
        ? m_passBundle->FindPass<OpaquePass>()
        : nullptr;
    OpaqueRasterPipelinePreflightResult opaquePipelinePreflight;
    if (m_readiness.hardwareSupported &&
        m_readiness.implementationSupported && hasValidLightingPlan &&
        opaquePass != nullptr)
    {
        // The post-lighting opaque subset is still required renderer work.
        // Resolve every pipeline from the same frozen frame plan before the
        // GBuffer/lighting graph can be registered, including the legitimate
        // ReadyNoWork case for a frame with no special opaque surfaces.
        opaquePipelinePreflight = opaquePass->PreflightRasterPipelines(
            m_recordContext,
            OpaqueRasterOutputFamily::ForwardSceneHDR,
            OpaqueRasterSurfaceRoute::ForwardSpecialOpaque);
    }
    m_readiness.pipelineReady = opaquePipelinePreflight.Succeeded() &&
        m_pipelineCache != nullptr &&
        m_pipelineCache->GetOpaqueSceneColorPyramidPipeline(
            RHIFormat::RGBA16_FLOAT) != nullptr &&
        m_pipelineCache->GetOpaqueSceneColorPyramidSetLayout() != nullptr;
    m_readiness.inputsReady = HasMatchingRecordContext(context) &&
        hasValidLightingPlan && m_passBundle != nullptr &&
        m_pipelineCache != nullptr && opaquePass != nullptr;
    if (!m_readiness.hardwareSupported)
    {
        m_readiness.diagnostic =
            "DeferredQuality requires the DX12, Vulkan, or Metal renderer path";
    }
    else if (!m_readiness.implementationSupported)
    {
        m_readiness.diagnostic =
            "Deferred forward-special chain requires a deferred-opaque recipe";
    }
    else if (!hasValidLightingPlan)
    {
        m_readiness.diagnostic =
            "Deferred forward-special chain requires a valid LightingFramePlan for the current frame and view";
    }
    else if (m_passBundle == nullptr || m_pipelineCache == nullptr ||
             opaquePass == nullptr)
    {
        m_readiness.diagnostic =
            "Deferred forward-special composition dependencies are unavailable";
    }
    else if (!m_readiness.pipelineReady)
    {
        m_readiness.diagnostic = opaquePipelinePreflight.diagnostic.empty()
            ? "Deferred forward-special frozen-plan pipeline preflight failed"
            : std::move(opaquePipelinePreflight.diagnostic);
    }
    else if (!HasMatchingRecordContext(context))
    {
        m_readiness.diagnostic =
            "Deferred forward-special record context is absent, stale, or mismatched";
    }
    m_readiness.executed = false;
}

RenderFeatureReadiness
DeferredForwardSpecialFeatureAdapter::ResolveReadiness() const
{
    return m_readiness;
}

void DeferredForwardSpecialFeatureAdapter::AddPasses(
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
    OpaquePass* const opaquePass = m_passBundle != nullptr
        ? m_passBundle->FindPass<OpaquePass>()
        : nullptr;
    if (opaquePass == nullptr)
    {
        SetCompositionFailure("Deferred forward-special opaque pass is unavailable");
        return;
    }
    const IRenderPass* const cameraVelocity =
        m_passBundle->FindPass<CameraVelocityPass>();
    const IRenderPass* const objectVelocity =
        m_passBundle->FindPass<ObjectVelocityPass>();
    if (!HasValidLightingPlan(m_preparedContext))
    {
        SetCompositionFailure(
            "Deferred forward-special record context lost its valid LightingFramePlan");
        return;
    }
    RenderGraphBlackboardCode code = RenderGraphBlackboardCode::Success;
    const VolumetricLightingOutputs* const volumetricLighting =
        blackboard.Require<VolumetricLightingOutputs>(
            BuiltinRenderFeatureIds::DeferredForwardSpecialChain,
            Describe(), &code);
    if (volumetricLighting == nullptr ||
        code != RenderGraphBlackboardCode::Success ||
        !volumetricLighting->resolvedSceneColor.IsValid() ||
        !HasCurrentGraphProvenance(
            volumetricLighting->resolvedSceneColor,
            m_recordContext.identity) ||
        volumetricLighting->frameIdentity != m_preparedContext.frameIdentity ||
        volumetricLighting->viewIdentity != m_preparedContext.viewIdentity)
    {
        SetCompositionFailure(
            "Deferred volumetric-resolved HDR output is unavailable");
        return;
    }
    code = RenderGraphBlackboardCode::Success;
    const SceneTextures* const sceneTextures =
        blackboard.Require<SceneTextures>(
            BuiltinRenderFeatureIds::DeferredForwardSpecialChain,
            Describe(), &code);
    if (sceneTextures == nullptr ||
        code != RenderGraphBlackboardCode::Success ||
        ValidateDeferredSceneTextures(*sceneTextures) !=
            DeferredSceneTexturesValidationCode::Success)
    {
        SetCompositionFailure(
            "Deferred forward-special chain requires the exact typed GBuffer surface outputs");
        return;
    }
    code = RenderGraphBlackboardCode::Success;
    const LocalShadowOutputs* const localShadows =
        blackboard.Require<LocalShadowOutputs>(
            BuiltinRenderFeatureIds::DeferredForwardSpecialChain,
            Describe(), &code);
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
            "Deferred forward-special chain requires the exact LocalShadowOutputs v1");
        return;
    }
    if (volumetricLighting->lightingPlanIdentity !=
        m_recordContext.lightingPlan->GetIdentityHash())
    {
        SetCompositionFailure(
            "Deferred VolumetricLightingOutputs LightingFramePlan identity mismatches the record context");
        return;
    }
    if (!builder.IsValid() || !m_recordContext.MatchesTargetGraph(builder.GetGraph()))
    {
        SetCompositionFailure(
            "Deferred forward-special record context does not match the target graph");
        return;
    }

    RenderPassRecordContext reflectionResolvedContext = m_recordContext;
    // The frozen frame snapshot still names the pre-volumetric HDR target.
    // Surface passes receive this explicit value-owned attachment instead of
    // mutating that snapshot or inferring the target from pass ordering.
    if (!TryBindFinalHdrSceneColor(
            reflectionResolvedContext,
            volumetricLighting->resolvedSceneColor))
    {
        SetCompositionFailure(
            "Deferred forward-special chain could not bind its final HDR attachment");
        return;
    }
    reflectionResolvedContext.volumetricLighting =
        VolumetricLightingRecordInput{
            reflectionResolvedContext.identity,
            volumetricLighting->volumetricPlanIdentity,
            volumetricLighting->froxelGrid,
            volumetricLighting->integratedScatteringTransmittance,
            volumetricLighting->identityRoute};
    bool postLightingContextValid = false;
    const RenderPassRecordContext postLightingContext = MakeOpaqueRecordContext(
        reflectionResolvedContext, postLightingContextValid);
    if (!postLightingContextValid ||
        !builder.AddGraphCallback(
            "DeferredForwardSpecialOpaque",
            postLightingContext,
            [opaque = opaquePass, context = postLightingContext](RenderGraph& graph)
            {
                opaque->AddForwardSpecialToGraph(graph, context);
            }))
    {
        SetCompositionFailure(
            "Deferred ForwardSpecialOpaque registration failed for the shared opaque submission");
        return;
    }
    ++m_lastPassChainStats.graphPassCount;
    m_lastPassChainStats.graphPassNames.push_back("DeferredForwardSpecialOpaque");

    std::string failure;
    if (!AddLegacyPassRange(
            builder,
            postLightingContext,
            *m_passBundle,
            opaquePass,
            [cameraVelocity, objectVelocity](const IRenderPass& pass)
            {
                return pass.GetPriority() > PassPriority::Opaque &&
                       pass.GetPriority() < PassPriority::Water &&
                       &pass != cameraVelocity && &pass != objectVelocity &&
                       !IsHybridRayTracingReflectionPass(pass);
            },
            m_lastPassChainStats,
            failure))
    {
        SetCompositionFailure(std::move(failure));
        return;
    }

    const WaterFeaturePass* const waterPass =
        FindWaterFeaturePass(*m_passBundle);
    const ParticleFeaturePass* const particlePass =
        m_passBundle->FindPass<ParticleFeaturePass>();
    const WaterSurfaceRequirements waterRequirements = waterPass != nullptr
        ? waterPass->GetRequirements()
        : WaterSurfaceRequirements{};
    const RenderPassStatus waterStatus = waterPass != nullptr
        ? waterPass->GetStatus() : RenderPassStatus{};
    const bool waterExecutable = waterPass != nullptr &&
        waterStatus.requestedEnabled && waterStatus.supported &&
        waterStatus.enabled;
    const ParticleVfxRequirements particleRequirements = particlePass != nullptr
        ? particlePass->GetVfxRequirements()
        : ParticleVfxRequirements{};
    const RenderPassStatus particleStatus = particlePass != nullptr
        ? particlePass->GetStatus() : RenderPassStatus{};
    const bool particleExecutable = particlePass != nullptr &&
        particleStatus.requestedEnabled && particleStatus.supported &&
        particleStatus.enabled;
    const SceneDepthSnapshotDemand depthDemand =
        ResolveSceneDepthSnapshotDemand(
            waterRequirements.depthSnapshotRequired,
            waterExecutable,
            particleRequirements.requested &&
                particleRequirements.softDepthRequested,
            particleRequirements.softDepthRequired,
            particleExecutable);
    if (!depthDemand.valid)
    {
        SetCompositionFailure(depthDemand.failureReason);
        return;
    }
    SceneDepthSnapshot sceneDepthSnapshot;
    sceneDepthSnapshot.provenance = builder.GetProvenance();
    sceneDepthSnapshot.frameIdentity = m_preparedContext.frameIdentity;
    sceneDepthSnapshot.viewIdentity = m_preparedContext.viewIdentity;
    sceneDepthSnapshot.outputExtent = m_preparedContext.outputExtent;
    sceneDepthSnapshot.requested = depthDemand.requested;
    sceneDepthSnapshot.required = depthDemand.required;
    SceneDepthSnapshotRecordInput sceneDepthSnapshotInput;

    OpaqueSceneColorPyramid opaqueSceneColorPyramid;
    OpaqueSceneColorPyramidRecordInput opaqueSceneColorPyramidInput;
    std::string opaqueSceneColorPyramidFailure;
    if (!BuildOpaqueSceneColorPyramid(
            builder,
            *m_pipelineCache,
            postLightingContext,
            m_preparedContext.frameIdentity,
            m_preparedContext.viewIdentity,
            m_preparedContext.outputExtent,
            m_preparedContext.resolvedQuality,
            opaqueSceneColorPyramid,
            opaqueSceneColorPyramidInput,
            opaqueSceneColorPyramidFailure))
    {
        SetCompositionFailure(std::move(opaqueSceneColorPyramidFailure));
        return;
    }
    m_lastPassChainStats.graphPassCount += opaqueSceneColorPyramid.mipCount;
    m_lastPassChainStats.graphPassNames.push_back(
        "OpaqueSceneColorPyramidBase");
    for (uint32 mipIndex = 1;
         mipIndex < opaqueSceneColorPyramid.mipCount;
         ++mipIndex)
    {
        m_lastPassChainStats.graphPassNames.push_back(
            "OpaqueSceneColorPyramidMip" + std::to_string(mipIndex));
    }
    if (sceneDepthSnapshot.requested)
    {
        const bool snapshotRequired = sceneDepthSnapshot.required;
        std::string sceneDepthFailure;
        if (!BuildSceneDepthSnapshot(
                builder,
                postLightingContext,
                m_preparedContext.frameIdentity,
                m_preparedContext.viewIdentity,
                m_preparedContext.outputExtent,
                snapshotRequired,
                sceneDepthSnapshot,
                sceneDepthSnapshotInput,
                sceneDepthFailure))
        {
            if (snapshotRequired)
            {
                SetCompositionFailure(std::move(sceneDepthFailure));
                return;
            }
            sceneDepthSnapshot.provenance = builder.GetProvenance();
            sceneDepthSnapshot.frameIdentity = m_preparedContext.frameIdentity;
            sceneDepthSnapshot.viewIdentity = m_preparedContext.viewIdentity;
            sceneDepthSnapshot.outputExtent = m_preparedContext.outputExtent;
            sceneDepthSnapshot.requested = true;
            sceneDepthSnapshot.required = false;
            sceneDepthSnapshot.available = false;
        }
        else
        {
            ++m_lastPassChainStats.graphPassCount;
            m_lastPassChainStats.graphPassNames.push_back(
                "SceneDepthSnapshotCopy");
        }
    }
    RenderPassRecordContext translucencyContext = postLightingContext;
    translucencyContext.opaqueSceneColorPyramid =
        opaqueSceneColorPyramidInput;
    translucencyContext.sceneDepthSnapshot = sceneDepthSnapshotInput;
    std::string waterSurfaceFailure;
    if (!BuildWaterSurfaceRecordInput(
            builder, translucencyContext, *m_passBundle,
            m_preparedContext.outputExtent, translucencyContext.waterSurface,
            waterSurfaceFailure))
    {
        SetCompositionFailure(std::move(waterSurfaceFailure));
        return;
    }
    RGTextureHandle finalSceneColor = translucencyContext.view.colorTarget;
    if (!AddLegacyPassRange(
            builder,
            translucencyContext,
            *m_passBundle,
            opaquePass,
            [cameraVelocity, objectVelocity](const IRenderPass& pass)
            {
                return pass.GetPriority() >= PassPriority::Water &&
                       pass.GetPriority() < PassPriority::Transparent &&
                       &pass != cameraVelocity && &pass != objectVelocity &&
                       !IsHybridRayTracingReflectionPass(pass);
            },
            m_lastPassChainStats,
            failure,
            &finalSceneColor))
    {
        SetCompositionFailure(std::move(failure));
        return;
    }

    if (!finalSceneColor.IsValid())
    {
        SetCompositionFailure(
            "DeferredQuality did not produce the canonical HDR scene color surface");
        return;
    }

    PreTranslucencyOutputs outputs;
    outputs.provenance = builder.GetProvenance();
    outputs.frameIdentity = m_preparedContext.frameIdentity;
    outputs.viewIdentity = m_preparedContext.viewIdentity;
    outputs.lightingPlanIdentity =
        m_recordContext.lightingPlan->GetIdentityHash();
    outputs.hdrSceneColor = finalSceneColor;
    outputs.hdrSceneColorMetadata = MakeMetadata(
        m_preparedContext.outputExtent, SceneTextures::HDRSceneColorContract);
    outputs.deviceDepth = m_recordContext.view.depthTarget;
    outputs.deviceDepthMetadata = MakeMetadata(
        m_preparedContext.outputExtent, SceneTextures::DeviceDepthContract);
    outputs.worldNormal = sceneTextures->gBuffer1;
    outputs.worldNormalMetadata = sceneTextures->gBuffer1Metadata;
    outputs.materialIdentity = sceneTextures->gBuffer3;
    outputs.materialIdentityMetadata = sceneTextures->gBuffer3Metadata;
    outputs.materialParameterTable = sceneTextures->materialTable;
    outputs.materialParameterTableMetadata =
        sceneTextures->materialTableMetadata;
    outputs.velocity = m_recordContext.view.velocityTarget;
    outputs.velocityMetadata = MakeMetadata(
        m_preparedContext.outputExtent, SceneTextures::VelocityContract);
    outputs.reactiveMask = m_recordContext.view.reactiveMaskTarget;
    outputs.reactiveMaskMetadata = MakeMetadata(
        m_preparedContext.outputExtent, SceneTextures::ReactiveMaskContract);
    outputs.opaqueSceneColorPyramid = std::move(opaqueSceneColorPyramid);
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
    if (blackboard.Publish(BuiltinRenderFeatureIds::DeferredForwardSpecialChain,
                           Describe(),
                           std::move(sceneDepthSnapshot)) !=
        RenderGraphBlackboardCode::Success ||
        blackboard.Publish(BuiltinRenderFeatureIds::DeferredForwardSpecialChain,
                           Describe(),
                           std::move(outputs)) !=
        RenderGraphBlackboardCode::Success)
    {
        SetCompositionFailure(
            "DeferredQuality could not publish PreTranslucencyOutputs v2");
    }
}

RenderFeatureExecutionReceipt
DeferredForwardSpecialFeatureAdapter::CollectReceipt() const
{
    RenderFeatureExecutionReceipt receipt;
    receipt.featureId = BuiltinRenderFeatureIds::DeferredForwardSpecialChain;
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

bool DeferredForwardSpecialFeatureAdapter::HasMatchingRecordContext(
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

bool DeferredForwardSpecialFeatureAdapter::HasValidLightingPlan(
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

void DeferredForwardSpecialFeatureAdapter::SetCompositionFailure(
    std::string diagnostic)
{
    m_readiness.inputsReady = false;
    m_readiness.executed = false;
    m_readiness.diagnostic = std::move(diagnostic);
}
} // namespace RVX
