// =============================================================================
// FroxelVolumetricLighting.hlsl
//
// Concrete four-stage froxel primitive.  The descriptor ABI is intentionally
// closed and matches PipelineCache::CreateFroxelVolumetricPipelineLayout():
// injection -> front-to-back Z integration -> temporal resolve -> HDR
// composite.  All stages use pre-cooked compute programs; no source compiler
// path exists at runtime.
// =============================================================================

// This program is compiled with Volumetric as its source directory by the
// runtime shader loader. Keep shared includes source-relative so DXC,
// glslang, and cooked compilation all resolve the same files without a
// caller-specific shader-root search path.
#include "../Include/BRDF.hlsli"
#include "../Include/Lighting.hlsli"

cbuffer FroxelVolumetricConstants : register(b0)
{
    float4x4 FroxelInverseViewProjection;
    float4 FroxelCameraPosition_NearPlane;
    float4 FroxelCameraForward_FarPlane;
    // xy = full resolution, zw = reciprocal full resolution.
    float4 FroxelOutputSize_InvOutputSize;
    // xy = froxel xy dimensions, z = depth slices, w = tile pixels.
    float4 FroxelGridSize_TilePixels;
    // x = density, y = anisotropy, z = max distance, w = temporal blend.
    float4 FroxelScattering;
    // x = height-fog base metres, y = height falloff per metre,
    // z = frame sequence low bits, w = use committed temporal history.
    float4 FroxelHeightFog_Temporal;
    // x = directional shadow enabled, y = local shadow enabled,
    // z = reverse-Z device depth, w = history reset mask.
    float4 FroxelShadowFlags;
};

// This declaration mirrors Render::LightConstants and is deliberately a
// uniform-buffer view of the frozen lighting plan, never a mutable global.
cbuffer FroxelLightConstants : register(b4)
{
    float4 FroxelMainLightDirection_Intensity;
    uint4 FroxelMainLightColor_ShadowMapIndexBits;
    float4x4 FroxelMainLightSpaceMatrix;
    uint FroxelNumPointLights;
    uint FroxelNumSpotLights;
    float2 FroxelLightPadding;
    float4x4 FroxelLocalShadowViewProjections[32];
    uint FroxelLocalShadowPageCount;
    float FroxelLocalShadowInvResolution;
    float FroxelLocalShadowDepthBias;
    float FroxelLocalShadowNormalBias;
};

cbuffer FroxelClusterConstants : register(b7)
{
    float4 FroxelClusterSize;
    float4 FroxelClusterScreenParams;
    float4x4 FroxelClusterInverseProjection;
};

struct FroxelGPUCluster
{
    uint offset;
    uint count;
    uint pointCount;
    uint spotCount;
};

Texture2D<float> FroxelDeviceDepth : register(t1);
Texture2DArray<float> FroxelDirectionalShadowMap : register(t2);
Texture2DArray<float> FroxelLocalShadowAtlas : register(t3);
StructuredBuffer<PointLight> FroxelPointLights : register(t5);
StructuredBuffer<SpotLight> FroxelSpotLights : register(t6);
StructuredBuffer<FroxelGPUCluster> FroxelClusterData : register(t8);
StructuredBuffer<uint> FroxelClusterLightIndices : register(t9);
Texture3D<float4> FroxelInput : register(t10);
Texture3D<float4> FroxelHistory : register(t11);
[[vk::image_format("rgba16f")]]
RWTexture3D<float4> FroxelOutput : register(u12);
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> FroxelHDRSceneOutput : register(u13);
Texture2D<float4> FroxelHDRSceneInput : register(t14);
SamplerState FroxelLinearClampSampler : register(s15);

static const uint RVX_FROXEL_MAX_LIGHTS_PER_CELL = 100u;
static const float RVX_FROXEL_MIN_DISTANCE = 0.001f;

float3 FroxelSafeNormalize(float3 value, float3 fallback)
{
    const float lengthSquared = dot(value, value);
    return lengthSquared > 1.0e-8f ? value * rsqrt(lengthSquared) : fallback;
}

float FroxelPhaseHenyeyGreenstein(float cosineTheta, float anisotropy)
{
    const float g = clamp(anisotropy, -0.95f, 0.95f);
    const float denominator = max(
        1.0f + g * g - 2.0f * g * cosineTheta,
        1.0e-4f);
    return (1.0f - g * g) /
        max(12.56637061436f * denominator * sqrt(denominator), 1.0e-4f);
}

