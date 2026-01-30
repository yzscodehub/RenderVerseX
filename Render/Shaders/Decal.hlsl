/**
 * @file Decal.hlsl
 * @brief Deferred decal shader
 * 
 * Projects decal textures onto scene geometry.
 */

// =============================================================================
// Constants
// =============================================================================

cbuffer DecalConstants : register(b0)
{
    float4x4 ViewProj;
    float4x4 InvViewProj;
    float4x4 DecalWorldToLocal;
    float4x4 DecalLocalToWorld;
    
    float4 DecalColor;          // rgb: color, a: alpha
    float4 DecalParams;         // x: normalStrength, y: roughness, z: metallic, w: angleFade
    float4 DecalParams2;        // x: fadeDistance, y: albedoContrib, z: normalContrib, w: roughnessContrib
    float4 CameraPosition;      // xyz: position, w: unused
    float4 ScreenSize;          // x: width, y: height, z: 1/width, w: 1/height
}

#define NormalStrength      DecalParams.x
#define Roughness           DecalParams.y
#define Metallic            DecalParams.z
#define AngleFade           DecalParams.w
#define FadeDistance        DecalParams2.x
#define AlbedoContribution  DecalParams2.y
#define NormalContribution  DecalParams2.z
#define RoughnessContribution DecalParams2.w

// GBuffer
Texture2D<float4> GBufferAlbedo : register(t0);
Texture2D<float4> GBufferNormal : register(t1);
Texture2D<float4> GBufferRoughness : register(t2);
Texture2D<float> DepthTexture : register(t3);

// Decal textures
Texture2D<float4> DecalAlbedo : register(t4);
Texture2D<float4> DecalNormal : register(t5);
Texture2D<float4> DecalRoughness : register(t6);

SamplerState LinearClampSampler : register(s0);

// =============================================================================
// Vertex shader (box mesh)
// =============================================================================

struct VSInput
{
    float3 position : POSITION;
};

struct VSOutput
{
    float4 position : SV_Position;
    float4 screenPos : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
};

VSOutput VS_Decal(VSInput input)
{
    VSOutput output;
    
    // Transform unit box vertex to world space
    float4 worldPos = mul(DecalLocalToWorld, float4(input.position, 1.0));
    output.worldPos = worldPos.xyz;
    
    // Project to clip space
    output.position = mul(ViewProj, worldPos);
    output.screenPos = output.position;
    
    return output;
}

// =============================================================================
// Pixel shader
// =============================================================================

float3 ReconstructWorldPosition(float2 uv, float depth)
{
    float4 clipPos = float4(uv * 2.0 - 1.0, depth, 1.0);
    clipPos.y = -clipPos.y;
    
    float4 worldPos = mul(InvViewProj, clipPos);
    return worldPos.xyz / worldPos.w;
}

float3 DecodeNormal(float3 normal)
{
    return normal * 2.0 - 1.0;
}

float3 EncodeNormal(float3 normal)
{
    return normal * 0.5 + 0.5;
}

// Transform normal from tangent space to world space
float3 TransformDecalNormal(float3 decalNormal, float3 worldNormal)
{
    // Create TBN matrix from world normal
    float3 up = abs(worldNormal.y) < 0.999 ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 tangent = normalize(cross(up, worldNormal));
    float3 bitangent = cross(worldNormal, tangent);
    
    // Transform
    float3x3 TBN = float3x3(tangent, bitangent, worldNormal);
    return normalize(mul(decalNormal, TBN));
}

struct PSOutput
{
    float4 albedo : SV_Target0;
    float4 normal : SV_Target1;
    float4 roughness : SV_Target2;
};

