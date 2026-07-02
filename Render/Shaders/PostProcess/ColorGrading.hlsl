// ColorGrading.hlsl - LDR fullscreen color-grading post-process

cbuffer ColorGradingConstants : register(b0, space0)
{
    float2 TextureSize;
    float2 InvTextureSize;
    float2 WhiteBalance;
    float Exposure;
    float Contrast;
    float Saturation;
    float HueShift;
    float4 Lift;
    float4 Gamma;
    float4 Gain;
    float4 RedChannel_Padding0;
    float4 GreenChannel_Padding1;
    float4 BlueChannel_Padding2;
    float4 ShadowsTint_SplitToningBalance;
    float4 HighlightsTint_Brightness;
};

#define RedChannel RedChannel_Padding0.xyz
#define Padding0 RedChannel_Padding0.w
#define GreenChannel GreenChannel_Padding1.xyz
#define Padding1 GreenChannel_Padding1.w
#define BlueChannel BlueChannel_Padding2.xyz
#define Padding2 BlueChannel_Padding2.w
#define ShadowsTint ShadowsTint_SplitToningBalance.xyz
#define SplitToningBalance ShadowsTint_SplitToningBalance.w
#define HighlightsTint HighlightsTint_Brightness.xyz
#define Brightness HighlightsTint_Brightness.w

Texture2D<float4> InputTexture : register(t1, space0);
SamplerState LinearSampler : register(s2, space0);

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    VSOutput output;
    output.uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

float Luminance(float3 rgb)
{
    return dot(rgb, float3(0.2126, 0.7152, 0.0722));
}

float3 RGBtoHSV(float3 rgb)
{
    float4 k = float4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    float4 p = lerp(float4(rgb.bg, k.wz), float4(rgb.gb, k.xy), step(rgb.b, rgb.g));
    float4 q = lerp(float4(p.xyw, rgb.r), float4(rgb.r, p.yzx), step(p.x, rgb.r));
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return float3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

float3 HSVtoRGB(float3 hsv)
{
    float4 k = float4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    float3 p = abs(frac(hsv.xxx + k.xyz) * 6.0 - k.www);
    return hsv.z * lerp(k.xxx, saturate(p - k.xxx), hsv.y);
}

float3 ApplyWhiteBalance(float3 color, float temperature, float tint)
{
    float temp = clamp(temperature, -100.0, 100.0) / 100.0;
    float tintFactor = clamp(tint, -100.0, 100.0) / 100.0;

    float3 tempShift = temp >= 0.0
        ? float3(1.0 + temp * 0.28, 1.0 + temp * 0.04, 1.0 - temp * 0.22)
        : float3(1.0 + temp * 0.16, 1.0 + temp * 0.02, 1.0 - temp * 0.32);

    float3 tintShift = float3(1.0 - tintFactor * 0.08,
                              1.0 + tintFactor * 0.22,
                              1.0 - tintFactor * 0.08);
    return color * tempShift * tintShift;
}

float3 ApplyContrast(float3 color, float contrast)
{
    const float midpoint = 0.5;
    return (color - midpoint) * max(contrast, 0.0) + midpoint;
}

float3 ApplyLiftGammaGain(float3 color, float4 lift, float4 gamma, float4 gain)
{
    color = color * max(lift.rgb, 0.0) + lift.a;
    color *= max(gain.rgb + gain.a, 0.0);
    float3 gammaExp = 1.0 / max(gamma.rgb + gamma.a, 0.001);
    return pow(max(color, 0.0), gammaExp);
}

float3 ApplyChannelMixer(float3 color, float3 redContrib, float3 greenContrib, float3 blueContrib)
{
    return float3(dot(color, redContrib),
                  dot(color, greenContrib),
                  dot(color, blueContrib));
}

float3 ApplyHSV(float3 color, float saturation, float hueShift)
{
    float3 hsv = RGBtoHSV(max(color, 0.0));
    hsv.x = frac(hsv.x + hueShift / 360.0);
    hsv.y = max(hsv.y * max(saturation, 0.0), 0.0);
    return HSVtoRGB(hsv);
}

float3 ApplySplitToning(float3 color, float3 shadowsTint, float3 highlightsTint, float balance)
{
    float luma = Luminance(saturate(color));
    float clampedBalance = clamp(balance, -1.0, 1.0);
    float shadowWeight = smoothstep(0.55 + clampedBalance * 0.35, 0.0, luma);
    float highlightWeight = smoothstep(0.45 + clampedBalance * 0.35, 1.0, luma);
    float3 shadowColor = color * lerp(float3(1.0, 1.0, 1.0), shadowsTint * 2.0, shadowWeight * 0.5);
    float3 highlightColor = color * lerp(float3(1.0, 1.0, 1.0), highlightsTint * 2.0, highlightWeight * 0.5);
    return lerp(shadowColor, highlightColor, saturate(luma + clampedBalance * 0.25));
}

float4 PSMain(VSOutput input) : SV_Target
{
    float4 color = InputTexture.Sample(LinearSampler, input.uv);
    float alpha = color.a;

    color.rgb *= exp2(Exposure);
    color.rgb += Brightness;
    color.rgb = ApplyWhiteBalance(color.rgb, WhiteBalance.x, WhiteBalance.y);
    color.rgb = ApplyContrast(color.rgb, Contrast);
    color.rgb = ApplyLiftGammaGain(color.rgb, Lift, Gamma, Gain);
    color.rgb = ApplyChannelMixer(color.rgb, RedChannel, GreenChannel, BlueChannel);
    color.rgb = ApplyHSV(color.rgb, Saturation, HueShift);
    color.rgb = ApplySplitToning(color.rgb, ShadowsTint, HighlightsTint, SplitToningBalance);
    color.rgb = max(color.rgb, 0.0);
    color.a = alpha;
    return color;
}