float FroxelSliceViewDepth(uint slice)
{
    const float nearPlane = max(FroxelCameraPosition_NearPlane.w,
                                RVX_FROXEL_MIN_DISTANCE);
    const float farPlane = max(FroxelCameraForward_FarPlane.w,
                               nearPlane + RVX_FROXEL_MIN_DISTANCE);
    const float normalizedSlice =
        (float(slice) + 0.5f) / max(FroxelGridSize_TilePixels.z, 1.0f);
    return nearPlane * pow(farPlane / nearPlane, saturate(normalizedSlice));
}

float3 FroxelWorldPosition(uint3 coordinate)
{
    const float2 froxelExtent = max(FroxelGridSize_TilePixels.xy, 1.0f.xx);
    const float2 uv = (float2(coordinate.xy) + 0.5f) / froxelExtent;
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 farWorldH = mul(
        FroxelInverseViewProjection,
        float4(ndc, FroxelShadowFlags.z > 0.5f ? 0.0f : 1.0f, 1.0f));
    const float3 farWorld = farWorldH.xyz /
        max(abs(farWorldH.w), RVX_FROXEL_MIN_DISTANCE);
    const float3 rayDirection = FroxelSafeNormalize(
        farWorld - FroxelCameraPosition_NearPlane.xyz,
        FroxelCameraForward_FarPlane.xyz);
    return FroxelCameraPosition_NearPlane.xyz +
        rayDirection * FroxelSliceViewDepth(coordinate.z);
}

float FroxelDirectionalShadow(float3 worldPosition)
{
    if (FroxelShadowFlags.x <= 0.5f ||
        FroxelMainLightColor_ShadowMapIndexBits.w == 0xffffffffu)
    {
        return 1.0f;
    }
    const float4 shadowClip = mul(
        FroxelMainLightSpaceMatrix, float4(worldPosition, 1.0f));
    if (abs(shadowClip.w) <= RVX_FROXEL_MIN_DISTANCE)
    {
        return 1.0f;
    }
    const float3 shadowNdc = shadowClip.xyz / shadowClip.w;
    const float2 shadowUV = float2(
        shadowNdc.x * 0.5f + 0.5f,
        0.5f - shadowNdc.y * 0.5f);
    if (any(shadowUV < 0.0f) || any(shadowUV > 1.0f) ||
        shadowNdc.z < 0.0f || shadowNdc.z > 1.0f)
    {
        return 1.0f;
    }
    const float storedDepth = FroxelDirectionalShadowMap.SampleLevel(
        FroxelLinearClampSampler, float3(shadowUV, 0.0f), 0.0f).r;
    return shadowNdc.z <= storedDepth + 0.0005f ? 1.0f : 0.0f;
}

float FroxelLocalShadow(int shadowPage, float3 worldPosition)
{
    if (FroxelShadowFlags.y <= 0.5f || shadowPage < 0 ||
        uint(shadowPage) >= FroxelLocalShadowPageCount ||
        FroxelLocalShadowInvResolution <= 0.0f)
    {
        return 1.0f;
    }
    const float4 shadowClip = mul(
        FroxelLocalShadowViewProjections[uint(shadowPage)],
        float4(worldPosition, 1.0f));
    if (abs(shadowClip.w) <= RVX_FROXEL_MIN_DISTANCE)
    {
        return 1.0f;
    }
    const float3 shadowNdc = shadowClip.xyz / shadowClip.w;
    const float2 shadowUV = float2(
        shadowNdc.x * 0.5f + 0.5f,
        0.5f - shadowNdc.y * 0.5f);
    if (any(shadowUV < 0.0f) || any(shadowUV > 1.0f) ||
        shadowNdc.z < 0.0f || shadowNdc.z > 1.0f)
    {
        return 1.0f;
    }
    const float storedDepth = FroxelLocalShadowAtlas.SampleLevel(
        FroxelLinearClampSampler, float3(shadowUV, uint(shadowPage)), 0.0f).r;
    return shadowNdc.z - max(FroxelLocalShadowDepthBias, 0.0f) <= storedDepth
        ? 1.0f : 0.0f;
}

float3 FroxelPointContribution(PointLight light, float3 worldPosition)
{
    const float3 lightVector = light.position - worldPosition;
    const float distanceToLight = length(lightVector);
    if (distanceToLight <= RVX_FROXEL_MIN_DISTANCE ||
        distanceToLight >= light.range || light.range <= RVX_FROXEL_MIN_DISTANCE)
    {
        return 0.0f.xxx;
    }
    const float attenuation = AttenuatePoint(distanceToLight, light.range) *
        FroxelLocalShadow(light.shadowMapIndex, worldPosition);
    return light.color * max(light.intensity, 0.0f) * attenuation;
}

