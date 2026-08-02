#pragma once

/**
 * @file RenderFrameExecutionPlan.h
 * @brief Value-only render-frame policy plans and execution reports.
 */

#include "Render/Policy/RenderPolicyTypes.h"

#include <vector>

namespace RVX
{
    /** @brief Per-pass work selected before render-graph recording. */
    struct RenderPassExecutionPlan
    {
        RenderPassKind pass = RenderPassKind::None;
        RenderVisibilityMode visibility = RenderVisibilityMode::Cpu;
        RenderSubmissionMode preferredSubmission =
            RenderSubmissionMode::Direct;
        /// Graph-compile-time Direct/fallback partition submission; never a
        /// recording-time silent fallback.
        RenderSubmissionMode fallbackSubmission =
            RenderSubmissionMode::Direct;
        DrawPacketRange gpuEligiblePackets{};
        DrawPacketRange directPackets{};
        RenderPolicyReason reason = RenderPolicyReason::ConservativeDefault;
    };

    /** @brief Immutable value plan for a frame's selected execution path. */
    struct RenderFrameExecutionPlan
    {
        uint64 frameSequence = 0;
        RenderViewPolicy viewPolicy{};
        std::vector<RenderPassExecutionPlan> passes{};
        RenderCapabilitySnapshot capabilities{};
        RenderQualificationSnapshot qualification{};
    };

    /** @brief Execution result for one preplanned GPU or Direct packet lane. */
    struct RenderPassLaneExecutionReport
    {
        RenderExecutionStatus status = RenderExecutionStatus::NotAttempted;
        RenderSubmissionMode submission = RenderSubmissionMode::Direct;
        DrawPacketRange packetRange{};
        uint32 executedPacketCount = 0;
        uint32 executedDrawCount = 0;
        RenderPolicyReason reason = RenderPolicyReason::ConservativeDefault;
    };

    /** @brief Per-pass result recorded after submission is attempted. */
    struct RenderPassExecutionReport
    {
        RenderPassKind pass = RenderPassKind::None;
        RenderExecutionStatus status = RenderExecutionStatus::NotAttempted;
        RenderVisibilityMode executedVisibility = RenderVisibilityMode::Cpu;
        RenderPassLaneExecutionReport gpuDrivenLane{};
        RenderPassLaneExecutionReport directLane{};
        uint32 skippedPacketCount = 0;
        RenderPolicyReason reason = RenderPolicyReason::ConservativeDefault;
    };

    /** @brief Value-only execution record for one frame plan. */
    struct RenderFrameExecutionReport
    {
        uint64 frameSequence = 0;
        RenderExecutionStatus status = RenderExecutionStatus::NotAttempted;
        std::vector<RenderPassExecutionReport> passes{};
    };
} // namespace RVX
