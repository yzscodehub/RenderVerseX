// =============================================================================
// RayTracedReflectionComposite.hlsl
// =============================================================================
//
// Fullscreen alpha composite for ray-traced reflection results.
// Input RGB is the pure reflection signal; input A is the confidence/strength
// produced by the ray-tracing pass. The graphics pipeline keeps destination
// alpha unchanged while blending color by source alpha.
//
// Descriptor layout: b0 constants, t1 reflection texture, s2 sampler.
//
// =============================================================================

#include "../Include/FullscreenTriangle.hlsli"

cbuffer RayTracedReflectionCompositeConstants : register(b0, space0)
{
    float4 IntensityScale_Padding;
};

#define IntensityScale IntensityScale_Padding.x

Texture2D<float4> ReflectionTexture : register(t1, space0);
SamplerState LinearSampler : register(s2, space0);

struct VSOutput
{
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD0;
};

VSOutput VSMain(uint vertexID : SV_VertexID)
{
    VSOutput output;
    output.TexCoord = RVX_GetFullscreenTriangleTexCoord(vertexID);
    output.Position = RVX_GetFullscreenTrianglePosition(output.TexCoord);
    return output;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    const float4 reflection = ReflectionTexture.Sample(LinearSampler, input.TexCoord);
    const float alpha = saturate(reflection.a * max(IntensityScale, 0.0));
    return float4(max(reflection.rgb, 0.0), alpha);
}
