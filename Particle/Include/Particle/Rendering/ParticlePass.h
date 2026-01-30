#pragma once

/**
 * @file ParticlePass.h
 * @brief Particle rendering pass for RenderGraph integration
 */

#include "Render/Passes/IRenderPass.h"
#include "Particle/ParticleTypes.h"
#include <vector>

namespace RVX::Particle
{
    class ParticleSystemInstance;
    class ParticleRenderer;
    class ParticleSorter;

    /**
     * @brief Draw batch for particles with same blend mode
     */
    struct ParticleDrawBatch
    {
        ParticleBlendMode blendMode = ParticleBlendMode::AlphaBlend;
        std::vector<ParticleSystemInstance*> instances;
    };

    /**
     * @brief Render pass for particles
     * 
     * Integrates particle rendering with the RenderGraph system.
     * Executes after the transparent pass to ensure correct depth reading.
     */
    class ParticlePass : public IRenderPass
    {
    public:
        ParticlePass();
        ~ParticlePass() override;

        // =====================================================================
        // IRenderPass Interface
        // =====================================================================

        const char* GetName() const override { return "ParticlePass"; }
        
        /// Priority 550: After TransparentPass (500)
        int32_t GetPriority() const override { return 550; }

        void Setup(RenderGraphBuilder& builder, const ViewData& view) override;
        void Execute(RHICommandContext& ctx, const ViewData& view) override;

        // =====================================================================
        // Configuration
        // =====================================================================

        /// Set particle systems to render
        void SetParticleSystems(const std::vector<ParticleSystemInstance*>& instances);

        /// Set the renderer
        void SetRenderer(ParticleRenderer* renderer) { m_renderer = renderer; }

        /// Set the sorter
        void SetSorter(ParticleSorter* sorter) { m_sorter = sorter; }

        /// Enable/disable sorting
        void SetSortingEnabled(bool enabled) { m_sortingEnabled = enabled; }

        /// Enable/disable soft particles
        void SetSoftParticlesEnabled(bool enabled) { m_softParticlesEnabled = enabled; }

    private:
        void SortIntoBatches();
        void SortParticlesByDistance(const ViewData& view);

        ParticleRenderer* m_renderer = nullptr;
        ParticleSorter* m_sorter = nullptr;

        std::vector<ParticleSystemInstance*> m_instances;
        std::vector<ParticleDrawBatch> m_batches;

        RGTextureHandle m_colorTarget;
        RGTextureHandle m_depthTarget;

        bool m_sortingEnabled = true;
        bool m_softParticlesEnabled = true;
    };

} // namespace RVX::Particle
