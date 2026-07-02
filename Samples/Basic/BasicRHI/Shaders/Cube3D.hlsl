// =============================================================================
// Cube3D.hlsl - 3D cube shader with basic lighting
// =============================================================================

cbuffer TransformCB : register(b0)
{
    float4x4 WorldViewProj;
    float4x4 World;
    float4 LightDir;
};

struct VSInput
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float4 Color : COLOR;
};

struct PSInput
{
    float4 Position : SV_POSITION;
    float3 WorldNormal : NORMAL;
    float4 Color : COLOR;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.Position = mul(WorldViewProj, float4(input.Position, 1.0));
    output.WorldNormal = mul((float3x3)World, input.Normal);
    output.Color = input.Color;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float3 normal = normalize(input.WorldNormal);
    float ndotl = max(dot(normal, -LightDir.xyz), 0.0);
    float lighting = 0.3 + ndotl * 0.7;
    return float4(input.Color.rgb * lighting, input.Color.a);
}
