#pragma once

/**
 * @file DepthPrepass.h
 * @brief Depth-only prepass for early-Z rejection optimization
 *
 * DepthPrepass renders all opaque geometry to the depth buffer only,
 * enabling early-Z rejection in subsequent passes to reduce overdraw.
 */

#include "Render/Passes/IRenderPass.h"
#include "Render/Passes/DirectDrawPacketBatch.h"
#include "Render/PipelineCache.h"
#include "Render/Renderer/RenderDrawItem.h"

#include <span>
#include <memory>
#include <vector>

namespace RVX
{
    class RenderResourceRegistry;
    class GPUCulling;
    class RenderScene;
    class MaterialSystem;

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
        void AddToGraph(RenderGraph& graph, const ViewData& view) override;
        void AddToGraph(RenderGraph& graph,
                        const RenderPassRecordContext& context) override;

        // =========================================================================
        // Configuration
        // =========================================================================

        /** @brief Set the pipeline dependency before rendering. */
        void SetResources(PipelineCache* pipelineCache);
        void SetMaterialSystem(MaterialSystem* materialSystem)
        {
            m_materialSystem = materialSystem;
        }
        void SetResourceRegistry(const RenderResourceRegistry* registry)
        {
            m_resourceRegistry = registry;
        }

        /**
         * @brief Set render scene and visible objects
         * @param scene The render scene
         * @param opaqueDrawItems Opaque draw items
         * @param maskedDrawItems Alpha-masked draw items
         */
        void SetRenderScene(const RenderScene* scene,
                            const std::vector<RenderDrawItem>* opaqueDrawItems,
                            const std::vector<RenderDrawItem>* maskedDrawItems);

        /**
         * @brief Set GPU-driven culling outputs for indirect depth rendering
         */
        void SetGPUDrivenCullingSource(const GPUCulling* gpuCulling);

        /** @brief Standalone compatibility injection for legacy validation. */
        void SetGPUDrivenRenderGraphResources(RGBufferHandle instanceBuffer,
                                              RGBufferHandle instanceIndexBuffer,
                                              RGBufferHandle indirectDrawBuffer,
                                              RGBufferHandle drawCountBuffer);

        const DepthPrepassDrawStats& GetDrawStats() const
        {
            return m_publishedRecordResults
                ? m_publishedRecordResults->depthStats : m_drawStats;
        }
        void PublishRecordResults(
            const std::shared_ptr<RenderPassRecordResults>& results,
            const RenderPassRecordIdentity& expectedIdentity)
        {
            if (results != nullptr && results->identity == expectedIdentity)
            {
                m_publishedRecordResults = results;
            }
        }

        /** @brief Standalone compatibility switch; SceneRenderer uses the plan. */
        void SetGPUDrivenDepthIndirectEnabled(bool enabled) { m_gpuDrivenDepthIndirectEnabled = enabled; }

        /**
         * @brief Set the depth target view
         * @param depthView The depth texture view to render to
         */
        void SetDepthTarget(RHITextureView* depthView);

        /**
         * @brief Enable or disable this pass
         */
        void SetEnabled(bool enabled) { m_enabled = enabled; }

        bool IsRequestedEnabled() const override { return m_enabled; }
        bool IsSupported() const override;
        const std::string& GetUnsupportedReason() const override { return m_unsupportedReason; }
        bool IsEnabled() const override { return IsRequestedEnabled() && IsSupported(); }

    private:
        struct PlannedDepthDraw;

        bool AreGPUDrivenDepthGroupsDrawable(
            uint32 expectedPacketCount,
            uint32 expectedGroupCount,
            uint32& outDrawItemCount) const;
        bool TryDrawGPUDrivenIndirect(RHICommandContext& ctx,
                                      const ViewData& view,
                                      uint32 expectedPacketCount = 0,
                                      uint32 expectedGroupCount = 0);
        bool TryDrawPlannedDirect(
            RHICommandContext& ctx,
            std::span<const PlannedDepthDraw> plannedDraws);
        bool BuildPlannedDirectBatch(
            const ViewData& view,
            std::vector<PlannedDepthDraw>& outPlannedDraws);

        bool m_enabled = false;  // Disabled by default until depth-only pipeline is ready
        std::string m_unsupportedReason = "Depth-only pipeline is not available";
        const RenderResourceRegistry* m_resourceRegistry = nullptr;
        PipelineCache* m_pipelineCache = nullptr;
        MaterialSystem* m_materialSystem = nullptr;
        const RenderScene* m_renderScene = nullptr;
        const GPUCulling* m_gpuCulling = nullptr;
        const std::vector<RenderDrawItem>* m_opaqueDrawItems = nullptr;
        const std::vector<RenderDrawItem>* m_maskedDrawItems = nullptr;
        RHITextureView* m_depthTargetView = nullptr;
        RGTextureHandle m_depthTargetHandle;
        RGBufferHandle m_gpuDrivenInstanceHandle;
        RGBufferHandle m_gpuDrivenInstanceIndexHandle;
        RGBufferHandle m_gpuDrivenIndirectHandle;
        RGBufferHandle m_gpuDrivenDrawCountHandle;
        DepthPrepassDrawStats m_drawStats;
        std::shared_ptr<RenderPassRecordResults> m_publishedRecordResults;
        // Standalone, no-plan GPU submission is a compatibility path only.
        // SceneRenderer opts in explicitly after it has published frame policy.
        bool m_gpuDrivenDepthIndirectEnabled = false;
    };

} // namespace RVX
