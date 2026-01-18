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

// =============================================================================
// Vertex Shader
// =============================================================================
PSInput VSMain(VSInput input)
{
    PSInput output;
    output.Position = mul(WorldViewProj, float4(input.Position, 1.0));
    output.WorldNormal = mul((float3x3)World, input.Normal);
    output.Color = input.Color;
    return output;
}

// =============================================================================
// Pixel Shader with simple diffuse lighting
// =============================================================================
float4 PSMain(PSInput input) : SV_TARGET
{
    // Normalize the interpolated normal
    float3 normal = normalize(input.WorldNormal);
    
    // Simple diffuse lighting
    float ndotl = max(dot(normal, -LightDir.xyz), 0.0);
    float ambient = 0.3;
    float diffuse = ndotl * 0.7;
    
    float3 lighting = (ambient + diffuse);
    float3 finalColor = input.Color.rgb * lighting;
    
    return float4(finalColor, input.Color.a);
}
