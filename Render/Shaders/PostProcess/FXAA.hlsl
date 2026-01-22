// =============================================================================
// FXAA.hlsl - Fast Approximate Anti-Aliasing (FXAA 3.11)
// =============================================================================
//
// Based on NVIDIA's FXAA 3.11 implementation.
// This is applied after tone mapping on LDR color for best results.
//
// =============================================================================

// =============================================================================
// Quality Presets
// =============================================================================

// Quality 10-15: Low quality (fast)
// Quality 20-29: Medium quality
// Quality 39: High quality (default)

#ifndef FXAA_QUALITY_PRESET
    #define FXAA_QUALITY_PRESET 39
#endif

#if (FXAA_QUALITY_PRESET == 10)
    #define FXAA_QUALITY_PS 3
    static const float fxaaQualityP[3] = { 1.5, 3.0, 12.0 };
#elif (FXAA_QUALITY_PRESET == 15)
    #define FXAA_QUALITY_PS 4
    static const float fxaaQualityP[4] = { 1.0, 1.5, 3.0, 12.0 };
#elif (FXAA_QUALITY_PRESET == 20)
    #define FXAA_QUALITY_PS 3
    static const float fxaaQualityP[3] = { 1.5, 2.0, 8.0 };
#elif (FXAA_QUALITY_PRESET == 29)
    #define FXAA_QUALITY_PS 5
    static const float fxaaQualityP[5] = { 1.0, 1.5, 2.0, 4.0, 12.0 };
#else  // FXAA_QUALITY_PRESET == 39 (default high quality)
    #define FXAA_QUALITY_PS 12
    static const float fxaaQualityP[12] = { 1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0 };
#endif

// =============================================================================
// Constant Buffer
// =============================================================================

cbuffer FXAAConstants : register(b0)
{
    float2 TextureSize;
    float2 InvTextureSize;
    float EdgeThreshold;       // Default 0.166
    float EdgeThresholdMin;    // Default 0.0833
    float SubpixelQuality;     // Default 0.75
    float Padding;
};

// =============================================================================
// Textures and Samplers
// =============================================================================

Texture2D<float4> InputTexture : register(t0);
SamplerState LinearSampler : register(s0);

// =============================================================================
// Utility Functions
// =============================================================================

float Luminance(float3 color)
{
    // Compute luma - use green channel which eye is most sensitive to
    return dot(color, float3(0.299, 0.587, 0.114));
}

// =============================================================================
// Fullscreen Vertex Shader
// =============================================================================

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
    output.TexCoord.y = 1.0 - output.TexCoord.y;
    return output;
}

// =============================================================================
// FXAA Pixel Shader
// =============================================================================

