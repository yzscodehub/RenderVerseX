/**
 * @file Caustics.hlsl
 * @brief Underwater caustics generation and application shaders
 * 
 * Generates animated caustic light patterns and applies them to
 * underwater surfaces.
 */

// =============================================================================
// Constant Buffers
// =============================================================================

cbuffer CausticsConstants : register(b0)
{
    float4 g_CausticsParams;        // (intensity, scale, speed, maxDepth)
    float4 g_LightDirection;        // (x, y, z, waterHeight)
    float4 g_TimeParams;            // (time, deltaTime, 0, 0)
    float4 g_TextureSize;           // (width, height, 1/width, 1/height)
};

// =============================================================================
// Textures
// =============================================================================

Texture2D<float4> g_DisplacementMap : register(t0);
Texture2D<float4> g_SceneDepth : register(t1);
RWTexture2D<float4> g_CausticsOutput : register(u0);

SamplerState g_LinearSampler : register(s0);

// =============================================================================
// Noise Functions for Procedural Caustics
// =============================================================================

// Simple value noise
float Hash(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

float Noise(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    
    float a = Hash(i);
    float b = Hash(i + float2(1.0, 0.0));
    float c = Hash(i + float2(0.0, 1.0));
    float d = Hash(i + float2(1.0, 1.0));
    
    float2 u = f * f * (3.0 - 2.0 * f);
    
    return lerp(a, b, u.x) + (c - a) * u.y * (1.0 - u.x) + (d - b) * u.x * u.y;
}

// Fractional Brownian Motion
float FBM(float2 p, int octaves)
{
    float value = 0.0;
    float amplitude = 0.5;
    float frequency = 1.0;
    
    for (int i = 0; i < octaves; i++)
    {
        value += amplitude * Noise(p * frequency);
        amplitude *= 0.5;
        frequency *= 2.0;
    }
    
    return value;
}

// Voronoi-based caustics pattern
float Voronoi(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    
    float minDist = 1.0;
    float secondMin = 1.0;
    
    for (int y = -1; y <= 1; y++)
    {
        for (int x = -1; x <= 1; x++)
        {
            float2 neighbor = float2(x, y);
            float2 point = Hash(i + neighbor).xx * 0.5 + 0.25;
            point += neighbor;
            
            float dist = length(point - f);
            
            if (dist < minDist)
            {
                secondMin = minDist;
                minDist = dist;
            }
            else if (dist < secondMin)
            {
                secondMin = dist;
            }
        }
    }
    
    return secondMin - minDist;
}

// =============================================================================
// Caustics Generation (Compute Shader)
// =============================================================================

[numthreads(8, 8, 1)]
void CS_GenerateCaustics(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    float2 uv = (dispatchThreadID.xy + 0.5) * g_TextureSize.zw;
    float time = g_TimeParams.x * g_CausticsParams.z;
    float scale = g_CausticsParams.y;
    
    // Animated UV coordinates
    float2 uv1 = uv * scale + float2(time * 0.1, time * 0.15);
    float2 uv2 = uv * scale * 1.3 + float2(-time * 0.12, time * 0.08);
    float2 uv3 = uv * scale * 0.7 + float2(time * 0.09, -time * 0.11);
    
    // Generate caustics pattern using Voronoi
    float caustics1 = Voronoi(uv1 * 8.0);
    float caustics2 = Voronoi(uv2 * 6.0);
    float caustics3 = Voronoi(uv3 * 10.0);
    
    // Combine patterns
    float caustics = caustics1 * 0.5 + caustics2 * 0.35 + caustics3 * 0.15;
    
    // Apply sharpening to create light focusing effect
    caustics = pow(caustics, 1.5) * 2.0;
    caustics = saturate(caustics);
    
    // Add some noise variation
    float noise = FBM(uv * scale * 2.0 + time * 0.05, 3);
    caustics *= 0.8 + noise * 0.4;
    
    // Output
    float intensity = caustics * g_CausticsParams.x;
    g_CausticsOutput[dispatchThreadID.xy] = float4(intensity.xxx, 1.0);
}

// =============================================================================
// Caustics Application (Full-screen Pixel Shader)
// =============================================================================

struct FullscreenVSOutput
{
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD0;
};

Texture2D<float4> g_CausticsTexture : register(t2);
Texture2D<float4> g_SceneColor : register(t3);

float4 PS_ApplyCaustics(FullscreenVSOutput input) : SV_TARGET
{
    float4 sceneColor = g_SceneColor.Sample(g_LinearSampler, input.TexCoord);
    
    // Get scene depth
    float depth = g_SceneDepth.Sample(g_LinearSampler, input.TexCoord).r;
    
    // Reconstruct world position from depth (simplified)
    // In real implementation, use inverse view-projection matrix
    float waterHeight = g_LightDirection.w;
    
    // Check if pixel is underwater
    // This is simplified - real implementation would use world position
    if (depth > 0.0)
    {
        // Calculate caustics UV based on world position
        float2 causticsUV = input.TexCoord;
        
        // Sample caustics
        float caustics = g_CausticsTexture.Sample(g_LinearSampler, causticsUV).r;
        
        // Depth-based falloff
        float maxDepth = g_CausticsParams.w;
        float depthFactor = saturate(1.0 - depth / maxDepth);
        
        // Apply caustics to scene color
        float3 causticsLight = g_LightDirection.xyz * caustics * depthFactor;
        sceneColor.rgb += causticsLight * g_CausticsParams.x;
    }
    
    return sceneColor;
}

// =============================================================================
// Ray-traced Caustics (Advanced, Compute Shader)
// =============================================================================

[numthreads(8, 8, 1)]
void CS_RaytraceCaustics(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    float2 uv = (dispatchThreadID.xy + 0.5) * g_TextureSize.zw;
    float time = g_TimeParams.x;
    
    // Sample water surface displacement and normal
    float3 displacement = g_DisplacementMap.SampleLevel(g_LinearSampler, uv, 0).xyz;
    
    // Calculate refracted light direction
    float3 lightDir = normalize(g_LightDirection.xyz);
    float3 normal = normalize(float3(displacement.x, 1.0, displacement.z));
    
    // Snell's law for refraction (air to water, n1=1.0, n2=1.33)
    float n1 = 1.0;
    float n2 = 1.33;
    float ratio = n1 / n2;
    
    float cosI = -dot(normal, lightDir);
    float sinT2 = ratio * ratio * (1.0 - cosI * cosI);
    
    float3 refracted = ratio * lightDir + (ratio * cosI - sqrt(1.0 - sinT2)) * normal;
    
    // Project refracted ray onto seafloor plane
    float waterHeight = g_LightDirection.w;
    float floorDepth = g_CausticsParams.w;
    
    float t = floorDepth / max(0.001, -refracted.y);
    float2 hitPoint = uv + refracted.xz * t * 0.01;
    
    // Accumulate light intensity at hit point
    // This is simplified - real implementation would use atomic operations
    float intensity = 1.0 / (1.0 + t * 0.1);
    
    g_CausticsOutput[dispatchThreadID.xy] = float4(intensity.xxx, 1.0);
}
