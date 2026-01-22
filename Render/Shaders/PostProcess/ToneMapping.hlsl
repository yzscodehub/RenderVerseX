// =============================================================================
// ToneMapping.hlsl - Tone mapping and gamma correction
// =============================================================================

// =============================================================================
// Constant Buffer
// =============================================================================

cbuffer ToneMappingConstants : register(b0)
{
    float Exposure;
    float Gamma;
    float WhitePoint;
    uint OperatorType;  // 0=Reinhard, 1=ReinhardExt, 2=ACES, 3=Uncharted2, 4=Neutral
    float2 TextureSize;
    float2 InvTextureSize;
};

// =============================================================================
// Textures and Samplers
// =============================================================================

Texture2D<float4> InputTexture : register(t0);
SamplerState LinearSampler : register(s0);

// =============================================================================
// Tone Mapping Operators
// =============================================================================

// Simple Reinhard
float3 Reinhard(float3 hdr)
{
    return hdr / (hdr + 1.0);
}

// Extended Reinhard with white point
float3 ReinhardExtended(float3 hdr, float whitePoint)
{
    float3 numerator = hdr * (1.0 + hdr / (whitePoint * whitePoint));
    return numerator / (hdr + 1.0);
}

// ACES Filmic (approximate)
float3 ACESFilmic(float3 x)
{
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

// Uncharted 2 filmic curve
float3 Uncharted2Partial(float3 x)
{
    float A = 0.15;  // Shoulder strength
    float B = 0.50;  // Linear strength
    float C = 0.10;  // Linear angle
    float D = 0.20;  // Toe strength
    float E = 0.02;  // Toe numerator
    float F = 0.30;  // Toe denominator
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

float3 Uncharted2(float3 hdr, float whitePoint)
{
    float3 curr = Uncharted2Partial(hdr * 2.0);
    float3 whiteScale = 1.0 / Uncharted2Partial(whitePoint);
    return curr * whiteScale;
}

// Neutral tonemapper
float3 NeutralTonemap(float3 x)
{
    float a = 0.2;
    float b = 0.29;
    float c = 0.24;
    float d = 0.272;
    float e = 0.02;
    float f = 0.3;
    float whiteLevel = 5.3;
    float whiteClip = 1.0;
    
    float3 toneMapped = ((x * (a * x + c * b) + d * e) / (x * (a * x + b) + d * f)) - e / f;
    float3 whiteScale = 1.0 / (((whiteLevel * (a * whiteLevel + c * b) + d * e) / 
                                (whiteLevel * (a * whiteLevel + b) + d * f)) - e / f);
    return toneMapped * whiteScale;
}

// =============================================================================
// Gamma Correction
// =============================================================================

float3 LinearToSRGB(float3 linear)
{
    return pow(linear, 1.0 / 2.2);
}

float3 GammaCorrect(float3 linear, float gamma)
{
    return pow(linear, 1.0 / gamma);
}

// =============================================================================
// Vertex Shader (Fullscreen Triangle)
// =============================================================================

struct VSOutput
{
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD0;
};

VSOutput VSMain(uint vertexID : SV_VertexID)
{
    VSOutput output;
    
    // Generate fullscreen triangle
    // Vertex 0: (-1, -1), Vertex 1: (3, -1), Vertex 2: (-1, 3)
    output.TexCoord = float2((vertexID << 1) & 2, vertexID & 2);
    output.Position = float4(output.TexCoord * 2.0 - 1.0, 0.0, 1.0);
    output.TexCoord.y = 1.0 - output.TexCoord.y;  // Flip Y for texture coords
    
    return output;
}

// =============================================================================
// Pixel Shader
// =============================================================================

float4 PSMain(VSOutput input) : SV_TARGET
{
    // Sample HDR scene color
    float3 hdr = InputTexture.Sample(LinearSampler, input.TexCoord).rgb;
    
    // Apply exposure
    hdr *= Exposure;
    
    // Apply tone mapping
    float3 ldr;
    switch (OperatorType)
    {
    case 0:  // Reinhard
        ldr = Reinhard(hdr);
        break;
    case 1:  // Reinhard Extended
        ldr = ReinhardExtended(hdr, WhitePoint);
        break;
    case 2:  // ACES
        ldr = ACESFilmic(hdr);
        break;
    case 3:  // Uncharted 2
        ldr = Uncharted2(hdr, WhitePoint);
        break;
    case 4:  // Neutral
        ldr = NeutralTonemap(hdr);
        break;
    default:  // Pass-through
        ldr = saturate(hdr);
        break;
    }
    
    // Gamma correction
    float3 output = GammaCorrect(ldr, Gamma);
    
    return float4(output, 1.0);
}