float3 FroxelSpotContribution(SpotLight light, float3 worldPosition)
{
    const float3 lightVector = light.position - worldPosition;
    const float distanceToLight = length(lightVector);
    if (distanceToLight <= RVX_FROXEL_MIN_DISTANCE ||
        distanceToLight >= light.range || light.range <= RVX_FROXEL_MIN_DISTANCE)
    {
        return 0.0f.xxx;
    }
    const float3 directionToReceiver = -lightVector / distanceToLight;
    const float cone = AttenuateSpot(
        directionToReceiver, FroxelSafeNormalize(light.direction, float3(0.0f, -1.0f, 0.0f)),
        light.innerConeAngle, light.outerConeAngle);
    const float attenuation = AttenuatePoint(distanceToLight, light.range) * cone *
        FroxelLocalShadow(light.shadowMapIndex, worldPosition);
    return light.color * max(light.intensity, 0.0f) * attenuation;
}

uint FroxelClusterIndex(uint3 coordinate)
{
    const uint clusterXCount = max(uint(round(FroxelClusterSize.x)), 1u);
    const uint clusterYCount = max(uint(round(FroxelClusterSize.y)), 1u);
    const uint clusterZCount = max(uint(round(FroxelClusterSize.z)), 1u);
    const uint x = min(
        uint(float(coordinate.x) / max(FroxelGridSize_TilePixels.x, 1.0f) * clusterXCount),
        clusterXCount - 1u);
    const uint y = min(
        uint(float(coordinate.y) / max(FroxelGridSize_TilePixels.y, 1.0f) * clusterYCount),
        clusterYCount - 1u);
    const uint z = min(
        uint(float(coordinate.z) / max(FroxelGridSize_TilePixels.z, 1.0f) * clusterZCount),
        clusterZCount - 1u);
    const uint index = x + y * clusterXCount + z * clusterXCount * clusterYCount;
    return min(index, max(uint(round(FroxelClusterSize.w)), 1u) - 1u);
}

float3 FroxelEvaluateLocalLighting(uint3 coordinate, float3 worldPosition)
{
    float3 radiance = 0.0f.xxx;
    const FroxelGPUCluster cluster = FroxelClusterData[FroxelClusterIndex(coordinate)];
    const uint count = min(cluster.count, RVX_FROXEL_MAX_LIGHTS_PER_CELL);
    [loop]
    for (uint index = 0u; index < count; ++index)
    {
        const uint packedLight = FroxelClusterLightIndices[cluster.offset + index];
        const uint lightIndex = packedLight & 0xffffu;
        const uint lightType = (packedLight >> 16u) & 0xffffu;
        if (lightType == 0u && lightIndex < FroxelNumPointLights)
        {
            radiance += FroxelPointContribution(
                FroxelPointLights[lightIndex], worldPosition);
        }
        else if (lightType == 1u && lightIndex < FroxelNumSpotLights)
        {
            radiance += FroxelSpotContribution(
                FroxelSpotLights[lightIndex], worldPosition);
        }
    }
    return radiance;
}

[numthreads(4, 4, 4)]
void CSInjectFroxels(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint3 extent = uint3(
        uint(FroxelGridSize_TilePixels.x),
        uint(FroxelGridSize_TilePixels.y),
        uint(FroxelGridSize_TilePixels.z));
    if (any(dispatchThreadId >= extent))
    {
        return;
    }

    const float3 worldPosition = FroxelWorldPosition(dispatchThreadId);
    const float3 viewDirection = FroxelSafeNormalize(
        worldPosition - FroxelCameraPosition_NearPlane.xyz,
        FroxelCameraForward_FarPlane.xyz);
    const float heightDensity = exp(-max(
        worldPosition.y - FroxelHeightFog_Temporal.x, 0.0f) *
        max(FroxelHeightFog_Temporal.y, 0.0f));
    const float density = max(FroxelScattering.x, 0.0f) * heightDensity;
    const float3 directionalDirection = FroxelSafeNormalize(
        -FroxelMainLightDirection_Intensity.xyz, float3(0.0f, 1.0f, 0.0f));
    const float directionalPhase = FroxelPhaseHenyeyGreenstein(
        dot(viewDirection, directionalDirection), FroxelScattering.y);
    const float3 directionalRadiance =
        float3(asfloat(FroxelMainLightColor_ShadowMapIndexBits.xyz)) *
        max(FroxelMainLightDirection_Intensity.w, 0.0f) *
        FroxelDirectionalShadow(worldPosition) * directionalPhase;
    const float3 localRadiance = FroxelEvaluateLocalLighting(
        dispatchThreadId, worldPosition) *
        FroxelPhaseHenyeyGreenstein(0.0f, FroxelScattering.y);
    const float sliceLength = max(
        FroxelSliceViewDepth(min(dispatchThreadId.z + 1u, extent.z - 1u)) -
        FroxelSliceViewDepth(dispatchThreadId.z),
        RVX_FROXEL_MIN_DISTANCE);
    const float transmittance = exp(-density * sliceLength);
    FroxelOutput[dispatchThreadId] = float4(
        (directionalRadiance + localRadiance) * density * sliceLength,
        transmittance);
}

