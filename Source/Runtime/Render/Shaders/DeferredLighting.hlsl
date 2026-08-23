/**
 * @file DeferredLighting.hlsl
 * @brief Compute lighting for the canonical deferred opaque GBuffer.
 *
 * The set-0 ABI intentionally does not borrow PipelineCache's mutable frame
 * descriptor. DeferredLightingPass seals a record-owned lighting snapshot, then
 * binds the same local lights, CSM and texture IBL semantics as DefaultLit.
 */

#include "Include/BRDF.hlsli"
#include "Include/Lighting.hlsli"

cbuffer DeferredLightingConstants : register(b0, space0)
{
    float4x4 InverseViewProjection;
    float4 OutputSize_InvSize;
    // x = clear depth, y = comparison epsilon, z = reverse-Z flag.
    float4 DepthBackground_ReverseZ;
};

// This is the strong, record-owned ViewConstants snapshot created by
// PipelineCache::CreateLightingBindingSnapshot. Its ABI intentionally mirrors
// DefaultLit so both raster paths use identical directional, CSM and IBL rules.
cbuffer DeferredLightingViewConstants : register(b20, space0)
{
    float4x4 ViewProjection;
    float4 CameraPosition_Time;
    float4 LightDirection_Intensity;
    float4 DirectionalLightColor_Padding;
    float4 IBLDiffuseAmbient;
    float4 IBLSpecularAmbient;
    float4 IBLTextureParams;
    float4 CameraForwardAndShadowCascadeCount;
    float4x4 DirectionalShadowViewProjections[4];
    float4 DirectionalShadowParams;
    float4 DirectionalShadowReceiverParams;
    float4 DirectionalShadowCascadeSplits;
    float4 DirectionalShadowCascadeFadeDistances;
    float4 RayTracedShadowParams;
};

cbuffer LightConstants : register(b10, space0)
{
    float4 MainLightDirection_Intensity;
    uint4 MainLightColor_ShadowMapIndexBits;
    float4x4 MainLightSpaceMatrix;
    uint NumPointLights;
    uint NumSpotLights;
    float2 LightPadding;
};

cbuffer ClusterConstants : register(b13, space0)
{
    float4 ClusterSize;
    float4 ClusterScreenParams;
    float4x4 ClusterInvProj;
};

#define CameraPosition CameraPosition_Time.xyz
#define LightDirection LightDirection_Intensity.xyz
#define DirectionalLightIntensity LightDirection_Intensity.w
#define DirectionalLightColor DirectionalLightColor_Padding.xyz
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

static const uint RVX_DEFERRED_MATERIAL_TABLE_SCHEMA_VERSION = 1u;
static const uint RVX_DEFERRED_MATERIAL_SHADING_MODEL_STANDARD_LIT = 1u;

struct DeferredMaterialClassification
{
    uint shadingModel;
    uint schemaVersion;
    uint reserved0;
    uint reserved1;
};

struct GPUCluster
{
    uint offset;
    uint count;
    uint pointCount;
    uint spotCount;
};

StructuredBuffer<DeferredMaterialClassification> MaterialClassificationTable : register(t7, space0);
Texture2DArray<float> DirectionalShadowMapTexture : register(t8, space0);
SamplerState DirectionalShadowSampler : register(s9, space0);
StructuredBuffer<PointLight> PointLights : register(t11, space0);
StructuredBuffer<SpotLight> SpotLights : register(t12, space0);
StructuredBuffer<GPUCluster> ClusterData : register(t14, space0);
StructuredBuffer<uint> ClusterLightIndices : register(t15, space0);
TextureCube IrradianceTexture : register(t16, space0);
TextureCube PrefilteredEnvironmentTexture : register(t17, space0);
Texture2D BRDFLUTTexture : register(t18, space0);
SamplerState IBLLinearClampSampler : register(s19, space0);

static const uint RVX_INVALID_CLUSTER_INDEX = 0xffffffffu;
static const uint RVX_MAX_CLUSTERED_LIGHTS_PER_PIXEL = 100u;

float3 SafeNormalize(float3 value, float3 fallback)
{
    const float lengthSquared = dot(value, value);
    return lengthSquared > 1.0e-8 ? value * rsqrt(lengthSquared) : fallback;
}

#include "Include/DirectionalShadow.hlsli"

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
    return SafeNormalize(normal, float3(0.0, 0.0, 1.0));
}

float3 ReconstructWorldPosition(uint2 pixel, float deviceDepth)
{
    const float2 uv = (float2(pixel) + 0.5) * OutputSize_InvSize.zw;
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 world = mul(InverseViewProjection, float4(ndc, deviceDepth, 1.0));
    world.xyz /= abs(world.w) > 1.0e-6 ? world.w : 1.0;
    return world.xyz;
}

