/**
 * @file PostProcessStack.cpp
 * @brief PostProcessStack implementation
 */

#include "Render/PostProcess/PostProcessStack.h"
#include "Core/Log.h"
#include "RHI/RHICommandContext.h"
#include <algorithm>
#include <utility>

namespace RVX
{

namespace
{
    bool IsToneMappingEffect(const IPostProcessPass* effect)
    {
        return effect && std::string(effect->GetName()) == "ToneMapping";
    }

    bool IsLDRPostToneMappingEffect(const IPostProcessPass* effect)
    {
        if (!effect)
        {
            return false;
        }

        const std::string name = effect->GetName();
        return name == "ChromaticAberration" ||
               name == "ColorGrading" ||
               name == "FilmGrain" ||
               name == "FXAA" ||
               name == "Vignette";
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

    PostProcessColorDomain GetColorDomainForFormat(RHIFormat format)
    {
        if (format == RHIFormat::Unknown)
        {
            return PostProcessColorDomain::Unknown;
        }

        return IsLDROutputFormat(format) ? PostProcessColorDomain::LDR : PostProcessColorDomain::HDR;
    }

    void MarkEnabledEffectPlansSkipped(PostProcessStackExecuteStats& stats, const std::string& reason)
    {
        for (PostProcessEffectExecutionPlan& plan : stats.effectPlans)
        {
            if (plan.enabled)
            {
                plan.skippedReason = reason;
                plan.reason = reason;
            }
        }
    }

    bool ValidateToneMappingBoundary(const std::vector<IPostProcessPass*>& enabledEffects,
                                     RHIFormat finalOutputFormat,
                                     PostProcessStackExecuteStats& stats,
                                     bool logWarnings)
    {
        bool seenToneMapping = false;
        for (IPostProcessPass* effect : enabledEffects)
        {
            if (IsLDRPostToneMappingEffect(effect) && !seenToneMapping)
            {
                stats.toneMappingBoundaryValid = false;
                stats.toneMappingBoundaryWarning =
                    "LDR post-process effects require ToneMapping before them";
                if (logWarnings)
                {
                    RVX_CORE_WARN("PostProcessStack: {}", stats.toneMappingBoundaryWarning);
                }
                return false;
            }

            if (IsToneMappingEffect(effect))
            {
                seenToneMapping = true;
                continue;
            }

            if (seenToneMapping && !IsLDRPostToneMappingEffect(effect))
            {
                stats.toneMappingBoundaryValid = false;
                stats.toneMappingBoundaryWarning =
                    "Only LDR post-process effects may run after ToneMapping";
                if (logWarnings)
                {
                    RVX_CORE_WARN("PostProcessStack: {}", stats.toneMappingBoundaryWarning);
                }
                return false;
            }
        }

        if (!seenToneMapping && IsLDROutputFormat(finalOutputFormat))
        {
            stats.toneMappingBoundaryValid = false;
            stats.toneMappingBoundaryWarning =
                "HDR post-process output requires ToneMapping before writing an LDR target";
            if (logWarnings)
            {
                RVX_CORE_WARN("PostProcessStack: {}", stats.toneMappingBoundaryWarning);
            }
            return false;
        }

        stats.toneMappingBoundaryValid = true;
        stats.toneMappingBoundaryWarning.clear();
        return true;
    }

    bool AreCopyCompatible(const RHITextureDesc& srcDesc, const RHITextureDesc& dstDesc)
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

    bool AddFallbackCopyPass(RenderGraph& graph,
                             RGTextureHandle input,
                             RGTextureHandle output,
                             PostProcessStackExecuteStats& stats,
                             const char* reason)
    {
        if (!input.IsValid() || !output.IsValid())
        {
            stats.fallbackCopyReason = "fallback copy skipped because input or output handle is invalid";
            return false;
        }

        const RHITextureDesc* inputDesc = graph.GetTextureDesc(input);
        const RHITextureDesc* outputDesc = graph.GetTextureDesc(output);
        if (!inputDesc || !outputDesc)
        {
            stats.fallbackCopyReason = "fallback copy skipped because texture descriptions are unavailable";
            return false;
        }

        stats.finalOutputFormat = outputDesc->format;
        if (!AreCopyCompatible(*inputDesc, *outputDesc))
        {
            stats.fallbackCopyReason = "fallback copy skipped because input and output textures are not copy-compatible";
            RVX_CORE_WARN("PostProcessStack: {}", stats.fallbackCopyReason);
            return false;
        }

        struct FallbackCopyData
        {
            RGTextureHandle input;
            RGTextureHandle output;
        };

        graph.AddPass<FallbackCopyData>(
            "PostProcessFallbackCopy",
            RenderGraphPassType::Copy,
            [input, output](RenderGraphBuilder& builder, FallbackCopyData& data)
            {
                data.input = builder.Read(input, RHIResourceState::CopySource);
                data.output = builder.Write(output, RHIResourceState::CopyDest);
            },
            [&graph](const FallbackCopyData& data, RHICommandContext& ctx)
            {
                RHITexture* inputTexture = graph.GetTexture(data.input);
                RHITexture* outputTexture = graph.GetTexture(data.output);
                if (!inputTexture || !outputTexture)
                {
                    RVX_CORE_WARN("PostProcessStack: fallback copy skipped because textures are unavailable");
                    return;
                }

                ctx.CopyTexture(inputTexture, outputTexture);
            });

        stats.fallbackCopyApplied = true;
        stats.fallbackCopyPassCount++;
        stats.graphPassCount++;
        stats.fallbackCopyReason = reason ? reason : "fallback copy";
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
            settings.enableSSAO = true;
            return;
        case RenderVisualQualityPreset::Cinematic:
            settings.enableToneMapping = true;
            settings.enableBloom = true;
            settings.enableFXAA = true;
            settings.enableColorGrading = true;
            settings.enableVignette = true;
            settings.enableChromaticAberration = true;
            settings.enableFilmGrain = true;
            settings.enableSSAO = true;
            settings.enableSSR = true;
            settings.enableTAA = true;
            settings.enableDOF = true;
            settings.enableMotionBlur = true;
            settings.enableVolumetricLighting = true;
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
            return a->GetPriority() < b->GetPriority();
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
        const PostProcessFrameInputRequirements requirements = effect->GetFrameInputRequirements();

        PostProcessEffectExecutionPlan plan;
        plan.effectName = effect->GetName() ? effect->GetName() : "";
        plan.sequenceIndex = static_cast<uint32>(stats.effectPlans.size());
        plan.priority = effect->GetPriority();
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
                    effect->GetName(),
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
                    effect->GetName(),
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
    PostProcessStackExecuteStats stats;
    SetQualityPresetStats(stats, m_settings);
    std::vector<IPostProcessPass*> enabledEffects = GatherEnabledEffects(stats, false, nullptr);
    if (!enabledEffects.empty())
    {
        (void)ValidateToneMappingBoundary(enabledEffects, RHIFormat::Unknown, stats, false);
    }
    return stats;
}

void PostProcessStack::Execute(RenderGraph& graph, RGTextureHandle sceneColor, RGTextureHandle output)
{
    PostProcessFrameInputs frameInputs;
    frameInputs.sceneColor = sceneColor;
    if (const RHITextureDesc* outputDesc = graph.GetTextureDesc(output))
    {
        frameInputs.outputFormat = outputDesc->format;
    }
    Execute(graph, frameInputs, output);
}

void PostProcessStack::Execute(RenderGraph& graph,
                               const PostProcessFrameInputs& frameInputs,
                               RGTextureHandle output)
{
    m_lastExecuteStats = {};
    SetQualityPresetStats(m_lastExecuteStats, m_settings);
    m_lastExecuteStats.frameInputs = frameInputs;

    std::vector<IPostProcessPass*> enabledEffects = GatherEnabledEffects(m_lastExecuteStats, true, &frameInputs);

    if (enabledEffects.empty())
    {
        m_lastExecuteStats.noEffectNoWork = true;
        AddFallbackCopyPass(graph,
                            frameInputs.sceneColor,
                            output,
                            m_lastExecuteStats,
                            "no supported enabled effects; copied scene color to output");
        return;
    }

    if (const RHITextureDesc* outputDesc = graph.GetTextureDesc(output))
    {
        m_lastExecuteStats.finalOutputFormat = outputDesc->format;
    }

    if (!ValidateToneMappingBoundary(enabledEffects, m_lastExecuteStats.finalOutputFormat, m_lastExecuteStats, true))
    {
        RVX_CORE_WARN("PostProcessStack: invalid ToneMapping boundary; skipping post-process execution");
        MarkEnabledEffectPlansSkipped(m_lastExecuteStats, m_lastExecuteStats.toneMappingBoundaryWarning);
        AddFallbackCopyPass(graph,
                            frameInputs.sceneColor,
                            output,
                            m_lastExecuteStats,
                            "invalid ToneMapping boundary; copied scene color to output");
        return;
    }

    std::vector<RGTextureHandle> intermediates;
    if (enabledEffects.size() > 1)
    {
        const RHITextureDesc* sceneDescPtr = graph.GetTextureDesc(frameInputs.sceneColor);
        const RHITextureDesc* outputDescPtr = graph.GetTextureDesc(output);
        if (!sceneDescPtr || !outputDescPtr)
        {
            m_lastExecuteStats.noEffectNoWork = true;
            RVX_CORE_WARN("PostProcessStack: cannot create intermediate textures without valid scene and output descriptions");
            return;
        }

        const RHITextureDesc sceneDesc = *sceneDescPtr;
        const RHITextureDesc outputDesc = *outputDescPtr;

        intermediates.reserve(enabledEffects.size() - 1);
        bool ldrDomain = false;
        for (size_t i = 0; i + 1 < enabledEffects.size(); ++i)
        {
            const bool writesLDR = ldrDomain || IsToneMappingEffect(enabledEffects[i]);
            RHITextureDesc intermediateDesc = writesLDR ? outputDesc : sceneDesc;
            intermediateDesc.usage = RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource;
            intermediateDesc.debugName = "PostProcessIntermediate";
            intermediates.push_back(graph.CreateTexture(intermediateDesc));
            m_lastExecuteStats.transientIntermediateFormat = intermediateDesc.format;
            if (writesLDR)
            {
                m_lastExecuteStats.ldrIntermediateCount++;
                m_lastExecuteStats.ldrIntermediateFormat = intermediateDesc.format;
            }
            else
            {
                m_lastExecuteStats.hdrIntermediateCount++;
                m_lastExecuteStats.hdrIntermediateFormat = intermediateDesc.format;
            }

            if (IsToneMappingEffect(enabledEffects[i]))
            {
                ldrDomain = true;
            }
        }
        m_lastExecuteStats.transientIntermediateCount = static_cast<uint32>(intermediates.size());
    }

    RGTextureHandle currentInput = frameInputs.sceneColor;
    size_t nextPlanIndex = 0;

    for (size_t i = 0; i < enabledEffects.size(); ++i)
    {
        bool isLast = (i == enabledEffects.size() - 1);
        RGTextureHandle currentOutput = isLast ? output : intermediates[i];

        PostProcessEffectExecutionPlan* plan = nullptr;
        while (nextPlanIndex < m_lastExecuteStats.effectPlans.size())
        {
            PostProcessEffectExecutionPlan& candidate = m_lastExecuteStats.effectPlans[nextPlanIndex++];
            if (candidate.enabled)
            {
                plan = &candidate;
                break;
            }
        }

        if (plan)
        {
            const RHITextureDesc* inputDesc = graph.GetTextureDesc(currentInput);
            const RHITextureDesc* outputDesc = graph.GetTextureDesc(currentOutput);
            plan->scheduled = true;
            m_lastExecuteStats.scheduledEffectCount++;
            plan->inputFormat = inputDesc ? inputDesc->format : RHIFormat::Unknown;
            plan->outputFormat = outputDesc ? outputDesc->format : RHIFormat::Unknown;
            plan->inputDomain = GetColorDomainForFormat(plan->inputFormat);
            plan->outputDomain = GetColorDomainForFormat(plan->outputFormat);
            plan->inputIsSceneColor = (i == 0);
            plan->outputIsTransientIntermediate = !isLast;
            plan->outputIsFinalTarget = isLast;
        }

        PostProcessFrameInputs passInputs = frameInputs;
        passInputs.sceneColor = currentInput;
        enabledEffects[i]->AddToGraph(graph, passInputs, currentOutput);
        m_lastExecuteStats.graphPassCount++;
        currentInput = currentOutput;
    }
}

} // namespace RVX
