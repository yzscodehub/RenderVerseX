/**
 * @file VolumetricLighting.hlsl
 * @brief Volumetric lighting shader
 * 
 * Ray-marched volumetric lighting implementation.
 */

// =============================================================================
// Constants
// =============================================================================

cbuffer VolumetricConstants : register(b0)
{
    float4x4 InvViewProj;
    float4x4 LightViewProj;
    float4x4 ViewProj;
    float4x4 PrevViewProj;
    
    float4 CameraPosition;      // xyz: position, w: unused
    float4 LightDirection;      // xyz: direction, w: unused
    float4 LightColor;          // rgb: color, a: intensity
    
    float4 ScatteringParams;    // x: scattering, y: absorption, z: anisotropy, w: intensity
    float4 RayMarchParams;      // x: maxDistance, y: jitter, z: sampleCount, w: frameIndex
    float4 HeightFogParams;     // x: enabled, y: height, z: falloff, w: unused
    float4 NoiseParams;         // x: enabled, y: scale, z: intensity, w: unused
    float4 TemporalParams;      // x: blendWeight, y: unused, z: unused, w: unused
    float4 ScreenSize;          // x: width, y: height, z: 1/width, w: 1/height
}

#define Scattering      ScatteringParams.x
#define Absorption      ScatteringParams.y
#define Anisotropy      ScatteringParams.z
#define Intensity       ScatteringParams.w
#define MaxDistance     RayMarchParams.x
#define JitterAmount    RayMarchParams.y
#define SampleCount     (uint)RayMarchParams.z
#define FrameIndex      (uint)RayMarchParams.w
#define UseHeightFog    (HeightFogParams.x > 0.5)
#define FogHeight       HeightFogParams.y
#define FogFalloff      HeightFogParams.z
#define UseNoise        (NoiseParams.x > 0.5)
#define NoiseScale      NoiseParams.y
#define NoiseIntensity  NoiseParams.z
#define TemporalWeight  TemporalParams.x

Texture2D<float> DepthTexture : register(t0);
Texture2D<float> ShadowMap : register(t1);
Texture3D<float> NoiseTexture : register(t2);
Texture2D<float4> HistoryTexture : register(t3);
Texture2D<float4> VolumetricTexture : register(t4);
Texture2D<float4> SceneColor : register(t5);

RWTexture2D<float4> VolumetricOutput : register(u0);
RWTexture2D<float4> CompositeOutput : register(u1);

SamplerState LinearClampSampler : register(s0);
SamplerState PointClampSampler : register(s1);
SamplerComparisonState ShadowSampler : register(s2);

// =============================================================================
// Helper functions
// =============================================================================

// Reconstruct world position from depth
float3 ReconstructWorldPosition(float2 uv, float depth)
{
    float4 clipPos = float4(uv * 2.0 - 1.0, depth, 1.0);
    clipPos.y = -clipPos.y; // Flip for DirectX
    
    float4 worldPos = mul(InvViewProj, clipPos);
    return worldPos.xyz / worldPos.w;
}

// Linear depth from depth buffer (reverse-Z)
float LinearizeDepth(float depth, float near)
{
    return near / depth;
}

// Henyey-Greenstein phase function
// g: anisotropy factor (-1 = back-scatter, 0 = isotropic, 1 = forward-scatter)
float HenyeyGreenstein(float cosTheta, float g)
{
    float g2 = g * g;
    float denom = 1.0 + g2 - 2.0 * g * cosTheta;
    return (1.0 - g2) / (4.0 * 3.14159 * pow(abs(denom), 1.5));
}

// Schlick approximation of HG (faster)
float SchlickPhase(float cosTheta, float k)
{
    float denom = 1.0 + k * cosTheta;
    return (1.0 - k * k) / (4.0 * 3.14159 * denom * denom);
}

