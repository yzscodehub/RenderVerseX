/**
 * @file ParticleSimulate.hlsl
 * @brief Particle simulation compute shader
 */

#include "Include/ParticleCommon.hlsli"
#include "Include/NoiseUtils.hlsli"

// Buffers
RWStructuredBuffer<GPUParticle> g_Particles : register(u0);
RWStructuredBuffer<uint> g_AliveListIn : register(u1);
RWStructuredBuffer<uint> g_AliveListOut : register(u2);
RWStructuredBuffer<uint> g_DeadList : register(u3);
RWBuffer<uint> g_Counters : register(u4);  // [0]=aliveIn, [1]=aliveOut, [2]=dead

// Constants
ConstantBuffer<SimulationData> g_Simulation : register(b0);

// LUT textures for curves
Texture1D<float> g_SizeCurve : register(t0);
Texture1D<float4> g_ColorGradient : register(t1);
SamplerState g_LinearSampler : register(s0);

// Counter indices
#define COUNTER_ALIVE_IN 0
#define COUNTER_ALIVE_OUT 1
#define COUNTER_DEAD 2

[numthreads(256, 1, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    uint index = DTid.x;
    
    if (index >= g_Simulation.aliveCount)
        return;
    
    uint particleIndex = g_AliveListIn[index];
    GPUParticle p = g_Particles[particleIndex];
    
    // Skip dead particles
    if (!(p.flags & PARTICLE_FLAG_ALIVE))
        return;
    
    // Update age
    p.age += g_Simulation.deltaTime;
    
    // Check death
    if (p.age >= p.lifetime)
    {
        p.flags &= ~PARTICLE_FLAG_ALIVE;
        
        // Add to dead list
        uint deadIndex;
        InterlockedAdd(g_Counters[COUNTER_DEAD], 1, deadIndex);
        g_DeadList[deadIndex] = particleIndex;
        
        g_Particles[particleIndex] = p;
        return;
    }
    
    float normalizedAge = saturate(p.age / p.lifetime);
    
    // Apply gravity
    p.velocity += g_Simulation.gravity.xyz * g_Simulation.deltaTime;
    
    // Apply constant force
    p.velocity += g_Simulation.forceParams.xyz * g_Simulation.deltaTime;
    
    // Apply drag
    float drag = g_Simulation.forceParams.w;
    p.velocity *= (1.0 - drag * g_Simulation.deltaTime);
    
    // Apply noise
    if (g_Simulation.noiseEnabled)
    {
        float noiseStrength = g_Simulation.noiseParams.x;
        float noiseFrequency = g_Simulation.noiseParams.y;
        float noiseScroll = g_Simulation.noiseParams.z;
        int noiseOctaves = int(g_Simulation.noiseParams.w);
        
        float3 noisePos = p.position * noiseFrequency + g_Simulation.totalTime * noiseScroll;
        float3 noiseOffset = CurlNoise3D(noisePos, 0.01);
        p.velocity += noiseOffset * noiseStrength * g_Simulation.deltaTime;
    }
    
    // Collision detection
    if (g_Simulation.collisionEnabled)
    {
        for (uint i = 0; i < g_Simulation.collisionPlaneCount; i++)
        {
            float4 plane = g_Simulation.collisionPlanes[i];
            float3 normal = plane.xyz;
            float dist = dot(p.position, normal) + plane.w;
            float radius = p.size.x * g_Simulation.collisionRadiusScale;
            
            if (dist < radius)
            {
                // Reflect velocity
                p.velocity = reflect(p.velocity, normal) * g_Simulation.collisionBounce;
                
                // Push out of plane
                p.position += normal * (radius - dist);
                
                // Lifetime loss
                p.lifetime -= p.lifetime * g_Simulation.collisionLifetimeLoss;
                
                // Set collision flag
                p.flags |= PARTICLE_FLAG_COLLISION;
            }
        }
    }
    
    // Update position
    p.position += p.velocity * g_Simulation.deltaTime;
    
    // Sample color gradient
    p.color = g_ColorGradient.SampleLevel(g_LinearSampler, normalizedAge, 0);
    
    // Sample size curve
    float sizeMultiplier = g_SizeCurve.SampleLevel(g_LinearSampler, normalizedAge, 0);
    // p.size *= sizeMultiplier;  // Would need to store initial size
    
    // Write back
    g_Particles[particleIndex] = p;
    
    // Add to output alive list
    uint aliveOutIndex;
    InterlockedAdd(g_Counters[COUNTER_ALIVE_OUT], 1, aliveOutIndex);
    g_AliveListOut[aliveOutIndex] = particleIndex;
}
