#pragma once

/**
 * @file ResourceDiagnostics.h
 * @brief Copyable resource residency and staged-work diagnostics.
 */

#include "Core/Diagnostics/DiagnosticValue.h"
#include "Core/Types.h"

namespace RVX::Resource
{
    template<typename T>
    using DiagnosticValue = RVX::DiagnosticValue<T>;

    /**
     * @brief Immutable-by-value observation of Resource's CPU/stream/render handoff state.
     *
     * The ResourceManager fills CPU/cache/load/stream/lease fields. The
     * ResourceSubsystem supplements pending gateway work on the update thread.
     */
    struct ResourceDiagnosticsSnapshot
    {
        uint64 activeOperations = 0;
        uint64 activeSubscribers = 0;
        uint64 pendingAsyncJobs = 0;
        uint64 pendingAsyncCompletions = 0;

        uint64 cacheEntryCount = 0;
        uint64 cacheCPUBytes = 0;
        uint64 cacheGPUBytes = 0;
        uint64 cacheHits = 0;
        uint64 cacheMisses = 0;

        // Resource loaders do not yet report actual source reads. These
        // deliberately remain unavailable instead of treating worker entry
        // or filesystem metadata as measured I/O.
        DiagnosticValue<uint64> sourceReadOperationCount =
            DiagnosticValue<uint64>::Unavailable(
                "Resource loaders do not expose actual source-read instrumentation.");
        DiagnosticValue<uint64> sourceReadBytes =
            DiagnosticValue<uint64>::Unavailable(
                "Resource loaders do not expose actual source-read byte instrumentation.");
        uint64 cancelledLoadCount = 0;

        uint64 decodeQueuedCount = 0;
        uint64 decodeActiveCount = 0;
        uint64 decodeCompletedCount = 0;
        uint64 decodeFailedCount = 0;
        uint64 decodeCancelledCount = 0;
        uint64 decodeCompletedBytes = 0;
        uint64 decodeReservedBytes = 0;
        uint64 decodePeakReservedBytes = 0;
        uint64 decodeBudgetBytes = 0;

        uint64 pendingPublicationCount = 0;
        uint64 pendingUploadCount = 0;
        uint64 pendingReplacementCount = 0;
        uint64 pendingRollbackCount = 0;
        uint64 pendingRetirementCount = 0;

        uint64 activeLeaseCount = 0;
        uint64 protectedResourceCount = 0;
        uint64 queuedLeaseUnloadCount = 0;
        uint64 leaseBlockedEvictionCount = 0;

        // Exact AssetKey unloads release an ownership closure rather than
        // merely dropping the requested root. These counters distinguish
        // completed owner releases from nodes deliberately retained because a
        // different published root or runtime resource still depends on them.
        uint64 closureUnloadRequestCount = 0;
        uint64 closureUnloadedResourceCount = 0;
        uint64 closureRetainedResourceCount = 0;
        uint64 closureUnloadRejectedCount = 0;
    };
} // namespace RVX::Resource

namespace RVX
{
    using Resource::ResourceDiagnosticsSnapshot;
} // namespace RVX
