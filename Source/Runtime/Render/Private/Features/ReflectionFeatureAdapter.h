#pragma once

/**
 * @file ReflectionFeatureAdapter.h
 * @brief Recipe-neutral typed reflection boundary over lit HDR scene color.
 */

#include "Render/Features/IRenderFeature.h"
#include "Render/Passes/RenderPassRecordContext.h"
#include "Render/PostProcess/SSR.h"
#include "Render/RayTracing/HybridRayTracingFramePlan.h"
#include "Render/Reflection/ReflectionFramePlan.h"

#include <array>
#include <memory>
#include <optional>
#include <string>

namespace RVX
{
    class PipelineCache;
    class DefaultRenderPassBundle;

    /** Exact graph resources paired with one sealed ReflectionFramePlan. */
    struct ReflectionFeatureRecordContext
    {
        RenderPassRecordContext pass{};
        std::shared_ptr<const ReflectionFramePlan> plan{};
        std::array<RGTextureHandle, RVX_SSR_LOCAL_PROBE_CAPACITY>
            probeTextures{};
        std::array<bool, RVX_SSR_LOCAL_PROBE_CAPACITY> probeReady{};
        /**
         * Complete policy sealed with the reflection plan. It is optional
         * only for a data-only IBL reflection boundary with no SSR/probe
         * graph work.
         */
        std::optional<SSRConfig> ssrQualityPolicy{};
        /** Hash of ssrQualityPolicy, computed before reflection planning. */
        uint64 ssrConfigurationIdentity = 0;
        /** Exact cooked SSR pipeline-family identity for this graph. */
        uint64 ssrPipelineIdentity = 0;
        /**
         * @brief Immutable hybrid route selected before graph recording.
         *
         * This value is deliberately copied into the feature context.  The
         * reflection adapter must never re-evaluate an RT fallback while
         * registering graph work for the already accepted frame.
         */
        std::optional<HybridRayTracingFramePlan> hybridRayTracingPlan{};
    };

    /**
     * @brief Preserves the current IBL-lit HDR result at the Reflection phase.
     *
     * The adapter records the immutable Hybrid RT reflection chain or the
     * selected Hi-Z SSR -> local probe -> global IBL route. Otherwise it
     * preserves the already lit scene color as an explicit data-only boundary.
     */
    class ReflectionFeatureAdapter final : public IRenderFeature
    {
    public:
        explicit ReflectionFeatureAdapter(
            DefaultRenderPassBundle* passBundle = nullptr,
            PipelineCache* pipelineCache = nullptr) noexcept;

        /** Compatibility path used by focused typed-blackboard tests. */
        void SetRecordContext(RenderPassRecordContext context);
        void SetRecordContext(ReflectionFeatureRecordContext context);
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

        /**
         * @brief True only after the Reflection feature received exact graph
         * completion evidence for the current record context.
         */
        [[nodiscard]] bool HasFinalizedSSRComputeStats() const noexcept
        {
            return m_hasFinalizedSSRComputeStats;
        }

        /**
         * @brief Immutable SSR status frozen by exact graph finalization.
         *
         * Callers must first check HasFinalizedSSRComputeStats(). The result
         * is intentionally value-only and exposes no RenderGraph resources.
         */
        [[nodiscard]] const SSRComputeStats& GetFinalizedSSRComputeStats()
            const noexcept
        {
            return m_finalizedSSRComputeStats;
        }

        /** Arm one qualification-only physical route-mask readback. */
        [[nodiscard]] bool ArmSSRQualificationProbe() noexcept;
        [[nodiscard]] bool HasPendingSSRQualificationProbe() const noexcept;
        [[nodiscard]] std::optional<SSRQualificationProbeIdentity>
        GetPendingSSRQualificationProbeIdentity() const noexcept;
        [[nodiscard]] bool NotifySSRQualificationProbeSubmission(
            const SSRQualificationProbeIdentity& identity,
            const GPUCompletionToken& completion,
            const RenderSubmissionTracker& tracker) noexcept;
        [[nodiscard]] bool MarkSSRQualificationProbePresented(
            const SSRQualificationProbeIdentity& identity) noexcept;
        [[nodiscard]] bool PollSSRQualificationProbe(
            const RenderSubmissionTracker& tracker,
            SSRQualificationProbeStats& outStats) noexcept;
        void AbandonSSRQualificationProbe() noexcept;

    private:
        [[nodiscard]] bool HasMatchingRecordContext(
            const RenderFeatureContext& context) const noexcept;
        [[nodiscard]] bool HasValidLightingPlan(
            const RenderFeatureContext& context) const noexcept;
        [[nodiscard]] bool HasValidReflectionPlan(
            const RenderFeatureContext& context) const noexcept;
        [[nodiscard]] bool HasValidSSRPolicyAndPlan(
            const RenderFeatureContext& context) const noexcept;
        [[nodiscard]] bool HasValidHybridRayTracingPlan(
            const RenderFeatureContext& context) const noexcept;
        [[nodiscard]] bool HasReadyRayTracedReflectionPasses() const noexcept;
        [[nodiscard]] bool UsesHybridRayTracedReflectionRoute() const noexcept;
        [[nodiscard]] bool UsesHybridScreenSpaceReflectionRoute() const noexcept;
        void SetCompositionFailure(std::string diagnostic);

        RenderPassRecordContext m_recordContext{};
        std::shared_ptr<const ReflectionFramePlan> m_reflectionPlan{};
        std::array<RGTextureHandle, RVX_SSR_LOCAL_PROBE_CAPACITY>
            m_probeTextures{};
        std::array<bool, RVX_SSR_LOCAL_PROBE_CAPACITY> m_probeReady{};
        std::optional<SSRConfig> m_ssrQualityPolicy{};
        uint64 m_ssrConfigurationIdentity = 0;
        uint64 m_ssrPipelineIdentity = 0;
        std::optional<HybridRayTracingFramePlan> m_hybridRayTracingPlan{};
        DefaultRenderPassBundle* m_passBundle = nullptr;
        SSR m_ssr{};
        RenderFeatureContext m_preparedContext{};
        mutable RenderFeatureExecutionReceipt m_lastReceipt{};
        RenderFeatureReadiness m_readiness{};
        bool m_hasRecordContext = false;
        bool m_requiresReflectionPlan = false;
        bool m_ssrScheduled = false;
        bool m_ssrStatsObserved = false;
        bool m_hasFinalizedSSRComputeStats = false;
        SSRComputeStats m_finalizedSSRComputeStats{};
        bool m_rayTracedReflectionScheduled = false;
        bool m_outputPublished = false;
    };
} // namespace RVX
