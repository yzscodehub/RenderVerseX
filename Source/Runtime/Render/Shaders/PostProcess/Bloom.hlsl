// =============================================================================
// Bloom.hlsl - HDR fullscreen bloom path
// =============================================================================
//
// RQ27 implements a small multi-pass bloom pyramid:
// - copy scene color when bloom intensity is zero
// - extract bright pixels with a soft knee
// - downsample a fixed transient bloom pyramid
// - composite the source scene plus every blurred level in one opaque write
//
// The narrow exposure-aware fullscreen layout additionally binds the candidate
// state at t6, so thresholding uses the exact same E/P state as ToneMapping.
//
// =============================================================================

#include "../Include/FullscreenTriangle.hlsli"
#include "../Include/Exposure.hlsli"

cbuffer BloomConstants : register(b0, space0)
{
    float Threshold;
    float SoftKnee;
    float Intensity;
    float Radius;
    float2 TextureSize;
    float2 InvTextureSize;
    float4 Mode_Padding;
};

#define Mode Mode_Padding.x

Texture2D<float4> InputTexture : register(t1, space0);
SamplerState LinearSampler : register(s2, space0);
Texture2D<float4> BloomPyramid0 : register(t3, space0);
Texture2D<float4> BloomPyramid1 : register(t4, space0);
Texture2D<float4> BloomPyramid2 : register(t5, space0);
StructuredBuffer<RVXExposureState> ExposureState : register(t6, space0);

static const float BLOOM_MODE_COPY_SCENE = 0.0;
static const float BLOOM_MODE_EXTRACT = 1.0;
static const float BLOOM_MODE_DOWNSAMPLE = 2.0;
static const float BLOOM_MODE_COMPOSITE_PYRAMID = 3.0;
static const float BLOOM_PYRAMID_WEIGHTS[3] = {0.45, 0.35, 0.20};

