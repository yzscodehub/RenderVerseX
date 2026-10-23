#pragma once

/**
 * @file Exposure.h
 * @brief GPU-owned histogram metering and presentation-qualified exposure history.
 */

#include "Render/Graph/RenderGraph.h"
#include "RenderContracts/RenderExposure.h"
#include "RenderContracts/RenderFrameTypes.h"

#include <memory>
#include <string>

namespace RVX
{
    class PipelineCache;

    inline constexpr uint32 RVX_EXPOSURE_HISTOGRAM_BIN_COUNT = 256u;
    inline constexpr uint32 RVX_EXPOSURE_STATE_STRIDE = 48u;

    /** Immutable prior-presented state selected before HDR graph construction. */
    struct ExposureFrameInput
    {
        RHIBufferRef stateBuffer{};
        uint64 configurationHash = 0;
        /** Stable semantic identity, never derived from a GPU pointer. */
        uint64 contentHash = 0;
        bool historyReset = true;
        RenderHistoryResetReason resetReasons =
            RenderHistoryResetReason::HistoryUnavailable;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return stateBuffer && configurationHash != 0 && contentHash != 0;
        }
    };

    /**
     * Fail-closed temporal-history decision after exposure compatibility is
     * known.  Exposure settings are part of the color-space identity of every
     * temporal HDR resource, even when the upstream view identity itself did
     * not change.
     */
    struct ExposureTemporalHistoryDecision
    {
        bool available = false;
        bool reset = true;
        RenderHistoryResetReason resetReasons =
            RenderHistoryResetReason::HistoryUnavailable;

        [[nodiscard]] bool IsConsistent() const noexcept
        {
            return available ==
                (!reset &&
                 resetReasons == RenderHistoryResetReason::None);
        }
    };

    [[nodiscard]] inline ExposureTemporalHistoryDecision
    ResolveExposureTemporalHistory(
        bool upstreamAvailable,
        bool upstreamReset,
        RenderHistoryResetReason upstreamResetReasons,
        const ExposureFrameInput& exposureInput) noexcept
    {
        ExposureTemporalHistoryDecision decision;
        decision.resetReasons = upstreamResetReasons;
        if (!exposureInput.IsValid())
        {
            decision.resetReasons |=
                RenderHistoryResetReason::HistoryUnavailable;
            return decision;
        }

        decision.resetReasons |= exposureInput.resetReasons;
        decision.available = upstreamAvailable && !upstreamReset &&
            !exposureInput.historyReset &&
            decision.resetReasons == RenderHistoryResetReason::None;
        decision.reset = upstreamReset || !decision.available ||
            decision.resetReasons != RenderHistoryResetReason::None;
        if (decision.reset &&
            decision.resetReasons == RenderHistoryResetReason::None)
        {
            decision.resetReasons |=
                RenderHistoryResetReason::HistoryUnavailable;
        }
        return decision;
    }

    /** GPU graph handles published to HDR post-process consumers for one view. */
    struct ExposureGraphOutputs
    {
        RGBufferHandle committedState{};
        RGBufferHandle candidateState{};
        RGBufferHandle histogram{};
        uint64 configurationHash = 0;
        /** Semantic identity of the prior-presented producer input. */
        uint64 committedStateContentHash = 0;
        /** Semantic identity of the candidate shared by all post consumers. */
        uint64 candidateStateContentHash = 0;
        bool histogramUsed = false;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return committedState.IsValid() && candidateState.IsValid() &&
                   histogram.IsValid() && configurationHash != 0 &&
                   committedStateContentHash != 0 &&
                   candidateStateContentHash != 0;
        }
    };

    /** Execution facts that are valid without any CPU readback. */
    struct ExposureControllerStats
    {
        bool requested = false;
        bool pipelineReady = false;
        bool inputsReady = false;
        bool histogramClearExecuted = false;
        bool histogramBuildExecuted = false;
        bool adaptationExecuted = false;
        bool candidatePrepared = false;
        bool candidateCommitted = false;
        bool candidateAbandoned = false;
        bool usedCommittedHistory = false;
        bool qualificationProbeRequested = false;
        bool qualificationProbeRecorded = false;
        bool qualificationProbeReady = false;
        uint32 histogramClearDispatchCount = 0;
        uint32 histogramBuildDispatchCount = 0;
        uint32 adaptationDispatchCount = 0;
        uint64 configurationHash = 0;
        uint64 committedStateContentHash = 0;
        uint64 candidateStateContentHash = 0;
        uint64 frameIndex = 0;
        RenderViewHistoryIdentity historyIdentity{};
        bool historyAvailable = false;
        RenderHistoryResetReason historyResetReasons =
            RenderHistoryResetReason::HistoryUnavailable;
        std::string fallbackReason;
    };

    /**
     * Explicit opt-in observation of one GPU-generated exposure candidate.
     *
     * Normal renderer execution never maps this data.  The caller may request
     * it for a qualification frame and must call CompleteQualificationProbe()
     * only after the submission fence for the matching identity has completed.
     */
    struct ExposureQualificationProbe
    {
        bool requested = false;
        bool recorded = false;
        bool ready = false;
        uint64 frameIndex = 0;
        RenderViewHistoryIdentity historyIdentity{};
        uint64 configurationHash = 0;
        /** Exact candidate nonce copied from this probe's GPU state. */
        uint64 candidateStateContentHash = 0;
        float targetEV100 = 0.0f;
        float adaptedEV100 = 0.0f;
        float exposureScale = 1.0f;
        float preExposure = 1.0f;
        float previousPreExposure = 1.0f;
    };

    /**
     * Owns persistent exposure state ping-pong buffers.  A state becomes the
     * next committed input only after the exact evaluated view is presented;
     * an abandoned candidate never alters the following frame's pre-exposure.
     */
    class ExposureController final
    {
    public:
        ExposureController() = default;
        ~ExposureController();

        ExposureController(const ExposureController&) = delete;
        ExposureController& operator=(const ExposureController&) = delete;

        void SetResources(PipelineCache* pipelineCache);
        void Shutdown() noexcept;

        [[nodiscard]] bool IsSupported() const noexcept;
        [[nodiscard]] RHIBuffer* GetCommittedStateBuffer() const noexcept;
        [[nodiscard]] RHIBufferRef GetCommittedStateBufferRef() const noexcept;
        [[nodiscard]] uint64 GetCommittedConfigurationHash() const noexcept;
        [[nodiscard]] const ExposureControllerStats& GetLastStats() const noexcept;

        /**
         * Selects the one prior state that HDR producers must import before
         * graph construction. Reset/configuration mismatch deliberately uses
         * the finite bootstrap state instead of a stale presented state.
         */
        [[nodiscard]] ExposureFrameInput PrepareFrameInput(
            const RenderExposureSettings& settings,
            const RenderViewHistoryIdentity& historyIdentity,
            bool historyAvailable,
            RenderHistoryResetReason historyResetReasons);

        /**
         * Returns the exact prepared input only when both the view identity
         * and exposure configuration still match.  This lets downstream
         * planners canonicalize their temporal contract before hashing it.
         */
        [[nodiscard]] const ExposureFrameInput* FindPreparedFrameInput(
            const RenderViewHistoryIdentity& historyIdentity,
            uint64 configurationHash) const noexcept;

        /**
         * Registers clear/build/adapt passes.  `committedState` must be the
         * one graph import of GetCommittedStateBuffer() shared by every HDR
         * producer; this method never creates a competing graph resource edge.
         * `sceneColor` must have been written with that state's scale.
         * HistogramAuto executes all three passes; ManualEV100 omits clear
         * and build but always executes one resolve candidate pass.
         */
        [[nodiscard]] ExposureGraphOutputs AddToGraph(
            RenderGraph& graph,
            RGTextureHandle sceneColor,
            RGBufferHandle committedState,
            const RenderExposureSettings& settings,
            float32 fixedDeltaSeconds,
            uint64 frameIndex,
            const RenderViewHistoryIdentity& historyIdentity,
            bool historyAvailable,
            RenderHistoryResetReason historyResetReasons);

        [[nodiscard]] bool CanCommitAfterPresent(
            const RenderViewHistoryIdentity& identity) const noexcept;
        [[nodiscard]] bool CommitAfterPresent(
            const RenderViewHistoryIdentity& identity) noexcept;
        void AbandonPendingCandidate() noexcept;
        void ResetHistory() noexcept;

        /** Arm exactly one small post-fence qualification readback. */
        [[nodiscard]] bool RequestQualificationProbe() noexcept;
        /** Cancel an unsubmitted or abandoned qualification observation. */
        void CancelQualificationProbe() noexcept;
        [[nodiscard]] bool CompleteQualificationProbe(
            const RenderViewHistoryIdentity& identity,
            ExposureQualificationProbe& outProbe) noexcept;

    private:
        struct FrameRecord;
        struct GPUState;
        struct GPUConstants;

        [[nodiscard]] bool EnsureRuntimeResources();
        [[nodiscard]] bool HasPipelineResources() const noexcept;
        [[nodiscard]] RHIBufferRef CreateImmutableConstants(
            const GPUConstants& constants,
            const char* debugName) const;
        [[nodiscard]] bool EnsureQualificationReadback();
        void ResetRuntimeResources() noexcept;

        PipelineCache* m_pipelineCache = nullptr;
        IRHIDevice* m_device = nullptr;
        RHIBufferRef m_bootstrapState;
        RHIBufferRef m_stateBuffers[2];
        RHIBufferRef m_histogramBuffer;
        RHIBufferRef m_probeReadback;
        uint32 m_committedStateIndex = RVX_INVALID_INDEX;
        uint32 m_pendingStateIndex = RVX_INVALID_INDEX;
        uint64 m_committedConfigurationHash = 0;
        uint64 m_committedStateContentHash = 0;
        uint64 m_pendingConfigurationHash = 0;
        uint64 m_pendingStateContentHash = 0;
        ExposureFrameInput m_preparedFrameInput{};
        RenderViewHistoryIdentity m_preparedHistoryIdentity{};
        bool m_hasPreparedFrameInput = false;
        bool m_hasPendingCandidate = false;
        RenderViewHistoryIdentity m_pendingIdentity{};
        bool m_probeRequested = false;
        bool m_probeScheduled = false;
        bool m_probeRecorded = false;
        RenderViewHistoryIdentity m_probeIdentity{};
        uint64 m_probeConfigurationHash = 0;
        uint64 m_probeCandidateStateContentHash = 0;
        RenderExposureMode m_probeMode =
            static_cast<RenderExposureMode>(255);
        float32 m_probeExposureCompensationEV = 0.0f;
        ExposureControllerStats m_lastStats{};
        std::shared_ptr<FrameRecord> m_lastFrameRecord;
    };
} // namespace RVX
