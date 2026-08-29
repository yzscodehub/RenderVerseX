/**
 * @file DeferredLighting.hlsl
 * @brief Compute lighting for the canonical deferred opaque GBuffer.
 *
 * The set-0 ABI intentionally does not borrow PipelineCache's mutable frame
 * descriptor. DeferredLightingPass seals a record-owned lighting snapshot, then
 * binds the same local lights, CSM and texture IBL semantics as DefaultLit.
 */

#include "Include/BRDF.hlsli"
#include "Include/AmbientOcclusion.hlsli"
#include "Include/Exposure.hlsli"
#include "Include/Lighting.hlsli"
#include "Include/SurfaceMaterial.hlsli"
#include "Include/ToonLighting.hlsli"

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
    float4 DirectionalLightColor_ReverseZ;
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
    float4x4 LocalShadowViewProjections[32];
    uint LocalShadowPageCount;
    float LocalShadowInvResolution;
    float LocalShadowDepthBias;
    float LocalShadowNormalBias;
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
#define DirectionalLightColor DirectionalLightColor_ReverseZ.xyz
#define OutputSize OutputSize_InvSize.xy
#define DepthBackgroundValue DepthBackground_ReverseZ.x
#define DepthBackgroundEpsilon DepthBackground_ReverseZ.y
#define ReverseZ DepthBackground_ReverseZ.z

Texture2D<float4> GBuffer0 : register(t1, space0);
Texture2D<float4> GBuffer1 : register(t2, space0);
Texture2D<float4> GBuffer2 : register(t3, space0);
Texture2D<uint> GBuffer3 : register(t4, space0);
Texture2D<float> DeviceDepth : register(t5, space0);
// Vulkan storage images require the SPIR-V image format to match the bound
// RGBA16F scene-color view exactly. DXIL ignores this Vulkan-only annotation.
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> HDRSceneColor : register(u6, space0);
// The two auxiliary UAVs form an all-or-none reflection-replacement bundle.
// The baseline is pre-exposed scene-linear radiance; the response is
// deliberately dimensionless so a replacement source is materialized once.
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> BaseIndirectSpecular : register(u24, space0);
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> SpecularResponse : register(u25, space0);

struct GPUCluster
{
    uint offset;
    uint count;
    uint pointCount;
    uint spotCount;
};

// MaterialSystem owns this descriptor-only snapshot. Each row is indexed by
// the stable RenderResourceHandle::slot written by PSGBuffer, and verifies its
// own table index before any lighting is evaluated.
StructuredBuffer<MaterialParameterData> MaterialClassificationTable : register(t7, space0);
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
// Canonical committed pre-exposure state. Deferred geometry deliberately
// leaves GBuffer data in physical scene-linear units; only this HDR producer
// applies the scale.
StructuredBuffer<RVXExposureState> ExposureStateBuffer : register(t21, space0);
// Exact R8 visibility field published by the AmbientOcclusion feature.
Texture2D<float> AmbientOcclusionTexture : register(t22, space0);
// Full-capacity Texture2DArray paired with the existing point-clamp sampler
// at s9. Depth comparison is explicit in LocalShadow.hlsli.
Texture2DArray<float> LocalShadowAtlasTexture : register(t23, space0);
#define LocalShadowAtlasSampler DirectionalShadowSampler

static const uint RVX_INVALID_CLUSTER_INDEX = 0xffffffffu;
static const uint RVX_MAX_CLUSTERED_LIGHTS_PER_PIXEL = 100u;

float3 SafeNormalize(float3 value, float3 fallback)
{
    const float lengthSquared = dot(value, value);
    return lengthSquared > 1.0e-8 ? value * rsqrt(lengthSquared) : fallback;
}

#include "Include/DirectionalShadow.hlsli"
#include "Include/LocalShadow.hlsli"

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
    float metallic, float roughness, bool receivesShadow)
{
    float3 result = 0.0;
    [loop]
    for (uint index = 0; index < min(NumPointLights, 256u); ++index)
    {
        result += EvaluatePointLight(
            PointLights[index], normal, viewDir, worldPos, baseColor, metallic, roughness,
            receivesShadow
                ? RVX_SamplePointLocalShadow(PointLights[index], worldPos, normal)
                : 1.0);
    }
    [loop]
    for (uint index = 0; index < min(NumSpotLights, 128u); ++index)
    {
        result += EvaluateSpotLight(
            SpotLights[index], normal, viewDir, worldPos, baseColor, metallic, roughness,
            receivesShadow
                ? RVX_SampleSpotLocalShadow(SpotLights[index], worldPos, normal)
                : 1.0);
    }
    return result;
}

