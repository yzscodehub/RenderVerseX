// =============================================================================
// SSAO.hlsl - depth-only low-tier fullscreen SSAO post-process
// =============================================================================

cbuffer SSAOConstants : register(b0, space0)
{
    float2 TextureSize;
    float2 InvTextureSize;
    float Radius;
    float Intensity;
    float Bias;
    float Power;
    float SampleCount;
    float ReverseZ;
    float NormalFallback;
    float TemporalFallback;
    float4 Padding0;
};

Texture2D<float4> InputTexture : register(t1, space0);
SamplerState LinearSampler : register(s2, space0);
Texture2D<float> DepthTexture : register(t3, space0);

struct VSOutput
{
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD0;
};

VSOutput VSMain(uint vertexID : SV_VertexID)
{
    VSOutput output;

    float2 positions[3] = {
        float2(-1.0, -1.0),
        float2(-1.0,  3.0),
        float2( 3.0, -1.0)
    };

    float2 texCoords[3] = {
        float2(0.0, 1.0),
        float2(0.0, -1.0),
        float2(2.0, 1.0)
    };

    output.Position = float4(positions[vertexID], 0.0, 1.0);
    output.TexCoord = texCoords[vertexID];
    return output;
}

float DepthToLinear01(float depth)
{
    return ReverseZ > 0.5 ? 1.0 - depth : depth;
}

float2 SampleDirection(uint index)
{
    static const float2 directions[16] = {
        float2( 1.000,  0.000),
        float2( 0.707,  0.707),
        float2( 0.000,  1.000),
        float2(-0.707,  0.707),
        float2(-1.000,  0.000),
        float2(-0.707, -0.707),
        float2( 0.000, -1.000),
        float2( 0.707, -0.707),
        float2( 0.923,  0.382),
        float2( 0.382,  0.923),
        float2(-0.382,  0.923),
        float2(-0.923,  0.382),
        float2(-0.923, -0.382),
        float2(-0.382, -0.923),
        float2( 0.382, -0.923),
        float2( 0.923, -0.382)
    };
    return directions[min(index, 15u)];
}

float4 PSMain(VSOutput input) : SV_Target
{
    const float2 uv = saturate(input.TexCoord);
    float4 color = InputTexture.Sample(LinearSampler, uv);

    const float centerDepth = DepthToLinear01(DepthTexture.Sample(LinearSampler, uv));
    const uint sampleCount = max(1u, min((uint)SampleCount, 16u));
    const float safeRadius = max(Radius, 0.001);
    const float safeIntensity = saturate(Intensity);
    const float safeBias = max(Bias, 0.0);
    const float safePower = max(Power, 0.01);
    const float radiusPixels = max(1.0, safeRadius * 24.0);

    float occlusion = 0.0;
    [unroll]
    for (uint i = 0; i < 16u; ++i)
    {
        if (i >= sampleCount)
        {
            break;
        }

        const float ring = 0.35 + 0.65 * ((float)i + 1.0) / (float)sampleCount;
        const float2 offset = SampleDirection(i) * radiusPixels * ring * InvTextureSize;
        const float sampleDepth = DepthToLinear01(DepthTexture.Sample(LinearSampler, saturate(uv + offset)));
        const float depthDelta = centerDepth - sampleDepth - safeBias;
        occlusion += saturate(depthDelta * 48.0 * safeRadius);
    }

    occlusion = pow(saturate(occlusion / (float)sampleCount), safePower);
    const float ao = saturate(1.0 - occlusion * safeIntensity);
    color.rgb *= ao;
    return color;
}
