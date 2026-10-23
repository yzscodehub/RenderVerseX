#pragma once

/** @file FrameConstantUploadArena.h @brief Completion-tracked paged upload constants. */

#include "Core/Types.h"
#include "RHI/RHI.h"
#include "Resources/RenderSubmissionTracker.h"

#include <string>
#include <vector>

namespace RVX
{
    /** @brief One immutable recording-time address inside a constant-buffer page. */
    struct FrameConstantUploadAllocation
    {
        RHIBufferRef buffer;
        uint64 pageIdentity = 0;
        uint32 slot = 0;
        uint32 dynamicOffset = 0;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return buffer && pageIdentity != 0;
        }
    };

    /**
     * @brief Reusable upload pages whose reuse is guarded only by completion evidence.
     *
     * Recording pages are never selected again after NotifySubmission().  A page
     * becomes Free only after its exact token is completed (or on an explicitly
     * rejected recording); Lost/invalid evidence poisons the page rather than
     * guessing that a CPU frame index has made it safe.
     */
    class FrameConstantUploadArena final
    {
    public:
        enum class PageState : uint8
        {
            Free = 0,
            Recording,
            InFlight,
            Unusable,
        };

        FrameConstantUploadArena() = default;
        ~FrameConstantUploadArena() = default;

        FrameConstantUploadArena(const FrameConstantUploadArena&) = delete;
        FrameConstantUploadArena& operator=(const FrameConstantUploadArena&) = delete;

        bool Initialize(IRHIDevice* device,
                        uint64 stride,
                        uint32 slotsPerPage,
                        std::string debugName);
        void Shutdown() noexcept;

        void SetSubmissionTracker(RenderSubmissionTracker* tracker) noexcept
        {
            m_tracker = tracker;
        }

        /** @brief Query in-flight pages without waiting; lost evidence poisons them. */
        void PollCompletions() noexcept;

        /** @brief Allocate and upload one fully formed constant record. */
        [[nodiscard]] bool Allocate(const void* source,
                                    uint64 sourceSize,
                                    FrameConstantUploadAllocation& outAllocation);

        /** @brief Bind every page recorded since the prior lifecycle transition. */
        [[nodiscard]] bool NotifySubmission(const GPUCompletionToken& completion) noexcept;

        /** @brief Release never-submitted recording pages immediately. */
        void ReleaseUnsubmittedFrame() noexcept;

        [[nodiscard]] uint64 GetStride() const noexcept { return m_stride; }
        [[nodiscard]] uint32 GetSlotsPerPage() const noexcept { return m_slotsPerPage; }
        [[nodiscard]] uint32 GetPageCount() const noexcept
        {
            return static_cast<uint32>(m_pages.size());
        }
        [[nodiscard]] uint32 GetUnusablePageCount() const noexcept;

    private:
        struct Page
        {
            RHIBufferRef buffer;
            GPUCompletionToken completion{};
            uint64 identity = 0;
            uint32 cursor = 0;
            PageState state = PageState::Free;
        };

        [[nodiscard]] Page* AcquireRecordingPage();
        [[nodiscard]] bool CreatePage(Page& outPage);
        [[nodiscard]] static bool IsCompletionSatisfied(GPUCompletionStatus status) noexcept;
        [[nodiscard]] static bool IsTokenUsable(const GPUCompletionToken& token) noexcept;

        IRHIDevice* m_device = nullptr;
        RenderSubmissionTracker* m_tracker = nullptr;
        uint64 m_stride = 0;
        uint32 m_slotsPerPage = 0;
        uint64 m_nextPageIdentity = 1;
        std::string m_debugName;
        std::vector<Page> m_pages;
    };
} // namespace RVX
