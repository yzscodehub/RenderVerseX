#pragma once

/**
 * @file RenderDiagnostics.h
 * @brief Immutable value-only render runtime diagnostics contract.
 */

#include "Render/RenderRuntimeTypes.h"

#include <array>
#include <string>

namespace RVX
{
    inline constexpr uint8 RVX_RENDER_DIAGNOSTICS_UNAVAILABLE_INDEX = 0xFFU;
    inline constexpr uint32 RVX_RENDER_DIAGNOSTICS_TRANSITION_CAPACITY = 8U;

    struct RenderLifecycleTransitionDiagnostics
    {
        RenderLifecycleState from = RenderLifecycleState::Stopped;
        RenderLifecycleState to = RenderLifecycleState::Stopped;
        uint64 sequence = 0;
        std::string message{};
    };

    struct RenderFrameTransportDiagnostics
    {
        uint32 capacity = 0;
        uint32 currentUsage = 0;
        uint32 highWaterMark = 0;
        uint64 replacements = 0;
        uint64 invalidPackets = 0;
        uint64 outOfOrderRejections = 0;
    };

    struct RenderUploadTransportDiagnostics
    {
        uint32 requestCapacity = 0;
        uint64 byteCapacity = 0;
        uint32 currentRequestUsage = 0;
        uint64 currentByteUsage = 0;
        uint32 requestHighWaterMark = 0;
        uint64 byteHighWaterMark = 0;
        uint64 pressureOutcomes = 0;
        uint64 acceptedCount = 0;
        uint64 completedCount = 0;
        uint64 failedCount = 0;
    };

    struct RenderReleaseTransportDiagnostics
    {
        uint32 capacity = 0;
        uint32 currentUsage = 0;
        uint32 highWaterMark = 0;
        uint64 acceptedCount = 0;
        uint64 completedCount = 0;
        uint64 staleCount = 0;
        uint32 oldestPendingGeneration = 0;
    };

    struct RenderResourceDiagnostics
    {
        std::array<uint64, 7> stateCounts{};
        uint64 staleGenerationAttempts = 0;
        uint64 fallbackCount = 0;
        uint64 skippedDrawCount = 0;
    };

    struct RenderQueueTimelineDiagnostics
    {
        bool available = false;
        uint8 logicalQueue = RVX_RENDER_DIAGNOSTICS_UNAVAILABLE_INDEX;
        uint8 physicalDomain = RVX_RENDER_DIAGNOSTICS_UNAVAILABLE_INDEX;
        uint8 completionMode = RVX_RENDER_DIAGNOSTICS_UNAVAILABLE_INDEX;
        uint8 timelineState = RVX_RENDER_DIAGNOSTICS_UNAVAILABLE_INDEX;
        uint64 lastSubmittedValue = 0;
        uint64 lastCompletedValue = 0;
    };

    struct RenderRetirementDiagnostics
    {
        uint32 entryCount = 0;
        uint64 estimatedBytes = 0;
        std::array<uint64, 3> oldestPendingCompletionValues{};
    };

    struct RenderFailureDiagnostics
    {
        bool available = false;
        RenderRuntimeResult runtime{};
        RenderShutdownResult shutdown{};
        std::string context{};
    };

    struct RenderDiagnosticsSnapshot
    {
        uint64 publicationSequence = 0;
        RenderExecutorKind executor = RenderExecutorKind::None;
        RenderLifecycleState lifecycle = RenderLifecycleState::Stopped;
        uint64 renderThreadIdentityHash = 0;
        uint64 transitionCount = 0;
        RenderLifecycleTransitionDiagnostics lastTransition{};
        std::array<RenderLifecycleTransitionDiagnostics,
                   RVX_RENDER_DIAGNOSTICS_TRANSITION_CAPACITY>
            transitions{};

        RHIBackendType backend = RHIBackendType::None;
        uint64 surfaceGeneration = 0;
        uint32 surfaceWidth = 0;
        uint32 surfaceHeight = 0;
        uint64 resizeAcceptedCount = 0;
        uint64 resizeCoalescedCount = 0;
        uint64 resizeRejectedCount = 0;

        uint64 lastPublishedFrameSequence = 0;
        uint64 lastAcquiredFrameSequence = 0;
        uint64 lastAppliedFrameSequence = 0;
        uint64 lastSubmittedFrameSequence = 0;
        uint64 lastPresentedFrameSequence = 0;
        RenderFrameTransportDiagnostics frameTransport{};
        RenderUploadTransportDiagnostics uploadTransport{};
        RenderReleaseTransportDiagnostics releaseTransport{};
        RenderResourceDiagnostics resources{};
        std::array<RenderQueueTimelineDiagnostics, 3> queues{};
        RenderRetirementDiagnostics retirement{};
        RenderFailureDiagnostics lastFailure{};

        uint64 pumpIterationCount = 0;
        uint64 idleWaitCount = 0;
    };
} // namespace RVX