float Luminance(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

float3 ApplySoftThreshold(float3 color)
{
    const RVXExposureState exposureState = ExposureState[0];
    const float exposureRatio = RVXExposureStateIsValid(exposureState)
        ? RVXExposureScale(exposureState) /
            RVXExposurePreExposure(exposureState)
        : 1.0;
    // Threshold is authored in physical scene exposure while HDR color is
    // pre-exposed with the prior producer scale. Keep returned bloom in the
    // original HDR domain so later composite remains energy-consistent.
    const float luma = Luminance(color) * exposureRatio;
    float knee = max(Threshold * SoftKnee, 0.0001);
    float soft = luma - Threshold + knee;
    soft = clamp(soft, 0.0, 2.0 * knee);
    soft = (soft * soft) / (4.0 * knee);

    float contribution = max(soft, luma - Threshold);
    contribution /= max(luma, 0.0001);
    return color * saturate(contribution);
}

float3 SampleBloomThreshold(Texture2D<float4> source, float2 uv)
{
    return ApplySoftThreshold(source.Sample(LinearSampler, uv).rgb);
}

float3 SampleBloomSource(Texture2D<float4> source, float2 uv)
{
    return source.Sample(LinearSampler, uv).rgb;
}

float3 ReconstructBloomPyramidLevel(Texture2D<float4> source, float2 uv)
{
    // Every level was already recursively low-pass filtered while the
    // pyramid was generated. Hardware bilinear reconstruction supplies the
    // correct full-resolution footprint here; applying the 13-tap generation
    // kernel again would spend 39 filtered fetches and blur the signal twice.
    return source.SampleLevel(LinearSampler, uv, 0.0).rgb;
}

float3 SampleBloomWideKernel(Texture2D<float4> source,
                             float2 uv,
                             bool thresholdSource)
{
    uint sourceWidth = 1;
    uint sourceHeight = 1;
    source.GetDimensions(sourceWidth, sourceHeight);
    const float2 inverseSourceSize = 1.0 / float2(
        max(sourceWidth, 1u), max(sourceHeight, 1u));
    float2 radius = inverseSourceSize * max(Radius, 0.0);
    float2 ring2 = radius * 2.0;

    float3 bloom = (thresholdSource ? SampleBloomThreshold(source, uv) : SampleBloomSource(source, uv)) * 0.18;

    // ring1: axial taps keep bright highlights crisp near the source.
    bloom += (thresholdSource ? SampleBloomThreshold(source, uv + float2(-radius.x, 0.0)) :
                                SampleBloomSource(source, uv + float2(-radius.x, 0.0))) * 0.12;
    bloom += (thresholdSource ? SampleBloomThreshold(source, uv + float2( radius.x, 0.0)) :
                                SampleBloomSource(source, uv + float2( radius.x, 0.0))) * 0.12;
    bloom += (thresholdSource ? SampleBloomThreshold(source, uv + float2(0.0, -radius.y)) :
                                SampleBloomSource(source, uv + float2(0.0, -radius.y))) * 0.12;
    bloom += (thresholdSource ? SampleBloomThreshold(source, uv + float2(0.0,  radius.y)) :
                                SampleBloomSource(source, uv + float2(0.0,  radius.y))) * 0.12;

    // ring1 diagonal taps soften the kernel into a rounder glow.
    bloom += (thresholdSource ? SampleBloomThreshold(source, uv + float2(-radius.x, -radius.y)) :
                                SampleBloomSource(source, uv + float2(-radius.x, -radius.y))) * 0.065;
    bloom += (thresholdSource ? SampleBloomThreshold(source, uv + float2( radius.x, -radius.y)) :
                                SampleBloomSource(source, uv + float2( radius.x, -radius.y))) * 0.065;
    bloom += (thresholdSource ? SampleBloomThreshold(source, uv + float2(-radius.x,  radius.y)) :
                                SampleBloomSource(source, uv + float2(-radius.x,  radius.y))) * 0.065;
    bloom += (thresholdSource ? SampleBloomThreshold(source, uv + float2( radius.x,  radius.y)) :
                                SampleBloomSource(source, uv + float2( radius.x,  radius.y))) * 0.065;

    // ring2: wider axial taps extend bloom across the transient pyramid.
    bloom += (thresholdSource ? SampleBloomThreshold(source, uv + float2(-ring2.x, 0.0)) :
                                SampleBloomSource(source, uv + float2(-ring2.x, 0.0))) * 0.02;
    bloom += (thresholdSource ? SampleBloomThreshold(source, uv + float2( ring2.x, 0.0)) :
                                SampleBloomSource(source, uv + float2( ring2.x, 0.0))) * 0.02;
    bloom += (thresholdSource ? SampleBloomThreshold(source, uv + float2(0.0, -ring2.y)) :
                                SampleBloomSource(source, uv + float2(0.0, -ring2.y))) * 0.02;
    bloom += (thresholdSource ? SampleBloomThreshold(source, uv + float2(0.0,  ring2.y)) :
                                SampleBloomSource(source, uv + float2(0.0,  ring2.y))) * 0.02;

    return bloom;
}

struct VSOutput
{
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD0;
};

VSOutput VSMain(uint vertexID : SV_VertexID)
{
    VSOutput output;
    output.TexCoord = RVX_GetFullscreenTriangleTexCoord(vertexID);
    output.Position = RVX_GetFullscreenTrianglePosition(output.TexCoord);
    return output;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    float4 scene = InputTexture.Sample(LinearSampler, input.TexCoord);
    if (Mode < BLOOM_MODE_EXTRACT - 0.5)
    {
        return scene;
    }

    if (Mode < BLOOM_MODE_DOWNSAMPLE - 0.5)
    {
        return float4(
            SampleBloomWideKernel(InputTexture, input.TexCoord, true),
            scene.a);
    }

    if (Mode < BLOOM_MODE_COMPOSITE_PYRAMID - 0.5)
    {
        return float4(
            SampleBloomWideKernel(InputTexture, input.TexCoord, false),
            scene.a);
    }

    const float3 bloom =
        ReconstructBloomPyramidLevel(BloomPyramid0, input.TexCoord) *
            BLOOM_PYRAMID_WEIGHTS[0] +
        ReconstructBloomPyramidLevel(BloomPyramid1, input.TexCoord) *
            BLOOM_PYRAMID_WEIGHTS[1] +
        ReconstructBloomPyramidLevel(BloomPyramid2, input.TexCoord) *
            BLOOM_PYRAMID_WEIGHTS[2];
    return float4(scene.rgb + bloom * max(Intensity, 0.0), scene.a);
}
