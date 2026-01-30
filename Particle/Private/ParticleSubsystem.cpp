#include "Particle/ParticleSubsystem.h"
#include "Particle/Rendering/ParticleRenderer.h"
#include "Particle/Rendering/ParticlePass.h"
#include "Particle/GPU/ParticleSorter.h"
#include "Particle/GPU/GPUParticleSimulator.h"
#include "Particle/GPU/CPUParticleSimulator.h"
#include "RHI/RHI.h"
#include "RHI/RHICapabilities.h"
#include "Core/Log.h"
#include <algorithm>

namespace RVX::Particle
{

ParticleSubsystem::~ParticleSubsystem()
{
    Deinitialize();
}

void ParticleSubsystem::Initialize()
{
    // Get RHI device from RenderSubsystem
    // m_device = GetDependency<RenderSubsystem>()->GetDevice();
    
    // For now, assume device is set externally or via dependency
    if (!m_device)
    {
        RVX_CORE_ERROR("ParticleSubsystem: No RHI device available");
        return;
    }

    // Check GPU capabilities
    CheckCapabilities();

    // Create rendering components
    CreateRenderComponents();

    m_instances.reserve(m_config.maxInstances);
    m_visibleInstances.reserve(m_config.maxInstances);

    RVX_CORE_INFO("ParticleSubsystem: Initialized (GPU={}, MaxParticles={})", 
                  m_gpuSimulationSupported, m_config.maxGlobalParticles);
}

void ParticleSubsystem::CheckCapabilities()
{
    if (!m_device)
        return;

    const auto& caps = m_device->GetCapabilities();
    m_gpuSimulationSupported = m_config.enableGPUSimulation;

    // Check for compute shader support
    switch (caps.backendType)
    {
    case RHIBackendType::OpenGL:
        if (!caps.opengl.hasComputeShader)
        {
            RVX_CORE_WARN("ParticleSubsystem: OpenGL Compute Shader unavailable, using CPU fallback");
            m_gpuSimulationSupported = false;
        }
        break;

    case RHIBackendType::DX11:
        if (caps.dx11.featureLevel < 0xB000)  // Feature Level 11.0
        {
            RVX_CORE_WARN("ParticleSubsystem: DX11 Feature Level < 11.0, using CPU fallback");
            m_gpuSimulationSupported = false;
        }
        break;

    case RHIBackendType::DX12:
    case RHIBackendType::Vulkan:
    case RHIBackendType::Metal:
        // These always support compute
        break;

    default:
        m_gpuSimulationSupported = false;
        break;
    }
}

void ParticleSubsystem::CreateRenderComponents()
{
    // Create renderer
    m_renderer = std::make_unique<ParticleRenderer>();
    m_renderer->Initialize(m_device);

    // Create sorter (if GPU simulation supported)
    if (m_gpuSimulationSupported && m_config.enableSorting)
    {
        m_sorter = std::make_unique<ParticleSorter>();
        m_sorter->Initialize(m_device, m_config.maxGlobalParticles);
    }

    // Create render pass
    m_renderPass = std::make_unique<ParticlePass>();
    m_renderPass->SetRenderer(m_renderer.get());
    m_renderPass->SetSorter(m_sorter.get());
    m_renderPass->SetSortingEnabled(m_config.enableSorting);
    m_renderPass->SetSoftParticlesEnabled(m_config.enableSoftParticles);
}

void ParticleSubsystem::Deinitialize()
{
    m_instances.clear();
    m_visibleInstances.clear();
    m_pool.Clear();

    m_renderPass.reset();
    m_sorter.reset();
    m_renderer.reset();

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
    
    // Create appropriate simulator
    if (m_gpuSimulationSupported)
    {
        auto simulator = std::make_unique<GPUParticleSimulator>();
        simulator->Initialize(m_device, system->maxParticles);
        // instance->SetSimulator(std::move(simulator));
    }
    else
    {
        auto simulator = std::make_unique<CPUParticleSimulator>();
        simulator->Initialize(m_device, system->maxParticles);
        // instance->SetSimulator(std::move(simulator));
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
        m_stats.activeInstances = static_cast<uint32>(m_instances.size());
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