float3 EvaluateClusteredLocalLights(
    uint2 pixel, float3 normal, float3 viewDir, float3 worldPos,
    float3 baseColor, float metallic, float roughness, bool receivesShadow)
{
    const uint clusterIndex = ResolveClusterIndex(pixel, worldPos);
    if (clusterIndex == RVX_INVALID_CLUSTER_INDEX)
    {
        return EvaluateLinearLocalLights(
            normal, viewDir, worldPos, baseColor, metallic, roughness,
            receivesShadow);
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
                baseColor, metallic, roughness,
                receivesShadow
                    ? RVX_SamplePointLocalShadow(
                        PointLights[lightIndex], worldPos, normal)
                    : 1.0);
        }
        else if (lightType == 1u && lightIndex < NumSpotLights)
        {
            result += EvaluateSpotLight(
                SpotLights[lightIndex], normal, viewDir, worldPos,
                baseColor, metallic, roughness,
                receivesShadow
                    ? RVX_SampleSpotLocalShadow(
                        SpotLights[lightIndex], worldPos, normal)
                    : 1.0);
        }
    }
    return result;
}

float3 EvaluateToonPointLight(
    PointLight light,
    float3 normal,
    float3 viewDir,
    float3 worldPos,
    float3 baseColor,
    MaterialParameterData material,
    bool receivesShadow)
{
    const float3 lightVector = light.position - worldPos;
    const float distanceToLight = length(lightVector);
    if (distanceToLight > light.range || distanceToLight <= 0.0f ||
        light.range <= 1.0e-4f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }
    return RVX_EvaluateToonDirectLight(
        normal,
        viewDir,
        lightVector / distanceToLight,
        baseColor,
        light.color * light.intensity * AttenuatePoint(distanceToLight, light.range),
        receivesShadow
            ? RVX_SamplePointLocalShadow(light, worldPos, normal)
            : 1.0f,
        material.ToonBands,
        material.ToonSpecularThreshold,
        material.ToonRimStrength,
        material.ToonShadowTint.xyz);
}

float3 EvaluateToonSpotLight(
    SpotLight light,
    float3 normal,
    float3 viewDir,
    float3 worldPos,
    float3 baseColor,
    MaterialParameterData material,
    bool receivesShadow)
{
    const float3 lightVector = light.position - worldPos;
    const float distanceToLight = length(lightVector);
    if (distanceToLight > light.range || distanceToLight <= 0.0f ||
        light.range <= 1.0e-4f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }
    const float3 lightDirection = lightVector / distanceToLight;
    const float cone = AttenuateSpot(
        -lightDirection,
        normalize(light.direction),
        light.innerConeAngle,
        light.outerConeAngle);
    if (cone <= 0.0f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }
    return RVX_EvaluateToonDirectLight(
        normal,
        viewDir,
        lightDirection,
        baseColor,
        light.color * light.intensity *
            AttenuatePoint(distanceToLight, light.range) * cone,
        receivesShadow
            ? RVX_SampleSpotLocalShadow(light, worldPos, normal)
            : 1.0f,
        material.ToonBands,
        material.ToonSpecularThreshold,
        material.ToonRimStrength,
        material.ToonShadowTint.xyz);
}

float3 EvaluateToonLinearLocalLights(
    float3 normal,
    float3 viewDir,
    float3 worldPos,
    float3 baseColor,
    MaterialParameterData material,
    bool receivesShadow)
{
    float3 result = 0.0f;
    [loop]
    for (uint index = 0; index < min(NumPointLights, 256u); ++index)
    {
        result += EvaluateToonPointLight(
            PointLights[index], normal, viewDir, worldPos, baseColor, material,
            receivesShadow);
    }
    [loop]
    for (uint index = 0; index < min(NumSpotLights, 128u); ++index)
    {
        result += EvaluateToonSpotLight(
            SpotLights[index], normal, viewDir, worldPos, baseColor, material,
            receivesShadow);
    }
    return result;
}

