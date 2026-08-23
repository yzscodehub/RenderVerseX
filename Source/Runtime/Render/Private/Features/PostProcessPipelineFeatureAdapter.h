#pragma once

/**
 * @file PostProcessPipelineFeatureAdapter.h
 * @brief Typed feature boundary between scene HDR and presentation output.
 */

#include "Render/Features/IRenderFeature.h"
#include "Render/Passes/RenderPassRecordContext.h"
#include "Render/PostProcess/PostProcessStack.h"

#include <string>

namespace RVX
{
    /**
     * @brief Value-owned presentation inputs frozen for one graph recording.
     *
     * Scene color is intentionally absent: the adapter obtains it only from
     * TranslucencyOutputs v2 so no SceneRenderer-side post-process bypass can
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

    private:
        [[nodiscard]] bool HasMatchingRecordContext(
            const RenderFeatureContext& context) const noexcept;
        [[nodiscard]] bool HasCurrentGraphProvenance(
            const RenderFeatureGraphBuilder& builder) const noexcept;
        [[nodiscard]] bool HasMatchingFinalizedEvidence(
            const RenderFeatureGraphExecutionEvidence& evidence) const noexcept;
        void SetCompositionFailure(std::string diagnostic);
        void RefreshExecutionReceipt();

        PostProcessStack* m_stack = nullptr;
        PostProcessPipelineRecordContext m_recordContext{};
        RenderFeatureContext m_preparedContext{};
        PostProcessStackExecuteStats m_lastPlanStats{};
        RenderGraphBlackboardProvenance m_composedProvenance{};
        mutable RenderFeatureExecutionReceipt m_lastReceipt{};
        RenderPostProcessExecutionReceipt m_lastExecutionReceipt{};
        RenderFeatureReadiness m_readiness{};
        bool m_hasRecordContext = false;
        bool m_hasComposedPresentationPlan = false;
        bool m_hardwareSRGBEncodeBoundary = false;
    };
} // namespace RVX
