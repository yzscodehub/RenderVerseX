#include "Features/TranslucencyFeatureAdapter.h"

#include "Features/DefaultRenderPassBundle.h"

#include "Render/PipelineCache.h"
#include "Render/Passes/ParticleFeaturePass.h"
#include "Render/Passes/TransparentPass.h"
#include "Render/Particles/ParticleBillboardDrawPlan.h"
#include "Render/Renderer/RenderDrawItem.h"
#include "Render/Translucency/TranslucencyFramePlan.h"
#include "RHI/RHIRenderPass.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <utility>

namespace RVX
{
namespace
{
    struct WeightedBlendedOITExecutionReceipt
    {
        std::atomic_bool cleared{false};
        std::atomic_bool composited{false};
    };

    struct WeightedBlendedOITClearPassData
    {
        RGTextureHandle accumulation{};
        RGTextureHandle revealage{};
        RGTextureViewHandle accumulationView{};
        RGTextureViewHandle revealageView{};
        WeightedBlendedOITScissor scissor{};
        std::shared_ptr<WeightedBlendedOITExecutionReceipt> receipt{};
        bool valid = false;
    };

    struct WeightedBlendedOITCompositePassData
    {
        RGTextureHandle accumulation{};
        RGTextureHandle revealage{};
        /** The existing HDR attachment is loaded and blended in place. */
        RGTextureHandle sceneHDR{};
        RGTextureHandle reactiveMask{};
        RGTextureViewHandle accumulationView{};
        RGTextureViewHandle revealageView{};
        RGTextureViewHandle sceneHDRView{};
        RGTextureViewHandle reactiveMaskView{};
        WeightedBlendedOITScissor scissor{};
        PipelineCache* pipelineCache = nullptr;
        std::shared_ptr<WeightedBlendedOITExecutionReceipt> receipt{};
        bool valid = false;
    };

    [[nodiscard]] RHITextureViewDesc MakeOITTextureViewDesc(
        RHIFormat format,
        RHITextureViewType type,
        const char* debugName)
    {
        RHITextureViewDesc desc;
        desc.format = format;
        desc.dimension = RHITextureDimension::Texture2D;
        desc.subresourceRange = RHISubresourceRange::All();
        desc.type = type;
        desc.debugName = debugName;
        return desc;
    }

    [[nodiscard]] bool RegisterWeightedBlendedOITClear(
        RenderFeatureGraphBuilder& builder,
        RGTextureHandle accumulation,
        RGTextureHandle revealage,
        WeightedBlendedOITScissor scissor,
        const std::shared_ptr<WeightedBlendedOITExecutionReceipt>& receipt)
    {
        return builder.AddPass<WeightedBlendedOITClearPassData>(
            "WeightedBlendedOITClear",
            RenderGraphPassType::Graphics,
            [accumulation, revealage, scissor, receipt](
                RenderGraphBuilder& graphBuilder,
                WeightedBlendedOITClearPassData& data)
            {
                data.accumulation = accumulation;
                data.revealage = revealage;
                data.scissor = scissor;
                data.receipt = receipt;
                const RHITextureDesc* const accumulationDesc =
                    graphBuilder.GetTextureDesc(accumulation);
                const RHITextureDesc* const revealageDesc =
                    graphBuilder.GetTextureDesc(revealage);
                if (accumulationDesc == nullptr || revealageDesc == nullptr ||
                    accumulationDesc->format != RHIFormat::RGBA16_FLOAT ||
                    revealageDesc->format != RHIFormat::R16_FLOAT ||
                    !data.scissor.IsValidFor(RenderOutputExtent{
                        accumulationDesc->width, accumulationDesc->height}) ||
                    revealageDesc->width != accumulationDesc->width ||
                    revealageDesc->height != accumulationDesc->height ||
                    accumulationDesc->sampleCount != RHISampleCount::Count1 ||
                    revealageDesc->sampleCount != RHISampleCount::Count1)
                {
                    return;
                }
                data.accumulationView = graphBuilder.CreateTextureView(
                    accumulation,
                    MakeOITTextureViewDesc(
                        RHIFormat::RGBA16_FLOAT,
                        RHITextureViewType::RenderTarget,
                        "WeightedBlendedOITAccumulationClearRTV"));
                data.revealageView = graphBuilder.CreateTextureView(
                    revealage,
                    MakeOITTextureViewDesc(
                        RHIFormat::R16_FLOAT,
                        RHITextureViewType::RenderTarget,
                        "WeightedBlendedOITRevealageClearRTV"));
                data.accumulationView = graphBuilder.Write(
                    data.accumulationView,
                    MakeRGAccessDesc(
                        RHIResourceState::RenderTarget,
                        RHIShaderStage::Pixel));
                data.revealageView = graphBuilder.Write(
                    data.revealageView,
                    MakeRGAccessDesc(
                        RHIResourceState::RenderTarget,
                        RHIShaderStage::Pixel));
                data.valid = data.accumulationView.IsValid() &&
                    data.revealageView.IsValid();
            },
            [](const WeightedBlendedOITClearPassData& data,
               RenderGraphPassContext& context)
            {
                RHITextureView* const accumulationView =
                    context.GetTextureView(data.accumulationView);
                RHITextureView* const revealageView =
                    context.GetTextureView(data.revealageView);
                if (!data.valid || accumulationView == nullptr ||
                    revealageView == nullptr || data.receipt == nullptr)
                {
                    context.MarkExecutionFailed(
                        "WeightedBlendedOIT clear could not realize its exact accumulation targets");
                    return;
                }
                RHIRenderPassDesc renderPass;
                renderPass.AddColorAttachment(
                    accumulationView,
                    RHILoadOp::Clear,
                    RHIStoreOp::Store,
                    RHIClearColor{0.0f, 0.0f, 0.0f, 0.0f});
                renderPass.AddColorAttachment(
                    revealageView,
                    RHILoadOp::Clear,
                    RHIStoreOp::Store,
                    RHIClearColor{1.0f, 1.0f, 1.0f, 1.0f});
                renderPass.SetRenderArea(
                    data.scissor.x,
                    data.scissor.y,
                    data.scissor.width,
                    data.scissor.height);
                context.Commands().BeginRenderPass(renderPass);
                context.Commands().EndRenderPass();
                data.receipt->cleared.store(
                    true, std::memory_order_release);
            });
    }

