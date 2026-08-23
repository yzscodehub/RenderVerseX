#include "Render/Passes/ParticleFeaturePass.h"

#include "Core/Log.h"

#include <utility>

namespace RVX
{
    void ParticleFeaturePass::SetSnapshot(const ParticleRenderSnapshot* snapshot)
    {
        m_snapshot = snapshot;
        RebuildStats();
    }

    void ParticleFeaturePass::Setup(RenderGraphBuilder& builder, const ViewData& view)
    {
        (void)builder;
        (void)view;
        RebuildStats();
        m_stats.graphPassScheduled = IsEnabled();
    }

    void ParticleFeaturePass::Execute(RHICommandContext& ctx, const ViewData& view)
    {
        (void)ctx;
        (void)view;
        RebuildStats();

        if (!m_stats.requested)
            return;

        if (!m_stats.supported)
        {
            RVX_CORE_WARN("ParticleFeaturePass: execute skipped: {}", m_stats.unsupportedReason);
            return;
        }

        m_stats.drawSubmitted = false;
    }

    void ParticleFeaturePass::Execute(RenderGraphPassContext& context,
                                      const ViewData& view)
    {
        (void)view;
        RebuildStats();

        if (!m_stats.requested)
        {
            // No extracted particle payload is an explicit no-work state.
            return;
        }

        // The feature is currently correctly reported as unsupported and is
        // therefore normally omitted by the legacy adapter. Keep the graph
        // override fail-closed in case a caller registers it directly or a
        // future support predicate changes before a draw implementation lands.
        m_stats.drawSubmitted = false;
        context.MarkExecutionFailed(
            m_stats.supported
                ? "ParticleFeaturePass has no renderer-owned draw implementation"
                : "ParticleFeaturePass was executed despite unsupported particle payloads");
    }

    void ParticleFeaturePass::RebuildStats()
    {
        ParticleFeaturePassStats stats;

        if (!m_snapshot || m_snapshot->items.empty())
        {
            m_stats = std::move(stats);
            return;
        }

        stats.requested = true;
        stats.itemCount = static_cast<uint32>(m_snapshot->items.size());
        stats.totalAliveParticles = m_snapshot->metadata.totalAliveParticles;

        for (const ParticleRenderSnapshotItem& item : m_snapshot->items)
        {
            if (item.payloadStatus == ParticleRenderSnapshotPayloadStatus::MetadataOnly)
            {
                ++stats.metadataOnlyItemCount;
            }
            if (item.renderPayloadAvailable ||
                item.payloadStatus == ParticleRenderSnapshotPayloadStatus::RenderOwnedPayloadReady)
            {
                ++stats.renderPayloadReadyItemCount;
            }
            if (item.sortingSupported)
            {
                ++stats.sortingSupportedItemCount;
            }
        }

        stats.supported = false;
        stats.enabled = false;
        if (stats.metadataOnlyItemCount > 0)
        {
            stats.unsupportedReason =
                "Particle snapshots contain metadata-only items; Render-owned particle draw payload extraction is not connected";
        }
        else if (stats.renderPayloadReadyItemCount > 0)
        {
            stats.unsupportedReason =
                "Render-owned particle draw implementation is not connected";
        }
        else
        {
            stats.unsupportedReason =
                "Particle snapshots do not contain renderable payloads";
        }

        m_stats = std::move(stats);
    }

} // namespace RVX
