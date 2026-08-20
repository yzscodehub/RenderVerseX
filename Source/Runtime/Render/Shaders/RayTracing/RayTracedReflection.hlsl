/**
 * @file RayTracedReflection.hlsl
 * @brief Minimal DXR reflection path using shared ray-tracing material metadata
 */

#include "RayTracingResourceBindings.hlsli"
#include "RayTracingSceneMetadata.hlsli"

RaytracingAccelerationStructure gScene : register(RVX_RT_REFLECTION_TLAS_REGISTER, space0);
RWTexture2D<float4> gReflectionOutput : register(RVX_RT_REFLECTION_OUTPUT_REGISTER, space0);
Texture2D<float4> gSceneColor : register(RVX_RT_REFLECTION_SCENE_COLOR_REGISTER, space0);
Texture2D<float> gSceneDepth : register(RVX_RT_REFLECTION_SCENE_DEPTH_REGISTER, space0);

static const uint RT_MAX_MATERIAL_TEXTURES = RVX_RT_REFLECTION_MAX_MATERIAL_TEXTURES;
static const uint RT_MAX_GEOMETRY_BUFFERS = RVX_RT_REFLECTION_MAX_GEOMETRY_BUFFERS;

StructuredBuffer<RayTracingInstanceMaterialMetadata> gInstanceMaterialMetadata : register(RVX_RT_REFLECTION_MATERIAL_METADATA_REGISTER, space0);
Texture2D<float4> gMaterialTextures[RT_MAX_MATERIAL_TEXTURES] : register(RVX_RT_REFLECTION_MATERIAL_TEXTURES_REGISTER, space0);
Texture2D<float4> gPreviousReflectionHistory : register(RVX_RT_REFLECTION_PREVIOUS_HISTORY_REGISTER, space0);
Texture2D<float> gPreviousDepthHistory : register(RVX_RT_REFLECTION_PREVIOUS_DEPTH_REGISTER, space0);
RWTexture2D<float> gCurrentDepthHistory : register(RVX_RT_REFLECTION_OUTPUT_DEPTH_REGISTER, space0);
Texture2D<float4> gPreviousNormalHistory : register(RVX_RT_REFLECTION_PREVIOUS_NORMAL_REGISTER, space0);
RWTexture2D<float4> gCurrentNormalHistory : register(RVX_RT_REFLECTION_OUTPUT_NORMAL_REGISTER, space0);
StructuredBuffer<RayTracingInstanceAlphaMetadata> gInstanceGeometryMetadata : register(RVX_RT_REFLECTION_GEOMETRY_METADATA_REGISTER, space0);
ByteAddressBuffer gReflectionIndexBuffers[RT_MAX_GEOMETRY_BUFFERS] : register(RVX_RT_REFLECTION_INDEX_BUFFERS_REGISTER, space0);
ByteAddressBuffer gReflectionUVBuffers[RT_MAX_GEOMETRY_BUFFERS] : register(RVX_RT_REFLECTION_UV_BUFFERS_REGISTER, space0);
ByteAddressBuffer gReflectionNormalBuffers[RT_MAX_GEOMETRY_BUFFERS] : register(RVX_RT_REFLECTION_NORMAL_BUFFERS_REGISTER, space0);
ByteAddressBuffer gReflectionTangentBuffers[RT_MAX_GEOMETRY_BUFFERS] : register(RVX_RT_REFLECTION_TANGENT_BUFFERS_REGISTER, space0);
Texture2D<float2> gSceneVelocity : register(RVX_RT_REFLECTION_SCENE_VELOCITY_REGISTER, space0);

cbuffer RayTracedReflectionConstants : register(RVX_RT_REFLECTION_CONSTANTS_REGISTER, space0)
{
    float4x4 gInverseViewProjection;
    float4x4 gPreviousViewProjection;
    float4 gCameraPositionAndTMax; // xyz: camera position, w: max reflection trace distance
    float4 gOutputSizeAndInvSize; // xy: reflection output size, zw: reciprocal output size
    float4 gSceneSizeAndInvSize; // xy: scene size, zw: reciprocal scene size
    float4 gReflectionOptions; // x: intensity, y: max roughness, z: TraceRay instance mask, w: distance fade start
    float4 gHistoryParams; // x: history valid, y: history weight, z: depth threshold, w: min normal dot
    float4 gStochasticParams; // x: samples per pixel, y: frame seed, z: roughness cone spread
    float4 gRayBiasParams; // x: normal bias, y: ray min T, z: firefly clamp luminance
    float4 gHistoryClampParams; // x: luminance tolerance, y: min current confidence, z: velocity available, w: velocity rejection scale
};

static const uint RVX_RT_REFLECTION_MAX_SAMPLES_PER_PIXEL = 4u;
static const float RVX_REFLECTION_PI = 3.14159265f;

struct ReflectionPayload
{
    float3 color;
    uint hit;
    float hitT;
};

bool IsBackgroundDepth(float depth)
{
    return depth >= 0.999999f;
}

uint2 MapOutputPixelToScenePixel(uint2 outputPixel)
{
    const uint2 sceneSize = max(uint2(gSceneSizeAndInvSize.xy), uint2(1u, 1u));
    const float2 uv = (float2(outputPixel) + 0.5f) * gOutputSizeAndInvSize.zw;
    return min(uint2(uv * gSceneSizeAndInvSize.xy), sceneSize - 1u);
}

float3 ReconstructWorldPosition(uint2 scenePixel, float depth)
{
    const float2 uv = (float2(scenePixel) + 0.5f) * gSceneSizeAndInvSize.zw;
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 world = mul(gInverseViewProjection, float4(ndc, depth, 1.0f));
    world.xyz /= abs(world.w) > 1.0e-6f ? world.w : 1.0f;
    return world.xyz;
}

