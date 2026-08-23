/**
 * @file PostProcessPipelineFeatureAdapter.cpp
 * @brief Typed feature composition for the post-process pipeline.
 */

#include "Features/PostProcessPipelineFeatureAdapter.h"

#include "Core/Diagnostics/ContentHash.h"

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

    [[nodiscard]] bool IsPresentableRecipe(
        RenderPipelineRecipe recipe) noexcept
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

    [[nodiscard]] bool UsesHardwareSRGBEncode(RHIFormat format) noexcept
    {
        return IsSRGBFormat(format);
    }

    [[nodiscard]] PostProcessTransfer FindScheduledToneMappingTransfer(
        const PostProcessStackExecuteStats& stats) noexcept
    {
        for (const PostProcessEffectExecutionPlan& effect : stats.effectPlans)
        {
            if (effect.scheduled &&
                effect.passId == PostProcessPassId::ToneMapping)
            {
                return effect.transfer;
            }
        }

        return PostProcessTransfer::None;
    }
} // namespace

PostProcessPipelineFeatureAdapter::PostProcessPipelineFeatureAdapter(
    PostProcessStack* stack) noexcept
    : m_stack(stack)
{
}

void PostProcessPipelineFeatureAdapter::SetRecordContext(
    PostProcessPipelineRecordContext context)
{
    m_recordContext = std::move(context);
    m_hasRecordContext = true;
    m_preparedContext = {};
    m_lastPlanStats = {};
    m_composedProvenance = {};
    m_lastReceipt = {};
    m_lastExecutionReceipt = {};
    m_readiness = {};
    m_hasComposedPresentationPlan = false;
    m_hardwareSRGBEncodeBoundary = false;
}

void PostProcessPipelineFeatureAdapter::SetStack(PostProcessStack* stack) noexcept
{
    if (m_stack == stack)
    {
        return;
    }

    m_stack = stack;
    m_hasRecordContext = false;
    m_recordContext = {};
    m_preparedContext = {};
    m_lastPlanStats = {};
    m_composedProvenance = {};
    m_lastReceipt = {};
    m_lastExecutionReceipt = {};
    m_readiness = {};
    m_hasComposedPresentationPlan = false;
    m_hardwareSRGBEncodeBoundary = false;
}

RenderFeatureDescriptor PostProcessPipelineFeatureAdapter::Describe() const
{
    RenderFeatureDescriptor descriptor;
    descriptor.featureId = BuiltinRenderFeatureIds::PostProcessPipeline;
    descriptor.schemaVersion = 1;
    descriptor.name = "PostProcessPipeline";
    descriptor.phase = RenderFeaturePhase::HDRPost;
    descriptor.consumedResources = {
        {RenderBlackboardResourceId::TranslucencyOutputs,
         TranslucencyOutputs::SchemaVersion}};
    descriptor.producedResources = {
        {RenderBlackboardResourceId::PostProcessOutputs,
         PostProcessOutputs::SchemaVersion}};
    descriptor.requiredCapabilities = ToMask(RenderFeatureCapability::Graphics);
    descriptor.requiredRecipes =
        ToMask(RenderPipelineRecipe::ForwardStandard) |
        ToMask(RenderPipelineRecipe::DeferredQuality);
    descriptor.qualityTiers = RVX_ALL_RENDER_QUALITY_TIERS;
    descriptor.fallbackPolicy = RenderFeatureFallbackPolicy::None;
    descriptor.shaderPipelineFamily = "PostProcessPipeline";
    return descriptor;
}

