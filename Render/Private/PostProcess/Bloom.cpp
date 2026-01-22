/**
 * @file Bloom.cpp
 * @brief BloomPass implementation
 */

#include "Render/PostProcess/Bloom.h"
#include "Core/Log.h"

namespace RVX
{

BloomPass::BloomPass()
{
    m_enabled = true;
}

void BloomPass::Configure(const PostProcessSettings& settings)
{
    m_enabled = settings.enableBloom;
    m_threshold = settings.bloomThreshold;
    m_intensity = settings.bloomIntensity;
    m_radius = settings.bloomRadius;
}

void BloomPass::AddToGraph(RenderGraph& graph, RGTextureHandle input, RGTextureHandle output)
{
    if (!m_enabled)
        return;

    // Bloom requires multiple passes:
    // 1. Bright pass (threshold)
    // 2. Downsample chain (multiple mip levels)
    // 3. Upsample chain (blur and combine)
    // 4. Final blend with original

    struct BloomData
    {
        RGTextureHandle input;
        RGTextureHandle output;
        float threshold;
        float intensity;
        float radius;
        float softKnee;
        uint32 mipCount;
    };

    // For now, simplified single-pass bloom (proper implementation would use mip chain)
    graph.AddPass<BloomData>(
        "Bloom",
        RenderGraphPassType::Graphics,
        [this, input, output](RenderGraphBuilder& builder, BloomData& data)
        {
            data.input = builder.Read(input);
            data.output = builder.Write(output, RHIResourceState::RenderTarget);
            data.threshold = m_threshold;
            data.intensity = m_intensity;
            data.radius = m_radius;
            data.softKnee = m_softKnee;
            data.mipCount = m_mipCount;
        },
        [](const BloomData& data, RHICommandContext& ctx)
        {
            (void)data;
            (void)ctx;
            // TODO: Full bloom implementation would:
            // 1. Bright pass: threshold high-luminance pixels
            // 2. Progressively downsample with blur (gaussian or dual kawase)
            // 3. Progressively upsample and accumulate
            // 4. Add bloom result to scene color
        });
}

} // namespace RVX
