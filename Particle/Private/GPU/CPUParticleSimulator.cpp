#include "Particle/GPU/CPUParticleSimulator.h"
#include "Particle/Events/ParticleEventHandler.h"
#include "Core/Log.h"
#include "Core/Job/JobSystem.h"
#include <cmath>
#include <glm/glm.hpp>
#include <mutex>

namespace RVX::Particle
{

CPUParticleSimulator::~CPUParticleSimulator()
{
    Shutdown();
}

void CPUParticleSimulator::Initialize(IRHIDevice* device, uint32 maxParticles)
{
    if (m_initialized)
        Shutdown();

    m_device = device;
    m_maxParticles = maxParticles;

    // Initialize CPU buffers
    m_particles.resize(maxParticles);
    m_aliveIndices.reserve(maxParticles);
    m_deadIndices.reserve(maxParticles);

    // All particles start dead
    for (uint32 i = 0; i < maxParticles; ++i)
    {
        m_deadIndices.push_back(i);
        m_particles[i].flags = 0;
    }

    // Initialize RNG
    m_rng.seed(std::random_device{}());

    // Create GPU buffers for rendering
    RHIBufferDesc particleDesc;
    particleDesc.size = sizeof(GPUParticle) * maxParticles;
    particleDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource;
    particleDesc.stride = sizeof(GPUParticle);
    particleDesc.memoryType = RHIMemoryType::Upload;
    particleDesc.debugName = "CPUParticleBuffer";
    m_gpuParticleBuffer = m_device->CreateBuffer(particleDesc);

    RHIBufferDesc indexDesc;
    indexDesc.size = sizeof(uint32) * maxParticles;
    indexDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource;
    indexDesc.stride = sizeof(uint32);
    indexDesc.memoryType = RHIMemoryType::Upload;
    indexDesc.debugName = "CPUAliveIndexBuffer";
    m_gpuAliveIndexBuffer = m_device->CreateBuffer(indexDesc);

    RHIBufferDesc indirectDesc;
    indirectDesc.size = sizeof(IndirectDrawArgs);
    indirectDesc.usage = RHIBufferUsage::IndirectArgs;
    indirectDesc.memoryType = RHIMemoryType::Upload;
    indirectDesc.debugName = "CPUIndirectDrawBuffer";
    m_gpuIndirectDrawBuffer = m_device->CreateBuffer(indirectDesc);

    m_initialized = true;
    RVX_CORE_INFO("CPUParticleSimulator: Initialized with {} max particles", maxParticles);
}

void CPUParticleSimulator::Shutdown()
{
    m_particles.clear();
    m_aliveIndices.clear();
    m_deadIndices.clear();
    
    m_gpuParticleBuffer.Reset();
    m_gpuAliveIndexBuffer.Reset();
    m_gpuIndirectDrawBuffer.Reset();
    m_uploadBuffer.Reset();

    m_initialized = false;
    m_device = nullptr;
}

void CPUParticleSimulator::Emit(const EmitParams& params)
{
    if (params.emitCount == 0 || m_deadIndices.empty())
        return;

    uint32 toEmit = std::min(params.emitCount, static_cast<uint32>(m_deadIndices.size()));

    for (uint32 i = 0; i < toEmit; ++i)
    {
        uint32 particleIndex = m_deadIndices.back();
        m_deadIndices.pop_back();
        
        EmitParticle(params, particleIndex);
        m_aliveIndices.push_back(particleIndex);
    }

    m_gpuDirty = true;
}

void CPUParticleSimulator::EmitParticle(const EmitParams& params, uint32 index)
{
    CPUParticle& p = m_particles[index];
    const EmitterGPUData& data = params.emitterData;

    float r1 = m_dist(m_rng);
    float r2 = m_dist(m_rng);
    float r3 = m_dist(m_rng);
    float r4 = m_dist(m_rng);
    float r5 = m_dist(m_rng);

    // Position from emitter shape
    p.position = GenerateEmitterPosition(data, r1);

    // Velocity
    float speed = data.velocityParams.x + (data.velocityParams.y - data.velocityParams.x) * r2;
    p.velocity = GenerateEmitterVelocity(data, r1) * speed;

    // Lifetime
    p.lifetime = data.lifetimeParams.x + (data.lifetimeParams.y - data.lifetimeParams.x) * r3;
    p.age = 0.0f;

    // Size
    float size = data.sizeParams.x + (data.sizeParams.y - data.sizeParams.x) * r4;
    p.size = Vec2(size, size);
    p.startSize = p.size;

    // Color
    p.color = data.colorStart;
    p.startColor = p.color;

    // Rotation
    p.rotation = data.rotationParams.x + (data.rotationParams.y - data.rotationParams.x) * r5;
    p.rotationSpeed = data.rotationParams.z + (data.rotationParams.w - data.rotationParams.z) * r5;

    // Flags
    p.flags = PARTICLE_FLAG_ALIVE;
    p.randomSeed = params.randomSeed + index;
}

Vec3 CPUParticleSimulator::GenerateEmitterPosition(const EmitterGPUData& data, float random)
{
    constexpr float PI = 3.14159265359f;
    constexpr float TWO_PI = 2.0f * PI;
    
    Vec3 localPos{0.0f, 0.0f, 0.0f};
    EmitterShape shape = static_cast<EmitterShape>(data.emitterShape);
    
    // Generate additional random values
    float r1 = m_dist(m_rng);
    float r2 = m_dist(m_rng);
    float r3 = m_dist(m_rng);
    
    switch (shape)
    {
        case EmitterShape::Point:
            localPos = Vec3(0.0f, 0.0f, 0.0f);
            break;
            
        case EmitterShape::Box:
        {
            // shapeParams: xyz = halfExtents, w = emitFromSurface
            Vec3 halfExtents = Vec3(data.shapeParams);
            bool surfaceOnly = data.shapeParams.w > 0.5f;
            
            if (surfaceOnly)
            {
                // Emit from box surface
                int face = static_cast<int>(r1 * 6.0f);
                float u = r2 * 2.0f - 1.0f;
                float v = r3 * 2.0f - 1.0f;
                
                switch (face)
                {
                    case 0: localPos = Vec3( halfExtents.x, u * halfExtents.y, v * halfExtents.z); break;
                    case 1: localPos = Vec3(-halfExtents.x, u * halfExtents.y, v * halfExtents.z); break;
                    case 2: localPos = Vec3(u * halfExtents.x,  halfExtents.y, v * halfExtents.z); break;
                    case 3: localPos = Vec3(u * halfExtents.x, -halfExtents.y, v * halfExtents.z); break;
                    case 4: localPos = Vec3(u * halfExtents.x, v * halfExtents.y,  halfExtents.z); break;
                    case 5: localPos = Vec3(u * halfExtents.x, v * halfExtents.y, -halfExtents.z); break;
                }
            }
            else
            {
                // Emit from box volume
                localPos = Vec3(
                    (r1 * 2.0f - 1.0f) * halfExtents.x,
                    (r2 * 2.0f - 1.0f) * halfExtents.y,
                    (r3 * 2.0f - 1.0f) * halfExtents.z
                );
            }
            break;
        }
        
        case EmitterShape::Sphere:
        case EmitterShape::Hemisphere:
        {
            // shapeParams: x = radius, y = radiusThickness, z = emitFromShell, w = hemisphere
            float radius = data.shapeParams.x;
            float thickness = data.shapeParams.y;
            bool shellOnly = data.shapeParams.z > 0.5f;
            bool hemisphere = shape == EmitterShape::Hemisphere || data.shapeParams.w > 0.5f;
            
            // Generate point on unit sphere
            float theta = r1 * TWO_PI;
            float phi = hemisphere ? (r2 * PI * 0.5f) : (std::acos(2.0f * r2 - 1.0f));
            
            float sinPhi = std::sin(phi);
            Vec3 dir(
                sinPhi * std::cos(theta),
                std::cos(phi),
                sinPhi * std::sin(theta)
            );
            
            // Apply radius with thickness
            float r;
            if (shellOnly)
            {
                r = radius;
            }
            else
            {
                float minR = radius * (1.0f - thickness);
                r = minR + std::cbrt(r3) * (radius - minR);
            }
            
            localPos = dir * r;
            break;
        }
        
        case EmitterShape::Cone:
        {
            // shapeParams: x = angle (radians), y = baseRadius, z = length, w = flags
            float angle = data.shapeParams.x;
            float baseRadius = data.shapeParams.y;
            float length = data.shapeParams.z;
            uint32 flags = static_cast<uint32>(data.shapeParams.w);
            bool emitFromBase = (flags & 1u) != 0;
            bool emitFromVolume = (flags & 2u) != 0;
            
            // Position along cone
            float t = emitFromBase ? 0.0f : (emitFromVolume ? r1 : 0.0f);
            float currentRadius = baseRadius + t * length * std::tan(angle);
            
            // Random point in circle
            float circleR = std::sqrt(r2) * currentRadius;
            float circleTheta = r3 * TWO_PI;
            
            localPos = Vec3(
                circleR * std::cos(circleTheta),
                t * length,
                circleR * std::sin(circleTheta)
            );
            break;
        }
        
        case EmitterShape::Circle:
        {
            // shapeParams: x = radius, y = radiusThickness, z = arc (radians), w = unused
            float radius = data.shapeParams.x;
            float thickness = data.shapeParams.y;
            float arc = data.shapeParams.z;
            
            float theta = r1 * arc;
            float r = thickness > 0.0f 
                ? radius * (1.0f - thickness) + std::sqrt(r2) * radius * thickness
                : radius;
            
            localPos = Vec3(r * std::cos(theta), 0.0f, r * std::sin(theta));
            break;
        }
        
        case EmitterShape::Edge:
        {
            // shapeParams: x = length
            float length = data.shapeParams.x;
            localPos = Vec3((r1 - 0.5f) * length, 0.0f, 0.0f);
            break;
        }
        
        case EmitterShape::Mesh:
        {
            // Mesh emission requires vertex data - use origin as fallback
            // Real implementation would sample from mesh vertex buffer
            localPos = Vec3(0.0f, 0.0f, 0.0f);
            break;
        }
    }
    
    // Transform to world space
    Vec4 worldPos = data.transform * Vec4(localPos, 1.0f);
    return Vec3(worldPos);
}

Vec3 CPUParticleSimulator::GenerateEmitterVelocity(const EmitterGPUData& data, float random)
{
    constexpr float PI = 3.14159265359f;
    constexpr float TWO_PI = 2.0f * PI;
    
    Vec3 direction{0.0f, 1.0f, 0.0f};
    EmitterShape shape = static_cast<EmitterShape>(data.emitterShape);
    bool useShapeVelocity = data.velocityParams.z > 0.5f;
    
    float r1 = m_dist(m_rng);
    float r2 = m_dist(m_rng);
    
    if (useShapeVelocity)
    {
        switch (shape)
        {
            case EmitterShape::Point:
            {
                // Random direction for point emitter
                float theta = r1 * TWO_PI;
                float phi = std::acos(2.0f * r2 - 1.0f);
                float sinPhi = std::sin(phi);
                direction = Vec3(
                    sinPhi * std::cos(theta),
                    std::cos(phi),
                    sinPhi * std::sin(theta)
                );
                break;
            }
            
            case EmitterShape::Box:
            {
                // Random direction biased upward
                float theta = r1 * TWO_PI;
                float phi = r2 * PI * 0.5f;
                float sinPhi = std::sin(phi);
                direction = Vec3(
                    sinPhi * std::cos(theta),
                    std::cos(phi),
                    sinPhi * std::sin(theta)
                );
                break;
            }
            
            case EmitterShape::Sphere:
            case EmitterShape::Hemisphere:
            {
                // Use position normalized as direction (radial outward)
                float theta = random * TWO_PI;
                float phi = shape == EmitterShape::Hemisphere 
                    ? r1 * PI * 0.5f 
                    : std::acos(2.0f * r1 - 1.0f);
                float sinPhi = std::sin(phi);
                direction = Vec3(
                    sinPhi * std::cos(theta),
                    std::cos(phi),
                    sinPhi * std::sin(theta)
                );
                break;
            }
            
            case EmitterShape::Cone:
            {
                // Velocity along cone direction with spread
                float angle = data.shapeParams.x;
                float theta = r1 * TWO_PI;
                float phi = r2 * angle;
                float sinPhi = std::sin(phi);
                direction = Vec3(
                    sinPhi * std::cos(theta),
                    std::cos(phi),
                    sinPhi * std::sin(theta)
                );
                break;
            }
            
            case EmitterShape::Circle:
            {
                // Upward direction
                direction = Vec3(0.0f, 1.0f, 0.0f);
                break;
            }
            
            case EmitterShape::Edge:
            {
                // Perpendicular to edge
                direction = Vec3(0.0f, 1.0f, 0.0f);
                break;
            }
            
            case EmitterShape::Mesh:
            {
                // Would use mesh normal - default to up
                direction = Vec3(0.0f, 1.0f, 0.0f);
                break;
            }
        }
    }
    
    // Transform direction to world space
    Vec4 worldDir = data.transform * Vec4(direction, 0.0f);
    return normalize(Vec3(worldDir));
}

void CPUParticleSimulator::Simulate(float deltaTime, const SimulateParams& params)
{
    if (m_aliveIndices.empty())
        return;

    // Use parallel simulation if JobSystem is available
    if (m_aliveIndices.size() > 256)
    {
        SimulateParallel(deltaTime, params);
    }
    else
    {
        for (uint32 index : m_aliveIndices)
        {
            SimulateParticle(index, deltaTime, params);
        }
    }

    // Remove dead particles
    std::vector<uint32> newAlive;
    newAlive.reserve(m_aliveIndices.size());

    for (uint32 index : m_aliveIndices)
    {
        if (m_particles[index].flags & PARTICLE_FLAG_ALIVE)
        {
            newAlive.push_back(index);
        }
        else
        {
            m_deadIndices.push_back(index);
        }
    }

    m_aliveIndices = std::move(newAlive);
    m_gpuDirty = true;
}

void CPUParticleSimulator::SimulateParticle(uint32 index, float deltaTime, const SimulateParams& params)
{
    CPUParticle& p = m_particles[index];
    const SimulationGPUData& data = params.simulationData;

    // Update age
    p.age += deltaTime;

    // Check death
    if (p.age >= p.lifetime)
    {
        p.flags &= ~PARTICLE_FLAG_ALIVE;
        return;
    }

    float normalizedAge = p.age / p.lifetime;

    // Apply gravity and forces
    Vec3 gravity = Vec3(data.gravity);
    Vec3 force = Vec3(data.forceParams);
    float drag = data.forceParams.w;

    p.velocity += gravity * deltaTime;
    p.velocity += force * deltaTime;
    p.velocity *= (1.0f - drag * deltaTime);

    // Update position
    p.position += p.velocity * deltaTime;

    // Update rotation
    p.rotation += p.rotationSpeed * deltaTime;

    // Collision (simplified plane collision)
    if (data.collisionEnabled)
    {
        for (uint32 i = 0; i < data.collisionPlaneCount; ++i)
        {
            Vec4 plane = data.collisionPlanes[i];
            Vec3 normal = Vec3(plane);
            float dist = dot(p.position, normal) + plane.w;
            
            if (dist < p.size.x * data.collisionRadiusScale)
            {
                // Reflect velocity
                p.velocity = glm::reflect(p.velocity, normal) * data.collisionBounce;
                
                // Push out of plane
                p.position += normal * (p.size.x * data.collisionRadiusScale - dist);
                
                // Lifetime loss
                p.lifetime -= p.lifetime * data.collisionLifetimeLoss;
                
                // Set collision flag
                p.flags |= PARTICLE_FLAG_COLLISION;
            }
        }
    }

    // Color/size over lifetime (simplified - just fade alpha)
    p.color.w = p.startColor.w * (1.0f - normalizedAge);
}

void CPUParticleSimulator::SimulateWithModules(float deltaTime, const CPUSimulateParams& params)
{
    if (m_aliveIndices.empty())
        return;
    
    m_queuedEvents.clear();

    // Use parallel simulation if enough particles
    if (m_aliveIndices.size() > 256)
    {
        SimulateParallelWithModules(deltaTime, params);
    }
    else
    {
        for (uint32 index : m_aliveIndices)
        {
            SimulateParticleWithModules(index, deltaTime, params);
        }
    }

    // Remove dead particles and queue death events
    std::vector<uint32> newAlive;
    newAlive.reserve(m_aliveIndices.size());

    for (uint32 index : m_aliveIndices)
    {
        if (m_particles[index].flags & PARTICLE_FLAG_ALIVE)
        {
            newAlive.push_back(index);
        }
        else
        {
            m_deadIndices.push_back(index);
            
            // Queue death event
            CPUParticle& p = m_particles[index];
            m_queuedEvents.push_back(MakeDeathEvent(
                p.position, p.velocity, p.color,
                p.age, p.lifetime, index, p.emitterIndex, params.instanceId
            ));
        }
    }

    m_aliveIndices = std::move(newAlive);
    m_gpuDirty = true;
    
    // Dispatch events if handler provided
    if (params.eventHandler && !m_queuedEvents.empty())
    {
        params.eventHandler->DispatchEvents(m_queuedEvents);
    }
}

void CPUParticleSimulator::SimulateParticleWithModules(uint32 index, float deltaTime, const CPUSimulateParams& params)
{
    CPUParticle& p = m_particles[index];
    const SimulationGPUData& data = params.simulationData;

    // Update age
    p.age += deltaTime;

    // Check death
    if (p.age >= p.lifetime)
    {
        p.flags &= ~PARTICLE_FLAG_ALIVE;
        return;
    }

    float normalizedAge = p.age / p.lifetime;

    // Apply gravity and forces
    Vec3 gravity = Vec3(data.gravity);
    Vec3 force = Vec3(data.forceParams);
    float drag = data.forceParams.w;

    p.velocity += gravity * deltaTime;
    p.velocity += force * deltaTime;
    
    // Apply velocity over lifetime module
    if (params.velocityModule && params.velocityModule->enabled)
    {
        float speedMod = params.velocityModule->speedModifier.Evaluate(normalizedAge);
        p.velocity *= speedMod;
        
        // Add linear velocity
        Vec3 linearVel = params.velocityModule->linearVelocity;
        p.velocity += linearVel * deltaTime;
        
        // Apply radial velocity (towards/away from origin)
        if (std::abs(params.velocityModule->radialVelocity) > 0.001f)
        {
            Vec3 radialDir = normalize(p.position);
            if (length(radialDir) > 0.001f)
            {
                p.velocity += radialDir * params.velocityModule->radialVelocity * deltaTime;
            }
        }
    }
    
    // Apply drag
    p.velocity *= (1.0f - drag * deltaTime);
    
    // Apply noise module
    if (params.noiseModule && params.noiseModule->enabled)
    {
        float strength = params.noiseModule->strength;
        float frequency = params.noiseModule->frequency;
        float scrollSpeed = params.noiseModule->scrollSpeed;
        float posAmount = params.noiseModule->positionAmount;
        
        Vec3 noisePos = p.position * frequency + Vec3(0.0f, params.totalTime * scrollSpeed, 0.0f);
        Vec3 noiseOffset = SampleCurlNoise(noisePos, 0.01f);
        p.velocity += noiseOffset * strength * posAmount * deltaTime;
    }

    // Update position
    p.position += p.velocity * deltaTime;

    // Apply rotation over lifetime module
    if (params.rotationModule && params.rotationModule->enabled)
    {
        float angVelMod = params.rotationModule->angularVelocityCurve.Evaluate(normalizedAge);
        float angVel = mix(params.rotationModule->angularVelocity.min, 
                          params.rotationModule->angularVelocity.max,
                          static_cast<float>(p.randomSeed % 1000) / 1000.0f);
        p.rotation += angVel * angVelMod * deltaTime;
    }
    else
    {
        p.rotation += p.rotationSpeed * deltaTime;
    }

    // Collision detection
    bool collided = false;
    if (data.collisionEnabled)
    {
        for (uint32 i = 0; i < data.collisionPlaneCount; ++i)
        {
            Vec4 plane = data.collisionPlanes[i];
            Vec3 normal = Vec3(plane);
            float dist = dot(p.position, normal) + plane.w;
            float radius = p.size.x * data.collisionRadiusScale;
            
            if (dist < radius)
            {
                // Reflect velocity
                p.velocity = glm::reflect(p.velocity, normal) * data.collisionBounce;
                
                // Push out of plane
                p.position += normal * (radius - dist);
                
                // Lifetime loss
                p.lifetime -= p.lifetime * data.collisionLifetimeLoss;
                
                // Set collision flag
                p.flags |= PARTICLE_FLAG_COLLISION;
                collided = true;
                
                // Queue collision event
                m_queuedEvents.push_back(MakeCollisionEvent(
                    p.position, p.velocity, normal, p.color,
                    p.age, p.lifetime, index, p.emitterIndex, params.instanceId
                ));
            }
        }
    }

    // Apply color over lifetime module
    if (params.colorModule && params.colorModule->enabled)
    {
        p.color = params.colorModule->Evaluate(normalizedAge);
        // Modulate with start color
        p.color *= p.startColor;
    }
    else
    {
        // Default: fade alpha
        p.color = p.startColor;
        p.color.w *= (1.0f - normalizedAge);
    }

    // Apply size over lifetime module
    if (params.sizeModule && params.sizeModule->enabled)
    {
        Vec2 sizeMod = params.sizeModule->Evaluate(normalizedAge);
        p.size = p.startSize * sizeMod;
    }
}

void CPUParticleSimulator::SimulateParallelWithModules(float deltaTime, const CPUSimulateParams& params)
{
    JobSystem& jobs = JobSystem::Get();
    
    if (!jobs.IsInitialized())
    {
        // Fall back to serial
        for (uint32 index : m_aliveIndices)
        {
            SimulateParticleWithModules(index, deltaTime, params);
        }
        return;
    }

    // Thread-local event queues
    std::vector<std::vector<ParticleEvent>> threadEvents(jobs.GetWorkerCount() + 1);
    
    jobs.ParallelFor(0, m_aliveIndices.size(),
        [this, deltaTime, &params, &threadEvents](size_t i)
        {
            uint32 idx = m_aliveIndices[i];
            CPUParticle& p = m_particles[idx];
            const SimulationGPUData& data = params.simulationData;
            
            // ... inline simulation for performance (same as SimulateParticleWithModules)
            p.age += deltaTime;
            
            if (p.age >= p.lifetime)
            {
                p.flags &= ~PARTICLE_FLAG_ALIVE;
                return;
            }

            float normalizedAge = p.age / p.lifetime;
            
            // Forces
            Vec3 gravity = Vec3(data.gravity);
            Vec3 force = Vec3(data.forceParams);
            float drag = data.forceParams.w;
            
            p.velocity += gravity * deltaTime;
            p.velocity += force * deltaTime;
            p.velocity *= (1.0f - drag * deltaTime);
            p.position += p.velocity * deltaTime;
            p.rotation += p.rotationSpeed * deltaTime;
            
            // Collision
            if (data.collisionEnabled)
            {
                for (uint32 ci = 0; ci < data.collisionPlaneCount; ++ci)
                {
                    Vec4 plane = data.collisionPlanes[ci];
                    Vec3 normal = Vec3(plane);
                    float dist = dot(p.position, normal) + plane.w;
                    float radius = p.size.x * data.collisionRadiusScale;
                    
                    if (dist < radius)
                    {
                        p.velocity = glm::reflect(p.velocity, normal) * data.collisionBounce;
                        p.position += normal * (radius - dist);
                        p.lifetime -= p.lifetime * data.collisionLifetimeLoss;
                        p.flags |= PARTICLE_FLAG_COLLISION;
                    }
                }
            }
            
            // Color over lifetime
            if (params.colorModule && params.colorModule->enabled)
            {
                p.color = params.colorModule->Evaluate(normalizedAge) * p.startColor;
            }
            else
            {
                p.color = p.startColor;
                p.color.w *= (1.0f - normalizedAge);
            }
            
            // Size over lifetime
            if (params.sizeModule && params.sizeModule->enabled)
            {
                Vec2 sizeMod = params.sizeModule->Evaluate(normalizedAge);
                p.size = p.startSize * sizeMod;
            }
        });
}

// Perlin noise implementation for CPU
float CPUParticleSimulator::SamplePerlinNoise(const Vec3& pos) const
{
    // Simplified Perlin noise implementation
    auto fade = [](float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); };
    auto lerp = [](float a, float b, float t) { return a + t * (b - a); };
    
