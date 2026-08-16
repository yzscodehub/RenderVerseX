#pragma once

/**
 * @file RenderContext.h
 * @brief Render context - manages RHI device, swap chain, and frame synchronization
 */

#include "RHI/RHI.h"
#include "Render/Context/FrameSynchronizer.h"
#include "Render/Context/RenderFrameTiming.h"
#include "Render/Graph/RenderGraphExecution.h"
#include <array>
#include <memory>

namespace RVX
{
    struct RenderContextInternalAccess;
    class RenderFrameTimingOwner;

    /**
     * @brief Render context configuration
     */
    struct RenderContextConfig
    {
        RHIBackendType backendType = RHIBackendType::None;
        bool enableValidation = true;
        bool enableGPUValidation = false;
        bool allowSoftwareAdapter = false;
        bool vsync = true;
        uint32_t frameBuffering = 2;  // Number of frames in flight
        const char* appName = "RenderVerseX";
    };

    /**
     * @brief Render context - encapsulates RHI device, swap chain, and frame synchronization
     * 
     * RenderContext is the central rendering resource manager. It owns the RHI device,
     * manages swap chain lifecycle, and handles multi-frame synchronization.
     * 
     * Responsibilities:
     * - RHI device creation and lifecycle
     * - Swap chain management (creation, resize, present)
     * - Frame synchronization (fences, frame indexing)
     * - Command context management per frame
     * 
     * Usage:
     * @code
     * RenderContextConfig config;
     * config.backendType = RHIBackendType::Vulkan;
     * config.enableValidation = true;
     * 
     * RenderContext ctx;
     * NativeSurfaceDesc surface;
     * ctx.Initialize(config, surface);
     * ctx.CreateSwapChain(surface);
     * 
     * // Main loop
     * while (running)
     * {
     *     if (!ctx.BeginFrame()) { break; }
     *     auto* cmdCtx = ctx.GetGraphicsContext();
     *     // ... record commands ...
     *     ctx.EndFrame();
     *     ctx.Present();
     * }
     * 
     * ctx.Shutdown();
     * @endcode
     */
    class RenderContext
    {
    public:
        RenderContext();
        ~RenderContext();

        // Non-copyable
        RenderContext(const RenderContext&) = delete;
        RenderContext& operator=(const RenderContext&) = delete;

        // =====================================================================
        // Lifecycle
        // =====================================================================

        /**
         * @brief Initialize the render context
         * @param config Configuration options
         * @return true if initialization succeeded
         */
        bool Initialize(const RenderContextConfig& config,
                        const NativeSurfaceDesc& initialSurface = {});

        /**
         * @brief Shutdown and release all resources
         */
        void Shutdown(bool waitForIdle = true);

        /**
         * @brief Check if the context is initialized
         */
        bool IsInitialized() const { return m_initialized; }

        // =====================================================================
        // Swap Chain Management
        // =====================================================================

        /**
         * @brief Create a swap chain for a window
         * @param windowHandle Native window handle (HWND on Windows)
         * @param width Initial width
         * @param height Initial height
         * @return true if creation succeeded
         */
        bool CreateSwapChain(const NativeSurfaceDesc& surface);

        /** @brief Apply a newer extent-only complete surface snapshot. */
        bool ResizeSwapChain(const NativeSurfaceDesc& surface);

        /**
         * @brief Resize the swap chain
         * @param width New width
         * @param height New height
         */
        void ResizeSwapChain(uint32_t width, uint32_t height);

        /**
         * @brief Check if swap chain exists
         */
        bool HasSwapChain() const { return m_swapChain != nullptr; }

        // =====================================================================
        // Frame Management
        // =====================================================================

        /**
         * @brief Begin a new frame
         * @return true when the frame slot is safe and recording began
         * 
         * Waits for the frame's previous work to complete (if using multi-buffering),
         * acquires the next swap chain image, and prepares the command context.
         */
        bool BeginFrame();

        /**
         * @brief End the current frame
         * @return Submitted Graphics completion point, or the zero point on failure
         *
         * Submits recorded commands to the GPU.
         */
        GPUCompletionPoint EndFrame();

        /**
         * @brief Adopt a recorded queue DAG between the frame Graphics prelude
         * and a final Graphics gateway used by capture and presentation.
         */
        bool AdoptQueueSubmission(
            RHIQueueSubmissionPlan plan,
            std::vector<RHICommandContextRef> ownedContexts);

