#include "Particle/ParticleComponent.h"
#include "Core/Log.h"
#include "Particle/ParticleSubsystem.h"
#include "Scene/SceneEntity.h"
#include <cstring>

namespace RVX::Particle
{

ParticleComponent::~ParticleComponent()
{
    DestroyInstance();
}

void ParticleComponent::OnAttach()
{
    if (m_particleSystem)
    {
        CreateInstance();
        
        if (m_autoPlay)
        {
            Play();
        }
    }
}

void ParticleComponent::OnDetach()
{
    DestroyInstance();
}

void ParticleComponent::Tick(float deltaTime)
{
    (void)deltaTime;
    
    if (m_instance)
    {
        UpdateTransform();
    }
}

AABB ParticleComponent::GetLocalBounds() const
{
    if (m_instance)
    {
        return m_instance->GetLocalBounds();
    }
    
    // Default bounds if no instance
    return AABB(Vec3(-1.0f), Vec3(1.0f));
}

void ParticleComponent::SetParticleSystem(ParticleSystem::Ptr system)
{
    if (m_particleSystem == system)
        return;

    DestroyInstance();
    m_particleSystem = system;

    if (GetOwner() && m_particleSystem)
    {
        CreateInstance();
        
        if (m_autoPlay)
        {
            Play();
        }
    }
}

void ParticleComponent::SetParticleSystemPath(const std::string& path)
{
    m_particleSystemPath = path;
    
    // Load asynchronously (would use ResourceManager)
    // ResourceManager::Get().LoadAsync<ParticleSystem>(path,
    //     [this](ParticleSystem::Ptr system) {
    //         SetParticleSystem(system);
    //     });
}

void ParticleComponent::CreateInstance()
{
    if (m_instance || !m_particleSystem)
        return;

    m_instanceOwnership = ParticleInstanceOwnership::None;
    m_instanceSubsystem = nullptr;

    if (auto* subsystem = ParticleSubsystem::GetActiveSubsystem())
    {
        if (subsystem->IsRenderIntegrationReady())
        {
            m_instance = subsystem->CreateInstance(m_particleSystem);
            if (m_instance)
            {
                m_instanceSubsystem = subsystem;
                m_instanceOwnership = ParticleInstanceOwnership::SubsystemOwned;
            }
        }
        else
        {
            RVX_CORE_WARN("ParticleComponent: ParticleSubsystem fallback for '{}': {}",
                          GetOwner() ? GetOwner()->GetName() : "<no-owner>",
                          subsystem->GetRenderIntegrationUnsupportedReason());
        }
    }

    if (!m_instance)
    {
        RVX_CORE_WARN("ParticleComponent: Using legacy direct particle instance fallback for '{}'",
                      GetOwner() ? GetOwner()->GetName() : "<no-owner>");
        m_instance = new ParticleSystemInstance(m_particleSystem);
        m_instanceOwnership = ParticleInstanceOwnership::LegacyFallback;
    }

    // Apply overrides
    if (m_instance)
    {
        m_instance->SetEmissionRateMultiplier(m_emissionRateOverride);
        m_instance->SetSimulationSpeedMultiplier(m_simulationSpeedOverride);
        m_instance->SetVisible(m_visible);
        m_instance->SetSimulateWhenHidden(m_simulateWhenHidden);

        if (m_hasStartColorOverride)
        {
            m_instance->SetStartColorOverride(m_startColorOverride);
        }
        if (m_hasStartSizeOverride)
        {
            m_instance->SetStartSizeOverride(m_startSizeOverride);
        }

        UpdateTransform();
    }
}

void ParticleComponent::DestroyInstance()
{
    if (m_instance)
    {
        if (m_instanceOwnership == ParticleInstanceOwnership::SubsystemOwned && m_instanceSubsystem)
        {
            m_instanceSubsystem->DestroyInstance(m_instance);
        }
        else
        {
            delete m_instance;
        }
        m_instance = nullptr;
    }

    m_instanceSubsystem = nullptr;
    m_instanceOwnership = ParticleInstanceOwnership::None;
}

void ParticleComponent::UpdateTransform()
{
    if (!m_instance)
        return;

    if (auto* owner = GetOwner())
    {
        m_instance->SetTransform(owner->GetWorldMatrix());
    }
}

void ParticleComponent::Play()
{
    if (m_instance)
    {
        m_instance->Play();
    }
}

void ParticleComponent::Stop()
{
    if (m_instance)
    {
        m_instance->Stop();
    }
}

void ParticleComponent::Pause()
{
    if (m_instance)
    {
        m_instance->Pause();
    }
}

void ParticleComponent::Resume()
{
    if (m_instance)
    {
        m_instance->Resume();
    }
}

void ParticleComponent::Clear()
{
    if (m_instance)
    {
        m_instance->Clear();
    }
}

void ParticleComponent::Restart()
{
    if (m_instance)
    {
        m_instance->Restart();
    }
}

bool ParticleComponent::IsPlaying() const
{
    return m_instance && m_instance->IsPlaying();
}

bool ParticleComponent::IsPaused() const
{
    return m_instance && m_instance->IsPaused();
}

bool ParticleComponent::IsStopped() const
{
    return !m_instance || m_instance->IsStopped();
}

void ParticleComponent::SetEmissionRate(float rate)
{
    m_emissionRateOverride = rate;
    if (m_instance)
    {
        m_instance->SetEmissionRateMultiplier(rate);
    }
}

void ParticleComponent::SetStartColor(const Vec4& color)
{
    m_startColorOverride = color;
    m_hasStartColorOverride = true;
    if (m_instance)
    {
        m_instance->SetStartColorOverride(color);
    }
}

void ParticleComponent::ClearStartColorOverride()
{
    m_hasStartColorOverride = false;
    if (m_instance)
    {
        m_instance->ClearStartColorOverride();
    }
}

void ParticleComponent::SetStartSize(float size)
{
    m_startSizeOverride = size;
    m_hasStartSizeOverride = true;
    if (m_instance)
    {
        m_instance->SetStartSizeOverride(size);
    }
}

void ParticleComponent::ClearStartSizeOverride()
{
    m_hasStartSizeOverride = false;
    if (m_instance)
    {
        m_instance->ClearStartSizeOverride();
    }
}

void ParticleComponent::SetSimulationSpeed(float speed)
{
    m_simulationSpeedOverride = speed;
    if (m_instance)
    {
        m_instance->SetSimulationSpeedMultiplier(speed);
    }
}

void ParticleComponent::SetVisible(bool visible)
{
    m_visible = visible;
    if (m_instance)
    {
        m_instance->SetVisible(visible);
    }
}

void ParticleComponent::SetSimulateWhenHidden(bool simulate)
{
    m_simulateWhenHidden = simulate;
    if (m_instance)
    {
        m_instance->SetSimulateWhenHidden(simulate);
    }
}

} // namespace RVX::Particle
