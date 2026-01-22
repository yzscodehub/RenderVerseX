/**
 * @file ToneMapping.cpp
 * @brief ToneMappingPass implementation
 */

#include "Render/PostProcess/ToneMapping.h"
#include "Core/Log.h"

namespace RVX
{

ToneMappingPass::ToneMappingPass()
{
    m_enabled = true;
}

void ToneMappingPass::Configure(const PostProcessSettings& settings)
{
    m_enabled = settings.enableToneMapping;
    m_exposure = settings.exposure;
    m_gamma = settings.gamma;
}

void ToneMappingPass::AddToGraph(RenderGraph& graph, RGTextureHandle input, RGTextureHandle output)
{
    if (!m_enabled)
        return;

    struct ToneMappingData
    {
        RGTextureHandle input;
        RGTextureHandle output;
        ToneMappingOperator op;
        float exposure;
        float gamma;
        float whitePoint;
    };

    graph.AddPass<ToneMappingData>(
        "ToneMapping",
        RenderGraphPassType::Graphics,
        [this, input, output](RenderGraphBuilder& builder, ToneMappingData& data)
        {
            data.input = builder.Read(input);
            data.output = builder.Write(output, RHIResourceState::RenderTarget);
            data.op = m_operator;
            data.exposure = m_exposure;
            data.gamma = m_gamma;
            data.whitePoint = m_whitePoint;
        },
        [](const ToneMappingData& data, RHICommandContext& ctx)
        {
            (void)data;
            (void)ctx;
            // TODO: Bind fullscreen pipeline and render
            // 1. Bind ToneMapping shader
            // 2. Set constants (exposure, gamma, white point, operator)
            // 3. Bind input texture as SRV
            // 4. Draw fullscreen triangle
        });
}

} // namespace RVX
