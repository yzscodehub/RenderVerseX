/**
 * @file ChromaticAberration.cpp
 * @brief Chromatic aberration implementation
 */

#include "Render/PostProcess/ChromaticAberration.h"
#include "Core/Log.h"

namespace RVX
{

ChromaticAberrationPass::ChromaticAberrationPass()
{
    m_enabled = false;
}

void ChromaticAberrationPass::Configure(const PostProcessSettings& settings)
{
    m_enabled = settings.enableChromaticAberration;
    m_config.intensity = settings.chromaticAberrationIntensity;
}

void ChromaticAberrationPass::AddToGraph(RenderGraph& graph, RGTextureHandle input, RGTextureHandle output)
{
    if (!m_enabled || m_config.intensity <= 0.0f)
        return;

    struct CAData
    {
        RGTextureHandle input;
        RGTextureHandle output;
        
        float intensity;
        float startOffset;
        Vec2 redOffset;
        Vec2 greenOffset;
        Vec2 blueOffset;
        bool radialFalloff;
        bool useSpectral;
    };

    graph.AddPass<CAData>(
        "ChromaticAberration",
        RenderGraphPassType::Compute,
        [this, input, output](RenderGraphBuilder& builder, CAData& data)
        {
            data.input = builder.Read(input);
            data.output = builder.Write(output, RHIResourceState::UnorderedAccess);
            
            data.intensity = m_config.intensity;
            data.startOffset = m_config.startOffset;
            data.redOffset = m_config.redOffset;
            data.greenOffset = m_config.greenOffset;
            data.blueOffset = m_config.blueOffset;
            data.radialFalloff = m_config.radialFalloff;
            data.useSpectral = m_config.useSpectral;
        },
        [](const CAData& data, RHICommandContext& ctx)
        {
            (void)data;
            (void)ctx;
            // TODO: Apply chromatic aberration shader
        });
}

} // namespace RVX
