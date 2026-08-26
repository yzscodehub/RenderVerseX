#pragma once

/**
 * @file PostProcessPipelineFeatureAdapter.h
 * @brief Typed feature boundary between scene HDR and presentation output.
 */

#include "Render/Features/IRenderFeature.h"
#include "Render/Passes/RenderPassRecordContext.h"
#include "Render/PostProcess/PostProcessStack.h"

#include <optional>
#include <string>

namespace RVX
{
    /**
     * @brief Value-owned presentation inputs frozen for one graph recording.
     *
     * Scene color is intentionally absent: the adapter obtains it only from
     * TranslucencyOutputs v4 so no SceneRenderer-side post-process bypass can
     * substitute a stale or untyped surface.
     */
    struct PostProcessPipelineRecordContext
    {
        RenderFrameIdentity frameIdentity{};
        RenderViewIdentity viewIdentity{};
        RGTextureHandle presentableColor{};
        SceneTextures::Metadata presentableColorMetadata{};
        PostProcessFrameInputs frameInputs{};
        RenderPassRecordContext passRecordContext{};
    };

    /** @brief One typed HDR-to-presentation renderer feature. */
    class PostProcessPipelineFeatureAdapter final : public IRenderFeature
    {
    public:
        explicit PostProcessPipelineFeatureAdapter(
            PostProcessStack* stack) noexcept;

        void SetRecordContext(PostProcessPipelineRecordContext context);
        /**
         * @brief Bind the composition-owned stack at a renderer lifecycle boundary.
         *
         * The adapter borrows the stack. Rebinding intentionally invalidates
         * frozen record state and receipts so a graph cannot consume evidence
         * from the prior stack lifetime.
         */
        void SetStack(PostProcessStack* stack) noexcept;

        [[nodiscard]] RenderFeatureDescriptor Describe() const override;
        void Prepare(const RenderFeatureContext& context) override;
        [[nodiscard]] RenderFeatureReadiness ResolveReadiness() const override;
        void AddPasses(RenderFeatureGraphBuilder& builder,
                       RenderGraphBlackboard& blackboard) override;
        void OnGraphExecutionFinalized(
            const RenderFeatureGraphExecutionEvidence& evidence) override;
        [[nodiscard]] RenderFeatureExecutionReceipt CollectReceipt()
            const override;

        [[nodiscard]] const PostProcessStackExecuteStats& GetLastPlanStats()
            const noexcept
        {
            return m_lastPlanStats;
        }

        [[nodiscard]] const RenderPostProcessExecutionReceipt&
        GetLastExecutionReceipt() const noexcept
        {
            return m_lastExecutionReceipt;
        }

        [[nodiscard]] const RenderToonOutlineExecutionReceipt&
        GetLastToonOutlineExecutionReceipt() const noexcept
        {
            return m_lastToonOutlineExecutionReceipt;
        }

        /**
         * @brief Commit all post-process temporal candidates only after the
         *        exact frame/view crossed a successful presentation boundary.
         */
        [[nodiscard]] bool CommitTemporalHistoryAfterPresent(
            const RenderViewHistoryIdentity& identity) noexcept;

        /**
         * @brief Side-effect-free exact-candidate preflight for presentation.
         *
         * SceneRenderer uses this together with its view-history coordinator
         * before advancing either temporal owner.
         */
        [[nodiscard]] bool CanCommitTemporalHistoryAfterPresent(
            const RenderViewHistoryIdentity& identity) noexcept;

        /** @brief Discard unpresented TAA/exposure candidates without advancing history. */
        void AbandonTemporalHistoryCandidate() noexcept;

        [[nodiscard]] bool RequiresTemporalHistoryCommit() const noexcept;

        /** @brief Arm one explicit 48-byte post-fence exposure probe. */
        [[nodiscard]] bool RequestExposureQualificationProbe() noexcept;

        /** @brief Map the exact probe only after its presentation fence completed. */
        [[nodiscard]] bool CompleteExposureQualificationProbe(
            const RenderViewHistoryIdentity& identity,
            RenderPostProcessExecutionReceipt& outReceipt) noexcept;

    private:
        [[nodiscard]] bool HasMatchingRecordContext(
            const RenderFeatureContext& context) const noexcept;
        [[nodiscard]] bool HasCurrentGraphProvenance(
            const RenderFeatureGraphBuilder& builder) const noexcept;
        [[nodiscard]] bool HasMatchingFinalizedEvidence(
            const RenderFeatureGraphExecutionEvidence& evidence) const noexcept;
        void SetCompositionFailure(std::string diagnostic);
        void RefreshExecutionReceipt();
        void RefreshToonOutlineExecutionReceipt();
        void SnapshotExposureProbeSourceReceipt() noexcept;
        void ClearExposureProbeSourceReceipt() noexcept;

        PostProcessStack* m_stack = nullptr;
        PostProcessPipelineRecordContext m_recordContext{};
        RenderFeatureContext m_preparedContext{};
        PostProcessStackExecuteStats m_lastPlanStats{};
        RenderGraphBlackboardProvenance m_composedProvenance{};
        mutable RenderFeatureExecutionReceipt m_lastReceipt{};
        RenderPostProcessExecutionReceipt m_lastExecutionReceipt{};
        RenderToonOutlineExecutionReceipt
            m_lastToonOutlineExecutionReceipt{};
        RenderFeatureReadiness m_readiness{};
        bool m_hasRecordContext = false;
        bool m_hasComposedPresentationPlan = false;
        bool m_hardwareSRGBEncodeBoundary = false;
        ExposureQualificationProbe m_lastExposureProbe{};
        std::optional<RenderViewHistoryIdentity>
            m_exposureProbeSourceIdentity;
        std::optional<RenderPostProcessExecutionReceipt>
            m_exposureProbeSourceReceipt;
    };
} // namespace RVX
