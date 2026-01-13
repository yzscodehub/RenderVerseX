// =============================================================================
// Triangle Shader with Transform Support
// =============================================================================

// Constant buffer for transformation matrix
cbuffer TransformCB : register(b0)
{
    row_major float4x4 worldMatrix;
    float4 tintColor;
};

struct VSInput
{
    float3 position : POSITION;
    float4 color : COLOR;
};

struct PSInput
{
    float4 position : SV_Position;
    float4 color : COLOR;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    float4 worldPos = mul(worldMatrix, float4(input.position, 1.0));
    // Keep z in valid clip range [0,1] to avoid clipping
    // Map z from [-1,1] to [0.1, 0.9] for safety margin
    output.position = float4(worldPos.xy, worldPos.z * 0.4 + 0.5, 1.0);
    output.color = input.color * tintColor;
    return output;
}

float4 PSMain(PSInput input) : SV_Target
{
    return input.color;
}
