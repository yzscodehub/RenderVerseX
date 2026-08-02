#pragma once

/**
 * @file DirectDrawPacketBatch.h
 * @brief Value-only validation and materialization of a planned Direct lane.
 */

#include "Render/Passes/MeshPassProcessor.h"
#include "Render/Policy/RenderPolicyResolver.h"

#include <vector>

namespace RVX
{
    /** @brief One source packet copied into a Direct recording batch. */
    struct DirectDrawPacket
    {
        RenderDrawPacket packet{};
        RenderSubmissionLayout layout{};
        uint32 sourcePacketIndex = 0;
        uint32 sourceOrdinal = 0;

        bool operator==(const DirectDrawPacket&) const = default;
    };

    /** @brief Owned, backend-neutral Direct packet batch for one pass. */
    struct DirectDrawPacketBatch
    {
        RenderPassKind pass = RenderPassKind::None;
        std::vector<DirectDrawPacket> packets{};

        bool operator==(const DirectDrawPacketBatch&) const = default;
    };

    /** @brief Fail-closed status for Direct packet batch construction. */
    struct DirectDrawPacketBatchBuildResult
    {
        bool succeeded = false;
        RenderPolicyReason reason = RenderPolicyReason::InconsistentFacts;
        DirectDrawPacketBatch batch{};

        bool operator==(const DirectDrawPacketBatchBuildResult&) const = default;
    };

    /**
     * @brief Materialize the Direct range of a complete canonical frame plan.
     *
     * The preparation stream is borrowed only for the duration of this call;
     * the result owns packet values and retains no scene, RHI, or stream
     * pointers.  `sourceOrdinal` is copied and checked for consistency, but
     * source packet identity is always the explicit `sourcePacketIndex`.
     */
    [[nodiscard]] DirectDrawPacketBatchBuildResult BuildDirectDrawPacketBatch(
        const RenderFrameExecutionPlan& plan,
        RenderPassKind pass,
        const MeshPassPacketStream& stream);
} // namespace RVX
