#pragma once

/**
 * @file ShadowPass.h
 * @brief Shadow map generation pass with CSM support
 * 
 * ShadowPass renders scene geometry to shadow maps for directional,
 * point, and spot lights. Supports Cascaded Shadow Maps (CSM) for
 * directional lights.
 */

#include "Core/MathTypes.h"
#include "Render/Passes/IRenderPass.h"
#include "Render/Renderer/ShadowConstants.h"

#include <memory>
#include <string>
#include <vector>

namespace RVX
{
    class RenderResourceRegistry;
    class RenderScene;
    class PipelineCache;

    /**
     * @brief Cascade info for CSM
     */
    struct ShadowCascade
    {
        Mat4 viewProjection;
        float splitDepth = 0.0f;
        Vec2 lightSpaceCenter{0.0f, 0.0f};
        float stableExtent = 0.0f;
        float texelWorldSize = 0.0f;
    };

    /**
     * @brief Shadow pass configuration
     */
    struct ShadowPassConfig
    {
        uint32_t shadowMapSize = 2048;       // Shadow map resolution
        uint32_t numCascades = 4;            // Number of CSM cascades
        float cascadeSplitLambda = 0.95f;    // PSSM split scheme parameter
        float shadowBias = 0.005f;           // Depth bias to reduce shadow acne
        float normalBias = 0.02f;            // Normal offset bias
        float filterRadiusTexels = 1.0f;     // PCF radius in shadow-map texels
        float casterDepthBias = 0.0f;         // Raster depth bias when writing shadow maps
        float casterSlopeScaledDepthBias = 0.0f; // Slope-scaled raster bias for shadow casters
        float casterDepthBiasClamp = 0.0f;    // Reserved for future clamp capability; sanitized to 0 for now
        bool stabilizeCascades = true;        // Snap cascades to shadow texels
        float cascadeBlendRatio = 0.05f;      // Fraction of cascade span used for transition fade
        bool rayTracedTemporalAccumulation = true; // Enable RT shadow history blending when the view is stable
        float rayTracedLightAngularRadius = 0.00465f; // Directional-light angular radius in radians (~sun disk)
        uint32_t rayTracedSamplesPerPixel = 1; // Per-pixel RT shadow rays, clamped to [1, 8]
        float rayTracedTemporalBlendFactor = 0.75f; // Weight of previous RT shadow mask in [0, 0.95]
        float rayTracedHistoryDepthThreshold = 0.01f; // Clip-space depth delta allowed for RT history reprojection
        float rayTracedHistoryNormalThreshold = 0.85f; // Minimum normal dot product allowed for RT history reprojection
        float rayTracedHistoryVelocityRejectionScale = 8.0f; // Reduces RT shadow history weight in moving regions
        uint32_t rayTracedInstanceMask = 0xFF; // Instance visibility mask used by RT shadow rays
        RayTracedShadowMode rayTracedShadowMode = RayTracedShadowMode::ComplementRaster; // RT mask composition strategy
    };

    /**
     * @brief Shadow map generation pass
     * 
     * Generates shadow maps for scene lights.
     * 
     * Key characteristics:
     * - Runs before opaque pass (priority 200)
     * - Renders depth-only to shadow maps
     * - Supports CSM for directional lights
     */
    class ShadowPass : public IRenderPass
    {
    public:
        ShadowPass();
        ~ShadowPass() override = default;

        // =========================================================================
        // IRenderPass Interface
        // =========================================================================

        const char* GetName() const override { return "ShadowPass"; }
        
        int32_t GetPriority() const override { return 200; }  // Before opaque
        
        RenderGraphPassType GetPassType() const override { return RenderGraphPassType::Graphics; }

        void Setup(RenderGraphBuilder& builder, const ViewData& view) override;
        void Execute(RHICommandContext& ctx, const ViewData& view) override;
        void AddToGraph(RenderGraph& graph, const ViewData& view) override;
        void AddToGraph(RenderGraph& graph,
                        const RenderPassRecordContext& context) override;

        // =========================================================================
        // Configuration
        // =========================================================================

        void SetResources(PipelineCache* pipelineCache);
        void SetResourceRegistry(const RenderResourceRegistry* registry)
        {
            m_resourceRegistry = registry;
        }
        /** @brief Compatibility-only legacy scene input. Typed recording snapshots it. */
        void SetRenderScene(const RenderScene* scene);
        void SetConfig(const ShadowPassConfig& config);

        /**
         * @brief Calculate CSM cascades from view data
         */
        void CalculateCascades(const ViewData& view,
                               const PrimaryDirectionalLightRecordInput& primaryLight);

        /**
         * @brief Get cascade info for shader binding
         */
        const std::vector<ShadowCascade>& GetCascades() const { return m_cascades; }
        const ShadowPassConfig& GetConfig() const { return m_config; }

        /**
         * @brief Get the shadow map texture (after execution)
         */
        RHITexture* GetShadowMap() const { return m_shadowMapTexture; }
        RGTextureHandle GetShadowMapTextureHandle() const { return m_shadowMapTextureHandle; }
        const std::vector<RGTextureHandle>& GetCascadeTextureHandles() const { return m_cascadeTextureHandles; }
        const ShadowPassStats& GetStats() const
        {
            return m_publishedRecordResults
                ? m_publishedRecordResults->shadowStats : m_stats;
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

        void SetEnabled(bool enabled) { m_enabled = enabled; }
        bool IsRequestedEnabled() const override { return m_enabled; }
        bool IsSupported() const override;
        const std::string& GetUnsupportedReason() const override { return m_unsupportedReason; }
        bool IsEnabled() const override { return IsRequestedEnabled() && IsSupported(); }

    private:
        bool ResolveCascadeViews(const ViewData& view);
        void Setup(RenderGraphBuilder& builder,
                   const ViewData& view,
                   const PrimaryDirectionalLightRecordInput& primaryLight);
        void Execute(RHICommandContext& ctx,
                     const ViewData& view,
                     const PrimaryDirectionalLightRecordInput& primaryLight);
        void RenderCascade(RHICommandContext& ctx,
                           const ViewData& view,
                           uint32_t cascadeIndex,
                           const PrimaryDirectionalLightRecordInput& primaryLight);

        bool m_enabled = false;
        mutable std::string m_unsupportedReason = "ShadowPass has not been configured";
        const RenderResourceRegistry* m_resourceRegistry = nullptr;
        PipelineCache* m_pipelineCache = nullptr;
        const RenderScene* m_renderScene = nullptr;

        ShadowPassConfig m_config;

        std::vector<ShadowCascade> m_cascades;

        // Shadow map resources
        RGTextureHandle m_shadowMapTextureHandle;
        RHITexture* m_shadowMapTexture = nullptr;
        std::vector<RGTextureHandle> m_cascadeTextureHandles;
        std::vector<RHITextureView*> m_cascadeViews;
        ShadowPassStats m_stats;
        std::shared_ptr<RenderPassRecordResults> m_publishedRecordResults;
    };

} // namespace RVX
