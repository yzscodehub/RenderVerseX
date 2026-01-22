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
    // Count enabled effects
    std::vector<IPostProcessPass*> enabledEffects;
    for (auto& effect : m_effects)
    {
        if (effect->IsEnabled())
        {
            enabledEffects.push_back(effect.get());
        }
    }

    if (enabledEffects.empty())
    {
        // No effects - just copy input to output
        // In a real implementation, we'd add a copy pass
        (void)graph;
        (void)sceneColor;
        (void)output;
        return;
    }

    // Execute effect chain
    // For now, simplified: just call each effect's AddToGraph
    // A proper implementation would manage ping-pong buffers
    RGTextureHandle currentInput = sceneColor;
    
    for (size_t i = 0; i < enabledEffects.size(); ++i)
    {
        bool isLast = (i == enabledEffects.size() - 1);
        RGTextureHandle currentOutput = isLast ? output : sceneColor;  // Would use intermediate buffer
        
        enabledEffects[i]->AddToGraph(graph, currentInput, currentOutput);
        currentInput = currentOutput;
    }
}

} // namespace RVX
