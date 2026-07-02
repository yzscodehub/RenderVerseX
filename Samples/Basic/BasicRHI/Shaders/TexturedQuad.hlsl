// =============================================================================
// TexturedQuad.hlsl - Simple textured quad shader
// =============================================================================

Texture2D g_Texture : register(t0);
SamplerState g_Sampler : register(s0);

struct VSInput
{
    float3 Position : POSITION;
    float2 TexCoord : TEXCOORD;
};

struct PSInput
{
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD0;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.Position = float4(input.Position, 1.0);
    output.TexCoord = input.TexCoord;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    return g_Texture.Sample(g_Sampler, input.TexCoord);
}