bool TryLoadWorldPosition(uint2 scenePixel, out float3 worldPosition)
{
    const float depth = gSceneDepth.Load(int3(scenePixel, 0)).r;
    if (IsBackgroundDepth(depth))
    {
        worldPosition = 0.0f;
        return false;
    }

    worldPosition = ReconstructWorldPosition(scenePixel, depth);
    return true;
}

bool TryReconstructWorldNormal(uint2 scenePixel, float3 centerWorld, out float3 normal)
{
    const uint2 sceneSize = max(uint2(gSceneSizeAndInvSize.xy), uint2(1u, 1u));
    const uint leftX = scenePixel.x > 0u ? scenePixel.x - 1u : 0u;
    const uint rightX = min(scenePixel.x + 1u, sceneSize.x - 1u);
    const uint upY = scenePixel.y > 0u ? scenePixel.y - 1u : 0u;
    const uint downY = min(scenePixel.y + 1u, sceneSize.y - 1u);
    const uint2 leftPixel = uint2(leftX, scenePixel.y);
    const uint2 rightPixel = uint2(rightX, scenePixel.y);
    const uint2 upPixel = uint2(scenePixel.x, upY);
    const uint2 downPixel = uint2(scenePixel.x, downY);

    float3 leftWorld;
    float3 rightWorld;
    float3 upWorld;
    float3 downWorld;
    const bool hasLeft = TryLoadWorldPosition(leftPixel, leftWorld);
    const bool hasRight = TryLoadWorldPosition(rightPixel, rightWorld);
    const bool hasUp = TryLoadWorldPosition(upPixel, upWorld);
    const bool hasDown = TryLoadWorldPosition(downPixel, downWorld);

    float3 dx = 0.0f;
    float3 dy = 0.0f;
    if (hasLeft && hasRight)
    {
        dx = rightWorld - leftWorld;
    }
    else if (hasRight)
    {
        dx = rightWorld - centerWorld;
    }
    else if (hasLeft)
    {
        dx = centerWorld - leftWorld;
    }

    if (hasUp && hasDown)
    {
        dy = downWorld - upWorld;
    }
    else if (hasDown)
    {
        dy = downWorld - centerWorld;
    }
    else if (hasUp)
    {
        dy = centerWorld - upWorld;
    }

    normal = normalize(cross(dy, dx));
    return all(isfinite(normal)) && dot(normal, normal) > 0.5f;
}

float3 ResolveReflectionFallback(uint2 scenePixel)
{
    return gSceneColor.Load(int3(scenePixel, 0)).rgb;
}

bool HasMaterialTexture(uint flags, uint textureFlag, uint textureTableIndex)
{
    return (flags & textureFlag) != 0u &&
           textureTableIndex < RT_MAX_MATERIAL_TEXTURES &&
           textureTableIndex != RT_INVALID_INDEX;
}

float4 LoadMaterialTextureRepresentative(uint textureTableIndex)
{
    return gMaterialTextures[NonUniformResourceIndex(textureTableIndex)].Load(int3(0, 0, 0));
}

uint LoadGeometryIndex(const RayTracingInstanceAlphaMetadata geometry, uint elementIndex)
{
    if ((geometry.Flags & RT_ALPHA_INDEX_FORMAT_UINT32) != 0u)
    {
        return gReflectionIndexBuffers[NonUniformResourceIndex(geometry.IndexBufferTableIndex)].Load(elementIndex * 4u);
    }

    if ((geometry.Flags & RT_ALPHA_INDEX_FORMAT_UINT16) != 0u)
    {
        const uint byteOffset = elementIndex * 2u;
        const uint packed = gReflectionIndexBuffers[NonUniformResourceIndex(geometry.IndexBufferTableIndex)].Load(byteOffset & ~3u);
        return (byteOffset & 2u) != 0u ? ((packed >> 16u) & 0xFFFFu) : (packed & 0xFFFFu);
    }

    return 0u;
}

bool CanLoadHitUV(const RayTracingInstanceAlphaMetadata geometry)
{
    return (geometry.Flags & RT_ALPHA_HAS_INDEX_BUFFER) != 0u &&
           (geometry.Flags & RT_ALPHA_HAS_UV_BUFFER) != 0u &&
           ((geometry.Flags & RT_ALPHA_INDEX_FORMAT_UINT32) != 0u ||
            (geometry.Flags & RT_ALPHA_INDEX_FORMAT_UINT16) != 0u) &&
           geometry.IndexBufferTableIndex < RT_MAX_GEOMETRY_BUFFERS &&
           geometry.UVBufferTableIndex < RT_MAX_GEOMETRY_BUFFERS &&
           geometry.IndexBufferTableIndex != RT_INVALID_INDEX &&
           geometry.UVBufferTableIndex != RT_INVALID_INDEX;
}

bool CanLoadHitNormal(const RayTracingInstanceAlphaMetadata geometry)
{
    return (geometry.Flags & RT_ALPHA_HAS_INDEX_BUFFER) != 0u &&
           (geometry.Flags & RT_ALPHA_HAS_NORMAL_BUFFER) != 0u &&
           ((geometry.Flags & RT_ALPHA_INDEX_FORMAT_UINT32) != 0u ||
            (geometry.Flags & RT_ALPHA_INDEX_FORMAT_UINT16) != 0u) &&
           geometry.IndexBufferTableIndex < RT_MAX_GEOMETRY_BUFFERS &&
           geometry.NormalBufferTableIndex < RT_MAX_GEOMETRY_BUFFERS &&
           geometry.IndexBufferTableIndex != RT_INVALID_INDEX &&
           geometry.NormalBufferTableIndex != RT_INVALID_INDEX;
}

