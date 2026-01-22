#pragma once

/**
 * @file DepthPrepass.h
 * @brief Depth-only prepass for early-Z rejection optimization
 * 
 * DepthPrepass renders all opaque geometry to the depth buffer only,
 * enabling early-Z rejection in subsequent passes to reduce overdraw.
 */

#include "Render/Passes/IRenderPass.h"
#include "Render/GPUResourceManager.h"
#include "Render/PipelineCache.h"

namespace RVX
{
    class RenderScene;

    /**
     * @brief Depth prepass for early-Z optimization
     * 
     * Renders opaque geometry with a minimal depth-only shader before
     * the main opaque pass. This populates the depth buffer, allowing
     * the GPU to skip shading for occluded fragments.
     * 
     * Benefits:
     * - Reduces pixel shader invocations for occluded geometry
     * - Particularly effective for complex scenes with high overdraw
     * - Enables Hi-Z culling on modern GPUs
     */
    class DepthPrepass : public IRenderPass
    {
    public:
        DepthPrepass();
        ~DepthPrepass() override = default;

        // =========================================================================
        // IRenderPass Interface
        // =========================================================================

        const char* GetName() const override { return "DepthPrepass"; }
        
        int32_t GetPriority() const override { return 50; }  // Run before opaque (100)
        
        RenderGraphPassType GetPassType() const override { return RenderGraphPassType::Graphics; }

        void Setup(RenderGraphBuilder& builder, const ViewData& view) override;
        void Execute(RHICommandContext& ctx, const ViewData& view) override;

        // =========================================================================
        // Configuration
        // =========================================================================

        /**
         * @brief Set resources needed for rendering
         * @param gpuResources GPU resource manager for mesh data
         * @param pipelineCache Pipeline cache for depth-only pipeline
         */
        void SetResources(GPUResourceManager* gpuResources, PipelineCache* pipelineCache);

        /**
         * @brief Set render scene and visible objects
         * @param scene The render scene
         * @param visibleIndices Indices of visible objects
         */
        void SetRenderScene(const RenderScene* scene, const std::vector<uint32_t>* visibleIndices);

        /**
         * @brief Set the depth target view
         * @param depthView The depth texture view to render to
         */
        void SetDepthTarget(RHITextureView* depthView);

        /**
         * @brief Enable or disable this pass
         */
        void SetEnabled(bool enabled) { m_enabled = enabled; }

        bool IsEnabled() const override { return m_enabled; }

    private:
        bool m_enabled = false;  // Disabled by default until depth-only pipeline is ready
        GPUResourceManager* m_gpuResources = nullptr;
        PipelineCache* m_pipelineCache = nullptr;
        const RenderScene* m_renderScene = nullptr;
        const std::vector<uint32_t>* m_visibleIndices = nullptr;
        RHITextureView* m_depthTargetView = nullptr;
    };

} // namespace RVX
