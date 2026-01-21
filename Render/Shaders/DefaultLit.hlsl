// =============================================================================
// DefaultLit.hlsl - Default lit shader for model rendering
// =============================================================================
//
// Vertex inputs come from SEPARATE vertex buffers (slots):
//   Slot 0: Position buffer (float3)
//   Slot 1: Normal buffer (float3)
//   Slot 2: UV buffer (float2)
//
// This matches glTF's separated attribute storage for simpler GPU upload.
// =============================================================================

// =============================================================================
// Constant Buffers
// =============================================================================

// View constants (bound via descriptor set)
// Note: GLM and HLSL cbuffer both use column-major storage, no transposition needed.
cbuffer ViewConstants : register(b0)
{
    float4x4 ViewProjection;
    float3 CameraPosition;
    float Time;
    float3 LightDirection;
    float Padding;
};

// Object constants (bound via descriptor set)
cbuffer ObjectConstants : register(b1)
{
    float4x4 World;
};

// =============================================================================
// Vertex Shader Input/Output
// =============================================================================

struct VSInput
{
    float3 Position : POSITION;   // Slot 0: position buffer
    float3 Normal   : NORMAL;     // Slot 1: normal buffer
    float2 TexCoord : TEXCOORD0;  // Slot 2: uv buffer
};

struct PSInput
{
    float4 Position    : SV_POSITION;
    float3 WorldPos    : TEXCOORD0;
    float3 WorldNormal : TEXCOORD1;
    float2 TexCoord    : TEXCOORD2;
};

// =============================================================================
// Vertex Shader
// =============================================================================

PSInput VSMain(VSInput input)
{
    PSInput output;
    
    // GLM and HLSL cbuffer both use column-major storage by default.
    // Use column-vector convention: mul(matrix, vector) = M * v
    float4 worldPos = mul(World, float4(input.Position, 1.0));
    output.WorldPos = worldPos.xyz;
    
    // Transform to clip space: clipPos = ViewProjection * worldPos
    output.Position = mul(ViewProjection, worldPos);
    
    // Transform normal to world space (assuming uniform scale)
    // For normals: n' = (M^-1)^T * n, but with uniform scale: n' = M * n
    output.WorldNormal = normalize(mul((float3x3)World, input.Normal));
    
    // Pass through texture coordinates
    output.TexCoord = input.TexCoord;
    
    return output;
}

// =============================================================================
// Pixel Shader
// =============================================================================

float4 PSMain(PSInput input) : SV_TARGET
{
    // Normalize interpolated normal
    float3 normal = normalize(input.WorldNormal);
    
    // Simple directional light (from above-front)
    float3 lightDir = normalize(LightDirection);
    
    // Lambertian diffuse
    float NdotL = max(dot(normal, -lightDir), 0.0);
    
    // Ambient + diffuse lighting
    float ambient = 0.15;
    float diffuse = NdotL * 0.85;
    float lighting = ambient + diffuse;
    
    // Base color (white for now - will add texture sampling later)
    float3 baseColor = float3(0.8, 0.8, 0.8);
    
    // Final color
    float3 finalColor = baseColor * lighting;
    
    return float4(finalColor, 1.0);
}