bool CanLoadHitTangent(const RayTracingInstanceAlphaMetadata geometry)
{
    return (geometry.Flags & RT_ALPHA_HAS_INDEX_BUFFER) != 0u &&
           (geometry.Flags & RT_ALPHA_HAS_TANGENT_BUFFER) != 0u &&
           ((geometry.Flags & RT_ALPHA_INDEX_FORMAT_UINT32) != 0u ||
            (geometry.Flags & RT_ALPHA_INDEX_FORMAT_UINT16) != 0u) &&
           geometry.IndexBufferTableIndex < RT_MAX_GEOMETRY_BUFFERS &&
           geometry.TangentBufferTableIndex < RT_MAX_GEOMETRY_BUFFERS &&
           geometry.IndexBufferTableIndex != RT_INVALID_INDEX &&
           geometry.TangentBufferTableIndex != RT_INVALID_INDEX;
}

bool TryLoadHitUV(const RayTracingInstanceAlphaMetadata geometry,
                  BuiltInTriangleIntersectionAttributes attributes,
                  out float2 uv)
{
    uv = 0.0f;
    if (!CanLoadHitUV(geometry))
    {
        return false;
    }

    const uint firstIndexElement = geometry.IndexElementOffset + PrimitiveIndex() * 3u;
    const uint i0 = LoadGeometryIndex(geometry, firstIndexElement) + geometry.BaseVertex;
    const uint i1 = LoadGeometryIndex(geometry, firstIndexElement + 1u) + geometry.BaseVertex;
    const uint i2 = LoadGeometryIndex(geometry, firstIndexElement + 2u) + geometry.BaseVertex;

    const uint2 uv0Bits = gReflectionUVBuffers[NonUniformResourceIndex(geometry.UVBufferTableIndex)].Load2(i0 * 8u);
    const uint2 uv1Bits = gReflectionUVBuffers[NonUniformResourceIndex(geometry.UVBufferTableIndex)].Load2(i1 * 8u);
    const uint2 uv2Bits = gReflectionUVBuffers[NonUniformResourceIndex(geometry.UVBufferTableIndex)].Load2(i2 * 8u);
    const float2 uv0 = asfloat(uv0Bits);
    const float2 uv1 = asfloat(uv1Bits);
    const float2 uv2 = asfloat(uv2Bits);

    const float3 barycentrics = float3(
        1.0f - attributes.barycentrics.x - attributes.barycentrics.y,
        attributes.barycentrics.x,
        attributes.barycentrics.y);
    uv = uv0 * barycentrics.x + uv1 * barycentrics.y + uv2 * barycentrics.z;
    return all(isfinite(uv));
}

bool TryLoadHitNormal(const RayTracingInstanceAlphaMetadata geometry,
                      BuiltInTriangleIntersectionAttributes attributes,
                      out float3 normal)
{
    normal = 0.0f;
    if (!CanLoadHitNormal(geometry))
    {
        return false;
    }

    const uint firstIndexElement = geometry.IndexElementOffset + PrimitiveIndex() * 3u;
    const uint i0 = LoadGeometryIndex(geometry, firstIndexElement) + geometry.BaseVertex;
    const uint i1 = LoadGeometryIndex(geometry, firstIndexElement + 1u) + geometry.BaseVertex;
    const uint i2 = LoadGeometryIndex(geometry, firstIndexElement + 2u) + geometry.BaseVertex;

    const uint3 n0Bits = gReflectionNormalBuffers[NonUniformResourceIndex(geometry.NormalBufferTableIndex)].Load3(i0 * 12u);
    const uint3 n1Bits = gReflectionNormalBuffers[NonUniformResourceIndex(geometry.NormalBufferTableIndex)].Load3(i1 * 12u);
    const uint3 n2Bits = gReflectionNormalBuffers[NonUniformResourceIndex(geometry.NormalBufferTableIndex)].Load3(i2 * 12u);
    const float3 n0 = asfloat(n0Bits);
    const float3 n1 = asfloat(n1Bits);
    const float3 n2 = asfloat(n2Bits);

    const float3 barycentrics = float3(
        1.0f - attributes.barycentrics.x - attributes.barycentrics.y,
        attributes.barycentrics.x,
        attributes.barycentrics.y);
    normal = normalize(n0 * barycentrics.x + n1 * barycentrics.y + n2 * barycentrics.z);
    return all(isfinite(normal)) && dot(normal, normal) > 0.5f;
}

