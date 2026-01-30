/**
 * @file ParticleBillboard.hlsl
 * @brief Billboard particle rendering shader
 */

#include "Include/ParticleCommon.hlsli"
#include "Include/SoftParticle.hlsli"

// Particle buffer
StructuredBuffer<GPUParticle> g_Particles : register(t0);
StructuredBuffer<uint> g_AliveList : register(t1);

// Particle texture
Texture2D g_ParticleTexture : register(t2);
SamplerState g_LinearSampler : register(s0);

// Constants
ConstantBuffer<RenderData> g_Render : register(b0);

struct VSInput
{
    uint vertexID : SV_VertexID;
    uint instanceID : SV_InstanceID;
};

struct VSOutput
{
    float4 position : SV_Position;
    float4 clipPos : TEXCOORD0;
    float4 color : COLOR;
    float2 uv : TEXCOORD1;
    float viewDepth : TEXCOORD2;
};

struct PSOutput
{
    float4 color : SV_Target0;
};

// Billboard corners
static const float2 g_Corners[4] = {
    float2(-1, -1),  // Bottom-left
    float2( 1, -1),  // Bottom-right
    float2(-1,  1),  // Top-left
    float2( 1,  1)   // Top-right
};

static const float2 g_UVs[4] = {
    float2(0, 1),
    float2(1, 1),
    float2(0, 0),
    float2(1, 0)
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    
    // Get particle data
    uint particleIndex = g_AliveList[input.instanceID];
    GPUParticle p = g_Particles[particleIndex];
    
    // Get corner
    float2 corner = g_Corners[input.vertexID];
    
    // Apply rotation
    float s = sin(p.rotation);
    float c = cos(p.rotation);
    float2 rotatedCorner = float2(
        corner.x * c - corner.y * s,
        corner.x * s + corner.y * c
    );
    
    // Calculate world position (camera-facing billboard)
    float3 worldPos = p.position;
    worldPos += g_Render.cameraRight.xyz * rotatedCorner.x * p.size.x;
    worldPos += g_Render.cameraUp.xyz * rotatedCorner.y * p.size.y;
    
    // Transform to clip space
    output.position = mul(g_Render.viewProjMatrix, float4(worldPos, 1.0));
    output.clipPos = output.position;
    output.color = p.color;
    output.uv = g_UVs[input.vertexID];
    
    // Calculate view depth for soft particles
    float4 viewPos = mul(g_Render.viewMatrix, float4(worldPos, 1.0));
    output.viewDepth = -viewPos.z;
    
    return output;
}

PSOutput PSMain(VSOutput input)
{
    PSOutput output;
    
    // Sample texture
    float4 texColor = g_ParticleTexture.Sample(g_LinearSampler, input.uv);
    
    // Apply particle color
    float4 finalColor = texColor * input.color;
    
    // Apply soft particle fade
    if (g_Render.softParticleEnabled)
    {
        float fade = ComputeSoftParticleFade(
            input.clipPos, 
            input.viewDepth, 
            0.1,    // Near plane (should come from constants)
            1000.0  // Far plane
        );
        finalColor.a *= fade;
    }
    
    // Alpha test
    if (finalColor.a < 0.01)
        discard;
    
    output.color = finalColor;
    return output;
}
