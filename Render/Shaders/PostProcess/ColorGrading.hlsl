/**
 * @file ColorGrading.hlsl
 * @brief Color grading shader
 * 
 * Comprehensive color grading with:
 * - White balance
 * - Lift/Gamma/Gain
 * - HSV adjustments
 * - Channel mixing
 * - 3D LUT support
 * - Split toning
 */

// =============================================================================
// Constants
// =============================================================================

cbuffer ColorGradingConstants : register(b0)
{
    float4 WhiteBalance;        // x: temperature, y: tint, z: unused, w: unused
    float4 GlobalAdjust;        // x: exposure, y: contrast, z: saturation, w: hueShift
    float4 Lift;                // rgb: color, a: offset
    float4 Gamma;               // rgb: color, a: offset
    float4 Gain;                // rgb: color, a: offset
    float4 RedChannel;          // xyz: contribution, w: unused
    float4 GreenChannel;        // xyz: contribution, w: unused
    float4 BlueChannel;         // xyz: contribution, w: unused
    float4 ShadowsTint;         // rgb: color, a: unused
    float4 HighlightsTint;      // rgb: color, a: balance
    float4 LUTParams;           // x: useLUT, y: contribution, z: lutSize, w: unused
    float4 ScreenSize;          // x: width, y: height, z: 1/width, w: 1/height
}

#define Temperature         WhiteBalance.x
#define Tint                WhiteBalance.y
#define Exposure            GlobalAdjust.x
#define Contrast            GlobalAdjust.y
#define Saturation          GlobalAdjust.z
#define HueShift            GlobalAdjust.w
#define SplitToningBalance  HighlightsTint.a
#define UseLUT              (LUTParams.x > 0.5)
#define LUTContribution     LUTParams.y
#define LUTSize             LUTParams.z

Texture2D<float4> InputColor : register(t0);
Texture3D<float4> LUTTexture : register(t1);

RWTexture2D<float4> OutputColor : register(u0);

SamplerState LinearClampSampler : register(s0);

// =============================================================================
// Color space conversions
// =============================================================================

