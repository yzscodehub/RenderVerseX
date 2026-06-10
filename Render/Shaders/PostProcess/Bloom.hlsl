// =============================================================================
// Bloom.hlsl - HDR fullscreen bloom path
// =============================================================================
//
// RQ25 implements a single-pass wide-kernel bloom approximation:
// - threshold bright pixels with a soft knee
// - sample a normalized center/ring1/ring2 fullscreen tap pattern
// - composite the thresholded contribution back into HDR scene color
//
// This shader does not implement a mip-chain downsample/upsample blur, but it
// gives the existing path a broader highlight response without changing the
// BloomPass pipeline or descriptor layout.
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
};

Texture2D<float4> InputTexture : register(t1, space0);
SamplerState LinearSampler : register(s2, space0);

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
    if (Intensity <= 0.0)
    {
        return scene;
    }

    float2 radius = InvTextureSize * max(Radius, 0.0);
    float2 ring2 = radius * 2.0;

    float3 bloom = SampleBloomThreshold(input.TexCoord) * 0.18;

    // ring1: axial taps keep bright highlights crisp near the source.
    bloom += SampleBloomThreshold(input.TexCoord + float2(-radius.x, 0.0)) * 0.12;
    bloom += SampleBloomThreshold(input.TexCoord + float2( radius.x, 0.0)) * 0.12;
    bloom += SampleBloomThreshold(input.TexCoord + float2(0.0, -radius.y)) * 0.12;
    bloom += SampleBloomThreshold(input.TexCoord + float2(0.0,  radius.y)) * 0.12;

    // ring1 diagonal taps soften the kernel into a rounder glow.
    bloom += SampleBloomThreshold(input.TexCoord + float2(-radius.x, -radius.y)) * 0.065;
    bloom += SampleBloomThreshold(input.TexCoord + float2( radius.x, -radius.y)) * 0.065;
    bloom += SampleBloomThreshold(input.TexCoord + float2(-radius.x,  radius.y)) * 0.065;
    bloom += SampleBloomThreshold(input.TexCoord + float2( radius.x,  radius.y)) * 0.065;

    // ring2: wider axial taps extend bloom without adding another pass.
    bloom += SampleBloomThreshold(input.TexCoord + float2(-ring2.x, 0.0)) * 0.02;
    bloom += SampleBloomThreshold(input.TexCoord + float2( ring2.x, 0.0)) * 0.02;
    bloom += SampleBloomThreshold(input.TexCoord + float2(0.0, -ring2.y)) * 0.02;
    bloom += SampleBloomThreshold(input.TexCoord + float2(0.0,  ring2.y)) * 0.02;

    return float4(scene.rgb + bloom * max(Intensity, 0.0), scene.a);
}
