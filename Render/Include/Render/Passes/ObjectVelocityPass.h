#pragma once

/**
 * @file ObjectVelocityPass.h
 * @brief Opaque and alpha-masked object motion-vector pass for temporal reprojection
 */

#include "Render/Passes/IRenderPass.h"

#include <memory>

namespace RVX
{
    class RenderResourceRegistry;
    class MaterialSystem;
    class PipelineCache;
    class RenderScene;
    class ResourceViewCache;

    class ObjectVelocityPass : public IRenderPass
    {
    public:
        ObjectVelocityPass() = default;
        ~ObjectVelocityPass() override = default;

        const char* GetName() const override { return "ObjectVelocityPass"; }
        int32_t GetPriority() const override { return PassPriority::PostProcess - 250; }

        void OnAdd(IRHIDevice* device) override;
        void OnRemove() override;
        void Setup(RenderGraphBuilder& builder, const ViewData& view) override;
        void Execute(RHICommandContext& ctx, const ViewData& view) override;
        void AddToGraph(RenderGraph& graph, const ViewData& view) override;
        void AddToGraph(RenderGraph& graph,
                        const RenderPassRecordContext& context) override;

        void SetResources(PipelineCache* pipelineCache,
                          ResourceViewCache* viewCache,
                          MaterialSystem* materialSystem);
        void SetResourceRegistry(const RenderResourceRegistry* registry)
        {
            m_resourceRegistry = registry;
        }
        void SetEnabled(bool enabled) { m_enabled = enabled; }
        bool IsRequestedEnabled() const override { return m_enabled; }
        bool IsSupported() const override;
        const std::string& GetUnsupportedReason() const override { return m_unsupportedReason; }
        bool IsEnabled() const override { return IsRequestedEnabled() && IsSupported(); }
        const ObjectVelocityPassStats& GetStats() const
        {
            return m_lastPublishedStats;
        }
        /** Publish only the latest completed recording's diagnostics. */
        void PublishRecordResults(
            const std::shared_ptr<RenderPassRecordResults>& results,
            const RenderPassRecordIdentity& expectedIdentity);

    private:
        IRHIDevice* m_device = nullptr;
        const RenderResourceRegistry* m_resourceRegistry = nullptr;
        PipelineCache* m_pipelineCache = nullptr;
        ResourceViewCache* m_viewCache = nullptr;
        MaterialSystem* m_materialSystem = nullptr;
        bool m_enabled = false;
        mutable std::string m_unsupportedReason = "Object velocity pass has not been requested";
        RenderPassRecordIdentity m_lastPublishedIdentity{};
        ObjectVelocityPassStats m_lastPublishedStats{};
    };

} // namespace RVX
