#pragma once

/**
 * @file ParticleFeaturePass.h
 * @brief Render-owned particle feature pass consuming RenderContracts snapshots.
 */

#include "Render/Passes/IRenderPass.h"
#include "Render/Translucency/TranslucencyFramePlan.h"
#include "RenderContracts/ParticleRenderSnapshot.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace RVX
{
    /** Result of preparing or recording one typed particle translucency stage. */
    enum class ParticleFeatureRecordingCode : uint8
    {
        Ready = 0,
        Recorded,
        NoWork,
        InvalidFramePlan,
        InvalidRecordContext,
        PlanInventoryMismatch,
        UnsupportedPlanRoute,
        RequiredCapabilityUnavailable,
        StalePreparedRecording,
        InvalidGraphTargets,
        StageAlreadyRecorded,
        StageOrderViolation,
        ResourcePreparationFailed,
    };

    class ParticleFeaturePreparedRecording;
    using ParticleFeaturePreparedRecordingRef =
        std::shared_ptr<ParticleFeaturePreparedRecording>;

    /** Shared WBOIT attachments owned and cleared by translucency composition. */
    struct ParticleWeightedBlendedRecordTargets
    {
        RGTextureHandle accumulation{};
        RGTextureHandle revealage{};
    };

    /** Post-OIT direct outputs. Both handles must belong to the prepared graph. */
    struct ParticleDirectRecordTargets
    {
        RGTextureHandle hdrSceneColor{};
        RGTextureHandle reactiveMask{};
    };

    struct ParticleFeaturePreparedRecordingResult
    {
        ParticleFeatureRecordingCode code =
            ParticleFeatureRecordingCode::InvalidRecordContext;
        ParticleFeaturePreparedRecordingRef recording;
        uint64 translucencyPlanIdentity = 0;
        uint32 weightedBatchCount = 0;
        uint32 weightedItemCount = 0;
        uint32 directBatchCount = 0;
        uint32 directItemCount = 0;
        std::string diagnostic;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return code == ParticleFeatureRecordingCode::Ready ||
                   code == ParticleFeatureRecordingCode::NoWork;
        }
    };

    struct ParticleFeatureStageRecordResult
    {
        ParticleFeatureRecordingCode code =
            ParticleFeatureRecordingCode::StalePreparedRecording;
        uint64 translucencyPlanIdentity = 0;
        uint32 batchCount = 0;
        uint32 itemCount = 0;
        /** Direct stage output after its optional distortion composite. */
        RGTextureHandle hdrSceneColor{};
        std::string diagnostic;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return code == ParticleFeatureRecordingCode::Recorded ||
                   code == ParticleFeatureRecordingCode::NoWork;
        }
    };

    /** Bounded value receipt for one contiguous particle billboard batch. */
    struct ParticleFeatureBatchReceipt
    {
        TranslucencyRoute route = TranslucencyRoute::Unsupported;
        uint32 planSourceOrdinal = RVX_INVALID_INDEX;
        uint64 planSourceIdentity = 0;
        ParticleRenderSnapshotBlendMode blendMode =
            ParticleRenderSnapshotBlendMode::AlphaBlend;
        uint64 effectDescriptorHash = 0;
        AssetId textureAssetId{};
        RenderResourceHandle texture{};
        uint32 textureResourceGeneration = 0;
        uint64 textureContentRevision = 0;
        uint32 instanceCount = 0;
        bool textureResolved = false;
    };

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
        bool softDepthRequested = false;
        bool softDepthRequired = false;
        bool softDepthEnabled = false;
        bool softDepthOptionalFallback = false;
        bool flipbookRequested = false;
        bool flipbookValidated = false;
        bool reactiveMaskRequested = false;
        bool reactiveMaskEnabled = false;
        bool distortionRequested = false;
        bool distortionEnabled = false;
        bool distortionPassScheduled = false;
        bool distortionPassExecuted = false;
        bool distortionCompositeExecuted = false;
        bool softDepthSnapshotReady = false;
        bool softDepthLinearizationReady = false;
        bool reactiveMaskWriteEnabled = false;
        uint32 reactiveMaskWriteDrawCount = 0;
        uint32 lightHintRequestedCount = 0;
        uint32 lightHintAdmittedCount = 0;
        uint32 lightHintDroppedCount = 0;
        uint64 recordFrameSequence = 0;
        RenderFrameIdentity recordFrameIdentity{};
        uint64 recordGraphIdentity = 0;
        uint64 recordGraphGeneration = 0;
        uint64 recordEpoch = 0;
        uint64 translucencyPlanIdentity = 0;
        uint64 translucencyParticleSourceIdentity = 0;
        uint32 preparationCount = 0;
        uint32 uploadPublicationCount = 0;
        bool weightedGraphPassScheduled = false;
        bool directGraphPassScheduled = false;
        uint32 weightedBatchCount = 0;
        uint32 weightedItemCount = 0;
        uint32 weightedDrawCallCount = 0;
        uint32 directBatchCount = 0;
        uint32 directItemCount = 0;
        uint32 directDrawCallCount = 0;
        uint32 optionalSortedFallbackBatchCount = 0;
        uint32 optionalSortedFallbackItemCount = 0;
        std::vector<ParticleFeatureBatchReceipt> batchReceipts;
        bool batchReceiptsOverflow = false;
        std::string unsupportedReason;
    };

    /** Value-only graph allocation requirements derived from a frozen packet snapshot. */
    struct ParticleVfxRequirements
    {
        bool requested = false;
        bool softDepthRequested = false;
        bool softDepthRequired = false;
        bool reactiveMaskRequested = false;
        bool distortionRequested = false;
    };

    class IShaderProgramProvider;
    class PipelineCache;
    class ParticleBillboardRenderer;
    class RenderResourceRegistry;
    struct ParticleBillboardDrawPlan;

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
        /** Borrow the renderer-owned neutral Texture3D volume fallback. */
        void SetPipelineCache(PipelineCache* pipelineCache) noexcept;
        /** Borrow the SceneRenderer-owned exact-generation resource registry. */
        void SetResourceRegistry(RenderResourceRegistry* resourceRegistry);
        /** @brief Select the depth comparison convention before resource creation. */
        void SetReverseZ(bool reverseZ);
        const ParticleFeaturePassStats& GetStats() const { return m_stats; }
        [[nodiscard]] ParticleVfxRequirements GetVfxRequirements() const noexcept;
        /**
         * @brief True only after the complete distortion producer/composite
         * chain has registered against this graph recording.
         */
        [[nodiscard]] bool HasRegisteredDistortionComposite() const noexcept
        {
            return m_stats.distortionRequested &&
                   m_stats.distortionPassScheduled;
        }
        /**
         * Extract the frozen per-system light hints before the immutable
         * LightingFramePlan is sealed. The returned values never borrow the
         * Particle runtime or LightManager.
         */
        [[nodiscard]] bool CollectLightingHints(
            std::vector<ParticleRenderLightHint>& outHints,
            std::string& outDiagnostic) const;
        /** Publish deterministic local-light admission evidence for this snapshot. */
        void SetLightingHintAdmissionStats(uint32 requested,
                                           uint32 admitted,
                                           uint32 dropped) noexcept;
        /** @brief Publish the completed graph record's immutable diagnostic receipt. */
        void PublishExecutionStats(const ParticleFeaturePassStats& stats);

        /**
         * @brief Materialize and validate the cooked particle WBOIT pipeline
         * before a translucency frame plan commits to the weighted route.
         *
         * This is deliberately a non-graph operation. It uses the same
         * persistent renderer cache as normal particle preparation, so a
         * succeeding preflight neither records graph work nor creates the
         * shader or pipeline again when the later draw submission is prepared.
         */
        [[nodiscard]] bool PreflightWeightedBlendedPipeline(
            std::string& outDiagnostic);

        /**
         * Validate, resolve and upload the particle submission exactly once for
         * an immutable translucency plan and graph recording.
         *
         * The returned object is intentionally opaque. It binds the plan,
         * frame/view identities, graph identity/generation and all resolved
         * resource generations needed by both recording stages.
         */
        [[nodiscard]] ParticleFeaturePreparedRecordingResult
        PrepareTranslucencyRecording(
            const TranslucencyFramePlan& plan,
            std::shared_ptr<const ParticleBillboardDrawPlan> drawPlan,
            const RenderPassRecordContext& context);

        /** Record only Particle + WeightedBlended entries into shared OIT MRTs. */
        [[nodiscard]] ParticleFeatureStageRecordResult RecordWeightedBlended(
            RenderGraph& graph,
            const ParticleFeaturePreparedRecordingRef& prepared,
            const ParticleWeightedBlendedRecordTargets& targets);

        /**
         * Record direct additive and declared optional sorted fallback after
         * the feature-level OIT composite. The method never composites OIT.
         */
        [[nodiscard]] ParticleFeatureStageRecordResult RecordDirect(
            RenderGraph& graph,
            const ParticleFeaturePreparedRecordingRef& prepared,
            const ParticleDirectRecordTargets& targets);

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
        PipelineCache* m_pipelineCache = nullptr;
        RenderResourceRegistry* m_resourceRegistry = nullptr;
        std::unique_ptr<ParticleBillboardRenderer> m_renderer;
        ParticleFeaturePassStats m_stats;
        uint32 m_lightHintRequestedCount = 0;
        uint32 m_lightHintAdmittedCount = 0;
        uint32 m_lightHintDroppedCount = 0;
        uint64 m_recordFrameSequence = 0;
        RenderFrameIdentity m_recordFrameIdentity{};
        uint64 m_recordGraphIdentity = 0;
        uint64 m_recordGraphGeneration = 0;
        uint64 m_recordEpoch = 0;
        std::vector<ParticleFeatureBatchReceipt> m_batchReceipts;
        bool m_batchReceiptsOverflow = false;
    };

} // namespace RVX
