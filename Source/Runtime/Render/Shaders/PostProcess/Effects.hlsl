/**
 * @file Effects.hlsl
 * @brief Simple post-process effects (Vignette, ChromaticAberration, FilmGrain)
 * 
 * These are simple effects that can share a shader file.
 */

// =============================================================================
// Common resources
// =============================================================================

Texture2D<float4> InputColor : register(t0);
RWTexture2D<float4> OutputColor : register(u0);
SamplerState LinearClampSampler : register(s0);

// =============================================================================
// VIGNETTE
// =============================================================================

cbuffer VignetteConstants : register(b0)
{
    float4 VignetteParams;      // x: intensity, y: smoothness, z: roundness, w: mode
    float4 VignetteCenter;      // xy: center, zw: aspectRatio
    float4 VignetteColor;       // rgb: color, a: unused
    float4 ScreenSize;          // x: width, y: height, z: 1/width, w: 1/height
}

float CalculateVignette(float2 uv, float intensity, float smoothness, float roundness, float2 center, float aspectRatio)
{
    float2 coord = (uv - center) * 2.0;
    
    // Adjust for aspect ratio
    coord.x *= aspectRatio;
    
    // Calculate distance based on roundness
    float dist;
    if (roundness >= 1.0)
    {
        dist = length(coord);
    }
    else
    {
        // Blend between square and circle
        float2 absCoord = abs(coord);
        float maxDist = max(absCoord.x, absCoord.y);
        float circDist = length(coord);
        dist = lerp(maxDist, circDist, roundness);
    }
    
    // Calculate vignette with smooth falloff
    float vignette = 1.0 - smoothstep(1.0 - smoothness, 1.0, dist * intensity);
    
    return vignette;
}

// Natural vignette (cos^4 falloff like real lenses)
float CalculateNaturalVignette(float2 uv, float intensity)
{
    float2 coord = uv - 0.5;
    float dist = length(coord) * 2.0;
    float cosAngle = 1.0 / sqrt(1.0 + dist * dist);
    return pow(cosAngle, 4.0 * intensity);
}

[numthreads(8, 8, 1)]
void CS_Vignette(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= (uint)ScreenSize.x || 
        dispatchThreadId.y >= (uint)ScreenSize.y)
        return;
    
    float2 uv = (dispatchThreadId.xy + 0.5) * ScreenSize.zw;
    float4 color = InputColor[dispatchThreadId.xy];
    
    float vignette;
    uint mode = (uint)VignetteParams.w;
    
    if (mode == 2) // Natural
    {
        vignette = CalculateNaturalVignette(uv, VignetteParams.x);
    }
    else // Classic or Rounded
    {
        vignette = CalculateVignette(uv, VignetteParams.x, VignetteParams.y, 
                                      VignetteParams.z, VignetteCenter.xy, 
                                      VignetteCenter.z);
    }
    
    // Blend with vignette color
    color.rgb = lerp(VignetteColor.rgb, color.rgb, vignette);
    
    OutputColor[dispatchThreadId.xy] = color;
}

// =============================================================================
// CHROMATIC ABERRATION
// =============================================================================

cbuffer ChromaticAberrationConstants : register(b0)
{
    float4 CAParams;            // x: intensity, y: startOffset, z: radialFalloff, w: useSpectral
    float4 CARedOffset;         // xy: offset, zw: unused
    float4 CAGreenOffset;       // xy: offset, zw: unused
    float4 CABlueOffset;        // xy: offset, zw: unused
    float4 CAScreenSize;        // x: width, y: height, z: 1/width, w: 1/height
}

[numthreads(8, 8, 1)]
void CS_ChromaticAberration(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= (uint)CAScreenSize.x || 
        dispatchThreadId.y >= (uint)CAScreenSize.y)
        return;
    
    float2 uv = (dispatchThreadId.xy + 0.5) * CAScreenSize.zw;
    float2 center = float2(0.5, 0.5);
    
    // Calculate radial distance from center
    float2 toCenter = uv - center;
    float dist = length(toCenter);
    
    // Calculate intensity falloff
    float falloff = 1.0;
    if (CAParams.z > 0.5) // radialFalloff
    {
        falloff = smoothstep(CAParams.y, 1.0, dist * 2.0);
    }
    
    float intensity = CAParams.x * falloff;
    
    // Calculate channel offsets
    float2 direction = normalize(toCenter);
    float2 redUV = uv + direction * CARedOffset.xy * intensity * CAScreenSize.zw * 100.0;
    float2 greenUV = uv + direction * CAGreenOffset.xy * intensity * CAScreenSize.zw * 100.0;
    float2 blueUV = uv + direction * CABlueOffset.xy * intensity * CAScreenSize.zw * 100.0;
    
    // Sample each channel
    float r = InputColor.SampleLevel(LinearClampSampler, redUV, 0).r;
    float g = InputColor.SampleLevel(LinearClampSampler, greenUV, 0).g;
    float b = InputColor.SampleLevel(LinearClampSampler, blueUV, 0).b;
    float a = InputColor.SampleLevel(LinearClampSampler, uv, 0).a;
    
    OutputColor[dispatchThreadId.xy] = float4(r, g, b, a);
}

