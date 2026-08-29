#pragma once

/**
 * @file DeferredLightingPass.h
 * @brief Compute lighting for the canonical deferred opaque GBuffer.
 */

#include "Render/Features/RenderFeatureGraphBuilder.h"
#include "Render/Passes/RenderPassRecordContext.h"
#include "Render/PipelineCache.h"
#include "RHI/RHI.h"

#include <memory>
#include <string>

namespace RVX
{
    class PipelineCache;
    struct RenderPassRecordResults;
    struct ViewData;

    /**
     * @brief Exact graph-owned lighting inputs for one deferred compute dispatch.
     *
     * Every handle belongs to the same immutable LightingFramePlan identity.
     * This lets the RenderGraph own state transitions until command recording,
     * where DeferredLightingPass resolves the physical resources and seals a
     * record-owned PipelineCache lighting binding snapshot.
     */
    struct DeferredLightingFrameResources
    {
        uint64 lightingPlanIdentity = 0;
        // Graph imports and the descriptor snapshot must retain this exact
        // value-owned allocation set.  Holding only the source upload buffers
        // would preserve lifetime while allowing a later frame to overwrite
        // the bytes before this graph records.
        FrozenFrameLightResources frozenLightResources{};
        RGBufferHandle lightConstants{};
        RGBufferHandle pointLights{};
        RGBufferHandle spotLights{};
        RGBufferHandle clusterConstants{};
        RGBufferHandle clusterData{};
        RGBufferHandle clusterLightIndices{};
        bool directionalShadowEnabled = false;
        RGTextureHandle directionalShadowMap{};
        bool localShadowEnabled = false;
        RGTextureHandle localShadowAtlas{};
        uint64 localShadowPlanIdentity = 0;
        uint64 localShadowAtlasPlanIdentity = 0;
        uint64 localShadowAtlasGeneration = 0;
        uint32 localShadowPageCount = 0;
        uint32 localShadowPageResolution = 0;
        uint32 localShadowPageCapacity = 0;
        bool textureIBLEnabled = false;
        RGTextureHandle irradiance{};
        RGTextureHandle prefilteredEnvironment{};
        RGTextureHandle brdfLUT{};
        RHITextureViewRef expectedIrradianceView;
        RHITextureViewRef expectedPrefilteredEnvironmentView;
        RHITextureViewRef expectedBRDFLUTView;
        std::shared_ptr<RenderPassRecordResults> recordResults;

        [[nodiscard]] bool IsComplete() const noexcept
        {
            const bool localAndClusteredLights = lightConstants.IsValid() &&
                pointLights.IsValid() && spotLights.IsValid() &&
                clusterConstants.IsValid() && clusterData.IsValid() &&
                clusterLightIndices.IsValid();
            const bool csmReady = !directionalShadowEnabled ||
                directionalShadowMap.IsValid();
            const bool localShadowReady = localShadowEnabled
                ? localShadowAtlas.IsValid() &&
                    localShadowPlanIdentity != 0 &&
                    localShadowAtlasPlanIdentity != 0 &&
                    localShadowAtlasGeneration != 0 &&
                    localShadowPageCount != 0 &&
                    localShadowPageResolution != 0 &&
                    localShadowPageCapacity != 0 &&
                    localShadowPageCount <= localShadowPageCapacity
                : !localShadowAtlas.IsValid() &&
                    localShadowPlanIdentity == 0 &&
                    localShadowAtlasPlanIdentity == 0 &&
                    localShadowAtlasGeneration == 0 &&
                    localShadowPageCount == 0 &&
                    localShadowPageResolution == 0 &&
                    localShadowPageCapacity == 0;
            const bool iblReady = !textureIBLEnabled ||
                (irradiance.IsValid() && prefilteredEnvironment.IsValid() &&
                 brdfLUT.IsValid() && expectedIrradianceView &&
                 expectedPrefilteredEnvironmentView && expectedBRDFLUTView);
            return lightingPlanIdentity != 0 &&
                   frozenLightResources.IsComplete() &&
                   localAndClusteredLights &&
                   csmReady && localShadowReady && iblReady;
        }
    };

    /** @brief Fixed resource inputs consumed by one deferred-lighting dispatch. */
    struct DeferredLightingInputs
    {
        RGTextureHandle gBuffer0{};
        RGTextureHandle gBuffer1{};
        RGTextureHandle gBuffer2{};
        RGTextureHandle gBuffer3{};
        RGTextureHandle deviceDepth{};
        RGTextureHandle hdrSceneColor{};
        /** Exact pre-exposed indirect-specular term already included in HDR. */
        RGTextureHandle baseIndirectSpecular{};
        /** Dimensionless material/visibility response for routed reflections. */
        RGTextureHandle specularResponse{};
        RGBufferHandle materialTable{};
        PostProcessMaterialTableMetadata materialTableMetadata{};
        /** Same committed prior-presented state consumed by every HDR producer. */
        RGBufferHandle exposureState{};
        /** Exact renderer-owned R8 GTAO visibility field for indirect lighting. */
        AmbientOcclusionRecordInput ambientOcclusion{};
        // This is the immutable plan identity captured by the feature adapter
        // when it imports the graph resources below.  It must agree with the
        // identity carried by those resources; accepting either identity on
        // its own would let a later plan silently shade an earlier frame.
        uint64 expectedLightingPlanIdentity = 0;
        DeferredLightingFrameResources lighting{};

        [[nodiscard]] bool IsComplete() const noexcept
        {
            return gBuffer0.IsValid() && gBuffer1.IsValid() &&
                   gBuffer2.IsValid() && gBuffer3.IsValid() &&
                   deviceDepth.IsValid() && hdrSceneColor.IsValid() &&
                   baseIndirectSpecular.IsValid() &&
                   specularResponse.IsValid() &&
                   materialTable.IsValid() &&
                   materialTableMetadata.IsValid() &&
                   exposureState.IsValid() &&
                   ambientOcclusion.IsCompatibleWith(
                       ambientOcclusion.identity) &&
                   expectedLightingPlanIdentity != 0 &&
                   expectedLightingPlanIdentity == lighting.lightingPlanIdentity &&
                   lighting.IsComplete();
        }
    };

    /**
     * @brief Deferred opaque-lighting compute implementation.
     *
     * This is a feature-owned graph primitive rather than an IRenderPass in
     * the legacy central list.  Feature composition may only enter through
     * RenderFeatureGraphBuilder, which records the pass name and preserves the
     * GFX02 graph-composition boundary.
     */
    class DeferredLightingPass final
    {
    public:
        DeferredLightingPass() = default;
        ~DeferredLightingPass() = default;

        void SetPipelineCache(PipelineCache* pipelineCache);

        /** @brief Lazily create the native compute pipeline and its layouts. */
        [[nodiscard]] bool EnsurePipelineReady();
        [[nodiscard]] bool IsSupported() const;
        [[nodiscard]] const std::string& GetUnsupportedReason() const
        {
            return m_unsupportedReason;
        }

        /**
         * @brief Declare and record the GBuffer-to-HDR compute dispatch.
         * @return false when capability or graph inputs are unavailable.
         */
        [[nodiscard]] bool AddToGraph(
            RenderFeatureGraphBuilder& builder,
            const ViewData& view,
            const DeferredLightingInputs& inputs);

    private:
        PipelineCache* m_pipelineCache = nullptr;
        IRHIDevice* m_device = nullptr;
        mutable std::string m_unsupportedReason{
            "Deferred lighting has not created a native compute pipeline"};
    };
} // namespace RVX
