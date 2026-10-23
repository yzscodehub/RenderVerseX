// =============================================================================
// GTAO.hlsl - deterministic, renderer-owned ground-truth AO approximation
// =============================================================================

#include "../Include/FullscreenTriangle.hlsli"
#include "../Include/AmbientOcclusion.hlsli"

cbuffer GTAOConstants : register(b0, space0)
{
    float4 RenderSize;          // xy extent, zw inverse extent
    float4 DepthParams;         // near, far, reverse-Z, normal source present
    float4 HorizonParams;       // radius, falloff start/end, thickness (meters)
    uint4 SamplingParams;       // direction count, step count, spatial radius, reserved
    float4 FilterParams;        // depth sigma (meters), normal threshold, temporal blend, history valid
    float4 TemporalParams;      // velocity scale, depth rejection (meters), normal rejection, velocity present
    float4 CameraPosition;
    float4 CameraForward;
    float4x4 InverseViewProjection;
    float4x4 PreviousInverseViewProjection;
};

// The layout always binds the complete family. A stage consumes only the
// entries it declares; the primitive still provides deterministic neutral
// fallbacks for optional normals and velocity.
Texture2D<float> DeviceDepth : register(t1, space0);
Texture2D<float4> NormalSource : register(t2, space0);
// Surface preparation preserves device depth in R32_FLOAT. Later visibility
// stages use the same scalar input binding with R8_UNORM fields.
Texture2D<float> AOInput : register(t3, space0);
Texture2D<float> AOHistory : register(t4, space0);
Texture2D<float2> Velocity : register(t5, space0);
Texture2D<float> HistoryDepth : register(t6, space0);
SamplerState LinearClampSampler : register(s7, space0);

struct VSOutput
{
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD0;
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    VSOutput output;
    output.TexCoord = RVX_GetFullscreenTriangleTexCoord(vertexId);
    output.Position = RVX_GetFullscreenTrianglePosition(output.TexCoord);
    return output;
}

bool RVX_IsBackgroundDepth(float deviceDepth)
{
    return DepthParams.z > 0.5
        ? deviceDepth <= 1.0e-6
        : deviceDepth >= 1.0 - 1.0e-6;
}

float2 RVX_OctWrap(float2 value)
{
    return (1.0 - abs(value.yx)) *
        float2(value.x >= 0.0 ? 1.0 : -1.0,
               value.y >= 0.0 ? 1.0 : -1.0);
}

float3 RVX_SafeNormalize(float3 value)
{
    const float lengthSquared = dot(value, value);
    return lengthSquared > 1.0e-10
        ? value * rsqrt(lengthSquared)
        : float3(0.0, 0.0, 1.0);
}

float3 RVX_DecodeOctNormal(float2 encoded)
{
    float2 folded = encoded * 2.0 - 1.0;
    float3 normal = float3(folded.xy, 1.0 - abs(folded.x) - abs(folded.y));
    if (normal.z < 0.0)
    {
        normal.xy = RVX_OctWrap(normal.xy);
    }
    return RVX_SafeNormalize(normal);
}

bool RVX_IsUVInBounds(float2 uv)
{
    return all(uv >= 0.0) && all(uv <= 1.0);
}

int2 RVX_PixelCoord(float2 uv)
{
    const int2 extent = max(int2(RenderSize.xy), int2(1, 1));
    return clamp(int2(uv * RenderSize.xy), int2(0, 0), extent - 1);
}

float2 RVX_PixelUV(int2 pixel)
{
    return (float2(pixel) + 0.5) * RenderSize.zw;
}

float RVX_LoadDeviceDepth(int2 pixel, bool previousFrame)
{
    return previousFrame
        ? HistoryDepth.Load(int3(pixel, 0))
        : DeviceDepth.Load(int3(pixel, 0));
}

float3 RVX_ReconstructWorldPosition(
    float2 uv,
    float deviceDepth,
    float4x4 inverseViewProjection)
{
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 world = mul(
        inverseViewProjection, float4(ndc, deviceDepth, 1.0));
    world.xyz /= abs(world.w) > 1.0e-6 ? world.w : 1.0;
    return world.xyz;
}

float RVX_ViewDepthMeters(float3 worldPosition)
{
    return dot(
        worldPosition - CameraPosition.xyz,
        RVX_SafeNormalize(CameraForward.xyz));
}

