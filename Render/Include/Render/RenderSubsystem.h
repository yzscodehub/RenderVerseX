#pragma once

/**
 * @file RenderSubsystem.h
 * @brief Render subsystem - main rendering coordinator
 */

#include "Core/Subsystem/EngineSubsystem.h"
#include "Render/RenderDiagnostics.h"
#include "Render/RenderRuntimeTypes.h"
#include "RenderContracts/IRenderResourceGateway.h"
#include "RenderContracts/RenderFramePacket.h"
#include "RHI/RHI.h"
#include "Runtime/Window/WindowSubsystem.h"

#include <memory>

namespace RVX
{
    // Forward declarations
    class World;
    class Camera;
    class RenderContext;
    class SceneRenderer;
    class RenderGraph;
    class LegacySynchronousRenderBridge;
    class RenderThreadRuntime;

    /**
     * @brief Render configuration
     */
    struct RenderConfig
    {
        RHIBackendType backendType = RHIBackendType::Auto;  // Auto selects best for platform
        bool enableValidation = true;
        bool vsync = true;
        uint32_t frameBuffering = 2;
        bool autoBindWindow = true;   // Automatically bind to WindowSubsystem
        bool autoRender = true;       // Automatically render in Engine::Tick (disable for manual control)
    };

    /**
     * @brief Render subsystem - coordinates rendering
     * 
     * This is the main entry point for the rendering system. It acts as a pure
     * coordinator, delegating to:
     * - RenderContext: RHI device, swap chain, and frame synchronization
     * - SceneRenderer: Scene data collection and render pass execution
     * 
     * Responsibilities:
     * - Engine integration (subsystem lifecycle)
     * - Frame lifecycle coordination (BeginFrame/EndFrame/Present)
     * - Window association and resize handling
     * 
     * Usage:
     * @code
     * auto* renderSys = engine.GetSubsystem<RenderSubsystem>();
     * 
     * // Main loop
     * renderSys->BeginFrame();
     * renderSys->Render(world, camera);
     * renderSys->EndFrame();
     * renderSys->Present();
     * @endcode
     */
    class RenderSubsystem : public EngineSubsystem,
                            public IRenderResourceGateway
    {
    public:
        RenderSubsystem();
        ~RenderSubsystem() override;

        const char* GetName() const override { return "RenderSubsystem"; }
        bool ShouldTick() const override { return false; }

        /// Dependencies - requires WindowSubsystem (type-safe)
        RVX_SUBSYSTEM_DEPENDENCIES(WindowSubsystem)

        void Initialize() override;
        void Deinitialize() override;

        // =====================================================================
        // Render Runtime
        // =====================================================================

        /** @brief Configure the dedicated runtime before Initialize(). */
        void Configure(const RenderRuntimeConfig& config,
                       const NativeSurfaceDesc& surface);
        RenderFramePublishResult TryPublishFrame(
            std::unique_ptr<const RenderFramePacket> packet);
        RenderResizeResult RequestResize(const NativeSurfaceDesc& surface);
        [[nodiscard]] RenderDiagnosticsSnapshot
            GetDiagnosticsSnapshot() const;
        [[nodiscard]] RenderRuntimeResult GetLastRuntimeResult() const;
        [[nodiscard]] RenderShutdownResult GetLastShutdownResult() const;

        /// Initialize with custom config
        void Initialize(const RenderConfig& config);

        // =====================================================================
        // Frame Lifecycle
        // =====================================================================

        /// Begin a new frame
        void BeginFrame();

        /// Render a world with a camera
        void Render(World* world, Camera* camera);

        /// End the current frame
        void EndFrame();

        /// Present to screen
        void Present();

        /**
         * @brief Render a complete frame for a world
         * 
         * This is a convenience method that calls BeginFrame, Render, EndFrame, Present.
         * It automatically gets the active camera from the world.
         * 
         * @param world The world to render (uses world's active camera)
         */
        void RenderFrame(World* world);

        // =====================================================================
        // Component Access
        // =====================================================================

        /// Get the render context (manages RHI resources)
        RenderContext* GetRenderContext() const;

        /// Get the scene renderer (manages render passes)
        SceneRenderer* GetSceneRenderer() const;

        /// Get the RHI device (convenience accessor)
        IRHIDevice* GetDevice() const;

        /// Get the swap chain (convenience accessor)
        RHISwapChain* GetSwapChain() const;

        /// Get the render graph (convenience accessor)
        RenderGraph* GetRenderGraph() const;

        // =====================================================================
        // Window Association
        // =====================================================================

        /// Set the captured native surface for rendering
        bool SetWindow(const NativeSurfaceDesc& surface);

        /// Set the window subsystem used by auto window binding.
        void SetWindowSubsystem(WindowSubsystem* windowSubsystem);

        /// Handle window resize
        void OnResize(uint32_t width, uint32_t height);

        // =====================================================================
        // GPU Resource Management
        // =====================================================================

        /// Process pending GPU resource uploads
        /// @param timeBudgetMs Maximum time to spend uploading (in milliseconds)
        void ProcessGPUUploads(float timeBudgetMs = 2.0f);

        /// Get the GPU resource manager
        class GPUResourceManager* GetGPUResourceManager() const;

        RenderResourceReserveResult ReserveResource(
            AssetId assetId,
            RenderResourceKind kind) noexcept override;
        RenderUploadEnqueueResult TryEnqueueUpload(
            const ResourceUploadRequestRef& request) noexcept override;
        RenderReleaseResult RequestRelease(
            RenderResourceHandle handle) noexcept override;
        [[nodiscard]] RenderResourceStatus QueryResourceStatus(
            RenderResourceHandle handle) const noexcept override;

        // =====================================================================
        // Configuration
        // =====================================================================

        /// Set configuration (call before Initialize)
        void SetConfig(const RenderConfig& config);
        const RenderConfig& GetConfig() const;

        /// Check if initialized and ready to render
        bool IsReady() const;

    private:
        friend struct RenderSubsystemTestAccess;

        void InitializeRuntime();
        void InitializeLegacy(const RenderConfig& config);
        void AutoBindWindow();
        void EnsureVisibleResourcesResident();
        
        std::unique_ptr<LegacySynchronousRenderBridge> m_legacyBridge;
        std::unique_ptr<RenderThreadRuntime> m_runtime;
        RenderRuntimeConfig m_runtimeConfig{};
        NativeSurfaceDesc m_runtimeSurface{};
        RenderRuntimeResult m_preRuntimeResult{};
        RenderShutdownResult m_preShutdownResult{};
        bool m_runtimeConfigured = false;
        bool m_initializeAttempted = false;
    };

} // namespace RVX
