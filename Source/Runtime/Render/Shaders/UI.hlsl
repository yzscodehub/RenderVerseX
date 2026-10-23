// =============================================================================
// UI.hlsl - Runtime RHI UI overlay shader
// =============================================================================

cbuffer UIPushConstants : register(b0, space0)
{
    float2 TargetSize;
    float2 InvTargetSize;
};

Texture2D<float4> UITexture : register(t0, space0);
SamplerState UISampler : register(s1, space0);

struct VSInput
{
    float2 Position : POSITION;
    float2 TexCoord : TEXCOORD0;
    float4 Color : COLOR0;
};

struct VSOutput
{
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD0;
    float4 Color : COLOR0;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.Position = float4(input.Position, 0.0, 1.0);
    output.TexCoord = input.TexCoord;
    output.Color = input.Color;
    return output;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    return input.Color * UITexture.Sample(UISampler, input.TexCoord);
}
