/**
 * @file ParticleEmit.hlsl
 * @brief Particle emission compute shader
 */

#include "Include/ParticleCommon.hlsli"

// Buffers
RWStructuredBuffer<GPUParticle> g_Particles : register(u0);
RWStructuredBuffer<uint> g_DeadList : register(u1);
RWStructuredBuffer<uint> g_AliveList : register(u2);
RWBuffer<uint> g_Counters : register(u3);  // [0]=alive, [1]=dead, [2]=emit

// Constants
ConstantBuffer<EmitterData> g_Emitter : register(b0);

// Counter indices
#define COUNTER_ALIVE 0
#define COUNTER_DEAD 1
#define COUNTER_EMIT 2

// Emitter shape constants
#define EMITTER_SHAPE_POINT      0
#define EMITTER_SHAPE_BOX        1
#define EMITTER_SHAPE_SPHERE     2
#define EMITTER_SHAPE_HEMISPHERE 3
#define EMITTER_SHAPE_CONE       4
#define EMITTER_SHAPE_CIRCLE     5
#define EMITTER_SHAPE_EDGE       6
#define EMITTER_SHAPE_MESH       7

// Generate position based on emitter shape
float3 GeneratePosition(uint seed, EmitterData emitter)
{
    float3 localPos = float3(0, 0, 0);
    
    float r1 = Random(seed);
    float r2 = Random(seed + 1);
    float r3 = Random(seed + 2);
    
    switch (emitter.emitterShape)
    {
        case EMITTER_SHAPE_POINT:
            localPos = float3(0, 0, 0);
            break;
            
        case EMITTER_SHAPE_BOX:
        {
            // shapeParams: xyz = halfExtents, w = emitFromSurface
            float3 halfExtents = emitter.shapeParams.xyz;
            bool surfaceOnly = emitter.shapeParams.w > 0.5;
            
            if (surfaceOnly)
            {
                // Emit from box surface
                int face = int(r1 * 6.0);
                float u = r2 * 2.0 - 1.0;
                float v = r3 * 2.0 - 1.0;
                
                [flatten]
                switch (face)
                {
                    case 0: localPos = float3( halfExtents.x, u * halfExtents.y, v * halfExtents.z); break;
                    case 1: localPos = float3(-halfExtents.x, u * halfExtents.y, v * halfExtents.z); break;
                    case 2: localPos = float3(u * halfExtents.x,  halfExtents.y, v * halfExtents.z); break;
                    case 3: localPos = float3(u * halfExtents.x, -halfExtents.y, v * halfExtents.z); break;
                    case 4: localPos = float3(u * halfExtents.x, v * halfExtents.y,  halfExtents.z); break;
                    default: localPos = float3(u * halfExtents.x, v * halfExtents.y, -halfExtents.z); break;
                }
            }
            else
            {
                // Emit from box volume
                localPos = float3(
                    (r1 * 2.0 - 1.0) * halfExtents.x,
                    (r2 * 2.0 - 1.0) * halfExtents.y,
                    (r3 * 2.0 - 1.0) * halfExtents.z
                );
            }
            break;
        }
            
        case EMITTER_SHAPE_SPHERE:
        case EMITTER_SHAPE_HEMISPHERE:
        {
            // shapeParams: x = radius, y = radiusThickness, z = emitFromShell, w = hemisphere
            float radius = emitter.shapeParams.x;
            float thickness = emitter.shapeParams.y;
            bool shellOnly = emitter.shapeParams.z > 0.5;
            bool hemisphere = (emitter.emitterShape == EMITTER_SHAPE_HEMISPHERE) || (emitter.shapeParams.w > 0.5);
            
            // Generate point on unit sphere
            float theta = r1 * 6.28318530718;
            float phi = hemisphere ? (r2 * 1.57079632679) : acos(2.0 * r2 - 1.0);
            
            float sinPhi = sin(phi);
            float3 dir = float3(
                sinPhi * cos(theta),
                cos(phi),
                sinPhi * sin(theta)
            );
            
            // Apply radius with thickness
            float r;
            if (shellOnly)
            {
                r = radius;
            }
            else
            {
                float minR = radius * (1.0 - thickness);
                r = minR + pow(r3, 1.0 / 3.0) * (radius - minR);
            }
            
            localPos = dir * r;
            break;
        }
        
        case EMITTER_SHAPE_CONE:
        {
            // shapeParams: x = angle (radians), y = baseRadius, z = length, w = flags
            float angle = emitter.shapeParams.x;
            float baseRadius = emitter.shapeParams.y;
            float length = emitter.shapeParams.z;
            uint flags = uint(emitter.shapeParams.w);
            bool emitFromBase = (flags & 1u) != 0;
            bool emitFromVolume = (flags & 2u) != 0;
            
            // Position along cone
            float t = emitFromBase ? 0.0 : (emitFromVolume ? r1 : 0.0);
            float currentRadius = baseRadius + t * length * tan(angle);
            
            // Random point in circle
            float circleR = sqrt(r2) * currentRadius;
            float circleTheta = r3 * 6.28318530718;
            
            localPos = float3(
                circleR * cos(circleTheta),
                t * length,
                circleR * sin(circleTheta)
            );
            break;
        }
        
        case EMITTER_SHAPE_CIRCLE:
        {
            // shapeParams: x = radius, y = radiusThickness, z = arc (radians), w = unused
            float radius = emitter.shapeParams.x;
            float thickness = emitter.shapeParams.y;
            float arc = emitter.shapeParams.z;
            
            float theta = r1 * arc;
            float r = thickness > 0.0 
                ? radius * (1.0 - thickness) + sqrt(r2) * radius * thickness
                : radius;
            
            localPos = float3(r * cos(theta), 0.0, r * sin(theta));
            break;
        }
        
        case EMITTER_SHAPE_EDGE:
        {
            // shapeParams: x = length
            float length = emitter.shapeParams.x;
            localPos = float3((r1 - 0.5) * length, 0.0, 0.0);
            break;
        }
        
        case EMITTER_SHAPE_MESH:
        {
            // Mesh emission requires vertex buffer - fall back to origin
            // Real implementation would sample from mesh buffer
            localPos = float3(0, 0, 0);
            break;
        }
        
        default:
            localPos = float3(0, 0, 0);
            break;
    }
    
    // Transform to world space
    float3 worldPos = mul(float4(localPos, 1.0), emitter.transform).xyz;
    return worldPos;
}

