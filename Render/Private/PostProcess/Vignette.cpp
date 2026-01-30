/**
 * @file Vignette.cpp
 * @brief Vignette implementation
 */

#include "Render/PostProcess/Vignette.h"
#include "Core/Log.h"

namespace RVX
{

VignettePass::VignettePass()
{
    m_enabled = false;  // Disabled by default
}

void VignettePass::Configure(const PostProcessSettings& settings)
{
    m_enabled = settings.enableVignette;
    m_config.intensity = settings.vignetteIntensity;
    m_config.smoothness = 1.0f - settings.vignetteRadius;  // Convert radius to smoothness
}

void VignettePass::AddToGraph(RenderGraph& graph, RGTextureHandle input, RGTextureHandle output)
{
    if (!m_enabled || m_config.intensity <= 0.0f)
        return;

    struct VignetteData
    {
        RGTextureHandle input;
        RGTextureHandle output;
        
        float intensity;
        float smoothness;
        float roundness;
        Vec2 center;
        Vec3 color;
        uint32 mode;
        float aspectRatio;
    };

    graph.AddPass<VignetteData>(
        "Vignette",
        RenderGraphPassType::Compute,
        [this, input, output](RenderGraphBuilder& builder, VignetteData& data)
        {
            data.input = builder.Read(input);
            data.output = builder.Write(output, RHIResourceState::UnorderedAccess);
            
            data.intensity = m_config.intensity;
            data.smoothness = m_config.smoothness;
            data.roundness = m_config.roundness;
            data.center = m_config.center;
            data.color = m_config.color;
            data.mode = static_cast<uint32>(m_config.mode);
            data.aspectRatio = 16.0f / 9.0f;  // TODO: Get from render target
        },
        [](const VignetteData& data, RHICommandContext& ctx)
        {
            (void)data;
            (void)ctx;
            // TODO: Apply vignette shader
        });
}

} // namespace RVX
