#pragma once

/**
 * @file RenderFrameTimingOwner.h
 * @brief RenderContext-private owner of completion-owned timestamp samples.
 */

#include "Render/Context/RenderFrameTiming.h"
#include "RHI/RHIQuery.h"
#include "RHI/RHIQueueTopology.h"

#include <array>

namespace RVX
{
    class FrameSynchronizer;
    class IRHIDevice;
    class RHICommandContext;
    class RHIQueryPool;

    /**
     * @brief Per-slot whole-frame timestamp state owned exclusively by RenderContext.
     *
     * A slot is only mapped after the FrameSynchronizer proves the exact Graphics
     * completion point bound to it has completed. The owner never waits and never
     * maps after device loss.
     */
    class RenderFrameTimingOwner
    {
    public:
        bool Initialize(IRHIDevice* device, uint32 frameCount);
        void Shutdown(bool deviceLost) noexcept;

        /** @brief Record the first Graphics timestamp after the prelude begins. */
        void BeginFrame(uint32 frameIndex, RHICommandContext& graphicsContext) noexcept;
        /** @brief Record/resolve the terminal timestamp before the Graphics context ends. */
        void EndFrame(uint32 frameIndex, RHICommandContext& graphicsContext) noexcept;

        /** @brief Associate the recording slot with its exact submitted Graphics point. */
        void MarkSubmitted(uint32 frameIndex, GPUCompletionPoint submittedPoint) noexcept;

        /** @brief Bind a source frame to the same non-zero Graphics completion point. */
        bool BindSubmittedFrame(GPUCompletionPoint submittedPoint,
                                uint64 sourceFrameSequence) noexcept;

        /**
         * @brief Read and retire a slot after its exact point has already completed.
         *
         * The caller supplies the FrameSynchronizer point that its blocking wait
         * or non-blocking completion observation has proven complete.
         */
        void DrainCompletedSlot(uint32 frameIndex,
                                GPUCompletionPoint completedPoint) noexcept;

        /** @brief Discard an unsubmitted recording or terminally lost slot. */
        void DiscardFrame(uint32 frameIndex, bool lost) noexcept;
        void MarkAllLost() noexcept;

        [[nodiscard]] const RenderGpuFrameTimingDiagnostics& GetDiagnostics() const noexcept
        {
            return m_diagnostics;
        }

        [[nodiscard]] bool IsEnabled() const noexcept { return m_enabled; }

    private:
        enum class SlotState : uint8
        {
            Idle = 0,
            Recording,
            AwaitingSubmission,
            Submitted,
        };

        struct Slot
        {
            RHIBufferRef readbackBuffer;
            GPUCompletionPoint submittedPoint;
            SlotState state = SlotState::Idle;
            bool sourceBound = false;
            uint64 sourceFrameSequence = 0;
        };

        void SetUnavailable(const char* reason) noexcept;
        void ClearSlot(Slot& slot) noexcept;
        void PublishSample(const Slot& slot,
                           uint64 startTimestamp,
                           uint64 endTimestamp) noexcept;

        IRHIDevice* m_device = nullptr;
        RHIQueryPoolRef m_queryPool;
        std::array<Slot, RVX_MAX_FRAME_COUNT> m_slots;
        uint32 m_frameCount = 0;
        uint64 m_timestampFrequency = 0;
        uint32 m_timestampValidBits = 0;
        bool m_enabled = false;
        RenderGpuFrameTimingDiagnostics m_diagnostics;
    };
} // namespace RVX
