#pragma once

/**
 * @file IParticleSimulator.h
 * @brief Interface for particle simulation (GPU or CPU)
 */

#include "Particle/ParticleTypes.h"
#include "RHI/RHI.h"

namespace RVX::Particle
{
    /**
     * @brief Emit parameters for particle spawning
     */
    struct EmitParams
    {
        EmitterGPUData emitterData;
        uint32 emitCount = 0;
        uint32 randomSeed = 0;
    };

    /**
     * @brief Simulation parameters
     */
    struct SimulateParams
    {
        SimulationGPUData simulationData;
        float deltaTime = 0.0f;
        float totalTime = 0.0f;
    };

    /**
     * @brief Interface for particle simulation
     * 
     * Implemented by GPUParticleSimulator (Compute Shader) and
     * CPUParticleSimulator (fallback for unsupported platforms).
     */
    class IParticleSimulator
    {
    public:
        virtual ~IParticleSimulator() = default;

        // =====================================================================
        // Lifecycle
        // =====================================================================

        /// Initialize the simulator
        virtual void Initialize(IRHIDevice* device, uint32 maxParticles) = 0;

        /// Shutdown and release resources
        virtual void Shutdown() = 0;

        /// Check if initialized
        virtual bool IsInitialized() const = 0;

        // =====================================================================
        // Simulation
        // =====================================================================

        /// Emit new particles
        virtual void Emit(const EmitParams& params) = 0;

        /// Update particle simulation
        virtual void Simulate(float deltaTime, const SimulateParams& params) = 0;

        /// Prepare for rendering (upload data to GPU if needed)
        virtual void PrepareRender(RHICommandContext& ctx) = 0;

        /// Clear all particles
        virtual void Clear() = 0;

        // =====================================================================
        // GPU Resources (for rendering)
        // =====================================================================

        /// Get the particle data buffer
        virtual RHIBuffer* GetParticleBuffer() const = 0;

        /// Get the alive particle index buffer
        virtual RHIBuffer* GetAliveIndexBuffer() const = 0;

        /// Get indirect draw arguments buffer
        virtual RHIBuffer* GetIndirectDrawBuffer() const = 0;

        /// Get number of alive particles
        virtual uint32 GetAliveCount() const = 0;

        /// Get maximum particle count
        virtual uint32 GetMaxParticles() const = 0;

        // =====================================================================
        // Type Information
        // =====================================================================

        /// Check if this is a GPU-based simulator
        virtual bool IsGPUBased() const = 0;
    };

} // namespace RVX::Particle