float3 EvaluateToonClusteredLocalLights(
    uint2 pixel,
    float3 normal,
    float3 viewDir,
    float3 worldPos,
    float3 baseColor,
    MaterialParameterData material,
    bool receivesShadow)
{
    const uint clusterIndex = ResolveClusterIndex(pixel, worldPos);
    if (clusterIndex == RVX_INVALID_CLUSTER_INDEX)
    {
        return EvaluateToonLinearLocalLights(
            normal, viewDir, worldPos, baseColor, material, receivesShadow);
    }

    const GPUCluster cluster = ClusterData[clusterIndex];
    const uint lightCount = min(cluster.count, RVX_MAX_CLUSTERED_LIGHTS_PER_PIXEL);
    float3 result = 0.0f;
    [loop]
    for (uint index = 0; index < lightCount; ++index)
    {
        const uint packedIndex = ClusterLightIndices[cluster.offset + index];
        const uint lightIndex = packedIndex & 0xffffu;
        const uint lightType = (packedIndex >> 16u) & 0xffffu;
        if (lightType == 0u && lightIndex < NumPointLights)
        {
            result += EvaluateToonPointLight(
                PointLights[lightIndex], normal, viewDir, worldPos, baseColor,
                material, receivesShadow);
        }
        else if (lightType == 1u && lightIndex < NumSpotLights)
        {
            result += EvaluateToonSpotLight(
                SpotLights[lightIndex], normal, viewDir, worldPos, baseColor,
                material, receivesShadow);
        }
    }
    return result;
}

struct EnvironmentIBLResult
{
    float3 total;
    float3 specular;
    float3 specularResponse;
};

