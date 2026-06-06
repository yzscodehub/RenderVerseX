/**
 * @file PostProcessStack.cpp
 * @brief PostProcessStack implementation
 */

#include "Render/PostProcess/PostProcessStack.h"
#include "Core/Log.h"
#include <algorithm>

namespace RVX
{

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

void PostProcessStack::Execute(RenderGraph& graph, RGTextureHandle sceneColor, RGTextureHandle output)
{
    m_lastExecuteStats = {};

    // Count enabled effects
    std::vector<IPostProcessPass*> enabledEffects;
    for (auto& effect : m_effects)
    {
        if (effect->IsRequestedEnabled())
        {
            m_lastExecuteStats.requestedEffectCount++;
        }

        if (effect->IsRequestedEnabled() && !effect->IsSupported())
        {
            m_lastExecuteStats.unsupportedSkippedCount++;
            RVX_CORE_WARN(
                "PostProcessStack: Skipping unsupported effect '{}': {}",
                effect->GetName(),
                effect->GetUnsupportedReason());
        }

        if (effect->IsEnabled())
        {
            enabledEffects.push_back(effect.get());
        }
    }
    m_lastExecuteStats.enabledEffectCount = static_cast<uint32>(enabledEffects.size());

    if (enabledEffects.empty())
    {
        m_lastExecuteStats.noEffectNoWork = true;
        RVX_CORE_WARN("PostProcessStack: no supported enabled effects; no copy pass is implemented");
        return;
    }

    std::vector<RGTextureHandle> intermediates;
    if (enabledEffects.size() > 1)
    {
        const RHITextureDesc* sceneDesc = graph.GetTextureDesc(sceneColor);
        if (!sceneDesc)
        {
            m_lastExecuteStats.noEffectNoWork = true;
            RVX_CORE_WARN("PostProcessStack: cannot create intermediate textures without a valid scene color description");
            return;
        }

        intermediates.reserve(enabledEffects.size() - 1);
        for (size_t i = 0; i + 1 < enabledEffects.size(); ++i)
        {
            RHITextureDesc intermediateDesc = *sceneDesc;
            intermediateDesc.usage = RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource;
            intermediateDesc.debugName = "PostProcessIntermediate";
            intermediates.push_back(graph.CreateTexture(intermediateDesc));
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
