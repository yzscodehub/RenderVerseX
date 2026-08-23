/**
 * @file PostProcessStack.cpp
 * @brief PostProcessStack implementation
 */

#include "Render/PostProcess/PostProcessStack.h"
#include "Core/Diagnostics/ContentHash.h"
#include "Core/Log.h"
#include "RHI/RHICommandContext.h"
#include <algorithm>
#include <limits>
#include <utility>

namespace RVX
{

namespace
{
    bool IsDisplayDomain(PostProcessColorDomain domain)
    {
        return domain == PostProcessColorDomain::DisplayLinearLDR ||
               domain == PostProcessColorDomain::DisplayEncodedLDR;
    }

    bool IsLDROutputFormat(RHIFormat format)
    {
        switch (format)
        {
            case RHIFormat::RGBA8_UNORM:
            case RHIFormat::RGBA8_UNORM_SRGB:
            case RHIFormat::BGRA8_UNORM:
            case RHIFormat::BGRA8_UNORM_SRGB:
            case RHIFormat::RGB10A2_UNORM:
                return true;
            default:
                return false;
        }
    }

    bool IsToneMappingPass(const PostProcessPassDescriptor& descriptor)
    {
        return descriptor.id == PostProcessPassId::ToneMapping;
    }

    uint32 GetStageOrder(PostProcessStage stage)
    {
        return static_cast<uint32>(stage);
    }

    bool IsDescriptorEqual(const PostProcessPassDescriptor& lhs,
                           const PostProcessPassDescriptor& rhs)
    {
        return lhs.id == rhs.id &&
               ((lhs.debugName == nullptr && rhs.debugName == nullptr) ||
                (lhs.debugName != nullptr && rhs.debugName != nullptr &&
                 std::string(lhs.debugName) == rhs.debugName)) &&
               lhs.stage == rhs.stage &&
               lhs.order == rhs.order &&
               lhs.inputDomain == rhs.inputDomain &&
               lhs.outputDomain == rhs.outputDomain &&
               lhs.transfer == rhs.transfer;
    }

    void MixTextureHandleFingerprint(uint64& hash, const RGTextureHandle& handle)
    {
        Diagnostics::MixContentHashValue(hash, handle.index);
        Diagnostics::MixContentHashValue(hash, handle.hasSubresourceRange ? 1u : 0u);
        Diagnostics::MixContentHashValue(hash, handle.graphIdentity);
        Diagnostics::MixContentHashValue(hash, handle.recordingGeneration);
        Diagnostics::MixContentHashValue(hash, handle.subresourceRange.baseMipLevel);
        Diagnostics::MixContentHashValue(hash, handle.subresourceRange.mipLevelCount);
        Diagnostics::MixContentHashValue(hash, handle.subresourceRange.baseArrayLayer);
        Diagnostics::MixContentHashValue(hash, handle.subresourceRange.arrayLayerCount);
        Diagnostics::MixContentHashValue(
            hash, static_cast<uint64>(handle.subresourceRange.aspect));
    }

    uint64 ComputePlanFingerprint(const PostProcessStackExecuteStats& stats)
    {
        uint64 hash = Diagnostics::RVX_DIAGNOSTICS_FNV1A64_OFFSET_BASIS;
        Diagnostics::MixContentHashValue(hash, static_cast<uint64>(stats.planContract));
        Diagnostics::MixContentHashValue(hash, static_cast<uint64>(stats.finalOutputFormat));
        Diagnostics::MixContentHashValue(hash, static_cast<uint64>(stats.inputColorDomain));
        Diagnostics::MixContentHashValue(hash, static_cast<uint64>(stats.outputColorDomain));
        Diagnostics::MixContentHashValue(
            hash, static_cast<uint64>(stats.frameInputs.outputFormat));
        Diagnostics::MixContentHashValue(hash, stats.frameInputs.frameIndex);
        Diagnostics::MixContentHashValue(
            hash, stats.frameInputs.currentViewProjectionValid ? 1u : 0u);
        Diagnostics::MixContentHashValue(
            hash, stats.frameInputs.previousViewProjectionValid ? 1u : 0u);
        Diagnostics::MixContentHashValue(
            hash, stats.frameInputs.resetTemporalHistory ? 1u : 0u);
        MixTextureHandleFingerprint(hash, stats.frameInputs.sceneColor);
        MixTextureHandleFingerprint(hash, stats.frameInputs.depth);
        MixTextureHandleFingerprint(hash, stats.frameInputs.normal);
        MixTextureHandleFingerprint(hash, stats.frameInputs.velocity);
        Diagnostics::MixContentHashValue(hash, stats.effectPlans.size());
        for (const PostProcessEffectExecutionPlan& effect : stats.effectPlans)
        {
            Diagnostics::MixContentHashValue(hash, static_cast<uint64>(effect.passId));
            Diagnostics::MixContentHashString(hash, effect.effectName);
            Diagnostics::MixContentHashValue(hash, static_cast<uint64>(effect.stage));
            Diagnostics::MixContentHashValue(
                hash, static_cast<uint64>(static_cast<int64>(effect.order)));
            Diagnostics::MixContentHashValue(hash, static_cast<uint64>(effect.inputDomain));
            Diagnostics::MixContentHashValue(hash, static_cast<uint64>(effect.outputDomain));
            Diagnostics::MixContentHashValue(hash, static_cast<uint64>(effect.transfer));
            Diagnostics::MixContentHashValue(hash, effect.requested ? 1u : 0u);
            Diagnostics::MixContentHashValue(hash, effect.supported ? 1u : 0u);
            Diagnostics::MixContentHashValue(hash, effect.enabled ? 1u : 0u);
            Diagnostics::MixContentHashValue(hash, effect.requiresDepth ? 1u : 0u);
            Diagnostics::MixContentHashValue(hash, effect.requiresNormal ? 1u : 0u);
            Diagnostics::MixContentHashValue(hash, effect.requiresVelocity ? 1u : 0u);
            Diagnostics::MixContentHashValue(hash, effect.requiresHistory ? 1u : 0u);
            Diagnostics::MixContentHashValue(
                hash, effect.frameInputsSatisfied ? 1u : 0u);
        }
        return hash;
    }

