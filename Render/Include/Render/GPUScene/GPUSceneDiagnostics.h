#pragma once

/**
 * @file GPUSceneDiagnostics.h
 * @brief Backend-neutral, value-only GPU-scene observability contract.
 *
 * These values are deliberately descriptive.  They neither retain GPU
 * resources nor participate in render-policy, qualification, or Auto-mode
 * decisions.  The contract is suitable for a runtime diagnostics snapshot,
 * offline evidence, and validation only.
 */

#include "Core/Types.h"
#include "Render/RenderUploadWorkDiagnostics.h"

#include <array>

namespace RVX
{
    /** @brief Stable value-only reason a CPU shadow publication was incomplete. */
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

    /** @brief Stable value-only reason a resident-table upload could not proceed. */
    enum class GPUSceneUploadFailureReason : uint8
    {
        None = 0,
        NotInitialized,
        ContinuityLost,
        BufferCreationFailed,
        StagingCreationFailed,
        StagingMapFailed,
        StagingCommitFailed,
        SubmissionRetentionFailed,
        InvalidCompletionToken,
        DeviceLost,
        UnexpectedFailure,
    };

    enum class GPUSceneDiagnosticsTable : uint8
    {
        Primitives = 0,
        Bounds,
        Transforms,
        Materials,
        Geometries,
        Draws,
        Count,
    };

    inline constexpr uint32 GPU_SCENE_DIAGNOSTICS_TABLE_COUNT =
        static_cast<uint32>(GPUSceneDiagnosticsTable::Count);

    /** @brief Actual CPU/GPU byte and dirty-range values for one fixed table. */
    struct GPUSceneTableDiagnostics
    {
        uint32 payloadRowCount = 0;
        /** @brief CPU mirror vector capacity, including the sentinel row. */
        uint32 cpuRowCapacity = 0;
        /** @brief Capacity in the selected resident GPU buffer set. */
        uint32 residentCapacity = 0;
        uint32 stride = 0;
        uint64 cpuPayloadBytes = 0;
        uint64 cpuReservedBytes = 0;
        uint64 residentBytes = 0;
        uint64 frameUploadBytes = 0;
        uint64 cumulativeUploadBytes = 0;
        uint64 peakFrameUploadBytes = 0;
        uint32 frameUploadRangeCount = 0;
        uint64 cumulativeUploadRangeCount = 0;
        bool resident = false;
        bool fullUpload = false;
        RenderUploadWorkDiagnostics uploadWork{};
    };

    /** @brief Exact allocator lifecycle totals; all counts exclude sentinel slot zero. */
    struct GPUSceneSlotLifecycleDiagnostics
    {
        uint32 liveSlotCount = 0;
        uint32 freeSlotCount = 0;
        uint32 retiredSlotCount = 0;
        uint32 permanentlyRetiredSlotCount = 0;
        uint32 liveDrawBlockCount = 0;
        uint32 freeDrawBlockCount = 0;
        uint32 retiredDrawBlockCount = 0;
        uint64 reclaimedSlotCount = 0;
        uint64 reclaimedDrawBlockCount = 0;
        /** @brief Retired identities conservatively withheld pending a real completion watermark. */
        uint32 conservativeReusePressureCount = 0;
    };

    /** @brief Optional timing annotations.  They are never a correctness gate. */
    struct GPUSceneTimingDiagnostics
    {
        bool cpuPlanTimingAvailable = false;
        bool cpuSubmissionTimingAvailable = false;
        bool delayedGpuTimingAvailable = false;
        bool nonGating = true;
        bool usedForAutoDecision = false;
        float64 cpuPlanMilliseconds = 0.0;
        float64 cpuSubmissionMilliseconds = 0.0;
        float64 delayedGpuMilliseconds = 0.0;
    };

    /**
     * @brief Complete value-only GPU-scene snapshot for one render frame.
     *
     * `available` only means that the renderer owns a GPU-scene update path;
     * it does not indicate that GPU-driven execution was selected.
     */
    struct GPUSceneDiagnostics
    {
        bool available = false;
        bool informationalOnly = true;
        bool publicationAttempted = false;
        bool publicationCandidate = false;
        bool publicationPublished = false;
        bool publicationFailed = false;
        bool publicationComplete = false;
        GPUScenePublicationFailureReason publicationFailureReason =
            GPUScenePublicationFailureReason::None;
        GPUSceneUploadFailureReason uploadFailureReason =
            GPUSceneUploadFailureReason::None;
        uint32 attemptedObjectCount = 0;
        uint32 attemptedDrawCount = 0;
        uint32 candidateObjectCount = 0;
        uint32 candidateDrawCount = 0;
        uint32 publishedObjectCount = 0;
        uint32 publishedDrawCount = 0;
        uint32 addCount = 0;
        uint32 updateCount = 0;
        uint32 removeCount = 0;
        uint32 noOpCount = 0;
        uint64 committedVersion = 0;
        uint64 residentVersion = 0;
        uint64 safeReclaimVersion = 0;
        uint64 requiredResidentVersion = 0;
        uint64 leaseVersion = 0;
        uint64 cpuPayloadBytes = 0;
        uint64 cpuReservedBytes = 0;
        uint64 gpuAllocationBytes = 0;
        uint64 frameUploadBytes = 0;
        uint64 cumulativeUploadBytes = 0;
        uint64 peakFrameUploadBytes = 0;
        uint64 cumulativeUploadRangeCount = 0;
        uint32 frameUploadRangeCount = 0;
        uint32 currentBufferSetCount = 0;
        uint32 peakBufferSetCount = 0;
        uint32 pendingBufferSetCount = 0;
        uint32 inFlightBufferSetCount = 0;
        uint32 unusableBufferSetCount = 0;
        bool fullUpload = false;
        bool gpuCullingOwnedBytesAvailable = false;
        uint64 gpuCullingOwnedBytes = 0;
        RenderUploadWorkDiagnostics uploadWork{};
        std::array<GPUSceneTableDiagnostics,
                   GPU_SCENE_DIAGNOSTICS_TABLE_COUNT> tables{};
        GPUSceneSlotLifecycleDiagnostics slots{};
        GPUSceneTimingDiagnostics timing{};
    };
} // namespace RVX
