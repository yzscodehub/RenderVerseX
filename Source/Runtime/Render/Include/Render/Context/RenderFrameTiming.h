#pragma once

/**
 * @file RenderFrameTiming.h
 * @brief Completion-owned whole-frame Graphics timeline timing diagnostics.
 */

#include "Core/Diagnostics/DiagnosticValue.h"
#include "Core/Types.h"

#include <array>

namespace RVX
{
    /**
     * @brief Maximum number of completed source-bound timing samples retained
     *        before their consumer drains them.
     *
     * The queue intentionally exceeds the number of in-flight frame slots so
     * a polling pass can retire every slot even when a consumer has not yet
     * observed the previous polling pass. It is a bounded, completion-owned
     * hand-off: it never replaces an older sample with a newer one.
     */
    inline constexpr uint32 RVX_RENDER_GPU_FRAME_TIMING_SAMPLE_QUEUE_CAPACITY =
        RVX_MAX_FRAME_COUNT * 2u;

    /**
     * @brief One source-bound Graphics terminal command-span sample.
     *
     * This record is produced only after the exact Graphics completion value
     * has completed and its readback pair has been mapped. It is deliberately
     * separate from the latest diagnostic snapshot: callers which need every
     * completed frame must consume this queue rather than polling a snapshot.
     */
    struct RenderGpuFrameTimingSample
    {
        uint64 sourceFrameSequence = 0;
        uint64 completionValue = 0;
        uint64 startTimestamp = 0;
        uint64 endTimestamp = 0;
        uint64 elapsedTimestampTicks = 0;
        uint64 timestampFrequency = 0;
        uint32 timestampValidBits = 0;
        float64 terminalCriticalPathMilliseconds = 0.0;
    };

    /**
     * @brief All source-bound timing samples made consumable by prior polls.
     *
     * `count == 0` is the fail-closed empty result. A successful drain always
     * contains a complete, ordered prefix of completion-owned samples; there
     * is no partial dequeue and no overwrite behaviour.
     */
    struct RenderGpuFrameTimingSampleBatch
    {
        std::array<RenderGpuFrameTimingSample,
                   RVX_RENDER_GPU_FRAME_TIMING_SAMPLE_QUEUE_CAPACITY>
            samples{};
        uint32 count = 0;
    };

    /**
     * @brief Last completion-owned Graphics frame-timing sample.
     *
     * The measured span is the Graphics timeline from the frame prelude to the
     * terminal Graphics gateway. It is deliberately not a sum of busy time on
     * multiple queues and must never participate in automatic path selection.
     */
    struct RenderGpuFrameTimingDiagnostics
    {
        /** @brief Elapsed prelude-to-terminal-Gateway time, or why no sample exists. */
        DiagnosticValue<float64> terminalCriticalPathMilliseconds =
            DiagnosticValue<float64>::Unavailable(
                "No completion-owned Graphics frame timing sample is available.");

        /** @brief Frame sequence explicitly bound to the published sample. */
        uint64 sourceFrameSequence = 0;
        /** @brief Exact Graphics completion value that made this sample readable. */
        uint64 completionValue = 0;
        /** @brief Raw timestamp query values retained for audit and conversion. */
        uint64 startTimestamp = 0;
        uint64 endTimestamp = 0;
        uint64 elapsedTimestampTicks = 0;
        uint64 timestampFrequency = 0;
        uint32 timestampValidBits = 0;

        /**
         * @brief Lifetime counters for completed, lost, and discarded samples.
         *
         * `completedSampleCount` counts every timestamp pair successfully
         * read after completion, including unbound samples silently drained
         * without publishing a source-frame metric.
         */
        uint64 completedSampleCount = 0;
        uint64 lostSampleCount = 0;
        uint64 droppedSampleCount = 0;

        /** @brief This diagnostic is observational only; policy must not consume it. */
        bool usedForAutoDecision = false;
    };
} // namespace RVX
