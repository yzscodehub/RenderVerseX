#pragma once

/**
 * @file GPUSceneUploader.h
 * @brief Renderer-private persistent upload path for the non-executing GPU scene.
 */

#include "Core/Types.h"
#include "GPUScene/GPUSceneDatabase.h"
#include "Render/Graph/RenderGraph.h"
#include "Render/GPUScene/GPUScenePublication.h"
#include "RHI/RHIResources.h"

#include <array>
#include <memory>
#include <optional>

namespace RVX
{
    class IRHIDevice;
    class RenderGraph;
    class RenderSubmissionResourceBatch;
    class RenderSubmissionTracker;
    struct GPUCompletionToken;

    /** @brief Renderer-private fixed ordering of one GPU-scene resident table set. */
    enum class GPUSceneResidentTable : uint8
    {
        Primitives = 0,
        Bounds,
        Transforms,
        Materials,
        Geometries,
        Draws,
        Count,
    };

    constexpr uint32 GPU_SCENE_RESIDENT_TABLE_COUNT =
        static_cast<uint32>(GPUSceneResidentTable::Count);

    /**
     * @brief Exact one-recording view of one fully current uploader buffer set.
     *
     * This renderer-private value deliberately owns all six buffers. Consumers
     * must use only its imported handles, then let GPUSceneUploader commit the
     * realized accesses and submission ownership for this exact set.
     */
    struct GPUSceneResidentGraphLease
    {
        uint64 version = 0;
        std::array<RHIBufferRef, GPU_SCENE_RESIDENT_TABLE_COUNT> buffers;
        std::array<RGBufferHandle, GPU_SCENE_RESIDENT_TABLE_COUNT> handles;
        std::array<uint32, GPU_SCENE_RESIDENT_TABLE_COUNT> capacities{};

        [[nodiscard]] bool IsValid() const noexcept
        {
            if (version == 0)
            {
                return false;
            }
            for (uint32 tableIndex = 0;
                 tableIndex < GPU_SCENE_RESIDENT_TABLE_COUNT;
                 ++tableIndex)
            {
                if (!buffers[tableIndex] || !handles[tableIndex].IsValid())
                {
                    return false;
                }
            }
            return true;
        }

    private:
        friend class GPUSceneUploader;
        uint32 m_bufferSetIndex = RVX_INVALID_INDEX;
    };

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

        /**
         * @brief Acquire one exact fully-current resident set for graph reads.
         *
         * Pending uploads, stale versions, dirty state, incomplete table sets,
         * and a second outstanding lease all fail closed. The returned handles
         * are imported with their prior realized snapshots; the consumer owns
         * declaring ShaderResource reads in its graph pass.
         */
        [[nodiscard]] std::optional<GPUSceneResidentGraphLease>
            AcquireCurrentGraphLease(
                RenderGraph& graph,
                RenderSubmissionResourceBatch* submissionBatch) noexcept;

        /**
         * @brief Cancel an acquired lease before any graph consumer registers it.
         *
         * This restores the pre-recording snapshots without marking the table
         * set as GPU-used. It is intentionally unavailable after access commit
         * or while an upload plan is pending.
         */
        [[nodiscard]] bool CancelCurrentGraphLease() noexcept;

        /** @brief Commit upload and exact-lease realized accesses after graph execution. */
        void CommitRealizedAccess(const RenderGraph& graph) noexcept;

        /** @brief Associate recorded upload resources with the actual submission token. */
        void NotifySubmission(const GPUCompletionToken& completion) noexcept;
        /** @brief Discard one recorded-but-never-submitted upload plan. */
        void ReleaseUnsubmittedFrame() noexcept;

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
