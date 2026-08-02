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
        bool gpuDrivenRequested = false;
        bool gpuDrivenCullingReady = false;
        bool gpuDrivenPipelineReady = false;
        bool gpuDrivenEligible = false;
        bool gpuDrivenSubmitted = false;
        uint32 gpuDrivenIndirectBatchCount = 0;
        uint32 gpuDrivenIndirectDrawCount = 0;
        GPUDrivenDrawFallbackReason gpuDrivenFallbackReason =
            GPUDrivenDrawFallbackReason::Disabled;
        uint32 skippedMaterialBindingCount = 0;
        bool planRequested = false;
        bool planValidated = false;
        bool directPacketPathUsed = false;
        uint32 plannedPacketCount = 0;
        uint32 executedPacketCount = 0;
        RenderPolicyReason failureReason =
            RenderPolicyReason::ConservativeDefault;
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
                                              RGBufferHandle instanceIndexBuffer,
                                              RGBufferHandle indirectDrawBuffer,
                                              RGBufferHandle drawCountBuffer);
        const OpaquePassShadowStats& GetShadowStats() const { return m_shadowStats; }
        const OpaquePassDrawStats& GetDrawStats() const { return m_drawStats; }
        void SetGPUDrivenOpaqueIndirectEnabled(bool enabled) { m_gpuDrivenOpaqueIndirectEnabled = enabled; }

        // =====================================================================
        // Render Targets
        // =====================================================================

        /**
         * @brief Set render target views for this pass
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
            std::vector<PlannedOpaqueDraw>& outPlannedDraws);

    };

} // namespace RVX
