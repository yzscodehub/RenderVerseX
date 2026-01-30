/**
 * @file DOF.cpp
 * @brief Depth of Field implementation
 */

#include "Render/PostProcess/DOF.h"
#include "Core/Log.h"
#include <cmath>

namespace RVX
{

DOFPass::DOFPass()
{
    m_enabled = true;
    m_currentFocusDistance = m_config.focusDistance;
}

void DOFPass::Configure(const PostProcessSettings& settings)
{
    m_enabled = settings.enableDOF;
    if (settings.dofFocusDistance > 0.0f)
        m_config.focusDistance = settings.dofFocusDistance;
    if (settings.dofAperture > 0.0f)
        m_config.aperture = settings.dofAperture;
    if (settings.dofFocalLength > 0.0f)
        m_config.focalLength = settings.dofFocalLength;
}

float DOFPass::CalculateCoC(float depth) const
{
    // Physically-based Circle of Confusion calculation
    // CoC = |S2 - S1| / S2 * (f^2 / (N * (S1 - f)))
    // Where:
    //   S1 = focus distance
    //   S2 = object distance (depth)
    //   f  = focal length
    //   N  = f-stop (aperture)
    
    float focusDistance = m_config.focusDistance;
    float focalLength = m_config.focalLength / 1000.0f;  // Convert mm to meters
    float aperture = m_config.aperture;
    float sensorSize = m_config.sensorSize / 1000.0f;    // Convert mm to meters
    
    if (depth <= 0.0f || focusDistance <= 0.0f)
        return 0.0f;
    
    // Calculate the CoC in meters at the sensor
    float numerator = focalLength * focalLength * (depth - focusDistance);
    float denominator = aperture * depth * (focusDistance - focalLength);
    
    if (std::abs(denominator) < 1e-6f)
        return 0.0f;
    
    float cocSensor = numerator / denominator;
    
    // Convert to pixels (assuming sensor covers the viewport)
    // This would need viewport size to be accurate
    float cocPixels = std::abs(cocSensor) / sensorSize * 1920.0f;  // Assume 1080p reference
    
    // Clamp to max blur radius
    cocPixels = std::min(cocPixels, m_config.maxBlurRadius);
    
    // Negative CoC for foreground (closer than focus)
    if (depth < focusDistance)
        cocPixels = -cocPixels;
    
    return cocPixels;
}

void DOFPass::SetAutoFocus(float screenX, float screenY, float depth)
{
    (void)screenX;
    (void)screenY;
    
    // Smooth transition to new focus distance
    if (depth > 0.0f)
    {
        m_config.focusDistance = depth;
    }
}

void DOFPass::AddToGraph(RenderGraph& graph, RGTextureHandle input, RGTextureHandle output)
{
    // Basic version without depth - just copy
    if (!m_enabled)
        return;

    struct DOFPassData
    {
        RGTextureHandle input;
        RGTextureHandle output;
    };

    graph.AddPass<DOFPassData>(
        "DOF_Fallback",
        RenderGraphPassType::Graphics,
        [input, output](RenderGraphBuilder& builder, DOFPassData& data)
        {
            data.input = builder.Read(input);
            data.output = builder.Write(output, RHIResourceState::RenderTarget);
        },
        [](const DOFPassData& data, RHICommandContext& ctx)
        {
            (void)data;
            (void)ctx;
            // Fallback: just copy without DOF (no depth available)
            RVX_CORE_WARN("DOF: No depth buffer provided, effect disabled");
        });
}

void DOFPass::AddToGraph(RenderGraph& graph, RGTextureHandle input, 
                         RGTextureHandle depth, RGTextureHandle output)
{
    if (!m_enabled)
        return;

    // Get sample count based on quality
    uint32 sampleCount = 8;
    switch (m_config.quality)
    {
        case DOFQuality::Low:    sampleCount = 4; break;
        case DOFQuality::Medium: sampleCount = 8; break;
        case DOFQuality::High:   sampleCount = 16; break;
        case DOFQuality::Ultra:  sampleCount = 32; break;
    }

    // =========================================================================
    // Pass 1: Calculate CoC and separate near/far
    // =========================================================================
    struct CoCPassData
    {
        RGTextureHandle inputColor;
        RGTextureHandle inputDepth;
        RGTextureHandle cocOutput;
        RGTextureHandle nearField;
        RGTextureHandle farField;
        
        float focusDistance;
        float focusRange;
        float aperture;
        float focalLength;
        float sensorSize;
        float maxBlurRadius;
        float nearBlurScale;
        float farBlurScale;
    };

    // Create intermediate textures
    RHITextureDesc cocDesc{};
    cocDesc.width = 0;   // Will be set from input
    cocDesc.height = 0;
    cocDesc.format = RHIFormat::R16_FLOAT;
    cocDesc.usage = RHITextureUsage::ShaderResource | RHITextureUsage::RenderTarget;
    RGTextureHandle cocTexture = graph.CreateTexture(cocDesc);

    graph.AddPass<CoCPassData>(
        "DOF_CalculateCoC",
        RenderGraphPassType::Compute,
        [this, input, depth, cocTexture](RenderGraphBuilder& builder, CoCPassData& data)
        {
            data.inputColor = builder.Read(input);
            data.inputDepth = builder.Read(depth);
            data.cocOutput = builder.Write(cocTexture, RHIResourceState::UnorderedAccess);
            
            data.focusDistance = m_config.focusDistance;
            data.focusRange = m_config.focusRange;
            data.aperture = m_config.aperture;
            data.focalLength = m_config.focalLength;
            data.sensorSize = m_config.sensorSize;
            data.maxBlurRadius = m_config.maxBlurRadius;
            data.nearBlurScale = m_config.nearBlurScale;
            data.farBlurScale = m_config.farBlurScale;
        },
        [](const CoCPassData& data, RHICommandContext& ctx)
        {
            (void)data;
            (void)ctx;
            // TODO: Dispatch compute shader to calculate CoC
            // - Sample depth
            // - Calculate CoC using physically-based formula
            // - Output signed CoC (negative = near, positive = far)
        });

    // =========================================================================
    // Pass 2: Downsample and blur far field
    // =========================================================================
    struct FarBlurPassData
    {
        RGTextureHandle inputColor;
        RGTextureHandle cocTexture;
        RGTextureHandle blurOutput;
        
        uint32 sampleCount;
        float maxBlurRadius;
        uint32 bokehShape;
        float anamorphicRatio;
    };

    RHITextureDesc blurDesc{};
    blurDesc.format = RHIFormat::RGBA16_FLOAT;
    blurDesc.usage = RHITextureUsage::ShaderResource | RHITextureUsage::RenderTarget;
    RGTextureHandle farBlurTexture = graph.CreateTexture(blurDesc);

    graph.AddPass<FarBlurPassData>(
        "DOF_FarFieldBlur",
        RenderGraphPassType::Compute,
        [this, input, cocTexture, farBlurTexture, sampleCount](RenderGraphBuilder& builder, FarBlurPassData& data)
        {
            data.inputColor = builder.Read(input);
            data.cocTexture = builder.Read(cocTexture);
            data.blurOutput = builder.Write(farBlurTexture, RHIResourceState::UnorderedAccess);
            
            data.sampleCount = sampleCount;
            data.maxBlurRadius = m_config.maxBlurRadius;
            data.bokehShape = static_cast<uint32>(m_config.bokehShape);
            data.anamorphicRatio = m_config.anamorphicRatio;
        },
        [](const FarBlurPassData& data, RHICommandContext& ctx)
        {
            (void)data;
            (void)ctx;
            // TODO: Dispatch blur shader for far field
            // - Use CoC as blur kernel size
            // - Apply bokeh shape weighting
            // - Handle anamorphic stretching
        });

    // =========================================================================
    // Pass 3: Blur near field (with dilation)
    // =========================================================================
    struct NearBlurPassData
    {
        RGTextureHandle inputColor;
        RGTextureHandle cocTexture;
        RGTextureHandle blurOutput;
        
        uint32 sampleCount;
        float maxBlurRadius;
    };

    RGTextureHandle nearBlurTexture = graph.CreateTexture(blurDesc);

    graph.AddPass<NearBlurPassData>(
        "DOF_NearFieldBlur",
        RenderGraphPassType::Compute,
        [this, input, cocTexture, nearBlurTexture, sampleCount](RenderGraphBuilder& builder, NearBlurPassData& data)
        {
            data.inputColor = builder.Read(input);
            data.cocTexture = builder.Read(cocTexture);
            data.blurOutput = builder.Write(nearBlurTexture, RHIResourceState::UnorderedAccess);
            
            data.sampleCount = sampleCount;
            data.maxBlurRadius = m_config.maxBlurRadius;
        },
        [](const NearBlurPassData& data, RHICommandContext& ctx)
        {
            (void)data;
            (void)ctx;
            // TODO: Dispatch blur shader for near field
            // - Dilate CoC before blur (near field bleeds over sharp areas)
            // - Apply larger kernel for foreground objects
        });

    // =========================================================================
    // Pass 4: Composite final result
    // =========================================================================
    struct CompositePassData
    {
        RGTextureHandle inputColor;
        RGTextureHandle cocTexture;
        RGTextureHandle farBlur;
        RGTextureHandle nearBlur;
        RGTextureHandle output;
        
        float focusRange;
    };

    graph.AddPass<CompositePassData>(
        "DOF_Composite",
        RenderGraphPassType::Graphics,
        [this, input, cocTexture, farBlurTexture, nearBlurTexture, output]
        (RenderGraphBuilder& builder, CompositePassData& data)
        {
            data.inputColor = builder.Read(input);
            data.cocTexture = builder.Read(cocTexture);
            data.farBlur = builder.Read(farBlurTexture);
            data.nearBlur = builder.Read(nearBlurTexture);
            data.output = builder.Write(output, RHIResourceState::RenderTarget);
            
            data.focusRange = m_config.focusRange;
        },
        [](const CompositePassData& data, RHICommandContext& ctx)
        {
            (void)data;
            (void)ctx;
            // TODO: Composite shader
            // - Blend between sharp, far blur, and near blur based on CoC
            // - Apply smooth transitions at focus boundaries
            // - Handle alpha properly for transparency
        });
}

} // namespace RVX