// Generate velocity based on emitter shape
float3 GenerateVelocity(uint seed, EmitterData emitter, float3 localPos)
{
    float speed = lerp(emitter.velocityParams.x, emitter.velocityParams.y, Random(seed));
    float3 direction;
    
    float r1 = Random(seed + 10);
    float r2 = Random(seed + 11);
    
    if (emitter.velocityParams.z > 0.5) // Use shape velocity
    {
        switch (emitter.emitterShape)
        {
            case EMITTER_SHAPE_POINT:
            {
                // Random direction for point emitter
                float theta = r1 * 6.28318530718;
                float phi = acos(2.0 * r2 - 1.0);
                float sinPhi = sin(phi);
                direction = float3(
                    sinPhi * cos(theta),
                    cos(phi),
                    sinPhi * sin(theta)
                );
                break;
            }
            
            case EMITTER_SHAPE_BOX:
            {
                // Random direction biased upward
                float theta = r1 * 6.28318530718;
                float phi = r2 * 1.57079632679;
                float sinPhi = sin(phi);
                direction = float3(
                    sinPhi * cos(theta),
                    cos(phi),
                    sinPhi * sin(theta)
                );
                break;
            }
            
            case EMITTER_SHAPE_SPHERE:
            case EMITTER_SHAPE_HEMISPHERE:
            {
                // Outward from center
                direction = normalize(localPos);
                if (length(direction) < 0.001)
                {
                    // If at center, use random direction
                    float theta = r1 * 6.28318530718;
                    float phi = (emitter.emitterShape == EMITTER_SHAPE_HEMISPHERE) 
                        ? r2 * 1.57079632679 
                        : acos(2.0 * r2 - 1.0);
                    float sinPhi = sin(phi);
                    direction = float3(
                        sinPhi * cos(theta),
                        cos(phi),
                        sinPhi * sin(theta)
                    );
                }
                break;
            }
                
            case EMITTER_SHAPE_CONE:
            {
                // Velocity along cone direction with spread
                float angle = emitter.shapeParams.x;
                float theta = r1 * 6.28318530718;
                float phi = r2 * angle;
                float sinPhi = sin(phi);
                direction = float3(
                    sinPhi * cos(theta),
                    cos(phi),
                    sinPhi * sin(theta)
                );
                break;
            }
            
            case EMITTER_SHAPE_CIRCLE:
            {
                // Upward direction (perpendicular to circle plane)
                direction = float3(0, 1, 0);
                break;
            }
            
            case EMITTER_SHAPE_EDGE:
            {
                // Perpendicular to edge (upward in local space)
                direction = float3(0, 1, 0);
                break;
            }
            
            case EMITTER_SHAPE_MESH:
            {
                // Would use mesh normal - default to up
                direction = float3(0, 1, 0);
                break;
            }
            
            default:
                direction = float3(0, 1, 0);
                break;
        }
    }
    else
    {
        // Random direction in hemisphere (upward biased)
        float theta = r1 * 6.28318530718;
        float phi = r2 * 1.57079632679;
        float sinPhi = sin(phi);
        direction = float3(
            sinPhi * cos(theta),
            cos(phi),
            sinPhi * sin(theta)
        );
    }
    
    // Transform direction to world space
    direction = mul(float4(direction, 0.0), emitter.transform).xyz;
    float len = length(direction);
    if (len > 0.001)
        direction /= len;
    else
        direction = float3(0, 1, 0);
    
    return direction * speed;
}

