#pragma once

/**
 * @file ParticleSorter.h
 * @brief Unsupported particle sorting diagnostic contract
 */

#include "Particle/ParticleTypes.h"

#include <string>

namespace RVX::Particle
{
    /**
     * @brief Sort key for particles (distance + index)
     */
    struct ParticleSortKey
    {
        float distance;     ///< Distance to camera (for sorting)
        uint32 index;       ///< Original particle index
    };

    /**
     * @brief Compatibility diagnostic for deferred particle sorting.
     *
     * Particle no longer owns GPU sorting resources. Render-owned feature
     * passes are expected to consume particle snapshots and provide sorting.
     */
    class ParticleSorter
    {
    public:
        ParticleSorter() = default;
        ~ParticleSorter() = default;

        // Non-copyable
        ParticleSorter(const ParticleSorter&) = delete;
        ParticleSorter& operator=(const ParticleSorter&) = delete;

        // =====================================================================
        // Lifecycle
        // =====================================================================

        void Initialize(uint32 maxParticles);
        void Shutdown();
        bool IsInitialized() const { return m_initialized; }
        bool IsSupported() const { return false; }
        const std::string& GetUnsupportedReason() const { return m_unsupportedReason; }
        uint32 GetMaxParticles() const { return m_maxParticles; }
        uint32 GetLastRequestedParticleCount() const { return m_lastRequestedParticleCount; }

        // =====================================================================
        // Sorting
        // =====================================================================

        /**
         * @brief Report that particle sorting is not owned by Particle.
         * @param particleCount Number of particles to sort
         * @param cameraPosition Camera world position
         * @return Always false until a Render-owned sorting pass is connected
         */
        bool Sort(uint32 particleCount, const Vec3& cameraPosition);

    private:
        bool m_initialized = false;
        uint32 m_maxParticles = 0;
        uint32 m_lastRequestedParticleCount = 0;
        Vec3 m_lastRequestedCameraPosition{0.0f, 0.0f, 0.0f};
        std::string m_unsupportedReason =
            "Particle sorting is deferred to Render-owned feature passes";
    };

} // namespace RVX::Particle
