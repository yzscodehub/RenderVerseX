#pragma once

/**
 * @file ParticleSubsystem.h
 * @brief Engine subsystem for particle system management
 */

#include "Core/Subsystem/EngineSubsystem.h"
#include "Particle/ParticleSystem.h"
#include "Particle/ParticleSystemInstance.h"
#include "Particle/ParticlePool.h"
#include "Render/Renderer/ViewData.h"
#include "Render/RenderSubsystem.h"
#include "Resource/ResourceSubsystem.h"
#include <memory>
#include <vector>

namespace RVX
{
    class IRHIDevice;
}

namespace RVX::Particle
{
    class ParticleRenderer;
    class ParticleSorter;
    class ParticlePass;

    /**
     * @brief Configuration for particle subsystem
     */
    struct ParticleSubsystemConfig
    {
        uint32 maxGlobalParticles = 1000000;    ///< Maximum particles across all systems
        uint32 maxInstances = 1000;             ///< Maximum particle system instances
        bool enableGPUSimulation = true;        ///< Prefer GPU simulation when available
        bool enableSorting = true;              ///< Enable transparency sorting
        bool enableSoftParticles = true;        ///< Enable soft particle depth fade
        float globalSimulationSpeed = 1.0f;     ///< Global simulation speed multiplier
    };

    /**
     * @brief Engine subsystem for particle system management
     * 
     * Handles:
     * - Particle system instance creation and destruction
     * - GPU/CPU simulation backend selection
     * - LOD and culling
     * - Integration with RenderGraph
     * - Object pooling
     */
    class ParticleSubsystem : public EngineSubsystem
    {
    public:
        ParticleSubsystem() = default;
        ~ParticleSubsystem() override;

        // =====================================================================
        // ISubsystem Interface
        // =====================================================================

        const char* GetName() const override { return "ParticleSubsystem"; }
        bool ShouldTick() const override { return true; }
        TickPhase GetTickPhase() const override { return TickPhase::PreRender; }

        RVX_SUBSYSTEM_DEPENDENCIES(RenderSubsystem, ResourceSubsystem);

        void Initialize() override;
        void Deinitialize() override;
        void Tick(float deltaTime) override;

        // =====================================================================
        // Instance Management
        // =====================================================================

        /// Create a particle system instance
        ParticleSystemInstance* CreateInstance(ParticleSystem::Ptr system);

        /// Destroy a particle system instance
        void DestroyInstance(ParticleSystemInstance* instance);

        /// Get all active instances
        const std::vector<std::unique_ptr<ParticleSystemInstance>>& GetInstances() const 
        { 
            return m_instances; 
        }

        // =====================================================================
        // Pooling
        // =====================================================================

        /// Get the object pool
        ParticlePool* GetPool() { return &m_pool; }

        /// Acquire an instance from pool
        ParticleSystemInstance* AcquireFromPool(ParticleSystem::Ptr system);

        /// Release an instance back to pool
        void ReleaseToPool(ParticleSystemInstance* instance);

        // =====================================================================
        // Simulation
        // =====================================================================

        /// Simulate all active particle systems
        void Simulate(float deltaTime);

        /// Prepare for rendering
        void PrepareRender(const ViewData& view);

        /// Get visible instances (after culling)
        const std::vector<ParticleSystemInstance*>& GetVisibleInstances() const 
        { 
            return m_visibleInstances; 
        }

        // =====================================================================
        // Configuration
        // =====================================================================

        /// Get configuration
        ParticleSubsystemConfig& GetConfig() { return m_config; }
        const ParticleSubsystemConfig& GetConfig() const { return m_config; }

        /// Check if GPU simulation is supported
        bool IsGPUSimulationSupported() const { return m_gpuSimulationSupported; }

        // =====================================================================
        // Rendering Components
        // =====================================================================

        ParticleRenderer* GetRenderer() { return m_renderer.get(); }
        ParticleSorter* GetSorter() { return m_sorter.get(); }
        ParticlePass* GetRenderPass() { return m_renderPass.get(); }

        // =====================================================================
        // Statistics
        // =====================================================================

        struct Statistics
        {
            uint32 activeInstances = 0;
            uint32 visibleInstances = 0;
            uint32 totalParticles = 0;
            uint32 gpuSimulatedParticles = 0;
            uint32 cpuSimulatedParticles = 0;
        };

        const Statistics& GetStatistics() const { return m_stats; }

    private:
        void CheckCapabilities();
        void CreateRenderComponents();
        void CullInstances(const ViewData& view);
        void UpdateLODs(const ViewData& view);

        ParticleSubsystemConfig m_config;
        IRHIDevice* m_device = nullptr;

        // Simulation capability
        bool m_gpuSimulationSupported = false;

        // Instances
        std::vector<std::unique_ptr<ParticleSystemInstance>> m_instances;
        std::vector<ParticleSystemInstance*> m_visibleInstances;

        // Object pool
        ParticlePool m_pool;

        // Rendering components
        std::unique_ptr<ParticleRenderer> m_renderer;
        std::unique_ptr<ParticleSorter> m_sorter;
        std::unique_ptr<ParticlePass> m_renderPass;

        // Statistics
        Statistics m_stats;
    };

} // namespace RVX::Particle
