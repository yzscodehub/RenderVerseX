// =============================================================================
// CameraVelocity.hlsl
// =============================================================================
//
// Fullscreen camera/depth motion-vector generation for temporal reprojection.
// Output is current-NDC minus previous-NDC velocity in RG16F. Consumers convert
// that NDC velocity to UV-space delta as float2(x * 0.5, -y * 0.5).
//
// Descriptor layout: b0 constants, t1 depth texture, s2 sampler.
//
// =============================================================================

cbuffer CameraVelocityConstants : register(b0, space0)
{
    float4x4 InverseViewProjection;
    float4x4 PreviousViewProjection;
    float4 OutputSizeAndInvSize; // xy: size, zw: inv size
    float4 VelocityParams;       // x: previous valid, y: unused, z: background depth threshold, w: unused
};

Texture2D<float> DepthTexture : register(t1, space0);
SamplerState PointSampler : register(s2, space0);

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

float2 PSMain(VSOutput input) : SV_TARGET
{
    const uint2 pixel = min(uint2(input.Position.xy), uint2(OutputSizeAndInvSize.xy) - 1u);
    const float depth = DepthTexture.Load(int3(pixel, 0)).r;
    if (VelocityParams.x <= 0.5f || depth >= VelocityParams.z)
    {
        return 0.0f;
    }

    const float2 uv = (float2(pixel) + 0.5f) * OutputSizeAndInvSize.zw;
    const float2 currentNdc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);

    float4 world = mul(InverseViewProjection, float4(currentNdc, depth, 1.0f));
    world.xyz /= abs(world.w) > 1.0e-6f ? world.w : 1.0f;

    float4 previousClip = mul(PreviousViewProjection, float4(world.xyz, 1.0f));
    if (abs(previousClip.w) <= 1.0e-6f)
    {
        return 0.0f;
    }

    const float2 previousNdc = previousClip.xy / previousClip.w;
    return currentNdc - previousNdc;
}