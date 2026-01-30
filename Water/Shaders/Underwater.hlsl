/**
 * @file Underwater.hlsl
 * @brief Underwater post-processing effects
 * 
 * Applies visual effects when the camera is submerged including
 * fog, color absorption, distortion, and god rays.
 */

// =============================================================================
// Constant Buffers
// =============================================================================

cbuffer UnderwaterConstants : register(b0)
{
    float4 g_FogColor;
    float4 g_AbsorptionColor;       // RGB absorption rates
    float4 g_FogParams;             // (density, start, end, cameraDepth)
    float4 g_DistortionParams;      // (strength, speed, scale, time)
    float4 g_GodRayParams;          // (intensity, decay, density, samples)
    float4 g_LightScreenPos;        // Light position in screen space
    float4 g_TextureSize;           // (width, height, 1/width, 1/height)
};

// =============================================================================
// Textures
// =============================================================================

Texture2D<float4> g_SceneColor : register(t0);
Texture2D<float> g_SceneDepth : register(t1);
Texture2D<float4> g_NoiseTexture : register(t2);

RWTexture2D<float4> g_OutputTexture : register(u0);

SamplerState g_LinearSampler : register(s0);
SamplerState g_LinearWrapSampler : register(s1);

// =============================================================================
// Vertex Shader (Fullscreen Triangle)
// =============================================================================

struct VSOutput
{
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD0;
};

VSOutput VS_Fullscreen(uint vertexID : SV_VertexID)
{
    VSOutput output;
    
    // Generate fullscreen triangle
    output.TexCoord = float2((vertexID << 1) & 2, vertexID & 2);
    output.Position = float4(output.TexCoord * 2.0 - 1.0, 0.0, 1.0);
    output.TexCoord.y = 1.0 - output.TexCoord.y;
    
    return output;
}

// =============================================================================
// Underwater Fog
// =============================================================================

float4 PS_UnderwaterFog(VSOutput input) : SV_TARGET
{
    float4 sceneColor = g_SceneColor.Sample(g_LinearSampler, input.TexCoord);
    float depth = g_SceneDepth.Sample(g_LinearSampler, input.TexCoord);
    
    // Convert depth to linear distance (simplified)
    float linearDepth = depth * 1000.0;  // Adjust based on camera near/far
    
    // Exponential fog
    float fogFactor = 1.0 - exp(-linearDepth * g_FogParams.x);
    fogFactor = saturate(fogFactor);
    
    // Distance-based fog (linear falloff)
    float distanceFog = saturate((linearDepth - g_FogParams.y) / (g_FogParams.z - g_FogParams.y));
    fogFactor = max(fogFactor, distanceFog);
    
    // Color absorption based on depth
    // Water absorbs red first, then green, blue last
    float3 absorption = exp(-linearDepth * g_AbsorptionColor.rgb * 0.01);
    sceneColor.rgb *= absorption;
    
    // Apply fog
    float3 foggedColor = lerp(sceneColor.rgb, g_FogColor.rgb, fogFactor);
    
    // Depth-based darkening
    float depthDarken = saturate(1.0 - linearDepth * 0.002);
    foggedColor *= 0.3 + depthDarken * 0.7;
    
    return float4(foggedColor, sceneColor.a);
}

// =============================================================================
// Screen Distortion
// =============================================================================

float4 PS_Distortion(VSOutput input) : SV_TARGET
{
    float time = g_DistortionParams.w;
    float strength = g_DistortionParams.x;
    float scale = g_DistortionParams.z;
    
    // Sample noise for distortion
    float2 noiseUV = input.TexCoord * scale + time * g_DistortionParams.y;
    float2 noise = g_NoiseTexture.Sample(g_LinearWrapSampler, noiseUV).rg;
    noise = noise * 2.0 - 1.0;  // Remap to [-1, 1]
    
    // Second layer of noise for more variation
    float2 noiseUV2 = input.TexCoord * scale * 0.7 - time * g_DistortionParams.y * 0.8;
    float2 noise2 = g_NoiseTexture.Sample(g_LinearWrapSampler, noiseUV2).rg;
    noise2 = noise2 * 2.0 - 1.0;
    
    // Combine noises
    float2 distortion = (noise + noise2 * 0.5) * strength;
    
    // Edge fade to avoid artifacts at screen edges
    float2 edgeFade = saturate(min(input.TexCoord, 1.0 - input.TexCoord) * 10.0);
    distortion *= edgeFade.x * edgeFade.y;
    
    // Sample scene with distorted UVs
    float2 distortedUV = input.TexCoord + distortion;
    float4 sceneColor = g_SceneColor.Sample(g_LinearSampler, distortedUV);
    
    // Chromatic aberration (optional)
    float2 chromaticOffset = distortion * 0.5;
    float r = g_SceneColor.Sample(g_LinearSampler, distortedUV + chromaticOffset).r;
    float b = g_SceneColor.Sample(g_LinearSampler, distortedUV - chromaticOffset).b;
    sceneColor.r = lerp(sceneColor.r, r, 0.3);
    sceneColor.b = lerp(sceneColor.b, b, 0.3);
    
    return sceneColor;
}

// =============================================================================
// God Rays (Radial Blur from Light Source)
// =============================================================================

