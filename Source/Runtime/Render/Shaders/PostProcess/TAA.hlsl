// =============================================================================
// TAA.hlsl - Temporal resolve with fused history candidates
// =============================================================================
//
// The resolve consumes current HDR color, committed history, current/previous
// linear depth, velocity and the renderer reactive mask. The same draw emits
// post-process output plus immutable color/depth history candidates through
// MRT; presentation code promotes those candidates only after exact Present.
// =============================================================================

#include "../Include/FullscreenTriangle.hlsli"
#include "../Include/Exposure.hlsli"

cbuffer TAAConstants : register(b0, space0)
{
    float4 RenderSize;       // xy = extent, zw = inverse extent
    float4 FeedbackParams;   // min, max, motion scale, velocity rejection
    float4 ResolveParams;    // depth reject scale, reactive scale, sharpness, history valid
};

Texture2D<float4> CurrentColor : register(t1, space0);
Texture2D<float4> HistoryColor : register(t2, space0);
Texture2D<float> CurrentDepth : register(t3, space0);
Texture2D<float> HistoryDepth : register(t4, space0);
Texture2D<float2> Velocity : register(t5, space0);
Texture2D<float> ReactiveMask : register(t6, space0);
SamplerState LinearClampSampler : register(s7, space0);
StructuredBuffer<RVXExposureState> ExposureState : register(t8, space0);

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

float3 RGBToYCoCg(float3 color)
{
    const float y = dot(color, float3(0.25, 0.5, 0.25));
    const float co = color.r - color.b;
    const float cg = color.g - 0.5 * (color.r + color.b);
    return float3(y, co, cg);
}

float3 YCoCgToRGB(float3 color)
{
    const float r = color.x + 0.5 * color.y - 0.5 * color.z;
    const float g = color.x + color.z;
    const float b = color.x - 0.5 * color.y - 0.5 * color.z;
    return float3(r, g, b);
}

bool IsFiniteColor(float3 color)
{
    // The equality test rejects NaN and the magnitude bound rejects infinities
    // without relying on backend-specific intrinsic availability.
    return all(color == color) && all(abs(color) < 1.0e20);
}

float3 SanitizeColor(float3 color)
{
    return IsFiniteColor(color) ? max(color, 0.0) : float3(0.0, 0.0, 0.0);
}

float SanitizeScalar(float value)
{
    return (value == value && abs(value) < 1.0e20) ? value : 0.0;
}

struct CurrentColorNeighborhood
{
    float4 center;
    float3 minimumYCoCg;
    float3 maximumYCoCg;
    float3 axialAverageRGB;
};

CurrentColorNeighborhood SampleCurrentColorNeighborhood(uint2 centerPixel,
                                                        uint2 extent)
{
    CurrentColorNeighborhood neighborhood;
    neighborhood.center = float4(0.0, 0.0, 0.0, 0.0);
    neighborhood.minimumYCoCg = float3(1.0e20, 1.0e20, 1.0e20);
    neighborhood.maximumYCoCg = float3(-1.0e20, -1.0e20, -1.0e20);
    float3 axialSumRGB = float3(0.0, 0.0, 0.0);

    // Keep the traversal row-major so all resolve consumers share exactly one
    // deterministic 3x3 current-color window.
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const int2 samplePixel = clamp(
                int2(centerPixel) + int2(x, y),
                int2(0, 0), int2(extent) - int2(1, 1));
            const float4 rawSample = CurrentColor.Load(
                int3(samplePixel, 0));
            const float3 sampleRGB = SanitizeColor(rawSample.rgb);
            const float3 sampleYCoCg = RGBToYCoCg(sampleRGB);
            neighborhood.minimumYCoCg = min(
                neighborhood.minimumYCoCg, sampleYCoCg);
            neighborhood.maximumYCoCg = max(
                neighborhood.maximumYCoCg, sampleYCoCg);

            if (x == 0 && y == 0)
            {
                neighborhood.center = rawSample;
            }
            if (abs(x) + abs(y) == 1)
            {
                axialSumRGB += sampleRGB;
            }
        }
    }

    neighborhood.axialAverageRGB = axialSumRGB * 0.25;
    return neighborhood;
}