bool IsBackgroundDepth(float deviceDepth)
{
    return ReverseZ > 0.5
        ? deviceDepth <= DepthBackgroundValue + DepthBackgroundEpsilon
        : deviceDepth >= DepthBackgroundValue - DepthBackgroundEpsilon;
}

bool IsClusteredLightingEnabled()
{
    return ClusterSize.w >= 1.0 && ClusterSize.x >= 1.0 &&
        ClusterSize.y >= 1.0 && ClusterSize.z >= 1.0 &&
        ClusterScreenParams.x >= 1.0 &&
        ClusterScreenParams.y >= 1.0 &&
        ClusterScreenParams.w > ClusterScreenParams.z;
}

uint ResolveClusterIndex(uint2 pixel, float3 worldPos)
{
    if (!IsClusteredLightingEnabled())
    {
        return RVX_INVALID_CLUSTER_INDEX;
    }

    const uint clusterCountX = max((uint)round(ClusterSize.x), 1u);
    const uint clusterCountY = max((uint)round(ClusterSize.y), 1u);
    const uint clusterCountZ = max((uint)round(ClusterSize.z), 1u);
    const uint totalClusterCount = max((uint)round(ClusterSize.w), 1u);
    const float screenX = saturate((pixel.x + 0.5) / max(ClusterScreenParams.x, 1.0));
    const float screenY = saturate((pixel.y + 0.5) / max(ClusterScreenParams.y, 1.0));
    const uint clusterX = min((uint)(screenX * clusterCountX), clusterCountX - 1u);
    const uint clusterY = min((uint)(screenY * clusterCountY), clusterCountY - 1u);

    const float nearPlane = max(ClusterScreenParams.z, 0.0001);
    const float farPlane = max(ClusterScreenParams.w, nearPlane + 0.0001);
    const float viewDepth = GetDirectionalShadowViewDepth(worldPos);
    if (viewDepth <= nearPlane || viewDepth >= farPlane)
    {
        return RVX_INVALID_CLUSTER_INDEX;
    }
    const float depthSlice = saturate(log(max(viewDepth, nearPlane) / nearPlane) /
        max(log(farPlane / nearPlane), 0.0001));
    const uint clusterZ = min((uint)(depthSlice * clusterCountZ), clusterCountZ - 1u);
    const uint clusterIndex = clusterX + clusterY * clusterCountX +
        clusterZ * clusterCountX * clusterCountY;
    return clusterIndex < totalClusterCount ? clusterIndex : RVX_INVALID_CLUSTER_INDEX;
}

float3 EvaluateLinearLocalLights(
    float3 normal, float3 viewDir, float3 worldPos, float3 baseColor,
    float metallic, float roughness)
{
    float3 result = 0.0;
    [loop]
    for (uint index = 0; index < min(NumPointLights, 256u); ++index)
    {
        result += EvaluatePointLight(
            PointLights[index], normal, viewDir, worldPos, baseColor, metallic, roughness);
    }
    [loop]
    for (uint index = 0; index < min(NumSpotLights, 128u); ++index)
    {
        result += EvaluateSpotLight(
            SpotLights[index], normal, viewDir, worldPos, baseColor, metallic, roughness, 1.0);
    }
    return result;
}

float3 EvaluateClusteredLocalLights(
    uint2 pixel, float3 normal, float3 viewDir, float3 worldPos,
    float3 baseColor, float metallic, float roughness)
{
    const uint clusterIndex = ResolveClusterIndex(pixel, worldPos);
    if (clusterIndex == RVX_INVALID_CLUSTER_INDEX)
    {
        return EvaluateLinearLocalLights(
            normal, viewDir, worldPos, baseColor, metallic, roughness);
    }

    const GPUCluster cluster = ClusterData[clusterIndex];
    const uint lightCount = min(cluster.count, RVX_MAX_CLUSTERED_LIGHTS_PER_PIXEL);
    float3 result = 0.0;
    [loop]
    for (uint index = 0; index < lightCount; ++index)
    {
        const uint packedIndex = ClusterLightIndices[cluster.offset + index];
        const uint lightIndex = packedIndex & 0xffffu;
        const uint lightType = (packedIndex >> 16u) & 0xffffu;
        if (lightType == 0u && lightIndex < NumPointLights)
        {
            result += EvaluatePointLight(
                PointLights[lightIndex], normal, viewDir, worldPos,
                baseColor, metallic, roughness);
        }
        else if (lightType == 1u && lightIndex < NumSpotLights)
        {
            result += EvaluateSpotLight(
                SpotLights[lightIndex], normal, viewDir, worldPos,
                baseColor, metallic, roughness, 1.0);
        }
    }
    return result;
}