    [[nodiscard]] bool RegisterWeightedBlendedOITComposite(
        RenderFeatureGraphBuilder& builder,
        PipelineCache& pipelineCache,
        RGTextureHandle sceneHDR,
        RGTextureHandle accumulation,
        RGTextureHandle revealage,
        RGTextureHandle reactiveMask,
        WeightedBlendedOITScissor scissor,
        const std::shared_ptr<WeightedBlendedOITExecutionReceipt>& receipt)
    {
        return builder.AddPass<WeightedBlendedOITCompositePassData>(
            "WeightedBlendedOITComposite",
            RenderGraphPassType::Graphics,
            [&pipelineCache,
             sceneHDR,
             accumulation,
             revealage,
             reactiveMask,
             scissor,
             receipt](RenderGraphBuilder& graphBuilder,
                       WeightedBlendedOITCompositePassData& data)
            {
                data.accumulation = graphBuilder.Read(
                    accumulation,
                    MakeRGAccessDesc(
                        RHIResourceState::ShaderResource,
                        RHIShaderStage::Pixel));
                data.revealage = graphBuilder.Read(
                    revealage,
                    MakeRGAccessDesc(
                        RHIResourceState::ShaderResource,
                        RHIShaderStage::Pixel));
                data.sceneHDR = sceneHDR;
                data.reactiveMask = reactiveMask;
                data.scissor = scissor;
                data.pipelineCache = &pipelineCache;
                data.receipt = receipt;

                const RHITextureDesc* const accumulationDesc =
                    graphBuilder.GetTextureDesc(accumulation);
                const RHITextureDesc* const revealageDesc =
                    graphBuilder.GetTextureDesc(revealage);
                const RHITextureDesc* const sceneHDRDesc =
                    graphBuilder.GetTextureDesc(sceneHDR);
                const RHITextureDesc* const reactiveDesc =
                    graphBuilder.GetTextureDesc(reactiveMask);
                if (accumulationDesc == nullptr || revealageDesc == nullptr ||
                    sceneHDRDesc == nullptr || reactiveDesc == nullptr ||
                    accumulationDesc->format != RHIFormat::RGBA16_FLOAT ||
                    revealageDesc->format != RHIFormat::R16_FLOAT ||
                    sceneHDRDesc->format != RHIFormat::RGBA16_FLOAT ||
                    reactiveDesc->format != RHIFormat::R8_UNORM ||
                    !data.scissor.IsValidFor(RenderOutputExtent{
                        sceneHDRDesc->width, sceneHDRDesc->height}) ||
                    accumulationDesc->width != sceneHDRDesc->width ||
                    accumulationDesc->height != sceneHDRDesc->height ||
                    revealageDesc->width != sceneHDRDesc->width ||
                    revealageDesc->height != sceneHDRDesc->height ||
                    reactiveDesc->width != sceneHDRDesc->width ||
                    reactiveDesc->height != sceneHDRDesc->height ||
                    !HasFlag(sceneHDRDesc->usage,
                             RHITextureUsage::RenderTarget))
                {
                    return;
                }

                data.accumulationView = graphBuilder.CreateTextureView(
                    data.accumulation,
                    MakeOITTextureViewDesc(
                        RHIFormat::RGBA16_FLOAT,
                        RHITextureViewType::ShaderResource,
                        "WeightedBlendedOITAccumulationSRV"));
                data.revealageView = graphBuilder.CreateTextureView(
                    data.revealage,
                    MakeOITTextureViewDesc(
                        RHIFormat::R16_FLOAT,
                        RHITextureViewType::ShaderResource,
                        "WeightedBlendedOITRevealageSRV"));
                data.sceneHDRView = graphBuilder.CreateTextureView(
                    data.sceneHDR,
                    MakeOITTextureViewDesc(
                        RHIFormat::RGBA16_FLOAT,
                        RHITextureViewType::RenderTarget,
                        "WeightedBlendedOITInPlaceHDRRTV"));
                data.reactiveMaskView = graphBuilder.CreateTextureView(
                    data.reactiveMask,
                    MakeOITTextureViewDesc(
                        RHIFormat::R8_UNORM,
                        RHITextureViewType::RenderTarget,
                        "WeightedBlendedOITCompositeReactiveRTV"));
                data.accumulationView = graphBuilder.Read(
                    data.accumulationView,
                    MakeRGAccessDesc(
                        RHIResourceState::ShaderResource,
                        RHIShaderStage::Pixel));
                data.revealageView = graphBuilder.Read(
                    data.revealageView,
                    MakeRGAccessDesc(
                        RHIResourceState::ShaderResource,
                        RHIShaderStage::Pixel));
                data.sceneHDRView = graphBuilder.ReadWrite(
                    data.sceneHDRView,
                    MakeRGAccessDesc(
                        RHIResourceState::RenderTarget,
                        RHIShaderStage::Pixel,
                        RHIDiscardIntent::Preserve));
                data.reactiveMaskView = graphBuilder.ReadWrite(
                    data.reactiveMaskView,
                    MakeRGAccessDesc(
                        RHIResourceState::RenderTarget,
                        RHIShaderStage::Pixel,
                        RHIDiscardIntent::Preserve));
                data.valid = data.accumulation.IsValid() &&
                    data.revealage.IsValid() &&
                    data.accumulationView.IsValid() &&
                    data.revealageView.IsValid() &&
                    data.sceneHDRView.IsValid() &&
                    data.reactiveMaskView.IsValid();
            },
            [](const WeightedBlendedOITCompositePassData& data,
               RenderGraphPassContext& context)
            {
                if (!data.valid || data.pipelineCache == nullptr ||
                    data.receipt == nullptr ||
                    !data.receipt->cleared.load(
                        std::memory_order_acquire))
                {
                    context.MarkExecutionFailed(
                        "WeightedBlendedOIT composite did not observe its exact clear/graph contract");
                    return;
                }
                RHITexture* const sceneHDR = context.GetTexture(data.sceneHDR);
                RHITextureView* const accumulationView =
                    context.GetTextureView(data.accumulationView);
                RHITextureView* const revealageView =
                    context.GetTextureView(data.revealageView);
                RHITextureView* const sceneHDRView =
                    context.GetTextureView(data.sceneHDRView);
                RHITextureView* const reactiveView =
                    context.GetTextureView(data.reactiveMaskView);
                RHIPipeline* const pipeline =
                    data.pipelineCache->GetWeightedBlendedOITCompositePipeline();
                RHIDescriptorSetLayout* const setLayout =
                    data.pipelineCache->GetWeightedBlendedOITCompositeSetLayout();
                IRHIDevice* const device = data.pipelineCache->GetDevice();
                if (sceneHDR == nullptr || accumulationView == nullptr ||
                    revealageView == nullptr || sceneHDRView == nullptr ||
                    reactiveView == nullptr || pipeline == nullptr ||
                    setLayout == nullptr || device == nullptr)
                {
                    context.MarkExecutionFailed(
                        "WeightedBlendedOIT composite resources or pipeline are unavailable");
                    return;
                }

                RHIDescriptorSetDesc descriptorDesc;
                descriptorDesc.layout = setLayout;
                descriptorDesc.debugName =
                    "WeightedBlendedOITCompositeDescriptorSet";
                descriptorDesc.BindTexture(0, accumulationView);
                descriptorDesc.BindTexture(1, revealageView);
                RHIDescriptorSetRef descriptorSet =
                    device->CreateDescriptorSet(descriptorDesc);
                if (!descriptorSet ||
                    !context.RetainSubmissionResource(descriptorSet))
                {
                    context.MarkExecutionFailed(
                        "WeightedBlendedOIT composite descriptor ownership failed");
                    return;
                }

                RHIRenderPassDesc renderPass;
                renderPass.AddColorAttachment(
                    sceneHDRView, RHILoadOp::Load, RHIStoreOp::Store);
                renderPass.AddColorAttachment(
                    reactiveView, RHILoadOp::Load, RHIStoreOp::Store);
                renderPass.SetRenderArea(
                    data.scissor.x,
                    data.scissor.y,
                    data.scissor.width,
                    data.scissor.height);
                RHICommandContext& commands = context.Commands();
                commands.BeginRenderPass(renderPass);
                commands.SetPipeline(pipeline);
                commands.SetDescriptorSet(0, descriptorSet.Get());
                RHIViewport viewport;
                viewport.width = static_cast<float32>(sceneHDR->GetWidth());
                viewport.height = static_cast<float32>(sceneHDR->GetHeight());
                viewport.minDepth = 0.0f;
                viewport.maxDepth = 1.0f;
                commands.SetViewport(viewport);
                RHIRect scissorRect;
                scissorRect.x = static_cast<int32>(data.scissor.x);
                scissorRect.y = static_cast<int32>(data.scissor.y);
                scissorRect.width = data.scissor.width;
                scissorRect.height = data.scissor.height;
                commands.SetScissor(scissorRect);
                commands.Draw(3, 1, 0, 0);
                commands.EndRenderPass();
                data.receipt->composited.store(
                    true, std::memory_order_release);
            });
    }

