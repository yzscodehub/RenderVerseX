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
    float4 JitterPixels;     // current xy / previous xy projection perturbation
    float4 ResolvePolicy;    // x: committed producer exposure; y: device depth
    float4 DepthProjection;  // P22/P32/P23/P33
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
    const float g = color.x + 0.5 * color.z;
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
    float reactiveMaximum;
};

CurrentColorNeighborhood SampleCurrentColorNeighborhood(uint2 centerPixel,
                                                        uint2 extent,
                                                        float2 rasterOffsetPixels)
{
    CurrentColorNeighborhood neighborhood;
    neighborhood.center = float4(0.0, 0.0, 0.0, 0.0);
    neighborhood.minimumYCoCg = float3(1.0e20, 1.0e20, 1.0e20);
    neighborhood.maximumYCoCg = float3(-1.0e20, -1.0e20, -1.0e20);
    float3 axialSumRGB = float3(0.0, 0.0, 0.0);
    float4 reconstructed = float4(0.0, 0.0, 0.0, 0.0);
    float3 rawCenter = float3(0.0, 0.0, 0.0);
    neighborhood.reactiveMaximum = 0.0;

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
            // Reconstruct the stable grid even if a reactive pixel rejects
            // history. Reuse the same nine current-color texels.
            const float2 weightXY = max(
                1.0 - abs(float2(x, y) - rasterOffsetPixels), 0.0);
            const float weight = weightXY.x * weightXY.y;
            reconstructed += float4(sampleRGB, SanitizeScalar(rawSample.a)) * weight;
            if (weight > 0.0)
            {
                const float mask = ReactiveMask.Load(int3(samplePixel, 0));
                neighborhood.reactiveMaximum = max(neighborhood.reactiveMaximum,
                    (mask == mask && abs(mask) < 1.0e20) ? saturate(mask) : 1.0);
            }
            neighborhood.minimumYCoCg = min(
                neighborhood.minimumYCoCg, sampleYCoCg);
            neighborhood.maximumYCoCg = max(
                neighborhood.maximumYCoCg, sampleYCoCg);

            if (x == 0 && y == 0)
            {
                rawCenter = sampleRGB;
            }
            if (abs(x) + abs(y) == 1)
            {
                axialSumRGB += sampleRGB;
            }
        }
    }

    neighborhood.center = reconstructed;
    neighborhood.axialAverageRGB = axialSumRGB * 0.25 +
        reconstructed.rgb - rawCenter;
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
    // Projection column-2 jitter moves raster NDC by -jitter (clip.w=-z).
    // Top-left UV reverses NDC Y. The output/history grid itself is unjittered.
    const float2 rasterOffsetPixels = float2(-JitterPixels.x, JitterPixels.y);
    const float2 previousRasterOffsetPixels = float2(-JitterPixels.z, JitterPixels.w);
    const bool opaqueRefraction = ResolvePolicy.x > 0.5;
    const CurrentColorNeighborhood neighborhood =
        SampleCurrentColorNeighborhood(pixel, extent, rasterOffsetPixels);
    const float4 currentSample = neighborhood.center;
    const float3 current = SanitizeColor(currentSample.rgb);
    const float rawDepth = CurrentDepth.Load(int3(pixel, 0));
    const float depthDenominator = rawDepth * DepthProjection.z - DepthProjection.x;
    const float currentDepth = ResolvePolicy.y > 0.5
        ? (abs(depthDenominator) > 1.0e-8
            ? abs((DepthProjection.y - rawDepth * DepthProjection.w) / depthDenominator)
            : 0.0)
        : rawDepth;
    const float reactive = saturate(
        neighborhood.reactiveMaximum * max(ResolveParams.y, 0.0));
    float3 resolved = current;
    if (ResolveParams.w > 0.5)
    {
        // Engine velocity is current-NDC minus previous-NDC. Convert it to
        // top-left UV delta once: x maps directly, while UV y opposes NDC y.
        // Velocity includes raster jitter; stored candidates now use the
        // stable grid. Remove only jitter before applying genuine view motion.
        const float2 velocityNDC = Velocity.Load(int3(pixel, 0));
        const float2 velocityUV = float2(velocityNDC.x * 0.5, -velocityNDC.y * 0.5);
        const float2 sceneVelocityUV = velocityUV -
            (rasterOffsetPixels - previousRasterOffsetPixels) * RenderSize.zw;
        const float2 previousUV = uv - sceneVelocityUV * FeedbackParams.z;
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
            ? (opaqueRefraction
                ? RVXExposureProducerScale(exposureState) / RVXExposurePreExposure(exposureState)
                : RVXExposurePreExposure(exposureState) / RVXExposurePreviousPreExposure(exposureState))
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
            length(sceneVelocityUV) * max(FeedbackParams.w, 0.0));
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
        SanitizeColor(resolved),
        SanitizeScalar(currentSample.a));
    output.HistoryColorCandidate = float4(SanitizeColor(resolved), SanitizeScalar(currentSample.a));
    output.HistoryDepthCandidate =
        (currentDepth == currentDepth && abs(currentDepth) < 1.0e20)
            ? currentDepth
            : 0.0;
    return output;
}
