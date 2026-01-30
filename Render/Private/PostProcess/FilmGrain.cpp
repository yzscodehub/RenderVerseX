/**
 * @file FilmGrain.cpp
 * @brief Film grain implementation
 */

#include "Render/PostProcess/FilmGrain.h"
#include "Core/Log.h"

namespace RVX
{

FilmGrainPass::FilmGrainPass()
{
    m_enabled = false;
}

void FilmGrainPass::Configure(const PostProcessSettings& settings)
{
    m_enabled = settings.enableFilmGrain;
    m_config.intensity = settings.filmGrainIntensity;
}

void FilmGrainPass::AddToGraph(RenderGraph& graph, RGTextureHandle input, RGTextureHandle output)
{
    if (!m_enabled || m_config.intensity <= 0.0f)
        return;

    struct FilmGrainData
    {
        RGTextureHandle input;
        RGTextureHandle output;
        
        float intensity;
        float response;
        float size;
        float luminanceContribution;
        float colorContribution;
        float time;
        uint32 type;
    };

    graph.AddPass<FilmGrainData>(
        "FilmGrain",
        RenderGraphPassType::Compute,
        [this, input, output](RenderGraphBuilder& builder, FilmGrainData& data)
        {
            data.input = builder.Read(input);
            data.output = builder.Write(output, RHIResourceState::UnorderedAccess);
            
            data.intensity = m_config.intensity;
            data.response = m_config.response;
            data.size = m_config.size;
            data.luminanceContribution = m_config.luminanceContribution;
            data.colorContribution = m_config.colorContribution;
            data.time = m_config.animated ? m_frameTime * m_config.animationSpeed : 0.0f;
            data.type = static_cast<uint32>(m_config.type);
        },
        [](const FilmGrainData& data, RHICommandContext& ctx)
        {
            (void)data;
            (void)ctx;
            // TODO: Apply film grain shader
        });
}

} // namespace RVX
