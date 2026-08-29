// Resolve graph-owned weighted-blended translucency into a distinct scene-HDR
// target. Inputs are integer loaded so the descriptor ABI needs no sampler and
// cannot accidentally filter across render-target texels.

#include "../Include/FullscreenTriangle.hlsli"
#include "WeightedBlendedOIT.hlsli"

Texture2D<float4> SourceSceneHDR : register(t0, space0);
Texture2D<float4> OITAccumulation : register(t1, space0);
Texture2D<float> OITRevealage : register(t2, space0);

struct CompositeVSOutput
{
    float4 Position : SV_Position;
    float2 UV : TEXCOORD0;
};

CompositeVSOutput VSMain(uint vertexId : SV_VertexID)
{
    CompositeVSOutput output;
    output.UV = RVX_GetFullscreenTriangleTexCoord(vertexId);
    output.Position = RVX_GetFullscreenTrianglePosition(output.UV);
    return output;
}

struct CompositeOutput
{
    float4 SceneHDR : SV_Target0;
    float ReactiveMask : SV_Target1;
};

CompositeOutput PSMain(CompositeVSOutput input)
{
    uint width;
    uint height;
    SourceSceneHDR.GetDimensions(width, height);
    const uint2 extent = max(uint2(width, height), uint2(1u, 1u));
    const uint2 pixel = min(uint2(input.Position.xy), extent - 1u);

    const float4 source = SourceSceneHDR.Load(int3(pixel, 0));
    const float4 accumulation = OITAccumulation.Load(int3(pixel, 0));
    const float revealage = saturate(OITRevealage.Load(int3(pixel, 0)));
    const float coverage = 1.0f - revealage;
    const float reciprocalWeight = rcp(max(
        accumulation.a,
        RVX_WEIGHTED_OIT_ACCUMULATION_EPSILON));
    const float3 resolved = min(
        max(accumulation.rgb * reciprocalWeight, 0.0f.xxx),
        RVX_WEIGHTED_OIT_MAX_HDR_CHANNEL.xxx);

    CompositeOutput output;
    output.SceneHDR = float4(
        source.rgb * revealage + resolved * coverage,
        source.a);
    output.ReactiveMask = coverage;
    return output;
}
