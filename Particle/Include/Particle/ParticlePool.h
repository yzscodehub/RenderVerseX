#pragma once

/**
 * @file ParticlePool.h
 * @brief Object pool for particle system instances
 */

#include "Particle/ParticleSystem.h"
#include <memory>
#include <vector>
#include <unordered_map>

namespace RVX::Particle
{
    class ParticleSystemInstance;

    /**
     * @brief Object pool for particle system instances
     * 
     * Reduces allocation overhead by reusing ParticleSystemInstance objects.
     */
    class ParticlePool
    {
    public:
        ParticlePool() = default;
        ~ParticlePool() = default;

        // Non-copyable
        ParticlePool(const ParticlePool&) = delete;
        ParticlePool& operator=(const ParticlePool&) = delete;

        // =====================================================================
        // Pool Operations
        // =====================================================================

        /// Acquire an instance from the pool (or create new if none available)
        ParticleSystemInstance* Acquire(ParticleSystem::Ptr system);

        /// Release an instance back to the pool
        void Release(ParticleSystemInstance* instance);

        /// Prewarm the pool with instances
        void Prewarm(ParticleSystem::Ptr system, uint32 count);

        /// Set maximum pool size for a system
        void SetPoolSize(ParticleSystem::Ptr system, uint32 size);

        /// Get current pool size for a system
        uint32 GetPoolSize(ParticleSystem::Ptr system) const;

        /// Clean up unused instances (older than maxIdleTime seconds)
        void Cleanup(float maxIdleTime = 30.0f);

        /// Clear all pools
        void Clear();

        // =====================================================================
        // Statistics
        // =====================================================================

        /// Get total pooled instances
        uint32 GetTotalPooled() const;

        /// Get number of systems with pools
        uint32 GetPooledSystemCount() const;

    private:
        struct PoolEntry
        {
            std::unique_ptr<ParticleSystemInstance> instance;
            float idleTime = 0.0f;
        };

        struct SystemPool
        {
            std::vector<PoolEntry> available;
            uint32 maxSize = 10;
        };

        std::unordered_map<ParticleSystem*, SystemPool> m_pools;
    };

} // namespace RVX::Particle
