#pragma once

/**
 * @file OpaquePass.h
 * @brief Opaque geometry render pass
 */

#include "Render/Graph/RenderGraph.h"
#include "Render/GPUDriven/GPUDrivenDiagnostics.h"
#include "Render/Passes/IRenderPass.h"
#include "Render/Renderer/RenderDrawItem.h"
#include "Render/Submission/RasterInstanceStream.h"
#include "Render/Submission/RenderInstanceBatchPlan.h"
#include "RHI/RHICommandContext.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
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
    struct MaterialBindingResult;
    struct ObjectConstantBinding;
    struct GPUCullingDrawGroup;
    struct GPUSceneRasterBindingSnapshot;

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
    private:
        struct PlannedOpaqueDraw;
        struct PlannedGPUDrivenOpaqueDraw;

        void Setup(RenderGraphBuilder& builder, const ViewData& view) override;
        void Execute(RHICommandContext& ctx, const ViewData& view) override;
        void Execute(RenderGraphPassContext& context, const ViewData& view);
        void InitializeGraphRecorder(
            const RenderScene* scene,
            const std::vector<RenderDrawItem>* opaqueDrawItems,
            const std::vector<RenderDrawItem>* maskedDrawItems,
            const GPUCulling* gpuCulling,
            const RenderPassGPUDrivenInputs& gpuInputs,
            bool gpuDrivenPlanned,
            const DirectionalShadowRecordOutput& directionalShadow,
            const RayTracedShadowRecordOutput& rayTracedShadow);

        RGTextureHandle m_colorTargetHandle;
        RGTextureHandle m_depthTargetHandle;
        RGTextureHandle m_directionalShadowReadHandle;
        RGTextureHandle m_rayTracedShadowMaskReadHandle;
        RGTextureViewHandle m_colorTargetViewHandle;
        RGTextureViewHandle m_depthTargetViewHandle;
        RGTextureViewHandle m_directionalShadowViewHandle;
        RGTextureViewHandle m_rayTracedShadowMaskViewHandle;
        RHIFormat m_colorTargetFormat = RHIFormat::Unknown;
        RGBufferHandle m_gpuDrivenInstanceHandle;
        RGBufferHandle m_gpuSceneCandidateHandle;
        RGBufferHandle m_gpuScenePrimitiveHandle;
        RGBufferHandle m_gpuSceneTransformHandle;
        RGBufferHandle m_gpuDrivenInstanceIndexHandle;
        RGBufferHandle m_gpuDrivenIndirectHandle;
        RGBufferHandle m_gpuDrivenDrawCountHandle;
        RGBufferHandle m_directInstanceHandle;
        RGBufferHandle m_directInstanceIndexHandle;
        RGBufferHandle m_directMaterialParameterHandle;
        RGBufferHandle m_gpuMaterialParameterHandle;
        RenderInstanceBatchPlan m_directInstancePlan;
        RasterInstanceStream m_directInstanceStream;
        RHIBufferRef m_directMaterialParameterTable;
        RHIBufferRef m_gpuMaterialParameterTable;
        bool m_directInstancingPreflightFailed = false;
        bool m_gpuMaterialTablePreflightFailed = false;
        std::shared_ptr<const GPUSceneRasterBindingSnapshot> m_gpuSceneRasterBinding;
        std::shared_ptr<std::atomic_bool> m_gpuSceneRecordingFailure;
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
        const GPUCulling* m_gpuCulling = nullptr;
        const std::vector<RenderDrawItem>* m_opaqueDrawItems = nullptr;
        const std::vector<RenderDrawItem>* m_maskedDrawItems = nullptr;

        bool m_gpuDrivenOpaqueIndirectEnabled = false;
        bool m_gpuSceneRasterEnabled = false;
        bool AreGPUDrivenOpaqueGroupsDrawable(
            uint32 expectedPacketCount,
            uint32 expectedGroupCount,
            uint32& outDrawItemCount) const;
        bool TryDrawGPUDrivenIndirect(RHICommandContext& ctx,
                                      RHIDescriptorSet* frameSet,
                                      const ObjectConstantBinding* tier1ObjectBinding,
                                      std::span<const PlannedGPUDrivenOpaqueDraw> plannedBatches,
                                      uint32 expectedPacketCount = 0,
                                      uint32 expectedGroupCount = 0);
        bool TryDrawPlannedDirect(
            RHICommandContext& ctx,
            std::span<const PlannedOpaqueDraw> plannedDraws);
        bool BuildPlannedDirectBatch(
            RenderGraphPassContext& context,
            const ViewData& view,
            RHIFormat colorTargetFormat,
            RHIDescriptorSet* frameSet,
            std::vector<PlannedOpaqueDraw>& outPlannedDraws);
        bool PrepareDirectInstanceStream(RenderGraphBuilder& builder,
                                         const ViewData& view);
        void ApplyDirectInstancePlan(
            RenderGraphPassContext& context,
            const ViewData& view,
            RHIFormat colorTargetFormat,
            std::vector<PlannedOpaqueDraw>& plannedDraws);

    };

} // namespace RVX
