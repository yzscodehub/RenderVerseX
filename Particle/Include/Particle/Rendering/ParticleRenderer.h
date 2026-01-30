#pragma once

/**
 * @file ParticleRenderer.h
 * @brief Particle rendering system
 */

#include "Particle/ParticleTypes.h"
#include "Particle/Rendering/SoftParticleConfig.h"
#include "Particle/Rendering/TrailRenderer.h"
#include "RHI/RHI.h"
#include "Render/Renderer/ViewData.h"
#include <unordered_map>
#include <memory>

namespace RVX::Particle
{
    class ParticleSystemInstance;

    /**
     * @brief Particle renderer - handles all particle rendering modes
     */
    class ParticleRenderer
    {
    public:
        ParticleRenderer() = default;
        ~ParticleRenderer();

        // Non-copyable
        ParticleRenderer(const ParticleRenderer&) = delete;
        ParticleRenderer& operator=(const ParticleRenderer&) = delete;

        // =====================================================================
        // Lifecycle
        // =====================================================================

        void Initialize(IRHIDevice* device);
        void Shutdown();
        bool IsInitialized() const { return m_device != nullptr; }

        // =====================================================================
        // Rendering
        // =====================================================================

        /**
         * @brief Draw particles for an instance
         * @param ctx Command context
         * @param instance Particle system instance
         * @param view View data
         * @param depthTexture Scene depth texture (for soft particles)
         */
        void DrawParticles(RHICommandContext& ctx,
                          ParticleSystemInstance* instance,
                          const ViewData& view,
                          RHITexture* depthTexture);

        /**
         * @brief Draw particles with indirect draw
         */
        void DrawParticlesIndirect(RHICommandContext& ctx,
                                   ParticleSystemInstance* instance,
                                   const ViewData& view,
                                   RHITexture* depthTexture);

        // =====================================================================
        // Pipeline Access
        // =====================================================================

        RHIPipeline* GetBillboardPipeline(ParticleBlendMode blend, bool softParticle);
        RHIPipeline* GetStretchedBillboardPipeline(ParticleBlendMode blend, bool softParticle);
        RHIPipeline* GetMeshPipeline(ParticleBlendMode blend);
        RHIPipeline* GetTrailPipeline(ParticleBlendMode blend, bool softParticle);

        // =====================================================================
        // Resource Management
        // =====================================================================

        /// Get or create quad vertex buffer
        RHIBuffer* GetQuadVertexBuffer();

        /// Get or create quad index buffer
        RHIBuffer* GetQuadIndexBuffer();

        // =====================================================================
        // Trail Renderer
        // =====================================================================

        TrailRenderer* GetTrailRenderer() { return m_trailRenderer.get(); }

    private:
        uint32 MakePipelineKey(ParticleRenderMode mode, ParticleBlendMode blend, bool soft);
        void CreateQuadBuffers();
        void UploadRenderConstants(const ViewData& view, const SoftParticleConfig& softConfig);

        IRHIDevice* m_device = nullptr;

        // Pipeline cache
        std::unordered_map<uint32, RHIPipelineRef> m_pipelineCache;

        // Shared geometry
        RHIBufferRef m_quadVertexBuffer;
        RHIBufferRef m_quadIndexBuffer;

        // Render constants
        RHIBufferRef m_renderConstantsBuffer;

        // Trail renderer
        std::unique_ptr<TrailRenderer> m_trailRenderer;
    };

} // namespace RVX::Particle