    // Simple hash function
    auto hash = [](int x, int y, int z) -> int {
        int n = x + y * 57 + z * 113;
        n = (n << 13) ^ n;
        return (n * (n * n * 15731 + 789221) + 1376312589) & 0x7fffffff;
    };
    
    auto grad = [](int h, float x, float y, float z) -> float {
        int b = h & 15;
        float u = b < 8 ? x : y;
        float v = b < 4 ? y : (b == 12 || b == 14 ? x : z);
        return ((b & 1) ? -u : u) + ((b & 2) ? -v : v);
    };
    
    int X = static_cast<int>(std::floor(pos.x)) & 255;
    int Y = static_cast<int>(std::floor(pos.y)) & 255;
    int Z = static_cast<int>(std::floor(pos.z)) & 255;
    
    float x = pos.x - std::floor(pos.x);
    float y = pos.y - std::floor(pos.y);
    float z = pos.z - std::floor(pos.z);
    
    float u = fade(x);
    float v = fade(y);
    float w = fade(z);
    
    int aaa = hash(X, Y, Z);
    int aba = hash(X, Y + 1, Z);
    int aab = hash(X, Y, Z + 1);
    int abb = hash(X, Y + 1, Z + 1);
    int baa = hash(X + 1, Y, Z);
    int bba = hash(X + 1, Y + 1, Z);
    int bab = hash(X + 1, Y, Z + 1);
    int bbb = hash(X + 1, Y + 1, Z + 1);
    