struct TAAResolveOutput
{
    float4 ResolvedColor : SV_TARGET0;
    float4 HistoryColorCandidate : SV_TARGET1;
    float HistoryDepthCandidate : SV_TARGET2;
};

TAAResolveOutput PSResolve(VSOutput input)
{
    const uint2 extent = max(uint2(RenderSize.xy), uint2(1u, 1u));
    const uint2 pixel = min(uint2(input.Position.xy), extent - 1u);
    const float2 uv = input.TexCoord;
    const CurrentColorNeighborhood neighborhood =
        SampleCurrentColorNeighborhood(pixel, extent);
    const float4 currentSample = neighborhood.center;
    const float3 current = SanitizeColor(currentSample.rgb);
    const float currentDepth = CurrentDepth.Load(int3(pixel, 0));
    const float reactive = saturate(
        ReactiveMask.Load(int3(pixel, 0)) * max(ResolveParams.y, 0.0));
    float3 resolved = current;
    if (ResolveParams.w > 0.5)
    {
        // Engine velocity is current-NDC minus previous-NDC. Convert it to
        // top-left UV delta once: x maps directly, while UV y opposes NDC y.
        // Current/previous velocity matrices are already jittered, so adding
        // a second correction here would double-apply the sample offset.
        const float2 velocityNDC = Velocity.Load(int3(pixel, 0));
        const float2 velocityUV = float2(velocityNDC.x * 0.5, -velocityNDC.y * 0.5);
        const float2 previousUV = uv - velocityUV * FeedbackParams.z;
        const bool previousUVInBounds =
            all(previousUV >= 0.0) && all(previousUV <= 1.0);

        const float previousDepth = HistoryDepth.SampleLevel(
            LinearClampSampler, saturate(previousUV), 0.0);
        const float depthTolerance = max(
            0.0005,
            max(abs(currentDepth), abs(previousDepth)) *
                max(ResolveParams.x, 0.0));
        const bool depthMatches =
            abs(currentDepth - previousDepth) <= depthTolerance;

        const RVXExposureState exposureState = ExposureState[0];
        const float historyRescale = RVXExposureStateIsValid(exposureState)
            ? RVXExposurePreExposure(exposureState) /
                RVXExposurePreviousPreExposure(exposureState)
            : 1.0;
        const float3 historyPreExposed = SanitizeColor(
            HistoryColor.SampleLevel(
                LinearClampSampler, saturate(previousUV), 0.0).rgb) *
            historyRescale;
        float3 historyYCoCg = RGBToYCoCg(historyPreExposed);
        historyYCoCg = clamp(
            historyYCoCg,
            neighborhood.minimumYCoCg,
            neighborhood.maximumYCoCg);
        const float3 history = SanitizeColor(YCoCgToRGB(historyYCoCg));

        const float velocityRejection = saturate(
            length(velocityUV) * max(FeedbackParams.w, 0.0));
        float historyWeight = lerp(
            saturate(FeedbackParams.y),
            saturate(FeedbackParams.x),
            velocityRejection);
        historyWeight *= 1.0 - reactive;
        historyWeight *=
            (previousUVInBounds && depthMatches && IsFiniteColor(history))
                ? 1.0
                : 0.0;
        resolved = lerp(current, history, historyWeight);
    }
    const float sharpenStrength = saturate(ResolveParams.z) * (1.0 - reactive);
    resolved = max(
        resolved +
            (resolved - neighborhood.axialAverageRGB) * sharpenStrength,
        0.0);
    TAAResolveOutput output;
    output.ResolvedColor = float4(
        SanitizeColor(resolved), SanitizeScalar(currentSample.a));
    output.HistoryColorCandidate = output.ResolvedColor;
    output.HistoryDepthCandidate =
        (currentDepth == currentDepth && abs(currentDepth) < 1.0e20)
            ? currentDepth
            : 0.0;
    return output;
}
