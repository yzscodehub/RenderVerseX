/**
 * @file RayTracedShadow.hlsl
 * @brief Minimal DXR directional shadow mask path
 */

#include "RayTracingResourceBindings.hlsli"
#include "RayTracingSceneMetadata.hlsli"

RaytracingAccelerationStructure gScene : register(RVX_RT_SHADOW_TLAS_REGISTER, space0);
RWTexture2D<float> gShadowMask : register(RVX_RT_SHADOW_OUTPUT_MASK_REGISTER, space0);
Texture2D<float> gSceneDepth : register(RVX_RT_SHADOW_SCENE_DEPTH_REGISTER, space0);
Texture2D<float> gPreviousShadowMask : register(RVX_RT_SHADOW_PREVIOUS_MASK_REGISTER, space0);
Texture2D<float> gPreviousDepthHistory : register(RVX_RT_SHADOW_PREVIOUS_DEPTH_REGISTER, space0);
RWTexture2D<float> gCurrentDepthHistory : register(RVX_RT_SHADOW_OUTPUT_DEPTH_REGISTER, space0);
Texture2D<float4> gPreviousNormalHistory : register(RVX_RT_SHADOW_PREVIOUS_NORMAL_REGISTER, space0);
RWTexture2D<float4> gCurrentNormalHistory : register(RVX_RT_SHADOW_OUTPUT_NORMAL_REGISTER, space0);

static const uint RT_MAX_MATERIAL_TEXTURES = RVX_RT_SHADOW_MAX_MATERIAL_TEXTURES;
static const uint RT_MAX_ALPHA_TEXTURES = RVX_RT_SHADOW_MAX_ALPHA_TEXTURES;
static const uint RT_MAX_ALPHA_GEOMETRY_BUFFERS = RVX_RT_SHADOW_MAX_ALPHA_GEOMETRY_BUFFERS;

StructuredBuffer<RayTracingInstanceAlphaMetadata> gInstanceAlphaMetadata : register(RVX_RT_SHADOW_ALPHA_METADATA_REGISTER, space0);
Texture2D<float4> gAlphaBaseColorTextures[RT_MAX_ALPHA_TEXTURES] : register(RVX_RT_SHADOW_ALPHA_TEXTURES_REGISTER, space0);
ByteAddressBuffer gAlphaIndexBuffers[RT_MAX_ALPHA_GEOMETRY_BUFFERS] : register(RVX_RT_SHADOW_ALPHA_INDEX_BUFFERS_REGISTER, space0);
ByteAddressBuffer gAlphaUVBuffers[RT_MAX_ALPHA_GEOMETRY_BUFFERS] : register(RVX_RT_SHADOW_ALPHA_UV_BUFFERS_REGISTER, space0);
StructuredBuffer<RayTracingInstanceMaterialMetadata> gInstanceMaterialMetadata : register(RVX_RT_SHADOW_MATERIAL_METADATA_REGISTER, space0);
Texture2D<float4> gMaterialTextures[RT_MAX_MATERIAL_TEXTURES] : register(RVX_RT_SHADOW_MATERIAL_TEXTURES_REGISTER, space0);
Texture2D<float2> gSceneVelocity : register(RVX_RT_SHADOW_SCENE_VELOCITY_REGISTER, space0);

cbuffer RayTracedShadowConstants : register(RVX_RT_SHADOW_CONSTANTS_REGISTER, space0)
{
    float4x4 gInverseViewProjection;
    float4x4 gPreviousViewProjection;
    float4 gLightDirectionAndTMax; // xyz: normalized receiver-to-light direction, w: max trace distance
    float4 gViewportSizeAndInvSize; // xy: size, zw: reciprocal size
    float4 gDepthAndBiasParams; // x: reverse-Z flag, y: origin bias, z: history valid, w: history weight
    float4 gHistoryReprojectionParams; // x: depth rejection threshold, y: minimum normal dot, z: velocity available, w: velocity rejection scale
    float4 gSoftShadowParams; // x: light angular radius in radians, y: frame seed, z: samples per pixel
    float4 gRayOptions; // x: TraceRay instance mask in [0, 255]
};

struct ShadowPayload
{
    uint hit;
};