// Sample shadow map
float SampleShadow(float3 worldPos)
{
    float4 lightClipPos = mul(LightViewProj, float4(worldPos, 1.0));
    lightClipPos.xyz /= lightClipPos.w;
    
    float2 shadowUV = lightClipPos.xy * 0.5 + 0.5;
    shadowUV.y = 1.0 - shadowUV.y;
    
    // Out of shadow frustum
    if (any(shadowUV < 0.0) || any(shadowUV > 1.0))
        return 1.0;
    
    return ShadowMap.SampleCmpLevelZero(ShadowSampler, shadowUV, lightClipPos.z);
}

// Height-based fog density
float GetFogDensity(float3 worldPos)
{
    if (!UseHeightFog)
        return 1.0;
    
    float height = worldPos.y;
    float fogFactor = exp(-max(height - FogHeight, 0.0) * FogFalloff);
    return fogFactor;
}

// 3D noise for fog variation
float GetNoiseDensity(float3 worldPos)
{
    if (!UseNoise)
        return 1.0;
    
    float3 noiseCoord = worldPos * NoiseScale;
    float noise = NoiseTexture.SampleLevel(LinearClampSampler, noiseCoord, 0);
    return lerp(1.0 - NoiseIntensity, 1.0 + NoiseIntensity, noise);
}

// Interleaved gradient noise for temporal jitter
float InterleavedGradientNoise(float2 pixelCoord, uint frame)
{
    pixelCoord += float(frame % 8) * float2(47.0, 17.0);
    const float3 magic = float3(0.06711056, 0.00583715, 52.9829189);
    return frac(magic.z * frac(dot(pixelCoord, magic.xy)));
}

// =============================================================================
// Ray march pass
// =============================================================================

[numthreads(8, 8, 1)]
void CS_RayMarch(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= (uint)ScreenSize.x || 
        dispatchThreadId.y >= (uint)ScreenSize.y)
        return;
    
    float2 uv = (dispatchThreadId.xy + 0.5) * ScreenSize.zw;
    
    // Get scene depth
    float sceneDepth = DepthTexture.SampleLevel(PointClampSampler, uv, 0);
    float3 sceneWorldPos = ReconstructWorldPosition(uv, sceneDepth);
    float3 cameraPos = CameraPosition.xyz;
    
    // Ray direction
    float3 rayDir = normalize(sceneWorldPos - cameraPos);
    float sceneDistance = length(sceneWorldPos - cameraPos);
    float rayLength = min(sceneDistance, MaxDistance);
    
    // Jitter starting position for temporal stability
    float jitter = InterleavedGradientNoise(dispatchThreadId.xy, FrameIndex) * JitterAmount;
    float stepSize = rayLength / float(SampleCount);
    
    // Phase function
    float cosTheta = dot(rayDir, -LightDirection.xyz);
    float phase = HenyeyGreenstein(cosTheta, Anisotropy);
    
    // Accumulate in-scattered light
    float3 inScattering = 0;
    float transmittance = 1.0;
    
    for (uint i = 0; i < SampleCount; i++)
    {
        float t = (float(i) + jitter) * stepSize;
        float3 samplePos = cameraPos + rayDir * t;
        
        // Get density at this point
        float fogDensity = GetFogDensity(samplePos);
        float noiseDensity = GetNoiseDensity(samplePos);
        float density = fogDensity * noiseDensity * Scattering;
        
        // Shadow visibility
        float shadow = SampleShadow(samplePos);
        
        // In-scattering from directional light
        float3 lightContrib = LightColor.rgb * LightColor.a * phase * shadow;
        
        // Beer-Lambert law for extinction
        float extinction = density + Absorption;
        float stepTransmittance = exp(-extinction * stepSize);
        
        // Accumulate
        float3 scatteredLight = lightContrib * density * stepSize * transmittance;
        inScattering += scatteredLight;
        transmittance *= stepTransmittance;
        
        // Early out if transmittance is very low
        if (transmittance < 0.01)
            break;
    }
    
    // Output: RGB = in-scattered light, A = transmittance
    VolumetricOutput[dispatchThreadId.xy] = float4(inScattering * Intensity, transmittance);
}

// =============================================================================
// Temporal reprojection pass
// =============================================================================

