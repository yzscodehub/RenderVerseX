#pragma once

/**
 * @file VolumetricLightingFeatureAdapter.h
 * @brief Typed composition owner for the renderer's froxel lighting field.
 */

#include "Render/Features/IRenderFeature.h"
#include "Render/Lighting/FroxelVolumetricLighting.h"
#include "Render/Passes/RenderPassRecordContext.h"

#include <memory>
#include <string>

namespace RVX
{
    /** Frozen frame inputs that are not produced through the typed blackboard. */
    struct VolumetricLightingFeatureRecordContext
    {
        RenderPassRecordContext passRecordContext{};
        RenderVolumetricLightingSettings settings{};
        /** Candidate produced by the frame being recorded. */
        RenderViewHistoryIdentity historyCandidate{};
        /** Last presentation-qualified source, when reuse is permitted. */
        RenderViewHistoryIdentity historySource{};
        RenderHistoryResetReason historyResetReasons =
            RenderHistoryResetReason::HistoryUnavailable;
        bool historyAvailable = false;
    };

    /**
     * Runs injection, integration, temporal resolve, and opaque composite once
     * per view, then publishes the exact Texture3D to every later consumer.
     */
    class VolumetricLightingFeatureAdapter final : public IRenderFeature
    {
    public:
        explicit VolumetricLightingFeatureAdapter(
            PipelineCache* pipelineCache = nullptr) noexcept;

        void SetRecordContext(VolumetricLightingFeatureRecordContext context);
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

        [[nodiscard]] bool RequiresHistoryCommit() const noexcept;
        [[nodiscard]] bool NotifyHistorySubmission(
            const RenderViewHistoryIdentity& identity) noexcept;
        [[nodiscard]] bool CanCommitHistoryAfterPresent(
            const RenderViewHistoryIdentity& identity) const noexcept;
        [[nodiscard]] bool CommitHistoryAfterPresent(
            const RenderViewHistoryIdentity& identity) noexcept;
        void AbandonHistoryCandidate() noexcept;
        void ResetHistory() noexcept;
        void Shutdown() noexcept;

        [[nodiscard]] const FroxelVolumetricLightingStats& GetLastStats()
            const noexcept
        {
            return m_froxelLighting.GetLastStats();
        }

        [[nodiscard]] uint64 GetPendingPlanIdentity() const noexcept
        {
            return m_pendingPlanIdentity;
        }

    private:
        [[nodiscard]] bool HasMatchingRecordContext(
            const RenderFeatureContext& context) const noexcept;
        [[nodiscard]] bool HasMatchingFinalizedEvidence(
            const RenderFeatureGraphExecutionEvidence& evidence) const noexcept;
        [[nodiscard]] bool BuildPlanAndInputs(
            const ReflectionOutputs& reflection,
            const LocalShadowOutputs& localShadows,
            VolumetricLightingFramePlan& outPlan,
            FroxelVolumetricLightingInputs& outInputs,
            std::string& outDiagnostic) const;
        void SetCompositionFailure(std::string diagnostic);

        PipelineCache* m_pipelineCache = nullptr;
        FroxelVolumetricLighting m_froxelLighting{};
        VolumetricLightingFeatureRecordContext m_recordContext{};
        RenderFeatureContext m_preparedContext{};
        RenderGraphBlackboardProvenance m_composedProvenance{};
        mutable RenderFeatureExecutionReceipt m_lastReceipt{};
        RenderFeatureReadiness m_readiness{};
        RenderViewHistoryIdentity m_pendingHistoryCandidate{};
        uint64 m_pendingPlanIdentity = 0;
        bool m_hasRecordContext = false;
        bool m_outputPublished = false;
        bool m_identityRoute = true;
    };
} // namespace RVX
