/**
 * @file DeferredLighting.hlsl
 * @brief Canonical deferred opaque-lighting compute kernel.
 *
 * Set 0 mirrors the fixed GBuffer ABI.  The pass intentionally keeps the
 * first implementation narrow: StandardLit's direct directional term plus
 * IBL-approximate ambient and emissive.  Clustered/local lights, shadows and
 * texture IBL join the same lighting identity in GFX08.
 */

cbuffer DeferredLightingConstants : register(b0, space0)
{
    float4 DirectionalLightDirection_Intensity;
    float4 DirectionalLightColor_AmbientIntensity;
    float4 AmbientColor_Padding;
    float4 OutputSize_InvSize;
    // x = depth clear value, y = comparison epsilon, z = reverse-Z flag.
    float4 DepthBackground_ReverseZ;
};

#define DirectionalLightDirection DirectionalLightDirection_Intensity.xyz
#define DirectionalLightIntensity DirectionalLightDirection_Intensity.w
#define DirectionalLightColor DirectionalLightColor_AmbientIntensity.xyz
#define AmbientIntensity DirectionalLightColor_AmbientIntensity.w
#define AmbientColor AmbientColor_Padding.xyz
#define OutputSize OutputSize_InvSize.xy
#define DepthBackgroundValue DepthBackground_ReverseZ.x
#define DepthBackgroundEpsilon DepthBackground_ReverseZ.y
#define ReverseZ DepthBackground_ReverseZ.z

Texture2D<float4> GBuffer0 : register(t1, space0);
Texture2D<float4> GBuffer1 : register(t2, space0);
Texture2D<float4> GBuffer2 : register(t3, space0);
Texture2D<uint> GBuffer3 : register(t4, space0);
Texture2D<float> DeviceDepth : register(t5, space0);
RWTexture2D<float4> HDRSceneColor : register(u6, space0);

// GFX07 permits only one material classification row.  This stays separate
// from the per-draw material parameter slot used by the raster shader: GBuffer3
// always contains row zero, whose contents prove the StandardLit-only ABI.
static const uint RVX_DEFERRED_MATERIAL_TABLE_SCHEMA_VERSION = 1u;
static const uint RVX_DEFERRED_MATERIAL_SHADING_MODEL_STANDARD_LIT = 1u;

struct DeferredMaterialClassification
{
    uint shadingModel;
    uint schemaVersion;
    uint reserved0;
    uint reserved1;
};

StructuredBuffer<DeferredMaterialClassification> MaterialClassificationTable
    : register(t7, space0);

float3 DecodeOctahedralNormal(float2 encoded)
{
    float2 oct = encoded * 2.0 - 1.0;
    float3 normal = float3(oct, 1.0 - abs(oct.x) - abs(oct.y));
    if (normal.z < 0.0)
    {
        normal.xy = (1.0 - abs(normal.yx)) *
            float2(normal.x >= 0.0 ? 1.0 : -1.0,
                   normal.y >= 0.0 ? 1.0 : -1.0);
    }
    return normalize(normal);
}

float3 EvaluateDeferredStandardLit(
    float3 baseColor,
    float ambientOcclusion,
    float3 normal,
    float roughness,
    float metallic,
    float3 emissive)
{
    // The GFX07 kernel deliberately shares the metallic-roughness material
    // contract with Forward+.  It uses a stable energy-conserving diffuse
    // approximation until GFX08 supplies the common clustered/shadow/IBL
    // lighting plan.
    float3 lightDirection = normalize(-DirectionalLightDirection);
    float nDotL = saturate(dot(normal, lightDirection));
    float3 diffuse = baseColor * (1.0 - saturate(metallic));
    float roughnessEnergy = lerp(1.0, 0.5, saturate(roughness));
    float3 direct = diffuse * DirectionalLightColor *
        (nDotL * max(DirectionalLightIntensity, 0.0) * roughnessEnergy);
    float3 ambient = baseColor * AmbientColor *
        (max(AmbientIntensity, 0.0) * saturate(ambientOcclusion));
    return direct + ambient + max(emissive, 0.0);
}

bool IsBackgroundDepth(float deviceDepth)
{
    // The depth prepass clears far/background to 1 for normal Z and 0 for
    // reverse Z.  This must be a real lighting decision, not a compiler-live
    // resource reference: GBuffer contents are undefined for the background.
    if (ReverseZ > 0.5)
    {
        return deviceDepth <= DepthBackgroundValue + DepthBackgroundEpsilon;
    }
    return deviceDepth >= DepthBackgroundValue - DepthBackgroundEpsilon;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadId.xy;
    uint width = uint(max(OutputSize.x, 1.0));
    uint height = uint(max(OutputSize.y, 1.0));
    if (pixel.x >= width || pixel.y >= height)
    {
        return;
    }

    float4 albedoAO = GBuffer0.Load(int3(pixel, 0));
    float4 normalRoughnessMetallic = GBuffer1.Load(int3(pixel, 0));
    float3 emissive = GBuffer2.Load(int3(pixel, 0)).rgb;
    // GFX07 supports exactly its renderer-owned StandardLit row.  Any other
    // index or schema is fail-dark rather than silently interpreting a per-draw
    // material parameter slot as a classification-table address.
    uint materialTableIndex = GBuffer3.Load(int3(pixel, 0));
    float deviceDepth = DeviceDepth.Load(int3(pixel, 0));
    if (IsBackgroundDepth(deviceDepth))
    {
        HDRSceneColor[pixel] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    if (materialTableIndex != 0u)
    {
        HDRSceneColor[pixel] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    DeferredMaterialClassification materialClassification =
        MaterialClassificationTable[materialTableIndex];
    if (materialClassification.shadingModel !=
            RVX_DEFERRED_MATERIAL_SHADING_MODEL_STANDARD_LIT ||
        materialClassification.schemaVersion !=
            RVX_DEFERRED_MATERIAL_TABLE_SCHEMA_VERSION ||
        materialClassification.reserved0 != 0u ||
        materialClassification.reserved1 != 0u)
    {
        HDRSceneColor[pixel] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    float3 lit = EvaluateDeferredStandardLit(
        albedoAO.rgb,
        albedoAO.a,
        DecodeOctahedralNormal(normalRoughnessMetallic.xy),
        normalRoughnessMetallic.z,
        normalRoughnessMetallic.w,
        emissive);
    HDRSceneColor[pixel] = float4(lit, 1.0);
}
