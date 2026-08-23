// =============================================================================
// TAA.hlsl - Temporal resolve and history store
// =============================================================================
//
// The resolve consumes current HDR color, committed history, current/previous
// linear depth, velocity and the renderer reactive mask. The history store is
// deliberately a separate MRT pass so presentation code can commit the
// candidate only after the exact frame was presented successfully.
// =============================================================================

#include "../Include/FullscreenTriangle.hlsli"

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

float3 SampleCurrentNeighborhoodMin(float2 uv)
{
    float3 minValue = RGBToYCoCg(SanitizeColor(CurrentColor.SampleLevel(LinearClampSampler, uv, 0.0).rgb));
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const float2 offset = float2(x, y) * RenderSize.zw;
            minValue = min(minValue, RGBToYCoCg(SanitizeColor(
                CurrentColor.SampleLevel(LinearClampSampler, uv + offset, 0.0).rgb)));
        }
    }
    return minValue;
}

float3 SampleCurrentNeighborhoodMax(float2 uv)
{
    float3 maxValue = RGBToYCoCg(SanitizeColor(CurrentColor.SampleLevel(LinearClampSampler, uv, 0.0).rgb));
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const float2 offset = float2(x, y) * RenderSize.zw;
            maxValue = max(maxValue, RGBToYCoCg(SanitizeColor(
                CurrentColor.SampleLevel(LinearClampSampler, uv + offset, 0.0).rgb)));
        }
    }
    return maxValue;
}

float4 PSResolve(VSOutput input) : SV_TARGET0
{
    const float2 uv = input.TexCoord;
    const float4 currentSample = CurrentColor.SampleLevel(LinearClampSampler, uv, 0.0);
    const float3 current = SanitizeColor(currentSample.rgb);
    const float reactive = saturate(ReactiveMask.SampleLevel(LinearClampSampler, uv, 0.0) *
                                      max(ResolveParams.y, 0.0));
    float3 resolved = current;
    if (ResolveParams.w > 0.5)
    {
        // Engine velocity is current-NDC minus previous-NDC. Convert it to
        // top-left UV delta once: x maps directly, while UV y opposes NDC y.
        // Current/previous velocity matrices are already jittered, so adding
        // a second correction here would double-apply the sample offset.
        const float2 velocityNDC = Velocity.SampleLevel(LinearClampSampler, uv, 0.0);
        const float2 velocityUV = float2(velocityNDC.x * 0.5, -velocityNDC.y * 0.5);
        const float2 previousUV = uv - velocityUV * FeedbackParams.z;
        const bool previousUVInBounds =
            all(previousUV >= 0.0) && all(previousUV <= 1.0);

        const float currentDepth = CurrentDepth.SampleLevel(LinearClampSampler, uv, 0.0);
        const float previousDepth = HistoryDepth.SampleLevel(
            LinearClampSampler, saturate(previousUV), 0.0);
        const float depthTolerance = max(
            0.0005,
            max(abs(currentDepth), abs(previousDepth)) *
                max(ResolveParams.x, 0.0));
        const bool depthMatches =
            abs(currentDepth - previousDepth) <= depthTolerance;

        const float3 neighborhoodMin = SampleCurrentNeighborhoodMin(uv);
        const float3 neighborhoodMax = SampleCurrentNeighborhoodMax(uv);
        float3 historyYCoCg = RGBToYCoCg(SanitizeColor(
            HistoryColor.SampleLevel(
                LinearClampSampler, saturate(previousUV), 0.0).rgb));
        historyYCoCg = clamp(historyYCoCg, neighborhoodMin, neighborhoodMax);
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
    const float3 north = SanitizeColor(CurrentColor.SampleLevel(
        LinearClampSampler, uv + float2(0.0, -RenderSize.w), 0.0).rgb);
    const float3 south = SanitizeColor(CurrentColor.SampleLevel(
        LinearClampSampler, uv + float2(0.0, RenderSize.w), 0.0).rgb);
    const float3 east = SanitizeColor(CurrentColor.SampleLevel(
        LinearClampSampler, uv + float2(RenderSize.z, 0.0), 0.0).rgb);
    const float3 west = SanitizeColor(CurrentColor.SampleLevel(
        LinearClampSampler, uv + float2(-RenderSize.z, 0.0), 0.0).rgb);
    const float3 localAverage = 0.25 * (north + south + east + west);
    const float sharpenStrength = saturate(ResolveParams.z) * (1.0 - reactive);
    resolved = max(resolved + (resolved - localAverage) * sharpenStrength, 0.0);
    return float4(SanitizeColor(resolved), SanitizeScalar(currentSample.a));
}

struct HistoryStoreOutput
{
    float4 Color : SV_TARGET0;
    float Depth : SV_TARGET1;
};

HistoryStoreOutput PSStore(VSOutput input)
{
    HistoryStoreOutput output;
    const float4 color = CurrentColor.SampleLevel(LinearClampSampler, input.TexCoord, 0.0);
    output.Color = float4(SanitizeColor(color.rgb), SanitizeScalar(color.a));
    const float depth = CurrentDepth.SampleLevel(LinearClampSampler, input.TexCoord, 0.0);
    output.Depth = (depth == depth && abs(depth) < 1.0e20) ? depth : 0.0;
    return output;
}