bool TryLoadHitTangent(const RayTracingInstanceAlphaMetadata geometry,
                       BuiltInTriangleIntersectionAttributes attributes,
                       out float4 tangent)
{
    tangent = float4(0.0f, 0.0f, 0.0f, 1.0f);
    if (!CanLoadHitTangent(geometry))
    {
        return false;
    }

    const uint firstIndexElement = geometry.IndexElementOffset + PrimitiveIndex() * 3u;
    const uint i0 = LoadGeometryIndex(geometry, firstIndexElement) + geometry.BaseVertex;
    const uint i1 = LoadGeometryIndex(geometry, firstIndexElement + 1u) + geometry.BaseVertex;
    const uint i2 = LoadGeometryIndex(geometry, firstIndexElement + 2u) + geometry.BaseVertex;

    const uint4 t0Bits = gReflectionTangentBuffers[NonUniformResourceIndex(geometry.TangentBufferTableIndex)].Load4(i0 * 16u);
    const uint4 t1Bits = gReflectionTangentBuffers[NonUniformResourceIndex(geometry.TangentBufferTableIndex)].Load4(i1 * 16u);
    const uint4 t2Bits = gReflectionTangentBuffers[NonUniformResourceIndex(geometry.TangentBufferTableIndex)].Load4(i2 * 16u);
    const float4 t0 = asfloat(t0Bits);
    const float4 t1 = asfloat(t1Bits);
    const float4 t2 = asfloat(t2Bits);

    const float3 barycentrics = float3(
        1.0f - attributes.barycentrics.x - attributes.barycentrics.y,
        attributes.barycentrics.x,
        attributes.barycentrics.y);
    tangent = t0 * barycentrics.x + t1 * barycentrics.y + t2 * barycentrics.z;
    tangent.xyz = normalize(tangent.xyz);
    tangent.w = tangent.w < 0.0f ? -1.0f : 1.0f;
    return all(isfinite(tangent)) && dot(tangent.xyz, tangent.xyz) > 0.5f;
}

float2 TransformMaterialUV(const RayTracingMaterialTextureSamplingMetadata sampling, float2 uv)
{
    float sine = 0.0f;
    float cosine = 1.0f;
    sincos(sampling.UVRotation, sine, cosine);

    uv *= sampling.UVScale;
    uv = float2(
        uv.x * cosine - uv.y * sine,
        uv.x * sine + uv.y * cosine);
    return uv + sampling.UVOffset;
}

float WrapMaterialCoordinate(float value, bool clampToEdge, bool mirrorRepeat)
{
    if (clampToEdge)
    {
        return saturate(value);
    }

    if (mirrorRepeat)
    {
        return 1.0f - abs(frac(value * 0.5f) * 2.0f - 1.0f);
    }

    return frac(value);
}

int WrapMaterialTexel(int texel, uint extent, bool clampToEdge, bool mirrorRepeat)
{
    const int lastTexel = (int)extent - 1;
    if (clampToEdge)
    {
        return min(max(texel, 0), lastTexel);
    }

    const int signedExtent = (int)extent;
    const int wrapExtent = mirrorRepeat ? signedExtent * 2 : signedExtent;
    int wrapped = texel % wrapExtent;
    if (wrapped < 0)
    {
        wrapped += wrapExtent;
    }

    if (mirrorRepeat && wrapped >= signedExtent)
    {
        wrapped = wrapExtent - wrapped - 1;
    }

    return wrapped;
}

float4 LoadMaterialTextureTexel(uint textureTableIndex, int2 texel)
{
    return gMaterialTextures[NonUniformResourceIndex(textureTableIndex)].Load(int3(texel, 0));
}

float4 SampleMaterialTextureAtUV(uint textureTableIndex,
                                 const RayTracingMaterialTextureSamplingMetadata sampling,
                                 float2 uv)
{
    uint textureWidth = 0u;
    uint textureHeight = 0u;
    gMaterialTextures[NonUniformResourceIndex(textureTableIndex)].GetDimensions(textureWidth, textureHeight);
    if (textureWidth == 0u || textureHeight == 0u)
    {
        return 1.0f;
    }

    const bool clampS = (sampling.SamplerFlags & RT_ALPHA_WRAP_S_CLAMP) != 0u;
    const bool clampT = (sampling.SamplerFlags & RT_ALPHA_WRAP_T_CLAMP) != 0u;
    const bool mirrorS = (sampling.SamplerFlags & RT_ALPHA_WRAP_S_MIRROR) != 0u;
    const bool mirrorT = (sampling.SamplerFlags & RT_ALPHA_WRAP_T_MIRROR) != 0u;
    const float2 transformedUV = TransformMaterialUV(sampling, uv);
    const float2 wrappedUV = float2(
        WrapMaterialCoordinate(transformedUV.x, clampS, mirrorS),
        WrapMaterialCoordinate(transformedUV.y, clampT, mirrorT));
    const float2 textureSize = float2(textureWidth, textureHeight);

    if ((sampling.SamplerFlags & RT_ALPHA_MAG_NEAREST) != 0u)
    {
        const uint2 texel = min(uint2(wrappedUV * textureSize),
                                uint2(textureWidth - 1u, textureHeight - 1u));
        return LoadMaterialTextureTexel(textureTableIndex, int2(texel));
    }

    const float2 texelPosition = wrappedUV * textureSize - 0.5f;
    const int2 baseTexel = int2(floor(texelPosition));
    const float2 blend = frac(texelPosition);
    const int2 texel00 = int2(WrapMaterialTexel(baseTexel.x, textureWidth, clampS, mirrorS),
                              WrapMaterialTexel(baseTexel.y, textureHeight, clampT, mirrorT));
    const int2 texel10 = int2(WrapMaterialTexel(baseTexel.x + 1, textureWidth, clampS, mirrorS),
                              texel00.y);
    const int2 texel01 = int2(texel00.x,
                              WrapMaterialTexel(baseTexel.y + 1, textureHeight, clampT, mirrorT));
    const int2 texel11 = int2(texel10.x, texel01.y);

    const float4 texelValue00 = LoadMaterialTextureTexel(textureTableIndex, texel00);
    const float4 texelValue10 = LoadMaterialTextureTexel(textureTableIndex, texel10);
    const float4 texelValue01 = LoadMaterialTextureTexel(textureTableIndex, texel01);
    const float4 texelValue11 = LoadMaterialTextureTexel(textureTableIndex, texel11);
    const float4 row0 = lerp(texelValue00, texelValue10, blend.x);
    const float4 row1 = lerp(texelValue01, texelValue11, blend.x);
    return lerp(row0, row1, blend.y);
}

