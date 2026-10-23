#pragma once

/** @file RenderRetirementQueue.h @brief Render-thread GPU completion-token retirement */

#include "Core/RefCounted.h"
#include "Render/RenderDiagnostics.h"
#include "Resources/RenderSubmissionTracker.h"
#include "Runtime/RenderThreadGuard.h"

#include <vector>

namespace RVX
{
    /** @brief Strong ownership held until every GPU completion point is satisfied. */
    struct RenderRetirementEntry
    {
        GPUCompletionToken completion;
        Ref<RefCounted> object;
        uint64 estimatedBytes = 0;
    };

    /** @brief Render-thread-only completion-token retirement queue. */
    class RenderRetirementQueue final : public NonMovable
    {
    public:
        RenderRetirementQueue() = default;
        ~RenderRetirementQueue();

        /** @brief Bind ownership to the current Render Thread and tracker. */
        [[nodiscard]] bool Initialize(RenderSubmissionTracker* tracker);

        /** @brief Transfer a final strong reference into the queue. */
        [[nodiscard]] bool Enqueue(RenderRetirementEntry&& entry);
        /**
         * @brief Atomically append a group of retirement references.
         * Validation and capacity growth complete before any source ownership is
         * moved into the queue, so a failed resource replacement cannot retire
         * only part of its previous committed content.
         */
        [[nodiscard]] bool EnqueueBatch(
            std::vector<RenderRetirementEntry> entries);

        /** @brief Release entries whose complete tokens are satisfied. */
        [[nodiscard]] GPUCompletionStatus Poll();

        /** @brief Release all retained entries after an explicit device-loss outcome. */
        [[nodiscard]] GPUCompletionStatus ForceDeviceLostTeardown();

        /** @brief Snapshot pending retirement count, bytes, and oldest domain points. */
        [[nodiscard]] RenderRetirementDiagnostics GetDiagnostics() const;

    private:
        [[nodiscard]] bool IsOnRenderThread() const;

        RenderSubmissionTracker* m_tracker = nullptr;
        RenderThreadGuard m_renderThreadGuard;
        std::vector<RenderRetirementEntry> m_entries;
    };
} // namespace RVX
