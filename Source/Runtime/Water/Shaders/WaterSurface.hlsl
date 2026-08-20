/**
 * @file WaterSurface.hlsl
 * @brief Water surface rendering shader
 * 
 * Renders the water surface with waves, reflections, refractions,
 * and foam effects.
 */

// =============================================================================
// Constant Buffers
// =============================================================================

cbuffer WaterConstants : register(b0)
{
    float4x4 g_WorldMatrix;
    float4x4 g_ViewProjection;
    float4x4 g_ReflectionMatrix;
    
    float4 g_SurfaceSize;           // (width, height, 1/width, 1/height)
    float4 g_WaveParams;            // (time, amplitude, frequency, speed)
    float4 g_ShallowColor;
    float4 g_DeepColor;
    float4 g_OpticalParams;         // (transparency, refraction, reflection, fresnel)
    float4 g_FoamParams;            // (threshold, intensity, falloff, 0)
    
    float3 g_CameraPosition;
    float g_Padding0;
    
    float3 g_SunDirection;
    float g_SpecularPower;
    
    float3 g_SunColor;
    float g_SpecularIntensity;
};

// =============================================================================
// Textures and Samplers
// =============================================================================

Texture2D<float4> g_DisplacementMap : register(t0);
Texture2D<float4> g_NormalMap : register(t1);
Texture2D<float4> g_FoamTexture : register(t2);
Texture2D<float4> g_ReflectionTexture : register(t3);
Texture2D<float4> g_RefractionTexture : register(t4);
Texture2D<float> g_DepthTexture : register(t5);
TextureCube<float4> g_EnvironmentMap : register(t6);

SamplerState g_LinearSampler : register(s0);
SamplerState g_PointSampler : register(s1);

// =============================================================================
// Structures
// =============================================================================

struct VSInput
{
    float3 Position : POSITION;
    float2 TexCoord : TEXCOORD0;
};

struct VSOutput
{
    float4 Position : SV_POSITION;
    float3 WorldPos : TEXCOORD0;
    float2 TexCoord : TEXCOORD1;
    float4 ClipPos : TEXCOORD2;
    float4 ReflectionPos : TEXCOORD3;
};

// =============================================================================
// Utility Functions
// =============================================================================

// Fresnel effect using Schlick approximation
float FresnelSchlick(float cosTheta, float F0, float power)
{
    return F0 + (1.0 - F0) * pow(saturate(1.0 - cosTheta), power);
}

// Sample displacement from displacement map
float3 SampleDisplacement(float2 uv)
{
    return g_DisplacementMap.SampleLevel(g_LinearSampler, uv, 0).xyz;
}

// Sample normal from normal map
float3 SampleNormal(float2 uv)
{
    float3 normal = g_NormalMap.Sample(g_LinearSampler, uv).xyz;
    return normalize(normal * 2.0 - 1.0);
}

// Gerstner wave calculation (for CPU fallback or simple mode)
float3 GerstnerWave(float2 position, float2 direction, float wavelength, 
                     float amplitude, float steepness, float time)
{
    float k = 2.0 * 3.14159 / wavelength;
    float c = sqrt(9.81 / k);
    float2 d = normalize(direction);
    float f = k * (dot(d, position) - c * time);
    float a = amplitude;
    float Q = steepness / (k * a);
    
    return float3(
        Q * a * d.x * cos(f),
        a * sin(f),
        Q * a * d.y * cos(f)
    );
}

// =============================================================================
// Vertex Shader
// =============================================================================

VSOutput VS_Main(VSInput input)
{
    VSOutput output;
    
    // Sample displacement from texture or calculate waves
    float3 displacement = SampleDisplacement(input.TexCoord);
    
    // Apply displacement to vertex position
    float3 worldPos = mul(float4(input.Position, 1.0), g_WorldMatrix).xyz;
    worldPos += displacement;
    
    // Transform to clip space
    output.Position = mul(float4(worldPos, 1.0), g_ViewProjection);
    output.WorldPos = worldPos;
    output.TexCoord = input.TexCoord;
    output.ClipPos = output.Position;
    
    // Reflection coordinates
    output.ReflectionPos = mul(float4(worldPos, 1.0), g_ReflectionMatrix);
    
    return output;
}

