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
        void SetCubemap(RHITexture* cubemap);

        /**
         * @brief Set procedural sky parameters
         */
        void SetProceduralSkyParams(const Vec3& sunDirection, const Vec3& skyColor, const Vec3& horizonColor);

        /**
         * @brief Enable or disable this pass
         */
        void SetEnabled(bool enabled) { m_enabled = enabled; }

        bool IsEnabled() const override { return m_enabled; }

    private:
        bool m_enabled = true;
        PipelineCache* m_pipelineCache = nullptr;
        RHITextureView* m_colorTargetView = nullptr;
        RHITextureView* m_depthTargetView = nullptr;
        RHITexture* m_cubemap = nullptr;

        // Procedural sky parameters
        Vec3 m_sunDirection{0.5f, 0.5f, 0.5f};
        Vec3 m_skyColor{0.4f, 0.6f, 1.0f};
        Vec3 m_horizonColor{0.8f, 0.85f, 0.9f};
        bool m_useProceduralSky = true;

        // RenderGraph handles
        RGTextureHandle m_colorTargetHandle;
        RGTextureHandle m_depthTargetHandle;
    };

} // namespace RVX