[numthreads(4, 4, 1)]
void CSIntegrateFroxels(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint width = uint(FroxelGridSize_TilePixels.x);
    const uint height = uint(FroxelGridSize_TilePixels.y);
    const uint depth = uint(FroxelGridSize_TilePixels.z);
    if (dispatchThreadId.x >= width || dispatchThreadId.y >= height || depth == 0u)
    {
        return;
    }

    float3 accumulatedScattering = 0.0f.xxx;
    float accumulatedTransmittance = 1.0f;
    [loop]
    for (uint slice = 0u; slice < depth; ++slice)
    {
        const uint3 coordinate = uint3(dispatchThreadId.xy, slice);
        const float4 injected = FroxelInput.Load(int4(coordinate, 0));
        accumulatedScattering += accumulatedTransmittance * max(injected.rgb, 0.0f.xxx);
        accumulatedTransmittance *= saturate(injected.a);
        FroxelOutput[coordinate] = float4(
            accumulatedScattering, accumulatedTransmittance);
    }
}

[numthreads(4, 4, 4)]
void CSTemporalResolveFroxels(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint3 extent = uint3(
        uint(FroxelGridSize_TilePixels.x),
        uint(FroxelGridSize_TilePixels.y),
        uint(FroxelGridSize_TilePixels.z));
    if (any(dispatchThreadId >= extent))
    {
        return;
    }

    const float4 current = FroxelInput.Load(int4(dispatchThreadId, 0));
    const bool useHistory = FroxelHeightFog_Temporal.w > 0.5f &&
        FroxelShadowFlags.w == 0.0f;
    // A reset candidate deliberately has no initialized prior allocation. Do
    // not merely zero its blend weight after an unconditional load: doing so
    // would still read undefined Texture3D data on the first rendered frame.
    const float4 history = useHistory
        ? FroxelHistory.Load(int4(dispatchThreadId, 0)) : current;
    const float historyWeight = useHistory ? saturate(FroxelScattering.w) : 0.0f;
    const float3 scatteringClamp = max(current.rgb * 2.0f + 1.0e-4f, 1.0e-4f.xxx);
    const float3 clampedHistory = clamp(history.rgb, 0.0f.xxx, scatteringClamp);
    FroxelOutput[dispatchThreadId] = float4(
        lerp(current.rgb, clampedHistory, historyWeight),
        lerp(saturate(current.a), saturate(history.a), historyWeight));
}

[numthreads(8, 8, 1)]
void CSCompositeFroxels(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint width = uint(FroxelOutputSize_InvOutputSize.x);
    const uint height = uint(FroxelOutputSize_InvOutputSize.y);
    if (dispatchThreadId.x >= width || dispatchThreadId.y >= height)
    {
        return;
    }

    const float2 uv = (float2(dispatchThreadId.xy) + 0.5f) *
        FroxelOutputSize_InvOutputSize.zw;
    // The volume stores slices in canonical near-to-far depth: 0 is the near
    // slice and 1 is the far slice. This is the same contract consumed by
    // DefaultLit, Water and Particle. Reverse-Z is only the device-depth
    // representation, so invert it before sampling the integrated volume.
    const float deviceDepth = FroxelDeviceDepth.Load(int3(dispatchThreadId.xy, 0));
    const float canonicalDepth = FroxelShadowFlags.z > 0.5f
        ? 1.0f - saturate(deviceDepth)
        : saturate(deviceDepth);
    const float4 volume = FroxelInput.SampleLevel(
        FroxelLinearClampSampler, float3(uv, canonicalDepth), 0.0f);
    const float4 scene = FroxelHDRSceneInput.Load(int3(dispatchThreadId.xy, 0));
    FroxelHDRSceneOutput[dispatchThreadId.xy] = float4(
        max(scene.rgb, 0.0f.xxx) * saturate(volume.a) + max(volume.rgb, 0.0f.xxx),
        scene.a);
}
