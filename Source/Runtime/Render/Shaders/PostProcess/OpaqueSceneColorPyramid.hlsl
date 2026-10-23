// =============================================================================
// OpaqueSceneColorPyramid.hlsl - late opaque-scene refraction pyramid
// =============================================================================
//
// Builds mip zero directly from the post-reflection/post-Sky HDR source, then
// reduces every later mip with a four-tap box filter. The source-copy mode is
// used only for Ultra's full-resolution base level; High intentionally starts
// at half resolution and therefore uses the same reduction kernel as later
// mips. The texture dimensions are queried from the source so each level has
// its own exact texel footprint.
// =============================================================================

#include "../Include/FullscreenTriangle.hlsli"

cbuffer OpaqueSceneColorPyramidConstants : register(b0, space0)
{
    float4 SourceParameters;
};

#define SourceIsFullResolution SourceParameters.x

Texture2D<float4> InputSceneColor : register(t1, space0);
SamplerState LinearClampSampler : register(s2, space0);

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
    if (SourceIsFullResolution > 0.5f)
    {
        return InputSceneColor.SampleLevel(LinearClampSampler, input.TexCoord, 0.0f);
    }

    uint sourceWidth = 1;
    uint sourceHeight = 1;
    InputSceneColor.GetDimensions(sourceWidth, sourceHeight);
    const float2 texelSize = 1.0f / float2(
        max(sourceWidth, 1u), max(sourceHeight, 1u));
    const float2 halfTexel = texelSize * 0.5f;
    const float2 uv = saturate(input.TexCoord);
    return (
        InputSceneColor.SampleLevel(LinearClampSampler, uv + float2(-halfTexel.x, -halfTexel.y), 0.0f) +
        InputSceneColor.SampleLevel(LinearClampSampler, uv + float2( halfTexel.x, -halfTexel.y), 0.0f) +
        InputSceneColor.SampleLevel(LinearClampSampler, uv + float2(-halfTexel.x,  halfTexel.y), 0.0f) +
        InputSceneColor.SampleLevel(LinearClampSampler, uv + float2( halfTexel.x,  halfTexel.y), 0.0f)) * 0.25f;
}
