// =============================================================================
// DefaultLit.hlsl - Default lit material shader for model rendering
// =============================================================================
//
// Descriptor sets:
//   set 0 / space0: frame data
//   set 1 / space1: object data
//   set 2 / space2: material data and environment IBL textures
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
    float DirectionalLightIntensity;
    float4 IBLDiffuseAmbient;   // rgb: color, a: diffuse intensity
    float4 IBLSpecularAmbient;  // rgb: color, a: specular intensity
    float4 IBLTextureParams;    // x: enabled, y: prefiltered mip count, z: intensity, w: ambient floor intensity
    float4 CameraForwardAndShadowCascadeCount; // xyz: camera forward, w: active cascade count
    float4x4 DirectionalShadowViewProjections[4];
    float4 DirectionalShadowParams; // x: enabled, y: depth bias, z: strength, w: UV-space PCF filter step
    float4 DirectionalShadowReceiverParams; // x: receiver normal bias in world units
    float4 DirectionalShadowCascadeSplits; // absolute camera-forward split distances
};

cbuffer ObjectConstants : register(b0, space1)
{
    float4x4 World;
    float4x4 NormalMatrix;
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
TextureCube IrradianceTexture : register(t7, space2);
TextureCube PrefilteredEnvironmentTexture : register(t8, space2);
Texture2D BRDFLUTTexture : register(t9, space2);
Texture2DArray<float> DirectionalShadowMapTexture : register(t1, space0);
SamplerState DirectionalShadowSampler : register(s2, space0);

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
    output.WorldNormal = normalize(mul((float3x3)NormalMatrix, input.Normal));
    output.TexCoord = input.TexCoord;
    output.WorldTangent = float4(normalize(mul((float3x3)World, input.Tangent.xyz)), input.Tangent.w);

    return output;
}

// =============================================================================
// Pixel Shader
// =============================================================================

float3 SafeNormalize(float3 value, float3 fallback)
{
    float lenSq = dot(value, value);
    return lenSq > 1.0e-8 ? value * rsqrt(lenSq) : fallback;
}

float3 SampleNormalMap(float2 uv, float3 worldNormal, float4 worldTangent)
{
    float3 tangentNormal = NormalTexture.Sample(MaterialSampler, uv).xyz * 2.0 - 1.0;
    tangentNormal.xy *= NormalScale;

    float3 n = SafeNormalize(worldNormal, float3(0.0, 0.0, 1.0));
    float3 t = SafeNormalize(worldTangent.xyz, float3(1.0, 0.0, 0.0));
    t = SafeNormalize(t - n * dot(n, t), float3(1.0, 0.0, 0.0));

    float tangentSign = worldTangent.w >= 0.0 ? 1.0 : -1.0;
    float3 b = SafeNormalize(cross(n, t) * tangentSign, float3(0.0, 1.0, 0.0));
    float3x3 tbn = float3x3(t, b, n);

    return SafeNormalize(mul(tangentNormal, tbn), n);
}

int SelectDirectionalShadowCascade(float3 worldPos)
{
    int cascadeCount = (int)clamp(round(CameraForwardAndShadowCascadeCount.w), 1.0, 4.0);
    float3 cameraForward = SafeNormalize(CameraForwardAndShadowCascadeCount.xyz, float3(0.0, 0.0, -1.0));
    float viewDepth = dot(worldPos - CameraPosition, cameraForward);
    int cascadeIndex = 0;

    [unroll]
    for (int i = 0; i < 3; ++i)
    {
        if (i + 1 < cascadeCount && viewDepth > DirectionalShadowCascadeSplits[i])
        {
            cascadeIndex = i + 1;
        }
    }

    return cascadeIndex;
}

float CompareDirectionalShadowDepth(float2 uv, float compareDepth, int cascadeIndex)
{
    float storedDepth = DirectionalShadowMapTexture.SampleLevel(
        DirectionalShadowSampler,
        float3(uv, (float)cascadeIndex),
        0).r;
    return compareDepth <= storedDepth ? 1.0 : 0.0;
}

float SampleDirectionalShadowPCF(float2 shadowUV, float compareDepth, float filterStep, int cascadeIndex)
{
    float filterStepUv = max(filterStep, 0.0);
    if (filterStepUv <= 1.0e-7)
    {
        return CompareDirectionalShadowDepth(shadowUV, compareDepth, cascadeIndex);
    }

    float visibility = 0.0;
    float tapCount = 0.0;

    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            float2 tapUV = shadowUV + float2((float)x, (float)y) * filterStepUv;
            if (!any(tapUV < 0.0) && !any(tapUV > 1.0))
            {
                visibility += CompareDirectionalShadowDepth(tapUV, compareDepth, cascadeIndex);
                tapCount += 1.0;
            }
        }
    }

    return tapCount > 0.5 ? visibility / tapCount : 1.0;
}

