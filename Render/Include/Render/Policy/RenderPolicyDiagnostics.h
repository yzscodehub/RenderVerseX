#pragma once

/**
 * @file RenderPolicyDiagnostics.h
 * @brief Value-only diagnostics projection for render policy selection.
 */

#include "Render/Policy/RenderFrameExecutionPlan.h"

namespace RVX
{
    /**
     * @brief Non-decision CPU-side observation of one selected frame policy.
     *
     * Counts are pass-packet aggregates. A source primitive may therefore be
     * represented once by Depth and again by Opaque; these values deliberately
     * do not claim to be unique scene-primitive counts.
     */
    struct RenderPolicyMeasurement
    {
        uint64 frameSequence = 0;
        uint64 planCpuNanoseconds = 0;
        uint64 submissionCpuNanoseconds = 0;
        uint64 candidatePacketCount = 0;
        uint64 drawGroupCount = 0;
        float64 averageGroupOccupancy = 0.0;
        bool planCpuTimingAvailable = false;
        bool submissionCpuTimingAvailable = false;
        bool averageGroupOccupancyAvailable = false;
        /// Measurements are informational only and must never gate a frame.
        bool nonGating = true;
        /// Preserves the explicit contract that Auto policy never reads this.
        bool usedForAutoDecision = false;
    };

    /** @brief Request, selected plan, and observed report for one frame policy. */
    struct RenderPolicyDiagnostics
    {
        bool requestAvailable = false;
        bool planAvailable = false;
        bool reportAvailable = false;
        RenderFramePolicyRequest request{};
        RenderFrameExecutionPlan selectedPlan{};
        RenderFrameExecutionReport executionReport{};
        RenderPolicyMeasurement measurement{};
    };
} // namespace RVX
