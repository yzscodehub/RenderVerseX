#ifndef RVX_DIRECTIONAL_SHADOW_HLSLI
#define RVX_DIRECTIONAL_SHADOW_HLSLI

// Shared directional-shadow sampling contract for forward and deferred
// surfaces. The including shader must declare the canonical ViewConstants
// fields plus DirectionalShadowMapTexture and DirectionalShadowSampler before
// including this file, and must provide SafeNormalize(float3, float3).

float GetDirectionalShadowViewDepth(float3 worldPos)
{
    const float3 cameraForward = SafeNormalize(
        CameraForwardAndShadowCascadeCount.xyz,
        float3(0.0, 0.0, -1.0));
    return dot(worldPos - CameraPosition, cameraForward);
}

int SelectDirectionalShadowCascade(float3 worldPos)
{
    const int cascadeCount = (int)clamp(
        round(CameraForwardAndShadowCascadeCount.w), 1.0, 4.0);
    const float viewDepth = GetDirectionalShadowViewDepth(worldPos);
    int cascadeIndex = 0;

    [unroll]
    for (int i = 0; i < 3; ++i)
    {
        if (i + 1 < cascadeCount &&
            viewDepth > DirectionalShadowCascadeSplits[i])
        {
            cascadeIndex = i + 1;
        }
    }

    return cascadeIndex;
}

float CompareDirectionalShadowDepth(
    float2 uv,
    float compareDepth,
    int cascadeIndex)
{
    const float storedDepth = DirectionalShadowMapTexture.SampleLevel(
        DirectionalShadowSampler,
        float3(uv, (float)cascadeIndex),
        0).r;
    return compareDepth <= storedDepth ? 1.0 : 0.0;
}

static const int RVX_DIRECTIONAL_SHADOW_POISSON_TAP_COUNT = 16;
static const float2 RVX_DIRECTIONAL_SHADOW_POISSON_DISK[16] =
{
    float2(0.000000, 0.000000),
    float2(-0.326212, -0.405810),
    float2(-0.840144, -0.073580),
    float2(-0.695914, 0.457137),
    float2(-0.203345, 0.620716),
    float2(0.962340, -0.194983),
    float2(0.473434, -0.480026),
    float2(0.519456, 0.767022),
    float2(0.185461, -0.893124),
    float2(0.507431, 0.064425),
    float2(0.896420, 0.412458),
    float2(-0.321940, -0.932615),
    float2(-0.791559, -0.597710),
    float2(-0.040088, 0.536087),
    float2(0.342312, -0.217275),
    float2(-0.620000, 0.000000)
};

float SampleDirectionalShadowPCF(
    float2 shadowUV,
    float compareDepth,
    float filterStep,
    int cascadeIndex)
{
    const float filterStepUv = max(filterStep, 0.0);
    if (filterStepUv <= 1.0e-7)
    {
        return CompareDirectionalShadowDepth(
            shadowUV,
            compareDepth,
            cascadeIndex);
    }

    float visibility = 0.0;
    float tapCount = 0.0;

    [unroll]
    for (int i = 0; i < RVX_DIRECTIONAL_SHADOW_POISSON_TAP_COUNT; ++i)
    {
        const float2 tapUV = shadowUV +
            RVX_DIRECTIONAL_SHADOW_POISSON_DISK[i] * filterStepUv;
        if (!any(tapUV < 0.0) && !any(tapUV > 1.0))
        {
            visibility += CompareDirectionalShadowDepth(
                tapUV,
                compareDepth,
                cascadeIndex);
            tapCount += 1.0;
        }
    }

    return tapCount > 0.5 ? visibility / tapCount : 1.0;
}

float SampleDirectionalShadowCascade(
    float3 worldPos,
    float3 worldNormal,
    int cascadeIndex)
{
    const float3 receiverNormal = SafeNormalize(
        worldNormal,
        float3(0.0, 1.0, 0.0));
    const float normalBias = max(DirectionalShadowReceiverParams.x, 0.0);
    const float3 biasedWorldPos = worldPos + receiverNormal * normalBias;
    const float4 shadowClip = mul(
        DirectionalShadowViewProjections[cascadeIndex],
        float4(biasedWorldPos, 1.0));
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

    const float compareDepth = shadowNdc.z -
        max(DirectionalShadowParams.y, 0.0);
    const float lit = SampleDirectionalShadowPCF(
        shadowUV,
        compareDepth,
        DirectionalShadowParams.w,
        cascadeIndex);
    return lerp(1.0 - saturate(DirectionalShadowParams.z), 1.0, lit);
}

float SampleDirectionalShadow(float3 worldPos, float3 worldNormal)
{
    if (DirectionalShadowParams.x <= 0.5)
    {
        return 1.0;
    }

    const int cascadeCount = (int)clamp(
        round(CameraForwardAndShadowCascadeCount.w), 1.0, 4.0);
    const float viewDepth = GetDirectionalShadowViewDepth(worldPos);
    if (viewDepth > DirectionalShadowCascadeSplits[cascadeCount - 1])
    {
        return 1.0;
    }

    const int cascadeIndex = SelectDirectionalShadowCascade(worldPos);
    float shadow = SampleDirectionalShadowCascade(
        worldPos,
        worldNormal,
        cascadeIndex);
    const int nextCascadeIndex = cascadeIndex + 1;

    if (nextCascadeIndex < cascadeCount)
    {
        const float splitDistance = DirectionalShadowCascadeSplits[cascadeIndex];
        const float fadeDistance =
            DirectionalShadowCascadeFadeDistances[cascadeIndex];
        const float fadeStart = splitDistance - fadeDistance;
        const bool insideFadeBand = fadeDistance > 1.0e-5 &&
            viewDepth >= fadeStart &&
            viewDepth <= splitDistance;
        if (insideFadeBand)
        {
            float fadeT = saturate((viewDepth - fadeStart) / fadeDistance);
            fadeT = fadeT * fadeT * (3.0 - 2.0 * fadeT);
            const float nextShadow = SampleDirectionalShadowCascade(
                worldPos,
                worldNormal,
                nextCascadeIndex);
            shadow = lerp(shadow, nextShadow, fadeT);
        }
    }

    return shadow;
}

#endif // RVX_DIRECTIONAL_SHADOW_HLSLI
