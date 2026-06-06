// =============================================================================
// DepthOnly.hlsl - Minimal depth-only vertex shader
// =============================================================================
//
// Descriptor sets:
//   set 0 / space0: frame data
//   set 1 / space1: object data
//
// Vertex inputs:
//   Slot 0: Position buffer (float3)
// =============================================================================

cbuffer ViewConstants : register(b0, space0)
{
    float4x4 ViewProjection;
    float3 CameraPosition;
    float Time;
    float3 LightDirection;
    float Padding;
};

cbuffer ObjectConstants : register(b0, space1)
{
    float4x4 World;
};

struct VSInput
{
    float3 Position : POSITION;
};

struct VSOutput
{
    float4 Position : SV_POSITION;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    float4 worldPosition = mul(World, float4(input.Position, 1.0));
    output.Position = mul(ViewProjection, worldPosition);
    return output;
}