uint LoadAlphaIndex(const RayTracingInstanceAlphaMetadata alpha, uint elementIndex)
{
    if ((alpha.Flags & RT_ALPHA_INDEX_FORMAT_UINT32) != 0u)
    {
        return gAlphaIndexBuffers[NonUniformResourceIndex(alpha.IndexBufferTableIndex)].Load(elementIndex * 4u);
    }

    if ((alpha.Flags & RT_ALPHA_INDEX_FORMAT_UINT16) != 0u)
    {
        const uint byteOffset = elementIndex * 2u;
        const uint packed = gAlphaIndexBuffers[NonUniformResourceIndex(alpha.IndexBufferTableIndex)].Load(byteOffset & ~3u);
        return (byteOffset & 2u) != 0u ? ((packed >> 16u) & 0xFFFFu) : (packed & 0xFFFFu);
    }

    return 0u;
}

float2 TransformAlphaUV(const RayTracingInstanceAlphaMetadata alpha, float2 uv)
{
    float sine = 0.0f;
    float cosine = 1.0f;
    sincos(alpha.BaseColorUVRotation, sine, cosine);

    uv *= alpha.BaseColorUVScale;
    uv = float2(
        uv.x * cosine - uv.y * sine,
        uv.x * sine + uv.y * cosine);
    return uv + alpha.BaseColorUVOffset;
}

float WrapAlphaCoordinate(float value, bool clampToEdge, bool mirrorRepeat)
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

int WrapAlphaTexel(int texel, uint extent, bool clampToEdge, bool mirrorRepeat)
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

float LoadAlphaTextureTexel(const RayTracingInstanceAlphaMetadata alpha, int2 texel)
{
    return gAlphaBaseColorTextures[NonUniformResourceIndex(alpha.BaseColorTextureTableIndex)].Load(int3(texel, 0)).a;
}

float LoadMaterialBaseColorAlphaTexel(uint textureTableIndex, int2 texel)
{
    return gMaterialTextures[NonUniformResourceIndex(textureTableIndex)].Load(int3(texel, 0)).a;
}

float SampleAlphaTexture(const RayTracingInstanceAlphaMetadata alpha, float2 uv)
{
    uint textureWidth = 0u;
    uint textureHeight = 0u;
    gAlphaBaseColorTextures[NonUniformResourceIndex(alpha.BaseColorTextureTableIndex)].GetDimensions(textureWidth, textureHeight);
    if (textureWidth == 0u || textureHeight == 0u)
    {
        return 1.0f;
    }

    const bool clampS = (alpha.BaseColorSamplerFlags & RT_ALPHA_WRAP_S_CLAMP) != 0u;
    const bool clampT = (alpha.BaseColorSamplerFlags & RT_ALPHA_WRAP_T_CLAMP) != 0u;
    const bool mirrorS = (alpha.BaseColorSamplerFlags & RT_ALPHA_WRAP_S_MIRROR) != 0u;
    const bool mirrorT = (alpha.BaseColorSamplerFlags & RT_ALPHA_WRAP_T_MIRROR) != 0u;
    const float2 transformedUV = TransformAlphaUV(alpha, uv);
    const float2 wrappedUV = float2(
        WrapAlphaCoordinate(transformedUV.x, clampS, mirrorS),
        WrapAlphaCoordinate(transformedUV.y, clampT, mirrorT));
    const float2 textureSize = float2(textureWidth, textureHeight);

    if ((alpha.BaseColorSamplerFlags & RT_ALPHA_MAG_NEAREST) != 0u)
    {
        const uint2 texel = min(uint2(wrappedUV * textureSize),
                                uint2(textureWidth - 1u, textureHeight - 1u));
        return LoadAlphaTextureTexel(alpha, int2(texel));
    }

    const float2 texelPosition = wrappedUV * textureSize - 0.5f;
    const int2 baseTexel = int2(floor(texelPosition));
    const float2 blend = frac(texelPosition);
    const int2 texel00 = int2(WrapAlphaTexel(baseTexel.x, textureWidth, clampS, mirrorS),
                              WrapAlphaTexel(baseTexel.y, textureHeight, clampT, mirrorT));
    const int2 texel10 = int2(WrapAlphaTexel(baseTexel.x + 1, textureWidth, clampS, mirrorS),
                              texel00.y);
    const int2 texel01 = int2(texel00.x,
                              WrapAlphaTexel(baseTexel.y + 1, textureHeight, clampT, mirrorT));
    const int2 texel11 = int2(texel10.x, texel01.y);

    const float alpha00 = LoadAlphaTextureTexel(alpha, texel00);
    const float alpha10 = LoadAlphaTextureTexel(alpha, texel10);
    const float alpha01 = LoadAlphaTextureTexel(alpha, texel01);
    const float alpha11 = LoadAlphaTextureTexel(alpha, texel11);
    const float row0 = lerp(alpha00, alpha10, blend.x);
    const float row1 = lerp(alpha01, alpha11, blend.x);
    return lerp(row0, row1, blend.y);
}