float3 EvaluateEnvironmentIBL(
    float3 normal, float3 viewDir, float3 baseColor, float metallic,
    float roughness, float ambientOcclusion)
{
    const float3 f0 = ComputeF0(baseColor, metallic);
    const float nDotV = max(dot(normal, viewDir), 0.001);
    const float3 fresnel = F_SchlickRoughness(nDotV, f0, roughness);
    float3 diffuseAmbient;
    float3 specularAmbient;
    if (IBLTextureParams.x > 0.5)
    {
        // Compute shaders have no implicit screen-space derivatives. Irradiance
        // and the BRDF LUT are sampled at their authored base level; the
        // prefiltered environment below still selects its roughness mip.
        const float3 irradiance = IrradianceTexture.SampleLevel(
            IBLLinearClampSampler, normal, 0.0).rgb * IBLTextureParams.z;
        const float3 reflectionDir = reflect(-viewDir, normal);
        const float prefilteredMip = roughness * max(IBLTextureParams.y - 1.0, 0.0);
        const float3 prefiltered = PrefilteredEnvironmentTexture.SampleLevel(
            IBLLinearClampSampler, reflectionDir, prefilteredMip).rgb * IBLTextureParams.z;
        const float2 brdf = BRDFLUTTexture.SampleLevel(
            IBLLinearClampSampler, float2(nDotV, roughness), 0.0).rg;
        diffuseAmbient = baseColor * (1.0 - fresnel) * (1.0 - metallic) *
            irradiance * ambientOcclusion;
        specularAmbient = prefiltered * (fresnel * brdf.x + brdf.y) * ambientOcclusion;
    }
    else
    {
        const float3 diffuse = IBLDiffuseAmbient.rgb * IBLDiffuseAmbient.a;
        const float3 specular = IBLSpecularAmbient.rgb * IBLSpecularAmbient.a;
        diffuseAmbient = baseColor * (1.0 - fresnel) * (1.0 - metallic) *
            ambientOcclusion * diffuse;
        specularAmbient = f0 * ambientOcclusion * (1.0 - roughness) * specular;
    }
    return diffuseAmbient + specularAmbient +
        baseColor * max(IBLTextureParams.w, 0.0);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    const uint width = uint(max(OutputSize.x, 1.0));
    const uint height = uint(max(OutputSize.y, 1.0));
    if (pixel.x >= width || pixel.y >= height)
    {
        return;
    }

    const float deviceDepth = DeviceDepth.Load(int3(pixel, 0));
    if (IsBackgroundDepth(deviceDepth))
    {
        HDRSceneColor[pixel] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    const uint materialTableIndex = GBuffer3.Load(int3(pixel, 0));
    if (materialTableIndex != 0u)
    {
        HDRSceneColor[pixel] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    const DeferredMaterialClassification classification =
        MaterialClassificationTable[materialTableIndex];
    if (classification.shadingModel != RVX_DEFERRED_MATERIAL_SHADING_MODEL_STANDARD_LIT ||
        classification.schemaVersion != RVX_DEFERRED_MATERIAL_TABLE_SCHEMA_VERSION ||
        classification.reserved0 != 0u || classification.reserved1 != 0u)
    {
        HDRSceneColor[pixel] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    const float4 albedoAO = GBuffer0.Load(int3(pixel, 0));
    const float4 normalRoughnessMetallic = GBuffer1.Load(int3(pixel, 0));
    const float3 emissive = max(GBuffer2.Load(int3(pixel, 0)).rgb, 0.0);
    const float3 normal = DecodeOctahedralNormal(normalRoughnessMetallic.xy);
    const float roughness = clamp(normalRoughnessMetallic.z, 0.04, 1.0);
    const float metallic = saturate(normalRoughnessMetallic.w);
    const float3 worldPos = ReconstructWorldPosition(pixel, deviceDepth);
    const float3 viewDir = SafeNormalize(CameraPosition - worldPos, float3(0.0, 0.0, 1.0));
    const float shadow = SampleDirectionalShadow(worldPos, normal);
    const float3 directional = EvaluatePBR(
        normal, viewDir, SafeNormalize(-LightDirection, float3(0.0, 1.0, 0.0)),
        albedoAO.rgb, metallic, roughness,
        DirectionalLightColor * max(DirectionalLightIntensity, 0.0), shadow);
    const float3 local = EvaluateClusteredLocalLights(
        pixel, normal, viewDir, worldPos, albedoAO.rgb, metallic, roughness);
    const float3 ambient = EvaluateEnvironmentIBL(
        normal, viewDir, albedoAO.rgb, metallic, roughness, saturate(albedoAO.a));
    HDRSceneColor[pixel] = float4(directional + local + ambient + emissive, 1.0);
}
