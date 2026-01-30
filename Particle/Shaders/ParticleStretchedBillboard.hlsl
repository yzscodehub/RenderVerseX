/**
 * @file ParticleStretchedBillboard.hlsl
 * @brief Velocity-stretched billboard particle shader
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

cbuffer StretchedParams : register(b1)
{
    float g_CameraVelocityScale;
    float g_SpeedScale;
    float g_LengthScale;
    float g_MinLength;
};

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

static const float2 g_Corners[4] = {
    float2(-0.5, -0.5),
    float2( 0.5, -0.5),
    float2(-0.5,  0.5),
    float2( 0.5,  0.5)
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
    
    uint particleIndex = g_AliveList[input.instanceID];
    GPUParticle p = g_Particles[particleIndex];
    
    // Calculate stretch direction and length
    float speed = length(p.velocity);
    float3 velocityDir = speed > 0.001 ? p.velocity / speed : g_Render.cameraForward.xyz;
    
    // Calculate stretch length
    float stretchLength = max(g_MinLength, speed * g_SpeedScale * g_LengthScale);
    
    // Calculate billboard axes
    float3 toCamera = normalize(g_Render.cameraPosition.xyz - p.position);
    float3 right = normalize(cross(velocityDir, toCamera));
    float3 up = velocityDir;
    
    // Get corner
    float2 corner = g_Corners[input.vertexID];
    
    // Calculate world position
    float3 worldPos = p.position;
    worldPos += right * corner.x * p.size.x;
    worldPos += up * corner.y * stretchLength;
    
    // Transform to clip space
    output.position = mul(g_Render.viewProjMatrix, float4(worldPos, 1.0));
    output.clipPos = output.position;
    output.color = p.color;
    output.uv = g_UVs[input.vertexID];
    
    float4 viewPos = mul(g_Render.viewMatrix, float4(worldPos, 1.0));
    output.viewDepth = -viewPos.z;
    
    return output;
}

PSOutput PSMain(VSOutput input)
{
    PSOutput output;
    
    float4 texColor = g_ParticleTexture.Sample(g_LinearSampler, input.uv);
    float4 finalColor = texColor * input.color;
    
    if (g_Render.softParticleEnabled)
    {
        float fade = ComputeSoftParticleFade(input.clipPos, input.viewDepth, 0.1, 1000.0);
        finalColor.a *= fade;
    }
    
    if (finalColor.a < 0.01)
        discard;
    
    output.color = finalColor;
    return output;
}
