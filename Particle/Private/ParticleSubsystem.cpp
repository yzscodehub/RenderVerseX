#include "Particle/ParticleSubsystem.h"
#include "Core/Log.h"
#include "Particle/GPU/CPUParticleSimulator.h"
#include "Particle/GPU/IParticleSimulator.h"
#include "Particle/GPU/ParticleSorter.h"
#include "Particle/Rendering/ParticlePass.h"
#include "Particle/Rendering/ParticleRenderer.h"
#include "Render/Renderer/SceneRenderer.h"
#include "RHI/RHI.h"
#include <algorithm>
#include <utility>

namespace RVX::Particle
{

ParticleSubsystem* ParticleSubsystem::s_activeSubsystem = nullptr;

ParticleSubsystem::ParticleSubsystem() = default;

ParticleSubsystem::~ParticleSubsystem()
{
    Deinitialize();
}

ParticleSubsystem* ParticleSubsystem::GetActiveSubsystem()
{
    return s_activeSubsystem;
}

void ParticleSubsystem::Initialize()
{
    s_activeSubsystem = this;
    m_renderIntegrationReady = false;
    m_renderIntegrationUnsupportedReason = "Particle render integration is not initialized";

    AcquireRenderDependencies();

    if (!m_device)
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
    if (!m_device)
        return;

    m_gpuSimulationSupported = false;

    if (m_config.enableGPUSimulation)
    {
        RVX_CORE_WARN("ParticleSubsystem: GPU particle simulation is unsupported until simulator pipelines are connected");
    }
}

void ParticleSubsystem::SetRenderSubsystem(RenderSubsystem* renderSubsystem)
{
    m_renderSubsystem = renderSubsystem;
    AcquireRenderDependencies();
}

void ParticleSubsystem::CreateRenderComponents()
{
    // Create renderer
    m_renderer = std::make_unique<ParticleRenderer>();
    if (m_hasRendererConfigOverride)
    {
        m_renderer->Initialize(m_device, m_rendererConfigOverride);
    }
    else
    {
        ParticleRendererConfig rendererConfig;
        // Matches PipelineCache::GetDefaultDepthStencilFormat() used by SceneRenderer.
        rendererConfig.depthStencilFormat = RHIFormat::D32_FLOAT;
        m_renderer->Initialize(m_device, rendererConfig);
    }

    // Create sorter (if GPU simulation supported)
    if (m_gpuSimulationSupported && m_config.enableSorting)
    {
        m_sorter = std::make_unique<ParticleSorter>();
        m_sorter->Initialize(m_device, m_config.maxGlobalParticles);
    }
}

void ParticleSubsystem::AcquireRenderDependencies()
{
    if (!m_renderSubsystem)
        return;

    if (!m_device)
    {
        m_device = m_renderSubsystem->GetDevice();
    }

    if (!m_sceneRenderer)
    {
        m_sceneRenderer = m_renderSubsystem->GetSceneRenderer();
    }
}

void ParticleSubsystem::RegisterRenderIntegration()
{
    if (!m_sceneRenderer)
    {
        MarkRenderIntegrationUnsupported("SceneRenderer is unavailable for particle pass registration");
        RVX_CORE_WARN("ParticleSubsystem: {}", m_renderIntegrationUnsupportedReason);
        return;
    }

    if (!m_renderer || !m_renderer->IsRenderingSupported())
    {
        const std::string reason = m_renderer && !m_renderer->GetUnsupportedReason().empty()
                                       ? "Particle renderer is unsupported: " + m_renderer->GetUnsupportedReason()
                                       : "Particle renderer is unavailable for pass registration";
        MarkRenderIntegrationUnsupported(reason);
        RVX_CORE_WARN("ParticleSubsystem: {}", m_renderIntegrationUnsupportedReason);
        return;
    }

    auto renderPass = std::make_unique<ParticlePass>();
    renderPass->SetRenderer(m_renderer.get());
    renderPass->SetSorter(m_sorter.get());
    renderPass->SetSortingEnabled(m_config.enableSorting);
    renderPass->SetSoftParticlesEnabled(m_config.enableSoftParticles);

    m_renderPass = renderPass.get();
    m_sceneRenderer->AddPass(std::move(renderPass));
    m_renderPassRegistered = true;
    m_stats.renderPassRegistered = true;

    if (!m_sceneRenderer->AddPreGraphPrepareCallback(
            this,
            [this](const ViewData& view)
            {
                if (m_renderIntegrationReady)
                {
                    PrepareRender(view);
                }
                else
                {
                    ++m_stats.skippedPrepareFrameCount;
                }
            }))
    {
        m_sceneRenderer->RemovePass("ParticlePass");
        m_renderPass = nullptr;
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

    if (m_sceneRenderer && m_preGraphCallbackRegistered)
    {
        m_sceneRenderer->RemovePreGraphPrepareCallback(this);
    }
    m_preGraphCallbackRegistered = false;
    m_stats.preGraphCallbackRegistered = false;

    MarkRenderIntegrationUnsupported("Particle subsystem is deinitialized");

    if (m_sceneRenderer && m_renderPassRegistered)
    {
        m_sceneRenderer->RemovePass("ParticlePass");
    }
    m_renderPassRegistered = false;
    m_stats.renderPassRegistered = false;
    m_renderPass = nullptr;

    m_instances.clear();
    m_visibleInstances.clear();
    m_pool.Clear();
    m_stats.activeInstances = 0;
    m_stats.visibleInstances = 0;
    m_stats.totalParticles = 0;
    m_stats.gpuSimulatedParticles = 0;
    m_stats.cpuSimulatedParticles = 0;

    m_sorter.reset();
    m_renderer.reset();

    m_sceneRenderer = nullptr;
    m_device = nullptr;

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
    if (m_device)
    {
        auto simulator = std::make_unique<CPUParticleSimulator>();
        simulator->Initialize(m_device, system->maxParticles);
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

void ParticleSubsystem::PrepareRender(const ViewData& view)
{
    ++m_stats.prepareFrameCount;
    if (!m_renderPass)
    {
        ++m_stats.skippedPrepareFrameCount;
        if (m_renderIntegrationUnsupportedReason.empty())
        {
            MarkRenderIntegrationUnsupported("Particle render pass is unavailable during PrepareRender");
        }
        return;
    }

    // Update LODs
    UpdateLODs(view);

    // Cull invisible instances
    CullInstances(view);

    // Update render pass
    m_renderPass->SetParticleSystems(m_visibleInstances);

    m_stats.visibleInstances = static_cast<uint32>(m_visibleInstances.size());
}

void ParticleSubsystem::CullInstances(const ViewData& view)
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
            float distance = length(pos - view.cameraPosition);
            
            if (system->lodConfig.ShouldCull(distance))
                continue;
        }

        // Frustum culling would happen here
        // AABB bounds = instance->GetWorldBounds();
        // if (!view.frustum.Contains(bounds)) continue;

        m_visibleInstances.push_back(instance.get());
    }
}

void ParticleSubsystem::UpdateLODs(const ViewData& view)
{
    for (auto& instance : m_instances)
    {
        if (!instance->IsPlaying())
            continue;

        Vec3 pos = instance->GetPosition();
        float distance = length(pos - view.cameraPosition);
        instance->UpdateLOD(distance);
    }
}

} // namespace RVX::Particle
