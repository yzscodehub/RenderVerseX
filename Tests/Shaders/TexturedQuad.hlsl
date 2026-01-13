// =============================================================================
// Test Textured Quad Shader
// =============================================================================

cbuffer PerObject : register(b0)
{
    float4x4 WorldViewProj;
};

Texture2D AlbedoTexture : register(t0);
SamplerState LinearSampler : register(s0);

struct VSInput
{
    float3 position : POSITION;
    float2 texcoord : TEXCOORD;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.position = mul(WorldViewProj, float4(input.position, 1.0));
    output.texcoord = input.texcoord;
    return output;
}

float4 PSMain(PSInput input) : SV_Target
{
    return AlbedoTexture.Sample(LinearSampler, input.texcoord);
}
