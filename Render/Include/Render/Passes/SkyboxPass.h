#pragma once

/**
 * @file SkyboxPass.h
 * @brief Skybox render pass for environment rendering
 * 
 * SkyboxPass renders environment skybox using cubemap textures
 * or procedural sky.
 */

#include "Render/Passes/IRenderPass.h"
#include "Core/MathTypes.h"
#include <deque>
#include <string>

namespace RVX
{
    class PipelineCache;

    /**
     * @brief Skybox render pass
     * 
     * Renders environment background after opaque geometry.
     * Supports cubemap-based skybox or procedural sky.
     * 
     * Key characteristics:
     * - Runs after opaque pass (priority 400)
     * - Uses reverse depth (draw at far plane)
     * - Depth test enabled, depth write disabled
     */
    class SkyboxPass : public IRenderPass
    {
    public:
        SkyboxPass();
        ~SkyboxPass() override = default;

        // =========================================================================
        // IRenderPass Interface
        // =========================================================================

        const char* GetName() const override { return "SkyboxPass"; }
        
        int32_t GetPriority() const override { return 400; }  // After opaque (300)
        
        RenderGraphPassType GetPassType() const override { return RenderGraphPassType::Graphics; }

        void Setup(RenderGraphBuilder& builder, const ViewData& view) override;
        void Execute(RHICommandContext& ctx, const ViewData& view) override;

        // =========================================================================
        // Configuration
        // =========================================================================

        /**
         * @brief Set resources needed for rendering
         */
        void SetResources(PipelineCache* pipelineCache);

        /**
         * @brief Set render targets
         */
        void SetRenderTargets(RHITextureView* colorTargetView, RHITextureView* depthTargetView);

        /**
         * @brief Set the skybox cubemap texture
         * @param cubemap Cubemap texture for skybox (nullptr for procedural sky)
         */
        void SetCubemap(RHITexture* cubemap,
                        float exposure = 1.0f,
                        float rotation = 0.0f,
                        float blurLevel = 0.0f);

        /**
         * @brief Set procedural sky parameters
         */
        void SetProceduralSkyParams(const Vec3& sunDirection,
                                    const Vec3& skyColor,
                                    const Vec3& horizonColor,
                                    const Vec3& groundColor = Vec3{0.3f, 0.25f, 0.2f},
                                    const Vec3& sunColor = Vec3{1.0f, 0.95f, 0.9f},
                                    float exposure = 1.0f,
                                    float scatteringIntensity = 1.0f);

        /**
         * @brief Set a solid-color background through the procedural shader path
         */
        void SetSolidColor(const Vec3& color, float exposure = 1.0f);

        /**
         * @brief Clear the selected skybox for this frame
         */
        void ClearSkybox(const char* reason = "No supported SkyboxComponent selected");

        /**
         * @brief Enable or disable this pass
         */
        void SetEnabled(bool enabled) { m_enabled = enabled; }

        bool IsRequestedEnabled() const override { return m_enabled; }
        bool IsSupported() const override { return m_drawReady; }
        bool IsEnabled() const override { return IsRequestedEnabled() && IsSupported(); }
        bool IsDrawReady() const { return m_drawReady; }
        bool IsCubemapSelected() const { return m_drawMode == SkyboxDrawMode::Cubemap && m_cubemap != nullptr; }
        RHITexture* GetSelectedCubemap() const { return m_cubemap; }
        const std::string& GetUnsupportedReason() const override { return m_unsupportedReason; }

    private:
        enum class SkyboxDrawMode : uint8
        {
            None = 0,
            Procedural,
            Cubemap
        };

        void RefreshSupport();
        bool EnsureRuntimeResources();
        RHITextureView* ResolveCubemapView(const ViewData& view);
        bool UpdateConstants(const ViewData& view);

        bool m_enabled = true;
        bool m_drawReady = false;
        bool m_skySelected = false;
        std::string m_unsupportedReason = "No supported SkyboxComponent selected";
        PipelineCache* m_pipelineCache = nullptr;
        IRHIDevice* m_resourceDevice = nullptr;
        RHITextureView* m_colorTargetView = nullptr;
        RHITextureView* m_depthTargetView = nullptr;
        RHITexture* m_cubemap = nullptr;
        RHIBufferRef m_constantBuffer;
        RHITextureRef m_fallbackCubemap;
        RHITextureViewRef m_fallbackCubemapView;
        RHISamplerRef m_sampler;
        std::deque<RHIDescriptorSetRef> m_retainedDescriptorSets;
        std::deque<RHITextureViewRef> m_retainedCubemapViews;

        // Procedural sky parameters
        Vec3 m_sunDirection{0.5f, 0.5f, 0.5f};
        Vec3 m_skyColor{0.4f, 0.6f, 1.0f};
        Vec3 m_horizonColor{0.8f, 0.85f, 0.9f};
        Vec3 m_groundColor{0.3f, 0.25f, 0.2f};
        Vec3 m_sunColor{1.0f, 0.95f, 0.9f};
        float m_exposure = 1.0f;
        float m_scatteringIntensity = 1.0f;
        float m_rotation = 0.0f;
        float m_blurLevel = 0.0f;
        SkyboxDrawMode m_drawMode = SkyboxDrawMode::None;

        // RenderGraph handles
        RGTextureHandle m_colorTargetHandle;
        RGTextureHandle m_depthTargetHandle;
    };

} // namespace RVX
