// =============================================================================
// Vignette.hlsl - LDR fullscreen vignette post-process
// =============================================================================

cbuffer VignetteConstants : register(b0, space0)
{
    float2 TextureSize;
    float2 InvTextureSize;
    float Intensity;
    float Smoothness;
    float Roundness;
    float Mode;
    float2 Center;
    float AspectRatio;
    float Padding0;
    float4 VignetteColor_Padding1;
};

#define VignetteColor VignetteColor_Padding1.xyz
#define Padding1 VignetteColor_Padding1.w

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

    float2 positions[3] = {
        float2(-1.0, -1.0),
        float2(-1.0,  3.0),
        float2( 3.0, -1.0)
    };

    float2 texCoords[3] = {
        float2(0.0, 1.0),
        float2(0.0, -1.0),
        float2(2.0, 1.0)
    };

    output.Position = float4(positions[vertexID], 0.0, 1.0);
    output.TexCoord = texCoords[vertexID];
    return output;
}

float CalculateVignette(float2 uv,
                        float intensity,
                        float smoothness,
                        float roundness,
                        float2 center,
                        float aspectRatio)
{
    float2 coord = (uv - center) * 2.0;
    coord.x *= aspectRatio;

    float dist;
    if (roundness >= 1.0)
    {
        dist = length(coord);
    }
    else
    {
        float2 absCoord = abs(coord);
        float maxDist = max(absCoord.x, absCoord.y);
        float circDist = length(coord);
        dist = lerp(maxDist, circDist, saturate(roundness));
    }

    return 1.0 - smoothstep(1.0 - smoothness, 1.0, dist * intensity);
}

float CalculateNaturalVignette(float2 uv, float intensity)
{
    float2 coord = uv - 0.5;
    float dist = length(coord) * 2.0;
    float cosAngle = 1.0 / sqrt(1.0 + dist * dist);
    return pow(cosAngle, 4.0 * intensity);
}

float4 PSMain(VSOutput input) : SV_Target
{
    float2 uv = saturate(input.TexCoord);
    float4 color = InputTexture.Sample(LinearSampler, uv);

    const float safeIntensity = max(Intensity, 0.0);
    const float safeSmoothness = saturate(Smoothness);
    const float safeRoundness = saturate(Roundness);
    const float safeAspectRatio = max(AspectRatio, 0.0001);

    float vignette;
    if ((uint)Mode == 2)
    {
        vignette = CalculateNaturalVignette(uv, safeIntensity);
    }
    else
    {
        vignette = CalculateVignette(uv,
                                     safeIntensity,
                                     safeSmoothness,
                                     safeRoundness,
                                     Center,
                                     safeAspectRatio);
    }

    color.rgb = lerp(VignetteColor, color.rgb, saturate(vignette));
    return color;
}
