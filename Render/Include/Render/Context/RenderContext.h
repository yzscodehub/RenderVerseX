#pragma once

/**
 * @file RenderContext.h
 * @brief Render context - manages RHI device, swap chain, and frame synchronization
 */

#include "RHI/RHI.h"
#include "Render/Context/FrameSynchronizer.h"
#include <memory>
#include <array>

namespace RVX
{
    /**
     * @brief Render context configuration
     */
    struct RenderContextConfig
    {
        RHIBackendType backendType = RHIBackendType::None;
        bool enableValidation = true;
        bool enableGPUValidation = false;
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
     * ctx.Initialize(config);
     * ctx.CreateSwapChain(windowHandle, 1280, 720);
     * 
     * // Main loop
     * while (running)
     * {
     *     ctx.BeginFrame();
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
        RenderContext() = default;
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
        bool Initialize(const RenderContextConfig& config);

        /**
         * @brief Shutdown and release all resources
         */
        void Shutdown();

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
        bool CreateSwapChain(void* windowHandle, uint32_t width, uint32_t height);

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
         * 
         * Waits for the frame's previous work to complete (if using multi-buffering),
         * acquires the next swap chain image, and prepares the command context.
         */
        void BeginFrame();

        /**
         * @brief End the current frame
         * 
         * Submits recorded commands to the GPU.
         */
        void EndFrame();

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

        // =====================================================================
        // Accessors
        // =====================================================================

        /// Get the RHI device
        IRHIDevice* GetDevice() const { return m_device.get(); }

        /// Get the swap chain
        RHISwapChain* GetSwapChain() const { return m_swapChain.Get(); }

        /// Get the current frame's graphics command context
        RHICommandContext* GetGraphicsContext() const;

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

    private:
        void CreateCommandContexts();
        void DestroyCommandContexts();

        RenderContextConfig m_config;
        bool m_initialized = false;
        bool m_frameActive = false;

        // RHI resources
        std::unique_ptr<IRHIDevice> m_device;
        RHISwapChainRef m_swapChain;
        
        // Per-frame command contexts
        std::array<RHICommandContextRef, RVX_MAX_FRAME_COUNT> m_graphicsContexts;
        
        // Frame synchronization
        FrameSynchronizer m_frameSynchronizer;
        uint32_t m_frameIndex = 0;
        uint64_t m_frameNumber = 0;
    };

} // namespace RVX
