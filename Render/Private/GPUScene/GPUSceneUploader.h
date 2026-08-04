#pragma once

/**
 * @file GPUSceneUploader.h
 * @brief Renderer-private persistent upload path for the non-executing GPU scene.
 */

#include "Core/Types.h"
#include "GPUScene/GPUSceneDatabase.h"
#include "Render/GPUScene/GPUScenePublication.h"

#include <memory>

namespace RVX
{
    class IRHIDevice;
    class RenderGraph;
    class RenderSubmissionResourceBatch;
    class RenderSubmissionTracker;
    struct GPUCompletionToken;

    /**
     * @brief Uploads the CPU GPU-scene mirror through RenderGraph copy passes.
     *
     * This type deliberately exposes no RHI resource to the rest of Render. It
     * establishes residency, state handoff and completion-aware reuse only;
     * Task 11D remains responsible for every shader/visibility/command consumer.
     */
    class GPUSceneUploader final : public NonMovable
    {
    public:
        GPUSceneUploader();
        ~GPUSceneUploader();

        [[nodiscard]] bool Initialize(
            IRHIDevice* device,
            RenderSubmissionTracker* submissionTracker) noexcept;
        void Shutdown() noexcept;

        /** @brief Observe one committed mirror generation and its exact delta. */
        void Observe(
            const GPUSceneCommittedMirror& mirror,
            const GPUSceneChangeSet& changes) noexcept;

        /** @brief Add copy passes after RenderGraph::Clear and before consumers. */
        void BuildRenderGraph(
            RenderGraph& graph,
            RenderSubmissionResourceBatch* submissionBatch) noexcept;

        /** @brief Commit realized access snapshots only after graph execution. */
        void CommitRealizedAccess(const RenderGraph& graph) noexcept;

        /** @brief Associate recorded upload resources with the actual submission token. */
        void NotifySubmission(const GPUCompletionToken& completion) noexcept;
        /** @brief Discard one recorded-but-never-submitted upload plan. */
        void ReleaseUnsubmittedFrame() noexcept;

        /**
         * @brief Record a future non-upload read of one resident GPU-scene version.
         *
         * Task 11C never calls this from shaders. Task 11D must call it while
         * recording any consumer and then use this uploader's Notify/Release
         * pair so all reads merge into the actual multi-domain submission token.
         */
        [[nodiscard]] bool MarkResidentVersionUsed(uint64 version) noexcept;

        /**
         * @brief Poll true multi-domain completion and return the safe version.
         *
         * Zero means no advancement is safe. Lost completion evidence fails
         * closed and permanently prevents in-place reuse for this uploader.
         */
        [[nodiscard]] uint64 PollSafeReclaimVersion() noexcept;

        /** @brief Confirm a successful database reclaim so retries stay exact. */
        void ConfirmReclaimedThrough(uint64 version) noexcept;

        [[nodiscard]] const GPUSceneUploadDiagnostics&
            GetDiagnostics() const noexcept
        {
            return m_diagnostics;
        }

    private:
        class Impl;
        std::unique_ptr<Impl> m_impl;
        GPUSceneUploadDiagnostics m_diagnostics;
    };
} // namespace RVX
