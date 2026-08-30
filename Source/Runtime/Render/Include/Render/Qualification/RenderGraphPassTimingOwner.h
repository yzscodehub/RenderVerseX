#pragma once

/**
 * @file RenderGraphPassTimingOwner.h
 * @brief Completion-owned exact-Graphics per-pass timestamp qualification.
 */

#include "Render/Graph/RenderGraph.h"

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace RVX
{
    class IRHIDevice;
    class RenderSubmissionTracker;
    struct GPUCompletionToken;
    struct RenderQualityPerformanceSessionTestAccess;

    enum class RenderGraphPassTimingState : uint8
    {
        Unavailable = 0,
        Idle,
        Recording,
        AwaitingSubmission,
        PendingCompletion,
        Completed,
        Rejected,
    };

    enum class RenderGraphPassTimingFailure : uint8
    {
        None = 0,
        Unsupported,
        InvalidContext,
        InvalidPassCount,
        AlreadyActive,
        InvalidBoundary,
        PassNameMissing,
        PassOrderMismatch,
        MissingPass,
        QueryResolveFailed,
        QueryCommandRejected,
        MultiQueueSubmission,
        SubmissionRejected,
        CompletionLost,
        ReadbackMapFailed,
        TimestampWrapped,
        TimestampNonAdvancing,
    };

    [[nodiscard]] inline const char* GetRenderGraphPassTimingFailureName(
        RenderGraphPassTimingFailure failure) noexcept
    {
        switch (failure)
        {
            case RenderGraphPassTimingFailure::None: return "None";
            case RenderGraphPassTimingFailure::Unsupported: return "Unsupported";
            case RenderGraphPassTimingFailure::InvalidContext: return "InvalidContext";
            case RenderGraphPassTimingFailure::InvalidPassCount: return "InvalidPassCount";
            case RenderGraphPassTimingFailure::AlreadyActive: return "AlreadyActive";
            case RenderGraphPassTimingFailure::InvalidBoundary: return "InvalidBoundary";
            case RenderGraphPassTimingFailure::PassNameMissing: return "PassNameMissing";
            case RenderGraphPassTimingFailure::PassOrderMismatch: return "PassOrderMismatch";
            case RenderGraphPassTimingFailure::MissingPass: return "MissingPass";
            case RenderGraphPassTimingFailure::QueryResolveFailed: return "QueryResolveFailed";
            case RenderGraphPassTimingFailure::QueryCommandRejected: return "QueryCommandRejected";
            case RenderGraphPassTimingFailure::MultiQueueSubmission: return "MultiQueueSubmission";
            case RenderGraphPassTimingFailure::SubmissionRejected: return "SubmissionRejected";
            case RenderGraphPassTimingFailure::CompletionLost: return "CompletionLost";
            case RenderGraphPassTimingFailure::ReadbackMapFailed: return "ReadbackMapFailed";
            case RenderGraphPassTimingFailure::TimestampWrapped: return "TimestampWrapped";
            case RenderGraphPassTimingFailure::TimestampNonAdvancing: return "TimestampNonAdvancing";
        }
        return "Unknown";
    }

    /** @brief One stable completed per-pass GPU timestamp pair. */
    struct RenderGraphPassTimingSample
    {
        uint32 executionIndex = 0;
        std::string passName;
        uint64 startTimestamp = 0;
        uint64 endTimestamp = 0;
        uint64 elapsedTimestampTicks = 0;
        float64 durationMilliseconds = 0.0;
    };

    /** @brief Observable, fail-closed timing owner status. */
    struct RenderGraphPassTimingDiagnostics
    {
        RenderGraphPassTimingState state = RenderGraphPassTimingState::Unavailable;
        RenderGraphPassTimingFailure failure = RenderGraphPassTimingFailure::Unsupported;
        uint32 expectedPassCount = 0;
        uint32 recordedPassCount = 0;
        uint32 completedPassCount = 0;
        uint64 completionValue = 0;
        uint64 timestampFrequency = 0;
        uint8 timestampValidBits = 0;
    };

    /**
     * @brief Owns query/readback resources for one Graphics-only graph execution.
     *
     * Query results are mapped only after the exact submitted Graphics token is
     * observed complete through the same RenderSubmissionTracker. This owner
     * never waits, never calls WaitIdle, and rejects ambiguous multi-queue or
     * timestamp-wrap evidence.
     */
    class RenderGraphPassTimingOwner final
        : public IRenderGraphPassTimestampRecorder,
          public IRenderGraphExecutionTerminalObserver,
          public std::enable_shared_from_this<RenderGraphPassTimingOwner>
    {
    public:
        [[nodiscard]] bool Initialize(IRHIDevice* device, uint32 maxPassCount);
        void Shutdown(bool deviceLost) noexcept;

        [[nodiscard]] bool BeginExactGraphicsExecution(
            RHICommandContext& graphicsContext,
            uint32 expectedPassCount) noexcept override;
        [[nodiscard]] bool RecordPassTimestamp(
            RHICommandContext& graphicsContext,
            uint32 executionIndex,
            std::string_view passName,
            RenderGraphPassTimestampBoundary boundary) noexcept override;
        [[nodiscard]] bool FinalizeExactGraphicsExecution(
            RHICommandContext& graphicsContext) noexcept override;
        [[nodiscard]] std::shared_ptr<IRenderGraphExecutionTerminalObserver>
        SealExecutionTerminalObserver() noexcept override;
        void AbortExactGraphicsExecution(const char* reason) noexcept override;

        void OnRenderGraphSubmitted(
            const GPUCompletionToken& completion) noexcept override;
        void OnRenderGraphAborted() noexcept override;
        void OnRenderGraphDeviceLost() noexcept override;
        void OnRenderGraphRetired() noexcept override;

        /** @brief Non-blocking completion poll; maps only an already-completed readback. */
        [[nodiscard]] bool PollCompletion(
            const RenderSubmissionTracker& tracker) noexcept;

        /** @brief Clear a completed or rejected sample before arming another graph. */
        void ResetCompletedTiming() noexcept;

        [[nodiscard]] bool IsEnabled() const noexcept { return m_enabled; }
        [[nodiscard]] const RenderGraphPassTimingDiagnostics&
        GetDiagnostics() const noexcept
        {
            return m_diagnostics;
        }
        [[nodiscard]] std::span<const RenderGraphPassTimingSample>
        GetCompletedSamples() const noexcept
        {
            return m_samples;
        }

    private:
        friend struct RenderQualityPerformanceSessionTestAccess;

        [[nodiscard]] bool Reject(RenderGraphPassTimingFailure failure) noexcept;
        [[nodiscard]] bool CompleteReadback() noexcept;
        void ClearActiveRecording() noexcept;

        IRHIDevice* m_device = nullptr;
        // Both objects remain owner-snapshotted through the terminal observer
        // until the exact Graphics completion token closes this timing sample.
        RHIQueryPoolRef m_queryPool;
        RHIBufferRef m_readbackBuffer;
        const RenderSubmissionTracker* m_submissionTracker = nullptr;
        std::vector<RenderGraphPassTimingSample> m_samples;
        uint32 m_maxPassCount = 0;
        uint32 m_openPassIndex = RVX_INVALID_INDEX;
        uint64 m_completionValue = 0;
        uint64 m_timestampFrequency = 0;
        uint8 m_timestampValidBits = 0;
        bool m_terminalObserverSealed = false;
        bool m_enabled = false;
        RenderGraphPassTimingDiagnostics m_diagnostics;
    };
} // namespace RVX