        /** @brief Atomically adopt graph commands and their completion-owned resources. */
        bool AdoptRenderGraphExecution(RenderGraphExecution&& execution);

        /** @brief End recording without submitting or enabling presentation. */
        void AbortFrame();

        /**
         * @brief Present the frame to the screen
         */
        void Present();

        /**
         * @brief Wait for all GPU work to complete
         * 
         * Useful for shutdown or resource recreation.
         */
        void WaitIdle();

        /**
         * @brief Bind a source frame sequence to the same submitted Graphics
         * completion point that owns its whole-frame timestamp sample.
         *
         * Timing availability never changes frame-submission success. Callers
         * may retry an exact bind; a mismatched point or sequence is rejected
         * without changing the pending sample.
         */
        bool BindSubmittedFrameTiming(GPUCompletionPoint submittedPoint,
                                      uint64 sourceFrameSequence) noexcept;

        /** @brief Non-blockingly collect already-completed Graphics timing samples. */
        void PollFrameTiming() noexcept;

        /** @brief Wait only for Graphics work referencing the current surface generation. */
        bool WaitForSurfaceGeneration();

        // =====================================================================
        // Accessors
        // =====================================================================

        /// Get the RHI device
        IRHIDevice* GetDevice() const { return m_device.get(); }

        /// Get the swap chain
        RHISwapChain* GetSwapChain() const { return m_swapChain.Get(); }

        /// Get the complete snapshot backing the current swap chain.
        const NativeSurfaceDesc& GetSurface() const { return m_surface; }

        /// Get the current frame's graphics command context
        RHICommandContext* GetGraphicsContext() const;

        /// Get the current frame's compute command context (for async compute)
        RHICommandContext* GetComputeContext() const;

        /// Check if async compute is supported
        bool SupportsAsyncCompute() const { return m_supportsAsyncCompute; }

        /// Get the current frame index (0 to frameBuffering-1)
        uint32_t GetFrameIndex() const { return m_frameIndex; }

        /// Get the frame synchronizer
        FrameSynchronizer* GetFrameSynchronizer() { return &m_frameSynchronizer; }

        /// Get the current back buffer texture
        RHITexture* GetCurrentBackBuffer() const;

        /// Get the current back buffer view
        RHITextureView* GetCurrentBackBufferView() const;

        /// Get the configuration
        const RenderContextConfig& GetConfig() const { return m_config; }

        /** @brief Latest completion-owned Graphics whole-frame timing snapshot. */
        const RenderGpuFrameTimingDiagnostics& GetGpuFrameTimingDiagnostics() const noexcept;

    private:
        friend struct RenderContextInternalAccess;

        void CreateCommandContexts();
        void DestroyCommandContexts();

        RenderContextConfig m_config;
        bool m_initialized = false;
        bool m_frameActive = false;
        bool m_frameReadyToPresent = false;

        // RHI resources
        std::unique_ptr<IRHIDevice> m_device;
        RHISwapChainRef m_swapChain;
        
        // Per-frame command contexts
        std::array<RHICommandContextRef, RVX_MAX_FRAME_COUNT> m_graphicsContexts;
        std::array<RHICommandContextRef, RVX_MAX_FRAME_COUNT> m_computeContexts;
        std::array<std::vector<RHICommandContextRef>, RVX_MAX_FRAME_COUNT>
            m_inFlightQueueContexts;
        RHIQueueSubmissionPlan m_pendingQueuePlan;
        std::vector<RHICommandContextRef> m_pendingQueueContexts;
        RHICommandContextRef m_pendingGraphicsGateway;
        bool m_queueSubmissionPending = false;
        bool m_graphicsContextRecording = false;
        std::unique_ptr<RenderGraphExecution> m_pendingGraphExecution;
        std::array<std::unique_ptr<RenderGraphExecution>, RVX_MAX_FRAME_COUNT>
            m_inFlightGraphExecutions;
        std::unique_ptr<RenderFrameTimingOwner> m_frameTiming;
        
        // Frame synchronization
        FrameSynchronizer m_frameSynchronizer;
        uint32_t m_frameIndex = 0;
        uint64_t m_frameNumber = 0;
        bool m_supportsAsyncCompute = false;
        NativeSurfaceDesc m_surface;
    };

} // namespace RVX
