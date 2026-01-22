#pragma once

/**
 * @file TransparentPass.h
 * @brief Transparent geometry render pass with alpha blending
 * 
 * TransparentPass renders transparent/alpha-blended geometry after opaque passes,
 * using back-to-front sorting for correct blending order.
 */

#include "Render/Passes/IRenderPass.h"
#include "Render/GPUResourceManager.h"
#include "Render/PipelineCache.h"

namespace RVX
{
    class RenderScene;

    /**
     * @brief Transparent geometry render pass
     * 
     * Renders objects with alpha blending in back-to-front order.
     * 
     * Key characteristics:
     * - Runs after opaque pass (priority 500)
     * - Reads depth buffer (no depth write)
     * - Uses alpha blending
     * - Objects sorted by camera distance (back-to-front)
     */
    class TransparentPass : public IRenderPass
    {
    public:
        TransparentPass();
        ~TransparentPass() override = default;

        // =========================================================================
        // IRenderPass Interface
        // =========================================================================

        const char* GetName() const override { return "TransparentPass"; }
        
        int32_t GetPriority() const override { return 500; }  // After opaque (300), sky (400)
        
        RenderGraphPassType GetPassType() const override { return RenderGraphPassType::Graphics; }

        void Setup(RenderGraphBuilder& builder, const ViewData& view) override;
        void Execute(RHICommandContext& ctx, const ViewData& view) override;

        // =========================================================================
        // Configuration
        // =========================================================================

        /**
         * @brief Set resources needed for rendering
         */
        void SetResources(GPUResourceManager* gpuResources, PipelineCache* pipelineCache);

        /**
         * @brief Set render scene and visible transparent objects
         * @param scene The render scene
         * @param transparentIndices Indices of visible transparent objects (pre-sorted back-to-front)
         */
        void SetRenderScene(const RenderScene* scene, const std::vector<uint32_t>* transparentIndices);

        /**
         * @brief Set the render targets
         * @param colorTargetView Color target for blending
         * @param depthTargetView Depth target for depth testing (read-only)
         */
        void SetRenderTargets(RHITextureView* colorTargetView, RHITextureView* depthTargetView);

        /**
         * @brief Enable or disable this pass
         */
        void SetEnabled(bool enabled) { m_enabled = enabled; }

        bool IsEnabled() const override { return m_enabled; }

    private:
        bool m_enabled = true;
        GPUResourceManager* m_gpuResources = nullptr;
        PipelineCache* m_pipelineCache = nullptr;
        const RenderScene* m_renderScene = nullptr;
        const std::vector<uint32_t>* m_transparentIndices = nullptr;
        RHITextureView* m_colorTargetView = nullptr;
        RHITextureView* m_depthTargetView = nullptr;

        // RenderGraph handles
        RGTextureHandle m_colorTargetHandle;
        RGTextureHandle m_depthTargetHandle;
    };

} // namespace RVX
