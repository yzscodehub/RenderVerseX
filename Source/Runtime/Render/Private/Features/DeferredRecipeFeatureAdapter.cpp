/**
 * @file DeferredRecipeFeatureAdapter.cpp
 * @brief DeferredQuality composition over graph-owned GBuffer resources.
 */

#include "Features/DeferredRecipeFeatureAdapter.h"

#include "Render/Passes/DeferredLightingPass.h"
#include "Render/Passes/OpaquePass.h"
#include "Render/PipelineCache.h"
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

    [[nodiscard]] bool IsDeferredQuality(
        const RenderFeatureContext& context) noexcept
    {
        return context.pipeline.recipe == RenderPipelineRecipe::DeferredQuality &&
               context.resolvedRecipe == RenderPipelineRecipe::DeferredQuality;
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
        const RenderPassRecordContext& context,
        RenderPassRegistry& registry,
        const OpaquePass* opaquePass,
        IncludePredicate include,
        DeferredRecipeFeaturePassChainStats& stats,
        std::string& outFailure)
    {
        stats.frameSequence = context.identity.frameSequence;
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
            ++stats.graphPassCount;
            stats.graphPassNames.push_back(status.name);
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
} // namespace

DeferredGeometryFeatureAdapter::DeferredGeometryFeatureAdapter(
    RenderPassRegistry* passRegistry,
    OpaquePass* opaquePass,
    PipelineCache* pipelineCache) noexcept
    : m_passRegistry(passRegistry)
    , m_opaquePass(opaquePass)
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

void DeferredGeometryFeatureAdapter::SetOpaquePass(OpaquePass* opaquePass) noexcept
{
    m_opaquePass = opaquePass;
}