[numthreads(64, 1, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    uint particleIndex = DTid.x;
    
    if (particleIndex >= g_Emitter.emitCount)
        return;
    
    // Get a slot from dead list
    uint deadIndex;
    InterlockedAdd(g_Counters[COUNTER_DEAD], -1, deadIndex);
    
    if (deadIndex == 0)
        return; // No available slots
    
    uint slot = g_DeadList[deadIndex - 1];
    
    // Generate random seed
    uint seed = g_Emitter.randomSeed + particleIndex * 7u;
    
    // Generate particle properties
    float3 localPos = float3(0, 0, 0);
    float3 position = GeneratePosition(seed, g_Emitter);
    float3 velocity = GenerateVelocity(seed + 5, g_Emitter, localPos);
    
    float lifetime = lerp(g_Emitter.lifetimeParams.x, g_Emitter.lifetimeParams.y, Random(seed + 20));
    float size = lerp(g_Emitter.sizeParams.x, g_Emitter.sizeParams.y, Random(seed + 30));
    float rotation = lerp(g_Emitter.rotationParams.x, g_Emitter.rotationParams.y, Random(seed + 40));
    
    // Initialize particle
    GPUParticle p;
    p.position = position;
    p.velocity = velocity;
    p.color = g_Emitter.colorStart;
    p.size = float2(size, size);
    p.rotation = rotation;
    p.lifetime = lifetime;
    p.age = 0.0;
    p.flags = PARTICLE_FLAG_ALIVE;
    
    g_Particles[slot] = p;
    
    // Add to alive list
    uint aliveIndex;
    InterlockedAdd(g_Counters[COUNTER_ALIVE], 1, aliveIndex);
    g_AliveList[aliveIndex] = slot;
}
