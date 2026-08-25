#pragma once

/**
 * @file AmbientOcclusionFeatureAdapter.h
 * @brief Typed composition boundary for the renderer-owned GTAO field.
 */

#include "Render/Features/IRenderFeature.h"
#include "Render/Lighting/GroundTruthAmbientOcclusion.h"
#include "Render/Passes/RenderPassRecordContext.h"

#include <string>

namespace RVX
{
    class PipelineCache;

    /** @brief Frozen inputs needed to produce one view's AO visibility field. */
    struct AmbientOcclusionFeatureRecordContext
    {
        RenderPassRecordContext passRecordContext{};
        RenderAmbientOcclusionSettings settings{};
        RenderViewHistoryIdentity historyIdentity{};
        RenderHistoryResetReason historyResetReasons =
            RenderHistoryResetReason::HistoryUnavailable;
        bool historyAvailable = false;
    };

    /**
     * @brief Lighting-owned GTAO feature shared by Forward+ and Deferred.
     *
     * Disabled AO still publishes a graph-owned neutral visibility field so
     * both lighting recipes keep one strict dependency chain. Detailed GTAO
     * evidence remains empty unless the effect was explicitly requested.
     */
    class AmbientOcclusionFeatureAdapter final : public IRenderFeature
    {
    public:
        explicit AmbientOcclusionFeatureAdapter(
            PipelineCache* pipelineCache) noexcept;

        void SetRecordContext(AmbientOcclusionFeatureRecordContext context);
        void SetPipelineCache(PipelineCache* pipelineCache) noexcept;

        [[nodiscard]] RenderFeatureDescriptor Describe() const override;
        void Prepare(const RenderFeatureContext& context) override;
        [[nodiscard]] RenderFeatureReadiness ResolveReadiness() const override;
        void AddPasses(RenderFeatureGraphBuilder& builder,
                       RenderGraphBlackboard& blackboard) override;
        void OnGraphExecutionFinalized(
            const RenderFeatureGraphExecutionEvidence& evidence) override;
        [[nodiscard]] RenderFeatureExecutionReceipt CollectReceipt()
            const override;

        [[nodiscard]] const RenderAmbientOcclusionExecutionReceipt&
        GetLastExecutionReceipt() const noexcept
        {
            return m_lastExecutionReceipt;
        }

        [[nodiscard]] bool RequiresHistoryCommit() const noexcept;
        [[nodiscard]] bool CanCommitHistoryAfterPresent(
            const RenderViewHistoryIdentity& identity) const noexcept;
        [[nodiscard]] bool NotifyHistorySubmission(
            const RenderViewHistoryIdentity& identity) noexcept;
        [[nodiscard]] bool CommitHistoryAfterPresent(
            const RenderViewHistoryIdentity& identity) noexcept;
        void AbandonHistoryCandidate() noexcept;
        void ResetHistory() noexcept;

        /** @brief Arm one physical GPU observation for the next exact view. */
        [[nodiscard]] bool RequestQualificationProbe() noexcept;
        [[nodiscard]] bool NotifyQualificationProbeSubmission(
            const RenderViewHistoryIdentity& identity,
            const GPUCompletionToken& completion,
            const RenderSubmissionTracker& tracker) noexcept;
        [[nodiscard]] bool CompleteQualificationProbe(
            const RenderSubmissionTracker& tracker,
            RenderAmbientOcclusionExecutionReceipt& inOutReceipt) noexcept;
        void AbandonQualificationProbe() noexcept;
        [[nodiscard]] bool HasPendingQualificationProbe() const noexcept;

        void Shutdown() noexcept;

    private:
        [[nodiscard]] bool HasMatchingRecordContext(
            const RenderFeatureContext& context) const noexcept;
        [[nodiscard]] bool HasCurrentGraphProvenance(
            const RenderFeatureGraphBuilder& builder) const noexcept;
        [[nodiscard]] bool HasMatchingFinalizedEvidence(
            const RenderFeatureGraphExecutionEvidence& evidence) const noexcept;
        [[nodiscard]] bool HasRequiredPipelines() const noexcept;
        [[nodiscard]] bool HasExactVelocityProducerExecution() const noexcept;
        [[nodiscard]] uint64 CalculateSurfaceFieldHash(
            const SceneTextures& textures) const noexcept;
        [[nodiscard]] uint64 CalculateVelocityFieldIdentity(
            const SceneTextures& textures) const noexcept;
        [[nodiscard]] uint64 CalculatePlanContentHash(
            const GroundTruthAmbientOcclusionStats& stats) const noexcept;
        void RefreshExecutionReceipt();
        void SetCompositionFailure(std::string diagnostic);

        PipelineCache* m_pipelineCache = nullptr;
        GroundTruthAmbientOcclusion m_gtao{};
        AmbientOcclusionFeatureRecordContext m_recordContext{};
        RenderFeatureContext m_preparedContext{};
        RenderGraphBlackboardProvenance m_composedProvenance{};
        GroundTruthAmbientOcclusionOutput m_composedOutput{};
        uint64 m_velocityFieldIdentity = 0;
        mutable RenderFeatureExecutionReceipt m_lastReceipt{};
        RenderAmbientOcclusionExecutionReceipt m_lastExecutionReceipt{};
        RenderFeatureReadiness m_readiness{};
        bool m_hasRecordContext = false;
        bool m_outputPublished = false;
        bool m_qualificationProbeRequestedForNextFrame = false;
    };
} // namespace RVX