PSOutput PS_Decal(VSOutput input)
{
    PSOutput output;
    
    // Get screen UV
    float2 screenUV = input.screenPos.xy / input.screenPos.w * 0.5 + 0.5;
    screenUV.y = 1.0 - screenUV.y;
    
    // Sample depth and reconstruct world position
    float depth = DepthTexture.Sample(LinearClampSampler, screenUV);
    float3 worldPos = ReconstructWorldPosition(screenUV, depth);
    
    // Transform to decal local space
    float4 decalPos = mul(DecalWorldToLocal, float4(worldPos, 1.0));
    
    // Clip outside decal box
    float3 absPos = abs(decalPos.xyz);
    if (absPos.x > 0.5 || absPos.y > 0.5 || absPos.z > 0.5)
        discard;
    
    // Calculate decal UV (use XZ in local space, Y is projection direction)
    float2 decalUV = decalPos.xz + 0.5;
    
    // Sample existing GBuffer
    float4 existingAlbedo = GBufferAlbedo.Sample(LinearClampSampler, screenUV);
    float3 existingNormal = DecodeNormal(GBufferNormal.Sample(LinearClampSampler, screenUV).rgb);
    float4 existingRoughness = GBufferRoughness.Sample(LinearClampSampler, screenUV);
    
    // Angle fade (fade on surfaces facing away from decal projection)
    float angleFactor = 1.0;
    if (AngleFade > 0.0)
    {
        float3 decalUp = normalize(DecalLocalToWorld[1].xyz);
        float angleDot = abs(dot(existingNormal, decalUp));
        angleFactor = smoothstep(0.0, AngleFade, angleDot);
    }
    
    // Distance fade
    float distanceFactor = 1.0;
    if (FadeDistance > 0.0)
    {
        float dist = length(worldPos - CameraPosition.xyz);
        distanceFactor = 1.0 - saturate(dist / FadeDistance);
    }
    
    float fadeFactor = angleFactor * distanceFactor;
    
    // Sample decal textures
    float4 decalAlbedoSample = DecalAlbedo.Sample(LinearClampSampler, decalUV);
    
    // Combine alpha
    float alpha = decalAlbedoSample.a * DecalColor.a * fadeFactor;
    
    // Blend albedo
    output.albedo = float4(
        lerp(existingAlbedo.rgb, decalAlbedoSample.rgb * DecalColor.rgb, alpha * AlbedoContribution),
        existingAlbedo.a
    );
    
    // Blend normal
    float3 decalNormalSample = DecodeNormal(DecalNormal.Sample(LinearClampSampler, decalUV).rgb);
    decalNormalSample.xy *= NormalStrength;
    float3 blendedNormal = TransformDecalNormal(decalNormalSample, existingNormal);
    blendedNormal = normalize(lerp(existingNormal, blendedNormal, alpha * NormalContribution));
    output.normal = float4(EncodeNormal(blendedNormal), 1.0);
    
    // Blend roughness/metallic
    float4 decalRoughnessSample = DecalRoughness.Sample(LinearClampSampler, decalUV);
    output.roughness = float4(
        lerp(existingRoughness.r, decalRoughnessSample.r > 0 ? decalRoughnessSample.r : Roughness, alpha * RoughnessContribution),
        lerp(existingRoughness.g, Metallic, alpha * RoughnessContribution),
        existingRoughness.ba
    );
    
    return output;
}

// =============================================================================
// Stain mode (only affects albedo, preserves other properties)
// =============================================================================

float4 PS_DecalStain(VSOutput input) : SV_Target0
{
    float2 screenUV = input.screenPos.xy / input.screenPos.w * 0.5 + 0.5;
    screenUV.y = 1.0 - screenUV.y;
    
    float depth = DepthTexture.Sample(LinearClampSampler, screenUV);
    float3 worldPos = ReconstructWorldPosition(screenUV, depth);
    
    float4 decalPos = mul(DecalWorldToLocal, float4(worldPos, 1.0));
    
    float3 absPos = abs(decalPos.xyz);
    if (absPos.x > 0.5 || absPos.y > 0.5 || absPos.z > 0.5)
        discard;
    
    float2 decalUV = decalPos.xz + 0.5;
    
    float4 existingAlbedo = GBufferAlbedo.Sample(LinearClampSampler, screenUV);
    float4 decalAlbedoSample = DecalAlbedo.Sample(LinearClampSampler, decalUV);
    
    float alpha = decalAlbedoSample.a * DecalColor.a;
    
    // Stain mode: multiply colors
    return float4(
        existingAlbedo.rgb * lerp(float3(1,1,1), decalAlbedoSample.rgb * DecalColor.rgb, alpha),
        existingAlbedo.a
    );
}
