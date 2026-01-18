// =============================================================================
// TexturedQuad.hlsl - Simple textured quad shader
// =============================================================================

// Texture and sampler
Texture2D g_Texture : register(t0);
SamplerState g_Sampler : register(s0);

// Vertex input
struct VSInput
{
    float3 Position : POSITION;
    float2 TexCoord : TEXCOORD;
};

// Vertex output / Pixel input
struct PSInput
{
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD0;
};

// =============================================================================
// Vertex Shader
// =============================================================================
PSInput VSMain(VSInput input)
{
    PSInput output;
    output.Position = float4(input.Position, 1.0);
    output.TexCoord = input.TexCoord;
    return output;
}

// =============================================================================
// Pixel Shader
// =============================================================================
float4 PSMain(PSInput input) : SV_TARGET
{
    float4 texColor = g_Texture.Sample(g_Sampler, input.TexCoord);
    return texColor;
}