float4 LoadMaterialTexture(uint textureTableIndex,
                           const RayTracingMaterialTextureSamplingMetadata sampling,
                           bool hitUVValid,
                           float2 hitUV)
{
    return hitUVValid
               ? SampleMaterialTextureAtUV(textureTableIndex, sampling, hitUV)
               : LoadMaterialTextureRepresentative(textureTableIndex);
}

float3 ComputeReflectionF0(float3 baseColor, float metallic)
{
    return lerp(float3(0.04f, 0.04f, 0.04f), baseColor, saturate(metallic));
}

float3 FresnelSchlickRoughness(float nDotV, float3 f0, float roughness)
{
    const float oneMinusRoughness = 1.0f - saturate(roughness);
    return f0 + (max(float3(oneMinusRoughness, oneMinusRoughness, oneMinusRoughness), f0) - f0) *
                    pow(1.0f - saturate(nDotV), 5.0f);
}

float RoughnessSpecularVisibility(float roughness)
{
    const float maxRoughness = max(gReflectionOptions.y, 1.0e-3f);
    const float visibility = saturate(1.0f - saturate(roughness) / maxRoughness);
    return visibility * visibility;
}

float3 EvaluateReflectionHitRadiance(float3 baseColor,
                                     float metallic,
                                     float roughness,
                                     float nDotV,
                                     float3 emissive)
{
    const float clampedRoughness = clamp(roughness, 0.04f, 1.0f);
    const float3 f0 = ComputeReflectionF0(baseColor, metallic);
    const float3 fresnel = FresnelSchlickRoughness(nDotV, f0, clampedRoughness);
    const float3 diffuseEnergy = baseColor * (1.0f - fresnel) * (1.0f - saturate(metallic));
    const float3 specularEnergy = fresnel * RoughnessSpecularVisibility(clampedRoughness);
    return max(diffuseEnergy + specularEnergy + emissive, 0.0f);
}

float3 ClampReflectionSampleRadiance(float3 radiance, float maxLuminance)
{
    if (!all(isfinite(radiance)))
    {
        return 0.0f;
    }

    radiance = max(radiance, 0.0f);
    const float clampLimit = max(maxLuminance, 0.0f);
    if (clampLimit <= 0.0f)
    {
        return radiance;
    }

    const float luminance = dot(radiance, float3(0.2126f, 0.7152f, 0.0722f));
    if (luminance <= clampLimit)
    {
        return radiance;
    }

    return radiance * (clampLimit / max(luminance, 1.0e-4f));
}

float ReflectionLuminance(float3 radiance)
{
    return dot(max(radiance, 0.0f), float3(0.2126f, 0.7152f, 0.0722f));
}

float3 ClampReflectionHistoryRadiance(float3 historyRadiance, float3 currentRadiance, float luminanceTolerance)
{
    if (!all(isfinite(historyRadiance)) || !all(isfinite(currentRadiance)))
    {
        return max(currentRadiance, 0.0f);
    }

    historyRadiance = max(historyRadiance, 0.0f);
    currentRadiance = max(currentRadiance, 0.0f);
    const float tolerance = max(luminanceTolerance, 0.0f);
    if (tolerance <= 0.0f)
    {
        return historyRadiance;
    }

    const float historyLuminance = ReflectionLuminance(historyRadiance);
    const float currentLuminance = ReflectionLuminance(currentRadiance);
    const float maxHistoryLuminance = currentLuminance + tolerance;
    if (historyLuminance <= maxHistoryLuminance)
    {
        return historyRadiance;
    }

    return historyRadiance * (maxHistoryLuminance / max(historyLuminance, 1.0e-4f));
}

float ComputeVelocityHistoryWeight(uint2 scenePixel)
{
    if (gHistoryClampParams.z <= 0.5f)
    {
        return 1.0f;
    }

    const float2 velocityNdc = gSceneVelocity.Load(int3(scenePixel, 0)).xy;
    if (!all(isfinite(velocityNdc)))
    {
        return 0.0f;
    }

    const float velocityRejectionScale = max(gHistoryClampParams.w, 0.0f);
    if (velocityRejectionScale <= 0.0f)
    {
        return 1.0f;
    }

    return saturate(1.0f - length(velocityNdc) * velocityRejectionScale);
}

float ComputeReflectionHistoryWeight(float4 currentReflection, float4 previousReflection, float velocityHistoryWeight)
{
    if (!all(isfinite(currentReflection.rgb)) || !all(isfinite(previousReflection.rgb)))
    {
        return 0.0f;
    }

    const float motionWeight = saturate(velocityHistoryWeight);
    if (motionWeight <= 0.0f)
    {
        return 0.0f;
    }

    const float baseWeight = saturate(gHistoryParams.y);
    const float minConfidence = saturate(gHistoryClampParams.y);
    if (currentReflection.a <= minConfidence && previousReflection.a > currentReflection.a)
    {
        return 0.0f;
    }

    const float tolerance = max(gHistoryClampParams.x, 0.0f);
    if (tolerance <= 0.0f)
    {
        return baseWeight * motionWeight;
    }

    const float luminanceDelta =
        abs(ReflectionLuminance(previousReflection.rgb) - ReflectionLuminance(currentReflection.rgb));
    const float luminanceWeight = saturate(1.0f - luminanceDelta / max(tolerance, 1.0e-4f));
    return baseWeight * luminanceWeight * motionWeight;
}

