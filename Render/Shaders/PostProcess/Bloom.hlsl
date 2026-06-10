// =============================================================================
// Bloom.hlsl - HDR fullscreen bloom path
// =============================================================================
//
// RQ27 implements a small multi-pass bloom pyramid:
// - copy scene color when bloom intensity is zero
// - extract bright pixels with a soft knee
// - downsample a fixed transient bloom pyramid
// - additively composite blurred levels back into HDR scene color
//
// The descriptor layout remains the shared fullscreen post-process layout:
// b0 constants, t1 input texture, s2 sampler.
//
// =============================================================================

cbuffer BloomConstants : register(b0, space0)
{
    float Threshold;
    float SoftKnee;
    float Intensity;
    float Radius;
    float2 TextureSize;
    float2 InvTextureSize;
    float Mode;
    float3 Padding;
};

Texture2D<float4> InputTexture : register(t1, space0);
SamplerState LinearSampler : register(s2, space0);

static const float BLOOM_MODE_COPY_SCENE = 0.0;
static const float BLOOM_MODE_EXTRACT = 1.0;
static const float BLOOM_MODE_DOWNSAMPLE = 2.0;
static const float BLOOM_MODE_COMPOSITE_ADDITIVE = 3.0;

float Luminance(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

float3 ApplySoftThreshold(float3 color)
{
    float luma = Luminance(color);
    float knee = max(Threshold * SoftKnee, 0.0001);
    float soft = luma - Threshold + knee;
    soft = clamp(soft, 0.0, 2.0 * knee);
    soft = (soft * soft) / (4.0 * knee);

    float contribution = max(soft, luma - Threshold);
    contribution /= max(luma, 0.0001);
    return color * saturate(contribution);
}

float3 SampleBloomThreshold(float2 uv)
{
    return ApplySoftThreshold(InputTexture.Sample(LinearSampler, uv).rgb);
}

float3 SampleBloomSource(float2 uv)
{
    return InputTexture.Sample(LinearSampler, uv).rgb;
}

float3 SampleBloomWideKernel(float2 uv, bool thresholdSource)
{
    float2 radius = InvTextureSize * max(Radius, 0.0);
    float2 ring2 = radius * 2.0;

    float3 bloom = (thresholdSource ? SampleBloomThreshold(uv) : SampleBloomSource(uv)) * 0.18;

    // ring1: axial taps keep bright highlights crisp near the source.
    bloom += (thresholdSource ? SampleBloomThreshold(uv + float2(-radius.x, 0.0)) :
                                SampleBloomSource(uv + float2(-radius.x, 0.0))) * 0.12;
    bloom += (thresholdSource ? SampleBloomThreshold(uv + float2( radius.x, 0.0)) :
                                SampleBloomSource(uv + float2( radius.x, 0.0))) * 0.12;
    bloom += (thresholdSource ? SampleBloomThreshold(uv + float2(0.0, -radius.y)) :
                                SampleBloomSource(uv + float2(0.0, -radius.y))) * 0.12;
    bloom += (thresholdSource ? SampleBloomThreshold(uv + float2(0.0,  radius.y)) :
                                SampleBloomSource(uv + float2(0.0,  radius.y))) * 0.12;

    // ring1 diagonal taps soften the kernel into a rounder glow.
    bloom += (thresholdSource ? SampleBloomThreshold(uv + float2(-radius.x, -radius.y)) :
                                SampleBloomSource(uv + float2(-radius.x, -radius.y))) * 0.065;
    bloom += (thresholdSource ? SampleBloomThreshold(uv + float2( radius.x, -radius.y)) :
                                SampleBloomSource(uv + float2( radius.x, -radius.y))) * 0.065;
    bloom += (thresholdSource ? SampleBloomThreshold(uv + float2(-radius.x,  radius.y)) :
                                SampleBloomSource(uv + float2(-radius.x,  radius.y))) * 0.065;
    bloom += (thresholdSource ? SampleBloomThreshold(uv + float2( radius.x,  radius.y)) :
                                SampleBloomSource(uv + float2( radius.x,  radius.y))) * 0.065;

    // ring2: wider axial taps extend bloom across the transient pyramid.
    bloom += (thresholdSource ? SampleBloomThreshold(uv + float2(-ring2.x, 0.0)) :
                                SampleBloomSource(uv + float2(-ring2.x, 0.0))) * 0.02;
    bloom += (thresholdSource ? SampleBloomThreshold(uv + float2( ring2.x, 0.0)) :
                                SampleBloomSource(uv + float2( ring2.x, 0.0))) * 0.02;
    bloom += (thresholdSource ? SampleBloomThreshold(uv + float2(0.0, -ring2.y)) :
                                SampleBloomSource(uv + float2(0.0, -ring2.y))) * 0.02;
    bloom += (thresholdSource ? SampleBloomThreshold(uv + float2(0.0,  ring2.y)) :
                                SampleBloomSource(uv + float2(0.0,  ring2.y))) * 0.02;

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
    output.TexCoord = float2((vertexID << 1) & 2, vertexID & 2);
    output.Position = float4(output.TexCoord * 2.0 - 1.0, 0.0, 1.0);
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
        return float4(SampleBloomWideKernel(input.TexCoord, true), scene.a);
    }

    float3 bloom = SampleBloomWideKernel(input.TexCoord, false);
    if (Mode < BLOOM_MODE_COMPOSITE_ADDITIVE - 0.5)
    {
        return float4(bloom, scene.a);
    }

    return float4(bloom * max(Intensity, 0.0), 0.0);
}
