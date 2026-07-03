#include "Particle/ParticleSubsystem.h"
#include "Core/Log.h"
#include "Particle/GPU/CPUParticleSimulator.h"
#include "Particle/GPU/IParticleSimulator.h"
#include "Particle/ParticleSubsystemRenderAccess.h"
#include "Particle/Rendering/ParticlePass.h"
#include "Particle/Rendering/ParticleRenderer.h"
#include "Resource/ResourceSubsystem.h"
#include <algorithm>
#include <utility>

namespace RVX::Particle
{
struct ParticleSubsystemRenderState
{
    IRHIDevice* device = nullptr;
    IParticleRenderIntegrationHost* renderHost = nullptr;
    std::unique_ptr<ParticleRendererConfig> rendererConfigOverride;
    std::unique_ptr<ParticleRenderer> renderer;
    ParticlePass* renderPass = nullptr;
};

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

ParticleSubsystem* ParticleSubsystem::s_activeSubsystem = nullptr;

ParticleSubsystem::ParticleSubsystem()
    : m_renderState(std::make_unique<ParticleSubsystemRenderState>())
{
}

void ParticleSubsystemRenderAccess::SetDeviceForTesting(ParticleSubsystem& subsystem, IRHIDevice* device)
{
    subsystem.m_renderState->device = device;
}

void ParticleSubsystemRenderAccess::SetRenderHostForTesting(ParticleSubsystem& subsystem,
                                                            IParticleRenderIntegrationHost* host)
{
    subsystem.m_renderState->renderHost = host;
}

void ParticleSubsystemRenderAccess::SetRendererConfigForTesting(ParticleSubsystem& subsystem,
                                                                const ParticleRendererConfig& config)
{
    subsystem.m_renderState->rendererConfigOverride = std::make_unique<ParticleRendererConfig>(config);
}

ParticlePass* ParticleSubsystemRenderAccess::GetRenderPassForTesting(ParticleSubsystem& subsystem)
{
    return subsystem.m_renderState->renderPass;
}

const ParticlePass* ParticleSubsystemRenderAccess::GetRenderPassForTesting(const ParticleSubsystem& subsystem)
{
    return subsystem.m_renderState->renderPass;
}

ParticleSubsystem::~ParticleSubsystem()
{
    Deinitialize();
}

ParticleSubsystem* ParticleSubsystem::GetActiveSubsystem()
{
    return s_activeSubsystem;
}

std::vector<SubsystemDependency> ParticleSubsystem::GetTypedDependencies() const
{
    return MakeDependencies<ResourceSubsystem>();
}

void ParticleSubsystem::Initialize()
{
    s_activeSubsystem = this;
    m_renderIntegrationReady = false;
    m_renderIntegrationUnsupportedReason = "Particle render integration is not initialized";

    if (!m_renderState->device)
    {
        MarkRenderIntegrationUnsupported("No RHI device available");
        RVX_CORE_ERROR("ParticleSubsystem: {}", m_renderIntegrationUnsupportedReason);
        return;
    }

    // Check GPU capabilities
    CheckCapabilities();

    // Create rendering components
    CreateRenderComponents();
    RegisterRenderIntegration();

    m_instances.reserve(m_config.maxInstances);
    m_visibleInstances.reserve(m_config.maxInstances);

    RVX_CORE_INFO("ParticleSubsystem: Initialized (GPU={}, MaxParticles={}, RenderIntegration={})",
                  m_gpuSimulationSupported,
                  m_config.maxGlobalParticles,
                  m_renderIntegrationReady);
}

void ParticleSubsystem::CheckCapabilities()
{
    if (!m_renderState->device)
        return;

    m_gpuSimulationSupported = false;

    if (m_config.enableGPUSimulation)
    {
        RVX_CORE_WARN("ParticleSubsystem: GPU particle simulation is unsupported until simulator pipelines are connected");
    }
}

const ParticleRendererDrawStats& ParticleSubsystem::GetLastRenderDrawStats() const
{
    static const ParticleRendererDrawStats emptyStats;
    return m_renderState->renderer ? m_renderState->renderer->GetLastDrawStats() : emptyStats;
}

void ParticleSubsystem::CreateRenderComponents()
{
    // Create renderer
    m_renderState->renderer = std::make_unique<ParticleRenderer>();
    if (m_renderState->rendererConfigOverride)
    {
        m_renderState->renderer->Initialize(m_renderState->device, *m_renderState->rendererConfigOverride);
    }
    else
    {
        ParticleRendererConfig rendererConfig;
        m_renderState->renderer->Initialize(m_renderState->device, rendererConfig);
    }

    if (m_config.enableSorting)
    {
        RVX_CORE_WARN("ParticleSubsystem: particle sorting is deferred to Render-owned feature passes");
    }
}

void ParticleSubsystem::RegisterRenderIntegration()
{
    if (!m_renderState->renderHost)
    {
        MarkRenderIntegrationUnsupported("Particle render integration host is unavailable for pass registration");
        RVX_CORE_WARN("ParticleSubsystem: {}", m_renderIntegrationUnsupportedReason);
        return;
    }

    if (!m_renderState->renderer || !m_renderState->renderer->IsRenderingSupported())
    {
        const std::string reason = m_renderState->renderer && !m_renderState->renderer->GetUnsupportedReason().empty()
                                       ? "Particle renderer is unsupported: " + m_renderState->renderer->GetUnsupportedReason()
                                       : "Particle renderer is unavailable for pass registration";
        MarkRenderIntegrationUnsupported(reason);
        RVX_CORE_WARN("ParticleSubsystem: {}", m_renderIntegrationUnsupportedReason);
        return;
    }

    auto renderPass = std::make_unique<ParticlePass>();
    renderPass->SetRenderer(m_renderState->renderer.get());
    renderPass->SetSortingEnabled(m_config.enableSorting);
    renderPass->SetSoftParticlesEnabled(m_config.enableSoftParticles);

    m_renderState->renderPass = renderPass.get();
    if (!m_renderState->renderHost->AddParticlePass(std::move(renderPass)))
    {
        m_renderState->renderPass = nullptr;
        MarkRenderIntegrationUnsupported("Particle render integration host rejected particle pass registration");
        RVX_CORE_WARN("ParticleSubsystem: {}", m_renderIntegrationUnsupportedReason);
        return;
    }

    m_renderPassRegistered = true;
    m_stats.renderPassRegistered = true;

    if (!m_renderState->renderHost->AddPreGraphPrepareCallback(
            this,
            [this](const Vec3& cameraPosition)
            {
                if (m_renderIntegrationReady)
                {
                    PrepareRenderForCamera(cameraPosition);
                }
                else
                {
                    ++m_stats.skippedPrepareFrameCount;
                }
            }))
    {
        m_renderState->renderHost->RemoveParticlePass("ParticlePass");
        m_renderState->renderPass = nullptr;
        m_renderPassRegistered = false;
        m_stats.renderPassRegistered = false;
        MarkRenderIntegrationUnsupported("Particle pre-graph prepare callback could not be registered");
        RVX_CORE_WARN("ParticleSubsystem: {}", m_renderIntegrationUnsupportedReason);
        return;
    }

    m_preGraphCallbackRegistered = true;
    m_stats.preGraphCallbackRegistered = true;
    m_renderIntegrationReady = true;
    m_renderIntegrationUnsupportedReason.clear();
}

void ParticleSubsystem::MarkRenderIntegrationUnsupported(const std::string& reason)
{
    m_renderIntegrationReady = false;
    m_renderIntegrationUnsupportedReason = reason.empty() ? "Particle render integration is unavailable" : reason;
}

void ParticleSubsystem::Deinitialize()
{
    if (s_activeSubsystem == this)
    {
        s_activeSubsystem = nullptr;
    }

    if (m_renderState->renderHost && m_preGraphCallbackRegistered)
    {
        m_renderState->renderHost->RemovePreGraphPrepareCallback(this);
    }
    m_preGraphCallbackRegistered = false;
    m_stats.preGraphCallbackRegistered = false;

    MarkRenderIntegrationUnsupported("Particle subsystem is deinitialized");

    if (m_renderState->renderHost && m_renderPassRegistered)
    {
        m_renderState->renderHost->RemoveParticlePass("ParticlePass");
    }
    m_renderPassRegistered = false;
    m_stats.renderPassRegistered = false;
    m_renderState->renderPass = nullptr;

    m_instances.clear();
    m_visibleInstances.clear();
    m_pool.Clear();
    m_stats.activeInstances = 0;
    m_stats.visibleInstances = 0;
    m_stats.totalParticles = 0;
    m_stats.gpuSimulatedParticles = 0;
    m_stats.cpuSimulatedParticles = 0;

    m_renderState->renderer.reset();

    m_renderState->renderHost = nullptr;
    m_renderState->device = nullptr;

    RVX_CORE_INFO("ParticleSubsystem: Deinitialized");
}

void ParticleSubsystem::Tick(float deltaTime)
{
    Simulate(deltaTime * m_config.globalSimulationSpeed);
}

ParticleSystemInstance* ParticleSubsystem::CreateInstance(ParticleSystem::Ptr system)
{
    if (!system)
        return nullptr;

    auto instance = std::make_unique<ParticleSystemInstance>(system);
    if (m_renderState->device)
    {
        auto simulator = std::make_unique<CPUParticleSimulator>();
        simulator->Initialize(m_renderState->device, system->maxParticles);
        if (m_config.deterministicCpuSimulation)
        {
            simulator->SetRandomSeed(m_config.cpuSimulationSeed + static_cast<uint32>(m_instances.size()));
        }
        instance->SetSimulator(std::move(simulator), "CPU");
    }
    else
    {
        instance->SetSimulationUnsupported("ParticleSubsystem has no RHI device for CPU particle simulation");
    }

    ParticleSystemInstance* ptr = instance.get();
    m_instances.push_back(std::move(instance));

    m_stats.activeInstances = static_cast<uint32>(m_instances.size());
    return ptr;
}

void ParticleSubsystem::DestroyInstance(ParticleSystemInstance* instance)
{
    auto it = std::find_if(m_instances.begin(), m_instances.end(),
        [instance](const auto& ptr) { return ptr.get() == instance; });

    if (it != m_instances.end())
    {
        m_instances.erase(it);
        m_visibleInstances.erase(std::remove(m_visibleInstances.begin(), m_visibleInstances.end(), instance),
                                 m_visibleInstances.end());
        m_stats.activeInstances = static_cast<uint32>(m_instances.size());
        m_stats.visibleInstances = static_cast<uint32>(m_visibleInstances.size());
    }
}

ParticleSystemInstance* ParticleSubsystem::AcquireFromPool(ParticleSystem::Ptr system)
{
    return m_pool.Acquire(system);
}

void ParticleSubsystem::ReleaseToPool(ParticleSystemInstance* instance)
{
    m_pool.Release(instance);
}

void ParticleSubsystem::Simulate(float deltaTime)
{
    m_stats.totalParticles = 0;
    m_stats.gpuSimulatedParticles = 0;
    m_stats.cpuSimulatedParticles = 0;

    for (auto& instance : m_instances)
    {
        if (!instance->IsPlaying())
            continue;

        instance->Simulate(deltaTime);

        uint32 count = instance->GetAliveCount();
        m_stats.totalParticles += count;

        if (auto* sim = instance->GetSimulator())
        {
            if (sim->IsGPUBased())
                m_stats.gpuSimulatedParticles += count;
            else
                m_stats.cpuSimulatedParticles += count;
        }
    }
}

void ParticleSubsystem::PrepareRenderForCamera(const Vec3& cameraPosition)
{
    ++m_stats.prepareFrameCount;
    if (!m_renderState->renderPass)
    {
        ++m_stats.skippedPrepareFrameCount;
        if (m_renderIntegrationUnsupportedReason.empty())
        {
            MarkRenderIntegrationUnsupported("Particle render pass is unavailable during render preparation");
        }
        return;
    }

    // Update LODs
    UpdateLODsForCamera(cameraPosition);

    // Cull invisible instances
    CullInstancesForCamera(cameraPosition);

    // Update render pass
    m_renderState->renderPass->SetParticleSystems(m_visibleInstances);

    m_stats.visibleInstances = static_cast<uint32>(m_visibleInstances.size());
}

bool ParticleSubsystem::BuildRenderSnapshot(RVX::ParticleRenderSnapshot& outSnapshot) const
{
    outSnapshot.BeginBuild(++m_nextRenderSnapshotSequence);

    for (const auto& instanceOwner : m_instances)
    {
        const ParticleSystemInstance* instance = instanceOwner.get();
        if (!instance)
        {
            outSnapshot.skippedReasons.push_back("Null particle instance");
            continue;
        }

        if (!instance->IsPlaying())
        {
            outSnapshot.skippedReasons.push_back("Particle instance is not playing");
            continue;
        }

        if (!instance->IsVisible())
        {
            outSnapshot.skippedReasons.push_back("Particle instance is hidden");
            continue;
        }

        const uint32 aliveCount = instance->GetAliveCount();
        if (aliveCount == 0)
        {
            outSnapshot.skippedReasons.push_back("Particle instance has no alive particles");
            continue;
        }

        const auto system = instance->GetSystem();
        if (!system)
        {
            outSnapshot.skippedReasons.push_back("Particle instance has no particle system");
            continue;
        }

        RVX::ParticleRenderSnapshotItem item;
        item.instanceId = instance->GetInstanceId();
        item.systemId = system->id;
        item.systemName = system->name;
        item.worldMatrix = instance->GetTransform();
        item.worldBounds = instance->GetWorldBounds();
        item.position = instance->GetPosition();
        item.renderMode = ToSnapshotRenderMode(system->renderMode);
        item.blendMode = ToSnapshotBlendMode(system->blendMode);
        item.simulationBackend = ToSnapshotSimulationBackend(*instance);
        item.payloadStatus = RVX::ParticleRenderSnapshotPayloadStatus::MetadataOnly;
        item.aliveParticleCount = aliveCount;
        item.maxParticleCount = instance->GetMaxParticles();
        item.lodLevel = instance->GetCurrentLODLevel();
        item.normalizedTime = instance->GetNormalizedTime();
        item.visible = instance->IsVisible();
        item.simulationSupported = instance->IsSimulationSupported();
        item.renderPayloadAvailable = false;
        item.sortingSupported = false;
        item.softParticlesEnabled = system->softParticleConfig.enabled;
        item.softParticleFadeDistance = system->softParticleConfig.fadeDistance;
        item.renderPayloadReason =
            "Particle snapshot contains metadata only; Render-owned particle draw data extraction is not connected";
        item.sortingReason =
            "Particle sorting is deferred to Render-owned feature passes";
        if (!item.simulationSupported)
        {
            item.unsupportedReason = instance->GetSimulationUnsupportedReason();
        }

        outSnapshot.metadata.totalAliveParticles += aliveCount;
        outSnapshot.items.push_back(std::move(item));
    }

    outSnapshot.MarkComplete();
    return true;
}

void ParticleSubsystem::CullInstancesForCamera(const Vec3& cameraPosition)
{
    m_visibleInstances.clear();

    for (auto& instance : m_instances)
    {
        if (!instance->IsPlaying() || instance->GetAliveCount() == 0)
            continue;

        if (!instance->IsVisible())
            continue;

        // Check LOD culling
        auto* system = instance->GetSystem().get();
        if (system && system->lodConfig.enabled)
        {
            Vec3 pos = instance->GetPosition();
            float distance = length(pos - cameraPosition);

            if (system->lodConfig.ShouldCull(distance))
                continue;
        }

        // Frustum culling would happen here
        // AABB bounds = instance->GetWorldBounds();
        // if (!view.frustum.Contains(bounds)) continue;

        m_visibleInstances.push_back(instance.get());
    }
}

void ParticleSubsystem::UpdateLODsForCamera(const Vec3& cameraPosition)
{
    for (auto& instance : m_instances)
    {
        if (!instance->IsPlaying())
            continue;

        Vec3 pos = instance->GetPosition();
        float distance = length(pos - cameraPosition);
        instance->UpdateLOD(distance);
    }
}

} // namespace RVX::Particle