float SampleMaterialBaseColorAlpha(uint textureTableIndex, const RayTracingInstanceAlphaMetadata alpha, float2 uv)
{
    uint textureWidth = 0u;
    uint textureHeight = 0u;
    gMaterialTextures[NonUniformResourceIndex(textureTableIndex)].GetDimensions(textureWidth, textureHeight);
    if (textureWidth == 0u || textureHeight == 0u)
    {
        return 1.0f;
    }

    const bool clampS = (alpha.BaseColorSamplerFlags & RT_ALPHA_WRAP_S_CLAMP) != 0u;
    const bool clampT = (alpha.BaseColorSamplerFlags & RT_ALPHA_WRAP_T_CLAMP) != 0u;
    const bool mirrorS = (alpha.BaseColorSamplerFlags & RT_ALPHA_WRAP_S_MIRROR) != 0u;
    const bool mirrorT = (alpha.BaseColorSamplerFlags & RT_ALPHA_WRAP_T_MIRROR) != 0u;
    const float2 transformedUV = TransformAlphaUV(alpha, uv);
    const float2 wrappedUV = float2(
        WrapAlphaCoordinate(transformedUV.x, clampS, mirrorS),
        WrapAlphaCoordinate(transformedUV.y, clampT, mirrorT));
    const float2 textureSize = float2(textureWidth, textureHeight);

    if ((alpha.BaseColorSamplerFlags & RT_ALPHA_MAG_NEAREST) != 0u)
    {
        const uint2 texel = min(uint2(wrappedUV * textureSize),
                                uint2(textureWidth - 1u, textureHeight - 1u));
        return LoadMaterialBaseColorAlphaTexel(textureTableIndex, int2(texel));
    }

    const float2 texelPosition = wrappedUV * textureSize - 0.5f;
    const int2 baseTexel = int2(floor(texelPosition));
    const float2 blend = frac(texelPosition);
    const int2 texel00 = int2(WrapAlphaTexel(baseTexel.x, textureWidth, clampS, mirrorS),
                              WrapAlphaTexel(baseTexel.y, textureHeight, clampT, mirrorT));
    const int2 texel10 = int2(WrapAlphaTexel(baseTexel.x + 1, textureWidth, clampS, mirrorS),
                              texel00.y);
    const int2 texel01 = int2(texel00.x,
                              WrapAlphaTexel(baseTexel.y + 1, textureHeight, clampT, mirrorT));
    const int2 texel11 = int2(texel10.x, texel01.y);

    const float alpha00 = LoadMaterialBaseColorAlphaTexel(textureTableIndex, texel00);
    const float alpha10 = LoadMaterialBaseColorAlphaTexel(textureTableIndex, texel10);
    const float alpha01 = LoadMaterialBaseColorAlphaTexel(textureTableIndex, texel01);
    const float alpha11 = LoadMaterialBaseColorAlphaTexel(textureTableIndex, texel11);
    const float row0 = lerp(alpha00, alpha10, blend.x);
    const float row1 = lerp(alpha01, alpha11, blend.x);
    return lerp(row0, row1, blend.y);
}

bool IsBackgroundDepth(float depth)
{
    if (gDepthAndBiasParams.x > 0.5f)
    {
        return depth <= 1.0e-6f;
    }

    return depth >= 0.999999f;
}