    float x1 = lerp(grad(aaa, x, y, z), grad(baa, x - 1, y, z), u);
    float x2 = lerp(grad(aba, x, y - 1, z), grad(bba, x - 1, y - 1, z), u);
    float y1 = lerp(x1, x2, v);
    
    float x3 = lerp(grad(aab, x, y, z - 1), grad(bab, x - 1, y, z - 1), u);
    float x4 = lerp(grad(abb, x, y - 1, z - 1), grad(bbb, x - 1, y - 1, z - 1), u);
    float y2 = lerp(x3, x4, v);
    
    return lerp(y1, y2, w);
}

Vec3 CPUParticleSimulator::SampleCurlNoise(const Vec3& pos, float epsilon) const
{
    Vec3 curl;
    
    float n1 = SamplePerlinNoise(pos + Vec3(0, epsilon, 0));
    float n2 = SamplePerlinNoise(pos - Vec3(0, epsilon, 0));
    float n3 = SamplePerlinNoise(pos + Vec3(0, 0, epsilon));
    float n4 = SamplePerlinNoise(pos - Vec3(0, 0, epsilon));
    float n5 = SamplePerlinNoise(pos + Vec3(epsilon, 0, 0));
    float n6 = SamplePerlinNoise(pos - Vec3(epsilon, 0, 0));
    
    curl.x = (n1 - n2) - (n3 - n4);
    curl.y = (n3 - n4) - (n5 - n6);
    curl.z = (n5 - n6) - (n1 - n2);
    
    return curl / (2.0f * epsilon);
}

