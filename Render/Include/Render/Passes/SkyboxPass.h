#pragma once

/**
 * @file SkyboxPass.h
 * @brief Skybox render pass for environment rendering
 * 
 * SkyboxPass renders environment skybox using cubemap textures
 * or procedural sky.
 */

#include "Render/Passes/IRenderPass.h"
#include <string>

namespace RVX
{
    class PipelineCache;
    class RenderResourceRegistry;

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
        void AddToGraph(RenderGraph& graph, const ViewData& view) override;
        void AddToGraph(RenderGraph& graph,
                        const RenderPassRecordContext& context) override;

        // =========================================================================
        // Configuration
        // =========================================================================

        /**
         * @brief Set resources needed for rendering
         */
        void SetResources(PipelineCache* pipelineCache);
        void SetResourceRegistry(const RenderResourceRegistry* registry)
        {
            m_resourceRegistry = registry;
        }

        /**
         * @brief Enable or disable this pass
         */
        void SetEnabled(bool enabled) { m_enabled = enabled; }

        bool IsRequestedEnabled() const override { return m_enabled; }
        // Support is intentionally frame-independent.  A selected sky is a
        // value in RenderPassFrameSnapshot, not mutable pass configuration.
        bool IsSupported() const override { return m_drawReady; }
        bool IsEnabled() const override { return IsRequestedEnabled() && IsSupported(); }
        bool IsDrawReady() const { return m_drawReady; }
        const std::string& GetUnsupportedReason() const override { return m_unsupportedReason; }

    private:
        void RefreshSupport();
        bool EnsureRuntimeResources();

        bool m_enabled = true;
        bool m_drawReady = false;
        std::string m_unsupportedReason = "Skybox pipeline resources are not available";
        const RenderResourceRegistry* m_resourceRegistry = nullptr;
        PipelineCache* m_pipelineCache = nullptr;
        IRHIDevice* m_resourceDevice = nullptr;
        RHITextureRef m_fallbackCubemap;
        RHITextureViewRef m_fallbackCubemapView;
        RHISamplerRef m_sampler;
    };

} // namespace RVX