float3 RVX_ReconstructNormalFromDepth(float2 uv, bool previousFrame)
{
    const int2 extent = max(int2(RenderSize.xy), int2(1, 1));
    const int2 centerPixel = RVX_PixelCoord(uv);
    const float centerDepth = RVX_LoadDeviceDepth(
        centerPixel, previousFrame);
    if (RVX_IsBackgroundDepth(centerDepth))
    {
        return -RVX_SafeNormalize(CameraForward.xyz);
    }

    const float4x4 inverseViewProjection = previousFrame
        ? PreviousInverseViewProjection
        : InverseViewProjection;
    const float3 center = RVX_ReconstructWorldPosition(
        RVX_PixelUV(centerPixel), centerDepth, inverseViewProjection);
    const float centerViewDepth = RVX_ViewDepthMeters(center);

    const int2 leftPixel = max(centerPixel - int2(1, 0), int2(0, 0));
    const int2 rightPixel = min(centerPixel + int2(1, 0), extent - 1);
    const int2 upPixel = max(centerPixel - int2(0, 1), int2(0, 0));
    const int2 downPixel = min(centerPixel + int2(0, 1), extent - 1);
    const float leftDepth = RVX_LoadDeviceDepth(leftPixel, previousFrame);
    const float rightDepth = RVX_LoadDeviceDepth(rightPixel, previousFrame);
    const float upDepth = RVX_LoadDeviceDepth(upPixel, previousFrame);
    const float downDepth = RVX_LoadDeviceDepth(downPixel, previousFrame);
    const bool leftValid = leftPixel.x != centerPixel.x &&
        !RVX_IsBackgroundDepth(leftDepth);
    const bool rightValid = rightPixel.x != centerPixel.x &&
        !RVX_IsBackgroundDepth(rightDepth);
    const bool upValid = upPixel.y != centerPixel.y &&
        !RVX_IsBackgroundDepth(upDepth);
    const bool downValid = downPixel.y != centerPixel.y &&
        !RVX_IsBackgroundDepth(downDepth);
    if ((!leftValid && !rightValid) || (!upValid && !downValid))
    {
        return -RVX_SafeNormalize(CameraForward.xyz);
    }

    const float3 left = leftValid
        ? RVX_ReconstructWorldPosition(
              RVX_PixelUV(leftPixel), leftDepth, inverseViewProjection)
        : center;
    const float3 right = rightValid
        ? RVX_ReconstructWorldPosition(
              RVX_PixelUV(rightPixel), rightDepth, inverseViewProjection)
        : center;
    const float3 up = upValid
        ? RVX_ReconstructWorldPosition(
              RVX_PixelUV(upPixel), upDepth, inverseViewProjection)
        : center;
    const float3 down = downValid
        ? RVX_ReconstructWorldPosition(
              RVX_PixelUV(downPixel), downDepth, inverseViewProjection)
        : center;
    const float leftDelta = leftValid
        ? abs(RVX_ViewDepthMeters(left) - centerViewDepth)
        : 1.0e30;
    const float rightDelta = rightValid
        ? abs(RVX_ViewDepthMeters(right) - centerViewDepth)
        : 1.0e30;
    const float upDelta = upValid
        ? abs(RVX_ViewDepthMeters(up) - centerViewDepth)
        : 1.0e30;
    const float downDelta = downValid
        ? abs(RVX_ViewDepthMeters(down) - centerViewDepth)
        : 1.0e30;
    const float3 derivativeX = leftDelta < rightDelta
        ? center - left
        : right - center;
    const float3 derivativeUp = upDelta < downDelta
        ? up - center
        : center - down;
    float3 normal = RVX_SafeNormalize(cross(derivativeX, derivativeUp));
    const float3 towardCamera = RVX_SafeNormalize(
        CameraPosition.xyz - center);
    if (dot(normal, towardCamera) < 0.0)
    {
        normal = -normal;
    }
    return normal;
}

float3 RVX_LoadNormal(float2 uv)
{
    return DepthParams.w > 0.5
        ? RVX_DecodeOctNormal(
              NormalSource.Load(int3(RVX_PixelCoord(uv), 0)).xy)
        : RVX_ReconstructNormalFromDepth(uv, false);
}

float2 RVX_Direction(uint index, uint count)
{
    // A fixed golden-angle rotation avoids temporal/random noise. It is
    // intentionally independent of wall-clock time and backend details.
    const float angle = 6.28318530718 *
        (float(index) / float(max(count, 1u)) + 0.61803398875 * 0.5);
    return float2(cos(angle), sin(angle));
}

float PSSurface(VSOutput input) : SV_TARGET0
{
    const float2 uv = saturate(input.TexCoord);
    return DeviceDepth.Load(int3(RVX_PixelCoord(uv), 0));
}

