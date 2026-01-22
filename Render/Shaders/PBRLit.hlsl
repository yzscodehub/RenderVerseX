// =============================================================================
// PBRLit.hlsl - Full PBR shader with metallic-roughness workflow
// =============================================================================
//
// Features:
// - Cook-Torrance specular BRDF with GGX distribution
// - Metallic-roughness workflow
// - Normal mapping support
// - Multi-light support (directional + point + spot)
// - Shadow mapping with CSM
// - Ambient occlusion
// - Emissive support
//
// =============================================================================

#include "Include/BRDF.hlsli"
#include "Include/Lighting.hlsli"

// =============================================================================
// Constant Buffers
// =============================================================================

cbuffer ViewConstants : register(b0)
{
    float4x4 ViewProjection;
    float3 CameraPosition;
    float Time;
    float3 LightDirection;      // Primary directional light
    float Padding;
    float3 AmbientColor;
    float AmbientIntensity;
};

cbuffer ObjectConstants : register(b1)
{
    float4x4 World;
    float4x4 WorldInverseTranspose;  // For proper normal transformation
};

cbuffer MaterialConstants : register(b2)
{
    float4 BaseColorFactor;
    float MetallicFactor;
    float RoughnessFactor;
    float NormalScale;
    float OcclusionStrength;
    float3 EmissiveColor;
    float EmissiveStrength;
    uint TextureFlags;  // Bitmask for which textures are bound
    float3 MaterialPadding;
};

cbuffer LightConstants : register(b3)
{
    DirectionalLight MainLight;
    uint NumPointLights;
    uint NumSpotLights;
    float2 LightPadding;
};

// =============================================================================
// Textures and Samplers
// =============================================================================

// Material textures
Texture2D BaseColorMap       : register(t0);
Texture2D NormalMap          : register(t1);
Texture2D MetallicRoughnessMap : register(t2);  // R=unused, G=roughness, B=metallic (glTF)
Texture2D OcclusionMap       : register(t3);
Texture2D EmissiveMap        : register(t4);

// Shadow map
Texture2DArray ShadowMap     : register(t5);

// Light buffers (for multi-light)
StructuredBuffer<PointLight> PointLights : register(t10);
StructuredBuffer<SpotLight> SpotLights   : register(t11);

// Samplers
SamplerState LinearSampler        : register(s0);
SamplerComparisonState ShadowSampler : register(s1);

// Texture flags
#define TEX_HAS_BASECOLOR       0x01
#define TEX_HAS_NORMAL          0x02
#define TEX_HAS_METALLICROUGHNESS 0x04
#define TEX_HAS_OCCLUSION       0x08
#define TEX_HAS_EMISSIVE        0x10

// =============================================================================
// Vertex Shader
// =============================================================================

struct VSInput
{
    float3 Position : POSITION;
    float3 Normal   : NORMAL;
    float2 TexCoord : TEXCOORD0;
    float4 Tangent  : TANGENT;  // xyz = tangent, w = handedness
};

struct PSInput
{
    float4 Position    : SV_POSITION;
    float3 WorldPos    : TEXCOORD0;
    float3 WorldNormal : TEXCOORD1;
    float2 TexCoord    : TEXCOORD2;
    float3 WorldTangent : TEXCOORD3;
    float3 WorldBitangent : TEXCOORD4;
    float ViewDepth    : TEXCOORD5;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    
    float4 worldPos = mul(World, float4(input.Position, 1.0));
    output.WorldPos = worldPos.xyz;
    output.Position = mul(ViewProjection, worldPos);
    
    // Transform normal using inverse transpose for proper scaling
    output.WorldNormal = normalize(mul((float3x3)WorldInverseTranspose, input.Normal));
    
    // Transform tangent
    output.WorldTangent = normalize(mul((float3x3)World, input.Tangent.xyz));
    
    // Compute bitangent with handedness
    output.WorldBitangent = cross(output.WorldNormal, output.WorldTangent) * input.Tangent.w;
    
    output.TexCoord = input.TexCoord;
    
    // Store view-space depth for CSM cascade selection
    float3 viewPos = worldPos.xyz - CameraPosition;
    output.ViewDepth = length(viewPos);
    
