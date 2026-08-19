#pragma once

/**
 * @file ParticleSubsystem.h
 * @brief Engine subsystem for particle system management
 */

#include "Core/Subsystem/EngineSubsystem.h"
#include "Particle/ParticleRenderStats.h"
#include "Particle/ParticleSystem.h"
#include "Particle/ParticleSystemInstance.h"
#include "Particle/ParticlePool.h"
#include "RenderContracts/ParticleRenderSnapshot.h"
#include <memory>
#include <string>
#include <vector>

namespace RVX::Particle
{
    /**
     * @brief Configuration for particle subsystem
     */
    struct ParticleSubsystemConfig
    {
        uint32 maxGlobalParticles = 1000000;    ///< Maximum particles across all systems
        uint32 maxInstances = 1000;             ///< Maximum particle system instances
        bool enableGPUSimulation = true;        ///< Request GPU simulation when a Render-owned backend is available
        bool enableSorting = true;              ///< Enable transparency sorting
        bool enableSoftParticles = true;        ///< Enable soft particle depth fade
        float globalSimulationSpeed = 1.0f;     ///< Global simulation speed multiplier
        bool deterministicCpuSimulation = false; ///< Use a fixed CPU simulator seed for reproducible captures
        uint32 cpuSimulationSeed = 0;            ///< Seed used when deterministicCpuSimulation is enabled
    };

    /**
     * @brief Engine subsystem for particle system management
     * 
     * Handles:
     * - Particle system instance creation and destruction
     * - CPU simulation state and snapshot export
     * - LOD and culling
     * - Integration with RenderGraph
     * - Object pooling
     */
    class ParticleSubsystem : public EngineSubsystem
    {
    public:
        ParticleSubsystem();
        ~ParticleSubsystem() override;

        static ParticleSubsystem* GetActiveSubsystem();

        // =====================================================================
        // ISubsystem Interface
        // =====================================================================

        const char* GetName() const override { return "ParticleSubsystem"; }
        bool ShouldTick() const override { return true; }
        TickPhase GetTickPhase() const override { return TickPhase::PreRender; }

        std::vector<SubsystemDependency> GetTypedDependencies() const override;

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

        /// Get visible instances (after culling)
        const std::vector<ParticleSystemInstance*>& GetVisibleInstances() const
        {
            return m_visibleInstances;
        }

        /// Build a Render-facing particle snapshot without exposing Render/RHI objects.
        bool BuildRenderSnapshot(RVX::ParticleRenderSnapshot& outSnapshot) const;

        // =====================================================================
        // Configuration
        // =====================================================================

        /// Get configuration
        ParticleSubsystemConfig& GetConfig() { return m_config; }
        const ParticleSubsystemConfig& GetConfig() const { return m_config; }

        /// Check if GPU simulation is supported
        bool IsGPUSimulationSupported() const { return m_gpuSimulationSupported; }

        /// Check whether the subsystem owns a legacy render path. Production rendering uses snapshots.
        bool IsRenderIntegrationReady() const { return m_renderIntegrationReady; }

        /// Human-readable reason when render integration is unavailable.
        const std::string& GetRenderIntegrationUnsupportedReason() const { return m_renderIntegrationUnsupportedReason; }

        /// Test/reporting view of the last render draw attempt without exposing Render/RHI headers.
        const ParticleRenderDrawStats& GetLastRenderDrawStats() const;

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
            uint64 prepareFrameCount = 0;
            uint64 skippedPrepareFrameCount = 0;
            bool renderPassRegistered = false;
            bool preGraphCallbackRegistered = false;
        };

        const Statistics& GetStatistics() const { return m_stats; }

    private:
        void CheckCapabilities();
        void MarkRenderIntegrationUnsupported(const std::string& reason);
        void PrepareRenderForCamera(const Vec3& cameraPosition);
        void CullInstancesForCamera(const Vec3& cameraPosition);
        void UpdateLODsForCamera(const Vec3& cameraPosition);

        ParticleSubsystemConfig m_config;

        // Simulation capability
        bool m_gpuSimulationSupported = false;

        // Instances
        std::vector<std::unique_ptr<ParticleSystemInstance>> m_instances;
        std::vector<ParticleSystemInstance*> m_visibleInstances;

        // Object pool
        ParticlePool m_pool;

        // Rendering components
        bool m_renderPassRegistered = false;
        bool m_preGraphCallbackRegistered = false;
        bool m_renderIntegrationReady = false;
        std::string m_renderIntegrationUnsupportedReason = "Particle render integration is not initialized";

        // Statistics
        Statistics m_stats;
        mutable uint64 m_nextRenderSnapshotSequence = 0;

        static ParticleSubsystem* s_activeSubsystem;
    };

} // namespace RVX::Particle