void PostProcessPipelineFeatureAdapter::Prepare(
    const RenderFeatureContext& context)
{
    m_preparedContext = context;
    m_readiness = {};
    m_readiness.compiled = true;
    m_readiness.hardwareSupported = HasGraphicsCapability(
        context.hardwareCapabilities);
    m_readiness.implementationSupported = IsPresentableRecipe(
        context.resolvedRecipe);
    m_readiness.pipelineReady = m_stack != nullptr && m_stack->IsInitialized();
    m_readiness.inputsReady = HasMatchingRecordContext(context);

    if (!m_readiness.hardwareSupported)
    {
        m_readiness.diagnostic = "Graphics capability is unavailable";
    }
    else if (!m_readiness.implementationSupported)
    {
        m_readiness.diagnostic =
            "Post-process pipeline is unavailable for the selected recipe";
    }
    else if (m_stack == nullptr || !m_stack->IsInitialized())
    {
        m_readiness.pipelineReady = false;
        m_readiness.diagnostic = "Post-process stack is unavailable";
    }
    else if (!m_readiness.inputsReady)
    {
        m_readiness.diagnostic =
            "Post-process record context is absent, stale, or mismatched";
    }
    m_readiness.executed = false;
}

RenderFeatureReadiness
PostProcessPipelineFeatureAdapter::ResolveReadiness() const
{
    return m_readiness;
}

