#pragma once

/**
 * @file ParticleFeaturePass.h
 * @brief Render-owned particle feature pass consuming RenderContracts snapshots.
 */

#include "Render/Passes/IRenderPass.h"
#include "RenderContracts/ParticleRenderSnapshot.h"

#include <cstdint>
#include <memory>
#include <string>

namespace RVX
{
    struct ParticleFeaturePassStats
    {
        bool requested = false;
        bool supported = false;
        bool enabled = false;
        bool graphPassScheduled = false;
        bool drawSubmitted = false;
        bool uploadPublished = false;
        uint32 itemCount = 0;
        uint32 metadataOnlyItemCount = 0;
        uint32 cpuPacketReadyItemCount = 0;
        uint32 sortingSupportedItemCount = 0;
        uint32 totalAliveParticles = 0;
        uint32 drawCallCount = 0;
        uint32 instanceCount = 0;
        uint32 fallbackCount = 0;
        uint32 waitIdleCallCount = 0;
        uint32 frameSlot = RVX_INVALID_INDEX;
        uint64 uploadBytes = 0;
        uint64 resourceCreationCount = 0;
        uint64 pipelineCreationCount = 0;
        std::string unsupportedReason;
    };

    class IShaderProgramProvider;
    class ParticleBillboardRenderer;

    /**
     * @brief Render-owned entry point for particle snapshots.
     *
     * This pass intentionally does not include or link the Particle feature module.
     * Feature data reaches Render exclusively through RenderContracts. CPU packet
     * snapshots are uploaded into renderer-owned frame slots and drawn as sorted
     * billboards; metadata-only or unsupported snapshot variants fail closed.
     */
    class ParticleFeaturePass final : public IRenderPass
    {
    public:
        const char* GetName() const override { return "ParticleFeaturePass"; }
        int32_t GetPriority() const override { return PassPriority::Transparent + 50; }

        ParticleFeaturePass();
        ~ParticleFeaturePass() override;

        void SetSnapshot(const ParticleRenderSnapshot* snapshot);
        /** @brief Borrow the SceneRenderer-owned cooked shader provider. */
        void SetShaderProgramProvider(IShaderProgramProvider* shaderProgramProvider);
        /** @brief Select the depth comparison convention before resource creation. */
        void SetReverseZ(bool reverseZ);
        const ParticleFeaturePassStats& GetStats() const { return m_stats; }
        /** @brief Publish the completed graph record's immutable diagnostic receipt. */
        void PublishExecutionStats(const ParticleFeaturePassStats& stats);

        bool IsRequestedEnabled() const override { return m_stats.requested; }
        bool IsSupported() const override { return !m_stats.requested || m_stats.supported; }
        const std::string& GetUnsupportedReason() const override { return m_stats.unsupportedReason; }
        bool IsEnabled() const override { return m_stats.enabled; }

        void Setup(RenderGraphBuilder& builder, const ViewData& view) override;
        void Execute(RHICommandContext& ctx, const ViewData& view) override;
        void Execute(RenderGraphPassContext& context,
                     const ViewData& view) override;
        void AddToGraph(RenderGraph& graph,
                        const RenderPassRecordContext& context) override;
        void OnAdd(IRHIDevice* device) override;
        void OnRemove() override;

    private:
        void RebuildStats();

        const ParticleRenderSnapshot* m_snapshot = nullptr;
        std::unique_ptr<ParticleBillboardRenderer> m_renderer;
        ParticleFeaturePassStats m_stats;
    };

} // namespace RVX