    bool IsCopyCompatible(const RHITextureDesc& srcDesc, const RHITextureDesc& dstDesc)
    {
        return srcDesc.dimension == dstDesc.dimension &&
               srcDesc.width == dstDesc.width &&
               srcDesc.height == dstDesc.height &&
               srcDesc.depth == dstDesc.depth &&
               srcDesc.mipLevels == dstDesc.mipLevels &&
               srcDesc.arraySize == dstDesc.arraySize &&
               srcDesc.sampleCount == dstDesc.sampleCount &&
               srcDesc.format == dstDesc.format;
    }

    bool AccumulateRecordedGraphPasses(
        const RenderGraph& graph,
        size_t passCountBefore,
        PostProcessStackExecuteStats& stats,
        const char* familyName)
    {
        const size_t passCountAfter = graph.GetRecordedPassCount();
        if (passCountAfter <= passCountBefore)
        {
            stats.planValid = false;
            stats.planFailureReason = std::string(
                familyName != nullptr ? familyName : "post-process effect") +
                " did not record any RenderGraph passes";
            return false;
        }
        const size_t addedPassCount = passCountAfter - passCountBefore;
        if (addedPassCount > std::numeric_limits<uint32>::max() -
                                 stats.graphPassCount)
        {
            stats.planValid = false;
            stats.planFailureReason =
                "post-process RenderGraph pass count overflow";
            return false;
        }
        stats.graphPassCount += static_cast<uint32>(addedPassCount);
        return true;
    }

    bool AddIdentityCopyPass(RenderGraph& graph,
                             RGTextureHandle input,
                             RGTextureHandle output,
                             PostProcessStackExecuteStats& stats,
                             const char* reason)
    {
        if (!input.IsValid() || !output.IsValid())
        {
            stats.fallbackCopyReason = "identity copy skipped because input or output handle is invalid";
            return false;
        }

        const RHITextureDesc* inputDesc = graph.GetTextureDesc(input);
        const RHITextureDesc* outputDesc = graph.GetTextureDesc(output);
        if (!inputDesc || !outputDesc)
        {
            stats.fallbackCopyReason = "identity copy skipped because texture descriptions are unavailable";
            return false;
        }
        if (!IsCopyCompatible(*inputDesc, *outputDesc))
        {
            stats.fallbackCopyReason = "identity copy skipped because input and output textures are not copy-compatible";
            return false;
        }

        struct FallbackCopyData
        {
            RGTextureHandle input;
            RGTextureHandle output;
        };

        const size_t passCountBefore = graph.GetRecordedPassCount();
        graph.AddPass<FallbackCopyData>(
            "PostProcessIdentityCopy",
            RenderGraphPassType::Copy,
            [input, output](RenderGraphBuilder& builder, FallbackCopyData& data)
            {
                data.input = builder.Read(input, RHIResourceState::CopySource);
                data.output = builder.Write(output, RHIResourceState::CopyDest);
            },
            [](const FallbackCopyData& data, RenderGraphPassContext& context)
            {
                RHITexture* inputTexture = context.GetTexture(data.input);
                RHITexture* outputTexture = context.GetTexture(data.output);
                if (!inputTexture || !outputTexture)
                {
                    RVX_CORE_WARN("PostProcessStack: identity copy skipped because textures are unavailable");
                    context.MarkExecutionFailed(
                        "PostProcess identity copy textures are unavailable");
                    return;
                }

                context.Commands().CopyTexture(inputTexture, outputTexture);
            });

        if (!AccumulateRecordedGraphPasses(
                graph, passCountBefore, stats, "PostProcessIdentityCopy"))
        {
            stats.fallbackCopyReason = stats.planFailureReason;
            return false;
        }
        stats.fallbackCopyApplied = true;
        stats.fallbackCopyPassCount++;
        stats.fallbackCopyReason = reason ? reason : "identity copy";
        return true;
    }

    std::string BuildMissingFrameInputReason(const PostProcessFrameInputRequirements& requirements,
                                             const PostProcessFrameInputs& inputs)
    {
        std::string reason;
        auto append = [&reason](const char* name)
        {
            if (!reason.empty())
            {
                reason += ", ";
            }
            reason += name;
        };

        if (requirements.requiresDepth && !inputs.HasDepth())
        {
            append("depth");
        }
        if (requirements.requiresNormal && !inputs.HasNormal())
        {
            append("normal");
        }
        if (requirements.requiresVelocity && !inputs.HasVelocity())
        {
            append("velocity");
        }
        if (requirements.requiresHistory && !inputs.HasHistory())
        {
            append("temporal history");
        }

        return reason.empty() ? reason : "missing required frame input(s): " + reason;
    }