// Spectral chromatic aberration (7-tap sampling across spectrum)
[numthreads(8, 8, 1)]
void CS_ChromaticAberrationSpectral(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= (uint)CAScreenSize.x || 
        dispatchThreadId.y >= (uint)CAScreenSize.y)
        return;
    
    float2 uv = (dispatchThreadId.xy + 0.5) * CAScreenSize.zw;
    float2 center = float2(0.5, 0.5);
    float2 toCenter = uv - center;
    float dist = length(toCenter);
    
    float falloff = CAParams.z > 0.5 ? smoothstep(CAParams.y, 1.0, dist * 2.0) : 1.0;
    float intensity = CAParams.x * falloff;
    float2 direction = normalize(toCenter);
    
    // Spectral weights (approximate visible spectrum)
    const float3 spectralWeights[7] = {
        float3(1.0, 0.0, 0.0),    // Red
        float3(1.0, 0.5, 0.0),    // Orange
        float3(1.0, 1.0, 0.0),    // Yellow
        float3(0.0, 1.0, 0.0),    // Green
        float3(0.0, 1.0, 1.0),    // Cyan
        float3(0.0, 0.0, 1.0),    // Blue
        float3(0.5, 0.0, 1.0)     // Violet
    };
    
    float3 colorSum = 0;
    float3 weightSum = 0;
    
    for (int i = 0; i < 7; i++)
    {
        float t = (float(i) / 6.0) * 2.0 - 1.0; // -1 to 1
        float2 offset = direction * t * intensity * CAScreenSize.zw * 100.0;
        float2 sampleUV = uv + offset;
        
        float3 sampleColor = InputColor.SampleLevel(LinearClampSampler, sampleUV, 0).rgb;
        colorSum += sampleColor * spectralWeights[i];
        weightSum += spectralWeights[i];
    }
    
    float3 result = colorSum / weightSum;
    float a = InputColor.SampleLevel(LinearClampSampler, uv, 0).a;
    
    OutputColor[dispatchThreadId.xy] = float4(result, a);
}

// =============================================================================
// FILM GRAIN
// =============================================================================

cbuffer FilmGrainConstants : register(b0)
{
    float4 GrainParams;         // x: intensity, y: response, z: size, w: time
    float4 GrainParams2;        // x: luminanceContrib, y: colorContrib, z: type, w: unused
    float4 GrainScreenSize;     // x: width, y: height, z: 1/width, w: 1/height
}

// Hash function for noise
float Hash(float2 p, float t)
{
    float3 p3 = frac(float3(p.xyx + t) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

// Simple noise
float SimpleNoise(float2 uv, float time, float size)
{
    float2 p = uv * GrainScreenSize.xy / size;
    return Hash(floor(p), time);
}

// Film-like grain (gaussian-ish distribution)
float FilmNoise(float2 uv, float time, float size)
{
    float2 p = uv * GrainScreenSize.xy / size;
    
    // Multiple octaves for more natural look
    float n = 0;
    n += Hash(floor(p * 1.0), time) * 0.5;
    n += Hash(floor(p * 2.0), time * 1.3) * 0.25;
    n += Hash(floor(p * 4.0), time * 1.7) * 0.125;
    n += Hash(floor(p * 8.0), time * 2.1) * 0.0625;
    
    // Transform to roughly gaussian
    n = n * 2.0 - 1.0;
    n = sign(n) * pow(abs(n), 0.5);
    
    return n * 0.5 + 0.5;
}

// Colored grain
float3 ColoredNoise(float2 uv, float time, float size)
{
    float2 p = uv * GrainScreenSize.xy / size;
    
    float r = Hash(floor(p), time);
    float g = Hash(floor(p), time + 100.0);
    float b = Hash(floor(p), time + 200.0);
    
    return float3(r, g, b);
}

float Luminance(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

[numthreads(8, 8, 1)]
void CS_FilmGrain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= (uint)GrainScreenSize.x || 
        dispatchThreadId.y >= (uint)GrainScreenSize.y)
        return;
    
    float2 uv = (dispatchThreadId.xy + 0.5) * GrainScreenSize.zw;
    float4 color = InputColor[dispatchThreadId.xy];
    
    float intensity = GrainParams.x;
    float response = GrainParams.y;
    float size = GrainParams.z;
    float time = GrainParams.w;
    float luminanceContrib = GrainParams2.x;
    float colorContrib = GrainParams2.y;
    uint grainType = (uint)GrainParams2.z;
    
    // Calculate grain intensity based on luminance
    float luma = Luminance(color.rgb);
    float responseMultiplier = lerp(1.0, 1.0 - luma, response);
    float grainIntensity = intensity * responseMultiplier;
    
    float3 grain;
    if (grainType == 0) // Fast
    {
        float n = SimpleNoise(uv, time, size) * 2.0 - 1.0;
        grain = float3(n, n, n);
    }
    else if (grainType == 1) // FilmLike
    {
        float n = FilmNoise(uv, time, size) * 2.0 - 1.0;
        grain = float3(n, n, n);
    }
    else // Colored
    {
        grain = ColoredNoise(uv, time, size) * 2.0 - 1.0;
    }
    
    // Apply grain with luminance and color contributions
    float3 lumaGrain = grain * luminanceContrib;
    float3 chromaGrain = grain * colorContrib;
    
    // Blend grain with image
    color.rgb += (lumaGrain + chromaGrain * (color.rgb - luma)) * grainIntensity;
    color.rgb = max(color.rgb, 0.0);
    
    OutputColor[dispatchThreadId.xy] = color;
}
