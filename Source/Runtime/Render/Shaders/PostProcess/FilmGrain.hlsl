// =============================================================================
// FilmGrain.hlsl - LDR fullscreen deterministic film grain post-process
// =============================================================================

#include "../Include/FullscreenTriangle.hlsli"

cbuffer FilmGrainConstants : register(b0, space0)
{
    float2 TextureSize;
    float2 InvTextureSize;
    float Intensity;
    float Response;
    float GrainSize;
    float LuminanceContribution;
    float ColorContribution;
    float Time;
    float Type;
    float Padding0;
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

float Hash21(float2 value)
{
    value = frac(value * float2(123.34, 456.21));
    value += dot(value, value + 45.32);
    return frac(value.x * value.y);
}

float3 Hash23(float2 value)
{
    return float3(Hash21(value + 11.17), Hash21(value + 37.41), Hash21(value + 83.53));
}

float4 PSMain(VSOutput input) : SV_Target
{
    float2 uv = saturate(input.TexCoord);
    float4 color = InputTexture.Sample(LinearSampler, uv);

    const float safeIntensity = saturate(Intensity);
    const float safeResponse = saturate(Response);
    const float safeGrainSize = max(GrainSize, 0.25);
    const float2 pixelCell = floor(uv * max(TextureSize, float2(1.0, 1.0)) / safeGrainSize);
    const float timeSeed = floor(max(Time, 0.0) * 60.0);

    const float luminance = dot(color.rgb, float3(0.2126, 0.7152, 0.0722));
    const float responseScale = lerp(1.0, 1.0 - saturate(luminance), safeResponse);
    const float strength = safeIntensity * responseScale;

    float monoGrain = Hash21(pixelCell + timeSeed) * 2.0 - 1.0;
    float3 colorGrain = Hash23(pixelCell + timeSeed * 1.37) * 2.0 - 1.0;

    const uint grainType = (uint)Type;
    if (grainType == 0)
    {
        colorGrain = monoGrain.xxx;
    }
    else if (grainType == 1)
    {
        colorGrain = lerp(monoGrain.xxx, colorGrain, 0.25);
    }

    float3 grain = monoGrain.xxx * max(LuminanceContribution, 0.0);
    grain += colorGrain * max(ColorContribution, 0.0);
    color.rgb = saturate(color.rgb + grain * strength);
    return color;
}