EnvironmentIBLResult EvaluateEnvironmentIBL(
    float3 normal, float3 viewDir, float3 baseColor, float metallic,
    float roughness, float materialOcclusion, float screenAmbientOcclusion)
{
    EnvironmentIBLResult result;
    result.total = 0.0f;
    result.specular = 0.0f;
    result.specularResponse = 0.0f;
    const float3 f0 = ComputeF0(baseColor, metallic);
    const float nDotV = max(dot(normal, viewDir), 0.001);
    const float3 fresnel = F_SchlickRoughness(nDotV, f0, roughness);
    float3 diffuseAmbient;
    float3 specularSource;
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
            irradiance * materialOcclusion;
        specularSource = prefiltered;
        result.specularResponse =
            (fresnel * brdf.x + brdf.y) * materialOcclusion;
    }
    else
    {
        const float3 diffuse = IBLDiffuseAmbient.rgb * IBLDiffuseAmbient.a;
        const float3 specular = IBLSpecularAmbient.rgb * IBLSpecularAmbient.a;
        diffuseAmbient = baseColor * (1.0 - fresnel) * (1.0 - metallic) *
            materialOcclusion * diffuse;
        specularSource = specular;
        result.specularResponse = f0 * materialOcclusion *
            (1.0 - roughness);
    }
    diffuseAmbient = RVX_ApplyIndirectDiffuseAmbientOcclusion(
        diffuseAmbient, screenAmbientOcclusion);
    result.specularResponse = RVX_ApplyIndirectSpecularAmbientOcclusion(
        result.specularResponse, screenAmbientOcclusion, roughness, nDotV);
    result.specular = specularSource * result.specularResponse;
    const float3 ambientFloor = RVX_ApplyIndirectDiffuseAmbientOcclusion(
        baseColor * max(IBLTextureParams.w, 0.0), screenAmbientOcclusion);
    result.total = diffuseAmbient + result.specular + ambientFloor;
    return result;
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

    const RVXExposureState exposureState = ExposureStateBuffer[0];
    if (!RVXExposureStateIsValid(exposureState))
    {
        // Do not approximate corrupt state with a unit scale. The CPU graph
        // contract makes the pass fail before execution for a missing handle;
        // zero is the defensive shader-side failure result.
        HDRSceneColor[pixel] = float4(0.0, 0.0, 0.0, 1.0);
        BaseIndirectSpecular[pixel] = 0.0f;
        SpecularResponse[pixel] = 0.0f;
        return;
    }
    const float preExposure = RVXExposureProducerScale(exposureState);

    const float deviceDepth = DeviceDepth.Load(int3(pixel, 0));
    if (IsBackgroundDepth(deviceDepth))
    {
        HDRSceneColor[pixel] = float4(0.0, 0.0, 0.0, 1.0);
        BaseIndirectSpecular[pixel] = 0.0f;
        SpecularResponse[pixel] = 0.0f;
        return;
    }

    const uint materialTableIndex = GBuffer3.Load(int3(pixel, 0));
    uint materialTableCount = 0u;
    uint materialTableStride = 0u;
    MaterialClassificationTable.GetDimensions(
        materialTableCount, materialTableStride);
    if (materialTableIndex >= materialTableCount)
    {
        HDRSceneColor[pixel] = float4(0.0, 0.0, 0.0, 1.0);
        BaseIndirectSpecular[pixel] = 0.0f;
        SpecularResponse[pixel] = 0.0f;
        return;
    }
    const MaterialParameterData material =
        MaterialClassificationTable[materialTableIndex];
    if (material.MaterialTableIndex != materialTableIndex ||
        (material.ShadingModel != MATERIAL_SHADING_MODEL_STANDARD_LIT &&
         material.ShadingModel != MATERIAL_SHADING_MODEL_TOON_LIT) ||
        material.Workflow != MATERIAL_WORKFLOW_METALLIC_ROUGHNESS)
    {
        HDRSceneColor[pixel] = float4(0.0, 0.0, 0.0, 1.0);
        BaseIndirectSpecular[pixel] = 0.0f;
        SpecularResponse[pixel] = 0.0f;
        return;
    }

    const float4 albedoAO = GBuffer0.Load(int3(pixel, 0));
    const float4 normalRoughnessMetallic = GBuffer1.Load(int3(pixel, 0));
    const float4 emissiveReceiver = GBuffer2.Load(int3(pixel, 0));
    const float3 emissive = max(emissiveReceiver.rgb, 0.0);
    const bool receivesShadow = emissiveReceiver.a > 0.5f;
    const float3 normal = DecodeOctahedralNormal(normalRoughnessMetallic.xy);
    const float roughness = clamp(normalRoughnessMetallic.z, 0.04, 1.0);
    const float metallic = saturate(normalRoughnessMetallic.w);
    const float3 worldPos = ReconstructWorldPosition(pixel, deviceDepth);
    const float3 viewDir = SafeNormalize(CameraPosition - worldPos, float3(0.0, 0.0, 1.0));
    const float shadow = receivesShadow
        ? SampleDirectionalShadow(worldPos, normal)
        : 1.0f;
    float3 directional;
    float3 local;
    if (material.ShadingModel == MATERIAL_SHADING_MODEL_TOON_LIT)
    {
        directional = RVX_EvaluateToonDirectLight(
            normal,
            viewDir,
            SafeNormalize(-LightDirection, float3(0.0, 1.0, 0.0)),
            albedoAO.rgb,
            DirectionalLightColor * max(DirectionalLightIntensity, 0.0),
            shadow,
            material.ToonBands,
            material.ToonSpecularThreshold,
            material.ToonRimStrength,
            material.ToonShadowTint.xyz);
        local = EvaluateToonClusteredLocalLights(
            pixel, normal, viewDir, worldPos, albedoAO.rgb, material,
            receivesShadow);
    }
    else
    {
        directional = EvaluatePBR(
            normal, viewDir, SafeNormalize(-LightDirection, float3(0.0, 1.0, 0.0)),
            albedoAO.rgb, metallic, roughness,
            DirectionalLightColor * max(DirectionalLightIntensity, 0.0), shadow);
        local = EvaluateClusteredLocalLights(
            pixel, normal, viewDir, worldPos, albedoAO.rgb, metallic, roughness,
            receivesShadow);
    }
    const float screenAmbientOcclusion = RVX_SaturateAmbientOcclusion(
        AmbientOcclusionTexture.Load(int3(pixel, 0)));
    const EnvironmentIBLResult ambient = EvaluateEnvironmentIBL(
        normal, viewDir, albedoAO.rgb, metallic, roughness,
        saturate(albedoAO.a), screenAmbientOcclusion);
    HDRSceneColor[pixel] = float4(
        (directional + local + ambient.total + emissive) * preExposure, 1.0);
    BaseIndirectSpecular[pixel] = float4(
        ambient.specular * preExposure, 1.0f);
    SpecularResponse[pixel] = float4(ambient.specularResponse, 1.0f);
}
