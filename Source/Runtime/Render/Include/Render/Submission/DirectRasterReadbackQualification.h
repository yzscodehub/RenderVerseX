#pragma once

/**
 * @file DirectRasterReadbackQualification.h
 * @brief Explicit post-fence evidence for the Direct Opaque raster inputs.
 */

#include "Render/RenderDiagnostics.h"
#include "Render/Submission/RasterInstanceStream.h"
#include "RHI/RHIQueueTopology.h"

#include <algorithm>
#include <span>
#include <vector>

namespace RVX
{
    class IRHIDevice;
    class RHICommandContext;
    class RenderSubmissionTracker;
    struct GPUCompletionToken;

    /** @brief Immutable provenance for one Direct Opaque graph recording. */
    struct DirectRasterReadbackRecordingIdentity
    {
        uint64 graphIdentity = 0;
        uint64 graphRecordingGeneration = 0;
        uint64 frameSequence = 0;
        uint64 recordEpoch = 0;
        uint32 sourceFrameSlot = RVX_INVALID_INDEX;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return graphIdentity != 0 && graphRecordingGeneration != 0 &&
                   frameSequence != 0 && recordEpoch != 0 &&
                   sourceFrameSlot < RVX_MAX_FRAME_COUNT;
        }

        bool operator==(const DirectRasterReadbackRecordingIdentity&) const =
            default;
    };

    /** @brief Draw facts required to rebuild the exact Direct raster transcript. */
    struct DirectRasterReadbackDraw
    {
        RenderDrawGroupKey key{};
        RenderDrawArguments arguments{};
        uint32 representedPacketCount = 0;
        bool instanced = false;
    };

    /**
     * @brief Purpose-specific one-shot Direct Opaque physical readback owner.
     *
     * Normal frames never allocate, copy, map, or retain CPU payload.  An
     * armed owner freezes the actual Direct lane after its material evidence
     * has been finalized, records two copies after the render pass, and maps
     * only after the exact graphics completion point has completed.
     */
    class DirectRasterReadbackQualification
    {
    public:
        [[nodiscard]] bool Arm() noexcept;
        [[nodiscard]] bool IsArmed() const noexcept { return m_armed; }

        /** Fail closed when the selected frame has no Direct Opaque lane. */
        void RejectNoDirectLane(
            const DirectRasterReadbackRecordingIdentity& identity) noexcept;

        /**
         * @brief Freeze physical Direct stream evidence and allocate readback.
         *
         * This is called only from the actual Opaque Direct lane, before its
         * DrawIndexed sequence. It has no authority to alter that sequence.
         */
        [[nodiscard]] bool Prepare(
            IRHIDevice& device,
            const DirectRasterReadbackRecordingIdentity& identity,
            const RasterInstanceStream& stream,
            const RasterInstanceStreamCache& cache,
            std::span<const DirectRasterReadbackDraw> draws,
            const RasterTranscriptDigest& cpuDirectReference);

        /** Record exactly the two post-render-pass physical buffer copies. */
        [[nodiscard]] bool RecordPostRenderCopy(
            RHICommandContext& ctx,
            const DirectRasterReadbackRecordingIdentity& identity);

        /** Transfer an exact post-recording Graphics completion point. */
        [[nodiscard]] bool NotifySubmission(
            const DirectRasterReadbackRecordingIdentity& identity,
            const GPUCompletionToken& completion,
            const RenderSubmissionTracker& tracker);

        /** Discard a recording which never reached submission. */
        void ReleaseUnsubmitted(
            const DirectRasterReadbackRecordingIdentity& identity) noexcept;

        /** Poll the accepted source completion without requiring a new frame. */
        [[nodiscard]] bool PollCompletion(
            const RenderSubmissionTracker& tracker);

        [[nodiscard]] bool HasPendingCompletion() const noexcept;

        [[nodiscard]] const DirectRasterReadbackQualificationDiagnostics&
            GetDiagnostics() const noexcept
        {
            return m_diagnostics;
        }

    private:
        struct Capture
        {
            DirectRasterReadbackRecordingIdentity identity{};
            RHIBufferRef sourceInstances{};
            RHIBufferRef sourceIndices{};
            RHIBufferRef instanceReadback{};
            RHIBufferRef indexReadback{};
            std::vector<GPUInstanceData> expectedInstances{};
            std::vector<uint32> expectedIndices{};
            std::vector<RasterInstanceStreamKey> residentKeys{};
            std::vector<uint64> residentSemanticIdentities{};
            std::vector<DirectRasterReadbackDraw> draws{};
            RasterTranscriptDigest expectedTranscript{};
            GPUCompletionPoint completionPoint{};
            uint64 cpuPayloadBytes = 0;
            bool copyRecorded = false;
            bool submissionAccepted = false;
            // A valid submission can still be unusable evidence (for example,
            // an identity or post-copy precondition rejection).  Keep its
            // resources alive until the exact fence closes, but never map it.
            bool compareEligible = true;
            bool closureOnly = false;

            [[nodiscard]] bool IsAllocated() const noexcept
            {
                return instanceReadback && indexReadback &&
                       !expectedInstances.empty() && !expectedIndices.empty();
            }
        };

        [[nodiscard]] bool CompareCompletedReadback();
        void SetFailure(
            DirectRasterReadbackQualificationMismatch mismatch) noexcept;

        bool m_armed = false;
        Capture m_recording{};
        Capture m_pending{};
        DirectRasterReadbackQualificationDiagnostics m_diagnostics{};
    };
} // namespace RVX
