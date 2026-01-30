#include "Particle/ParticleSystemInstance.h"
#include "Particle/Events/ParticleEventHandler.h"
#include "Particle/GPU/IParticleSimulator.h"
#include <algorithm>
#include <cmath>

namespace RVX::Particle
{

uint64 ParticleSystemInstance::s_nextInstanceId = 1;

ParticleSystemInstance::ParticleSystemInstance()
    : m_instanceId(s_nextInstanceId++)
{
    m_eventHandler = std::make_unique<ParticleEventHandler>();
}

ParticleSystemInstance::ParticleSystemInstance(ParticleSystem::Ptr system)
    : ParticleSystemInstance()
{
    SetSystem(system);
}

ParticleSystemInstance::~ParticleSystemInstance() = default;

ParticleSystemInstance::ParticleSystemInstance(ParticleSystemInstance&&) noexcept = default;
ParticleSystemInstance& ParticleSystemInstance::operator=(ParticleSystemInstance&&) noexcept = default;

void ParticleSystemInstance::SetSystem(ParticleSystem::Ptr system)
{
    if (m_system == system)
        return;
    
    Stop();
    m_system = system;
    
    if (m_system)
    {
        Initialize();
    }
}

void ParticleSystemInstance::Initialize()
{
    // Simulator is created by ParticleSubsystem based on capabilities
    m_aliveCount = 0;
    m_simulationTime = 0.0f;
    m_emissionAccumulator = 0.0f;
    m_boundsDirty = true;
}

void ParticleSystemInstance::Play()
{
    if (!m_system)
        return;
    
    if (m_playbackState == PlaybackState::Stopped)
    {
        m_simulationTime = 0.0f;
        m_emissionAccumulator = 0.0f;
        
        // Handle prewarm
        if (m_system->prewarm)
        {
            Prewarm(m_system->prewarmTime);
        }
    }
    
    m_playbackState = PlaybackState::Playing;
}

void ParticleSystemInstance::Stop()
{
    m_playbackState = PlaybackState::Stopped;
    Clear();
    m_simulationTime = 0.0f;
}

void ParticleSystemInstance::Pause()
{
    if (m_playbackState == PlaybackState::Playing)
    {
        m_playbackState = PlaybackState::Paused;
    }
}

void ParticleSystemInstance::Resume()
{
    if (m_playbackState == PlaybackState::Paused)
    {
        m_playbackState = PlaybackState::Playing;
    }
}

void ParticleSystemInstance::Clear()
{
    m_aliveCount = 0;
    m_emissionAccumulator = 0.0f;
    
    if (m_simulator)
    {
        // Clear simulator state
    }
    
    m_boundsDirty = true;
}

void ParticleSystemInstance::Restart()
{
    Stop();
    Play();
}

bool ParticleSystemInstance::IsFinished() const
{
    if (!m_system || m_system->looping)
        return false;
    
    // Non-looping system is finished when time exceeds duration and no particles alive
    return m_simulationTime >= m_system->duration && m_aliveCount == 0;
}

void ParticleSystemInstance::Simulate(float deltaTime)
{
    if (!m_system || m_playbackState != PlaybackState::Playing)
        return;
    
    // Apply simulation speed
    float effectiveDeltaTime = deltaTime * m_system->simulationSpeed * m_simulationSpeedMultiplier;
    
    // Handle visibility
    if (!m_visible && !m_simulateWhenHidden)
        return;
    
    // Update simulation time
    m_simulationTime += effectiveDeltaTime;
    
    // Check for non-looping completion
    if (!m_system->looping && m_simulationTime >= m_system->duration)
    {
        // Stop emitting but continue simulating existing particles
        effectiveDeltaTime = deltaTime * m_system->simulationSpeed * m_simulationSpeedMultiplier;
    }
    
    // Emission
    if (m_system->looping || m_simulationTime < m_system->duration)
    {
        UpdateEmission(effectiveDeltaTime);
    }
    
    // Simulation would be done by the simulator
    if (m_simulator)
    {
        // m_simulator->Simulate(effectiveDeltaTime, params);
        // m_aliveCount = m_simulator->GetAliveCount();
    }
    
    m_boundsDirty = true;
}

void ParticleSystemInstance::Prewarm(float duration)
{
    if (!m_system)
        return;
    
    const float step = 1.0f / 60.0f;  // 60 FPS simulation
    for (float t = 0.0f; t < duration; t += step)
    {
        Simulate(step);
    }
}

float ParticleSystemInstance::GetNormalizedTime() const
{
    if (!m_system || m_system->duration <= 0.0f)
        return 0.0f;
    
    if (m_system->looping)
    {
        return std::fmod(m_simulationTime, m_system->duration) / m_system->duration;
    }
    
    return std::clamp(m_simulationTime / m_system->duration, 0.0f, 1.0f);
}

uint32 ParticleSystemInstance::GetMaxParticles() const
{
    return m_system ? m_system->maxParticles : 0;
}

bool ParticleSystemInstance::IsAtCapacity() const
{
    return m_aliveCount >= GetMaxParticles();
}

void ParticleSystemInstance::SetTransform(const Mat4& transform)
{
    m_worldTransform = transform;
    
    // Extract position from transform
    m_position = Vec3(transform[3]);
}

Vec3 ParticleSystemInstance::GetPosition() const
{
    return m_position;
}

void ParticleSystemInstance::SetPosition(const Vec3& position)
{
    m_position = position;
    m_worldTransform = MakeTranslation(position) * 
                       QuatToMat4(QuatFromEuler(radians(m_rotation))) *
                       MakeScale(m_scale);
}

void ParticleSystemInstance::SetRotation(const Vec3& rotation)
{
    m_rotation = rotation;
    m_worldTransform = MakeTranslation(m_position) * 
                       QuatToMat4(QuatFromEuler(radians(rotation))) *
                       MakeScale(m_scale);
}

void ParticleSystemInstance::SetScale(const Vec3& scale)
{
    m_scale = scale;
    m_worldTransform = MakeTranslation(m_position) * 
                       QuatToMat4(QuatFromEuler(radians(m_rotation))) *
                       MakeScale(scale);
}

AABB ParticleSystemInstance::GetWorldBounds() const
{
    AABB local = GetLocalBounds();
    // Transform bounds to world space
    // (simplified - proper implementation would transform all 8 corners)
    Vec3 center = Vec3(m_worldTransform * Vec4(local.GetCenter(), 1.0f));
    Vec3 halfSize = local.GetExtent() * m_scale;
    return AABB(center - halfSize, center + halfSize);
}

AABB ParticleSystemInstance::GetLocalBounds() const
{
    if (!m_system)
        return AABB();
    
    if (m_system->boundsMode == ParticleSystem::BoundsMode::Custom)
    {
        Vec3 halfSize = m_system->customBoundsSize * 0.5f;
        return AABB(m_system->customBoundsCenter - halfSize,
                    m_system->customBoundsCenter + halfSize);
    }
    
    return m_localBounds;
}

void ParticleSystemInstance::UpdateBounds()
{
    // Would calculate bounds from particle positions
    m_boundsDirty = false;
}

void ParticleSystemInstance::UpdateLOD(float distanceToCamera)
{
    if (!m_system || !m_system->lodConfig.enabled)
    {
        m_currentLODLevel = 0;
        return;
    }
    
    if (m_forcedLODLevel >= 0)
    {
        m_currentLODLevel = static_cast<uint32>(m_forcedLODLevel);
        return;
    }
    
    m_currentLODLevel = m_system->lodConfig.GetLevelIndex(distanceToCamera);
}

void ParticleSystemInstance::UpdateEmission(float deltaTime)
{
    if (!m_system)
        return;
    
    // Get LOD multiplier
    float lodMultiplier = 1.0f;
    if (m_system->lodConfig.enabled && !m_system->lodConfig.levels.empty())
    {
        lodMultiplier = m_system->lodConfig.levels[m_currentLODLevel].emissionRateMultiplier;
    }
    
    // Calculate particles to emit this frame
    for (const auto& emitter : m_system->emitters)
    {
        if (!emitter->enabled)
            continue;
        
        float rate = emitter->emissionRate * m_emissionRateMultiplier * lodMultiplier;
        m_emissionAccumulator += rate * deltaTime;
        
        uint32 toEmit = static_cast<uint32>(m_emissionAccumulator);
        m_emissionAccumulator -= static_cast<float>(toEmit);
        
        // Clamp to available space
        uint32 available = GetMaxParticles() - m_aliveCount;
        toEmit = std::min(toEmit, available);
        
        // Emit particles would be done via simulator
        // m_simulator->Emit(toEmit, emitter->GetEmitParams());
    }
}

void ParticleSystemInstance::ApplyOverrides()
{
    // Applied during emission/simulation
}

} // namespace RVX::Particle
