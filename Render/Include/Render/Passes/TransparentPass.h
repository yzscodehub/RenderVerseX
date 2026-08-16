#pragma once

/**
 * @file TransparentPass.h
 * @brief Transparent geometry render pass with alpha blending
 * 
 * TransparentPass renders transparent/alpha-blended geometry after opaque passes,
 * using back-to-front sorting for correct blending order.
 */

#include "Render/Passes/IRenderPass.h"
#include "Render/Renderer/RenderDrawItem.h"

namespace RVX
{
    class RenderResourceRegistry;
    class ClusteredLighting;
    class LightManager;
    class MaterialSystem;
    class PipelineCache;
    class RenderScene;

    /**
     * @brief Transparent geometry render pass
     * 
     * Renders objects with alpha blending in back-to-front order.
     * 
     * Key characteristics:
     * - Runs after opaque pass (priority 500)
     * - Reads depth buffer (no depth write)
     * - Uses alpha blending
     * - Objects sorted by camera distance (back-to-front)
     */
    class TransparentPass : public IRenderPass
    {
    public:
        TransparentPass();
        ~TransparentPass() override = default;

        // =========================================================================
        // IRenderPass Interface
        // =========================================================================

        const char* GetName() const override { return "TransparentPass"; }
        
        int32_t GetPriority() const override { return 500; }  // After opaque (300), sky (400)
        
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
        void SetResources(PipelineCache* pipelineCache,
                          MaterialSystem* materialSystem,
                          LightManager* lightManager = nullptr,
                          ClusteredLighting* clusteredLighting = nullptr);
        void SetResourceRegistry(const RenderResourceRegistry* registry)
        {
            m_resourceRegistry = registry;
        }

        /**
         * @brief Enable or disable this pass
         */
        void SetEnabled(bool enabled) { m_enabled = enabled; }

        bool IsRequestedEnabled() const override { return m_enabled; }
        bool IsEnabled() const override { return IsRequestedEnabled() && IsSupported(); }

        [[nodiscard]] const TransparentPassDrawStats& GetDrawStats() const
        {
            return m_publishedRecordResults != nullptr
                ? m_publishedRecordResults->transparentStats
                : m_drawStats;
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
        bool m_enabled = true;
        const RenderResourceRegistry* m_resourceRegistry = nullptr;
        PipelineCache* m_pipelineCache = nullptr;
        MaterialSystem* m_materialSystem = nullptr;
        LightManager* m_lightManager = nullptr;
        ClusteredLighting* m_clusteredLighting = nullptr;
        TransparentPassDrawStats m_drawStats{};
        std::shared_ptr<RenderPassRecordResults> m_publishedRecordResults;
    };

} // namespace RVX
