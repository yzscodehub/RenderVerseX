#pragma once

/**
 * @file IParticleSimulator.h
 * @brief Interface for particle simulation without Render/RHI ownership.
 */

#include "Particle/ParticleTypes.h"
#include "RenderContracts/ParticleRenderSnapshot.h"

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
     * Implemented by CPU-side simulation backends. Render owns GPU upload and
     * draw resource creation through the exported particle payload contract.
     */
    class IParticleSimulator
    {
    public:
        virtual ~IParticleSimulator() = default;

        // =====================================================================
        // Lifecycle
        // =====================================================================

        /// Initialize the simulator
        virtual void Initialize(uint32 maxParticles) = 0;

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

        /// Clear all particles
        virtual void Clear() = 0;

        // =====================================================================
        // Render Payload
        // =====================================================================

        /// Export alive particle data for Render-owned upload/draw preparation.
        virtual bool BuildRenderParticlePayload(std::vector<RVX::ParticleRenderParticleData>& outParticles) const = 0;

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
