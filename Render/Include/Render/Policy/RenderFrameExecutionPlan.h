#pragma once

/**
 * @file RenderFrameExecutionPlan.h
 * @brief Value-only render-frame policy plans and execution reports.
 */

#include "Render/Passes/MeshPassProcessor.h"
#include "Render/Policy/RenderPolicyTypes.h"
#include "RenderContracts/RenderIdentity.h"

#include <array>
#include <compare>
#include <vector>

namespace RVX
{
    /** @brief Collision-safe identity for one packet in a compiled frame/view. */
    struct RenderDrawPacketId
    {
        uint64 frameSequence = 0;
        uint32 viewOrdinal = 0;
        RenderPassKind pass = RenderPassKind::None;
        uint64 objectId = 0;
        uint32 primitiveData = 0;
        RenderResourceHandle mesh{};
        uint32 logicalSubmeshIndex = 0;
        uint32 geometrySubmeshIndex = 0;
        uint32 sourcePacketIndex = 0;
        uint32 sourceOrdinal = 0;

        auto operator<=>(const RenderDrawPacketId&) const = default;
    };

    /** @brief Exact prepared values guarded against post-plan source drift. */
    struct RenderPreparedDrawPacketSignature
    {
        MeshPassDisposition disposition = MeshPassDisposition::Skip;
        MeshPassEligibilityReason reason =
            MeshPassEligibilityReason::PassIrrelevant;
        RenderDrawPacket packet{};
        RenderDrawGroupKey groupKey{};
        RenderSubmissionLayout directLayout{};
        uint32 sourceOrdinal = 0;
        float32 viewDepth = 0.0f;

        bool operator==(
            const RenderPreparedDrawPacketSignature&) const = default;
    };

    /** @brief Stable, owned source reference selected for a frame-local pass. */
    struct RenderDrawPacketReference
    {
        RenderPassKind pass = RenderPassKind::None;
        uint32 sourcePacketIndex = 0;
        uint32 sourceOrdinal = 0;
        RenderDrawPacketId packetId{};
        RenderPreparedDrawPacketSignature sourceSignature{};

        bool operator==(const RenderDrawPacketReference&) const = default;
    };

    /** @brief Exactly-once terminal-lane accounting for one compiled pass. */
    struct RenderPacketIdentityAccounting
    {
        uint32 expectedPacketCount = 0;
        uint32 terminalPacketCount = 0;
        uint32 uniquePacketIdCount = 0;
        uint32 duplicatePacketIdCount = 0;
        uint32 unaccountedPacketIdCount = 0;

        [[nodiscard]] bool IsExactlyOnce() const noexcept
        {
            return terminalPacketCount == expectedPacketCount &&
                   uniquePacketIdCount == expectedPacketCount &&
                   duplicatePacketIdCount == 0 &&
                   unaccountedPacketIdCount == 0;
        }

        bool operator==(const RenderPacketIdentityAccounting&) const = default;
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
        RenderPacketIdentityAccounting identityAccounting{};
        RenderPolicyReason reason = RenderPolicyReason::ConservativeDefault;
        /// One deterministic outcome reason for every source packet, including
        /// pass-irrelevant packets that enter the Skip lane.
        std::array<uint32, static_cast<size_t>(RenderPolicyReason::Count)>
            reasonCounts{};

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
        bool executedCountsAvailable = false;
        /// CPU record time for this lane's bindings and submission calls.
        uint64 submissionCpuNanoseconds = 0;
        bool submissionCpuTimingAvailable = false;

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
        /// Whole-view tier actually recorded after pre-graph confirmation.
        GPUDrivenTier executedTier = GPUDrivenTier::Direct;
        /// Non-None only for an explicit pre-graph whole-view fallback.
        RenderPolicyReason tierFallbackReason = RenderPolicyReason::None;
        std::vector<RenderPassExecutionReport> passes{};

        bool operator==(const RenderFrameExecutionReport&) const = default;
    };
} // namespace RVX