    [[nodiscard]] const ParticleFeaturePass* FindParticleFeaturePass(
        const DefaultRenderPassBundle& bundle) noexcept
    {
        return bundle.FindPass<ParticleFeaturePass>();
    }

    [[nodiscard]] ParticleFeaturePass* FindParticleFeaturePass(
        DefaultRenderPassBundle& bundle) noexcept
    {
        return bundle.FindPass<ParticleFeaturePass>();
    }

    [[nodiscard]] TransparentPass* FindTransparentPass(
        DefaultRenderPassBundle& bundle) noexcept
    {
        return bundle.FindPass<TransparentPass>();
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
        const ParticleFeaturePass* const particle =
            FindParticleFeaturePass(bundle);
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
        if (!requirements.exposureStateRequired)
        {
            outFailure =
                "Particle VFX exposure contract was not required for an active particle snapshot";
            return false;
        }
        if (!context.view.exposureState.IsValid())
        {
            outFailure =
                "Particle VFX requires a committed exposure-state handle in the current graph view";
            return false;
        }
        if (!HasCurrentGraphProvenance(
                context.view.exposureState, context.identity))
        {
            outFailure =
                "Particle VFX exposure-state handle belongs to a stale graph recording";
            return false;
        }
        if (context.view.exposureStateContentHash == 0)
        {
            outFailure =
                "Particle VFX committed exposure state is missing its semantic content hash";
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
            const RHITextureDesc* const depthSnapshotDesc =
                builder.GetGraph().GetTextureDesc(
                    context.sceneDepthSnapshot.texture);
            const bool validSnapshot =
                context.sceneDepthSnapshot.IsCompatibleWith(
                    context.identity, extent) &&
                depthSnapshotDesc != nullptr &&
                depthSnapshotDesc->format == RHIFormat::D32_FLOAT &&
                depthSnapshotDesc->width == extent.width &&
                depthSnapshotDesc->height == extent.height &&
                depthSnapshotDesc->sampleCount == RHISampleCount::Count1 &&
                HasFlag(depthSnapshotDesc->usage,
                        RHITextureUsage::ShaderResource);
            if (!validSnapshot)
            {
                if (requirements.softDepthRequired)
                {
                    outFailure =
                        "Particle required soft depth needs the current SceneDepthSnapshot D32 surface at the exact view extent";
                    return false;
                }
            }
            else
            {
                outInput.softDepthSnapshot = context.sceneDepthSnapshot;
            }
        }

        if (requirements.distortionRequested)
        {
            RHITextureDesc distortionDesc = RHITextureDesc::RenderTarget(
                extent.width, extent.height, RHIFormat::RG16_FLOAT);
            distortionDesc.SetOptimizedClearColor(
                RHIClearColor{0.0f, 0.0f, 0.0f, 1.0f});
            distortionDesc.debugName = "ParticleDistortionVectors";
            outInput.distortionVectors = builder.CreateTexture(distortionDesc);

            RHITextureDesc compositeDesc = RHITextureDesc::RenderTarget(
                extent.width, extent.height, RHIFormat::RGBA16_FLOAT);
            compositeDesc.SetOptimizedClearColor(
                RHIClearColor{0.0f, 0.0f, 0.0f, 1.0f});
            compositeDesc.debugName =
                "ParticleDistortionCompositeSceneColor";
            outInput.compositedSceneColor =
                builder.CreateTexture(compositeDesc);
            if (!outInput.distortionVectors.IsValid() ||
                !outInput.compositedSceneColor.IsValid() ||
                outInput.compositedSceneColor.index ==
                    context.view.colorTarget.index)
            {
                outFailure =
                    "Particle distortion requires distinct RG16F and HDR graph outputs";
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool HasGraphicsCapability(
        RenderFeatureCapabilityMask capabilities) noexcept
    {
        return (capabilities &
                ToMask(RenderFeatureCapability::Graphics)) != 0;
    }

    [[nodiscard]] bool UsesRasterTranslucency(
        const RenderFeatureContext& context) noexcept
    {
        return IsPresentableRasterRecipe(context.pipeline.recipe) &&
               context.resolvedRecipe == context.pipeline.recipe;
    }
} // namespace

TranslucencyFeatureAdapter::TranslucencyFeatureAdapter(
    DefaultRenderPassBundle* passBundle,
    PipelineCache* pipelineCache) noexcept
    : m_passBundle(passBundle)
    , m_pipelineCache(pipelineCache)
{
}

void TranslucencyFeatureAdapter::SetRecordContext(
    RenderPassRecordContext context)
{
    m_recordContext = std::move(context);
    m_hasRecordContext = true;
    m_preparedContext = {};
    m_particleDrawPlan.reset();
    m_framePlan.reset();
    m_lastPassChainStats = {};
    m_lastReceipt = {};
    m_readiness = {};
}

RenderFeatureDescriptor TranslucencyFeatureAdapter::Describe() const
{
    RenderFeatureDescriptor descriptor;
    descriptor.featureId = BuiltinRenderFeatureIds::Translucency;
    descriptor.schemaVersion = 1;
    descriptor.name = "Translucency";
    descriptor.phase = RenderFeaturePhase::Translucency;
    descriptor.consumedResources = {
        {RenderBlackboardResourceId::SceneDepthSnapshot,
         SceneDepthSnapshot::SchemaVersion},
        {RenderBlackboardResourceId::PreTranslucencyOutputs,
         PreTranslucencyOutputs::SchemaVersion}};
    descriptor.producedResources = {
        {RenderBlackboardResourceId::TranslucencyOutputs,
         TranslucencyOutputs::SchemaVersion}};
    descriptor.requiredCapabilities =
        ToMask(RenderFeatureCapability::Graphics);
    descriptor.requiredRecipes =
        ToMask(RenderPipelineRecipe::ForwardStandard) |
        ToMask(RenderPipelineRecipe::DeferredQuality) |
        ToMask(RenderPipelineRecipe::HybridRayTracing);
    descriptor.qualityTiers = RVX_ALL_RENDER_QUALITY_TIERS;
    descriptor.fallbackPolicy = RenderFeatureFallbackPolicy::None;
    descriptor.shaderPipelineFamily = "TranslucencyComposition";
    // A frame with no transparent mesh or particle work still publishes the
    // typed HDR/depth contract unchanged. The registry must distinguish that
    // legitimate data-only result from an unrecorded GPU feature.
    descriptor.allowsDataOnlyCompletion = true;
    return descriptor;
}

void TranslucencyFeatureAdapter::Prepare(
    const RenderFeatureContext& context)
{
    m_preparedContext = context;
    m_readiness = {};
    m_readiness.compiled = true;
    m_readiness.hardwareSupported =
        HasGraphicsCapability(context.hardwareCapabilities);
    m_readiness.implementationSupported = UsesRasterTranslucency(context);
    const bool hasValidLightingPlan = HasValidLightingPlan(context);
    std::string framePlanDiagnostic;
    const bool framePlanReady =
        BuildFramePlan(context, framePlanDiagnostic);
    m_readiness.pipelineReady =
        m_passBundle != nullptr && framePlanReady;
    m_readiness.inputsReady = HasMatchingRecordContext(context) &&
        hasValidLightingPlan && m_readiness.pipelineReady;
    if (!m_readiness.hardwareSupported)
    {
        m_readiness.diagnostic = "Graphics capability is unavailable";
    }
    else if (!m_readiness.implementationSupported)
    {
        m_readiness.diagnostic =
            "Translucency requires a resolved presentable raster recipe";
    }
    else if (!hasValidLightingPlan)
    {
        m_readiness.diagnostic =
            "Translucency requires the current LightingFramePlan";
    }
    else if (!m_readiness.pipelineReady)
    {
        m_readiness.diagnostic = framePlanDiagnostic.empty()
            ? "Translucency pass registry or PipelineCache is unavailable"
            : std::move(framePlanDiagnostic);
    }
    else if (!HasMatchingRecordContext(context))
    {
        m_readiness.diagnostic =
            "Translucency record context is absent, stale, or mismatched";
    }
}

RenderFeatureReadiness TranslucencyFeatureAdapter::ResolveReadiness() const
{
    return m_readiness;
}

void TranslucencyFeatureAdapter::AddPasses(
    RenderFeatureGraphBuilder& builder,
    RenderGraphBlackboard& blackboard)
{
    const TranslucencyFeaturePassChainStats planStats =
        m_lastPassChainStats;
    m_lastPassChainStats = {};
    m_lastPassChainStats.frameSequence = planStats.frameSequence;
    m_lastPassChainStats.planIdentity = planStats.planIdentity;
    m_lastPassChainStats.weightedMeshCount =
        planStats.weightedMeshCount;
    m_lastPassChainStats.weightedParticleBatchCount =
        planStats.weightedParticleBatchCount;
    m_lastPassChainStats.weightedParticleItemCount =
        planStats.weightedParticleItemCount;
    m_lastPassChainStats.sortedEntryCount = planStats.sortedEntryCount;
    m_lastPassChainStats.additiveEntryCount = planStats.additiveEntryCount;
    m_lastPassChainStats.weightedBlendedRequested =
        planStats.weightedBlendedRequested;
    m_lastPassChainStats.weightedBlendedRequired =
        planStats.weightedBlendedRequired;
    m_lastPassChainStats.weightedBlendedEnabled =
        planStats.weightedBlendedEnabled;
    m_lastPassChainStats.weightedBlendedFallbackUsed =
        planStats.weightedBlendedFallbackUsed;
    m_lastReceipt = {};
    m_readiness.executed = false;
    if (!m_readiness.IsReadyToExecute())
    {
        return;
    }
    if (!builder.IsValid() ||
        !m_recordContext.MatchesTargetGraph(builder.GetGraph()) ||
        m_recordContext.results == nullptr)
    {
        SetCompositionFailure(
            "Translucency record context does not match the target graph or has no result owner");
        return;
    }

    RenderGraphBlackboardCode code = RenderGraphBlackboardCode::Success;
    const PreTranslucencyOutputs* const inputs =
        blackboard.Require<PreTranslucencyOutputs>(
            BuiltinRenderFeatureIds::Translucency, Describe(), &code);
    const uint64 lightingPlanIdentity =
        m_recordContext.lightingPlan != nullptr
            ? m_recordContext.lightingPlan->GetIdentityHash()
            : 0;
    if (inputs == nullptr ||
        code != RenderGraphBlackboardCode::Success ||
        inputs->frameIdentity != m_preparedContext.frameIdentity ||
        inputs->viewIdentity != m_preparedContext.viewIdentity ||
        inputs->lightingPlanIdentity != lightingPlanIdentity ||
        !inputs->hdrSceneColor.IsValid() ||
        !inputs->opaqueSceneColorPyramid.texture.IsValid() ||
        inputs->opaqueSceneColorPyramid.receipt == nullptr)
    {
        SetCompositionFailure(
            "Translucency requires the exact PreTranslucencyOutputs v3");
        return;
    }

    code = RenderGraphBlackboardCode::Success;
    const SceneDepthSnapshot* const sceneDepthSnapshot =
        blackboard.Require<SceneDepthSnapshot>(
            BuiltinRenderFeatureIds::Translucency, Describe(), &code);
    if (sceneDepthSnapshot == nullptr ||
        code != RenderGraphBlackboardCode::Success ||
        sceneDepthSnapshot->frameIdentity != m_preparedContext.frameIdentity ||
        sceneDepthSnapshot->viewIdentity != m_preparedContext.viewIdentity)
    {
        SetCompositionFailure(
            "Translucency requires the exact SceneDepthSnapshot v1 declaration");
        return;
    }

    RenderPassRecordContext context = m_recordContext;
    context.view.colorTarget = inputs->hdrSceneColor;
    context.view.depthTarget = inputs->deviceDepth;
    context.view.velocityTarget = inputs->velocity;
    context.view.reactiveMaskTarget = inputs->reactiveMask;
    context.view.exposureState = inputs->exposureState;
    context.directionalShadow =
        context.results->directionalShadowOutput;
    if (context.directionalShadow.identity != context.identity)
    {
        context.directionalShadow = {};
        context.directionalShadow.identity = context.identity;
    }
    context.rayTracedShadow = context.results->rayTracedShadowOutput;
    if (context.rayTracedShadow.identity != context.identity)
    {
        context.rayTracedShadow = {};
        context.rayTracedShadow.identity = context.identity;
    }
    context.ambientOcclusion = {};
    context.opaqueSceneColorPyramid = OpaqueSceneColorPyramidRecordInput{
        context.identity,
        inputs->opaqueSceneColorPyramid.texture,
        inputs->opaqueSceneColorPyramid.sourceExtent,
        inputs->opaqueSceneColorPyramid.baseExtent,
        inputs->opaqueSceneColorPyramid.rasterToColorUV,
        inputs->opaqueSceneColorPyramid.quality,
        inputs->opaqueSceneColorPyramid.mipCount,
        inputs->opaqueSceneColorPyramid.receipt};
    if (sceneDepthSnapshot->available)
    {
        context.sceneDepthSnapshot = SceneDepthSnapshotRecordInput{
            context.identity,
            sceneDepthSnapshot->depthSnapshot,
            sceneDepthSnapshot->depthSnapshotMetadata.extent,
            sceneDepthSnapshot->depthSnapshotMetadata.format,
            sceneDepthSnapshot->depthSnapshotMetadata.sampleCount,
            sceneDepthSnapshot->receipt};
        context.sceneDepthSnapshot.transmissionPyramid = sceneDepthSnapshot->transmissionPyramid;
        context.sceneDepthSnapshot.transmissionBaseExtent = sceneDepthSnapshot->transmissionMetadata.extent;
        context.sceneDepthSnapshot.transmissionMipCount = sceneDepthSnapshot->transmissionMipCount;
        context.sceneDepthSnapshot.transmissionReceipt = sceneDepthSnapshot->transmissionReceipt;
    }
    context.volumetricLighting = VolumetricLightingRecordInput{
        context.identity,
        inputs->volumetricPlanIdentity,
        inputs->froxelGrid,
        inputs->integratedScatteringTransmittance,
        inputs->volumetricIdentityRoute};

    m_lastPassChainStats.frameSequence = context.identity.frameSequence;
    m_lastPassChainStats.passStatuses =
        m_passBundle->GetRegistry().GetPassStatuses();
    m_lastPassChainStats.registeredPassCount =
        m_lastPassChainStats.passStatuses.size();
    if (m_framePlan == nullptr || m_particleDrawPlan == nullptr ||
        !m_framePlan->IsExecutable() ||
        m_framePlan->GetIdentityHash() != planStats.planIdentity ||
        m_framePlan->GetFrameIdentity() != m_preparedContext.frameIdentity ||
        m_framePlan->GetViewIdentity() != m_preparedContext.viewIdentity ||
        m_framePlan->GetProfileHash() != m_preparedContext.profileHash)
    {
        SetCompositionFailure(
            "Translucency requires the exact immutable frame plan prepared for this view");
        return;
    }

    const TranslucencyRouteSummary& routeSummary =
        m_framePlan->GetSummary();
    TransparentPass* const transparentPass =
        FindTransparentPass(*m_passBundle);
    ParticleFeaturePass* const particlePass =
        FindParticleFeaturePass(*m_passBundle);
    if (routeSummary.meshCandidateCount != 0 && transparentPass == nullptr)
    {
        SetCompositionFailure(
            "Translucency frame plan contains mesh work but no TransparentPass owner");
        return;
    }
    if (routeSummary.particleBatchCandidateCount != 0 &&
        particlePass == nullptr)
    {
        SetCompositionFailure(
            "Translucency frame plan contains particle work but no ParticleFeaturePass owner");
        return;
    }

    const size_t firstFeatureGraphPass =
        builder.GetGraph().GetDiagnostics().passes.size();
    if (routeSummary.particleBatchCandidateCount != 0)
    {
        std::string particleFailure;
        if (!BuildParticleVfxRecordInput(
                builder,
                context,
                *m_passBundle,
                m_preparedContext.outputExtent,
                context.particleVfx,
                particleFailure))
        {
            SetCompositionFailure(std::move(particleFailure));
            return;
        }
    }

    TransparentPass::PreparedTranslucencyRecording meshRecording;
    if (routeSummary.meshCandidateCount != 0)
    {
        meshRecording = transparentPass->PrepareTranslucencyRecording(
            *m_framePlan, context);
        if (!meshRecording.IsPrepared() ||
            meshRecording.GetPlanIdentityHash() !=
                m_framePlan->GetIdentityHash())
        {
            SetCompositionFailure(
                "TransparentPass rejected the immutable mesh translucency inventory");
            return;
        }
    }

    ParticleFeaturePreparedRecordingResult particlePreparation;
    if (routeSummary.particleBatchCandidateCount != 0)
    {
        particlePreparation = particlePass->PrepareTranslucencyRecording(
            *m_framePlan, m_particleDrawPlan, context);
        if (!particlePreparation.Succeeded() ||
            particlePreparation.translucencyPlanIdentity !=
                m_framePlan->GetIdentityHash() ||
            particlePreparation.recording == nullptr)
        {
            SetCompositionFailure(
                particlePreparation.diagnostic.empty()
                    ? "ParticleFeaturePass rejected the immutable particle translucency inventory"
                    : std::move(particlePreparation.diagnostic));
            return;
        }
    }

    // Keep non-mesh history interference (Water/Particle/OIT) separate from
    // sorted mesh reactivity. Qualified single surfaces may override only the
    // latter; they must never erase a particle or water contribution.
    const bool produceTemporal = meshRecording.IsPrepared() && inputs->velocity.IsValid() &&
        !context.transparentTemporalSurface.has_value();
    RGTextureHandle meshReactive = inputs->reactiveMask;
    if (produceTemporal)
    {
        auto desc = RHITextureDesc::RenderTarget(m_preparedContext.outputExtent.width,
            m_preparedContext.outputExtent.height, RHIFormat::R8_UNORM);
        desc.usage = desc.usage | RHITextureUsage::ShaderResource;
        desc.SetOptimizedClearColor(RHIClearColor{0, 0, 0, 0});
        desc.debugName = "TransparentMeshReactive";
        meshReactive = builder.CreateTexture(desc);
        struct ClearData { RGTextureViewHandle view; RenderOutputExtent extent; };
        if (!meshReactive.IsValid() || !builder.AddPass<ClearData>("TransparentMeshReactiveClear",
            RenderGraphPassType::Graphics,
            [meshReactive, extent = m_preparedContext.outputExtent](RenderGraphBuilder& graph, ClearData& data)
            {
                RHITextureViewDesc view;
                view.format = RHIFormat::R8_UNORM;
                view.type = RHITextureViewType::RenderTarget;
                data.view = graph.Write(graph.CreateTextureView(meshReactive, view),
                    MakeRGAccessDesc(RHIResourceState::RenderTarget, RHIShaderStage::Pixel));
                data.extent = extent;
            }, [](const ClearData& data, RenderGraphPassContext& passContext)
            {
                auto* view = passContext.GetTextureView(data.view);
                if (!view) { passContext.MarkExecutionFailed("Transparent mesh reactive clear unavailable"); return; }
                RHIRenderPassDesc pass;
                pass.AddColorAttachment(view, RHILoadOp::Clear, RHIStoreOp::Store, RHIClearColor{0, 0, 0, 0});
                pass.SetRenderArea(0, 0, data.extent.width, data.extent.height);
                passContext.Commands().BeginRenderPass(pass);
                passContext.Commands().EndRenderPass();
            }))
        {
            SetCompositionFailure("Transparent mesh reactive target unavailable");
            return;
        }
    }
    RGTextureHandle activeSceneColor = inputs->hdrSceneColor;
    const uint32 weightedEntryCount = routeSummary.GetEntryCount(
        TranslucencyRoute::WeightedBlended);
    const bool weightedBlendedEnabled =
        m_framePlan->GetWeightedBlendedResolution() ==
        WeightedBlendedOITCapabilityResolution::Enabled;
    if (weightedBlendedEnabled)
    {
        if (weightedEntryCount == 0 || m_pipelineCache == nullptr)
        {
            SetCompositionFailure(
                "Enabled weighted-blended translucency has no routed work or composite pipeline owner");
            return;
        }

        RHITextureDesc accumulationDesc = RHITextureDesc::RenderTarget(
            m_preparedContext.outputExtent.width,
            m_preparedContext.outputExtent.height,
            RHIFormat::RGBA16_FLOAT);
        accumulationDesc.SetOptimizedClearColor(
            RHIClearColor{0.0f, 0.0f, 0.0f, 0.0f});
        accumulationDesc.debugName = "WeightedBlendedOITAccumulation";
        RHITextureDesc revealageDesc = RHITextureDesc::RenderTarget(
            m_preparedContext.outputExtent.width,
            m_preparedContext.outputExtent.height,
            RHIFormat::R16_FLOAT);
        revealageDesc.SetOptimizedClearColor(
            RHIClearColor{1.0f, 1.0f, 1.0f, 1.0f});
        revealageDesc.debugName = "WeightedBlendedOITRevealage";

        const RGTextureHandle accumulation =
            builder.CreateTexture(accumulationDesc);
        const RGTextureHandle revealage =
            builder.CreateTexture(revealageDesc);
        if (!accumulation.IsValid() || !revealage.IsValid() ||
            accumulation.index == revealage.index ||
            accumulation.index == inputs->hdrSceneColor.index ||
            revealage.index == inputs->hdrSceneColor.index ||
            accumulation.index == inputs->reactiveMask.index ||
            revealage.index == inputs->reactiveMask.index)
        {
            SetCompositionFailure(
                "Weighted-blended translucency could not allocate distinct accumulation attachments");
            return;
        }

        // No shared projected-bounds contract exists yet for mesh and particle
        // contributors. Every WBOIT stage therefore uses the exact same full
        // viewport fallback rather than an unsound partial scissor.
        const WeightedBlendedOITScissor scissor =
            ResolveWeightedBlendedOITScissor(m_preparedContext.outputExtent);
        if (!scissor.IsValidFor(m_preparedContext.outputExtent))
        {
            SetCompositionFailure(
                "Weighted-blended translucency did not resolve a valid conservative scissor policy");
            return;
        }
        auto oitReceipt =
            std::make_shared<WeightedBlendedOITExecutionReceipt>();
        if (!RegisterWeightedBlendedOITClear(
                builder,
                accumulation,
                revealage,
                scissor,
                oitReceipt))
        {
            SetCompositionFailure(
                "Weighted-blended translucency clear registration failed");
            return;
        }
        m_lastPassChainStats.accumulationClearRegistered = true;

        if (meshRecording.IsPrepared() &&
            meshRecording.GetStats().weightedBlendedDrawCount != 0)
        {
            bool meshWeightedRecorded = false;
            if (!builder.AddGraphCallback(
                    "TransparentWeightedBlendedOIT",
                    context,
                    [&](RenderGraph& graph)
                    {
                        meshWeightedRecorded =
                            transparentPass->RecordWeightedBlended(
                                graph,
                                meshRecording,
                                accumulation,
                                revealage);
                    }) ||
                !meshWeightedRecorded)
            {
                SetCompositionFailure(
                    "TransparentPass weighted-blended recording failed");
                return;
            }
        }

        if (particlePreparation.recording != nullptr &&
            particlePreparation.weightedBatchCount != 0)
        {
            ParticleFeatureStageRecordResult particleWeighted;
            if (!builder.AddGraphCallback(
                    "ParticleWeightedBlendedOIT",
                    context,
                    [&](RenderGraph& graph)
                    {
                        particleWeighted =
                            particlePass->RecordWeightedBlended(
                                graph,
                                particlePreparation.recording,
                                ParticleWeightedBlendedRecordTargets{
                                    accumulation, revealage});
                    }) ||
                !particleWeighted.Succeeded())
            {
                SetCompositionFailure(
                    particleWeighted.diagnostic.empty()
                        ? "ParticleFeaturePass weighted-blended recording failed"
                        : std::move(particleWeighted.diagnostic));
                return;
            }
        }

        if (!RegisterWeightedBlendedOITComposite(
                builder,
                *m_pipelineCache,
                inputs->hdrSceneColor,
                accumulation,
                revealage,
                inputs->reactiveMask,
                scissor,
                oitReceipt))
        {
            SetCompositionFailure(
                "Weighted-blended translucency composite registration failed");
            return;
        }
        m_lastPassChainStats.compositeRegistered = true;
        activeSceneColor = inputs->hdrSceneColor;
    }
    else if (weightedEntryCount != 0)
    {
        SetCompositionFailure(
            "Weighted-blended routes were emitted without an enabled capability resolution");
        return;
    }

    if (meshRecording.IsPrepared())
    {
        bool meshDirectRecorded = false;
        if (!builder.AddGraphCallback(
                "TransparentSortedAndAdditive",
                context,
                [&](RenderGraph& graph)
                {
                    meshDirectRecorded = transparentPass->RecordDirect(
                        graph,
                        meshRecording,
                        activeSceneColor,
                        meshReactive);
                }) ||
            !meshDirectRecorded)
        {
            SetCompositionFailure(
                "TransparentPass sorted/additive recording failed");
            return;
        }
    }

    if (particlePreparation.recording != nullptr)
    {
        ParticleFeatureStageRecordResult particleDirect;
        if (!builder.AddGraphCallback(
                "ParticleDirectTranslucency",
                context,
                [&](RenderGraph& graph)
                {
                    particleDirect = particlePass->RecordDirect(
                        graph,
                        particlePreparation.recording,
                        ParticleDirectRecordTargets{
                            activeSceneColor, inputs->reactiveMask});
                }) ||
            !particleDirect.Succeeded() ||
            !particleDirect.hdrSceneColor.IsValid())
        {
            SetCompositionFailure(
                particleDirect.diagnostic.empty()
                    ? "ParticleFeaturePass direct recording failed"
                    : std::move(particleDirect.diagnostic));
            return;
        }
        activeSceneColor = particleDirect.hdrSceneColor;
    }

    if (!activeSceneColor.IsValid())
    {
        SetCompositionFailure(
            "Translucency did not preserve a valid HDR scene color");
        return;
    }

    if (produceTemporal)
    {
        TransparentTemporalCandidates candidates;
        bool recorded = false;
        if (!builder.AddGraphCallback("TransparentTemporalCandidates", context,
                [&](RenderGraph& graph)
                { recorded = transparentPass->RecordTemporalCandidates(graph, meshRecording, candidates); }) || !recorded)
        {
            SetCompositionFailure("Transparent temporal candidate recording failed");
            return;
        }
        TransparentTemporalSurfaceInputs surface;
        surface.frameIdentity = inputs->frameIdentity;
        surface.viewIdentity = inputs->viewIdentity;
        surface.extent = candidates.extent;
        surface.motionCoverageDepth = candidates.motionCoverageDepth;
        surface.flags = candidates.flags;
        surface.layerCount = candidates.layerCount;
        surface.meshReactive = meshReactive;
        surface.opaqueDeviceDepth = inputs->deviceDepth;
        surface.inverseProjectionMatrix = context.view.inverseProjectionMatrix;
        surface.rawCandidates = true;
        surface.historyAvailable = context.view.previousViewProjectionValid != 0 && !context.view.resetTemporalHistory;
        context.transparentTemporalSurface = surface;
    }
    TranslucencyOutputs outputs;
    outputs.provenance = builder.GetProvenance();
    outputs.frameIdentity = inputs->frameIdentity;
    outputs.viewIdentity = inputs->viewIdentity;
    outputs.hdrSceneColor = activeSceneColor;
    outputs.hdrSceneColorMetadata = inputs->hdrSceneColorMetadata;
    outputs.deviceDepth = inputs->deviceDepth;
    outputs.deviceDepthMetadata = inputs->deviceDepthMetadata;
    outputs.worldNormal = inputs->worldNormal;
    outputs.worldNormalMetadata = inputs->worldNormalMetadata;
    outputs.materialIdentity = inputs->materialIdentity;
    outputs.materialIdentityMetadata = inputs->materialIdentityMetadata;
    outputs.materialParameterTable = inputs->materialParameterTable;
    outputs.materialParameterTableMetadata =
        inputs->materialParameterTableMetadata;
    outputs.velocity = inputs->velocity;
    outputs.velocityMetadata = inputs->velocityMetadata;
    outputs.reactiveMask = inputs->reactiveMask;
    outputs.reactiveMaskMetadata = inputs->reactiveMaskMetadata;
    if (context.transparentTemporalSurface.has_value())
    {
        TransparentTemporalResolveOutputs temporal;
        std::string failure;
        auto surface = *context.transparentTemporalSurface;
        surface.historyAvailable = surface.historyAvailable &&
            context.view.previousViewProjectionValid != 0 && !context.view.resetTemporalHistory;
        if (m_pipelineCache == nullptr ||
            !BuildTransparentTemporalResolve(builder, *m_pipelineCache,
                inputs->frameIdentity, inputs->viewIdentity,
                inputs->velocity, inputs->reactiveMask,
                surface, temporal, failure))
        {
            SetCompositionFailure(failure.empty()
                ? "Transparent temporal resolve has no pipeline resources" : failure);
            return;
        }
        outputs.velocity = temporal.velocity;
        outputs.reactiveMask = temporal.reactive;
        outputs.temporalLinearDepth = temporal.linearDepth;
        outputs.temporalLinearDepthMetadata = SceneTextures::Metadata{
            surface.extent, RHIFormat::R32_FLOAT, RHISampleCount::Count1};
        // Motion/reactive preserve their RG16F/R8 metadata; TAA receives the
        // paired linear depth separately without changing other depth consumers.
        // Opaque velocity remains a separate read-only resource in this graph.
    }
    outputs.opaqueSceneColorPyramid =
        inputs->opaqueSceneColorPyramid;
    outputs.distortionVectors = context.particleVfx.distortionVectors;
    if (outputs.distortionVectors.IsValid())
    {
        outputs.distortionVectorsMetadata = SceneTextures::Metadata{
            m_preparedContext.outputExtent,
            RHIFormat::RG16_FLOAT,
            RHISampleCount::Count1};
    }
    outputs.exposureState = inputs->exposureState;
    outputs.exposureStateMetadata = inputs->exposureStateMetadata;
    outputs.colorDomain = PostProcessColorDomain::SceneLinearHDR;
    outputs.transfer = PostProcessTransfer::None;
    const RenderGraph::Diagnostics graphDiagnostics =
        builder.GetGraph().GetDiagnostics();
    for (size_t passIndex = firstFeatureGraphPass;
         passIndex < graphDiagnostics.passes.size(); ++passIndex)
    {
        ++m_lastPassChainStats.graphPassCount;
        m_lastPassChainStats.graphPassNames.push_back(graphDiagnostics.passes[passIndex].name);
    }
    if (blackboard.Publish(
            BuiltinRenderFeatureIds::Translucency,
            Describe(),
            std::move(outputs)) != RenderGraphBlackboardCode::Success)
    {
        SetCompositionFailure(
              "Translucency could not publish TranslucencyOutputs v7");
    }
}

RenderFeatureExecutionReceipt
TranslucencyFeatureAdapter::CollectReceipt() const
{
    RenderFeatureExecutionReceipt receipt;
    receipt.featureId = BuiltinRenderFeatureIds::Translucency;
    receipt.featureSchemaVersion = 1;
    receipt.phase = RenderFeaturePhase::Translucency;
    receipt.requirement = RenderFeatureRequirement::Required;
    receipt.selected = true;
    receipt.readiness = m_readiness;
    receipt.diagnostic = m_readiness.diagnostic;
    receipt.readiness.executed = false;
    m_lastReceipt = receipt;
    return receipt;
}

bool TranslucencyFeatureAdapter::HasMatchingRecordContext(
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

bool TranslucencyFeatureAdapter::HasValidLightingPlan(
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

bool TranslucencyFeatureAdapter::BuildFramePlan(
    const RenderFeatureContext& context,
    std::string& outDiagnostic)
{
    m_particleDrawPlan.reset();
    m_framePlan.reset();
    if (m_passBundle == nullptr || !HasMatchingRecordContext(context) ||
        m_recordContext.frameSnapshot == nullptr)
    {
        outDiagnostic =
            "Translucency planning requires current registry, pipeline cache, and frozen frame inputs";
        return false;
    }

    const std::vector<RenderDrawItem>& meshItems =
        m_recordContext.frameSnapshot->transparentDrawItems;
    if (!IsTransparentDrawListStrictlyOrdered(meshItems))
    {
        outDiagnostic =
            "Translucency planning requires the canonical frozen transparent mesh order";
        return false;
    }

    TranslucencyFramePlanRequest request;
    request.frameIdentity = context.frameIdentity;
    request.viewIdentity = context.viewIdentity;
    request.profileHash = context.profileHash;
    request.meshSourceIdentityHash =
        BuildTransparentDrawSourceIdentityHash(meshItems);
    request.meshCandidates.reserve(meshItems.size());
    bool hasWeightedMeshCandidate = false;
    bool needsRigidMeshPipeline = false;
    bool needsSkinnedMeshPipeline = false;
    for (uint32 ordinal = 0;
         ordinal < static_cast<uint32>(meshItems.size());
         ++ordinal)
    {
        const RenderDrawItem& item = meshItems[ordinal];
        const PipelineKey& policy = item.packet.pipelineKey;
        request.meshCandidates.push_back(TranslucencyMeshCandidate{
            ordinal,
            BuildTransparentDrawItemIdentityHash(item),
            policy.shadingModel,
            policy.blendMode,
            policy.surfacePathEligibility,
            policy.materialPolicyValid});
        const bool weightedEligible = policy.materialPolicyValid &&
            policy.surfacePathEligibility ==
                SurfacePathEligibility::ForwardTranslucency &&
            (policy.shadingModel == MaterialShadingModel::StandardLit ||
             policy.shadingModel == MaterialShadingModel::Unlit) &&
            (policy.blendMode == MaterialBlendMode::AlphaBlend ||
             policy.blendMode == MaterialBlendMode::Premultiplied);
        hasWeightedMeshCandidate |= weightedEligible;
        needsSkinnedMeshPipeline |= weightedEligible && policy.skinned;
        needsRigidMeshPipeline |= weightedEligible && !policy.skinned;
    }

    ParticleFeaturePass* const particlePass =
        FindParticleFeaturePass(*m_passBundle);
    const bool particleRequested =
        particlePass != nullptr && particlePass->IsRequestedEnabled();
    auto particlePlan = std::make_shared<ParticleBillboardDrawPlan>();
    if (particleRequested)
    {
        const ParticleRenderSnapshot* const particleSnapshot =
            context.snapshotInputs.GetParticle();
        if (particleSnapshot == nullptr)
        {
            outDiagnostic =
                "Translucency planning is missing the requested particle snapshot";
            return false;
        }
        *particlePlan = BuildParticleBillboardDrawPlan(
            *particleSnapshot, m_recordContext.view);
        if (!particlePlan->valid)
        {
            outDiagnostic = particlePlan->diagnostic.empty()
                ? "Translucency particle draw planning failed"
                : particlePlan->diagnostic;
            return false;
        }
    }
    else
    {
        particlePlan->valid = true;
    }

    request.particleSourceIdentityHash =
        BuildParticleBillboardSourceIdentityHash(*particlePlan);
    request.particleItemCount = particlePlan->GetInstanceCount();
    request.particleCandidates.reserve(particlePlan->batches.size());
    bool hasWeightedParticleCandidate = false;
    for (uint32 ordinal = 0;
         ordinal < static_cast<uint32>(particlePlan->batches.size());
         ++ordinal)
    {
        const ParticleBillboardDrawBatch& batch =
            particlePlan->batches[ordinal];
        request.particleCandidates.push_back(TranslucencyParticleCandidate{
            ordinal,
            BuildParticleBillboardBatchIdentityHash(*particlePlan, ordinal),
            batch.firstItem,
            batch.itemCount,
            batch.blendMode});
        hasWeightedParticleCandidate |=
            batch.blendMode == ParticleRenderSnapshotBlendMode::AlphaBlend ||
            batch.blendMode ==
                ParticleRenderSnapshotBlendMode::Premultiplied;
    }

    const bool hasWeightedCandidate =
        hasWeightedMeshCandidate || hasWeightedParticleCandidate;
    const bool highQualityRequired =
        context.resolvedQuality == RenderQualityTier::High ||
        context.resolvedQuality == RenderQualityTier::Ultra;
    const bool policyRequested = hasWeightedCandidate &&
        context.resolvedQuality != RenderQualityTier::Low;
    if (policyRequested && m_pipelineCache == nullptr)
    {
        outDiagnostic =
            "Weighted-blended translucency requires the renderer PipelineCache";
        return false;
    }
    request.weightedBlendedRequirement =
        policyRequested && highQualityRequired
            ? TranslucencyCapabilityRequirement::Required
            : TranslucencyCapabilityRequirement::Optional;

    const bool hardwareReady =
        (context.hardwareCapabilities &
         ToMask(RenderFeatureCapability::IndependentFloatBlend)) != 0;
    bool meshPipelinesReady = true;
    if (policyRequested && hardwareReady && needsRigidMeshPipeline)
    {
        meshPipelinesReady =
            m_pipelineCache->GetWeightedBlendedOITAccumulationPipeline(
                DefaultLitDirectVertexInputMode::Rigid) != nullptr;
    }
    if (policyRequested && hardwareReady && needsSkinnedMeshPipeline)
    {
        meshPipelinesReady = meshPipelinesReady &&
            m_pipelineCache->GetWeightedBlendedOITAccumulationPipeline(
                DefaultLitDirectVertexInputMode::Skinned) != nullptr;
    }
    const bool compositePipelineReady =
        !policyRequested || !hardwareReady ||
        m_pipelineCache->GetWeightedBlendedOITCompositePipeline() != nullptr;
    bool particleWeightedPipelineReady = true;
    std::string particleWeightedPipelineDiagnostic;
    if (policyRequested && hardwareReady && hasWeightedParticleCandidate)
    {
        // The particle WBOIT program is not owned by PipelineCache. Prove its
        // actual cooked-shader/pipeline path before sealing this frame plan;
        // otherwise a Required route could appear ready until graph execution.
        particleWeightedPipelineReady = particlePass != nullptr &&
            particlePass->PreflightWeightedBlendedPipeline(
                particleWeightedPipelineDiagnostic);
        if (!particleWeightedPipelineReady &&
            particleWeightedPipelineDiagnostic.empty())
        {
            particleWeightedPipelineDiagnostic =
                "Particle weighted-blended graphics pipeline is unavailable";
        }
    }
    request.weightedBlendedReadiness =
        WeightedBlendedOITCapabilityReadiness{
            .requested = policyRequested,
            .compiled = true,
            .hardwareSupported = hardwareReady,
            .independentBlendSupported = hardwareReady,
            .accumulationFormatBlendSupported = hardwareReady,
            .revealageFormatBlendSupported = hardwareReady,
            .implementationSupported = true,
            .pipelineReady = meshPipelinesReady && compositePipelineReady &&
                particleWeightedPipelineReady,
            .inputsReady = context.outputExtent.IsValid()};

    TranslucencyFramePlanBuildResult result =
        TranslucencyFramePlanBuilder{}.Build(request);
    if (!result.Succeeded())
    {
        outDiagnostic = !particleWeightedPipelineReady &&
                !particleWeightedPipelineDiagnostic.empty()
            ? "Weighted-blended particle pipeline preflight failed: " +
                particleWeightedPipelineDiagnostic
            : result.diagnostic.empty()
            ? GetTranslucencyFramePlanCodeName(result.code)
            : std::move(result.diagnostic);
        return false;
    }
    m_particleDrawPlan = std::move(particlePlan);
    m_framePlan = std::make_shared<const TranslucencyFramePlan>(
        std::move(result.plan));
    m_lastPassChainStats.frameSequence =
        context.frameIdentity.frameSequence;
    m_lastPassChainStats.planIdentity = m_framePlan->GetIdentityHash();
    const TranslucencyRouteSummary& summary = m_framePlan->GetSummary();
    m_lastPassChainStats.weightedMeshCount = 0;
    m_lastPassChainStats.weightedParticleBatchCount = 0;
    for (const TranslucencyPlanEntry& entry : m_framePlan->GetEntries())
    {
        if (entry.route != TranslucencyRoute::WeightedBlended)
        {
            continue;
        }
        if (entry.sourceKind == TranslucencySourceKind::Mesh)
        {
            ++m_lastPassChainStats.weightedMeshCount;
        }
        else
        {
            ++m_lastPassChainStats.weightedParticleBatchCount;
            m_lastPassChainStats.weightedParticleItemCount +=
                entry.itemCount;
        }
    }
    m_lastPassChainStats.sortedEntryCount =
        summary.GetEntryCount(TranslucencyRoute::SortedRefraction);
    m_lastPassChainStats.additiveEntryCount =
        summary.GetEntryCount(TranslucencyRoute::DirectAdditive);
    m_lastPassChainStats.weightedBlendedRequested =
        m_framePlan->GetWeightedBlendedReadiness().requested;
    m_lastPassChainStats.weightedBlendedRequired =
        m_framePlan->GetWeightedBlendedRequirement() ==
            TranslucencyCapabilityRequirement::Required;
    m_lastPassChainStats.weightedBlendedEnabled =
        m_framePlan->GetWeightedBlendedResolution() ==
            WeightedBlendedOITCapabilityResolution::Enabled;
    m_lastPassChainStats.weightedBlendedFallbackUsed =
        m_framePlan->GetWeightedBlendedResolution() ==
            WeightedBlendedOITCapabilityResolution::OptionalSortedFallback;
    return true;
}

void TranslucencyFeatureAdapter::SetCompositionFailure(
    std::string diagnostic)
{
    m_readiness.inputsReady = false;
    m_readiness.executed = false;
    m_readiness.diagnostic = std::move(diagnostic);
}
} // namespace RVX
