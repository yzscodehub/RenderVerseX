#pragma once

/**
 * @file ParticleFeaturePass.h
 * @brief Render-owned particle feature pass consuming RenderContracts snapshots.
 */

#include "Render/Passes/IRenderPass.h"
#include "RenderContracts/ParticleRenderSnapshot.h"

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
        uint32 itemCount = 0;
        uint32 metadataOnlyItemCount = 0;
        uint32 renderPayloadReadyItemCount = 0;
        uint32 sortingSupportedItemCount = 0;
        uint32 totalAliveParticles = 0;
        std::string unsupportedReason;
    };

    /**
     * @brief Render-owned entry point for particle snapshots.
     *
     * This pass intentionally does not include or link the Particle feature module.
     * Feature data reaches Render through RenderContracts. Until the draw payload
     * extraction is connected, requested snapshots report explicit unsupported
     * diagnostics instead of pretending that metadata-only particles rendered.
     */
    class ParticleFeaturePass final : public IRenderPass
    {
    public:
        const char* GetName() const override { return "ParticleFeaturePass"; }
        int32_t GetPriority() const override { return PassPriority::Transparent + 50; }

        void SetSnapshot(const ParticleRenderSnapshot* snapshot);
        const ParticleFeaturePassStats& GetStats() const { return m_stats; }

        bool IsRequestedEnabled() const override { return m_stats.requested; }
        bool IsSupported() const override { return !m_stats.requested || m_stats.supported; }
        const std::string& GetUnsupportedReason() const override { return m_stats.unsupportedReason; }
        bool IsEnabled() const override { return m_stats.requested && m_stats.supported; }

        void Setup(RenderGraphBuilder& builder, const ViewData& view) override;
        void Execute(RHICommandContext& ctx, const ViewData& view) override;

    private:
        void RebuildStats();

        const ParticleRenderSnapshot* m_snapshot = nullptr;
        ParticleFeaturePassStats m_stats;
    };

} // namespace RVX
