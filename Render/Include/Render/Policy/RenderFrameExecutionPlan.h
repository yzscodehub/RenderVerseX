#pragma once

/**
 * @file RenderFrameExecutionPlan.h
 * @brief Value-only render-frame policy plans and execution reports.
 */

#include "Render/Policy/RenderPolicyTypes.h"

#include <vector>

namespace RVX
{
    /** @brief Stable, owned source reference selected for a frame-local pass. */
    struct RenderDrawPacketReference
    {
        RenderPassKind pass = RenderPassKind::None;
        uint32 sourcePacketIndex = 0;
        uint32 sourceOrdinal = 0;

        bool operator==(const RenderDrawPacketReference&) const = default;
    };

    /** @brief Packet accounting for one pass before command recording. */
    struct RenderPacketPartitionSummary
    {
        uint32 inputPacketCount = 0;
        uint32 relevantPacketCount = 0;
        uint32 candidatePacketCount = 0;
        uint32 gpuDrivenPacketCount = 0;
        uint32 directPacketCount = 0;
        uint32 skippedPacketCount = 0;
        uint32 drawGroupCount = 0;

        bool operator==(const RenderPacketPartitionSummary&) const = default;
    };

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
        DrawPacketRange skippedPackets{};
        RenderPacketPartitionSummary partition{};
        RenderPolicyReason reason = RenderPolicyReason::ConservativeDefault;

        bool operator==(const RenderPassExecutionPlan&) const = default;
    };

    /** @brief Immutable value plan for a frame's selected execution path. */
    struct RenderFrameExecutionPlan
    {
        uint64 frameSequence = 0;
        uint32 viewOrdinal = 0;
        RenderViewPolicy viewPolicy{};
        /// All packet ranges in @ref passes index this frame-local vector.
        std::vector<RenderDrawPacketReference> packetReferences{};
        std::vector<RenderPassExecutionPlan> passes{};
        RenderCapabilitySnapshot capabilities{};
        RenderQualificationSnapshot qualification{};

        bool operator==(const RenderFrameExecutionPlan&) const = default;
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

        bool operator==(const RenderPassLaneExecutionReport&) const = default;
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

        bool operator==(const RenderPassExecutionReport&) const = default;
    };

    /** @brief Value-only execution record for one frame plan. */
    struct RenderFrameExecutionReport
    {
        uint64 frameSequence = 0;
        RenderExecutionStatus status = RenderExecutionStatus::NotAttempted;
        std::vector<RenderPassExecutionReport> passes{};

        bool operator==(const RenderFrameExecutionReport&) const = default;
    };
} // namespace RVX
