#pragma once

/**
 * @file OpaquePass.h
 * @brief Opaque geometry render pass
 */

#include "Render/Graph/RenderGraph.h"
#include "Render/GPUDriven/GPUDrivenDiagnostics.h"
#include "Render/Passes/IRenderPass.h"
#include "Render/Renderer/RenderDrawItem.h"
#include "RHI/RHICommandContext.h"
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace RVX
{
    class RenderResourceRegistry;
    // Forward declarations
    class ClusteredLighting;
    class GPUCulling;
    class LightManager;
    class MaterialSystem;
    class PipelineCache;
    class RenderScene;
    class ShadowPass;
    struct GPUCullingDrawGroup;

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
        void AddToGraph(RenderGraph& graph, const ViewData& view) override;
        void AddToGraph(RenderGraph& graph,
                        const RenderPassRecordContext& context) override;

        // =====================================================================
        // Resource Dependencies
        // =====================================================================

        /** @brief Set render-owned resource dependencies before rendering. */
        void SetResources(PipelineCache* pipelines,
                          MaterialSystem* materialSystem,
                          LightManager* lightManager = nullptr,
                          ClusteredLighting* clusteredLighting = nullptr);
        void SetResourceRegistry(const RenderResourceRegistry* registry)
        {
            m_resourceRegistry = registry;
        }

        /**
         * @brief Set render scene data for this frame
         * @param scene The render scene containing objects
         * @param opaqueDrawItems Opaque submesh draw items
         * @param maskedDrawItems Alpha-masked submesh draw items
         */
        void SetRenderScene(const RenderScene* scene,
                            const std::vector<RenderDrawItem>* opaqueDrawItems,
                            const std::vector<RenderDrawItem>* maskedDrawItems);

        void SetDirectionalShadowSource(const ShadowPass* shadowPass);
        /** @brief Set graph-owned directional-shadow inputs for one recording. */
        void SetDirectionalShadowRecordInputs(
            const DirectionalShadowRecordOutput& inputs)
        {
            m_directionalShadowInputs = inputs;
        }
        /** @brief Set graph-owned ray-traced-shadow inputs for one recording. */
        void SetRayTracedShadowRecordInputs(
            const RayTracedShadowRecordOutput& inputs)
        {
            m_rayTracedShadowInputs = inputs;
        }
        void SetGPUDrivenCullingSource(const GPUCulling* gpuCulling);
        /** @brief Standalone compatibility injection for legacy validation. */
        void SetGPUDrivenRenderGraphResources(RGBufferHandle instanceBuffer,
                                              RGBufferHandle instanceIndexBuffer,
                                              RGBufferHandle indirectDrawBuffer,
                                              RGBufferHandle drawCountBuffer);
        const OpaquePassShadowStats& GetShadowStats() const
        {
            return m_publishedRecordResults
                ? m_publishedRecordResults->opaqueShadowStats : m_shadowStats;
        }
        const OpaquePassDrawStats& GetDrawStats() const
        {
            return m_publishedRecordResults
                ? m_publishedRecordResults->opaqueStats : m_drawStats;
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
        void SetGPUDrivenOpaqueIndirectEnabled(bool enabled) { m_gpuDrivenOpaqueIndirectEnabled = enabled; }

        // =====================================================================
        // Render Targets
        // =====================================================================

        /**
         * @brief Set render target views for standalone compatibility rendering.
         * Typed graph recordings resolve their attachments from RenderGraph handles.
         */
        void SetRenderTargets(RHITextureView* colorTargetView, RHITextureView* depthTargetView);

    private:
        struct PlannedOpaqueDraw;

        RGTextureHandle m_colorTargetHandle;
        RGTextureHandle m_depthTargetHandle;
        RGTextureHandle m_directionalShadowReadHandle;
        RGTextureHandle m_rayTracedShadowMaskReadHandle;
        RGBufferHandle m_gpuDrivenInstanceHandle;
        RGBufferHandle m_gpuDrivenInstanceIndexHandle;
        RGBufferHandle m_gpuDrivenIndirectHandle;
        RGBufferHandle m_gpuDrivenDrawCountHandle;
        OpaquePassShadowStats m_shadowStats;
        OpaquePassDrawStats m_drawStats;
        DirectionalShadowRecordOutput m_directionalShadowInputs;
        RayTracedShadowRecordOutput m_rayTracedShadowInputs;
        std::shared_ptr<RenderPassRecordResults> m_publishedRecordResults;

        // Resource dependencies
        const RenderResourceRegistry* m_resourceRegistry = nullptr;
        PipelineCache* m_pipelineCache = nullptr;
        MaterialSystem* m_materialSystem = nullptr;
        LightManager* m_lightManager = nullptr;
        ClusteredLighting* m_clusteredLighting = nullptr;
        const RenderScene* m_renderScene = nullptr;
        const ShadowPass* m_shadowPass = nullptr;
        const GPUCulling* m_gpuCulling = nullptr;
        const std::vector<RenderDrawItem>* m_opaqueDrawItems = nullptr;
        const std::vector<RenderDrawItem>* m_maskedDrawItems = nullptr;

        // Render target views
        RHITextureView* m_colorTargetView = nullptr;
        RHITextureView* m_depthTargetView = nullptr;
        // Typed recordings may only consume the current RenderGraph's handles.
        bool m_requireGraphOwnedAttachments = false;

        // The no-plan GPU submission path is intentionally opt-in.  It exists
        // only for compatibility validation and must never become a Direct
        // rendering fallback when the frame policy was not published.
        bool m_gpuDrivenOpaqueIndirectEnabled = false;
        bool AreGPUDrivenOpaqueGroupsDrawable(
            uint32 expectedPacketCount,
            uint32 expectedGroupCount,
            uint32& outDrawItemCount) const;
        bool TryDrawGPUDrivenIndirect(RHICommandContext& ctx,
                                      const ViewData& view,
                                      RHIFormat colorTargetFormat,
                                      RHIDescriptorSet* frameSet,
                                      bool requireObjectConstantUpload,
                                      uint32 expectedPacketCount = 0,
                                      uint32 expectedGroupCount = 0);
        bool TryDrawPlannedDirect(
            RHICommandContext& ctx,
            std::span<const PlannedOpaqueDraw> plannedDraws);
        bool BuildPlannedDirectBatch(
            const ViewData& view,
            RHIFormat colorTargetFormat,
            RHIDescriptorSet* frameSet,
            std::vector<PlannedOpaqueDraw>& outPlannedDraws);

    };

} // namespace RVX
