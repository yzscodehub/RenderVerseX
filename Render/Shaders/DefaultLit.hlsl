// =============================================================================
// DefaultLit.hlsl - Default lit material shader for model rendering
// =============================================================================
//
// Descriptor sets:
//   set 0 / space0: frame data
//   set 1 / space1: object data
//   set 2 / space2: material data
//
// Vertex inputs come from separate vertex buffers:
//   Slot 0: Position buffer (float3)
//   Slot 1: Normal buffer (float3)
//   Slot 2: UV buffer (float2)
//   Slot 3: Tangent buffer (float4)
// =============================================================================

#include "Include/BRDF.hlsli"

#define MATERIAL_TEXTURE_BASE_COLOR 0x01
#define MATERIAL_TEXTURE_NORMAL 0x02
#define MATERIAL_TEXTURE_METALLIC_ROUGHNESS 0x04
#define MATERIAL_TEXTURE_OCCLUSION 0x08
#define MATERIAL_TEXTURE_EMISSIVE 0x10

#define MATERIAL_ALPHA_OPAQUE 0
#define MATERIAL_ALPHA_MASK 1
#define MATERIAL_ALPHA_BLEND 2

// =============================================================================
// Constant Buffers
// =============================================================================

cbuffer ViewConstants : register(b0, space0)
{
    float4x4 ViewProjection;
    float3 CameraPosition;
    float Time;
    float3 LightDirection;
    float Padding;
};

cbuffer ObjectConstants : register(b0, space1)
{
    float4x4 World;
};

cbuffer MaterialConstants : register(b0, space2)
{
    float4 BaseColorFactor;
    float MetallicFactor;
    float RoughnessFactor;
    float NormalScale;
    float OcclusionStrength;
    float3 EmissiveColor;
    float EmissiveStrength;
    uint TextureFlags;
    uint AlphaMode;
    float AlphaCutoff;
    uint Workflow;
    uint DoubleSided;
    float3 MaterialPadding;
};

Texture2D BaseColorTexture : register(t1, space2);
Texture2D NormalTexture : register(t2, space2);
Texture2D MetallicRoughnessTexture : register(t3, space2);
Texture2D OcclusionTexture : register(t4, space2);
Texture2D EmissiveTexture : register(t5, space2);
SamplerState MaterialSampler : register(s6, space2);

// =============================================================================
// Vertex Shader Input/Output
// =============================================================================

struct VSInput
{
    float3 Position : POSITION;
    float3 Normal   : NORMAL;
    float2 TexCoord : TEXCOORD0;
    float4 Tangent  : TANGENT;
};

struct PSInput
{
    float4 Position    : SV_POSITION;
    float3 WorldPos    : TEXCOORD0;
    float3 WorldNormal : TEXCOORD1;
    float2 TexCoord    : TEXCOORD2;
    float4 WorldTangent : TEXCOORD3;
};

// =============================================================================
// Vertex Shader
// =============================================================================

PSInput VSMain(VSInput input)
{
    PSInput output;

    float4 worldPos = mul(World, float4(input.Position, 1.0));
    output.WorldPos = worldPos.xyz;
    output.Position = mul(ViewProjection, worldPos);
    output.WorldNormal = normalize(mul((float3x3)World, input.Normal));
    output.TexCoord = input.TexCoord;
    output.WorldTangent = float4(normalize(mul((float3x3)World, input.Tangent.xyz)), input.Tangent.w);

    return output;
}

// =============================================================================
// Pixel Shader
// =============================================================================

float3 SampleNormalMap(float2 uv, float3 worldNormal, float4 worldTangent)
{
    float3 tangentNormal = NormalTexture.Sample(MaterialSampler, uv).xyz * 2.0 - 1.0;
    tangentNormal.xy *= NormalScale;

    float3 n = normalize(worldNormal);
    float3 t = normalize(worldTangent.xyz);
    t = normalize(t - n * dot(n, t));

    float tangentSign = worldTangent.w >= 0.0 ? 1.0 : -1.0;
    float3 b = normalize(cross(n, t) * tangentSign);
    float3x3 tbn = float3x3(t, b, n);

    return normalize(mul(tangentNormal, tbn));
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float4 baseColor = BaseColorFactor;
    if ((TextureFlags & MATERIAL_TEXTURE_BASE_COLOR) != 0)
    {
        baseColor *= BaseColorTexture.Sample(MaterialSampler, input.TexCoord);
    }

    if (AlphaMode == MATERIAL_ALPHA_MASK && baseColor.a < AlphaCutoff)
    {
        discard;
    }

    float metallic = MetallicFactor;
    float roughness = RoughnessFactor;
    if ((TextureFlags & MATERIAL_TEXTURE_METALLIC_ROUGHNESS) != 0)
    {
        float4 mr = MetallicRoughnessTexture.Sample(MaterialSampler, input.TexCoord);
        roughness *= mr.g;
        metallic *= mr.b;
    }

    float occlusion = 1.0;
    if ((TextureFlags & MATERIAL_TEXTURE_OCCLUSION) != 0)
    {
        occlusion = lerp(1.0, OcclusionTexture.Sample(MaterialSampler, input.TexCoord).r, OcclusionStrength);
    }

    float3 emissive = EmissiveColor * EmissiveStrength;
    if ((TextureFlags & MATERIAL_TEXTURE_EMISSIVE) != 0)
    {
        emissive *= EmissiveTexture.Sample(MaterialSampler, input.TexCoord).rgb;
    }

    float3 normal = normalize(input.WorldNormal);
    if ((TextureFlags & MATERIAL_TEXTURE_NORMAL) != 0)
    {
        normal = SampleNormalMap(input.TexCoord, normal, input.WorldTangent);
    }

    float3 toLight = normalize(-LightDirection);
    float3 viewDir = normalize(CameraPosition - input.WorldPos);
    float clampedRoughness = clamp(roughness, 0.04, 1.0);
    float3 f0 = ComputeF0(baseColor.rgb, metallic);

    float3 directLight = EvaluatePBR(
        normal,
        viewDir,
        toLight,
        baseColor.rgb,
        metallic,
        clampedRoughness,
        float3(4.0, 4.0, 4.0),
        1.0);

    float nDotV = max(dot(normal, viewDir), 0.001);
    float3 fresnel = F_SchlickRoughness(nDotV, f0, clampedRoughness);
    float3 ambientDiffuse = baseColor.rgb * (1.0 - fresnel) * (1.0 - metallic) * occlusion * 0.12;
    float3 ambientSpecular = f0 * occlusion * (1.0 - clampedRoughness) * 0.04;
    float3 finalColor = ambientDiffuse + ambientSpecular + directLight + emissive;

    return float4(finalColor, baseColor.a);
}
