#pragma once

/**
 * @file RenderRuntimeComposition.h
 * @brief Engine-owned orchestration for extraction, publication, and shutdown.
 */

#include "Core/Types.h"
#include "Render/RenderDiagnostics.h"
#include "RenderExtraction/RenderFrameExtractor.h"
#include "RHI/RHINativeSurface.h"

#include <functional>
#include <memory>
#include <utility>

namespace RVX
{
    class RenderSubsystem;
    class WindowSubsystem;
    class World;

    namespace Resource
    {
        class ResourceSubsystem;
    }

    struct RenderRuntimeCompositionFrameInput
    {
        uint64 sequence = 0;
        uint64 worldRevision = 0;
        uint64 temporalEpoch = 0;
        bool explicitDiscontinuity = false;
        RenderViewExtractionParameters view{};
        RenderFrameSettings settings{};
        RenderFrameCaptureRequest captureRequest{};
        World* world = nullptr;
    };

    struct RenderRuntimeCompositionStats
    {
        uint64 extractionAttempts = 0;
        uint64 extractionCompleted = 0;
        uint64 publicationAccepted = 0;
        uint64 publicationRejected = 0;
        uint64 resizeRequests = 0;
        uint64 lastAttemptedSequence = 0;
        uint64 worldRevision = 0;
        uint64 temporalEpoch = 1;
        uint64 surfaceGeneration = 0;
        RenderFrameExtractionResultCode lastExtractionCode =
            RenderFrameExtractionResultCode::SealFailed;
        RenderFramePublishResult lastPublishResult{};
        RenderResizeResult lastResizeResult{};
        RenderShutdownResult lastShutdownResult{};
    };

    /** @brief Narrow adapter used by the orchestration policy and focused tests. */
    class IRenderRuntimeCompositionServices
    {
    public:
        virtual ~IRenderRuntimeCompositionServices() = default;

        virtual void InjectRenderResourceGateway() = 0;
        [[nodiscard]] virtual bool IsWindowInitialized() const noexcept = 0;
        [[nodiscard]] virtual NativeSurfaceDesc CaptureRenderSurface() = 0;
        virtual void ReleaseGraphicsContextFromUpdateThread() = 0;
        virtual void ConfigureRender(const RenderRuntimeConfig& config,
                                     const NativeSurfaceDesc& surface) = 0;
        [[nodiscard]] virtual bool IsRenderReady() const noexcept = 0;
        [[nodiscard]] virtual RenderFrameExtractionResult ExtractFrame(
            const RenderRuntimeCompositionFrameInput& input) = 0;
        /** @brief Publish the reliable scene update before its v5 frame. */
        virtual RenderFramePublishResult PublishExtractedFrame(
            RenderFrameExtractionResult extraction) = 0;
        [[nodiscard]] virtual RenderDiagnosticsSnapshot
            GetRenderDiagnostics() const = 0;
        virtual RenderResizeResult RequestResize(
            const NativeSurfaceDesc& surface) = 0;
        virtual void BeginResourceShutdown() = 0;
        virtual RenderShutdownResult StopRender() = 0;
        virtual void DrainTerminalRenderRequests() = 0;
    };

    /**
     * @brief State machine that composes update-owned values with Render runtime.
     *
     * Engine owns this object. It never exposes a live RHI or renderer object.
     */
    class RenderRuntimeComposition final : public NonMovable
    {
    public:
        RenderRuntimeComposition(
            RenderRuntimeConfig config,
            RenderFrameSettings initialSettings,
            std::unique_ptr<IRenderRuntimeCompositionServices> services);

        [[nodiscard]] bool PrepareBeforeSubsystemInitialization();
        void BeforeRenderSubsystemInitialize();
        void TickAfterWorlds(World* activeWorld,
                             float32 deltaTime,
                             float32 absoluteTime);
        void OnActiveWorldChanged() noexcept;
        [[nodiscard]] uint64 RequestTemporalReset() noexcept;
        [[nodiscard]] bool SetFrameSettings(
            const RenderFrameSettings& settings) noexcept;
        [[nodiscard]] const RenderFrameSettings& GetFrameSettings() const noexcept
        {
            return m_settings;
        }
        [[nodiscard]] bool QueueCapture(
            const RenderFrameCaptureRequest& request) noexcept;
        [[nodiscard]] bool RequestSurfaceResize(uint32 width,
                                                uint32 height);
        [[nodiscard]] RenderShutdownResult Shutdown();
        [[nodiscard]] const RenderRuntimeCompositionStats& GetStats() const noexcept
        {
            return m_stats;
        }

    private:
        [[nodiscard]] static bool IsCaptureRequestValid(
            const RenderFrameCaptureRequest& request) noexcept;
        [[nodiscard]] RHIBackendType ResolveBackend() const noexcept;
        void AcknowledgePublishedOneShotValues();
        void RouteSurfaceUpdate();

        RenderRuntimeConfig m_config{};
        RenderFrameSettings m_settings{};
        RenderFrameCaptureRequest m_pendingCapture{};
        std::unique_ptr<IRenderRuntimeCompositionServices> m_services;
        NativeSurfaceDesc m_surface{};
        RenderRuntimeCompositionStats m_stats{};
        uint64 m_nextFrameSequence = 1;
        uint64 m_worldRevision = 0;
        uint64 m_temporalEpoch = 1;
        uint64 m_temporalResetPublishedSequence = 0;
        uint64 m_lastAcknowledgedCaptureRequestId = 0;
        bool m_temporalResetPending = false;
        bool m_prepared = false;
        bool m_renderConfigured = false;
        bool m_shutdown = false;
    };

    [[nodiscard]] std::unique_ptr<IRenderRuntimeCompositionServices>
        CreateEngineRenderRuntimeCompositionServices(
            Resource::ResourceSubsystem& resources,
            RenderSubsystem& render,
            WindowSubsystem& window,
            std::function<bool()> stopRender);

} // namespace RVX
