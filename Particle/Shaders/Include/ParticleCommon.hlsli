/**
 * @file ParticleCommon.hlsli
 * @brief Common particle shader definitions and structures
 */

#ifndef PARTICLE_COMMON_HLSLI
#define PARTICLE_COMMON_HLSLI

// Particle flags
#define PARTICLE_FLAG_ALIVE     0x01
#define PARTICLE_FLAG_COLLISION 0x02
#define PARTICLE_FLAG_TRAIL     0x04

// GPU Particle structure (64 bytes, matches C++ GPUParticle)
struct GPUParticle
{
    float3 position;
    float lifetime;
    float3 velocity;
    float age;
    float4 color;
    float2 size;
    float rotation;
    uint flags;
};

// Emitter GPU data
struct EmitterData
{
    float4x4 transform;
    float4 shapeParams;
    float4 velocityParams;
    float4 lifetimeParams;
    float4 sizeParams;
    float4 colorStart;
    float4 rotationParams;
    uint emitterShape;
    uint emitCount;
    uint randomSeed;
    uint flags;
};

// Simulation constants
struct SimulationData
{
    float4 gravity;
    float4 forceParams;
    float4 noiseParams;
    float4 collisionPlanes[8];
    float deltaTime;
    float totalTime;
    uint aliveCount;
    uint maxParticles;
    uint collisionPlaneCount;
    float collisionBounce;
    float collisionLifetimeLoss;
    float collisionRadiusScale;
    uint noiseEnabled;
    uint collisionEnabled;
    uint2 pad;
};

// Render constants
struct RenderData
{
    float4x4 viewMatrix;
    float4x4 projMatrix;
    float4x4 viewProjMatrix;
    float4 cameraPosition;
    float4 cameraRight;
    float4 cameraUp;
    float4 cameraForward;
    float2 screenSize;
    float2 invScreenSize;
    float softParticleFadeDistance;
    float softParticleContrast;
    uint softParticleEnabled;
    uint pad;
};

// Random number generation (PCG)
uint PCGHash(uint input)
{
    uint state = input * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float Random(uint seed)
{
    return float(PCGHash(seed)) / 4294967295.0;
}

float3 Random3(uint seed)
{
    return float3(
        Random(seed),
        Random(seed + 1u),
        Random(seed + 2u)
    );
}

// Utility functions
float3 RotateVector(float3 v, float angle)
{
    float s = sin(angle);
    float c = cos(angle);
    return float3(v.x * c - v.y * s, v.x * s + v.y * c, v.z);
}

float3 SphericalToCartesian(float theta, float phi)
{
    float sinTheta = sin(theta);
    return float3(
        sinTheta * cos(phi),
        cos(theta),
        sinTheta * sin(phi)
    );
}

float3 RandomInSphere(uint seed)
{
    float u = Random(seed);
    float v = Random(seed + 1u);
    float theta = 2.0 * 3.14159265 * u;
    float phi = acos(2.0 * v - 1.0);
    float r = pow(Random(seed + 2u), 1.0 / 3.0);
    return r * SphericalToCartesian(phi, theta);
}

float3 RandomOnSphere(uint seed)
{
    float u = Random(seed);
    float v = Random(seed + 1u);
    float theta = 2.0 * 3.14159265 * u;
    float phi = acos(2.0 * v - 1.0);
    return SphericalToCartesian(phi, theta);
}

float3 RandomInCone(uint seed, float angle)
{
    float u = Random(seed);
    float v = Random(seed + 1u);
    float theta = angle * sqrt(u);
    float phi = 2.0 * 3.14159265 * v;
    return float3(sin(theta) * cos(phi), cos(theta), sin(theta) * sin(phi));
}

float3 RandomInBox(uint seed, float3 halfExtents)
{
    float3 r = Random3(seed);
    return (r * 2.0 - 1.0) * halfExtents;
}

#endif // PARTICLE_COMMON_HLSLI