float4 PS_GodRays(VSOutput input) : SV_TARGET
{
    float4 sceneColor = g_SceneColor.Sample(g_LinearSampler, input.TexCoord);
    
    // Direction from pixel to light
    float2 lightPos = g_LightScreenPos.xy;
    float2 deltaTexCoord = (input.TexCoord - lightPos) * g_GodRayParams.z;
    
    // Initial values
    float2 samplePos = input.TexCoord;
    float illuminationDecay = 1.0;
    float3 godRays = float3(0, 0, 0);
    
    int numSamples = (int)g_GodRayParams.w;
    
    // Ray march toward light
    [loop]
    for (int i = 0; i < numSamples; i++)
    {
        samplePos -= deltaTexCoord;
        
        // Sample scene
        float4 sample = g_SceneColor.Sample(g_LinearSampler, samplePos);
        
        // Check if sample is above water (would be bright)
        // This is simplified - real implementation would use a separate bright pass
        float brightness = dot(sample.rgb, float3(0.299, 0.587, 0.114));
        
        // Accumulate with decay
        sample.rgb *= illuminationDecay * brightness;
        godRays += sample.rgb;
        
        // Decay the illumination
        illuminationDecay *= g_GodRayParams.y;
    }
    
    // Normalize
    godRays /= (float)numSamples;
    
    // Add god rays to scene with underwater tint
    float3 underwaterTint = float3(0.3, 0.7, 1.0);  // Blue-green tint
    sceneColor.rgb += godRays * g_GodRayParams.x * underwaterTint;
    
    return sceneColor;
}

// =============================================================================
// Combined Underwater Effect (Compute Shader)
// =============================================================================

[numthreads(8, 8, 1)]
void CS_UnderwaterCombined(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    float2 uv = (dispatchThreadID.xy + 0.5) * g_TextureSize.zw;
    
    // Apply distortion first
    float time = g_DistortionParams.w;
    float2 noiseUV = uv * g_DistortionParams.z + time * g_DistortionParams.y;
    float2 noise = g_NoiseTexture.SampleLevel(g_LinearWrapSampler, noiseUV, 0).rg * 2.0 - 1.0;
    float2 distortedUV = uv + noise * g_DistortionParams.x;
    
    // Sample scene with distortion
    float4 sceneColor = g_SceneColor.SampleLevel(g_LinearSampler, distortedUV, 0);
    float depth = g_SceneDepth.SampleLevel(g_LinearSampler, uv, 0);
    
    // Apply color absorption
    float linearDepth = depth * 1000.0;
    float3 absorption = exp(-linearDepth * g_AbsorptionColor.rgb * 0.01);
    sceneColor.rgb *= absorption;
    
    // Apply fog
    float fogFactor = 1.0 - exp(-linearDepth * g_FogParams.x);
    sceneColor.rgb = lerp(sceneColor.rgb, g_FogColor.rgb, saturate(fogFactor));
    
    // Apply depth darkening
    float depthDarken = saturate(1.0 - linearDepth * 0.002);
    sceneColor.rgb *= 0.3 + depthDarken * 0.7;
    
    // Output
    g_OutputTexture[dispatchThreadID.xy] = sceneColor;
}

// =============================================================================
// Floating Particles (Vertex/Geometry/Pixel Shaders)
// =============================================================================

struct ParticleData
{
    float3 Position;
    float Size;
};

StructuredBuffer<ParticleData> g_Particles : register(t3);

cbuffer ParticleConstants : register(b1)
{
    float4x4 g_ViewProjection;
    float3 g_CameraPosition;
    float g_ParticleAlpha;
};

struct ParticleVSOutput
{
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD0;
    float Alpha : TEXCOORD1;
};

ParticleVSOutput VS_Particle(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
    ParticleVSOutput output;
    
    ParticleData particle = g_Particles[instanceID];
    
    // Quad corners
    float2 corners[4] = {
        float2(-1, -1),
        float2( 1, -1),
        float2(-1,  1),
        float2( 1,  1)
    };
    
    float2 corner = corners[vertexID];
    
    // Billboard facing camera
    float3 right = float3(1, 0, 0);  // Simplified - would use view matrix
    float3 up = float3(0, 1, 0);
    
    float3 worldPos = particle.Position + 
                       right * corner.x * particle.Size + 
                       up * corner.y * particle.Size;
    
    output.Position = mul(float4(worldPos, 1.0), g_ViewProjection);
    output.TexCoord = corner * 0.5 + 0.5;
    
    // Distance-based alpha fade
    float dist = length(particle.Position - g_CameraPosition);
    output.Alpha = saturate(1.0 - dist * 0.01) * g_ParticleAlpha;
    
    return output;
}

float4 PS_Particle(ParticleVSOutput input) : SV_TARGET
{
    // Simple circular particle
    float dist = length(input.TexCoord - 0.5) * 2.0;
    float alpha = saturate(1.0 - dist) * input.Alpha;
    
    // Soft particle edge
    alpha *= smoothstep(1.0, 0.5, dist);
    
    // White-ish particle with slight blue tint
    float3 color = float3(0.8, 0.9, 1.0);
    
    return float4(color, alpha);
}
