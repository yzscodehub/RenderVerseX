/**
 * @file MotionBlur.cpp
 * @brief Motion blur implementation
 */

#include "Render/PostProcess/MotionBlur.h"
#include "Core/Log.h"

namespace RVX
{

MotionBlurPass::MotionBlurPass()
{
    m_enabled = true;
}

void MotionBlurPass::Configure(const PostProcessSettings& settings)
{
    m_enabled = settings.enableMotionBlur;
    if (settings.motionBlurIntensity > 0.0f)
        m_config.intensity = settings.motionBlurIntensity;
    if (settings.motionBlurMaxVelocity > 0.0f)
        m_config.maxVelocity = settings.motionBlurMaxVelocity;
}

void MotionBlurPass::SetCameraMatrices(const Mat4& currentViewProj, const Mat4& prevViewProj)
{
    m_currentViewProj = currentViewProj;
    m_prevViewProj = prevViewProj;
    m_hasCameraData = true;
}

void MotionBlurPass::AddToGraph(RenderGraph& graph, RGTextureHandle input, RGTextureHandle output)
{
    // Basic version without velocity buffer - use camera motion
    if (!m_enabled)
        return;

    struct CameraMotionBlurData
    {
        RGTextureHandle input;
        RGTextureHandle output;
        Mat4 currentViewProj;
        Mat4 prevViewProj;
        float intensity;
        float maxVelocity;
        uint32 sampleCount;
    };

    // Get sample count based on quality
    uint32 sampleCount = 8;
    switch (m_config.quality)
    {
        case MotionBlurQuality::Low:    sampleCount = 4; break;
        case MotionBlurQuality::Medium: sampleCount = 8; break;
        case MotionBlurQuality::High:   sampleCount = 16; break;
        case MotionBlurQuality::Ultra:  sampleCount = 32; break;
    }

    graph.AddPass<CameraMotionBlurData>(
        "MotionBlur_Camera",
        RenderGraphPassType::Graphics,
        [this, input, output, sampleCount](RenderGraphBuilder& builder, CameraMotionBlurData& data)
        {
            data.input = builder.Read(input);
            data.output = builder.Write(output, RHIResourceState::RenderTarget);
            data.currentViewProj = m_currentViewProj;
            data.prevViewProj = m_prevViewProj;
            data.intensity = m_config.intensity;
            data.maxVelocity = m_config.maxVelocity;
            data.sampleCount = sampleCount;
        },
        [](const CameraMotionBlurData& data, RHICommandContext& ctx)
        {
            (void)data;
            (void)ctx;
            // TODO: Camera-only motion blur
            // - Reconstruct velocity from depth and camera matrices
            // - Apply blur along velocity direction
        });
}

void MotionBlurPass::AddToGraph(RenderGraph& graph, RGTextureHandle input,
                                 RGTextureHandle velocity, RGTextureHandle depth,
                                 RGTextureHandle output)
{
    if (!m_enabled)
        return;

    // Get sample count based on quality
    uint32 sampleCount = 8;
    switch (m_config.quality)
    {
        case MotionBlurQuality::Low:    sampleCount = 4; break;
        case MotionBlurQuality::Medium: sampleCount = 8; break;
        case MotionBlurQuality::High:   sampleCount = 16; break;
        case MotionBlurQuality::Ultra:  sampleCount = 32; break;
    }

    // =========================================================================
    // Pass 1: Tile max velocity (for optimization)
    // =========================================================================
    if (m_config.useTileMax)
    {
        struct TileMaxData
        {
            RGTextureHandle velocity;
            RGTextureHandle tileMaxOutput;
            uint32 tileSize;
        };

        RHITextureDesc tileDesc{};
        tileDesc.format = RHIFormat::RG16_FLOAT;
        tileDesc.usage = RHITextureUsage::ShaderResource | RHITextureUsage::UnorderedAccess;
        RGTextureHandle tileMaxTexture = graph.CreateTexture(tileDesc);

        graph.AddPass<TileMaxData>(
            "MotionBlur_TileMax",
            RenderGraphPassType::Compute,
            [this, velocity, tileMaxTexture](RenderGraphBuilder& builder, TileMaxData& data)
            {
                data.velocity = builder.Read(velocity);
                data.tileMaxOutput = builder.Write(tileMaxTexture, RHIResourceState::UnorderedAccess);
                data.tileSize = m_config.tileSize;
            },
            [](const TileMaxData& data, RHICommandContext& ctx)
            {
                (void)data;
                (void)ctx;
                // TODO: Compute max velocity per tile
                // - Reduce velocity to tile resolution
                // - Store max velocity magnitude per tile
            });

        // =========================================================================
        // Pass 2: Neighbor max (expand tiles for better coverage)
        // =========================================================================
        struct NeighborMaxData
        {
            RGTextureHandle tileMax;
            RGTextureHandle neighborMaxOutput;
        };

        RGTextureHandle neighborMaxTexture = graph.CreateTexture(tileDesc);

        graph.AddPass<NeighborMaxData>(
            "MotionBlur_NeighborMax",
            RenderGraphPassType::Compute,
            [tileMaxTexture, neighborMaxTexture](RenderGraphBuilder& builder, NeighborMaxData& data)
            {
                data.tileMax = builder.Read(tileMaxTexture);
                data.neighborMaxOutput = builder.Write(neighborMaxTexture, RHIResourceState::UnorderedAccess);
            },
            [](const NeighborMaxData& data, RHICommandContext& ctx)
            {
                (void)data;
                (void)ctx;
                // TODO: 3x3 max of neighboring tiles
                // - This ensures proper blur across tile boundaries
            });
    }

    // =========================================================================
    // Pass 3: Motion blur gather
    // =========================================================================
    struct MotionBlurGatherData
    {
        RGTextureHandle inputColor;
        RGTextureHandle velocity;
        RGTextureHandle depth;
        RGTextureHandle output;
        
        float intensity;
        float maxVelocity;
        float minVelocity;
        float shutterAngle;
        float softZDistance;
        float jitterStrength;
        uint32 sampleCount;
        bool depthAware;
    };

    graph.AddPass<MotionBlurGatherData>(
        "MotionBlur_Gather",
        RenderGraphPassType::Compute,
        [this, input, velocity, depth, output, sampleCount](RenderGraphBuilder& builder, MotionBlurGatherData& data)
        {
            data.inputColor = builder.Read(input);
            data.velocity = builder.Read(velocity);
            data.depth = builder.Read(depth);
            data.output = builder.Write(output, RHIResourceState::UnorderedAccess);
            
            data.intensity = m_config.intensity;
            data.maxVelocity = m_config.maxVelocity;
            data.minVelocity = m_config.minVelocity;
            data.shutterAngle = m_config.shutterAngle / 360.0f;  // Normalize
            data.softZDistance = m_config.softZDistance;
            data.jitterStrength = m_config.jitterStrength;
            data.sampleCount = sampleCount;
            data.depthAware = m_config.depthAwareBlur;
        },
        [](const MotionBlurGatherData& data, RHICommandContext& ctx)
        {
            (void)data;
            (void)ctx;
            // TODO: Main motion blur pass
            // - Sample along velocity direction
            // - Use depth comparison for soft edges
            // - Apply jittering to reduce banding
            // - Weight samples by depth similarity
        });
}

} // namespace RVX
