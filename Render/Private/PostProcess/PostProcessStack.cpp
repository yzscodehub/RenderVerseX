/**
 * @file PostProcessStack.cpp
 * @brief PostProcessStack implementation
 */

#include "Render/PostProcess/PostProcessStack.h"
#include "Core/Log.h"
#include <algorithm>

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
        return name == "ColorGrading" || name == "FXAA" || name == "Vignette";
    }
} // namespace

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
                                                                      bool logUnsupported) const
{
    std::vector<IPostProcessPass*> enabledEffects;
    for (const auto& effect : m_effects)
    {
        if (effect->IsRequestedEnabled())
        {
            stats.requestedEffectCount++;
        }

        if (effect->IsRequestedEnabled() && !effect->IsSupported())
        {
            stats.unsupportedSkippedCount++;
            if (logUnsupported)
            {
                RVX_CORE_WARN(
                    "PostProcessStack: Skipping unsupported effect '{}': {}",
                    effect->GetName(),
                    effect->GetUnsupportedReason());
            }
        }

        if (effect->IsEnabled())
        {
            enabledEffects.push_back(effect.get());
        }
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
    (void)GatherEnabledEffects(stats, false);
    return stats;
}

void PostProcessStack::Execute(RenderGraph& graph, RGTextureHandle sceneColor, RGTextureHandle output)
{
    m_lastExecuteStats = {};

    std::vector<IPostProcessPass*> enabledEffects = GatherEnabledEffects(m_lastExecuteStats, true);

    if (enabledEffects.empty())
    {
        m_lastExecuteStats.noEffectNoWork = true;
        RVX_CORE_WARN("PostProcessStack: no supported enabled effects; no copy pass is implemented");
        return;
    }

    if (const RHITextureDesc* outputDesc = graph.GetTextureDesc(output))
    {
        m_lastExecuteStats.finalOutputFormat = outputDesc->format;
    }

    bool seenToneMapping = false;
    bool invalidToneMappingBoundary = false;
    for (IPostProcessPass* effect : enabledEffects)
    {
        if (IsLDRPostToneMappingEffect(effect) && !seenToneMapping)
        {
            m_lastExecuteStats.toneMappingBoundaryValid = false;
            m_lastExecuteStats.toneMappingBoundaryWarning =
                "LDR post-process effects require ToneMapping before them";
            RVX_CORE_WARN("PostProcessStack: {}", m_lastExecuteStats.toneMappingBoundaryWarning);
            invalidToneMappingBoundary = true;
            break;
        }

        if (IsToneMappingEffect(effect))
        {
            seenToneMapping = true;
            continue;
        }

        if (seenToneMapping && !IsLDRPostToneMappingEffect(effect))
        {
            m_lastExecuteStats.toneMappingBoundaryValid = false;
            m_lastExecuteStats.toneMappingBoundaryWarning =
                "Only LDR post-process effects may run after ToneMapping";
            RVX_CORE_WARN("PostProcessStack: {}", m_lastExecuteStats.toneMappingBoundaryWarning);
            invalidToneMappingBoundary = true;
            break;
        }
    }

    if (invalidToneMappingBoundary)
    {
        RVX_CORE_WARN("PostProcessStack: invalid ToneMapping boundary; skipping post-process execution");
        return;
    }

    std::vector<RGTextureHandle> intermediates;
    if (enabledEffects.size() > 1)
    {
        const RHITextureDesc* sceneDescPtr = graph.GetTextureDesc(sceneColor);
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

    RGTextureHandle currentInput = sceneColor;

    for (size_t i = 0; i < enabledEffects.size(); ++i)
    {
        bool isLast = (i == enabledEffects.size() - 1);
        RGTextureHandle currentOutput = isLast ? output : intermediates[i];

        enabledEffects[i]->AddToGraph(graph, currentInput, currentOutput);
        m_lastExecuteStats.graphPassCount++;
        currentInput = currentOutput;
    }
}

} // namespace RVX