    return output;
}

// =============================================================================
// Normal Mapping
// =============================================================================

float3 SampleNormalMap(float2 uv, float3 N, float3 T, float3 B)
{
    if ((TextureFlags & TEX_HAS_NORMAL) == 0)
        return N;
    
    float3 tangentNormal = NormalMap.Sample(LinearSampler, uv).xyz * 2.0 - 1.0;
    tangentNormal.xy *= NormalScale;
    
    // TBN matrix
    float3x3 TBN = float3x3(T, B, N);
    return normalize(mul(tangentNormal, TBN));
}

// =============================================================================
// Material Sampling
// =============================================================================

struct MaterialSample
{
    float3 albedo;
    float metallic;
    float roughness;
    float ao;
    float3 emissive;
};

MaterialSample SampleMaterial(float2 uv)
{
    MaterialSample mat;
    
    // Base color
    if (TextureFlags & TEX_HAS_BASECOLOR)
        mat.albedo = BaseColorMap.Sample(LinearSampler, uv).rgb * BaseColorFactor.rgb;
    else
        mat.albedo = BaseColorFactor.rgb;
    
    // Metallic/Roughness (glTF packing: R=unused, G=roughness, B=metallic)
    if (TextureFlags & TEX_HAS_METALLICROUGHNESS)
    {
        float4 mr = MetallicRoughnessMap.Sample(LinearSampler, uv);
        mat.roughness = mr.g * RoughnessFactor;
        mat.metallic = mr.b * MetallicFactor;
    }
    else
    {
        mat.metallic = MetallicFactor;
        mat.roughness = RoughnessFactor;
    }
    
    // Clamp roughness to avoid singularities
    mat.roughness = max(mat.roughness, 0.04);
    
    // Ambient occlusion
    if (TextureFlags & TEX_HAS_OCCLUSION)
        mat.ao = lerp(1.0, OcclusionMap.Sample(LinearSampler, uv).r, OcclusionStrength);
    else
        mat.ao = 1.0;
    
    // Emissive
    if (TextureFlags & TEX_HAS_EMISSIVE)
        mat.emissive = EmissiveMap.Sample(LinearSampler, uv).rgb * EmissiveColor * EmissiveStrength;
    else
        mat.emissive = EmissiveColor * EmissiveStrength;
    
    return mat;
}

// =============================================================================
// Pixel Shader
// =============================================================================

float4 PSMain(PSInput input) : SV_TARGET
{
    // Sample material
    MaterialSample mat = SampleMaterial(input.TexCoord);
    
    // Compute normal (with normal mapping)
    float3 N = SampleNormalMap(input.TexCoord, 
                               normalize(input.WorldNormal),
                               normalize(input.WorldTangent),
                               normalize(input.WorldBitangent));
    
    // View direction
    float3 V = normalize(CameraPosition - input.WorldPos);
    
    // Accumulate lighting
    float3 Lo = float3(0.0, 0.0, 0.0);
    
    // Main directional light
    float shadow = 1.0;  // TODO: Sample shadow map
    Lo += EvaluateDirectionalLight(MainLight, N, V, input.WorldPos,
                                   mat.albedo, mat.metallic, mat.roughness, shadow);
    
    // Point lights
    for (uint i = 0; i < NumPointLights; ++i)
    {
        Lo += EvaluatePointLight(PointLights[i], N, V, input.WorldPos,
                                 mat.albedo, mat.metallic, mat.roughness);
    }
    
    // Spot lights
    for (uint j = 0; j < NumSpotLights; ++j)
    {
        Lo += EvaluateSpotLight(SpotLights[j], N, V, input.WorldPos,
                                mat.albedo, mat.metallic, mat.roughness, 1.0);
    }
    
    // Ambient lighting
    float3 ambient = AmbientLighting(N, V, mat.albedo, mat.metallic, mat.roughness,
                                     AmbientColor * AmbientIntensity, mat.ao);
    
    // Final color
    float3 color = ambient + Lo + mat.emissive;
    
    // Output (HDR - tone mapping applied in post-process)
    return float4(color, BaseColorFactor.a);
}