RenderFeatureDescriptor DeferredGeometryFeatureAdapter::Describe() const
{
    RenderFeatureDescriptor descriptor;
    descriptor.featureId = BuiltinRenderFeatureIds::DeferredGeometry;
    descriptor.schemaVersion = 1;
    descriptor.name = "DeferredGeometry";
    descriptor.phase = RenderFeaturePhase::Geometry;
    descriptor.producedResources = {
        {RenderBlackboardResourceId::SceneTextures, SceneTextures::SchemaVersion}};
    descriptor.requiredCapabilities =
        ToMask(RenderFeatureCapability::Graphics) |
        ToMask(RenderFeatureCapability::MultipleRenderTargets);
    descriptor.requiredRecipes = ToMask(RenderPipelineRecipe::DeferredQuality);
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
    m_readiness.implementationSupported = IsDeferredQuality(context);
    OpaqueRasterPipelinePreflightResult opaquePipelinePreflight;
    if (m_readiness.hardwareSupported &&
        m_readiness.implementationSupported && m_opaquePass != nullptr &&
        m_pipelineCache != nullptr)
    {
        // A DeferredQuality frame may be selected only after the shared
        // opaque recorder has resolved every actual frozen DeferredOpaque
        // route request. This deliberately does not synthesize a rigid
        // baseline: masked, skinned, and sealed GPU variants are all
        // preflighted from the current frame plan before graph registration.
        opaquePipelinePreflight = m_opaquePass->PreflightRasterPipelines(
            m_recordContext,
            OpaqueRasterOutputFamily::DeferredGBuffer,
            OpaqueRasterSurfaceRoute::DeferredOpaque);
    }
    m_readiness.pipelineReady = opaquePipelinePreflight.Succeeded();
    m_readiness.inputsReady = HasMatchingRecordContext(context) &&
        HasDeferredTargets() && m_opaquePass != nullptr &&
        m_passRegistry != nullptr;

    if (!m_readiness.hardwareSupported)
    {
        m_readiness.diagnostic =
            "DeferredQuality requires the DX12, Vulkan, or Metal renderer path";
    }
    else if (!m_readiness.implementationSupported)
    {
        m_readiness.diagnostic =
            "Deferred geometry only implements DeferredQuality";
    }
    else if (m_opaquePass == nullptr || m_passRegistry == nullptr ||
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

    std::string failure;
    if (!AddLegacyPassRange(
            builder,
            m_recordContext,
            *m_passRegistry,
            m_opaquePass,
            [](const IRenderPass& pass)
            {
                return pass.GetPriority() < PassPriority::Opaque;
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
    // GFX07 does not publish velocity from allocation alone.  The existing
    // camera/object velocity passes can still own an independently allocated
    // view target, but SceneTextures.velocity remains absent until the typed
    // velocity feature introduced by GFX10/GFX11 proves a producer and its
    // semantic execution for this exact view.
    textures.velocity = {};
    textures.velocityMetadata = {};
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

    // GFX07 intentionally supports one canonical StandardLit classification
    // row.  It must be initialized before graph import: a graph-created
    // default-memory placeholder would make GBuffer3 index undefined data.
    RHIBufferRef materialTable;
    std::string materialTableError;
    IRHIDevice* const device = m_pipelineCache != nullptr
        ? m_pipelineCache->GetDevice()
        : nullptr;
    if (device == nullptr ||
        !CreateDeferredStandardLitMaterialTable(
            *device, materialTable, materialTableError))
    {
        SetCompositionFailure(materialTableError.empty()
            ? "Deferred geometry could not create the canonical StandardLit material table"
            : std::move(materialTableError));
        return;
    }
    textures.materialTable = builder.ImportBuffer(
        std::move(materialTable), RHIResourceState::ShaderResource);

    if (ValidateDeferredSceneTextures(textures) !=
        DeferredSceneTexturesValidationCode::Success)
    {
        SetCompositionFailure("Deferred scene texture contract is invalid");
        return;
    }

    bool opaqueContextValid = false;
    const RenderPassRecordContext opaqueContext = MakeOpaqueRecordContext(
        m_recordContext, opaqueContextValid);
    if (!opaqueContextValid ||
        !builder.AddGraphCallback(
            "DeferredGBuffer",
            opaqueContext,
            [opaque = m_opaquePass,
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
    DeferredLightingPass* lightingPass,
    PipelineCache* pipelineCache) noexcept
    : m_lightingPass(lightingPass)
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

void DeferredLightingFeatureAdapter::SetLightingPass(
    DeferredLightingPass* lightingPass) noexcept
{
    m_lightingPass = lightingPass;
}

RenderFeatureDescriptor DeferredLightingFeatureAdapter::Describe() const
{
    RenderFeatureDescriptor descriptor;
    descriptor.featureId = BuiltinRenderFeatureIds::DeferredLighting;
    descriptor.schemaVersion = 1;
    descriptor.name = "DeferredLighting";
    descriptor.phase = RenderFeaturePhase::Lighting;
    descriptor.consumedResources = {
        {RenderBlackboardResourceId::SceneTextures, SceneTextures::SchemaVersion}};
    descriptor.producedResources = {
        {RenderBlackboardResourceId::LightingOutputs,
         LightingOutputs::SchemaVersion}};
    descriptor.requiredCapabilities =
        ToMask(RenderFeatureCapability::Graphics) |
        ToMask(RenderFeatureCapability::Compute) |
        ToMask(RenderFeatureCapability::DescriptorBindings) |
        ToMask(RenderFeatureCapability::StorageTexture);
    descriptor.requiredRecipes = ToMask(RenderPipelineRecipe::DeferredQuality);
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
    m_readiness.implementationSupported = IsDeferredQuality(context);
    const bool createdPipeline = m_lightingPass != nullptr &&
        m_lightingPass->EnsurePipelineReady();
    m_readiness.pipelineReady = createdPipeline && m_pipelineCache != nullptr &&
        m_pipelineCache->IsDeferredLightingPipelineReady();
    m_readiness.inputsReady = HasMatchingRecordContext(context) &&
        m_lightingPass != nullptr;

    if (!m_readiness.hardwareSupported)
    {
        m_readiness.diagnostic =
            "DeferredQuality requires the DX12, Vulkan, or Metal renderer path";
    }
    else if (!m_readiness.implementationSupported)
    {
        m_readiness.diagnostic =
            "Deferred lighting only implements DeferredQuality";
    }
    else if (m_lightingPass == nullptr || m_pipelineCache == nullptr)
    {
        m_readiness.diagnostic = "Deferred lighting composition dependencies are unavailable";
    }
    else if (!m_readiness.pipelineReady)
    {
        m_readiness.diagnostic = m_lightingPass->GetUnsupportedReason();
    }
    else if (!m_lightingPass->IsSupported())
    {
        m_readiness.pipelineReady = false;
        m_readiness.diagnostic = m_lightingPass->GetUnsupportedReason();
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
    RenderGraphBlackboardCode code = RenderGraphBlackboardCode::Success;
    const SceneTextures* const sceneTextures = blackboard.Require<SceneTextures>(
        BuiltinRenderFeatureIds::DeferredLighting, Describe(), &code);
    if (sceneTextures == nullptr || code != RenderGraphBlackboardCode::Success ||
        ValidateDeferredSceneTextures(*sceneTextures) !=
            DeferredSceneTexturesValidationCode::Success)
    {
        SetCompositionFailure("Deferred lighting requires a valid SceneTextures v2 entry");
        return;
    }
    if (!builder.IsValid() || !m_recordContext.MatchesTargetGraph(builder.GetGraph()))
    {
        SetCompositionFailure(
            "Deferred lighting record context does not match the target graph");
        return;
    }

    const DeferredLightingInputs inputs{
        sceneTextures->gBuffer0,
        sceneTextures->gBuffer1,
        sceneTextures->gBuffer2,
        sceneTextures->gBuffer3,
        sceneTextures->deviceDepth,
        sceneTextures->hdrSceneColor,
        sceneTextures->materialTable};
    if (!inputs.IsComplete() ||
        !m_lightingPass->AddToGraph(builder, m_recordContext.view, inputs))
    {
        SetCompositionFailure(m_lightingPass->GetUnsupportedReason());
        return;
    }

    LightingOutputs lighting;
    lighting.provenance = builder.GetProvenance();
    lighting.hdrSceneColor = sceneTextures->hdrSceneColor;
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

void DeferredLightingFeatureAdapter::SetCompositionFailure(std::string diagnostic)
{
    m_readiness.inputsReady = false;
    m_readiness.executed = false;
    m_readiness.diagnostic = diagnostic.empty()
        ? "Deferred lighting graph composition failed"
        : std::move(diagnostic);
}

DeferredForwardSpecialFeatureAdapter::DeferredForwardSpecialFeatureAdapter(
    RenderPassRegistry* passRegistry,
    OpaquePass* opaquePass) noexcept
    : m_passRegistry(passRegistry)
    , m_opaquePass(opaquePass)
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

void DeferredForwardSpecialFeatureAdapter::SetOpaquePass(
    OpaquePass* opaquePass) noexcept
{
    m_opaquePass = opaquePass;
}

RenderFeatureDescriptor DeferredForwardSpecialFeatureAdapter::Describe() const
{
    RenderFeatureDescriptor descriptor;
    descriptor.featureId = BuiltinRenderFeatureIds::DeferredForwardSpecialChain;
    descriptor.schemaVersion = 1;
    descriptor.name = "DeferredForwardSpecialChain";
    descriptor.phase = RenderFeaturePhase::Sky;
    descriptor.consumedResources = {
        {RenderBlackboardResourceId::LightingOutputs,
         LightingOutputs::SchemaVersion}};
    descriptor.requiredCapabilities = ToMask(RenderFeatureCapability::Graphics);
    descriptor.requiredRecipes = ToMask(RenderPipelineRecipe::DeferredQuality);
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
    m_readiness.implementationSupported = IsDeferredQuality(context);
    OpaqueRasterPipelinePreflightResult opaquePipelinePreflight;
    if (m_readiness.hardwareSupported &&
        m_readiness.implementationSupported && m_opaquePass != nullptr)
    {
        // The post-lighting opaque subset is still required renderer work.
        // Resolve every pipeline from the same frozen frame plan before the
        // GBuffer/lighting graph can be registered, including the legitimate
        // ReadyNoWork case for a frame with no special opaque surfaces.
        opaquePipelinePreflight = m_opaquePass->PreflightRasterPipelines(
            m_recordContext,
            OpaqueRasterOutputFamily::ForwardSceneHDR,
            OpaqueRasterSurfaceRoute::ForwardSpecialOpaque);
    }
    m_readiness.pipelineReady = opaquePipelinePreflight.Succeeded();
    m_readiness.inputsReady = HasMatchingRecordContext(context) &&
        m_passRegistry != nullptr && m_opaquePass != nullptr;
    if (!m_readiness.hardwareSupported)
    {
        m_readiness.diagnostic =
            "DeferredQuality requires the DX12, Vulkan, or Metal renderer path";
    }
    else if (!m_readiness.implementationSupported)
    {
        m_readiness.diagnostic =
            "Deferred forward-special chain only implements DeferredQuality";
    }
    else if (m_passRegistry == nullptr || m_opaquePass == nullptr)
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
    RenderGraphBlackboardCode code = RenderGraphBlackboardCode::Success;
    const LightingOutputs* const lighting = blackboard.Require<LightingOutputs>(
        BuiltinRenderFeatureIds::DeferredForwardSpecialChain, Describe(), &code);
    if (lighting == nullptr || code != RenderGraphBlackboardCode::Success ||
        !lighting->hdrSceneColor.IsValid())
    {
        SetCompositionFailure("Deferred lighting output is unavailable");
        return;
    }
    if (!builder.IsValid() || !m_recordContext.MatchesTargetGraph(builder.GetGraph()))
    {
        SetCompositionFailure(
            "Deferred forward-special record context does not match the target graph");
        return;
    }

    bool opaqueContextValid = false;
    const RenderPassRecordContext opaqueContext = MakeOpaqueRecordContext(
        m_recordContext, opaqueContextValid);
    if (!opaqueContextValid ||
        !builder.AddGraphCallback(
            "DeferredForwardSpecialOpaque",
            opaqueContext,
            [opaque = m_opaquePass, context = opaqueContext](RenderGraph& graph)
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
            m_recordContext,
            *m_passRegistry,
            m_opaquePass,
            [](const IRenderPass& pass)
            {
                return pass.GetPriority() > PassPriority::Opaque;
            },
            m_lastPassChainStats,
            failure))
    {
        SetCompositionFailure(std::move(failure));
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

void DeferredForwardSpecialFeatureAdapter::SetCompositionFailure(
    std::string diagnostic)
{
    m_readiness.inputsReady = false;
    m_readiness.executed = false;
    m_readiness.diagnostic = std::move(diagnostic);
}
} // namespace RVX