float4 PSMain(VSOutput input) : SV_TARGET
{
    float2 posM = input.TexCoord;
    float2 offset = InvTextureSize;
    
    // Sample center and 4 neighbors
    float4 centerSample = InputTexture.Sample(LinearSampler, posM);
    float3 rgbM = centerSample.rgb;
    float lumaM = Luminance(rgbM);
    
    float lumaN = Luminance(InputTexture.Sample(LinearSampler, posM + float2(0, -offset.y)).rgb);
    float lumaS = Luminance(InputTexture.Sample(LinearSampler, posM + float2(0, offset.y)).rgb);
    float lumaE = Luminance(InputTexture.Sample(LinearSampler, posM + float2(offset.x, 0)).rgb);
    float lumaW = Luminance(InputTexture.Sample(LinearSampler, posM + float2(-offset.x, 0)).rgb);
    
    // Compute local contrast
    float lumaMin = min(lumaM, min(min(lumaN, lumaS), min(lumaE, lumaW)));
    float lumaMax = max(lumaM, max(max(lumaN, lumaS), max(lumaE, lumaW)));
    float lumaRange = lumaMax - lumaMin;
    
    // Early exit if local contrast is too low
    if (lumaRange < max(EdgeThresholdMin, lumaMax * EdgeThreshold))
    {
        return centerSample;
    }
    
    // Sample corners
    float lumaNW = Luminance(InputTexture.Sample(LinearSampler, posM + float2(-offset.x, -offset.y)).rgb);
    float lumaNE = Luminance(InputTexture.Sample(LinearSampler, posM + float2(offset.x, -offset.y)).rgb);
    float lumaSW = Luminance(InputTexture.Sample(LinearSampler, posM + float2(-offset.x, offset.y)).rgb);
    float lumaSE = Luminance(InputTexture.Sample(LinearSampler, posM + float2(offset.x, offset.y)).rgb);
    
    float lumaNS = lumaN + lumaS;
    float lumaWE = lumaW + lumaE;
    
    // Compute edge direction
    float edgeHorz = abs(lumaNW + lumaNE - 2.0 * lumaM - lumaSW - lumaSE) +
                     abs(lumaN + lumaS - 2.0 * lumaM) * 2.0;
    float edgeVert = abs(lumaNW + lumaSW - 2.0 * lumaM - lumaNE - lumaSE) +
                     abs(lumaW + lumaE - 2.0 * lumaM) * 2.0;
    
    bool horzSpan = edgeHorz >= edgeVert;
    
    // Select edge direction
    float lengthSign = horzSpan ? offset.y : offset.x;
    float luma1 = horzSpan ? lumaN : lumaW;
    float luma2 = horzSpan ? lumaS : lumaE;
    float gradient1 = luma1 - lumaM;
    float gradient2 = luma2 - lumaM;
    
    bool is1Steepest = abs(gradient1) >= abs(gradient2);
    float gradientScaled = 0.25 * max(abs(gradient1), abs(gradient2));
    
    // Select the steeper gradient direction
    if (is1Steepest) lengthSign = -lengthSign;
    
    // Compute step direction
    float2 posB = posM;
    if (horzSpan)
        posB.y += lengthSign * 0.5;
    else
        posB.x += lengthSign * 0.5;
    
    // Search along edge
    float2 offNP = horzSpan ? float2(offset.x, 0) : float2(0, offset.y);
    
    float2 posN = posB - offNP * fxaaQualityP[0];
    float2 posP = posB + offNP * fxaaQualityP[0];
    
    float lumaEndN = Luminance(InputTexture.Sample(LinearSampler, posN).rgb);
    float lumaEndP = Luminance(InputTexture.Sample(LinearSampler, posP).rgb);
    
    lumaEndN -= (is1Steepest ? luma1 : luma2);
    lumaEndP -= (is1Steepest ? luma1 : luma2);
    
    bool doneN = abs(lumaEndN) >= gradientScaled;
    bool doneP = abs(lumaEndP) >= gradientScaled;
    
    // Continue searching
    for (int i = 1; i < FXAA_QUALITY_PS && !(doneN && doneP); i++)
    {
        if (!doneN)
        {
            posN -= offNP * fxaaQualityP[i];
            lumaEndN = Luminance(InputTexture.Sample(LinearSampler, posN).rgb) - (is1Steepest ? luma1 : luma2);
            doneN = abs(lumaEndN) >= gradientScaled;
        }
        if (!doneP)
        {
            posP += offNP * fxaaQualityP[i];
            lumaEndP = Luminance(InputTexture.Sample(LinearSampler, posP).rgb) - (is1Steepest ? luma1 : luma2);
            doneP = abs(lumaEndP) >= gradientScaled;
        }
    }
    
    // Compute sub-pixel offset
    float dstN = horzSpan ? posM.x - posN.x : posM.y - posN.y;
    float dstP = horzSpan ? posP.x - posM.x : posP.y - posM.y;
    
    bool directionN = dstN < dstP;
    float dst = min(dstN, dstP);
    float spanLength = dstN + dstP;
    
    bool goodSpan = directionN ? (lumaEndN < 0.0) : (lumaEndP < 0.0);
    
    float subpixNS = (-2.0 * lumaM + lumaNS) * (-2.0 * lumaM + lumaNS);
    float subpixWE = (-2.0 * lumaM + lumaWE) * (-2.0 * lumaM + lumaWE);
    float subpixNSWE = subpixNS + subpixWE;
    
    float pixelOffset = -dst / spanLength + 0.5;
    float subPixelOffset = subpixNSWE * (1.0 / 12.0);
    subPixelOffset = saturate(subPixelOffset);
    subPixelOffset = subPixelOffset * subPixelOffset * SubpixelQuality;
    
    float finalOffset = max(pixelOffset, subPixelOffset);
    if (!goodSpan) finalOffset = subPixelOffset;
    
    // Apply offset and sample
    float2 finalPos = posM;
    if (horzSpan)
        finalPos.y += finalOffset * lengthSign;
    else
        finalPos.x += finalOffset * lengthSign;
    
    float4 result = InputTexture.Sample(LinearSampler, finalPos);
    
    return result;
}
