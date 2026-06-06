// =============================================================================
// Bloom.hlsl - Minimum fullscreen bloom path
// =============================================================================
//
// R9e implements a deliberately small one-pass bloom approximation:
// - threshold bright pixels with a soft knee
// - sample a tiny fullscreen-neighborhood tap pattern
// - add the thresholded contribution back to the scene color
//
// This shader does not implement a mip-chain downsample/upsample blur.
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

    float2 offset = InvTextureSize * max(Radius, 0.0);
    float3 bloom = ApplySoftThreshold(scene.rgb) * 0.5;
    bloom += ApplySoftThreshold(InputTexture.Sample(LinearSampler, input.TexCoord + float2(-offset.x, 0.0)).rgb) * 0.125;
    bloom += ApplySoftThreshold(InputTexture.Sample(LinearSampler, input.TexCoord + float2( offset.x, 0.0)).rgb) * 0.125;
    bloom += ApplySoftThreshold(InputTexture.Sample(LinearSampler, input.TexCoord + float2(0.0, -offset.y)).rgb) * 0.125;
    bloom += ApplySoftThreshold(InputTexture.Sample(LinearSampler, input.TexCoord + float2(0.0,  offset.y)).rgb) * 0.125;

    return float4(scene.rgb + bloom * max(Intensity, 0.0), scene.a);
}
