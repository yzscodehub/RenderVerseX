#pragma once

/**
 * @file ShadowPass.h
 * @brief Shadow map generation pass with CSM support
 * 
 * ShadowPass renders scene geometry to shadow maps for directional,
 * point, and spot lights. Supports Cascaded Shadow Maps (CSM) for
 * directional lights.
 */

#include "Render/Passes/IRenderPass.h"
#include "Core/MathTypes.h"
#include "Render/Renderer/ShadowConstants.h"

#include <vector>

namespace RVX
{
    class RenderScene;
    class GPUResourceManager;
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
    };

    struct ShadowPassStats
    {
        uint32_t configuredCascadeCount = 0;
        uint32_t declaredCascadeResourceCount = 0;
        uint32_t resolvedCascadeViewCount = 0;
        uint32_t shadowCasterCount = 0;
        uint32_t drawCount = 0;
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

        // =========================================================================
        // Configuration
        // =========================================================================

        void SetResources(GPUResourceManager* gpuResources, PipelineCache* pipelineCache);
        void SetRenderScene(const RenderScene* scene);
        void SetConfig(const ShadowPassConfig& config);

        /**
         * @brief Set directional light for shadow mapping
         */
        void SetDirectionalLight(const Vec3& direction, const Vec3& color, float intensity);

        /**
         * @brief Calculate CSM cascades from view data
         */
        void CalculateCascades(const ViewData& view);

        /**
         * @brief Get cascade info for shader binding
         */
        const std::vector<ShadowCascade>& GetCascades() const { return m_cascades; }
        const ShadowPassConfig& GetConfig() const { return m_config; }
        const Vec3& GetLightDirection() const { return m_lightDirection; }
        float GetLightIntensity() const { return m_lightIntensity; }

        /**
         * @brief Get the shadow map texture (after execution)
         */
        RHITexture* GetShadowMap() const { return m_shadowMapTexture; }
        RGTextureHandle GetShadowMapTextureHandle() const { return m_shadowMapTextureHandle; }
        const std::vector<RGTextureHandle>& GetCascadeTextureHandles() const { return m_cascadeTextureHandles; }
        const ShadowPassStats& GetStats() const { return m_stats; }

        void SetEnabled(bool enabled) { m_enabled = enabled; }
        bool IsRequestedEnabled() const override { return m_enabled; }
        bool IsSupported() const override;
        const std::string& GetUnsupportedReason() const override { return m_unsupportedReason; }
        bool IsEnabled() const override { return IsRequestedEnabled() && IsSupported(); }

    private:
        bool ResolveCascadeViews(const ViewData& view);
        void RenderCascade(RHICommandContext& ctx, const ViewData& view, uint32_t cascadeIndex);

        bool m_enabled = false;  // Disabled by default until light is configured
        mutable std::string m_unsupportedReason = "ShadowPass has not been configured";
        GPUResourceManager* m_gpuResources = nullptr;
        PipelineCache* m_pipelineCache = nullptr;
        const RenderScene* m_renderScene = nullptr;

        ShadowPassConfig m_config;
        Vec3 m_lightDirection{0.0f, -1.0f, 0.0f};
        Vec3 m_lightColor{1.0f, 1.0f, 1.0f};
        float m_lightIntensity = 1.0f;

        std::vector<ShadowCascade> m_cascades;

        // Shadow map resources
        RGTextureHandle m_shadowMapTextureHandle;
        RHITexture* m_shadowMapTexture = nullptr;
        std::vector<RGTextureHandle> m_cascadeTextureHandles;
        std::vector<RHITextureView*> m_cascadeViews;
        ShadowPassStats m_stats;
    };

} // namespace RVX
