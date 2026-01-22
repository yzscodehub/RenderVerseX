// =============================================================================
// Bloom.hlsl - Multi-pass bloom effect
// =============================================================================
//
// Implements a dual Kawase blur bloom:
// 1. BrightPass: Extract pixels above threshold
// 2. Downsample: Progressive downsampling with blur
// 3. Upsample: Progressive upsampling with blur and accumulation
// 4. Composite: Add bloom to scene color
//
// =============================================================================

// =============================================================================
// Constant Buffer
// =============================================================================

cbuffer BloomConstants : register(b0)
{
    float Threshold;
    float SoftKnee;
    float Intensity;
    float Radius;
    float2 TextureSize;
    float2 InvTextureSize;
    uint PassType;  // 0=BrightPass, 1=Downsample, 2=Upsample, 3=Composite
    float3 Padding;
};

// =============================================================================
// Textures and Samplers
// =============================================================================

Texture2D<float4> InputTexture : register(t0);
Texture2D<float4> BloomTexture : register(t1);  // For composite pass
SamplerState LinearSampler : register(s0);

// =============================================================================
// Utility Functions
// =============================================================================

float Luminance(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

// Soft threshold function for smooth transition
float3 QuadraticThreshold(float3 color, float threshold, float knee)
{
    float luma = Luminance(color);
    float soft = luma - threshold + knee;
    soft = clamp(soft, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 0.00001);
    float multiplier = max(soft, luma - threshold) / max(luma, 0.00001);
    return color * multiplier;
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
// Bright Pass - Extract bright areas
// =============================================================================

float4 PSBrightPass(VSOutput input) : SV_TARGET
{
    float3 color = InputTexture.Sample(LinearSampler, input.TexCoord).rgb;
    
    // Apply soft threshold
    float3 bloom = QuadraticThreshold(color, Threshold, Threshold * SoftKnee);
    
    return float4(bloom, 1.0);
}

// =============================================================================
// Dual Kawase Downsample - Efficient box blur during downsample
// =============================================================================

float4 PSDownsample(VSOutput input) : SV_TARGET
{
    float2 offset = InvTextureSize * Radius;
    
    // 5-tap pattern
    float3 color = InputTexture.Sample(LinearSampler, input.TexCoord).rgb * 4.0;
    color += InputTexture.Sample(LinearSampler, input.TexCoord + float2(-offset.x, -offset.y)).rgb;
    color += InputTexture.Sample(LinearSampler, input.TexCoord + float2( offset.x, -offset.y)).rgb;
    color += InputTexture.Sample(LinearSampler, input.TexCoord + float2(-offset.x,  offset.y)).rgb;
    color += InputTexture.Sample(LinearSampler, input.TexCoord + float2( offset.x,  offset.y)).rgb;
    
    return float4(color / 8.0, 1.0);
}

// =============================================================================
// Dual Kawase Upsample - Blur during upsample and add to previous level
// =============================================================================

float4 PSUpsample(VSOutput input) : SV_TARGET
{
    float2 offset = InvTextureSize * Radius * 0.5;
    
    // 9-tap tent filter
    float3 color = float3(0.0, 0.0, 0.0);
    
    color += InputTexture.Sample(LinearSampler, input.TexCoord + float2(-offset.x * 2, 0)).rgb;
    color += InputTexture.Sample(LinearSampler, input.TexCoord + float2(-offset.x, offset.y)).rgb * 2.0;
    color += InputTexture.Sample(LinearSampler, input.TexCoord + float2(0, offset.y * 2)).rgb;
    color += InputTexture.Sample(LinearSampler, input.TexCoord + float2(offset.x, offset.y)).rgb * 2.0;
    color += InputTexture.Sample(LinearSampler, input.TexCoord + float2(offset.x * 2, 0)).rgb;
    color += InputTexture.Sample(LinearSampler, input.TexCoord + float2(offset.x, -offset.y)).rgb * 2.0;
    color += InputTexture.Sample(LinearSampler, input.TexCoord + float2(0, -offset.y * 2)).rgb;
    color += InputTexture.Sample(LinearSampler, input.TexCoord + float2(-offset.x, -offset.y)).rgb * 2.0;
    
    color /= 12.0;
    
    // Add lower mip level if available
    color += BloomTexture.Sample(LinearSampler, input.TexCoord).rgb;
    
    return float4(color, 1.0);
}

// =============================================================================
// Composite - Add bloom to scene
// =============================================================================

float4 PSComposite(VSOutput input) : SV_TARGET
{
    float3 scene = InputTexture.Sample(LinearSampler, input.TexCoord).rgb;
    float3 bloom = BloomTexture.Sample(LinearSampler, input.TexCoord).rgb;
    
    // Additive blend with intensity
    float3 result = scene + bloom * Intensity;
    
    return float4(result, 1.0);
}

// =============================================================================
// Main Pixel Shader (selects based on PassType)
// =============================================================================

float4 PSMain(VSOutput input) : SV_TARGET
{
    switch (PassType)
    {
    case 0:
        return PSBrightPass(input);
    case 1:
        return PSDownsample(input);
    case 2:
        return PSUpsample(input);
    case 3:
    default:
        return PSComposite(input);
    }
}