void PostProcessPipelineFeatureAdapter::AddPasses(
    RenderFeatureGraphBuilder& builder,
    RenderGraphBlackboard& blackboard)
{
    m_lastPlanStats = {};
    m_composedProvenance = {};
    m_lastReceipt = {};
    m_lastExecutionReceipt = {};
    m_readiness.executed = false;
    m_hasComposedPresentationPlan = false;
    m_hardwareSRGBEncodeBoundary = false;
    if (!m_readiness.IsReadyToExecute())
    {
        return;
    }
    if (!HasCurrentGraphProvenance(builder))
    {
        SetCompositionFailure(
            "Post-process presentation surface is stale or belongs to another graph");
        return;
    }

    RenderGraphBlackboardCode code = RenderGraphBlackboardCode::Success;
    const TranslucencyOutputs* const translucency =
        blackboard.Require<TranslucencyOutputs>(
            BuiltinRenderFeatureIds::PostProcessPipeline, Describe(), &code);
    if (translucency == nullptr || code != RenderGraphBlackboardCode::Success)
    {
        SetCompositionFailure("Post-process requires TranslucencyOutputs v2");
        return;
    }
    if (translucency->frameIdentity != m_preparedContext.frameIdentity ||
        translucency->viewIdentity != m_preparedContext.viewIdentity ||
        translucency->colorDomain != PostProcessColorDomain::SceneLinearHDR ||
        translucency->transfer != PostProcessTransfer::None ||
        !HandleMatchesBuilder(translucency->hdrSceneColor, builder))
    {
        SetCompositionFailure(
            "Post-process received mismatched HDR scene-color provenance or domain");
        return;
    }

    PostProcessFrameInputs inputs = m_recordContext.frameInputs;
    inputs.sceneColor = translucency->hdrSceneColor;
    inputs.depth = translucency->deviceDepth;
    inputs.normal = translucency->worldNormal;
    inputs.velocity = translucency->velocity;
    inputs.sceneColorDomain = translucency->colorDomain;
    const bool hardwareSRGBEncode = UsesHardwareSRGBEncode(
        m_recordContext.presentableColorMetadata.format);
    inputs.outputColorDomain = hardwareSRGBEncode
        ? PostProcessColorDomain::DisplayLinearLDR
        : PostProcessColorDomain::DisplayEncodedLDR;
    inputs.outputFormat = m_recordContext.presentableColorMetadata.format;

    if (!m_stack->BuildPlan(
            inputs,
            m_recordContext.presentableColorMetadata.format,
            PostProcessPlanContract::Presentation,
            m_lastPlanStats) ||
        !m_lastPlanStats.planValid)
    {
        SetCompositionFailure(
            m_lastPlanStats.planFailureReason.empty()
                ? "Post-process typed plan is invalid"
                : m_lastPlanStats.planFailureReason);
        return;
    }
    if (m_lastPlanStats.enabledToneMappingEffectCount != 1 ||
        m_lastPlanStats.fallbackCopyApplied ||
        m_lastPlanStats.noEffectNoWork)
    {
        SetCompositionFailure(
            "Post-process requires exactly one real ToneMapping plan without fallback copy");
        return;
    }
    bool planMaterialized = false;
    if (!builder.AddGraphCallback(
            "PostProcessPipeline",
            m_recordContext.passRecordContext,
            [this, &planMaterialized, inputs, plan = m_lastPlanStats](RenderGraph& graph)
            {
                const bool executed = m_stack != nullptr &&
                    m_stack->ExecutePlan(
                        graph,
                        inputs,
                        m_recordContext.presentableColor,
                        plan);
                planMaterialized = executed;
            }) ||
        !planMaterialized)
    {
        SetCompositionFailure("Post-process typed plan could not be materialized");
        return;
    }

    m_lastPlanStats = m_stack->GetLastExecuteStats();
    if (!m_lastPlanStats.planValid ||
        m_lastPlanStats.planContract !=
            PostProcessPlanContract::Presentation ||
        m_lastPlanStats.planFingerprint == 0 ||
        m_lastPlanStats.scheduledToneMappingEffectCount != 1 ||
        m_lastPlanStats.fallbackCopyApplied ||
        m_lastPlanStats.outputColorDomain != inputs.outputColorDomain)
    {
        SetCompositionFailure(
            "Post-process materialization did not preserve the required presentation boundary");
        return;
    }

    const PostProcessTransfer toneMappingTransfer =
        FindScheduledToneMappingTransfer(m_lastPlanStats);
    const PostProcessTransfer expectedToneMappingTransfer = hardwareSRGBEncode
        ? PostProcessTransfer::SceneLinearToDisplayLinear
        : PostProcessTransfer::SceneLinearToDisplayEncoded;
    if (toneMappingTransfer != expectedToneMappingTransfer)
    {
        SetCompositionFailure(
            "Post-process ToneMapping transfer does not match the presentation surface");
        return;
    }

    PostProcessOutputs outputs;
    outputs.provenance = builder.GetProvenance();
    outputs.frameIdentity = m_preparedContext.frameIdentity;
    outputs.viewIdentity = m_preparedContext.viewIdentity;
    outputs.presentableColor = m_recordContext.presentableColor;
    outputs.presentableColorMetadata =
        m_recordContext.presentableColorMetadata;
    // An sRGB attachment receives display-linear shader output and performs
    // the final encode in hardware. The blackboard describes the stored
    // presentable resource, not the shader-visible target domain.
    outputs.colorDomain = PostProcessColorDomain::DisplayEncodedLDR;
    outputs.transfer = hardwareSRGBEncode
        ? PostProcessTransfer::DisplayLinearToEncoded
        : toneMappingTransfer;
    if (blackboard.Publish(BuiltinRenderFeatureIds::PostProcessPipeline,
                           Describe(),
                           std::move(outputs)) !=
        RenderGraphBlackboardCode::Success)
    {
        SetCompositionFailure("Post-process could not publish PostProcessOutputs v2");
        return;
    }

    m_composedProvenance = builder.GetProvenance();
    m_hasComposedPresentationPlan = true;
    m_hardwareSRGBEncodeBoundary = hardwareSRGBEncode;
}

void PostProcessPipelineFeatureAdapter::OnGraphExecutionFinalized(
    const RenderFeatureGraphExecutionEvidence& evidence)
{
    m_lastExecutionReceipt = {};
    if (!HasMatchingFinalizedEvidence(evidence) ||
        !evidence.HasExactExecution())
    {
        return;
    }

    RefreshExecutionReceipt();
}

