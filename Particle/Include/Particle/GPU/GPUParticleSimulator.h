#pragma once

/**
 * @file GPUParticleSimulator.h
 * @brief GPU-based particle simulation using Compute Shaders
 */

#include "Particle/GPU/IParticleSimulator.h"
#include "RHI/RHI.h"

namespace RVX::Particle
{
    /**
     * @brief GPU-based particle simulator using Compute Shaders
     * 
     * Uses Compute Shaders for:
     * - Particle emission
     * - Particle simulation (forces, noise, collision)
     * - Dead particle compaction
     * 
     * Uses Indirect Draw for rendering without CPU readback.
     */
    class GPUParticleSimulator : public IParticleSimulator
    {
    public:
        GPUParticleSimulator() = default;
        ~GPUParticleSimulator() override;

        // Non-copyable
        GPUParticleSimulator(const GPUParticleSimulator&) = delete;
        GPUParticleSimulator& operator=(const GPUParticleSimulator&) = delete;

        // =====================================================================
        // IParticleSimulator Interface
        // =====================================================================

        void Initialize(IRHIDevice* device, uint32 maxParticles) override;
        void Shutdown() override;
        bool IsInitialized() const override { return m_device != nullptr; }

        void Emit(const EmitParams& params) override;
        void Simulate(float deltaTime, const SimulateParams& params) override;
        void PrepareRender(RHICommandContext& ctx) override;
        void Clear() override;

        RHIBuffer* GetParticleBuffer() const override { return m_particleBuffer.Get(); }
        RHIBuffer* GetAliveIndexBuffer() const override { return m_aliveIndexBuffer.Get(); }
        RHIBuffer* GetIndirectDrawBuffer() const override { return m_indirectDrawBuffer.Get(); }
        uint32 GetAliveCount() const override { return m_aliveCount; }
        uint32 GetMaxParticles() const override { return m_maxParticles; }

        bool IsGPUBased() const override { return true; }

        // =====================================================================
        // GPU Execution
        // =====================================================================

        /// Execute emit pass (Compute Shader)
        void ExecuteEmitPass(RHICommandContext& ctx);

        /// Execute simulation pass (Compute Shader)
        void ExecuteSimulatePass(RHICommandContext& ctx, float deltaTime);

        /// Execute compaction pass (removes dead particles)
        void ExecuteCompactPass(RHICommandContext& ctx);

        // =====================================================================
        // Pipeline Management
        // =====================================================================

        /// Set emit compute pipeline
        void SetEmitPipeline(RHIPipeline* pipeline) { m_emitPipeline = pipeline; }

        /// Set simulate compute pipeline
        void SetSimulatePipeline(RHIPipeline* pipeline) { m_simulatePipeline = pipeline; }

        /// Set compact compute pipeline
        void SetCompactPipeline(RHIPipeline* pipeline) { m_compactPipeline = pipeline; }

    private:
        void CreateBuffers();
        void InitializeDeadList();

        IRHIDevice* m_device = nullptr;
        uint32 m_maxParticles = 0;
        uint32 m_aliveCount = 0;

        // Double-buffered particle data
        RHIBufferRef m_particleBuffer;
        RHIBufferRef m_particleBufferBack;

        // Index lists
        RHIBufferRef m_aliveIndexBuffer;
        RHIBufferRef m_aliveIndexBufferBack;
        RHIBufferRef m_deadIndexBuffer;

        // Counters (alive count, dead count)
        RHIBufferRef m_counterBuffer;

        // Indirect draw arguments
        RHIBufferRef m_indirectDrawBuffer;

        // Constant buffers
        RHIBufferRef m_emitConstantsBuffer;
        RHIBufferRef m_simulateConstantsBuffer;

        // Pipelines
        RHIPipeline* m_emitPipeline = nullptr;
        RHIPipeline* m_simulatePipeline = nullptr;
        RHIPipeline* m_compactPipeline = nullptr;

        // Pending emit data
        EmitParams m_pendingEmit;
        bool m_hasPendingEmit = false;

        // Current simulation parameters
        SimulateParams m_simulateParams;
    };

} // namespace RVX::Particle
