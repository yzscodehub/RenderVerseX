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
#include "Render/Submission/OpaqueRasterSubmission.h"
#include "RHI/RHICommandContext.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace RVX
{
    class RenderResourceRegistry;
    // Forward declarations
    class ClusteredLighting;
    class GPUCulling;
    class LightingFramePlan;
    class LightManager;
    class MaterialSystem;
    class PipelineCache;
    class RenderScene;
    class ShadowPass;
    class DirectRasterReadbackQualification;
    struct MaterialBindingResult;
    struct ObjectConstantBinding;
    struct GPUCullingDrawGroup;
    struct GPUSceneRasterBindingSnapshot;
    struct LightingBindingSnapshot;
    struct RenderPassRecordContext;
    struct RenderPassRecordResults;
    struct RenderLightingFrameResourceSnapshot;
    class RenderDeformationFrameResources;

    /** @brief Result of resolving every frozen opaque-raster pipeline request. */
    enum class OpaqueRasterPipelinePreflightCode : uint8
    {
        Ready = 0,
        ReadyNoWork,
        DependenciesUnavailable,
        InvalidRecordContext,
        InvalidExecutionPlan,
        InvalidPacketPartition,
        MissingGPURecording,
        StaleGPURecording,
        InvalidGPUGroup,
        PipelineUnavailable,
    };

    /**
     * @brief Fail-closed readiness for one immutable opaque-raster route.
     *
     * This is deliberately evaluated from the published frame plan, not from
     * mutable scene lists.  It lets a recipe reject a required route before a
     * graph callback or attachment write is registered.
     */
    struct OpaqueRasterPipelinePreflightResult
    {
        OpaqueRasterPipelinePreflightCode code =
            OpaqueRasterPipelinePreflightCode::DependenciesUnavailable;
        uint32 selectedDirectPacketCount = 0;
        uint32 selectedGPUDrivenPacketCount = 0;
        uint32 matchedGPUGroupCount = 0;
        uint32 uniquePipelineRequestCount = 0;
        bool hasFailedRequest = false;
        OpaqueRasterPipelineRequest failedRequest{};
        std::string diagnostic{};

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return code == OpaqueRasterPipelinePreflightCode::Ready ||
                   code == OpaqueRasterPipelinePreflightCode::ReadyNoWork;
        }
    };

    /** @brief Graph-owned attachments consumed by the deferred opaque path. */
    struct OpaqueDeferredGBufferTargets
    {
        std::array<RGTextureHandle, 4> color{};
        RGTextureHandle depth{};

        [[nodiscard]] bool IsValid() const noexcept
        {
            return depth.IsValid() &&
                   std::all_of(color.begin(), color.end(),
                               [](const RGTextureHandle& handle)
                               {
                                   return handle.IsValid();
                               });
        }
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

        void AddToGraph(RenderGraph& graph,
                        const RenderPassRecordContext& context) override;

        /** @brief Register the shared opaque submission against four GBuffer targets. */
        void AddDeferredToGraph(
            RenderGraph& graph,
            const RenderPassRecordContext& context,
            const OpaqueDeferredGBufferTargets& targets);

        /**
         * @brief Record only ForwardSpecialOpaque packets into an existing HDR
         * scene color after deferred lighting.
         */
        void AddForwardSpecialToGraph(
            RenderGraph& graph,
            const RenderPassRecordContext& context);

        /**
         * @brief Resolve every pipeline used by the frozen opaque-plan route.
         *
         * A successful empty route returns ReadyNoWork and checks only the
         * canonical family metadata. Direct instancing is intentionally not
         * inferred here: deferred geometry records ordinary frozen Direct
         * packets, while GPU requests are derived solely from sealed culling
         * groups.
         */
        [[nodiscard]] OpaqueRasterPipelinePreflightResult
        PreflightRasterPipelines(
            const RenderPassRecordContext& context,
            OpaqueRasterOutputFamily outputFamily,
            OpaqueRasterSurfaceRoute surfaceRoute) const;

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
        void Execute(RenderGraphPassContext& context,
                     const ViewData& view,
                     RenderPassRecordResults* results);
        void InitializeGraphRecorder(
            const RenderScene* scene,
            const std::vector<RenderDrawItem>* opaqueDrawItems,
            const std::vector<RenderDrawItem>* maskedDrawItems,
            const GPUCulling* gpuCulling,
            const RenderPassGPUDrivenInputs& gpuInputs,
            bool gpuDrivenPlanned,
            std::shared_ptr<const LightingFramePlan> lightingPlan,
            std::shared_ptr<const RenderLightingFrameResourceSnapshot>
                lightingResources,
            std::shared_ptr<const RenderDeformationFrameResources>
                deformationResources,
            const DirectionalShadowRecordOutput& directionalShadow,
            const RayTracedShadowRecordOutput& rayTracedShadow,
            std::shared_ptr<RasterInstanceStreamCache> directInstanceStreamCache,
            DirectRasterReadbackQualification* directReadbackQualification,
            const RenderPassRecordIdentity& recordIdentity,
            uint32 sourceFrameSlot,
            OpaqueRasterOutputFamily outputFamily,
            OpaqueRasterSurfaceRoute surfaceRoute,
            OpaqueDeferredGBufferTargets deferredTargets);
        void AddToGraphInternal(
            RenderGraph& graph,
            const RenderPassRecordContext& context,
            OpaqueRasterOutputFamily outputFamily,
            OpaqueRasterSurfaceRoute surfaceRoute,
            const OpaqueDeferredGBufferTargets& deferredTargets);

        RGTextureHandle m_colorTargetHandle;
        RGTextureHandle m_depthTargetHandle;
        RGTextureHandle m_directionalShadowReadHandle;
        RGTextureHandle m_rayTracedShadowMaskReadHandle;
        RGBufferHandle m_lightConstantsReadHandle;
        RGBufferHandle m_pointLightsReadHandle;
        RGBufferHandle m_spotLightsReadHandle;
        RGBufferHandle m_clusterConstantsReadHandle;
        RGBufferHandle m_clusterDataReadHandle;
        RGBufferHandle m_clusterLightIndicesReadHandle;
        /** Exact committed prior-presented pre-exposure state for DefaultLit. */
        RGBufferHandle m_exposureStateReadHandle;
        RGBufferHandle m_currentPaletteReadHandle;
        RGBufferHandle m_previousPaletteReadHandle;
        RGTextureViewHandle m_colorTargetViewHandle;
        RGTextureViewHandle m_depthTargetViewHandle;
        RGTextureViewHandle m_directionalShadowViewHandle;
        RGTextureViewHandle m_rayTracedShadowMaskViewHandle;
        RGTextureViewHandle m_environmentIBLIrradianceViewHandle;
        RGTextureViewHandle m_environmentIBLPrefilteredViewHandle;
        RGTextureViewHandle m_environmentIBLBRDFLUTViewHandle;
        std::array<RGTextureHandle, 4> m_gBufferTargetHandles{};
        std::array<RGTextureViewHandle, 4> m_gBufferTargetViewHandles{};
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
        std::shared_ptr<RasterInstanceStreamCache> m_directInstanceStreamCache =
            std::make_shared<RasterInstanceStreamCache>();
        DirectRasterReadbackQualification* m_directReadbackQualification = nullptr;
        RenderPassRecordIdentity m_recordIdentity{};
        uint32 m_directReadbackSourceFrameSlot = RVX_INVALID_INDEX;
        RasterInstanceStream m_directInstanceStream;
        RHIBufferRef m_directMaterialParameterTable;
        RHIBufferRef m_gpuMaterialParameterTable;
        /** CPU-only resolved table semantics, indexed by material slot. */
        std::vector<uint64> m_directRasterMaterialSemanticKeysBySlot;
        std::vector<uint64> m_gpuRasterMaterialSemanticKeysBySlot;
        bool m_directInstancingPreflightFailed = false;
        bool m_gpuMaterialTablePreflightFailed = false;
        std::shared_ptr<const GPUSceneRasterBindingSnapshot> m_gpuSceneRasterBinding;
        std::shared_ptr<std::atomic_bool> m_gpuSceneRecordingFailure;
        OpaquePassShadowStats m_shadowStats;
        OpaquePassDrawStats m_drawStats;
        std::shared_ptr<const LightingFramePlan> m_lightingPlan;
        std::shared_ptr<const RenderLightingFrameResourceSnapshot>
            m_lightingResources;
        std::shared_ptr<const RenderDeformationFrameResources>
            m_deformationResources;
        std::shared_ptr<LightingBindingSnapshot> m_lightingBindingSnapshot;
        DirectionalShadowRecordOutput m_directionalShadowInputs;
        RayTracedShadowRecordOutput m_rayTracedShadowInputs;
        std::shared_ptr<RenderPassRecordResults> m_publishedRecordResults;
        OpaqueRasterOutputFamily m_outputFamily =
            OpaqueRasterOutputFamily::ForwardSceneHDR;
        OpaqueRasterSurfaceRoute m_surfaceRoute =
            OpaqueRasterSurfaceRoute::AllOpaque;
        OpaqueDeferredGBufferTargets m_deferredTargets{};

        // Resource dependencies
        const RenderResourceRegistry* m_resourceRegistry = nullptr;
        PipelineCache* m_pipelineCache = nullptr;
        MaterialSystem* m_materialSystem = nullptr;
        LightManager* m_lightManager = nullptr;
        ClusteredLighting* m_clusteredLighting = nullptr;
        const RenderScene* m_renderScene = nullptr;
        const GPUCulling* m_gpuCulling = nullptr;
        std::shared_ptr<GPUCullingRecordedState> m_gpuCullingRecordedState;
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
                                      RenderPassRecordResults* results,
                                      uint32 expectedPacketCount = 0,
                                      uint32 expectedGroupCount = 0);
        bool TryDrawPlannedDirect(
            RHICommandContext& ctx,
            std::span<const PlannedOpaqueDraw> plannedDraws,
            RenderPassRecordResults* results);
        bool BuildPlannedDirectBatch(
            RenderGraphPassContext& context,
            const ViewData& view,
            RHIFormat colorTargetFormat,
            RHIDescriptorSet* frameSet,
            std::vector<PlannedOpaqueDraw>& outPlannedDraws);
        bool PrepareDirectInstanceStream(RenderGraphBuilder& builder,
                                         const ViewData& view);
        [[nodiscard]] bool FinalizeDirectRasterSemanticEvidence(
            std::span<const PlannedOpaqueDraw> plannedDraws);
        [[nodiscard]] bool BuildCompleteDirectRasterTranscript(
            std::span<const PlannedOpaqueDraw> plannedDraws,
            RasterTranscriptDigest& outDigest) const;
        void ApplyDirectInstancePlan(
            RenderGraphPassContext& context,
            const ViewData& view,
            RHIFormat colorTargetFormat,
            std::vector<PlannedOpaqueDraw>& plannedDraws);

    };

} // namespace RVX