RenderFeatureExecutionReceipt
PostProcessPipelineFeatureAdapter::CollectReceipt() const
{
    RenderFeatureExecutionReceipt receipt;
    receipt.featureId = BuiltinRenderFeatureIds::PostProcessPipeline;
    receipt.featureSchemaVersion = 1;
    receipt.phase = RenderFeaturePhase::HDRPost;
    receipt.requirement = RenderFeatureRequirement::Required;
    receipt.selected = true;
    receipt.readiness = m_readiness;
    receipt.diagnostic = m_readiness.diagnostic;
    receipt.readiness.executed = false;
    m_lastReceipt = receipt;
    return receipt;
}

bool PostProcessPipelineFeatureAdapter::HasMatchingRecordContext(
    const RenderFeatureContext& context) const noexcept
{
    return m_hasRecordContext && context.frameIdentity.IsValid() &&
           context.viewIdentity.IsValid() &&
           m_recordContext.frameIdentity == context.frameIdentity &&
           m_recordContext.viewIdentity == context.viewIdentity &&
           m_recordContext.presentableColor.IsValid() &&
           m_recordContext.presentableColorMetadata.extent ==
               context.outputExtent &&
           m_recordContext.presentableColorMetadata.format !=
               RHIFormat::Unknown &&
           m_recordContext.presentableColorMetadata.sampleCount ==
               RHISampleCount::Count1;
}

bool PostProcessPipelineFeatureAdapter::HasCurrentGraphProvenance(
    const RenderFeatureGraphBuilder& builder) const noexcept
{
    return HandleMatchesBuilder(m_recordContext.presentableColor, builder);
}

bool PostProcessPipelineFeatureAdapter::HasMatchingFinalizedEvidence(
    const RenderFeatureGraphExecutionEvidence& evidence) const noexcept
{
    return m_hasComposedPresentationPlan &&
           evidence.featureId == BuiltinRenderFeatureIds::PostProcessPipeline &&
           evidence.graphIdentity == m_composedProvenance.graphIdentity &&
           evidence.recordingGeneration ==
               m_composedProvenance.recordingGeneration &&
           evidence.frameIdentity == m_preparedContext.frameIdentity &&
           evidence.viewIdentity == m_preparedContext.viewIdentity;
}

void PostProcessPipelineFeatureAdapter::SetCompositionFailure(
    std::string diagnostic)
{
    m_readiness.inputsReady = false;
    m_readiness.executed = false;
    m_readiness.diagnostic = diagnostic.empty()
        ? "Post-process graph composition failed"
        : std::move(diagnostic);
    m_composedProvenance = {};
    m_hasComposedPresentationPlan = false;
    m_hardwareSRGBEncodeBoundary = false;
}

