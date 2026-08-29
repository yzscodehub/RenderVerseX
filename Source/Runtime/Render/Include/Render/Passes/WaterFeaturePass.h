#pragma once

/**
 * @file WaterFeaturePass.h
 * @brief Render-owned deterministic Gerstner water pass.
 */

#include "Render/Passes/IRenderPass.h"
#include "RenderContracts/WaterRenderSnapshot.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace RVX
{
    /** Value-only receipt for one immutable water surface consumed by a frame. */
    struct WaterFeatureSurfaceReceipt
    {
        uint64 componentId = 0;
        uint64 contentHash = 0;
        uint64 waveContentHash = 0;
        uint32 waveCount = 0;
    };

    /**
     * Execution-grounded diagnostics for the render-owned water feature.
     * Lifetime creation counters deliberately do not reset per frame; they
     * expose accidental resource or pipeline growth after warm-up.
     */
    struct WaterFeaturePassStats
    {
        bool requested = false;
        bool supported = false;
        bool enabled = false;
        bool graphPassScheduled = false;
        bool drawSubmitted = false;
        bool uploadPublished = false;
        bool opaqueSceneSnapshotReady = false;
        bool depthSnapshotReady = false;
        bool depthLinearizationReady = false;
        bool layeredNormalsEvaluated = false;
        bool fresnelEvaluated = false;
        bool thicknessEvaluated = false;
        bool beerLambertEvaluated = false;
        bool foamEvaluated = false;
        bool reactiveMaskWriteEnabled = false;
        uint32 itemCount = 0;
        uint32 drawCallCount = 0;
        uint32 waveCount = 0;
        uint32 fallbackCount = 0;
        uint32 waitIdleCallCount = 0;
        uint32 frameSlot = RVX_INVALID_INDEX;
        uint32 reactiveMaskWriteDrawCount = 0;
        uint64 uploadBytes = 0;
        uint64 resourceCreationCount = 0;
        uint64 pipelineCreationCount = 0;
        uint64 gridVertexCount = 0;
        uint64 gridIndexCount = 0;
        RenderFrameIdentity recordFrameIdentity{};
        uint64 recordGraphIdentity = 0;
        uint64 recordGraphGeneration = 0;
        uint64 recordEpoch = 0;
        std::vector<WaterFeatureSurfaceReceipt> surfaceReceipts;
        bool surfaceReceiptsOverflow = false;
        std::string unsupportedReason;
    };

    /** Resource requirements derived from a frozen v3 water snapshot. */
    struct WaterSurfaceRequirements
    {
        bool requested = false;
        bool opaqueSceneSnapshotRequired = false;
        bool depthSnapshotRequired = false;
        bool reactiveMaskRequired = false;
    };

    class IShaderProgramProvider;
    class PipelineCache;
    class WaterSurfaceRenderer;

    /**
     * Consumes only the RenderContracts snapshot and the frame's frozen effect
     * time. No Water runtime, gameplay query object, or mutable simulation is
     * visible to this renderer-owned pass.
     */
    class WaterFeaturePass final : public IRenderPass
    {
    public:
        WaterFeaturePass();
        ~WaterFeaturePass() override;

        const char* GetName() const override { return "WaterFeaturePass"; }
        int32_t GetPriority() const override { return PassPriority::Water; }

        void SetSnapshot(const WaterRenderSnapshot* snapshot);
        void SetShaderProgramProvider(IShaderProgramProvider* shaderProgramProvider);
        /** Borrow the renderer-owned neutral Texture3D volume fallback. */
        void SetPipelineCache(PipelineCache* pipelineCache) noexcept;
        void SetReverseZ(bool reverseZ);
        [[nodiscard]] WaterSurfaceRequirements GetRequirements() const noexcept;
        [[nodiscard]] const WaterFeaturePassStats& GetStats() const noexcept
        {
            return m_stats;
        }
        void PublishExecutionStats(const WaterFeaturePassStats& stats);

        bool IsRequestedEnabled() const override { return m_stats.requested; }
        bool IsSupported() const override
        {
            return !m_stats.requested || m_stats.supported;
        }
        const std::string& GetUnsupportedReason() const override
        {
            return m_stats.unsupportedReason;
        }
        bool IsEnabled() const override { return m_stats.enabled; }

        void Setup(RenderGraphBuilder& builder, const ViewData& view) override;
        void Execute(RHICommandContext& ctx, const ViewData& view) override;
        void Execute(RenderGraphPassContext& context, const ViewData& view) override;
        void AddToGraph(RenderGraph& graph,
                        const RenderPassRecordContext& context) override;
        void OnAdd(IRHIDevice* device) override;
        void OnRemove() override;

    private:
        void RebuildStats();

        const WaterRenderSnapshot* m_snapshot = nullptr;
        PipelineCache* m_pipelineCache = nullptr;
        std::unique_ptr<WaterSurfaceRenderer> m_renderer;
        WaterFeaturePassStats m_stats{};
    };
} // namespace RVX