float SampleDirectionalShadow(float3 worldPos, float3 worldNormal)
{
    if (DirectionalShadowParams.x <= 0.5)
    {
        return 1.0;
    }

    float3 receiverNormal = SafeNormalize(worldNormal, float3(0.0, 1.0, 0.0));
    float normalBias = max(DirectionalShadowReceiverParams.x, 0.0);
    float3 biasedWorldPos = worldPos + receiverNormal * normalBias;
    int cascadeIndex = SelectDirectionalShadowCascade(worldPos);
    float4 shadowClip = mul(DirectionalShadowViewProjections[cascadeIndex], float4(biasedWorldPos, 1.0));
    if (abs(shadowClip.w) <= 1.0e-6)
    {
        return 1.0;
    }

    float3 shadowNdc = shadowClip.xyz / shadowClip.w;
    float2 shadowUV = float2(shadowNdc.x * 0.5 + 0.5, 0.5 - shadowNdc.y * 0.5);
    if (any(shadowUV < 0.0) || any(shadowUV > 1.0) || shadowNdc.z < 0.0 || shadowNdc.z > 1.0)
    {
        return 1.0;
    }

    float compareDepth = shadowNdc.z - max(DirectionalShadowParams.y, 0.0);
    float lit = SampleDirectionalShadowPCF(shadowUV, compareDepth, DirectionalShadowParams.w, cascadeIndex);
    return lerp(1.0 - saturate(DirectionalShadowParams.z), 1.0, lit);
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

    float3 normal = SafeNormalize(input.WorldNormal, float3(0.0, 0.0, 1.0));
    if ((TextureFlags & MATERIAL_TEXTURE_NORMAL) != 0)
    {
        normal = SampleNormalMap(input.TexCoord, normal, input.WorldTangent);
    }

    float3 toLight = SafeNormalize(-LightDirection, float3(0.0, 1.0, 0.0));
    float3 viewDir = SafeNormalize(CameraPosition - input.WorldPos, float3(0.0, 0.0, 1.0));
    float clampedRoughness = clamp(roughness, 0.04, 1.0);
    float3 f0 = ComputeF0(baseColor.rgb, metallic);

    float shadowVisibility = SampleDirectionalShadow(input.WorldPos, normal);
    float3 directLight = EvaluatePBR(
        normal,
        viewDir,
        toLight,
        baseColor.rgb,
        metallic,
        clampedRoughness,
        float3(DirectionalLightIntensity, DirectionalLightIntensity, DirectionalLightIntensity),
        shadowVisibility);

    float nDotV = max(dot(normal, viewDir), 0.001);
    float3 fresnel = F_SchlickRoughness(nDotV, f0, clampedRoughness);
    float3 ambientDiffuse;
    float3 ambientSpecular;
    if (IBLTextureParams.x > 0.5)
    {
        float3 irradiance = IrradianceTexture.Sample(MaterialSampler, normal).rgb * IBLTextureParams.z;
        float prefilteredMip = clampedRoughness * max(IBLTextureParams.y - 1.0, 0.0);
        float3 reflectionDir = reflect(-viewDir, normal);
        float3 prefilteredColor = PrefilteredEnvironmentTexture.SampleLevel(
            MaterialSampler,
            reflectionDir,
            prefilteredMip).rgb * IBLTextureParams.z;
        float2 brdf = BRDFLUTTexture.Sample(MaterialSampler, float2(nDotV, clampedRoughness)).rg;

        float3 diffuseEnergy = baseColor.rgb * (1.0 - fresnel) * (1.0 - metallic);
        ambientDiffuse = diffuseEnergy * irradiance * occlusion;
        ambientSpecular = prefilteredColor * (fresnel * brdf.x + brdf.y) * occlusion;
    }
    else
    {
        float3 iblDiffuse = IBLDiffuseAmbient.rgb * IBLDiffuseAmbient.a;
        float3 iblSpecular = IBLSpecularAmbient.rgb * IBLSpecularAmbient.a;
        ambientDiffuse = baseColor.rgb * (1.0 - fresnel) * (1.0 - metallic) * occlusion * iblDiffuse;
        ambientSpecular = f0 * occlusion * (1.0 - clampedRoughness) * iblSpecular;
    }
    float3 ambientFloor = baseColor.rgb * max(IBLTextureParams.w, 0.0);
    float3 finalColor = ambientDiffuse + ambientSpecular + directLight + emissive + ambientFloor;

    return float4(finalColor, baseColor.a);
}
