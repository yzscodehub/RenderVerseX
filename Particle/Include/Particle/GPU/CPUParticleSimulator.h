#pragma once

/**
 * @file CPUParticleSimulator.h
 * @brief CPU-based particle simulation fallback
 */

#include "Particle/GPU/IParticleSimulator.h"
#include "Particle/Modules/ColorOverLifetimeModule.h"
#include "Particle/Modules/SizeOverLifetimeModule.h"
#include "Particle/Modules/VelocityOverLifetimeModule.h"
#include "Particle/Modules/RotationOverLifetimeModule.h"
#include "Particle/Modules/NoiseModule.h"
#include "Particle/Events/ParticleEvent.h"
#include <vector>
#include <random>

namespace RVX::Particle
{
    // Forward declarations
    class ParticleEventHandler;

    /**
     * @brief Extended simulation parameters with module data
     */
    struct CPUSimulateParams : public SimulateParams
    {
        /// Optional modules for simulation
        const ColorOverLifetimeModule* colorModule = nullptr;
        const SizeOverLifetimeModule* sizeModule = nullptr;
        const VelocityOverLifetimeModule* velocityModule = nullptr;
        const RotationOverLifetimeModule* rotationModule = nullptr;
        const NoiseModule* noiseModule = nullptr;
        
        /// Event handler for particle events
        ParticleEventHandler* eventHandler = nullptr;
        
        /// Total simulation time
        float totalTime = 0.0f;
        
        /// Instance ID for events
        uint64 instanceId = 0;
    };

    /**
     * @brief CPU-based particle simulator
     * 
     * Fallback implementation for platforms without Compute Shader support.
     * Uses JobSystem for parallel simulation where available.
     */
    class CPUParticleSimulator : public IParticleSimulator
    {
    public:
        CPUParticleSimulator() = default;
        ~CPUParticleSimulator() override;

        // Non-copyable
        CPUParticleSimulator(const CPUParticleSimulator&) = delete;
        CPUParticleSimulator& operator=(const CPUParticleSimulator&) = delete;

        // =====================================================================
        // IParticleSimulator Interface
        // =====================================================================

        void Initialize(IRHIDevice* device, uint32 maxParticles) override;
        void Shutdown() override;
        bool IsInitialized() const override { return m_initialized; }

        void Emit(const EmitParams& params) override;
        void Simulate(float deltaTime, const SimulateParams& params) override;
        void PrepareRender(RHICommandContext& ctx) override;
        void Clear() override;

        RHIBuffer* GetParticleBuffer() const override { return m_gpuParticleBuffer.Get(); }
        RHIBuffer* GetAliveIndexBuffer() const override { return m_gpuAliveIndexBuffer.Get(); }
        RHIBuffer* GetIndirectDrawBuffer() const override { return m_gpuIndirectDrawBuffer.Get(); }
        uint32 GetAliveCount() const override { return static_cast<uint32>(m_aliveIndices.size()); }
        uint32 GetMaxParticles() const override { return m_maxParticles; }

        bool IsGPUBased() const override { return false; }

        // =====================================================================
        // Extended Simulation with Modules
        // =====================================================================
        
        /// Simulate with extended module parameters
        void SimulateWithModules(float deltaTime, const CPUSimulateParams& params);
        
        /// Get queued events from last simulation
        const std::vector<ParticleEvent>& GetQueuedEvents() const { return m_queuedEvents; }
        
        /// Clear queued events
        void ClearQueuedEvents() { m_queuedEvents.clear(); }

    private:
        void EmitParticle(const EmitParams& params, uint32 index);
        void SimulateParticle(uint32 index, float deltaTime, const SimulateParams& params);
        void SimulateParticleWithModules(uint32 index, float deltaTime, const CPUSimulateParams& params);
        void SimulateParallel(float deltaTime, const SimulateParams& params);
        void SimulateParallelWithModules(float deltaTime, const CPUSimulateParams& params);
        void UploadToGPU();
        Vec3 GenerateEmitterPosition(const EmitterGPUData& data, float random);
        Vec3 GenerateEmitterVelocity(const EmitterGPUData& data, float random);
        
        // Noise sampling
        float SamplePerlinNoise(const Vec3& pos) const;
        Vec3 SampleCurlNoise(const Vec3& pos, float epsilon) const;

        bool m_initialized = false;
        IRHIDevice* m_device = nullptr;
        uint32 m_maxParticles = 0;

        // CPU particle data
        std::vector<CPUParticle> m_particles;
        std::vector<uint32> m_aliveIndices;
        std::vector<uint32> m_deadIndices;
        
        // Event queue for particle events
        std::vector<ParticleEvent> m_queuedEvents;

        // GPU upload buffers
        RHIBufferRef m_gpuParticleBuffer;
        RHIBufferRef m_gpuAliveIndexBuffer;
        RHIBufferRef m_gpuIndirectDrawBuffer;
        RHIBufferRef m_uploadBuffer;

        // Random number generation
        std::mt19937 m_rng;
        std::uniform_real_distribution<float> m_dist{0.0f, 1.0f};

        // Dirty flag for upload
        bool m_gpuDirty = true;
    };

} // namespace RVX::Particle
