#pragma once

/**
 * @file ParticleSorter.h
 * @brief GPU-based particle sorting for correct transparency rendering
 */

#include "Particle/ParticleTypes.h"
#include "RHI/RHI.h"

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
     * @brief GPU-based particle sorter using Bitonic Sort
     * 
     * Implements GPU parallel bitonic sort for back-to-front
     * particle rendering order (required for correct transparency).
     */
    class ParticleSorter
    {
    public:
        ParticleSorter() = default;
        ~ParticleSorter();

        // Non-copyable
        ParticleSorter(const ParticleSorter&) = delete;
        ParticleSorter& operator=(const ParticleSorter&) = delete;

        // =====================================================================
        // Lifecycle
        // =====================================================================

        void Initialize(IRHIDevice* device, uint32 maxParticles);
        void Shutdown();
        bool IsInitialized() const { return m_device != nullptr; }

        // =====================================================================
        // Sorting
        // =====================================================================

        /**
         * @brief Sort particles by distance to camera
         * @param ctx Command context
         * @param particleBuffer Buffer containing particle data
         * @param indexBuffer Output: sorted index buffer
         * @param particleCount Number of particles to sort
         * @param cameraPosition Camera world position
         */
        void Sort(RHICommandContext& ctx,
                  RHIBuffer* particleBuffer,
                  RHIBuffer* indexBuffer,
                  uint32 particleCount,
                  const Vec3& cameraPosition);

        // =====================================================================
        // Pipeline Management
        // =====================================================================

        void SetKeyGenPipeline(RHIPipeline* pipeline) { m_keyGenPipeline = pipeline; }
        void SetBitonicSortPipeline(RHIPipeline* pipeline) { m_bitonicSortPipeline = pipeline; }
        void SetBitonicMergePipeline(RHIPipeline* pipeline) { m_bitonicMergePipeline = pipeline; }

    private:
        void GenerateSortKeys(RHICommandContext& ctx, 
                             RHIBuffer* particleBuffer,
                             uint32 particleCount,
                             const Vec3& cameraPosition);
        
        void BitonicSort(RHICommandContext& ctx, uint32 count);
        void ScatterToOutput(RHICommandContext& ctx, RHIBuffer* indexBuffer, uint32 count);

        IRHIDevice* m_device = nullptr;
        uint32 m_maxParticles = 0;

        // Sort key buffer (distance + index pairs)
        RHIBufferRef m_sortKeyBuffer;
        RHIBufferRef m_sortKeyBufferBack;

        // Constants buffer
        RHIBufferRef m_sortConstantsBuffer;

        // Pipelines
        RHIPipeline* m_keyGenPipeline = nullptr;
        RHIPipeline* m_bitonicSortPipeline = nullptr;
        RHIPipeline* m_bitonicMergePipeline = nullptr;
    };

} // namespace RVX::Particle
