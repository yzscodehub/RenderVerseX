/**
 * @file DOF.hlsl
 * @brief Depth of Field shader
 * 
 * Multi-pass DOF implementation:
 * - CoC calculation pass
 * - Far field blur pass
 * - Near field blur pass (with dilation)
 * - Composite pass
 */

// =============================================================================
// Common structures and constants
// =============================================================================

cbuffer DOFConstants : register(b0)
{
    float4 DOFParams;           // x: focusDistance, y: focusRange, z: aperture, w: focalLength
    float4 DOFParams2;          // x: sensorSize, y: maxBlurRadius, z: nearBlurScale, w: farBlurScale
    float4 DOFParams3;          // x: anamorphicRatio, y: bokehShape, z: sampleCount, w: chromaticAberration
    float4 ScreenSize;          // x: width, y: height, z: 1/width, w: 1/height
    float4x4 InvProjection;
}

#define FocusDistance   DOFParams.x
#define FocusRange      DOFParams.y
#define Aperture        DOFParams.z
#define FocalLength     DOFParams.w
#define SensorSize      DOFParams2.x
#define MaxBlurRadius   DOFParams2.y
#define NearBlurScale   DOFParams2.z
#define FarBlurScale    DOFParams2.w
#define AnamorphicRatio DOFParams3.x
#define BokehShape      DOFParams3.y
#define SampleCount     DOFParams3.z
#define ChromaticAberr  DOFParams3.w

Texture2D<float4> InputColor : register(t0);
Texture2D<float> DepthTexture : register(t1);
Texture2D<float> CoCTexture : register(t2);
Texture2D<float4> FarBlurTexture : register(t3);
Texture2D<float4> NearBlurTexture : register(t4);

RWTexture2D<float> CoCOutput : register(u0);
RWTexture2D<float4> BlurOutput : register(u1);

SamplerState LinearClampSampler : register(s0);
SamplerState PointClampSampler : register(s1);

// =============================================================================
// Helper functions
// =============================================================================

// Convert depth buffer value to linear depth
float LinearizeDepth(float depth)
{
    // Assuming reverse-Z infinite far plane
    float near = 0.1;
    return near / depth;
}

// Calculate Circle of Confusion
// Returns signed CoC: negative = foreground, positive = background
float CalculateCoC(float linearDepth)
{
    // Physically-based CoC calculation
    float focalLengthMeters = FocalLength * 0.001;
    
    // CoC = |S2 - S1| / S2 * (f^2 / (N * (S1 - f)))
    float numerator = focalLengthMeters * focalLengthMeters * (linearDepth - FocusDistance);
    float denominator = Aperture * linearDepth * (FocusDistance - focalLengthMeters);
    
    if (abs(denominator) < 1e-6)
        return 0.0;
    
    float cocSensor = numerator / denominator;
    
    // Convert to pixels
    float sensorMeters = SensorSize * 0.001;
    float cocPixels = abs(cocSensor) / sensorMeters * ScreenSize.y;
    
    // Clamp to max blur
    cocPixels = min(cocPixels, MaxBlurRadius);
    
    // Apply near/far scales
    if (linearDepth < FocusDistance)
        cocPixels *= NearBlurScale;
    else
        cocPixels *= FarBlurScale;
    
    // Sign indicates near/far
    return (linearDepth < FocusDistance) ? -cocPixels : cocPixels;
}

// Bokeh shape weight (circle, hexagon, octagon)
float BokehWeight(float2 offset, float bokehType)
{
    float r = length(offset);
    
    if (bokehType < 0.5)
    {
        // Circle
        return saturate(1.0 - r);
    }
    else if (bokehType < 1.5)
    {
        // Hexagon
        float2 absOffset = abs(offset);
        float hex = max(absOffset.x * 0.866 + absOffset.y * 0.5, absOffset.y);
        return saturate(1.0 - hex);
    }
    else
    {
        // Octagon
        float2 absOffset = abs(offset);
        float oct = max(max(absOffset.x, absOffset.y), 
                       (absOffset.x + absOffset.y) * 0.707);
        return saturate(1.0 - oct);
    }
}

// Golden angle spiral for sample distribution
float2 GetSampleOffset(uint sampleIndex, uint totalSamples)
{
    float goldenAngle = 2.399963; // radians
    float r = sqrt((float)sampleIndex / (float)totalSamples);
    float theta = sampleIndex * goldenAngle;
    return float2(r * cos(theta), r * sin(theta));
}

// =============================================================================
// Pass 1: Calculate CoC
// =============================================================================

[numthreads(8, 8, 1)]
void CS_CalculateCoC(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= (uint)ScreenSize.x || 
        dispatchThreadId.y >= (uint)ScreenSize.y)
        return;
    
    float depth = DepthTexture[dispatchThreadId.xy];
    float linearDepth = LinearizeDepth(depth);
    float coc = CalculateCoC(linearDepth);
    
    CoCOutput[dispatchThreadId.xy] = coc;
}

// =============================================================================
// Pass 2: Far field blur
// =============================================================================

