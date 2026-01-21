#pragma once

/**
 * @file FrameSynchronizer.h
 * @brief Frame synchronization management for multi-buffered rendering
 */

#include "RHI/RHI.h"
#include <array>

namespace RVX
{
    /**
     * @brief Manages GPU/CPU synchronization for multi-frame in-flight rendering
     * 
     * Handles fence creation and waiting to ensure proper synchronization
     * when using multiple frames in flight (typically 2-3 frames).
     * 
     * Usage:
     * @code
     * FrameSynchronizer sync;
     * sync.Initialize(device, 3);  // 3 frames in flight
     * 
     * // Frame loop
     * sync.WaitForFrame(frameIndex);  // Wait for frame to complete
     * // ... record commands ...
     * sync.SignalFrame(frameIndex);   // Signal when submitted
     * @endcode
     */
    class FrameSynchronizer
    {
    public:
        FrameSynchronizer() = default;
        ~FrameSynchronizer();

        // Non-copyable
        FrameSynchronizer(const FrameSynchronizer&) = delete;
        FrameSynchronizer& operator=(const FrameSynchronizer&) = delete;

        /**
         * @brief Initialize the synchronizer
         * @param device RHI device to create fences
         * @param frameCount Number of frames in flight (typically 2-3)
         * @return true if initialization succeeded
         */
        bool Initialize(IRHIDevice* device, uint32_t frameCount = RVX_MAX_FRAME_COUNT);

        /**
         * @brief Shutdown and release all fences
         */
        void Shutdown();

        /**
         * @brief Wait for a specific frame to complete
         * @param frameIndex The frame index to wait for
         * 
         * Call this at the beginning of a frame before reusing resources
         * from that frame index.
         */
        void WaitForFrame(uint32_t frameIndex);

        /**
         * @brief Signal that a frame has been submitted
         * @param frameIndex The frame index that was submitted
         * 
         * Call this after submitting command buffers for the frame.
         */
        void SignalFrame(uint32_t frameIndex);

        /**
         * @brief Wait for all frames to complete
         * 
         * Useful during shutdown or when needing to flush all GPU work.
         */
        void WaitForAllFrames();

        /**
         * @brief Get the fence for a specific frame
         * @param frameIndex The frame index
         * @return The fence, or nullptr if invalid
         */
        RHIFence* GetFence(uint32_t frameIndex) const;

        /**
         * @brief Get the current fence value for a frame
         * @param frameIndex The frame index
         * @return The expected fence value for this frame
         */
        uint64_t GetFrameFenceValue(uint32_t frameIndex) const;

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
        IRHIDevice* m_device = nullptr;
        uint32_t m_frameCount = 0;
        
        std::array<RHIFenceRef, RVX_MAX_FRAME_COUNT> m_fences;
        std::array<uint64_t, RVX_MAX_FRAME_COUNT> m_fenceValues = {};
    };

} // namespace RVX
