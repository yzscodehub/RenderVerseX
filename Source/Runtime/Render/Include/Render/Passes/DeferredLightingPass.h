#pragma once

/**
 * @file DeferredLightingPass.h
 * @brief Compute lighting for the canonical deferred opaque GBuffer.
 */

#include "Render/Features/RenderFeatureGraphBuilder.h"
#include "Render/PipelineCache.h"
#include "RHI/RHI.h"

#include <memory>
#include <string>

namespace RVX
{
    class PipelineCache;
    struct RenderPassRecordResults;
    struct ViewData;

    /** @brief Canonical GFX07 deferred material-table shading model values. */
    enum class DeferredMaterialShadingModel : uint32
    {
        Invalid = 0,
        StandardLit = 1,
    };

    /**
     * @brief One shader-visible material classification row.
     *
     * GFX07 deliberately has one canonical StandardLit row.  The table ABI is
     * independent from material parameter slots: GBuffer3 always addresses this
     * row until a later renderer feature supplies a real multi-material table.
     */
    struct DeferredMaterialClassification
    {
        static constexpr uint32 SchemaVersion = 1;

        uint32 shadingModel = static_cast<uint32>(
            DeferredMaterialShadingModel::StandardLit);
        uint32 schemaVersion = SchemaVersion;
        uint32 reserved0 = 0;
        uint32 reserved1 = 0;
    };

    static_assert(sizeof(DeferredMaterialClassification) == 16);

    /**
     * @brief Create and publish the only material-classification table GFX07 accepts.
     *
     * The result is a one-row Upload structured SRV.  Callers import this exact
     * buffer into the frame graph and pass the resulting handle through
     * DeferredLightingInputs::materialTable.  The lighting pass validates the
     * fixed one-row buffer ABI before it records a dispatch.
     */
    [[nodiscard]] bool CreateDeferredStandardLitMaterialTable(
        IRHIDevice& device,
        RHIBufferRef& outTable,
        std::string& outError);

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
            const bool iblReady = !textureIBLEnabled ||
                (irradiance.IsValid() && prefilteredEnvironment.IsValid() &&
                 brdfLUT.IsValid() && expectedIrradianceView &&
                 expectedPrefilteredEnvironmentView && expectedBRDFLUTView);
            return lightingPlanIdentity != 0 &&
                   frozenLightResources.IsComplete() &&
                   localAndClusteredLights &&
                   csmReady && iblReady;
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
        RGBufferHandle materialTable{};
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
                   materialTable.IsValid() &&
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
