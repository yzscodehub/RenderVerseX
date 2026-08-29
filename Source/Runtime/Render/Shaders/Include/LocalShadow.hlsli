// =============================================================================
// LocalShadow.hlsli - Shared point/spot Texture2DArray shadow sampling
// =============================================================================
//
// The including shader owns the resource declarations and must expose these
// names from its LightConstants cbuffer:
//   LocalShadowViewProjections[32]
//   LocalShadowPageCount / LocalShadowInvResolution / LocalShadowDepthBias /
//   LocalShadowNormalBias
// It must also declare LocalShadowAtlasTexture and LocalShadowAtlasSampler.
// The contract is right-handed, Y-up, forward-Z [0,1] and uses ordinary depth
// sampling with manual PCF.  Invalid state is deliberately fully lit.
// =============================================================================

#ifndef RVX_LOCAL_SHADOW_HLSLI
#define RVX_LOCAL_SHADOW_HLSLI

static const uint RVX_LOCAL_SHADOW_MAX_VISIBLE_PAGES = 32u;
static const uint RVX_LOCAL_SHADOW_POINT_FACE_COUNT = 6u;
static const uint RVX_LOCAL_SHADOW_PCF_RADIUS = 1u;

float3 RVX_LocalShadowSafeNormalize(float3 value, float3 fallback)
{
    const float lengthSquared = dot(value, value);
    return lengthSquared > 1.0e-8 ? value * rsqrt(lengthSquared) : fallback;
}

bool RVX_IsLocalShadowPageValid(int firstPage, uint pageOffset)
{
    if (firstPage < 0 || LocalShadowPageCount == 0u ||
        LocalShadowPageCount > RVX_LOCAL_SHADOW_MAX_VISIBLE_PAGES ||
        LocalShadowInvResolution <= 0.0 || !isfinite(LocalShadowInvResolution))
    {
        return false;
    }

    const uint pageIndex = uint(firstPage) + pageOffset;
    return pageIndex >= uint(firstPage) && pageIndex < LocalShadowPageCount;
}

// Fixed right-handed Y-up cube-face order.  It is intentionally identical to
// LocalShadowPageProjection: +X, -X, +Y, -Y, +Z, -Z.
uint RVX_SelectPointShadowFace(float3 lightToReceiver)
{
    const float3 axis = abs(lightToReceiver);
    if (axis.x >= axis.y && axis.x >= axis.z)
    {
        return lightToReceiver.x >= 0.0 ? 0u : 1u;
    }
    if (axis.y >= axis.z)
    {
        return lightToReceiver.y >= 0.0 ? 2u : 3u;
    }
    return lightToReceiver.z >= 0.0 ? 4u : 5u;
}

float RVX_SampleLocalShadowPage(
    int firstPage,
    uint pageOffset,
    float3 worldPos,
    float3 worldNormal)
{
    if (!RVX_IsLocalShadowPageValid(firstPage, pageOffset))
    {
        return 1.0;
    }

    const uint pageIndex = uint(firstPage) + pageOffset;
    const float3 receiverNormal = RVX_LocalShadowSafeNormalize(
        worldNormal, float3(0.0, 1.0, 0.0));
    const float3 biasedWorldPos = worldPos + receiverNormal *
        max(LocalShadowNormalBias, 0.0);
    const float4 shadowClip = mul(
        LocalShadowViewProjections[pageIndex], float4(biasedWorldPos, 1.0));
    if (abs(shadowClip.w) <= 1.0e-6)
    {
        return 1.0;
    }

    const float3 shadowNdc = shadowClip.xyz / shadowClip.w;
    const float2 shadowUV = float2(
        shadowNdc.x * 0.5 + 0.5,
        0.5 - shadowNdc.y * 0.5);
    if (any(shadowUV < 0.0) || any(shadowUV > 1.0) ||
        shadowNdc.z < 0.0 || shadowNdc.z > 1.0)
    {
        return 1.0;
    }

    const float compareDepth = shadowNdc.z - max(LocalShadowDepthBias, 0.0);
    float visibleTaps = 0.0;
    float tapCount = 0.0;
    [unroll]
    for (int y = -int(RVX_LOCAL_SHADOW_PCF_RADIUS);
         y <= int(RVX_LOCAL_SHADOW_PCF_RADIUS); ++y)
    {
        [unroll]
        for (int x = -int(RVX_LOCAL_SHADOW_PCF_RADIUS);
             x <= int(RVX_LOCAL_SHADOW_PCF_RADIUS); ++x)
        {
            const float2 tapUV = shadowUV + float2(x, y) *
                LocalShadowInvResolution;
            if (any(tapUV < 0.0) || any(tapUV > 1.0))
            {
                continue;
            }
            const float storedDepth = LocalShadowAtlasTexture.SampleLevel(
                LocalShadowAtlasSampler, float3(tapUV, pageIndex), 0.0).r;
            visibleTaps += compareDepth <= storedDepth ? 1.0 : 0.0;
            tapCount += 1.0;
        }
    }
    return tapCount > 0.0 ? visibleTaps / tapCount : 1.0;
}

float RVX_SamplePointLocalShadow(
    PointLight light,
    float3 worldPos,
    float3 worldNormal)
{
    if (light.shadowMapIndex < 0)
    {
        return 1.0;
    }
    const float3 lightToReceiver = worldPos - light.position;
    if (dot(lightToReceiver, lightToReceiver) <= 1.0e-8)
    {
        return 1.0;
    }
    return RVX_SampleLocalShadowPage(
        light.shadowMapIndex,
        RVX_SelectPointShadowFace(lightToReceiver),
        worldPos,
        worldNormal);
}

float RVX_SampleSpotLocalShadow(
    SpotLight light,
    float3 worldPos,
    float3 worldNormal)
{
    return RVX_SampleLocalShadowPage(
        light.shadowMapIndex, 0u, worldPos, worldNormal);
}

#endif // RVX_LOCAL_SHADOW_HLSLI
