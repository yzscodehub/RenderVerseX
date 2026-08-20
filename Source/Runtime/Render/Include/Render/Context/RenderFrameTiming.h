#pragma once

/**
 * @file RenderFrameTiming.h
 * @brief Completion-owned whole-frame Graphics timeline timing diagnostics.
 */

#include "Core/Diagnostics/DiagnosticValue.h"
#include "Core/Types.h"

namespace RVX
{
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
