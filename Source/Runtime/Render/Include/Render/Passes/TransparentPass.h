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
#include "Render/Translucency/TranslucencyFramePlan.h"
#include "RenderContracts/TransparentTemporalHistory.h"

#include <atomic>
#include <memory>

namespace RVX
{
    class RenderResourceRegistry;
    class ClusteredLighting;
    class LightManager;
    class MaterialSystem;
    class PipelineCache;
    class RenderScene;
    struct TransparentStagedRecordingState;


    struct TransparentTemporalRasterReceipt
    {
        std::atomic_bool recorded{false};
        std::atomic_bool failed{false};
        std::atomic_uint32_t drawCount{0};
    };

    /** Raw graph-local candidates. Never bind flags directly to temporal resolve. */
    struct TransparentTemporalCandidates
    {
        RGTextureHandle motionCoverageDepth{};
        RGTextureHandle flags{};
        RGTextureHandle layerCount{};
        RenderOutputExtent extent{};
        std::shared_ptr<TransparentTemporalRasterReceipt> receipt;
    };

    /**
     * @brief Execution evidence for plan-driven mesh translucency recording.
     *
     * This intentionally lives beside TransparentPass rather than in the
     * frame-wide legacy statistics: a staged recording has two graphics
     * passes and must not allow the latter to overwrite the former's facts.
     */
    struct TransparentStagedRecordingStats
    {
        bool prepared = false;
        bool noWork = false;
        bool preparationFailed = false;
        bool weightedBlendedPassRecorded = false;
        bool directPassRecorded = false;
        bool weightedBlendedExecuted = false;
        bool directExecuted = false;
        bool executionFailed = false;
        uint64 planIdentityHash = 0;
        uint32 preparationCount = 0;
        uint32 plannedMeshDrawCount = 0;
        uint32 weightedBlendedDrawCount = 0;
        uint32 sortedRefractionDrawCount = 0;
        uint32 directAdditiveDrawCount = 0;
        uint32 weightedBlendedExecutedDrawCount = 0;
        uint32 sortedRefractionExecutedDrawCount = 0;
        uint32 directAdditiveExecutedDrawCount = 0;
    };

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
        /**
         * @brief Opaque, single-preparation token for staged mesh recording.
         *
         * A token is bound to one TranslucencyFramePlan, RenderPassRecordContext,
         * frame/view identity, and graph identity. It may record its weighted
         * and direct routes exactly once each.
         */
        class PreparedTranslucencyRecording final
        {
        public:
            PreparedTranslucencyRecording() = default;

            [[nodiscard]] bool IsPrepared() const noexcept;
            [[nodiscard]] uint64 GetPlanIdentityHash() const noexcept;
            [[nodiscard]] TransparentStagedRecordingStats GetStats() const;

        private:
            explicit PreparedTranslucencyRecording(
                std::shared_ptr<TransparentStagedRecordingState> state) noexcept;

            std::shared_ptr<TransparentStagedRecordingState> m_state;

            friend class TransparentPass;
        };

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

        /**
         * @brief Prepares every mesh plan entry exactly once without graph writes.
         *
         * Preparation resolves material snapshots, mesh buffers, object bindings,
         * and the route-specific pipeline. It validates the complete frozen draw
         * inventory before returning a token; any stale, duplicate, missing, or
         * unsupported entry produces an invalid token.
         */
        [[nodiscard]] PreparedTranslucencyRecording
        PrepareTranslucencyRecording(
            const TranslucencyFramePlan& plan,
            const RenderPassRecordContext& context);

        /**
         * @brief Records only WeightedBlended mesh entries into shared OIT targets.
         *
         * Accumulation must be RGBA16_FLOAT and revealage R16_FLOAT with matching
         * extent/sample count. The composition must clear the shared targets once;
         * this contributor always loads them, reads depth only, and never writes
         * HDR or the reactive mask.
         */
        [[nodiscard]] bool RecordWeightedBlended(
            RenderGraph& graph,
            PreparedTranslucencyRecording& prepared,
            RGTextureHandle accumulation,
            RGTextureHandle revealage);

        /**
         * @brief Records only sorted-refraction and direct-additive mesh entries.
         *
         * When weighted OIT is enabled, `sceneHDR` must be the distinct
         * post-composite HDR texture. The declared disabled/optional-sorted
         * fallback permits the original HDR target. The method preserves frozen
         * back-to-front order and writes the supplied reactive mask only for the
         * direct routes.
         */
        [[nodiscard]] bool RecordDirect(
            RenderGraph& graph,
            PreparedTranslucencyRecording& prepared,
            RGTextureHandle sceneHDR,
            RGTextureHandle reactiveMask);

        /** Records the sealed draws once, without altering color-route receipts.
         * Raw flags retain the unadmitted marker until material/LOD and overlap
         * qualification. Resources and palette reads belong to this exact graph.
         */
        [[nodiscard]] bool RecordTemporalCandidates(
            RenderGraph& graph,
            PreparedTranslucencyRecording& prepared,
            TransparentTemporalCandidates& output);

        /** Presentation authority only; recording or an abandoned frame cannot advance history. */
        void CommitTemporalHistoryAfterPresent(uint64 frameSequence,
            const RenderViewIdentity& view, bool presentationQualified);

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
        TransparentTemporalHistory m_temporalHistory;
        std::vector<TransparentTemporalDrawIdentity> m_pendingTemporalDraws;
        RenderViewIdentity m_pendingTemporalView{};
        uint64 m_pendingTemporalFrame = 0;
        std::shared_ptr<TransparentTemporalRasterReceipt> m_pendingTemporalReceipt;
    };

} // namespace RVX
