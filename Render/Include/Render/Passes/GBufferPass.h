#pragma once

/**
 * @file GBufferPass.h
 * @brief GBuffer render pass for deferred rendering
 */

#include "Render/Passes/IRenderPass.h"
#include "Render/Graph/RenderGraph.h"
#include <vector>

namespace RVX
{
    // Forward declarations
    class GPUResourceManager;
    class PipelineCache;
    class RenderScene;

    // =============================================================================
    // GBuffer Pass Data (used in RenderGraph)
    // =============================================================================
    struct GBufferPassData
    {
        // Output render targets
        RGTextureHandle albedoRT;       // RGBA8: Albedo(RGB) + AO(A)
        RGTextureHandle normalRT;       // RGBA16F: Normal(XYZ) + Metallic(A)
        RGTextureHandle roughnessRT;    // RGBA16F: Roughness + padding
        RGTextureHandle velocityRT;      // RG16F: Velocity(XY) (optional, for TAA)

        // Input depth
        RGTextureHandle depthTexture;

        // Pass reference for execution
        class GBufferPass* pass = nullptr;
    };

    /**
     * @brief GBuffer render pass for deferred rendering
     *
     * Renders geometry to multiple render targets (GBuffer):
     * - Albedo + AO
     * - Normal + Metallic
     * - Roughness
     * - Velocity (for TAA)
     *
     * This enables efficient deferred lighting and decal application.
     */
    class GBufferPass : public IRenderPass
    {
    public:
        GBufferPass() = default;
        ~GBufferPass() override = default;

        const char* GetName() const override { return "GBufferPass"; }
        int32_t GetPriority() const override { return PassPriority::Opaque - 50; }  // Before OpaquePass

        void OnAdd(IRHIDevice* device) override;
        void OnRemove() override;

        void Setup(RenderGraphBuilder& builder, const ViewData& view) override;
        void Execute(RHICommandContext& ctx, const ViewData& view) override;

        // =====================================================================
        // Resource Dependencies
        // =====================================================================

        /**
         * @brief Set resource dependencies before rendering
         * @param gpuMgr GPU resource manager for mesh buffers
         * @param pipelines Pipeline cache for shaders and pipelines
         */
        void SetResources(GPUResourceManager* gpuMgr, PipelineCache* pipelines);

        /**
         * @brief Set render scene data for this frame
         * @param scene The render scene containing objects
         * @param visibleIndices Indices of visible objects (after culling)
         */
        void SetRenderScene(const RenderScene* scene, const std::vector<uint32_t>* visibleIndices);

        /**
         * @brief Set output render targets
         */
        void SetRenderTargets(
            RHITextureView* albedoView,
            RHITextureView* normalView,
            RHITextureView* roughnessView,
            RHITextureView* depthView);

        // =====================================================================
        // Configuration
        // =====================================================================

        /**
         * @brief Enable/disable velocity output for TAA
         */
        void SetVelocityEnabled(bool enabled) { m_velocityEnabled = enabled; }

    private:
        // Render targets
        RGTextureHandle m_albedoHandle;
        RGTextureHandle m_normalHandle;
        RGTextureHandle m_roughnessHandle;
        RGTextureHandle m_velocityHandle;
        RGTextureHandle m_depthHandle;

        // Resource dependencies
        GPUResourceManager* m_gpuResources = nullptr;
        PipelineCache* m_pipelineCache = nullptr;
        const RenderScene* m_renderScene = nullptr;
        const std::vector<uint32_t>* m_visibleIndices = nullptr;

        // Render target views
        RHITextureView* m_albedoView = nullptr;
        RHITextureView* m_normalView = nullptr;
        RHITextureView* m_roughnessView = nullptr;
        RHITextureView* m_depthView = nullptr;

        // Configuration
        bool m_velocityEnabled = false;

        // Device reference
        IRHIDevice* m_device = nullptr;
    };

} // namespace RVX
