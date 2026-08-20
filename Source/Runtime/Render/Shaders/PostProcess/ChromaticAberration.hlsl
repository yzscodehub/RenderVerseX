// =============================================================================
// ChromaticAberration.hlsl - LDR fullscreen chromatic aberration post-process
// =============================================================================

#include "../Include/FullscreenTriangle.hlsli"

cbuffer ChromaticAberrationConstants : register(b0, space0)
{
    float2 TextureSize;
    float2 InvTextureSize;
    float Intensity;
    float StartOffset;
    float RadialFalloff;
    float UseSpectral;
    float2 RedOffset;
    float2 GreenOffset;
    float2 BlueOffset;
    float2 Padding0;
};

Texture2D<float4> InputTexture : register(t1, space0);
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

float2 ClampUV(float2 uv)
{
    return saturate(uv);
}

float CalculateFalloff(float2 uv, float startOffset, float radialFalloff)
{
    if (radialFalloff < 0.5)
    {
        return 1.0;
    }

    const float safeStart = saturate(startOffset);
    const float edgeSpan = max(1.0 - safeStart, 0.0001);
    const float2 centered = uv - 0.5;
    const float distanceFromCenter = saturate(length(centered) * 2.0);
    return saturate((distanceFromCenter - safeStart) / edgeSpan);
}

float4 PSMain(VSOutput input) : SV_Target
{
    const float2 uv = saturate(input.TexCoord);
    const float4 baseColor = InputTexture.Sample(LinearSampler, uv);
    const float safeIntensity = max(Intensity, 0.0);
    const float falloff = CalculateFalloff(uv, StartOffset, RadialFalloff);

    const float2 pixelStep = InvTextureSize * (safeIntensity * falloff * 4.0);
    const float red = InputTexture.Sample(LinearSampler, ClampUV(uv + RedOffset * pixelStep)).r;
    const float green = InputTexture.Sample(LinearSampler, ClampUV(uv + GreenOffset * pixelStep)).g;
    const float blue = InputTexture.Sample(LinearSampler, ClampUV(uv + BlueOffset * pixelStep)).b;

    return float4(red, green, blue, baseColor.a);
}