[numthreads(8, 8, 1)]
void CS_FarFieldBlur(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= (uint)ScreenSize.x || 
        dispatchThreadId.y >= (uint)ScreenSize.y)
        return;
    
    float2 uv = (dispatchThreadId.xy + 0.5) * ScreenSize.zw;
    float centerCoC = CoCTexture.SampleLevel(PointClampSampler, uv, 0);
    
    // Only blur background (positive CoC)
    if (centerCoC <= 0)
    {
        BlurOutput[dispatchThreadId.xy] = InputColor.SampleLevel(LinearClampSampler, uv, 0);
        return;
    }
    
    float blurRadius = centerCoC;
    uint numSamples = (uint)SampleCount;
    
    float4 colorSum = 0;
    float weightSum = 0;
    
    for (uint i = 0; i < numSamples; i++)
    {
        float2 offset = GetSampleOffset(i, numSamples);
        
        // Apply anamorphic stretching
        offset.x *= AnamorphicRatio;
        
        // Scale by blur radius
        float2 sampleUV = uv + offset * blurRadius * ScreenSize.zw;
        
        // Sample CoC at this position
        float sampleCoC = CoCTexture.SampleLevel(PointClampSampler, sampleUV, 0);
        
        // Only accumulate if sample is also in background
        if (sampleCoC > 0)
        {
            float weight = BokehWeight(offset, BokehShape);
            float4 color = InputColor.SampleLevel(LinearClampSampler, sampleUV, 0);
            
            colorSum += color * weight;
            weightSum += weight;
        }
    }
    
    if (weightSum > 0)
        BlurOutput[dispatchThreadId.xy] = colorSum / weightSum;
    else
        BlurOutput[dispatchThreadId.xy] = InputColor.SampleLevel(LinearClampSampler, uv, 0);
}

// =============================================================================
// Pass 3: Near field blur (with CoC dilation)
// =============================================================================

[numthreads(8, 8, 1)]
void CS_NearFieldBlur(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= (uint)ScreenSize.x || 
        dispatchThreadId.y >= (uint)ScreenSize.y)
        return;
    
    float2 uv = (dispatchThreadId.xy + 0.5) * ScreenSize.zw;
    float centerCoC = CoCTexture.SampleLevel(PointClampSampler, uv, 0);
    
    // Find maximum near CoC in neighborhood (dilation)
    float maxNearCoC = centerCoC;
    float dilationRadius = MaxBlurRadius;
    
    for (int dy = -2; dy <= 2; dy++)
    {
        for (int dx = -2; dx <= 2; dx++)
        {
            float2 sampleUV = uv + float2(dx, dy) * dilationRadius * 0.25 * ScreenSize.zw;
            float sampleCoC = CoCTexture.SampleLevel(PointClampSampler, sampleUV, 0);
            if (sampleCoC < 0)
                maxNearCoC = min(maxNearCoC, sampleCoC);
        }
    }
    
    // Only blur foreground (negative CoC)
    if (maxNearCoC >= 0)
    {
        BlurOutput[dispatchThreadId.xy] = float4(0, 0, 0, 0);
        return;
    }
    
    float blurRadius = abs(maxNearCoC);
    uint numSamples = (uint)SampleCount;
    
    float4 colorSum = 0;
    float weightSum = 0;
    
    for (uint i = 0; i < numSamples; i++)
    {
        float2 offset = GetSampleOffset(i, numSamples);
        offset.x *= AnamorphicRatio;
        
        float2 sampleUV = uv + offset * blurRadius * ScreenSize.zw;
        
        float weight = BokehWeight(offset, BokehShape);
        float4 color = InputColor.SampleLevel(LinearClampSampler, sampleUV, 0);
        
        colorSum += color * weight;
        weightSum += weight;
    }
    
    float alpha = saturate(abs(maxNearCoC) / MaxBlurRadius);
    
    if (weightSum > 0)
        BlurOutput[dispatchThreadId.xy] = float4((colorSum / weightSum).rgb, alpha);
    else
        BlurOutput[dispatchThreadId.xy] = float4(0, 0, 0, 0);
}

// =============================================================================
// Pass 4: Composite
// =============================================================================

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput VS_Fullscreen(uint vertexId : SV_VertexID)
{
    VSOutput output;
    
    // Fullscreen triangle
    output.uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(output.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    
    return output;
}

float4 PS_Composite(VSOutput input) : SV_Target
{
    float2 uv = input.uv;
    
    // Sample all layers
    float4 sharpColor = InputColor.Sample(LinearClampSampler, uv);
    float coc = CoCTexture.Sample(PointClampSampler, uv);
    float4 farBlur = FarBlurTexture.Sample(LinearClampSampler, uv);
    float4 nearBlur = NearBlurTexture.Sample(LinearClampSampler, uv);
    
    // Blend far field
    float farBlend = saturate(coc / MaxBlurRadius);
    float4 result = lerp(sharpColor, farBlur, farBlend);
    
    // Blend near field (using pre-multiplied alpha)
    result = lerp(result, nearBlur, nearBlur.a);
    
    return result;
}