void CPUParticleSimulator::SimulateParallel(float deltaTime, const SimulateParams& params)
{
    JobSystem& jobs = JobSystem::Get();
    
    if (!jobs.IsInitialized())
    {
        // Fall back to serial
        for (uint32 index : m_aliveIndices)
        {
            SimulateParticle(index, deltaTime, params);
        }
        return;
    }

    jobs.ParallelFor(0, m_aliveIndices.size(),
        [this, deltaTime, &params](size_t i)
        {
            SimulateParticle(m_aliveIndices[i], deltaTime, params);
        });
}

void CPUParticleSimulator::PrepareRender(RHICommandContext& ctx)
{
    (void)ctx;
    
    if (!m_gpuDirty)
        return;

    UploadToGPU();
    m_gpuDirty = false;
}

void CPUParticleSimulator::UploadToGPU()
{
    if (m_aliveIndices.empty())
        return;

    // Convert CPU particles to GPU format and upload
    std::vector<GPUParticle> gpuParticles(m_aliveIndices.size());
    
    for (size_t i = 0; i < m_aliveIndices.size(); ++i)
    {
        const CPUParticle& cpu = m_particles[m_aliveIndices[i]];
        GPUParticle& gpu = gpuParticles[i];
        
        gpu.position = cpu.position;
        gpu.lifetime = cpu.lifetime;
        gpu.velocity = cpu.velocity;
        gpu.age = cpu.age;
        gpu.color = cpu.color;
        gpu.size = cpu.size;
        gpu.rotation = cpu.rotation;
        gpu.flags = cpu.flags;
    }

    // Upload particle data
    m_gpuParticleBuffer->Upload(gpuParticles.data(), gpuParticles.size());

    // Upload alive indices (0, 1, 2, ... for sequential access)
    std::vector<uint32> indices(m_aliveIndices.size());
    for (size_t i = 0; i < indices.size(); ++i)
        indices[i] = static_cast<uint32>(i);
    m_gpuAliveIndexBuffer->Upload(indices.data(), indices.size());

    // Update indirect draw args
    IndirectDrawArgs args = {};
    args.indexCountPerInstance = 6;
    args.instanceCount = static_cast<uint32>(m_aliveIndices.size());
    args.startIndexLocation = 0;
    args.baseVertexLocation = 0;
    args.startInstanceLocation = 0;
    m_gpuIndirectDrawBuffer->Upload(&args, 1);
}

void CPUParticleSimulator::Clear()
{
    // Move all alive to dead
    for (uint32 index : m_aliveIndices)
    {
        m_particles[index].flags = 0;
        m_deadIndices.push_back(index);
    }
    m_aliveIndices.clear();
    m_gpuDirty = true;
}

} // namespace RVX::Particle