// =============================================================================
// Pixel Shader
// =============================================================================

float4 PS_Main(VSOutput input) : SV_TARGET
{
    // Sample water normal
    float3 normal = SampleNormal(input.TexCoord);
    
    // View direction
    float3 viewDir = normalize(g_CameraPosition - input.WorldPos);
    
    // Fresnel factor
    float fresnel = FresnelSchlick(
        dot(normal, viewDir), 
        g_OpticalParams.w,  // Fresnel bias
        5.0                 // Fresnel power
    );
    
    // Screen-space coordinates for sampling
    float2 screenUV = input.ClipPos.xy / input.ClipPos.w * 0.5 + 0.5;
    screenUV.y = 1.0 - screenUV.y;  // Flip Y
    
    // Refraction with distortion
    float2 refractionOffset = normal.xz * g_OpticalParams.y;
    float2 refractionUV = screenUV + refractionOffset;
    float4 refraction = g_RefractionTexture.Sample(g_LinearSampler, refractionUV);
    
    // Depth-based water color
    float sceneDepth = g_DepthTexture.Sample(g_PointSampler, screenUV);
    float waterDepth = input.ClipPos.w;
    float depthDifference = sceneDepth - waterDepth;
    float depthFactor = saturate(depthDifference * 0.1);
    
    float3 waterColor = lerp(g_ShallowColor.rgb, g_DeepColor.rgb, depthFactor);
    
    // Apply color absorption to refraction
    float3 absorption = exp(-depthDifference * float3(0.45, 0.05, 0.025));
    refraction.rgb *= absorption;
    
    // Blend refraction with water color
    float3 underwaterColor = lerp(refraction.rgb, waterColor, 1.0 - g_OpticalParams.x);
    
    // Reflection
    float2 reflectionUV = input.ReflectionPos.xy / input.ReflectionPos.w * 0.5 + 0.5;
    reflectionUV += normal.xz * 0.02;  // Slight distortion
    float4 reflection = g_ReflectionTexture.Sample(g_LinearSampler, reflectionUV);
    
    // Fallback to environment map if no planar reflection
    if (reflection.a < 0.01)
    {
        float3 reflectDir = reflect(-viewDir, normal);
        reflection = g_EnvironmentMap.Sample(g_LinearSampler, reflectDir);
    }
    
    // Combine reflection and refraction based on fresnel
    float3 finalColor = lerp(underwaterColor, reflection.rgb, fresnel * g_OpticalParams.z);
    
    // Specular highlights
    float3 halfDir = normalize(g_SunDirection + viewDir);
    float specular = pow(saturate(dot(normal, halfDir)), g_SpecularPower);
    finalColor += g_SunColor * specular * g_SpecularIntensity;
    
    // Foam
    float foam = g_FoamTexture.Sample(g_LinearSampler, input.TexCoord * 10.0).r;
    float foamMask = saturate((normal.y - g_FoamParams.x) * g_FoamParams.z);
    foamMask = max(foamMask, saturate(1.0 - depthDifference * 2.0));  // Shore foam
    finalColor = lerp(finalColor, float3(1, 1, 1), foam * foamMask * g_FoamParams.y);
    
    // Fog (optional, for distant water)
    float fogFactor = saturate(length(input.WorldPos - g_CameraPosition) / 5000.0);
    finalColor = lerp(finalColor, g_ShallowColor.rgb * 0.5, fogFactor * 0.5);
    
    return float4(finalColor, 1.0);
}

// =============================================================================
// Technique
// =============================================================================

// Main water surface rendering
technique WaterSurface
{
    pass P0
    {
        VertexShader = compile vs_5_0 VS_Main();
        PixelShader = compile ps_5_0 PS_Main();
    }
}
