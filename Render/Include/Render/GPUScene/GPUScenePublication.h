#pragma once

/**
 * @file GPUScenePublication.h
 * @brief Read-only status of the non-executable GPU-scene CPU shadow.
 */

#include "Core/Types.h"
#include "Render/GPUScene/GPUSceneDiagnostics.h"

namespace RVX
{
    /**
     * @brief Value-only diagnostics for the most recent shadow publication attempt.
     *
     * A successful full mirror is still non-executable in Task 11B.  The
     * counters describe accepted RenderScene data and its independently
     * committed CPU shadow only; they expose neither RHI resources nor the
     * private GPUSceneDatabase.
     */
    struct GPUScenePublicationStats
    {
        /** @brief True only for the publication attempt represented by this snapshot. */
        bool attempted = false;
        /** @brief Sequence observed for this attempted publication. */
        uint64 sourceSequence = 0;
        /** @brief Actual persistent CPU-shadow version, never a candidate version. */
        uint64 committedVersion = 0;
        /** @brief Source sequence that produced the currently committed shadow. */
        uint64 committedSourceSequence = 0;
        /** @brief All accepted RenderScene objects/draws examined by this attempt. */
        uint32 attemptedObjectCount = 0;
        uint32 attemptedDrawCount = 0;
        /** @brief Valid candidates constructed by this attempt before diffing. */
        uint32 candidateObjectCount = 0;
        uint32 candidateDrawCount = 0;
        /** @brief Backward-compatible names for the attempt input counts. */
        uint32 acceptedObjectCount = 0;
        uint32 acceptedDrawCount = 0;
        /** @brief Actual counts in the committed database mirror. */
        uint32 publishedObjectCount = 0;
        uint32 excludedObjectCount = 0;
        uint32 publishedDrawCount = 0;
        uint32 excludedDrawCount = 0;
        uint32 addCount = 0;
        uint32 updateCount = 0;
        uint32 removeCount = 0;
        uint32 noOpCount = 0;
        GPUScenePublicationFailureReason failureReason =
            GPUScenePublicationFailureReason::None;
        bool complete = false;
        bool executionEligible = false;
    };

    static_assert(static_cast<uint8>(GPUScenePublicationFailureReason::None) == 0);

    /**
     * @brief Value-only diagnostics for the renderer-private GPU-scene uploader.
     *
     * Task 11C still does not bind these buffers or alter rendering policy. The
     * counters therefore describe upload planning and lifetime only.
     */
    struct GPUSceneUploadDiagnostics
    {
        uint64 observedVersion = 0;
        uint64 residentVersion = 0;
        uint64 safeReclaimVersion = 0;
        uint64 persistentBytes = 0;
        uint64 frameUploadBytes = 0;
        uint64 cumulativeUploadBytes = 0;
        uint64 peakFrameUploadBytes = 0;
        uint64 cumulativeUploadRangeCount = 0;
        uint64 cpuPayloadBytes = 0;
        uint64 cpuReservedBytes = 0;
        uint64 gpuAllocationBytes = 0;
        uint32 bufferSetCount = 0;
        uint32 peakBufferSetCount = 0;
        uint32 frameUploadRangeCount = 0;
        uint32 pendingSetCount = 0;
        uint32 inFlightSetCount = 0;
        uint32 unusableSetCount = 0;
        std::array<GPUSceneTableDiagnostics,
                   GPU_SCENE_DIAGNOSTICS_TABLE_COUNT> tables{};
        GPUSceneUploadFailureReason failureReason = GPUSceneUploadFailureReason::None;
        bool fullUpload = false;
        bool continuityLost = false;
        bool deviceLost = false;
        bool rollbackPending = false;
        bool executionEligible = false;
    };

    static_assert(static_cast<uint8>(GPUSceneUploadFailureReason::None) == 0);
} // namespace RVX
