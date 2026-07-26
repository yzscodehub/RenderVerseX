#pragma once

/**
 * @file OpaquePass.h
 * @brief Opaque geometry render pass
 */

#include "Render/Graph/RenderGraph.h"
#include "Render/Passes/IRenderPass.h"
#include "Render/Renderer/RenderDrawItem.h"
#include "RHI/RHICommandContext.h"
#include <cstdint>
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
    class RayTracedShadowPass;
    class RenderScene;
    class ShadowPass;
    struct GPUCullingDrawGroup;

    struct OpaquePassShadowStats
    {
        bool requested = false;
        bool renderGraphReadDeclared = false;
        bool frameShadowReady = false;
        bool rayTracedRequested = false;
        bool rayTracedRenderGraphReadDeclared = false;
        bool rayTracedFrameMaskReady = false;
        uint32 receiverCandidateDrawItemCount = 0;
        uint32 shadowReceivingDrawItemCount = 0;
        uint32 shadowReceiverOptOutDrawItemCount = 0;
    };

    struct OpaquePassDrawStats
    {
        uint32 directDrawCount = 0;
        uint32 indirectBatchCount = 0;
        uint32 indirectDrawCount = 0;
        bool gpuDrivenRequested = false;
        bool gpuDrivenEligible = false;
        uint32 gpuDrivenIndirectBatchCount = 0;
        uint32 gpuDrivenIndirectDrawCount = 0;
        uint32 skippedInvalidObjectCount = 0;
        uint32 skippedMissingMeshCount = 0;
        uint32 skippedInvalidSubmeshCount = 0;
        uint32 skippedMaterialBindingCount = 0;
    };

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
        void SetRayTracedShadowSource(const RayTracedShadowPass* shadowPass);
        void SetGPUDrivenCullingSource(const GPUCulling* gpuCulling);
        void SetGPUDrivenRenderGraphResources(RGBufferHandle instanceBuffer,
                                              RGBufferHandle indirectDrawBuffer,
                                              RGBufferHandle drawCountBuffer);
        const OpaquePassShadowStats& GetShadowStats() const { return m_shadowStats; }
        const OpaquePassDrawStats& GetDrawStats() const { return m_drawStats; }
        void SetIndirectBatchingEnabled(bool enabled) { m_indirectBatchingEnabled = enabled; }
        void SetGPUDrivenOpaqueIndirectEnabled(bool enabled) { m_gpuDrivenOpaqueIndirectEnabled = enabled; }

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
        RGTextureHandle m_directionalShadowReadHandle;
        RGTextureHandle m_rayTracedShadowMaskReadHandle;
        RGBufferHandle m_gpuDrivenInstanceHandle;
        RGBufferHandle m_gpuDrivenIndirectHandle;
        RGBufferHandle m_gpuDrivenDrawCountHandle;
        OpaquePassShadowStats m_shadowStats;
        OpaquePassDrawStats m_drawStats;

        // Resource dependencies
        const RenderResourceRegistry* m_resourceRegistry = nullptr;
        PipelineCache* m_pipelineCache = nullptr;
        MaterialSystem* m_materialSystem = nullptr;
        LightManager* m_lightManager = nullptr;
        ClusteredLighting* m_clusteredLighting = nullptr;
        const RenderScene* m_renderScene = nullptr;
        const ShadowPass* m_shadowPass = nullptr;
        const RayTracedShadowPass* m_rayTracedShadowPass = nullptr;
        const GPUCulling* m_gpuCulling = nullptr;
        const std::vector<RenderDrawItem>* m_opaqueDrawItems = nullptr;
        const std::vector<RenderDrawItem>* m_maskedDrawItems = nullptr;

        // Render target views
        RHITextureView* m_colorTargetView = nullptr;
        RHITextureView* m_depthTargetView = nullptr;

        // Device reference
        IRHIDevice* m_device = nullptr;

        bool m_indirectBatchingEnabled = true;
        bool m_gpuDrivenOpaqueIndirectEnabled = true;
        RHIBufferRef m_indirectDrawBuffer;
        uint32 m_indirectDrawBufferCapacity = 0;
        std::vector<IndirectDrawIndexedCommand> m_indirectDrawCommands;

        uint32 FindIndirectBatchLength(const std::vector<RenderDrawItem>& drawItems,
                                       size_t startIndex) const;
        bool EnsureIndirectDrawCapacity(uint32 commandCount);
        const RenderDrawItem* FindGPUDrivenGroupRepresentative(const GPUCullingDrawGroup& group) const;
        bool AreGPUDrivenOpaqueGroupsDrawable(uint32& outDrawItemCount) const;
        bool TryDrawGPUDrivenIndirect(RHICommandContext& ctx,
                                      const ViewData& view,
                                      RHIFormat colorTargetFormat,
                                      RHIDescriptorSet* frameSet);

    };

} // namespace RVX