float PSHorizon(VSOutput input) : SV_TARGET0
{
    const float2 uv = saturate(input.TexCoord);
    const float centerDeviceDepth = AOInput.Load(
        int3(RVX_PixelCoord(uv), 0));
    if (RVX_IsBackgroundDepth(centerDeviceDepth))
    {
        return 1.0;
    }

    const uint directionCount = clamp(SamplingParams.x, 1u, 64u);
    const uint stepCount = clamp(SamplingParams.y, 1u, 32u);
    const float3 centerPosition = RVX_ReconstructWorldPosition(
        uv, centerDeviceDepth, InverseViewProjection);
    const float centerViewDepth = RVX_ViewDepthMeters(centerPosition);
    const float2 onePixelUV = uv + float2(RenderSize.z, 0.0);
    const float3 onePixelPosition = RVX_ReconstructWorldPosition(
        onePixelUV, centerDeviceDepth, InverseViewProjection);
    const float metersPerPixel = max(
        length(onePixelPosition - centerPosition), 1.0e-5);
    const float radiusPixels = max(1.0, HorizonParams.x / metersPerPixel);
    const float falloffInterval = max(
        HorizonParams.z - HorizonParams.y, 1.0e-4);
    const float3 normal = RVX_LoadNormal(uv);
    float occlusion = 0.0;

    [loop]
    for (uint directionIndex = 0; directionIndex < 64u; ++directionIndex)
    {
        if (directionIndex >= directionCount)
        {
            break;
        }

        const float2 direction = RVX_Direction(directionIndex, directionCount);
        float horizon = 0.0;
        [loop]
        for (uint step = 1; step <= 32u; ++step)
        {
            if (step > stepCount)
            {
                break;
            }
            const float distancePixels = radiusPixels *
                (float(step) / float(stepCount));
            const float2 sampleUV =
                uv + direction * distancePixels * RenderSize.zw;
            if (!RVX_IsUVInBounds(sampleUV))
            {
                continue;
            }
            const float sampleDeviceDepth = AOInput.Load(
                int3(RVX_PixelCoord(sampleUV), 0));
            if (RVX_IsBackgroundDepth(sampleDeviceDepth))
            {
                continue;
            }
            const float3 samplePosition = RVX_ReconstructWorldPosition(
                sampleUV, sampleDeviceDepth, InverseViewProjection);
            const float3 sampleVector = samplePosition - centerPosition;
            const float distanceMeters = max(length(sampleVector), 1.0e-5);
            const float sampleViewDepth = RVX_ViewDepthMeters(samplePosition);
            const float normalizedDistance = saturate(
                (distanceMeters - HorizonParams.y) / falloffInterval);
            const float smoothDistance = normalizedDistance *
                normalizedDistance * (3.0 - 2.0 * normalizedDistance);
            const float distanceFalloff = 1.0 - smoothDistance;
            const float occluderHeight = centerViewDepth - sampleViewDepth -
                HorizonParams.w;
            const float depthHorizon = saturate(
                occluderHeight / distanceMeters);
            const float normalHorizon = saturate(
                dot(normal, RVX_SafeNormalize(sampleVector)));
            horizon = max(
                horizon, depthHorizon * distanceFalloff * normalHorizon);
        }
        occlusion += horizon;
    }

    const float ao = 1.0 - saturate(occlusion / float(directionCount));
    return RVX_SaturateAmbientOcclusion(ao);
}

float RVX_BilateralFilter(float2 uv, float2 axis)
{
    const float centerAO = AOInput.SampleLevel(LinearClampSampler, uv, 0.0).r;
    const float centerDeviceDepth = DeviceDepth.Load(
        int3(RVX_PixelCoord(uv), 0));
    if (RVX_IsBackgroundDepth(centerDeviceDepth))
    {
        return 1.0;
    }
    const float3 centerWorldPosition = RVX_ReconstructWorldPosition(
        uv, centerDeviceDepth, InverseViewProjection);
    const float centerDepth = RVX_ViewDepthMeters(centerWorldPosition);
    const float3 centerNormal = RVX_LoadNormal(uv);
    float weightedAO = centerAO;
    float weightSum = 1.0;
    const int spatialRadius = clamp((int)SamplingParams.z, 1, 8);
    const float radius = float(spatialRadius);
    const float normalThreshold = saturate(FilterParams.y);

    [loop]
    for (int offset = -8; offset <= 8; ++offset)
    {
        if (offset == 0 || abs(offset) > spatialRadius)
        {
            continue;
        }
        const float2 sampleUV =
            uv + axis * float(offset) * RenderSize.zw;
        if (!RVX_IsUVInBounds(sampleUV))
        {
            continue;
        }
        const float sampleDeviceDepth = DeviceDepth.Load(
            int3(RVX_PixelCoord(sampleUV), 0));
        if (RVX_IsBackgroundDepth(sampleDeviceDepth))
        {
            continue;
        }
        const float3 sampleWorldPosition = RVX_ReconstructWorldPosition(
            sampleUV, sampleDeviceDepth, InverseViewProjection);
        const float sampleDepth = RVX_ViewDepthMeters(sampleWorldPosition);
        const float3 sampleNormal = RVX_LoadNormal(sampleUV);
        const float normalDot = dot(centerNormal, sampleNormal);
        if (normalDot < normalThreshold)
        {
            continue;
        }
        const float sampleDistance = float(abs(offset));
        const float spatial = exp(
            -0.5 * sampleDistance * sampleDistance / (radius * radius));
        const float depthWeight = exp(
            -abs(sampleDepth - centerDepth) / max(FilterParams.x, 1.0e-4));
        const float normalWeight = saturate(
            (normalDot - normalThreshold) /
            max(1.0 - normalThreshold, 1.0e-4));
        const float weight = spatial * depthWeight * normalWeight;
        weightedAO += AOInput.SampleLevel(LinearClampSampler, sampleUV, 0.0).r * weight;
        weightSum += weight;
    }
    return RVX_SaturateAmbientOcclusion(weightedAO / max(weightSum, 1.0e-5));
}