[numthreads(8, 8, 1)]
void CS_TemporalReproject(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= (uint)ScreenSize.x || 
        dispatchThreadId.y >= (uint)ScreenSize.y)
        return;
    
    float2 uv = (dispatchThreadId.xy + 0.5) * ScreenSize.zw;
    
    // Current frame
    float4 current = VolumetricTexture[dispatchThreadId.xy];
    
    // Reproject to previous frame
    float sceneDepth = DepthTexture.SampleLevel(PointClampSampler, uv, 0);
    float3 worldPos = ReconstructWorldPosition(uv, sceneDepth);
    
    float4 prevClipPos = mul(PrevViewProj, float4(worldPos, 1.0));
    float2 prevUV = prevClipPos.xy / prevClipPos.w * 0.5 + 0.5;
    prevUV.y = 1.0 - prevUV.y;
    
    // Check if reprojection is valid
    bool validReproject = all(prevUV >= 0.0) && all(prevUV <= 1.0);
    
    float4 history = current;
    if (validReproject)
    {
        history = HistoryTexture.SampleLevel(LinearClampSampler, prevUV, 0);
    }
    
    // Blend current and history
    float blendWeight = validReproject ? TemporalWeight : 0.0;
    float4 result = lerp(current, history, blendWeight);
    
    VolumetricOutput[dispatchThreadId.xy] = result;
}

// =============================================================================
// Bilateral upscale pass
// =============================================================================

[numthreads(8, 8, 1)]
void CS_BilateralUpscale(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= (uint)ScreenSize.x || 
        dispatchThreadId.y >= (uint)ScreenSize.y)
        return;
    
    float2 uv = (dispatchThreadId.xy + 0.5) * ScreenSize.zw;
    
    // Get full-res depth
    float centerDepth = DepthTexture.SampleLevel(PointClampSampler, uv, 0);
    
    // Bilateral filter using depth weights
    float4 colorSum = 0;
    float weightSum = 0;
    
    const float depthSigma = 0.01;
    
    for (int dy = -1; dy <= 1; dy++)
    {
        for (int dx = -1; dx <= 1; dx++)
        {
            float2 offset = float2(dx, dy) * 2.0 * ScreenSize.zw;
            float2 sampleUV = uv + offset;
            
            float4 sampleColor = VolumetricTexture.SampleLevel(LinearClampSampler, sampleUV, 0);
            float sampleDepth = DepthTexture.SampleLevel(PointClampSampler, sampleUV, 0);
            
            float depthDiff = abs(centerDepth - sampleDepth);
            float depthWeight = exp(-depthDiff * depthDiff / (2.0 * depthSigma * depthSigma));
            
            colorSum += sampleColor * depthWeight;
            weightSum += depthWeight;
        }
    }
    
    VolumetricOutput[dispatchThreadId.xy] = colorSum / max(weightSum, 0.0001);
}

// =============================================================================
// Composite pass
// =============================================================================

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput VS_Fullscreen(uint vertexId : SV_VertexID)
{
    VSOutput output;
    output.uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(output.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return output;
}

float4 PS_Composite(VSOutput input) : SV_Target
{
    float4 scene = SceneColor.Sample(LinearClampSampler, input.uv);
    float4 volumetric = VolumetricTexture.Sample(LinearClampSampler, input.uv);
    
    // Apply extinction to scene color
    float3 result = scene.rgb * volumetric.a;
    
    // Add in-scattered light
    result += volumetric.rgb;
    
    return float4(result, scene.a);
}

[numthreads(8, 8, 1)]
void CS_Composite(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= (uint)ScreenSize.x || 
        dispatchThreadId.y >= (uint)ScreenSize.y)
        return;
    
    float2 uv = (dispatchThreadId.xy + 0.5) * ScreenSize.zw;
    
    float4 scene = SceneColor[dispatchThreadId.xy];
    float4 volumetric = VolumetricTexture.SampleLevel(LinearClampSampler, uv, 0);
    
    float3 result = scene.rgb * volumetric.a + volumetric.rgb;
    
    CompositeOutput[dispatchThreadId.xy] = float4(result, scene.a);
}