bool TryApplyNormalMap(const RayTracingInstanceMaterialMetadata material,
                       const RayTracingInstanceAlphaMetadata geometry,
                       BuiltInTriangleIntersectionAttributes attributes,
                       bool hitUVValid,
                       float2 hitUV,
                       inout float3 hitNormal)
{
    if (!HasMaterialTexture(material.Flags, RT_MATERIAL_HAS_NORMAL_TEXTURE, material.NormalTextureTableIndex))
    {
        return false;
    }

    float4 hitTangent;
    if (!TryLoadHitTangent(geometry, attributes, hitTangent))
    {
        return false;
    }

    const float3 sampledNormal = LoadMaterialTexture(
        material.NormalTextureTableIndex,
        material.NormalTextureSampling,
        hitUVValid,
        hitUV).xyz;
    float3 tangentNormal = sampledNormal * 2.0f - 1.0f;
    tangentNormal.xy *= max(material.MaterialFactors.w, 0.0f);
    tangentNormal = normalize(tangentNormal);
    if (!all(isfinite(tangentNormal)) || dot(tangentNormal, tangentNormal) <= 0.5f)
    {
        return false;
    }

    float3 tangent = normalize(hitTangent.xyz - hitNormal * dot(hitNormal, hitTangent.xyz));
    if (!all(isfinite(tangent)) || dot(tangent, tangent) <= 0.5f)
    {
        return false;
    }

    const float tangentSign = hitTangent.w < 0.0f ? -1.0f : 1.0f;
    const float3 bitangent = normalize(cross(hitNormal, tangent)) * tangentSign;
    const float3 mappedNormal = normalize(
        tangent * tangentNormal.x +
        bitangent * tangentNormal.y +
        hitNormal * tangentNormal.z);
    if (!all(isfinite(mappedNormal)) || dot(mappedNormal, mappedNormal) <= 0.5f)
    {
        return false;
    }

    hitNormal = mappedNormal;
    return true;
}

float ComputeHitDistanceConfidence(float hitT, float tMax)
{
    const float safeTMax = max(tMax, 1.0e-3f);
    const float fadeStart = saturate(gReflectionOptions.w) * safeTMax;
    if (hitT <= fadeStart)
    {
        return 1.0f;
    }

    return saturate((safeTMax - hitT) / max(safeTMax - fadeStart, 1.0e-3f));
}

uint HashUInt(uint value)
{
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;
    return value;
}

float HashToUnitFloat(uint value)
{
    return (float)(HashUInt(value) & 0x00FFFFFFu) * (1.0f / 16777216.0f);
}

float2 ReflectionRandom2(uint2 pixel, uint sampleIndex)
{
    const uint frameSeed = (uint)max(gStochasticParams.y, 0.0f);
    const uint seed = pixel.x * 1973u ^ pixel.y * 9277u ^ sampleIndex * 26699u ^ frameSeed * 104729u;
    return float2(HashToUnitFloat(seed), HashToUnitFloat(seed ^ 0x9e3779b9u));
}

