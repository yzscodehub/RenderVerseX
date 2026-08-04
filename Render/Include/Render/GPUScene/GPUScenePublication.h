#pragma once

/**
 * @file GPUScenePublication.h
 * @brief Read-only status of the non-executable GPU-scene CPU shadow.
 */

#include "Core/Types.h"

namespace RVX
{
    /** @brief Why an accepted scene could not be mirrored completely. */
    enum class GPUScenePublicationFailureReason : uint8
    {
        None = 0,
        RegistryUnavailable,
        InvalidObject,
        ResourceUnavailable,
        ResourceResolutionFailed,
        DependencyUnavailable,
        DependencyCycle,
        DatabaseCommitFailed,
        AllocationFailed,
        UnexpectedFailure,
    };

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

    /** @brief Non-authoritative GPU-scene upload planner failure reason. */
    enum class GPUSceneUploadFailureReason : uint8
    {
        None = 0,
        NotInitialized,
        ContinuityLost,
        BufferCreationFailed,
        StagingCreationFailed,
        StagingMapFailed,
        SubmissionRetentionFailed,
        InvalidCompletionToken,
        DeviceLost,
        UnexpectedFailure,
    };

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
        uint32 bufferSetCount = 0;
        uint32 frameUploadRangeCount = 0;
        uint32 pendingSetCount = 0;
        GPUSceneUploadFailureReason failureReason = GPUSceneUploadFailureReason::None;
        bool fullUpload = false;
        bool continuityLost = false;
        bool deviceLost = false;
        bool rollbackPending = false;
        bool executionEligible = false;
    };

    static_assert(static_cast<uint8>(GPUSceneUploadFailureReason::None) == 0);
} // namespace RVX