float PSBilateralHorizontal(VSOutput input) : SV_TARGET0
{
    return RVX_BilateralFilter(saturate(input.TexCoord), float2(1.0, 0.0));
}

float PSBilateralVertical(VSOutput input) : SV_TARGET0
{
    return RVX_BilateralFilter(saturate(input.TexCoord), float2(0.0, 1.0));
}

float PSTemporal(VSOutput input) : SV_TARGET0
{
    const float2 uv = saturate(input.TexCoord);
    const float currentAO = AOInput.SampleLevel(LinearClampSampler, uv, 0.0).r;
    if (FilterParams.w <= 0.5)
    {
        return RVX_SaturateAmbientOcclusion(currentAO);
    }

    // Engine motion vectors are current NDC minus prior NDC. Convert to the
    // top-left UV convention exactly once before reprojecting candidate AO.
    const float2 velocityNDC = TemporalParams.w > 0.5
        ? Velocity.Load(int3(RVX_PixelCoord(uv), 0))
        : float2(0.0, 0.0);
    const float2 previousUV = uv - float2(velocityNDC.x * 0.5,
                                           -velocityNDC.y * 0.5) *
                                         max(TemporalParams.x, 0.0);
    const bool inBounds = all(previousUV >= 0.0) && all(previousUV <= 1.0);
    const float currentDeviceDepth = DeviceDepth.Load(
        int3(RVX_PixelCoord(uv), 0));
    const float historyDeviceDepth = HistoryDepth.Load(
        int3(RVX_PixelCoord(saturate(previousUV)), 0));
    const bool validCurrentDepth =
        !RVX_IsBackgroundDepth(currentDeviceDepth);
    const bool validHistoryDepth =
        !RVX_IsBackgroundDepth(historyDeviceDepth);
    const float3 currentWorldPosition = RVX_ReconstructWorldPosition(
        uv, currentDeviceDepth, InverseViewProjection);
    const float3 historyWorldPosition = RVX_ReconstructWorldPosition(
        saturate(previousUV), historyDeviceDepth,
        PreviousInverseViewProjection);
    const bool depthMatches = validCurrentDepth && validHistoryDepth &&
        distance(currentWorldPosition, historyWorldPosition) <=
            max(TemporalParams.y, 0.0);
    const float3 currentNormal = RVX_LoadNormal(uv);
    const float3 historyNormal = RVX_ReconstructNormalFromDepth(
        saturate(previousUV), true);
    const bool normalMatches = dot(currentNormal, historyNormal) >=
        saturate(TemporalParams.z);
    const float historyAO = AOHistory.SampleLevel(
        LinearClampSampler, saturate(previousUV), 0.0);
    float neighborhoodMinimum = currentAO;
    float neighborhoodMaximum = currentAO;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const float neighbor = AOInput.SampleLevel(
                LinearClampSampler,
                saturate(uv + float2(x, y) * RenderSize.zw), 0.0).r;
            neighborhoodMinimum = min(neighborhoodMinimum, neighbor);
            neighborhoodMaximum = max(neighborhoodMaximum, neighbor);
        }
    }
    const float clampedHistoryAO = clamp(
        historyAO, neighborhoodMinimum, neighborhoodMaximum);
    const float historyWeight = (inBounds && depthMatches && normalMatches)
        ? saturate(FilterParams.z)
        : 0.0;
    return RVX_SaturateAmbientOcclusion(
        lerp(currentAO, clampedHistoryAO, historyWeight));
}
