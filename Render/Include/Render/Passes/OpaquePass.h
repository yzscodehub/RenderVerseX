#pragma once

/**
 * @file OpaquePass.h
 * @brief Opaque geometry render pass
 */

#include "Render/Passes/IRenderPass.h"
#include "Render/Graph/RenderGraph.h"
#include <vector>

namespace RVX
{
    // Forward declarations
    class GPUResourceManager;
    class PipelineCache;
    class RenderScene;

    /**
     * @brief Opaque geometry render pass
     * 
     * Renders all opaque geometry in the scene with front-to-back sorting
     * for optimal early-z rejection.
     * 
     * Uses separate vertex buffer slots:
     *   Slot 0: Position
     *   Slot 1: Normal
     *   Slot 2: UV
     */
    class OpaquePass : public IRenderPass
    {
    public:
        OpaquePass() = default;
        ~OpaquePass() override = default;

        const char* GetName() const override { return "OpaquePass"; }
        int32_t GetPriority() const override { return PassPriority::Opaque; }

        void OnAdd(IRHIDevice* device) override;
        void OnRemove() override;

        void Setup(RenderGraphBuilder& builder, const ViewData& view) override;
        void Execute(RHICommandContext& ctx, const ViewData& view) override;

        // =====================================================================
        // Resource Dependencies
        // =====================================================================

        /**
         * @brief Set resource dependencies before rendering
         * @param gpuMgr GPU resource manager for mesh buffers
         * @param pipelines Pipeline cache for shaders and pipelines
         */
        void SetResources(GPUResourceManager* gpuMgr, PipelineCache* pipelines);

        /**
         * @brief Set render scene data for this frame
         * @param scene The render scene containing objects
         * @param visibleIndices Indices of visible objects (after culling)
         */
        void SetRenderScene(const RenderScene* scene, const std::vector<uint32_t>* visibleIndices);

        // =====================================================================
        // Render Targets
        // =====================================================================

        /**
         * @brief Set render target views for this pass
         */
        void SetRenderTargets(RHITextureView* colorTargetView, RHITextureView* depthTargetView);

    private:
        RGTextureHandle m_colorTargetHandle;
        RGTextureHandle m_depthTargetHandle;

        // Resource dependencies
        GPUResourceManager* m_gpuResources = nullptr;
        PipelineCache* m_pipelineCache = nullptr;
        const RenderScene* m_renderScene = nullptr;
        const std::vector<uint32_t>* m_visibleIndices = nullptr;

        // Render target views
        RHITextureView* m_colorTargetView = nullptr;
        RHITextureView* m_depthTargetView = nullptr;

        // Device reference
        IRHIDevice* m_device = nullptr;
    };

} // namespace RVX
