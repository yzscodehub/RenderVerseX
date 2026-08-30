#pragma once

/**
 * @file RenderQualityPerformanceSession.h
 * @brief Render-owned, completion-qualified 60 + 300 performance session.
 */

#include "Render/Context/RenderFrameTiming.h"
#include "Render/Features/RenderFeatureTypes.h"
#include "Render/Qualification/RenderGraphPassTimingOwner.h"
#include "RenderContracts/RenderQualityPerformance.h"

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace RVX
{
    class IRHIDevice;
    class RenderSubmissionTracker;
    struct GPUCompletionToken;
    struct RenderQualityPerformanceSessionTestAccess;

    /**
     * @brief Coordinates a fixed ring of exact RenderGraph timestamp owners.
     *
     * Every sampled frame owns one ring slot until its exact Graphics terminal
     * token completes. The session never reuses a pending slot and never drops
     * a frame: saturation, query failure, receipt mismatch, presentation loss,
     * or pass-schema drift rejects the complete session.
     */
    class RenderQualityPerformanceSession final
    {
    public:
        RenderQualityPerformanceSession();
        ~RenderQualityPerformanceSession();

        RenderQualityPerformanceSession(
            const RenderQualityPerformanceSession&) = delete;
        RenderQualityPerformanceSession& operator=(
            const RenderQualityPerformanceSession&) = delete;

        /**
         * @brief Arm exactly one fixed 60-warmup + 300-measurement session.
         *
         * The caller freezes hardware capability provenance and the upload
         * counter before the first sampled frame. This prevents the first
         * warmup frame from inheriting pre-session upload work.
         */
        [[nodiscard]] bool Arm(
            const RenderQualityPerformanceHardwareIdentity& hardware,
            uint64 initialCumulativeTrackedUploadBytes) noexcept;

        /** @brief True while the next accepted frame must be recorded Graphics-only. */
        [[nodiscard]] bool RequiresExactGraphicsFrame() const noexcept;

        /**
         * @brief Reserve one preallocated timing slot for the accepted frame.
         * @return Recorder injected into the exact RenderGraph execution, or
         * null after fail-closed rejection.
         */
        [[nodiscard]] IRenderGraphPassTimestampRecorder* PrepareFrame(
            IRHIDevice* device,
            uint32 expectedPassCount,
            const RenderFrameIdentity& frameIdentity,
            const RenderViewIdentity& viewIdentity) noexcept;

        /** @brief Bind the exact graph-accepted receipt before submission. */
        [[nodiscard]] bool BindExecutionReceipt(
            const RenderExecutionReceipt& receipt) noexcept;

        /** @brief Bind the final presentation-qualified receipt. */
        [[nodiscard]] bool MarkFramePresented(
            const RenderExecutionReceipt& receipt,
            const RenderQualityPerformanceFrameTelemetry& telemetry) noexcept;

        /** @brief True only when this presentation must capture same-frame telemetry. */
        [[nodiscard]] bool RequiresFrameTelemetry(
            const RenderFrameIdentity& frameIdentity) const noexcept;

        /** @brief Bind real CPU timing measured for the same accepted frame. */
        [[nodiscard]] bool RecordCpuTiming(
            const RenderFrameIdentity& frameIdentity,
            const RenderQualityPerformanceCpuTiming& timing) noexcept;

        /** @brief Freeze the one Graphics completion value for the recording slot. */
        [[nodiscard]] bool NotifyGraphicsSubmission(
            const GPUCompletionToken& completion) noexcept;

        /**
         * @brief Publish a terminal admission failure before a frame can arm.
         *
         * RenderThreadRuntime dispatches requests asynchronously.  The render
         * owner uses this when it cannot produce the required hardware or
         * telemetry baseline so callers observe Rejected rather than an idle
         * session that can only expire at a watchdog.
         */
        void RejectAdmission(
            RenderQualityPerformanceFailure failure,
            const char* diagnostic) noexcept;

        /** @brief Reject the current unsubmitted recording without fabricating a sample. */
        void AbortCurrentFrame(
            RenderQualityPerformanceFailure failure,
            const char* diagnostic) noexcept;

        /** @brief Non-blocking poll of every completion-owned ring slot. */
        void PollCompletion(const RenderSubmissionTracker& tracker) noexcept;

        /**
         * @brief Bind source-frame Graphics terminal spans before pass readback.
         *
         * RenderContext owns the Graphics prelude-to-terminal query. Its
         * completed batch must be consumed before this session polls the RenderGraph
         * pass-query owners, so each slot can prove that its terminal span and
         * per-pass breakdown belong to the same source frame and completion
         * value.
         */
        [[nodiscard]] bool BindGraphicsTerminalCommandSpans(
            const RenderGpuFrameTimingSampleBatch& samples) noexcept;

        /** @brief Mark all nonterminal slots lost after a real device loss. */
        void NotifyDeviceLost() noexcept;

        /** @brief Release resources only after completion drain or device loss. */
        void Shutdown(bool deviceLost) noexcept;

        [[nodiscard]] bool HasPendingCompletion() const noexcept;

        [[nodiscard]] const RenderQualityPerformanceSessionDiagnostics&
        GetDiagnostics() const noexcept
        {
            return m_diagnostics;
        }

        [[nodiscard]] std::shared_ptr<
            const RenderQualityPerformanceEvidence>
        GetCompletedEvidence() const noexcept
        {
            return m_completedEvidence;
        }

    private:
        friend struct RenderQualityPerformanceSessionTestAccess;

        struct Slot
        {
            std::shared_ptr<RenderGraphPassTimingOwner> timing{};
            RenderFrameIdentity frameIdentity{};
            RenderViewIdentity viewIdentity{};
            /** Graph-accepted receipt; never published as final evidence. */
            RenderQualityPerformanceReceiptBinding executionBinding{};
            /** Presentation-qualified full receipt published with the sample. */
            RenderQualityPerformanceReceiptBinding recordingBinding{};
            RenderQualityPerformanceCpuTiming cpu{};
            RenderQualityPerformanceFrameTelemetry telemetry{};
            /** Session ordinal frozen after exact Present/submission succeeds. */
            uint64 submittedFrame = 0;
            /** Set only when this exact Graphics submission completes. */
            uint64 completedFrame = 0;
            /** Exact Graphics timeline value bound at graph submission. */
            uint64 graphicsCompletionValue = 0;
            uint64 trackedUploadBytes = 0;
            uint32 sessionOrdinal = 0;
            RenderGpuFrameTimingSample graphicsTerminalCommandSpan{};
            bool inUse = false;
            bool receiptBound = false;
            bool presented = false;
            bool cpuBound = false;
            bool telemetryBound = false;
            bool graphicsTerminalCommandSpanBound = false;
        };

        struct ConsumedGraphicsTerminalCommandSpan
        {
            uint64 sourceFrameSequence = 0;
            uint64 completionValue = 0;
        };

        [[nodiscard]] bool InitializeSlots(
            IRHIDevice* device,
            uint32 expectedPassCount) noexcept;
        [[nodiscard]] Slot* FindFreeSlot() noexcept;
        [[nodiscard]] Slot* FindSlot(
            const RenderFrameIdentity& frameIdentity) noexcept;
        [[nodiscard]] bool FinalizeCompletedSlot(Slot& slot) noexcept;
        [[nodiscard]] bool CommitCompletedSample(
            uint32 sessionOrdinal,
            const RenderQualityPerformanceReceiptBinding& binding,
            const RenderQualityPerformanceCpuTiming& cpu,
            uint64 submittedFrame,
            uint64 completedFrame,
            uint64 trackedUploadBytes,
            uint32 trackedUploadCoverageMask,
            const RenderQualityPerformanceTrackedEstimatedFootprint&
                trackedEstimatedFootprint,
            const RenderQualityPerformanceResourceCounts& resourceCounts,
            uint64 completionValue,
            uint64 timestampFrequency,
            const RenderGpuFrameTimingSample& graphicsTerminalCommandSpan,
            std::span<const RenderGraphPassTimingSample> timings) noexcept;
        [[nodiscard]] bool PublishCompletedEvidence() noexcept;
        [[nodiscard]] bool Reject(
            RenderQualityPerformanceFailure failure,
            const char* diagnostic) noexcept;
        void ReleaseSlot(Slot& slot) noexcept;
        void ReleaseTerminalSlots() noexcept;
        void RefreshInFlightDiagnostics() noexcept;

        std::array<Slot, RVX_RENDER_QUALITY_PERFORMANCE_SLOT_COUNT> m_slots{};
        std::array<
            bool,
            RVX_RENDER_QUALITY_PERFORMANCE_WARMUP_SAMPLE_COUNT +
                RVX_RENDER_QUALITY_PERFORMANCE_MEASURED_SAMPLE_COUNT>
            m_completedOrdinals{};
        std::array<
            ConsumedGraphicsTerminalCommandSpan,
            RVX_RENDER_QUALITY_PERFORMANCE_WARMUP_SAMPLE_COUNT +
                RVX_RENDER_QUALITY_PERFORMANCE_MEASURED_SAMPLE_COUNT>
            m_consumedGraphicsTerminalCommandSpans{};
        std::vector<RenderQualityPerformanceSample> m_measuredSamples{};
        std::shared_ptr<const RenderQualityPerformanceEvidence>
            m_completedEvidence{};
        Slot* m_recordingSlot = nullptr;
        uint32 m_expectedPassCount = 0;
        uint32 m_scheduledFrameCount = 0;
        uint32 m_completedFrameCount = 0;
        uint64 m_sessionSurfaceGeneration = 0;
        uint64 m_sessionConfigurationRevision = 0;
        RenderViewIdentity m_sessionViewIdentity{};
        uint32 m_sessionReceiptSchemaVersion = 0;
        RenderQualityPerformanceHardwareIdentity m_hardware{};
        RenderQualityPerformanceResourceCounts m_measurementResourceCounts{};
        RenderQualityPerformanceTrackedEstimatedFootprint
            m_measurementTrackedEstimatedFootprint{};
        uint64 m_lastCumulativeTrackedUploadBytes = 0;
        uint64 m_lastPresentedSubmissionFrame = 0;
        uint64 m_lastCompletedSubmissionFrame = 0;
        bool m_measurementResourceCountsCaptured = false;
        bool m_measurementTrackedEstimatedFootprintCaptured = false;
        std::string m_sessionSourceCommit{};
        std::string m_sessionPreset{};
        std::string m_sessionBackend{};
        RenderPipelineRecipe m_sessionRecipe =
            RenderPipelineRecipe::ForwardStandard;
        RenderQualityTier m_sessionQuality = RenderQualityTier::Medium;
        uint64 m_sessionProfileHash = 0;
        bool m_slotsInitialized = false;
        bool m_requestIssued = false;
        RenderQualityPerformanceSessionDiagnostics m_diagnostics{};
    };

    /** @brief Narrow synthetic access used by the hermetic session validation. */
    struct RenderQualityPerformanceSessionTestAccess
    {
        [[nodiscard]] static bool ArmSynthetic(
            RenderQualityPerformanceSession& session,
            uint64 initialCumulativeTrackedUploadBytes = 0) noexcept;
        [[nodiscard]] static bool CommitCompletedSample(
            RenderQualityPerformanceSession& session,
            uint32 sessionOrdinal,
            const RenderQualityPerformanceReceiptBinding& binding,
            const RenderQualityPerformanceCpuTiming& cpu,
            const RenderQualityPerformanceFrameTelemetry& telemetry,
            uint64 submittedFrame,
            uint64 completedFrame,
            uint64 completionValue,
            uint64 timestampFrequency,
            std::span<const RenderGraphPassTimingSample> timings) noexcept;
        [[nodiscard]] static bool CommitCompletedSampleWithGraphicsTerminal(
            RenderQualityPerformanceSession& session,
            uint32 sessionOrdinal,
            const RenderQualityPerformanceReceiptBinding& binding,
            const RenderQualityPerformanceCpuTiming& cpu,
            const RenderQualityPerformanceFrameTelemetry& telemetry,
            uint64 submittedFrame,
            uint64 completedFrame,
            uint64 completionValue,
            uint64 timestampFrequency,
            const RenderGpuFrameTimingSample& graphicsTerminalCommandSpan,
            std::span<const RenderGraphPassTimingSample> timings) noexcept;
        static void SaturateSlots(
            RenderQualityPerformanceSession& session) noexcept;
        static void StageSyntheticRecordingSlot(
            RenderQualityPerformanceSession& session) noexcept;
        static void StageSyntheticSubmittedSlot(
            RenderQualityPerformanceSession& session,
            uint64 frameSequence,
            uint64 completionValue) noexcept;
        static void StageSyntheticCompletedPassTimingSlot(
            RenderQualityPerformanceSession& session,
            uint64 frameSequence,
            uint64 completionValue) noexcept;
        static void PollSyntheticCompletion(
            RenderQualityPerformanceSession& session) noexcept;
        [[nodiscard]] static bool IsSyntheticSlotReusable(
            const RenderQualityPerformanceSession& session) noexcept;
        [[nodiscard]] static bool RejectSaturatedFrame(
            RenderQualityPerformanceSession& session) noexcept;
        [[nodiscard]] static bool PublishCompletedEvidence(
            RenderQualityPerformanceSession& session) noexcept;
    };
} // namespace RVX