uint HashUInt(uint value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

float RandomFloat01(uint seed)
{
    return (float)(HashUInt(seed) & 0x00FFFFFFu) * (1.0f / 16777216.0f);
}

float3 BuildSoftShadowRayDirection(float3 lightDirection, uint2 pixel, uint sampleIndex)
{
    const float angularRadius = max(gSoftShadowParams.x, 0.0f);
    if (angularRadius <= 1.0e-6f)
    {
        return lightDirection;
    }

    const uint frameSeed = (uint)gSoftShadowParams.y;
    const uint pixelSeed = pixel.x * 1973u ^ pixel.y * 9277u ^ frameSeed * 26699u ^ sampleIndex * 0x9e3779b9u;
    const float u0 = RandomFloat01(pixelSeed);
    const float u1 = RandomFloat01(pixelSeed ^ 0x68bc21ebu);
    const float radius = sqrt(u0) * tan(min(angularRadius, 0.25f));
    const float angle = u1 * 6.28318530718f;
    const float2 disk = float2(cos(angle), sin(angle)) * radius;

    const float3 referenceUp = abs(lightDirection.y) < 0.999f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
    const float3 tangent = normalize(cross(referenceUp, lightDirection));
    const float3 bitangent = cross(lightDirection, tangent);
    return normalize(lightDirection + tangent * disk.x + bitangent * disk.y);
}

float TraceShadowVisibility(float3 worldPosition, float3 rayDirection, float originBias)
{
    RayDesc ray;
    ray.Origin = worldPosition + rayDirection * originBias;
    ray.Direction = rayDirection;
    ray.TMin = 0.001f;
    ray.TMax = max(gLightDirectionAndTMax.w, 1.0f);

    ShadowPayload payload;
    payload.hit = 0;

    TraceRay(gScene,
             RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_NON_OPAQUE,
             ((uint)gRayOptions.x) & 0xFFu,
             0,
             1,
             0,
             ray,
             payload);

    return payload.hit != 0 ? 0.0f : 1.0f;
}

float3 ReconstructWorldPosition(uint2 pixel, float depth)
{
    const float2 uv = (float2(pixel) + 0.5f) * gViewportSizeAndInvSize.zw;
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 world = mul(gInverseViewProjection, float4(ndc, depth, 1.0f));
    world.xyz /= abs(world.w) > 1.0e-6f ? world.w : 1.0f;
    return world.xyz;
}

bool TryLoadWorldPosition(uint2 pixel, out float3 worldPosition)
{
    const float depth = gSceneDepth.Load(int3(pixel, 0)).r;
    if (IsBackgroundDepth(depth))
    {
        worldPosition = 0.0f;
        return false;
    }

    worldPosition = ReconstructWorldPosition(pixel, depth);
    return true;
}

float4 EncodeNormalHistory(float3 normal, bool valid)
{
    return valid ? float4(normal * 0.5f + 0.5f, 1.0f) : float4(0.0f, 0.0f, 0.0f, 0.0f);
}

bool DecodeNormalHistory(float4 encoded, out float3 normal)
{
    if (encoded.w <= 0.5f)
    {
        normal = 0.0f;
        return false;
    }

    normal = normalize(encoded.xyz * 2.0f - 1.0f);
    return all(isfinite(normal));
}

bool TryReconstructWorldNormal(uint2 pixel, float3 centerWorld, out float3 normal)
{
    const uint2 viewportSize = uint2(gViewportSizeAndInvSize.xy);
    const uint leftX = pixel.x > 0u ? pixel.x - 1u : 0u;
    const uint rightX = min(pixel.x + 1u, viewportSize.x - 1u);
    const uint upY = pixel.y > 0u ? pixel.y - 1u : 0u;
    const uint downY = min(pixel.y + 1u, viewportSize.y - 1u);
    const uint2 leftPixel = uint2(leftX, pixel.y);
    const uint2 rightPixel = uint2(rightX, pixel.y);
    const uint2 upPixel = uint2(pixel.x, upY);
    const uint2 downPixel = uint2(pixel.x, downY);

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

float ComputeVelocityHistoryWeight(uint2 pixel)
{
    if (gHistoryReprojectionParams.z <= 0.5f)
    {
        return 1.0f;
    }

    const float2 velocityNdc = gSceneVelocity.Load(int3(pixel, 0)).xy;
    if (!all(isfinite(velocityNdc)))
    {
        return 0.0f;
    }

    const float velocityRejectionScale = max(gHistoryReprojectionParams.w, 0.0f);
    if (velocityRejectionScale <= 0.0f)
    {
        return 1.0f;
    }

    return saturate(1.0f - length(velocityNdc) * velocityRejectionScale);
}

bool TryLoadReprojectedHistory(float3 worldPosition, float3 currentNormal, out float previousVisibility)
{
    previousVisibility = 1.0f;

    const float4 previousClip = mul(gPreviousViewProjection, float4(worldPosition, 1.0f));
    if (abs(previousClip.w) <= 1.0e-6f)
    {
        return false;
    }

    const float3 previousNdc = previousClip.xyz / previousClip.w;
    if (previousNdc.x < -1.0f || previousNdc.x > 1.0f ||
        previousNdc.y < -1.0f || previousNdc.y > 1.0f ||
        previousNdc.z < 0.0f || previousNdc.z > 1.0f)
    {
        return false;
    }

    const uint2 viewportSize = uint2(gViewportSizeAndInvSize.xy);
    const float2 previousUv = float2(previousNdc.x * 0.5f + 0.5f, 0.5f - previousNdc.y * 0.5f);
    uint2 previousPixel = uint2(previousUv * gViewportSizeAndInvSize.xy);
    previousPixel = min(previousPixel, viewportSize - 1u);

    const float previousDepth = gPreviousDepthHistory.Load(int3(previousPixel, 0)).r;
    if (IsBackgroundDepth(previousDepth))
    {
        return false;
    }

    const float depthDelta = abs(previousDepth - previousNdc.z);
    if (depthDelta > max(gHistoryReprojectionParams.x, 0.0f))
    {
        return false;
    }

    float3 previousNormal;
    if (!DecodeNormalHistory(gPreviousNormalHistory.Load(int3(previousPixel, 0)), previousNormal))
    {
        return false;
    }

    if (dot(previousNormal, currentNormal) < saturate(gHistoryReprojectionParams.y))
    {
        return false;
    }

    previousVisibility = saturate(gPreviousShadowMask.Load(int3(previousPixel, 0)).r);
    return true;
}

[shader("raygeneration")]
void RayGen()
{
    const uint3 dispatchIndex = DispatchRaysIndex();
    const uint2 pixel = dispatchIndex.xy;
    const float depth = gSceneDepth.Load(int3(pixel, 0)).r;
    gCurrentDepthHistory[pixel] = depth;

    if (IsBackgroundDepth(depth))
    {
        gCurrentNormalHistory[pixel] = EncodeNormalHistory(0.0f, false);
        gShadowMask[pixel] = 1.0f;
        return;
    }

    const float3 worldPosition = ReconstructWorldPosition(pixel, depth);
    float3 worldNormal = 0.0f;
    const bool normalValid = TryReconstructWorldNormal(pixel, worldPosition, worldNormal);
    gCurrentNormalHistory[pixel] = EncodeNormalHistory(worldNormal, normalValid);
    const float3 baseRayDirection = normalize(gLightDirectionAndTMax.xyz);
    const float originBias = max(gDepthAndBiasParams.y, 0.001f);
    const uint sampleCount = min(max((uint)gSoftShadowParams.z, 1u), 8u);

    float visibility = 0.0f;
    [loop]
    for (uint sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex)
    {
        const float3 rayDirection = BuildSoftShadowRayDirection(baseRayDirection, pixel, sampleIndex);
        visibility += TraceShadowVisibility(worldPosition, rayDirection, originBias);
    }
    visibility *= 1.0f / (float)sampleCount;
    if (gDepthAndBiasParams.z > 0.5f)
    {
        float previousVisibility = 1.0f;
        if (normalValid && TryLoadReprojectedHistory(worldPosition, worldNormal, previousVisibility))
        {
            const float velocityHistoryWeight = ComputeVelocityHistoryWeight(pixel);
            const float historyWeight = saturate(gDepthAndBiasParams.w) * velocityHistoryWeight;
            visibility = lerp(visibility, previousVisibility, historyWeight);
        }
    }

    gShadowMask[pixel] = visibility;
}

[shader("miss")]
void ShadowMiss(inout ShadowPayload payload)
{
    payload.hit = 0;
}

[shader("anyhit")]
void ShadowAnyHit(inout ShadowPayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    const uint instanceId = InstanceID();
    const RayTracingInstanceMaterialMetadata material = gInstanceMaterialMetadata[instanceId];
    if ((material.Flags & RT_MATERIAL_SHADOW_CASTER) == 0u)
    {
        IgnoreHit();
        return;
    }

    const RayTracingInstanceAlphaMetadata alpha = gInstanceAlphaMetadata[instanceId];
    const bool alphaTestEnabled =
        (material.Flags & RT_MATERIAL_ALPHA_TEST) != 0u ||
        (alpha.Flags & RT_ALPHA_TEST_ENABLED) != 0u;
    if (!alphaTestEnabled)
    {
        return;
    }

    const float baseAlpha = isfinite(material.BaseColorFactor.a) ? saturate(material.BaseColorFactor.a) : 0.0f;
    const float cutoff = isfinite(material.MaterialFactors.z) ? saturate(material.MaterialFactors.z) : 0.5f;
    float resolvedAlpha = baseAlpha;

    const bool canSampleAlphaTexture =
        (alpha.Flags & RT_ALPHA_HAS_RESOLVED_BASE_COLOR_TEXTURE) != 0u &&
        (alpha.Flags & RT_ALPHA_HAS_INDEX_BUFFER) != 0u &&
        (alpha.Flags & RT_ALPHA_HAS_UV_BUFFER) != 0u &&
        ((alpha.Flags & RT_ALPHA_INDEX_FORMAT_UINT32) != 0u ||
         (alpha.Flags & RT_ALPHA_INDEX_FORMAT_UINT16) != 0u) &&
        alpha.BaseColorTextureTableIndex < RT_MAX_ALPHA_TEXTURES &&
        alpha.IndexBufferTableIndex < RT_MAX_ALPHA_GEOMETRY_BUFFERS &&
        alpha.UVBufferTableIndex < RT_MAX_ALPHA_GEOMETRY_BUFFERS &&
        alpha.BaseColorTextureTableIndex != RT_INVALID_INDEX &&
        alpha.IndexBufferTableIndex != RT_INVALID_INDEX &&
        alpha.UVBufferTableIndex != RT_INVALID_INDEX;
    const bool canSampleMaterialBaseColor =
        (material.Flags & RT_MATERIAL_HAS_BASE_COLOR_TEXTURE) != 0u &&
        material.BaseColorTextureTableIndex < RT_MAX_MATERIAL_TEXTURES &&
        material.BaseColorTextureTableIndex != RT_INVALID_INDEX &&
        (alpha.Flags & RT_ALPHA_HAS_INDEX_BUFFER) != 0u &&
        (alpha.Flags & RT_ALPHA_HAS_UV_BUFFER) != 0u &&
        ((alpha.Flags & RT_ALPHA_INDEX_FORMAT_UINT32) != 0u ||
         (alpha.Flags & RT_ALPHA_INDEX_FORMAT_UINT16) != 0u) &&
        alpha.IndexBufferTableIndex < RT_MAX_ALPHA_GEOMETRY_BUFFERS &&
        alpha.UVBufferTableIndex < RT_MAX_ALPHA_GEOMETRY_BUFFERS &&
        alpha.IndexBufferTableIndex != RT_INVALID_INDEX &&
        alpha.UVBufferTableIndex != RT_INVALID_INDEX;

    if (canSampleMaterialBaseColor || canSampleAlphaTexture)
    {
        const uint firstIndexElement = alpha.IndexElementOffset + PrimitiveIndex() * 3u;
        const uint i0 = LoadAlphaIndex(alpha, firstIndexElement) + alpha.BaseVertex;
        const uint i1 = LoadAlphaIndex(alpha, firstIndexElement + 1u) + alpha.BaseVertex;
        const uint i2 = LoadAlphaIndex(alpha, firstIndexElement + 2u) + alpha.BaseVertex;

        const uint2 uv0Bits = gAlphaUVBuffers[NonUniformResourceIndex(alpha.UVBufferTableIndex)].Load2(i0 * 8u);
        const uint2 uv1Bits = gAlphaUVBuffers[NonUniformResourceIndex(alpha.UVBufferTableIndex)].Load2(i1 * 8u);
        const uint2 uv2Bits = gAlphaUVBuffers[NonUniformResourceIndex(alpha.UVBufferTableIndex)].Load2(i2 * 8u);
        const float2 uv0 = asfloat(uv0Bits);
        const float2 uv1 = asfloat(uv1Bits);
        const float2 uv2 = asfloat(uv2Bits);

        const float3 barycentrics = float3(
            1.0f - attributes.barycentrics.x - attributes.barycentrics.y,
            attributes.barycentrics.x,
            attributes.barycentrics.y);
        const float2 uv = uv0 * barycentrics.x + uv1 * barycentrics.y + uv2 * barycentrics.z;
        resolvedAlpha *= canSampleMaterialBaseColor
                             ? SampleMaterialBaseColorAlpha(material.BaseColorTextureTableIndex, alpha, uv)
                             : SampleAlphaTexture(alpha, uv);
    }

    if (resolvedAlpha < cutoff)
    {
        IgnoreHit();
    }
}

[shader("closesthit")]
void ShadowClosestHit(inout ShadowPayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    payload.hit = 1;
}
