#pragma once

/** @file RenderGraphExecution.h @brief Completion-owned realized RenderGraph execution */

#include "RHI/RHI.h"

#include <memory>
#include <vector>

namespace RVX
{
    class RenderGraph;
    class RenderContext;
    class TransientTextureLease;
    class TransientBufferLease;
    struct GPUCompletionToken;

    enum class RenderGraphExecutionState : uint8
    {
        Invalid = 0,
        Prepared,
        Recorded,
        Adopted,
        Submitted,
        Retired,
        AbortUnsubmitted,
        DeviceLost,
    };

    /**
     * @brief Move-only ownership unit for one physical graph realization.
     *
     * Every transient lease and graph-owned RHI object is resolved through one
     * terminal completion token. Destruction of a prepared/recorded execution
     * aborts it; a submitted execution must be retired only after completion.
     */
    class RenderGraphExecution final
    {
    public:
        RenderGraphExecution();
        ~RenderGraphExecution();
        RenderGraphExecution(RenderGraphExecution&& other) noexcept;
        RenderGraphExecution& operator=(RenderGraphExecution&& other) noexcept;
        RenderGraphExecution(const RenderGraphExecution&) = delete;
        RenderGraphExecution& operator=(const RenderGraphExecution&) = delete;

        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] RenderGraphExecutionState GetState() const noexcept;
        [[nodiscard]] bool HasQueueSubmissionPlan() const noexcept;

        [[nodiscard]] bool AbortUnsubmitted();
        [[nodiscard]] bool MarkDeviceLost();
        [[nodiscard]] bool Retire();

    private:
        friend class RenderGraph;
        friend class RenderContext;

        void Prepare();
        [[nodiscard]] bool MarkRecorded();
        [[nodiscard]] bool MarkAdopted();
        [[nodiscard]] bool Commit(const GPUCompletionToken& completion);
        void AddTextureLease(
            TransientTextureLease&& lease,
            RHITextureAccessSnapshot finalAccess);
        void AddBufferLease(
            TransientBufferLease&& lease,
            RHIBufferAccessSnapshot finalAccess);
        void RetainTexture(RHITextureRef texture);
        void RetainBuffer(RHIBufferRef buffer);
        void RetainHeap(RHIHeapRef heap);
        void SetQueueSubmission(
            RHIQueueSubmissionPlan plan,
            std::vector<RHICommandContextRef> ownedContexts);
        [[nodiscard]] bool TakeQueueSubmission(
            RHIQueueSubmissionPlan& plan,
            std::vector<RHICommandContextRef>& ownedContexts);

        class Impl;
        std::unique_ptr<Impl> m_impl;
    };
} // namespace RVX
