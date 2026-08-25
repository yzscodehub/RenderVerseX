#pragma once

/**
 * @file CameraVelocityPass.h
 * @brief Camera/depth motion-vector pass for temporal reprojection
 */

#include "Render/Passes/IRenderPass.h"

#include <memory>


namespace RVX
{
    class PipelineCache;
    class ResourceViewCache;

    class CameraVelocityPass : public IRenderPass
    {
    public:
        CameraVelocityPass() = default;
        ~CameraVelocityPass() override = default;

        const char* GetName() const override { return "CameraVelocityPass"; }
        int32_t GetPriority() const override { return PassPriority::PostProcess - 300; }

        void OnAdd(IRHIDevice* device) override;
        void OnRemove() override;
        void Setup(RenderGraphBuilder& builder, const ViewData& view) override;
        void Execute(RHICommandContext& ctx, const ViewData& view) override;
        void Execute(RenderGraphPassContext& context,
                     const ViewData& view) override;
        void AddToGraph(RenderGraph& graph, const ViewData& view) override;
        void AddToGraph(RenderGraph& graph,
                        const RenderPassRecordContext& context) override;

        void SetResources(PipelineCache* pipelineCache, ResourceViewCache* viewCache);
        void SetEnabled(bool enabled) { m_enabled = enabled; }
        bool IsRequestedEnabled() const override { return m_enabled; }
        bool IsSupported() const override;
        const std::string& GetUnsupportedReason() const override { return m_unsupportedReason; }
        bool IsEnabled() const override { return IsRequestedEnabled() && IsSupported(); }
        /** Compatibility diagnostic snapshot of the newest completed record. */
        const CameraVelocityPassStats& GetStats() const
        {
            return m_lastPublishedStats;
        }
        void PublishRecordResults(
            const std::shared_ptr<RenderPassRecordResults>& results,
            const RenderPassRecordIdentity& expectedIdentity);

    private:
        IRHIDevice* m_device = nullptr;
        PipelineCache* m_pipelineCache = nullptr;
        ResourceViewCache* m_viewCache = nullptr;
        bool m_enabled = false;
        mutable std::string m_unsupportedReason = "Camera velocity pass has not been requested";

        RenderPassRecordIdentity m_lastPublishedIdentity{};
        CameraVelocityPassStats m_lastPublishedStats{};
    };

} // namespace RVX