// RGB to HSV
float3 RGBtoHSV(float3 rgb)
{
    float4 K = float4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    float4 p = lerp(float4(rgb.bg, K.wz), float4(rgb.gb, K.xy), step(rgb.b, rgb.g));
    float4 q = lerp(float4(p.xyw, rgb.r), float4(rgb.r, p.yzx), step(p.x, rgb.r));
    
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    
    return float3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

// HSV to RGB
float3 HSVtoRGB(float3 hsv)
{
    float4 K = float4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    float3 p = abs(frac(hsv.xxx + K.xyz) * 6.0 - K.www);
    return hsv.z * lerp(K.xxx, clamp(p - K.xxx, 0.0, 1.0), hsv.y);
}

// RGB to luminance
float Luminance(float3 rgb)
{
    return dot(rgb, float3(0.2126, 0.7152, 0.0722));
}

// =============================================================================
// White balance
// =============================================================================

// Convert temperature to RGB shift (simplified)
float3 TemperatureToRGB(float temperature)
{
    // Approximate daylight locus
    float t = temperature / 100.0;
    float3 result;
    
    if (temperature > 0)
    {
        // Warmer (more red/yellow)
        result = float3(1.0 + t * 0.3, 1.0, 1.0 - t * 0.2);
    }
    else
    {
        // Cooler (more blue)
        result = float3(1.0 + t * 0.2, 1.0, 1.0 - t * 0.3);
    }
    
    return result;
}

float3 ApplyWhiteBalance(float3 color, float temperature, float tint)
{
    // Temperature (blue-orange axis)
    float3 tempShift = TemperatureToRGB(temperature);
    color *= tempShift;
    
    // Tint (green-magenta axis)
    float tintFactor = tint / 100.0;
    color.g *= (1.0 + tintFactor * 0.3);
    color.rb *= (1.0 - tintFactor * 0.1);
    
    return color;
}

// =============================================================================
// Lift/Gamma/Gain
// =============================================================================

float3 ApplyLiftGammaGain(float3 color, float4 lift, float4 gamma, float4 gain)
{
    // Lift (shadows)
    color = color * (lift.rgb + 1.0) + lift.a;
    
    // Gain (highlights)
    color = color * (gain.rgb + gain.a);
    
    // Gamma (midtones)
    float3 gammaExp = 1.0 / max(gamma.rgb + gamma.a, 0.001);
    color = pow(max(color, 0.0), gammaExp);
    
    return color;
}

// =============================================================================
// Channel mixer
// =============================================================================

float3 ApplyChannelMixer(float3 color, float3 redContrib, float3 greenContrib, float3 blueContrib)
{
    float3 result;
    result.r = dot(color, redContrib);
    result.g = dot(color, greenContrib);
    result.b = dot(color, blueContrib);
    return result;
}

// =============================================================================
// HSV adjustments
// =============================================================================

float3 ApplyHSVAdjustments(float3 color, float saturation, float hueShift)
{
    float3 hsv = RGBtoHSV(color);
    
    // Hue shift
    hsv.x = frac(hsv.x + hueShift / 360.0);
    
    // Saturation
    hsv.y *= saturation;
    
    return HSVtoRGB(hsv);
}

// =============================================================================
// Split toning
// =============================================================================

float3 ApplySplitToning(float3 color, float3 shadowsTint, float3 highlightsTint, float balance)
{
    float luma = Luminance(color);
    
    // Adjust balance point
    float shadowWeight = smoothstep(0.5 + balance * 0.5, 0.0, luma);
    float highlightWeight = smoothstep(0.5 - balance * 0.5, 1.0, luma);
    
    // Blend tints
    float3 shadowColor = lerp(color, color * shadowsTint * 2.0, shadowWeight * 0.5);
    float3 highlightColor = lerp(color, color * highlightsTint * 2.0, highlightWeight * 0.5);
    
    return lerp(shadowColor, highlightColor, luma);
}

// =============================================================================
// Contrast
// =============================================================================

float3 ApplyContrast(float3 color, float contrast)
{
    // Apply contrast around midpoint (0.5 or 0.18 for linear)
    float midpoint = 0.18;  // Linear space midgray
    return (color - midpoint) * contrast + midpoint;
}

// =============================================================================
// LUT sampling
// =============================================================================

float3 SampleLUT(float3 color, float lutSize)
{
    // Scale and offset for LUT sampling
    float scale = (lutSize - 1.0) / lutSize;
    float offset = 0.5 / lutSize;
    
    float3 lutCoord = color * scale + offset;
    return LUTTexture.SampleLevel(LinearClampSampler, lutCoord, 0).rgb;
}

// =============================================================================
// Main compute shader
// =============================================================================

[numthreads(8, 8, 1)]
void CS_ColorGrading(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= (uint)ScreenSize.x || 
        dispatchThreadId.y >= (uint)ScreenSize.y)
        return;
    
    float2 uv = (dispatchThreadId.xy + 0.5) * ScreenSize.zw;
    float4 color = InputColor[dispatchThreadId.xy];
    
    // Store original alpha
    float alpha = color.a;
    
    // 1. Exposure (first, as it affects all subsequent operations)
    color.rgb *= exp2(Exposure);
    
    // 2. White balance
    color.rgb = ApplyWhiteBalance(color.rgb, Temperature, Tint);
    
    // 3. Contrast
    color.rgb = ApplyContrast(color.rgb, Contrast);
    
    // 4. Lift/Gamma/Gain
    color.rgb = ApplyLiftGammaGain(color.rgb, Lift, Gamma, Gain);
    
    // 5. Channel mixing
    color.rgb = ApplyChannelMixer(color.rgb, RedChannel.xyz, GreenChannel.xyz, BlueChannel.xyz);
    
    // 6. HSV adjustments
    color.rgb = ApplyHSVAdjustments(color.rgb, Saturation, HueShift);
    
    // 7. Split toning
    color.rgb = ApplySplitToning(color.rgb, ShadowsTint.rgb, HighlightsTint.rgb, SplitToningBalance);
    
    // 8. LUT (optional)
    if (UseLUT)
    {
        float3 lutColor = SampleLUT(saturate(color.rgb), LUTSize);
        color.rgb = lerp(color.rgb, lutColor, LUTContribution);
    }
    
    // Ensure non-negative
    color.rgb = max(color.rgb, 0.0);
    
    // Restore alpha
    color.a = alpha;
    
    OutputColor[dispatchThreadId.xy] = color;
}

// =============================================================================
// Graphics pass version (fullscreen triangle)
// =============================================================================

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput VS_Fullscreen(uint vertexId : SV_VertexID)
{
    VSOutput output;
    output.uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(output.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return output;
}

float4 PS_ColorGrading(VSOutput input) : SV_Target
{
    float4 color = InputColor.Sample(LinearClampSampler, input.uv);
    float alpha = color.a;
    
    color.rgb *= exp2(Exposure);
    color.rgb = ApplyWhiteBalance(color.rgb, Temperature, Tint);
    color.rgb = ApplyContrast(color.rgb, Contrast);
    color.rgb = ApplyLiftGammaGain(color.rgb, Lift, Gamma, Gain);
    color.rgb = ApplyChannelMixer(color.rgb, RedChannel.xyz, GreenChannel.xyz, BlueChannel.xyz);
    color.rgb = ApplyHSVAdjustments(color.rgb, Saturation, HueShift);
    color.rgb = ApplySplitToning(color.rgb, ShadowsTint.rgb, HighlightsTint.rgb, SplitToningBalance);
    
    if (UseLUT)
    {
        float3 lutColor = SampleLUT(saturate(color.rgb), LUTSize);
        color.rgb = lerp(color.rgb, lutColor, LUTContribution);
    }
    
    color.rgb = max(color.rgb, 0.0);
    color.a = alpha;
    
    return color;
}