void BuildOrthonormalBasis(float3 axis, out float3 tangent, out float3 bitangent)
{
    const float3 up = abs(axis.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(0.0f, 1.0f, 0.0f);
    tangent = normalize(cross(up, axis));
    bitangent = cross(axis, tangent);
}

float3 BuildStochasticReflectionDirection(float3 mirrorDirection,
                                          float3 surfaceNormal,
                                          uint2 pixel,
                                          uint sampleIndex,
                                          uint sampleCount)
{
    const float coneSpread = saturate(gStochasticParams.z);
    if (sampleCount <= 1u || coneSpread <= 1.0e-5f)
    {
        return mirrorDirection;
    }

    const float roughness = saturate(gReflectionOptions.y);
    const float coneFactor = saturate(roughness * roughness * coneSpread);
    if (coneFactor <= 1.0e-5f)
    {
        return mirrorDirection;
    }

    const float2 xi = ReflectionRandom2(pixel, sampleIndex);
    const float maxConeAngle = coneFactor * (0.5f * RVX_REFLECTION_PI);
    const float cosTheta = cos(maxConeAngle * sqrt(saturate(xi.x)));
    const float sinTheta = sqrt(max(0.0f, 1.0f - cosTheta * cosTheta));
    const float phi = 2.0f * RVX_REFLECTION_PI * xi.y;

    float3 tangent;
    float3 bitangent;
    BuildOrthonormalBasis(mirrorDirection, tangent, bitangent);

    const float3 direction = normalize(
        tangent * (cos(phi) * sinTheta) +
        bitangent * (sin(phi) * sinTheta) +
        mirrorDirection * cosTheta);

    if (!all(isfinite(direction)) || dot(direction, surfaceNormal) <= 1.0e-4f)
    {
        return mirrorDirection;
    }

    return direction;
}

float4 EncodeNormalHistory(float3 normal, bool valid)
{
    return valid ? float4(normal * 0.5f + 0.5f, 1.0f) : float4(0.5f, 0.5f, 0.5f, 0.0f);
}

bool DecodeNormalHistory(float4 encoded, out float3 normal)
{
    normal = encoded.xyz * 2.0f - 1.0f;
    return encoded.w > 0.5f && all(isfinite(normal)) && dot(normal, normal) > 0.5f;
}

bool TryResolvePreviousReflectionHistoryPixel(float3 worldPosition,
                                              uint2 scenePixel,
                                              out uint2 previousPixel,
                                              out float previousDepthNdc)
{
    previousPixel = 0u;
    previousDepthNdc = 0.0f;

    const float4 previousClip = mul(gPreviousViewProjection, float4(worldPosition, 1.0f));
    if (abs(previousClip.w) <= 1.0e-6f)
    {
        return false;
    }

    const float3 previousNdc = previousClip.xyz / previousClip.w;
    if (previousNdc.z < 0.0f || previousNdc.z > 1.0f)
    {
        return false;
    }

    float2 previousUv = float2(previousNdc.x * 0.5f + 0.5f, 0.5f - previousNdc.y * 0.5f);
    if (gHistoryClampParams.z > 0.5f)
    {
        const float2 currentUv = (float2(scenePixel) + 0.5f) * gSceneSizeAndInvSize.zw;
        const float2 velocityNdc = gSceneVelocity.Load(int3(scenePixel, 0)).xy;
        if (all(isfinite(velocityNdc)))
        {
            const float2 velocityUv = float2(velocityNdc.x * 0.5f, -velocityNdc.y * 0.5f);
            previousUv = currentUv - velocityUv;
        }
    }

    if (previousUv.x < 0.0f || previousUv.x > 1.0f ||
        previousUv.y < 0.0f || previousUv.y > 1.0f)
    {
        return false;
    }

    const uint2 outputSize = max(uint2(gOutputSizeAndInvSize.xy), uint2(1u, 1u));
    previousPixel = min(uint2(previousUv * gOutputSizeAndInvSize.xy), outputSize - 1u);
    previousDepthNdc = previousNdc.z;
    return true;
}

bool TryLoadReprojectedReflection(float3 worldPosition,
                                  float3 currentNormal,
                                  uint2 scenePixel,
                                  out float4 previousReflection)
{
    previousReflection = float4(0.0f, 0.0f, 0.0f, 0.0f);

    uint2 previousPixel;
    float previousDepthNdc;
    if (!TryResolvePreviousReflectionHistoryPixel(worldPosition, scenePixel, previousPixel, previousDepthNdc))
    {
        return false;
    }

    const float previousDepth = gPreviousDepthHistory.Load(int3(previousPixel, 0)).r;
    if (IsBackgroundDepth(previousDepth))
    {
        return false;
    }

    const float depthDelta = abs(previousDepth - previousDepthNdc);
    if (depthDelta > max(gHistoryParams.z, 0.0f))
    {
        return false;
    }

    float3 previousNormal;
    if (!DecodeNormalHistory(gPreviousNormalHistory.Load(int3(previousPixel, 0)), previousNormal))
    {
        return false;
    }

    if (dot(previousNormal, currentNormal) < saturate(gHistoryParams.w))
    {
        return false;
    }

    previousReflection = gPreviousReflectionHistory.Load(int3(previousPixel, 0));
    previousReflection.rgb = max(previousReflection.rgb, 0.0f);
    previousReflection.a = saturate(previousReflection.a);
    return true;
}

[shader("raygeneration")]
void ReflectionRayGen()
{
    const uint3 dispatchIndex = DispatchRaysIndex();
    const uint2 pixel = dispatchIndex.xy;
    const uint2 scenePixel = MapOutputPixelToScenePixel(pixel);
    const float depth = gSceneDepth.Load(int3(scenePixel, 0)).r;
    gCurrentDepthHistory[pixel] = depth;
    if (IsBackgroundDepth(depth))
    {
        gCurrentNormalHistory[pixel] = EncodeNormalHistory(0.0f, false);
        gReflectionOutput[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    const float3 worldPosition = ReconstructWorldPosition(scenePixel, depth);
    float3 worldNormal = 0.0f;
    const bool normalValid = TryReconstructWorldNormal(scenePixel, worldPosition, worldNormal);
    gCurrentNormalHistory[pixel] = EncodeNormalHistory(worldNormal, normalValid);
    if (!normalValid)
    {
        gReflectionOutput[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    const float3 viewDirection = normalize(worldPosition - gCameraPositionAndTMax.xyz);
    const float3 reflectionDirection = normalize(reflect(viewDirection, worldNormal));
    const uint sampleCount = min(
        max((uint)round(gStochasticParams.x), 1u),
        RVX_RT_REFLECTION_MAX_SAMPLES_PER_PIXEL);

    float3 accumulatedColor = 0.0f;
    float accumulatedConfidence = 0.0f;
    [loop]
    for (uint sampleIndex = 0u; sampleIndex < RVX_RT_REFLECTION_MAX_SAMPLES_PER_PIXEL; ++sampleIndex)
    {
        if (sampleIndex >= sampleCount)
        {
            break;
        }

        const float3 rayDirection =
            BuildStochasticReflectionDirection(reflectionDirection, worldNormal, pixel, sampleIndex, sampleCount);

        RayDesc ray;
        ray.Origin = worldPosition + worldNormal * max(gRayBiasParams.x, 0.0f);
        ray.Direction = rayDirection;
        ray.TMin = max(gRayBiasParams.y, 0.0f);
        ray.TMax = max(gCameraPositionAndTMax.w, 1.0f);

        ReflectionPayload payload;
        payload.color = 0.0f;
        payload.hit = 0u;
        payload.hitT = 0.0f;

        TraceRay(gScene,
                 RAY_FLAG_NONE,
                 ((uint)gReflectionOptions.z) & 0xFFu,
                 0,
                 1,
                 0,
                 ray,
                 payload);

        accumulatedColor += ClampReflectionSampleRadiance(payload.color, gRayBiasParams.z);
        accumulatedConfidence += payload.hit != 0u
                                     ? ComputeHitDistanceConfidence(payload.hitT, ray.TMax)
                                     : 0.0f;
    }

    const float intensity = saturate(gReflectionOptions.x);
    const float invSampleCount = rcp((float)sampleCount);
    float4 reflection = float4(
        accumulatedColor * invSampleCount,
        saturate(accumulatedConfidence * invSampleCount) * intensity);
    if (gHistoryParams.x > 0.5f)
    {
        float4 previousReflection;
        if (TryLoadReprojectedReflection(worldPosition, worldNormal, scenePixel, previousReflection))
        {
            const float velocityHistoryWeight = ComputeVelocityHistoryWeight(scenePixel);
            const float historyWeight =
                ComputeReflectionHistoryWeight(reflection, previousReflection, velocityHistoryWeight);
            previousReflection.rgb =
                ClampReflectionHistoryRadiance(previousReflection.rgb, reflection.rgb, gHistoryClampParams.x);
            reflection = lerp(reflection, previousReflection, historyWeight);
        }
    }

    gReflectionOutput[pixel] = reflection;
}

[shader("miss")]
void ReflectionMiss(inout ReflectionPayload payload)
{
    payload.color = 0.0f;
    payload.hit = 0u;
    payload.hitT = 0.0f;
}

bool ShouldIgnoreReflectionHitForAlpha(const RayTracingInstanceMaterialMetadata material,
                                       const RayTracingInstanceAlphaMetadata geometry,
                                       BuiltInTriangleIntersectionAttributes attributes)
{
    const bool alphaTestEnabled =
        (material.Flags & RT_MATERIAL_ALPHA_TEST) != 0u ||
        (geometry.Flags & RT_ALPHA_TEST_ENABLED) != 0u;
    if (!alphaTestEnabled)
    {
        return false;
    }

    const float baseAlpha = isfinite(material.BaseColorFactor.a) ? saturate(material.BaseColorFactor.a) : 0.0f;
    const float cutoff = isfinite(material.MaterialFactors.z) ? saturate(material.MaterialFactors.z) : 0.5f;
    float resolvedAlpha = baseAlpha;

    if (HasMaterialTexture(material.Flags, RT_MATERIAL_HAS_BASE_COLOR_TEXTURE, material.BaseColorTextureTableIndex))
    {
        float2 hitUV = 0.0f;
        const bool hitUVValid = TryLoadHitUV(geometry, attributes, hitUV);
        resolvedAlpha *= LoadMaterialTexture(
            material.BaseColorTextureTableIndex,
            material.BaseColorTextureSampling,
            hitUVValid,
            hitUV).a;
    }

    return resolvedAlpha < cutoff;
}

[shader("anyhit")]
void ReflectionAnyHit(inout ReflectionPayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    const uint instanceId = InstanceID();
    const RayTracingInstanceMaterialMetadata material = gInstanceMaterialMetadata[instanceId];
    const RayTracingInstanceAlphaMetadata geometry = gInstanceGeometryMetadata[instanceId];
    if (ShouldIgnoreReflectionHitForAlpha(material, geometry, attributes))
    {
        IgnoreHit();
    }
}

[shader("closesthit")]
void ReflectionClosestHit(inout ReflectionPayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    const uint instanceId = InstanceID();
    const RayTracingInstanceMaterialMetadata material = gInstanceMaterialMetadata[instanceId];
    const RayTracingInstanceAlphaMetadata geometry = gInstanceGeometryMetadata[instanceId];
    float2 hitUV = 0.0f;
    const bool hitUVValid = TryLoadHitUV(geometry, attributes, hitUV);
    float3 baseColor = saturate(material.BaseColorFactor.rgb);
    if (HasMaterialTexture(material.Flags, RT_MATERIAL_HAS_BASE_COLOR_TEXTURE, material.BaseColorTextureTableIndex))
    {
        baseColor *= LoadMaterialTexture(
            material.BaseColorTextureTableIndex,
            material.BaseColorTextureSampling,
            hitUVValid,
            hitUV).rgb;
    }

    float3 hitNormal = 0.0f;
    float nDotV = 1.0f;
    if (TryLoadHitNormal(geometry, attributes, hitNormal))
    {
        TryApplyNormalMap(material, geometry, attributes, hitUVValid, hitUV, hitNormal);
        const float3 hitViewDirection = normalize(-ObjectRayDirection());
        hitNormal = dot(hitNormal, hitViewDirection) < 0.0f ? -hitNormal : hitNormal;
        nDotV = max(dot(hitNormal, hitViewDirection), 0.001f);
    }

    float roughness = saturate(material.MaterialFactors.y);
    float metallic = saturate(material.MaterialFactors.x);
    if (HasMaterialTexture(material.Flags,
                           RT_MATERIAL_HAS_METALLIC_ROUGHNESS_TEXTURE,
                           material.MetallicRoughnessTextureTableIndex))
    {
        const float4 metallicRoughness = LoadMaterialTexture(
            material.MetallicRoughnessTextureTableIndex,
            material.MetallicRoughnessTextureSampling,
            hitUVValid,
            hitUV);
        roughness *= saturate(metallicRoughness.g);
        metallic *= saturate(metallicRoughness.b);
    }

    float3 emissive = material.EmissiveFactor.rgb * material.EmissiveFactor.w;
    if (HasMaterialTexture(material.Flags, RT_MATERIAL_HAS_EMISSIVE_TEXTURE, material.EmissiveTextureTableIndex))
    {
        emissive *= LoadMaterialTexture(
            material.EmissiveTextureTableIndex,
            material.EmissiveTextureSampling,
            hitUVValid,
            hitUV).rgb;
    }

    payload.color = EvaluateReflectionHitRadiance(baseColor, metallic, roughness, nDotV, emissive);
    payload.hit = 1u;
    payload.hitT = RayTCurrent();
}
