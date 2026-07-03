#include "Particle/ParticleComponent.h"
#include "Core/Log.h"
#include "Particle/ParticleSubsystem.h"
#include "Scene/SceneEntity.h"
#include <cstring>
#include <utility>

namespace RVX::Particle
{
namespace
{
    RVX::ParticleRenderSnapshotMode ToSnapshotRenderMode(ParticleRenderMode mode)
    {
        switch (mode)
        {
            case ParticleRenderMode::Billboard:
                return RVX::ParticleRenderSnapshotMode::Billboard;
            case ParticleRenderMode::StretchedBillboard:
                return RVX::ParticleRenderSnapshotMode::StretchedBillboard;
            case ParticleRenderMode::HorizontalBillboard:
                return RVX::ParticleRenderSnapshotMode::HorizontalBillboard;
            case ParticleRenderMode::VerticalBillboard:
                return RVX::ParticleRenderSnapshotMode::VerticalBillboard;
            case ParticleRenderMode::Mesh:
                return RVX::ParticleRenderSnapshotMode::Mesh;
            case ParticleRenderMode::Trail:
                return RVX::ParticleRenderSnapshotMode::Trail;
        }

        return RVX::ParticleRenderSnapshotMode::Billboard;
    }

    RVX::ParticleRenderSnapshotBlendMode ToSnapshotBlendMode(ParticleBlendMode mode)
    {
        switch (mode)
        {
            case ParticleBlendMode::Additive:
                return RVX::ParticleRenderSnapshotBlendMode::Additive;
            case ParticleBlendMode::AlphaBlend:
                return RVX::ParticleRenderSnapshotBlendMode::AlphaBlend;
            case ParticleBlendMode::Multiply:
                return RVX::ParticleRenderSnapshotBlendMode::Multiply;
            case ParticleBlendMode::Premultiplied:
                return RVX::ParticleRenderSnapshotBlendMode::Premultiplied;
        }

        return RVX::ParticleRenderSnapshotBlendMode::AlphaBlend;
    }

    RVX::ParticleRenderSnapshotSimulationBackend ToSnapshotSimulationBackend(const ParticleSystemInstance& instance)
    {
        if (!instance.IsSimulationSupported())
            return RVX::ParticleRenderSnapshotSimulationBackend::None;

        const std::string& backendName = instance.GetSimulationBackendName();
        if (backendName.find("GPU") != std::string::npos)
            return RVX::ParticleRenderSnapshotSimulationBackend::GPU;
        if (backendName.find("CPU") != std::string::npos)
            return RVX::ParticleRenderSnapshotSimulationBackend::CPU;

        return RVX::ParticleRenderSnapshotSimulationBackend::External;
    }
} // namespace

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
        m_instance = subsystem->CreateInstance(m_particleSystem);
        if (m_instance)
        {
            m_instanceSubsystem = subsystem;
            m_instanceOwnership = ParticleInstanceOwnership::SubsystemOwned;
        }
        if (!subsystem->IsRenderIntegrationReady() &&
            !subsystem->GetRenderIntegrationUnsupportedReason().empty())
        {
            RVX_CORE_INFO("ParticleComponent: ParticleSubsystem render path for '{}': {}",
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

bool ParticleComponent::AppendRenderFeatureSnapshot(RVX::RenderFeatureSnapshot& outSnapshot) const
{
    if (!m_instance)
    {
        outSnapshot.particles.skippedReasons.push_back("ParticleComponent has no runtime instance");
        return true;
    }

    if (!m_particleSystem)
    {
        outSnapshot.particles.skippedReasons.push_back("ParticleComponent has no particle system");
        return true;
    }

    if (!m_instance->IsPlaying())
    {
        outSnapshot.particles.skippedReasons.push_back("ParticleComponent instance is not playing");
        return true;
    }

    if (!m_instance->IsVisible())
    {
        outSnapshot.particles.skippedReasons.push_back("ParticleComponent instance is hidden");
        return true;
    }

    const uint32 aliveCount = m_instance->GetAliveCount();
    if (aliveCount == 0)
    {
        outSnapshot.particles.skippedReasons.push_back("ParticleComponent instance has no alive particles");
        return true;
    }

    RVX::ParticleRenderSnapshotItem item;
    item.instanceId = m_instance->GetInstanceId();
    item.systemId = m_particleSystem->id;
    item.systemName = m_particleSystem->name;
    item.worldMatrix = m_instance->GetTransform();
    item.worldBounds = m_instance->GetWorldBounds();
    item.position = m_instance->GetPosition();
    item.renderMode = ToSnapshotRenderMode(m_particleSystem->renderMode);
    item.blendMode = ToSnapshotBlendMode(m_particleSystem->blendMode);
    item.simulationBackend = ToSnapshotSimulationBackend(*m_instance);
    item.payloadStatus = RVX::ParticleRenderSnapshotPayloadStatus::MetadataOnly;
    item.aliveParticleCount = aliveCount;
    item.maxParticleCount = m_instance->GetMaxParticles();
    item.lodLevel = m_instance->GetCurrentLODLevel();
    item.normalizedTime = m_instance->GetNormalizedTime();
    item.visible = m_instance->IsVisible();
    item.simulationSupported = m_instance->IsSimulationSupported();
    item.renderPayloadAvailable = false;
    item.sortingSupported = false;
    item.softParticlesEnabled = m_particleSystem->softParticleConfig.enabled;
    item.softParticleFadeDistance = m_particleSystem->softParticleConfig.fadeDistance;
    item.renderPayloadReason =
        "Particle snapshot contains metadata only; Render-owned particle draw data extraction is not connected";
    item.sortingReason =
        "Particle sorting is deferred to Render-owned feature passes";
    if (!item.simulationSupported)
    {
        item.unsupportedReason = m_instance->GetSimulationUnsupportedReason();
    }

    outSnapshot.particles.metadata.totalAliveParticles += aliveCount;
    outSnapshot.particles.items.push_back(std::move(item));
    return true;
}

} // namespace RVX::Particle
