#pragma once

/**
 * @file RenderUploadWorkDiagnostics.h
 * @brief Value-only receipt evidence for one render upload domain.
 */

#include "Core/Diagnostics/DiagnosticValue.h"
#include "Core/Types.h"

#include <array>

namespace RVX
{
    /**
     * @brief Backend-neutral projection of the host-visibility scope reported
     * by an RHI mapped-write receipt.
     *
     * The numeric order intentionally mirrors RHIHostWriteSynchronization so
     * producers can preserve every receipt scope without exposing RHI objects
     * through diagnostics snapshots.
     */
    enum class RenderHostWriteSynchronizationScope : uint8
    {
        Unavailable = 0,
        CoherentNoExplicitSync,
        ExactRange,
        AtomAlignedRange,
        WholeResource,
        WholeAllocation,
        Count,
    };

    inline constexpr uint32 RVX_RENDER_HOST_WRITE_SYNCHRONIZATION_SCOPE_COUNT =
        static_cast<uint32>(RenderHostWriteSynchronizationScope::Count);

    /**
     * @brief Actual work and receipt evidence for an independent upload stream.
     *
     * CPU copy and commit counters record work, not residency. A consumer must
     * use its owning stream's generation/access contract before treating the
     * bytes as a valid graph input. `hostVisibilitySynchronizedBytes` remains
     * unavailable whenever any committed range lacks quantitative visibility
     * evidence, which is distinct from a measured zero-byte synchronization.
     */
    struct RenderUploadWorkDiagnostics
    {
        uint64 cpuCopiedPayloadBytes = 0;
        uint64 committedPayloadBytes = 0;
        uint32 mappedRangeCount = 0;
        uint32 committedRangeCount = 0;
        DiagnosticValue<uint64> hostVisibilitySynchronizedBytes =
            DiagnosticValue<uint64>::Unavailable(
                "No committed mapped host-write receipts were recorded");
        std::array<uint64,
                   RVX_RENDER_HOST_WRITE_SYNCHRONIZATION_SCOPE_COUNT>
            hostVisibilitySynchronizationScopeRangeCounts{};
        std::array<uint64,
                   RVX_RENDER_HOST_WRITE_SYNCHRONIZATION_SCOPE_COUNT>
            hostVisibilitySynchronizationScopeBytes{};
        uint64 gpuCopyBytes = 0;
        uint32 gpuCopyRangeCount = 0;
    };
} // namespace RVX