void PostProcessPipelineFeatureAdapter::RefreshExecutionReceipt()
{
    if (!m_hasComposedPresentationPlan || !m_lastPlanStats.planValid ||
        m_lastPlanStats.planContract !=
            PostProcessPlanContract::Presentation ||
        m_lastPlanStats.planFingerprint == 0 ||
        m_lastPlanStats.scheduledToneMappingEffectCount != 1 ||
        m_lastPlanStats.fallbackCopyApplied)
    {
        m_lastExecutionReceipt = {};
        return;
    }

    RenderPostProcessExecutionReceipt receipt;
    receipt.inputColorDomain = m_lastPlanStats.inputColorDomain;
    receipt.plannedOutputColorDomain = m_lastPlanStats.outputColorDomain;
    receipt.storedOutputColorDomain =
        PostProcessColorDomain::DisplayEncodedLDR;
    receipt.finalOutputFormat = m_lastPlanStats.finalOutputFormat;
    receipt.toneMappingTransfer =
        FindScheduledToneMappingTransfer(m_lastPlanStats);
    receipt.presentationTransfer = m_hardwareSRGBEncodeBoundary
        ? PostProcessTransfer::DisplayLinearToEncoded
        : receipt.toneMappingTransfer;

    uint64 hash = Diagnostics::RVX_DIAGNOSTICS_FNV1A64_OFFSET_BASIS;
    Diagnostics::MixContentHashValue(hash, m_lastPlanStats.planFingerprint);
    Diagnostics::MixContentHashValue(hash, m_composedProvenance.graphIdentity);
    Diagnostics::MixContentHashValue(
        hash, m_composedProvenance.recordingGeneration);
    Diagnostics::MixContentHashValue(
        hash, m_preparedContext.frameIdentity.frameSequence);
    Diagnostics::MixContentHashValue(
        hash, m_preparedContext.viewIdentity.sceneRuntimeId);
    Diagnostics::MixContentHashValue(
        hash, m_preparedContext.viewIdentity.cameraEntityPacked);
    Diagnostics::MixContentHashValue(
        hash, static_cast<uint64>(m_preparedContext.viewIdentity.subViewIndex));
    Diagnostics::MixContentHashValue(
        hash, static_cast<uint64>(m_preparedContext.viewIdentity.role));
    Diagnostics::MixContentHashValue(
        hash, static_cast<uint64>(receipt.inputColorDomain));
    Diagnostics::MixContentHashValue(
        hash, static_cast<uint64>(receipt.plannedOutputColorDomain));
    Diagnostics::MixContentHashValue(
        hash, static_cast<uint64>(receipt.finalOutputFormat));
    Diagnostics::MixContentHashValue(
        hash, static_cast<uint64>(receipt.storedOutputColorDomain));
    Diagnostics::MixContentHashValue(
        hash, static_cast<uint64>(receipt.toneMappingTransfer));
    Diagnostics::MixContentHashValue(
        hash, static_cast<uint64>(receipt.presentationTransfer));

    for (const PostProcessEffectExecutionPlan& effect :
         m_lastPlanStats.effectPlans)
    {
        if (!effect.scheduled)
        {
            continue;
        }
        Diagnostics::MixContentHashValue(
            hash, static_cast<uint64>(effect.passId));
        Diagnostics::MixContentHashValue(
            hash, static_cast<uint64>(effect.stage));
        Diagnostics::MixContentHashValue(
            hash, static_cast<uint64>(effect.order));
        Diagnostics::MixContentHashValue(
            hash, static_cast<uint64>(effect.inputDomain));
        Diagnostics::MixContentHashValue(
            hash, static_cast<uint64>(effect.outputDomain));
        Diagnostics::MixContentHashValue(
            hash, static_cast<uint64>(effect.transfer));

        switch (effect.passId)
        {
            case PostProcessPassId::ToneMapping:
                ++receipt.toneMapPassCount;
                break;
            case PostProcessPassId::Bloom:
                ++receipt.bloomPassCount;
                break;
            case PostProcessPassId::ColorGrading:
                ++receipt.colorGradingPassCount;
                break;
            case PostProcessPassId::FXAA:
                ++receipt.fxaaPassCount;
                break;
            default: break;
        }
    }

    receipt.postProcessContentHash = hash;
    receipt.hdrInputReady =
        m_lastPlanStats.planValid &&
        m_lastPlanStats.frameInputs.sceneColor.IsValid() &&
        m_lastPlanStats.inputColorDomain ==
            PostProcessColorDomain::SceneLinearHDR;
    receipt.displayLinearOutputReady = std::any_of(
        m_lastPlanStats.effectPlans.begin(),
        m_lastPlanStats.effectPlans.end(),
        [](const PostProcessEffectExecutionPlan& effect)
        {
            return effect.scheduled && effect.outputDomain ==
                PostProcessColorDomain::DisplayLinearLDR;
        });
    receipt.encodedOutputReady =
        m_hardwareSRGBEncodeBoundary ||
        m_lastPlanStats.outputColorDomain ==
            PostProcessColorDomain::DisplayEncodedLDR;
    receipt.fallbackUsed = m_lastPlanStats.fallbackCopyApplied;
    m_lastExecutionReceipt = receipt.IsStructurallyValid()
        ? std::move(receipt)
        : RenderPostProcessExecutionReceipt{};
}
} // namespace RVX