    void SetQualityPresetStats(PostProcessStackExecuteStats& stats, const PostProcessSettings& settings)
    {
        stats.requestedQualityPreset = settings.visualQualityPreset;
        stats.appliedQualityPreset = settings.visualQualityPreset;
    }
} // namespace

const char* GetPostProcessPassCanonicalName(PostProcessPassId id)
{
    switch (id)
    {
        case PostProcessPassId::Bloom: return "Bloom";
        case PostProcessPassId::SSAO: return "SSAO";
        case PostProcessPassId::ToneMapping: return "ToneMapping";
        case PostProcessPassId::ColorGrading: return "ColorGrading";
        case PostProcessPassId::ChromaticAberration: return "ChromaticAberration";
        case PostProcessPassId::Vignette: return "Vignette";
        case PostProcessPassId::FilmGrain: return "FilmGrain";
        case PostProcessPassId::FXAA: return "FXAA";
        case PostProcessPassId::DepthOfField: return "DepthOfField";
        case PostProcessPassId::MotionBlur: return "MotionBlur";
        case PostProcessPassId::VolumetricLighting: return "VolumetricLighting";
        case PostProcessPassId::Invalid:
        default:
            return nullptr;
    }
}

const char* GetRenderVisualQualityPresetName(RenderVisualQualityPreset preset)
{
    switch (preset)
    {
        case RenderVisualQualityPreset::Off:
            return "off";
        case RenderVisualQualityPreset::Low:
            return "low";
        case RenderVisualQualityPreset::Medium:
            return "medium";
        case RenderVisualQualityPreset::High:
            return "high";
        case RenderVisualQualityPreset::Cinematic:
            return "cinematic";
        default:
            return "unknown";
    }
}

void ApplyRenderVisualQualityPreset(PostProcessSettings& settings, RenderVisualQualityPreset preset)
{
    settings.visualQualityPreset = preset;

    settings.enableToneMapping = false;
    settings.enableBloom = false;
    settings.enableFXAA = false;
    settings.enableColorGrading = false;
    settings.enableVignette = false;
    settings.enableChromaticAberration = false;
    settings.enableFilmGrain = false;
    settings.enableSSAO = false;
    settings.enableSSR = false;
    settings.enableVelocityOutput = false;
    settings.enableTAA = false;
    settings.enableDOF = false;
    settings.enableMotionBlur = false;
    settings.enableVolumetricLighting = false;

    switch (preset)
    {
        case RenderVisualQualityPreset::Off:
            return;
        case RenderVisualQualityPreset::Low:
            settings.enableToneMapping = true;
            settings.enableFXAA = true;
            return;
        case RenderVisualQualityPreset::Medium:
            settings.enableToneMapping = true;
            settings.enableBloom = true;
            settings.enableFXAA = true;
            settings.enableColorGrading = true;
            return;
        case RenderVisualQualityPreset::High:
            settings.enableToneMapping = true;
            settings.enableBloom = true;
            settings.enableFXAA = true;
            settings.enableColorGrading = true;
            settings.enableVignette = true;
            settings.enableChromaticAberration = true;
            return;
        case RenderVisualQualityPreset::Cinematic:
            settings.enableToneMapping = true;
            settings.enableBloom = true;
            settings.enableFXAA = true;
            settings.enableColorGrading = true;
            settings.enableVignette = true;
            settings.enableChromaticAberration = true;
            settings.enableFilmGrain = true;
            // The historical TAA object has no resolve implementation. It is
            // deliberately not enabled by a quality preset before GFX11.
            settings.enableTAA = false;
            // SSAO/GTAO, SSR, DOF, motion blur, and volumetrics each remain
            // explicitly opt-in and unavailable until their own verified
            // renderer features land. A quality preset must not request a
            // placeholder that strict planning would reject.
            return;
        default:
            return;
    }
}

PostProcessStack::~PostProcessStack()
{
    Shutdown();
}

void PostProcessStack::Initialize(IRHIDevice* device)
{
    if (m_device)
    {
        RVX_CORE_WARN("PostProcessStack: Already initialized");
        return;
    }

    m_device = device;
    RVX_CORE_DEBUG("PostProcessStack: Initialized");
}

void PostProcessStack::Shutdown()
{
    if (!m_device)
        return;

    ClearEffects();
    m_device = nullptr;

    RVX_CORE_DEBUG("PostProcessStack: Shutdown");
}

void PostProcessStack::ClearEffects()
{
    m_effects.clear();
}

void PostProcessStack::SortEffects()
{
    std::sort(m_effects.begin(), m_effects.end(),
        [](const std::unique_ptr<IPostProcessPass>& a, const std::unique_ptr<IPostProcessPass>& b)
        {
            const PostProcessPassDescriptor lhs = a->Describe();
            const PostProcessPassDescriptor rhs = b->Describe();
            if (lhs.stage != rhs.stage)
            {
                return GetStageOrder(lhs.stage) < GetStageOrder(rhs.stage);
            }
            if (lhs.order != rhs.order)
            {
                return lhs.order < rhs.order;
            }
            return static_cast<uint16>(lhs.id) < static_cast<uint16>(rhs.id);
        });
}

void PostProcessStack::ApplySettings(const PostProcessSettings& settings)
{
    m_settings = settings;
    for (auto& effect : m_effects)
    {
        effect->Configure(m_settings);
    }
}

std::vector<IPostProcessPass*> PostProcessStack::GatherEnabledEffects(PostProcessStackExecuteStats& stats,
                                                                      bool logUnsupported,
                                                                      const PostProcessFrameInputs* frameInputs) const
{
    std::vector<IPostProcessPass*> enabledEffects;
    for (const auto& effect : m_effects)
    {
        const PostProcessPassDescriptor descriptor = effect->Describe();
        const PostProcessFrameInputRequirements requirements = effect->GetFrameInputRequirements();

        PostProcessEffectExecutionPlan plan;
        plan.passId = descriptor.id;
        plan.effectName = descriptor.debugName ? descriptor.debugName : "";
        plan.sequenceIndex = static_cast<uint32>(stats.effectPlans.size());
        plan.stage = descriptor.stage;
        plan.order = descriptor.order;
        plan.priority = descriptor.order;
        plan.inputDomain = descriptor.inputDomain;
        plan.outputDomain = descriptor.outputDomain;
        plan.transfer = descriptor.transfer;
        plan.requested = effect->IsRequestedEnabled();
        plan.supported = effect->IsSupported();
        plan.enabled = effect->IsEnabled();
        plan.pipelineReady = effect->IsSupported();
        plan.pipelineReadinessReason = effect->IsSupported()
                                           ? std::string()
                                           : effect->GetUnsupportedReason();
        plan.requiresDepth = requirements.requiresDepth;
        plan.requiresNormal = requirements.requiresNormal;
        plan.requiresVelocity = requirements.requiresVelocity;
        plan.requiresHistory = requirements.requiresHistory;
        if (frameInputs)
        {
            plan.frameInputsSatisfied = requirements.IsSatisfiedBy(*frameInputs);
            plan.missingFrameInputReason = BuildMissingFrameInputReason(requirements, *frameInputs);
        }

        if (effect->IsRequestedEnabled())
        {
            stats.requestedEffectCount++;
            if (IsToneMappingPass(descriptor))
            {
                ++stats.requestedToneMappingEffectCount;
            }
        }

        if (IsToneMappingPass(descriptor) &&
            effect->IsRequestedEnabled() && effect->IsSupported())
        {
            ++stats.supportedToneMappingEffectCount;
        }

        if (effect->IsRequestedEnabled() && !effect->IsSupported())
        {
            stats.unsupportedSkippedCount++;
            plan.skippedReason = effect->GetUnsupportedReason().empty()
                                     ? "Unsupported"
                                     : effect->GetUnsupportedReason();
            plan.reason = plan.skippedReason;
            if (logUnsupported)
            {
                RVX_CORE_WARN(
                    "PostProcessStack: Skipping unsupported effect '{}': {}",
                descriptor.debugName ? descriptor.debugName : "<unnamed>",
                    effect->GetUnsupportedReason());
            }
        }

        if (effect->IsRequestedEnabled() && frameInputs && !plan.frameInputsSatisfied)
        {
            if (plan.skippedReason.empty())
            {
                plan.skippedReason = plan.missingFrameInputReason;
            }
            if (plan.reason.empty())
            {
                plan.reason = plan.missingFrameInputReason;
            }
            if (logUnsupported)
            {
                RVX_CORE_WARN(
                    "PostProcessStack: Skipping effect '{}' because {}",
                descriptor.debugName ? descriptor.debugName : "<unnamed>",
                    plan.missingFrameInputReason);
            }
        }

        if (!plan.requested)
        {
            plan.reason = "not requested";
        }

        if (effect->IsEnabled() && (!frameInputs || plan.frameInputsSatisfied))
        {
            enabledEffects.push_back(effect.get());
            if (IsToneMappingPass(descriptor))
            {
                ++stats.enabledToneMappingEffectCount;
            }
        }

        stats.effectPlans.push_back(std::move(plan));
    }
    stats.enabledEffectCount = static_cast<uint32>(enabledEffects.size());

    if (enabledEffects.empty())
    {
        stats.noEffectNoWork = true;
    }

    return enabledEffects;
}

PostProcessStackExecuteStats PostProcessStack::EvaluateEffects() const
{
    return EvaluateEffects(RHIFormat::RGBA8_UNORM);
}

PostProcessStackExecuteStats PostProcessStack::EvaluateEffects(
    RHIFormat finalOutputFormat) const
{
    PostProcessFrameInputs inputs;
    (void)ResolveLegacyOutputColorDomain(finalOutputFormat, inputs.outputColorDomain);

    PostProcessStackExecuteStats stats;
    (void)BuildPlan(inputs, finalOutputFormat, stats);
    return stats;
}

void PostProcessStack::Execute(RenderGraph& graph, RGTextureHandle sceneColor, RGTextureHandle output)
{
    PostProcessFrameInputs frameInputs;
    frameInputs.sceneColor = sceneColor;
    if (const RHITextureDesc* outputDesc = graph.GetTextureDesc(output))
    {
        frameInputs.outputFormat = outputDesc->format;
        (void)ResolveLegacyOutputColorDomain(outputDesc->format, frameInputs.outputColorDomain);
    }
    Execute(graph, frameInputs, output);
}

void PostProcessStack::Execute(RenderGraph& graph,
                               const PostProcessFrameInputs& frameInputs,
                               RGTextureHandle output)
{
    PostProcessFrameInputs resolvedInputs = frameInputs;
    RHIFormat finalOutputFormat = resolvedInputs.outputFormat;
    if (const RHITextureDesc* outputDesc = graph.GetTextureDesc(output))
    {
        finalOutputFormat = outputDesc->format;
        resolvedInputs.outputFormat = outputDesc->format;
    }
    if (resolvedInputs.outputColorDomain == PostProcessColorDomain::Unknown)
    {
        // Compatibility entry points may derive a domain from a format. New
        // feature composition must supply the semantic domain explicitly.
        (void)ResolveLegacyOutputColorDomain(finalOutputFormat, resolvedInputs.outputColorDomain);
    }

    PostProcessStackExecuteStats plan;
    if (!BuildPlan(resolvedInputs, finalOutputFormat, plan))
    {
        m_lastExecuteStats = std::move(plan);
        RVX_CORE_WARN("PostProcessStack: typed plan rejected: {}", m_lastExecuteStats.planFailureReason);
        return;
    }

    (void)ExecutePlan(graph, resolvedInputs, output, plan);
}

bool PostProcessStack::ValidateDescriptor(const PostProcessPassDescriptor& descriptor,
                                          std::string& outReason) const
{
    outReason.clear();
    const char* canonicalName = GetPostProcessPassCanonicalName(descriptor.id);
    if (!canonicalName || !descriptor.debugName || std::string(descriptor.debugName) != canonicalName)
    {
        outReason = "post-process descriptor has an invalid stable id or non-canonical debug name";
        return false;
    }

    if (descriptor.inputDomain == PostProcessColorDomain::Unknown ||
        descriptor.outputDomain == PostProcessColorDomain::Unknown)
    {
        outReason = "post-process descriptor has an unknown color domain";
        return false;
    }

    if (descriptor.id == PostProcessPassId::ToneMapping)
    {
        if (descriptor.stage != PostProcessStage::ToneMap ||
            descriptor.inputDomain != PostProcessColorDomain::SceneLinearHDR ||
            !IsDisplayDomain(descriptor.outputDomain) ||
            (descriptor.transfer != PostProcessTransfer::SceneLinearToDisplayLinear &&
             descriptor.transfer != PostProcessTransfer::SceneLinearToDisplayEncoded))
        {
            outReason = "ToneMapping must be the sole SceneLinearHDR-to-display transfer";
            return false;
        }
        return true;
    }

    if (descriptor.stage == PostProcessStage::ToneMap)
    {
        outReason = "only the ToneMapping stable pass id may use the ToneMap stage";
        return false;
    }

    if (descriptor.stage == PostProcessStage::Temporal ||
        descriptor.stage == PostProcessStage::HDRPost)
    {
        if (descriptor.inputDomain != PostProcessColorDomain::SceneLinearHDR ||
            descriptor.outputDomain != PostProcessColorDomain::SceneLinearHDR ||
            descriptor.transfer != PostProcessTransfer::None)
        {
            outReason = "Temporal/HDR post-process passes must preserve SceneLinearHDR";
            return false;
        }
        return true;
    }

    if (descriptor.stage == PostProcessStage::LDRPost || descriptor.stage == PostProcessStage::Overlay)
    {
        if (!IsDisplayDomain(descriptor.inputDomain) ||
            descriptor.inputDomain != descriptor.outputDomain ||
            descriptor.transfer != PostProcessTransfer::None)
        {
            outReason = "LDR/overlay post-process passes must preserve one explicit display domain";
            return false;
        }
        return true;
    }

    outReason = "post-process descriptor has an unsupported stage";
    return false;
}

bool PostProcessStack::ResolveLegacyOutputColorDomain(
    RHIFormat format,
    PostProcessColorDomain& outDomain) const
{
    switch (format)
    {
        case RHIFormat::RGBA8_UNORM_SRGB:
        case RHIFormat::BGRA8_UNORM_SRGB:
            // The render target performs the final hardware sRGB encode.
            outDomain = PostProcessColorDomain::DisplayLinearLDR;
            return true;
        case RHIFormat::RGBA8_UNORM:
        case RHIFormat::BGRA8_UNORM:
        case RHIFormat::RGB10A2_UNORM:
            outDomain = PostProcessColorDomain::DisplayEncodedLDR;
            return true;
        case RHIFormat::Unknown:
            outDomain = PostProcessColorDomain::Unknown;
            return false;
        default:
            outDomain = IsLDROutputFormat(format)
                ? PostProcessColorDomain::DisplayEncodedLDR
                : PostProcessColorDomain::SceneLinearHDR;
            return true;
    }
}

bool PostProcessStack::BuildPlan(const PostProcessFrameInputs& frameInputs,
                                 RHIFormat finalOutputFormat,
                                 PostProcessStackExecuteStats& outStats) const
{
    return BuildPlan(frameInputs,
                     finalOutputFormat,
                     PostProcessPlanContract::Generic,
                     outStats);
}

bool PostProcessStack::BuildPlan(const PostProcessFrameInputs& frameInputs,
                                 RHIFormat finalOutputFormat,
                                 PostProcessPlanContract contract,
                                 PostProcessStackExecuteStats& outStats) const
{
    outStats = {};
    SetQualityPresetStats(outStats, m_settings);
    outStats.frameInputs = frameInputs;
    outStats.finalOutputFormat = finalOutputFormat;
    outStats.inputColorDomain = frameInputs.sceneColorDomain;
    outStats.outputColorDomain = frameInputs.outputColorDomain;
    outStats.planContract = contract;
    outStats.planValid = false;

    const std::vector<IPostProcessPass*> enabledEffects =
        GatherEnabledEffects(outStats, false, &frameInputs);

    auto fail = [&outStats](const std::string& reason)
    {
        outStats.planValid = false;
        outStats.planFailureReason = reason;
        outStats.toneMappingBoundaryValid = false;
        outStats.toneMappingBoundaryWarning = reason;
        for (PostProcessEffectExecutionPlan& effectPlan : outStats.effectPlans)
        {
            if (effectPlan.enabled && effectPlan.skippedReason.empty())
            {
                effectPlan.skippedReason = reason;
                effectPlan.reason = reason;
            }
        }
        return false;
    };

    if (frameInputs.sceneColorDomain == PostProcessColorDomain::Unknown ||
        frameInputs.outputColorDomain == PostProcessColorDomain::Unknown)
    {
        return fail("post-process plan requires explicit input and output color domains");
    }
    if (contract != PostProcessPlanContract::Generic &&
        contract != PostProcessPlanContract::Presentation)
    {
        return fail("post-process plan has an unknown consumer contract");
    }

    std::vector<PostProcessPassId> seenIds;
    seenIds.reserve(outStats.effectPlans.size());
    for (const PostProcessEffectExecutionPlan& plan : outStats.effectPlans)
    {
        if (!plan.requested)
        {
            continue;
        }

        const PostProcessPassDescriptor descriptor{
            plan.passId,
            plan.effectName.c_str(),
            plan.stage,
            plan.order,
            plan.inputDomain,
            plan.outputDomain,
            plan.transfer};
        std::string descriptorReason;
        if (!ValidateDescriptor(descriptor, descriptorReason))
        {
            return fail(descriptorReason);
        }

        if (std::find(seenIds.begin(), seenIds.end(), plan.passId) != seenIds.end())
        {
            return fail("post-process plan contains a duplicate stable pass id");
        }
        seenIds.push_back(plan.passId);
    }

    if (outStats.requestedEffectCount != outStats.enabledEffectCount ||
        outStats.unsupportedSkippedCount != 0)
    {
        return fail("requested post-process pass is unsupported or missing required frame inputs");
    }

    if (contract == PostProcessPlanContract::Presentation &&
        outStats.enabledToneMappingEffectCount != 1)
    {
        return fail("presentation post-process plan requires exactly one enabled ToneMapping transform");
    }

    if (enabledEffects.empty())
    {
        outStats.noEffectNoWork = true;
        if (frameInputs.sceneColorDomain != frameInputs.outputColorDomain)
        {
            return fail("an empty post-process chain cannot change color domain");
        }

        outStats.planValid = true;
        outStats.toneMappingBoundaryValid = true;
        outStats.toneMappingBoundaryWarning.clear();
        outStats.planFingerprint = ComputePlanFingerprint(outStats);
        return true;
    }

    if (outStats.enabledToneMappingEffectCount != 1)
    {
        return fail("post-process plan requires exactly one enabled ToneMapping transform");
    }

    PostProcessColorDomain currentDomain = frameInputs.sceneColorDomain;
    uint32 previousStage = 0;
    int32 previousOrder = 0;
    PostProcessPassId previousId = PostProcessPassId::Invalid;
    bool first = true;
    for (PostProcessEffectExecutionPlan& plan : outStats.effectPlans)
    {
        if (!plan.enabled)
        {
            continue;
        }

        const uint32 stageOrder = GetStageOrder(plan.stage);
        if (!first &&
            (stageOrder < previousStage ||
             (stageOrder == previousStage &&
              (plan.order < previousOrder ||
               (plan.order == previousOrder &&
                static_cast<uint16>(plan.passId) <= static_cast<uint16>(previousId))))))
        {
            return fail("post-process plan is not ordered by stage, order, and stable id");
        }
        if (plan.inputDomain != currentDomain)
        {
            return fail("post-process plan contains an illegal color-domain transition");
        }

        currentDomain = plan.outputDomain;
        previousStage = stageOrder;
        previousOrder = plan.order;
        previousId = plan.passId;
        first = false;
    }

    if (currentDomain != frameInputs.outputColorDomain)
    {
        return fail("post-process plan final color domain does not match the presentation contract");
    }

    outStats.planValid = true;
    outStats.toneMappingBoundaryValid = true;
    outStats.toneMappingBoundaryWarning.clear();
    outStats.planFingerprint = ComputePlanFingerprint(outStats);
    return true;
}

bool PostProcessStack::ExecutePlan(RenderGraph& graph,
                                   const PostProcessFrameInputs& frameInputs,
                                   RGTextureHandle output,
                                   const PostProcessStackExecuteStats& plan)
{
    m_lastExecuteStats = plan;
    if (!plan.planValid || plan.planFingerprint == 0)
    {
        m_lastExecuteStats.planFailureReason = plan.planFailureReason.empty()
            ? "ExecutePlan requires a valid immutable typed plan"
            : plan.planFailureReason;
        m_lastExecuteStats.toneMappingBoundaryValid = false;
        return false;
    }

    const RHITextureDesc* sceneDescPtr = graph.GetTextureDesc(frameInputs.sceneColor);
    const RHITextureDesc* finalDescPtr = graph.GetTextureDesc(output);
    if (!sceneDescPtr || !finalDescPtr ||
        (plan.finalOutputFormat != RHIFormat::Unknown && plan.finalOutputFormat != finalDescPtr->format))
    {
        m_lastExecuteStats.planValid = false;
        m_lastExecuteStats.planFailureReason = "typed post-process plan does not match graph texture descriptions";
        m_lastExecuteStats.toneMappingBoundaryValid = false;
        m_lastExecuteStats.toneMappingBoundaryWarning = m_lastExecuteStats.planFailureReason;
        return false;
    }

    if (frameInputs.sceneColorDomain != plan.frameInputs.sceneColorDomain ||
        frameInputs.outputColorDomain != plan.frameInputs.outputColorDomain ||
        frameInputs.outputFormat != plan.frameInputs.outputFormat)
    {
        m_lastExecuteStats.planValid = false;
        m_lastExecuteStats.planFailureReason =
            "typed post-process plan input color-domain or format changed after BuildPlan";
        m_lastExecuteStats.toneMappingBoundaryValid = false;
        m_lastExecuteStats.toneMappingBoundaryWarning =
            m_lastExecuteStats.planFailureReason;
        return false;
    }

    PostProcessStackExecuteStats revalidatedPlan;
    if (!BuildPlan(frameInputs,
                   finalDescPtr->format,
                   plan.planContract,
                   revalidatedPlan) ||
        !revalidatedPlan.planValid ||
        revalidatedPlan.planFingerprint != plan.planFingerprint)
    {
        m_lastExecuteStats.planValid = false;
        m_lastExecuteStats.planFailureReason = !revalidatedPlan.planValid &&
                !revalidatedPlan.planFailureReason.empty()
            ? "post-process plan is no longer valid: " + revalidatedPlan.planFailureReason
            : "post-process immutable plan fingerprint changed after BuildPlan";
        m_lastExecuteStats.toneMappingBoundaryValid = false;
        m_lastExecuteStats.toneMappingBoundaryWarning =
            m_lastExecuteStats.planFailureReason;
        return false;
    }

    PostProcessStackExecuteStats currentSelection;
    const std::vector<IPostProcessPass*> enabledEffects =
        GatherEnabledEffects(currentSelection, false, &frameInputs);
    if (enabledEffects.size() != plan.enabledEffectCount)
    {
        m_lastExecuteStats.planValid = false;
        m_lastExecuteStats.planFailureReason = "post-process pass selection changed after BuildPlan";
        m_lastExecuteStats.toneMappingBoundaryValid = false;
        m_lastExecuteStats.toneMappingBoundaryWarning = m_lastExecuteStats.planFailureReason;
        return false;
    }

    size_t selectedIndex = 0;
    for (const PostProcessEffectExecutionPlan& currentPlan : currentSelection.effectPlans)
    {
        if (!currentPlan.enabled)
        {
            continue;
        }
        while (selectedIndex < plan.effectPlans.size() && !plan.effectPlans[selectedIndex].enabled)
        {
            ++selectedIndex;
        }
        if (selectedIndex == plan.effectPlans.size())
        {
            m_lastExecuteStats.planValid = false;
            m_lastExecuteStats.planFailureReason = "post-process plan selection is internally inconsistent";
            m_lastExecuteStats.toneMappingBoundaryValid = false;
            m_lastExecuteStats.toneMappingBoundaryWarning = m_lastExecuteStats.planFailureReason;
            return false;
        }

        const PostProcessEffectExecutionPlan& planned = plan.effectPlans[selectedIndex++];
        const PostProcessPassDescriptor currentDescriptor{
            currentPlan.passId,
            currentPlan.effectName.c_str(),
            currentPlan.stage,
            currentPlan.order,
            currentPlan.inputDomain,
            currentPlan.outputDomain,
            currentPlan.transfer};
        const PostProcessPassDescriptor plannedDescriptor{
            planned.passId,
            planned.effectName.c_str(),
            planned.stage,
            planned.order,
            planned.inputDomain,
            planned.outputDomain,
            planned.transfer};
        if (!IsDescriptorEqual(currentDescriptor, plannedDescriptor))
        {
            m_lastExecuteStats.planValid = false;
            m_lastExecuteStats.planFailureReason = "post-process pass descriptor changed after BuildPlan";
            m_lastExecuteStats.toneMappingBoundaryValid = false;
            m_lastExecuteStats.toneMappingBoundaryWarning = m_lastExecuteStats.planFailureReason;
            return false;
        }
    }

    if (enabledEffects.empty())
    {
        if (plan.planContract == PostProcessPlanContract::Presentation)
        {
            m_lastExecuteStats.planValid = false;
            m_lastExecuteStats.planFailureReason =
                "presentation post-process execution requires one ToneMapping transform";
            m_lastExecuteStats.toneMappingBoundaryValid = false;
            m_lastExecuteStats.toneMappingBoundaryWarning =
                m_lastExecuteStats.planFailureReason;
            return false;
        }
        if (frameInputs.sceneColorDomain != frameInputs.outputColorDomain)
        {
            m_lastExecuteStats.planValid = false;
            m_lastExecuteStats.planFailureReason = "identity post-process execution cannot change color domain";
            m_lastExecuteStats.toneMappingBoundaryValid = false;
            m_lastExecuteStats.toneMappingBoundaryWarning = m_lastExecuteStats.planFailureReason;
            return false;
        }
        return AddIdentityCopyPass(graph,
                                   frameInputs.sceneColor,
                                   output,
                                   m_lastExecuteStats,
                                   "empty identity post-process plan copied scene color to output");
    }

    const RHITextureDesc sceneDesc = *sceneDescPtr;
    const RHITextureDesc finalDesc = *finalDescPtr;
    std::vector<RGTextureHandle> intermediates;
    intermediates.reserve(enabledEffects.size() - 1);

    size_t plannedIndex = 0;
    for (size_t effectIndex = 0; effectIndex + 1 < enabledEffects.size(); ++effectIndex)
    {
        while (plannedIndex < m_lastExecuteStats.effectPlans.size() &&
               !m_lastExecuteStats.effectPlans[plannedIndex].enabled)
        {
            ++plannedIndex;
        }
        if (plannedIndex == m_lastExecuteStats.effectPlans.size())
        {
            m_lastExecuteStats.planValid = false;
            m_lastExecuteStats.planFailureReason = "post-process intermediate plan is internally inconsistent";
            return false;
        }

        const PostProcessEffectExecutionPlan& effectPlan = m_lastExecuteStats.effectPlans[plannedIndex++];
        RHITextureDesc intermediateDesc = effectPlan.outputDomain == PostProcessColorDomain::SceneLinearHDR
            ? sceneDesc
            : finalDesc;
        intermediateDesc.usage = RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource;
        intermediateDesc.debugName = "PostProcessIntermediate";
        intermediates.push_back(graph.CreateTexture(intermediateDesc));
        m_lastExecuteStats.transientIntermediateFormat = intermediateDesc.format;
        if (effectPlan.outputDomain == PostProcessColorDomain::SceneLinearHDR)
        {
            ++m_lastExecuteStats.hdrIntermediateCount;
            m_lastExecuteStats.hdrIntermediateFormat = intermediateDesc.format;
        }
        else
        {
            ++m_lastExecuteStats.ldrIntermediateCount;
            m_lastExecuteStats.ldrIntermediateFormat = intermediateDesc.format;
        }
    }
    m_lastExecuteStats.transientIntermediateCount = static_cast<uint32>(intermediates.size());

    RGTextureHandle currentInput = frameInputs.sceneColor;
    plannedIndex = 0;
    for (size_t effectIndex = 0; effectIndex < enabledEffects.size(); ++effectIndex)
    {
        while (plannedIndex < m_lastExecuteStats.effectPlans.size() &&
               !m_lastExecuteStats.effectPlans[plannedIndex].enabled)
        {
            ++plannedIndex;
        }
        if (plannedIndex == m_lastExecuteStats.effectPlans.size())
        {
            m_lastExecuteStats.planValid = false;
            m_lastExecuteStats.planFailureReason = "post-process execution plan is internally inconsistent";
            return false;
        }

        PostProcessEffectExecutionPlan& effectPlan = m_lastExecuteStats.effectPlans[plannedIndex++];
        const bool isLast = effectIndex + 1 == enabledEffects.size();
        const RGTextureHandle currentOutput = isLast ? output : intermediates[effectIndex];
        const RHITextureDesc* inputDesc = graph.GetTextureDesc(currentInput);
        const RHITextureDesc* outputDesc = graph.GetTextureDesc(currentOutput);
        if (!inputDesc || !outputDesc)
        {
            m_lastExecuteStats.planValid = false;
            m_lastExecuteStats.planFailureReason = "post-process graph resources disappeared after plan validation";
            return false;
        }

        effectPlan.scheduled = true;
        ++m_lastExecuteStats.scheduledEffectCount;
        if (effectPlan.passId == PostProcessPassId::ToneMapping)
        {
            ++m_lastExecuteStats.scheduledToneMappingEffectCount;
        }
        effectPlan.inputFormat = inputDesc->format;
        effectPlan.outputFormat = outputDesc->format;
        effectPlan.inputIsSceneColor = effectIndex == 0;
        effectPlan.outputIsTransientIntermediate = !isLast;
        effectPlan.outputIsFinalTarget = isLast;

        PostProcessFrameInputs passInputs = frameInputs;
        passInputs.sceneColor = currentInput;
        const size_t passCountBefore = graph.GetRecordedPassCount();
        enabledEffects[effectIndex]->AddToGraph(graph, passInputs, currentOutput);
        if (!AccumulateRecordedGraphPasses(
                graph,
                passCountBefore,
                m_lastExecuteStats,
                GetPostProcessPassCanonicalName(effectPlan.passId)))
        {
            return false;
        }
        currentInput = currentOutput;
    }

    return true;
}

} // namespace RVX
