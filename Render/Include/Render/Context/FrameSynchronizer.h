#pragma once

/**
 * @file FrameSynchronizer.h
 * @brief Frame synchronization management for multi-buffered rendering
 */

#include "RHI/RHIDefinitions.h"
#include "RHI/RHIQueueTopology.h"
#include <array>
#include <memory>

namespace RVX
{
    class IRHIDevice;
    class RHICommandContext;
    class RenderContext;
    class RenderSubmissionTracker;
    struct RenderContextInternalAccess;

    /**
     * @brief Manages GPU/CPU synchronization for multi-frame in-flight rendering
     *
     * Stores one Graphics completion point per frame slot and resolves it
     * through one shared per-domain submission tracker.
     *
     * Usage:
     * @code
     * FrameSynchronizer sync;
     * sync.Initialize(device, 3);  // 3 frames in flight
     *
     * // Frame loop
     * if (!sync.WaitForFrame(frameIndex)) { return; }  // Do not reuse a lost slot
     * // ... record commands ...
     * sync.SignalFrame(frameIndex, submittedPoint);   // Record submitted completion point
     * @endcode
     */
    class FrameSynchronizer
    {
    public:
        FrameSynchronizer();
        ~FrameSynchronizer();

        // Non-copyable
        FrameSynchronizer(const FrameSynchronizer&) = delete;
        FrameSynchronizer& operator=(const FrameSynchronizer&) = delete;

        /**
         * @brief Initialize the synchronizer
         * @param device RHI device that publishes the queue topology
         * @param frameCount Number of frames in flight (typically 2-3)
         * @return true if initialization succeeded
         */
        bool Initialize(IRHIDevice* device, uint32_t frameCount = RVX_MAX_FRAME_COUNT);

        /**
         * @brief Drain frame slots and release the shared tracker
         */
        void Shutdown();

        /**
         * @brief Wait for a specific frame to complete
         * @param frameIndex The frame index to wait for
         * @return true when the slot is safe to reuse; false when completion was lost
         *
         * Call this at the beginning of a frame before reusing resources
         * from that frame index.
         */
        bool WaitForFrame(uint32_t frameIndex);

        /**
         * @brief Record that a frame has been submitted
         * @param frameIndex The frame index that was submitted
         * @param submittedPoint Graphics completion point returned by tracked submission
         *
         * Call this after submitting command buffers for the frame.
         */
        void SignalFrame(uint32_t frameIndex, GPUCompletionPoint submittedPoint);

        /**
         * @brief Wait for all frames to complete
         * @return true when every frame slot completed successfully
         *
         * Useful during shutdown or when needing to flush all GPU work.
         */
        bool WaitForAllFrames();

        /**
         * @brief Get the Graphics completion point stored for a frame slot
         * @param frameIndex The frame index
         * @return The point, or the zero point if invalid/unsubmitted
         */
        GPUCompletionPoint GetFrameCompletionPoint(uint32_t frameIndex) const;

        /**
         * @brief Check if a frame has completed
         * @param frameIndex The frame index to check
         * @return true if the frame's GPU work has completed
         */
        bool IsFrameComplete(uint32_t frameIndex) const;

        /**
         * @brief Get the number of frames in flight
         */
        uint32_t GetFrameCount() const { return m_frameCount; }

    private:
        friend class RenderContext;
        friend struct RenderContextInternalAccess;

        GPUCompletionPoint SubmitGraphics(RHICommandContext* context);

        IRHIDevice* m_device = nullptr;
        uint32_t m_frameCount = 0;
        std::unique_ptr<RenderSubmissionTracker> m_submissionTracker;
        std::array<GPUCompletionPoint, RVX_MAX_FRAME_COUNT> m_framePoints{};
    };

} // namespace RVX
