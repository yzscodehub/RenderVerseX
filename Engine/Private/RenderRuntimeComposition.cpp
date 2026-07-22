/**
 * @file RenderRuntimeComposition.cpp
 * @brief Engine-owned rendering composition implementation.
 */

#include "RenderRuntimeComposition.h"

#include "Render/RenderSubsystem.h"
#include "RenderContracts/RenderFrameValidation.h"
#include "Resource/ResourceSubsystem.h"
#include "Runtime/Window/WindowSubsystem.h"

#include <stdexcept>
#include <utility>

namespace RVX
{
    namespace
    {
        class EngineRenderRuntimeCompositionServices final
            : public IRenderRuntimeCompositionServices,
              public NonMovable
        {
        public:
            EngineRenderRuntimeCompositionServices(
                Resource::ResourceSubsystem& resources,
                RenderSubsystem& render,
                WindowSubsystem& window,
                std::function<bool()> stopRender)
                : m_resources(resources),
                  m_render(render),
                  m_window(window),
                  m_stopRender(std::move(stopRender))
            {
            }

            void InjectRenderResourceGateway() override
            {
                m_resources.SetRenderResourceGateway(&m_render);
            }

            bool IsWindowInitialized() const noexcept override
            {
                return m_window.IsInitialized() && m_window.GetWindow() != nullptr;
            }

            NativeSurfaceDesc CaptureRenderSurface() override
            {
                return m_window.CaptureRenderSurface();
            }

            void ReleaseGraphicsContextFromUpdateThread() override
            {
                m_window.ReleaseGraphicsContextFromCurrentThread();
            }

            void ConfigureRender(const RenderRuntimeConfig& config,
                                 const NativeSurfaceDesc& surface) override
            {
                m_render.Configure(config, surface);
            }

            bool IsRenderReady() const noexcept override
            {
                return m_render.IsReady();
            }

            RenderFrameExtractionResult ExtractFrame(
                const RenderRuntimeCompositionFrameInput& input) override
            {
                RenderFrameExtractionInput extraction;
                extraction.sequence = input.sequence;
                extraction.worldRevision = input.worldRevision;
                extraction.temporalEpoch = input.temporalEpoch;
                extraction.explicitDiscontinuity = input.explicitDiscontinuity;
                extraction.view = input.view;
                extraction.settings = input.settings;
                extraction.captureRequest = input.captureRequest;
                extraction.world = input.world;
                extraction.resources = &m_resources;
                return m_extractor.Extract(extraction);
            }

            RenderFramePublishResult PublishFrame(
                std::unique_ptr<const RenderFramePacket> packet) override
            {
                return m_render.TryPublishFrame(std::move(packet));
            }

            RenderDiagnosticsSnapshot GetRenderDiagnostics() const override
            {
                return m_render.GetDiagnosticsSnapshot();
            }

            RenderResizeResult RequestResize(
                const NativeSurfaceDesc& surface) override
            {
                return m_render.RequestResize(surface);
            }

            void BeginResourceShutdown() override
            {
                m_resources.BeginRenderShutdown();
            }

            RenderShutdownResult StopRender() override
            {
                if (m_stopRender)
                {
                    static_cast<void>(m_stopRender());
                }
                return m_render.GetLastShutdownResult();
            }

            void DrainTerminalRenderRequests() override
            {
                m_resources.DrainTerminalRenderRequests();
            }

        private:
            Resource::ResourceSubsystem& m_resources;
            RenderSubsystem& m_render;
            WindowSubsystem& m_window;
            std::function<bool()> m_stopRender;
            RenderFrameExtractor m_extractor;
        };
    } // namespace

    RenderRuntimeComposition::RenderRuntimeComposition(
        RenderRuntimeConfig config,
        RenderFrameSettings initialSettings,
        std::unique_ptr<IRenderRuntimeCompositionServices> services)
        : m_config(std::move(config)),
          m_settings(std::move(initialSettings)),
          m_services(std::move(services))
    {
        m_stats.temporalEpoch = m_temporalEpoch;
    }

    bool RenderRuntimeComposition::PrepareBeforeSubsystemInitialization()
    {
        if (m_prepared)
        {
            return true;
        }
        if (m_services == nullptr ||
            !IsValidRenderFrameSettings(m_settings))
        {
            return false;
        }
        m_services->InjectRenderResourceGateway();
        m_prepared = true;
        return true;
    }

    void RenderRuntimeComposition::BeforeRenderSubsystemInitialize()
    {
        if (!m_prepared || m_services == nullptr)
        {
            throw std::logic_error(
                "Render runtime composition was not prepared before initialization");
        }
        if (!m_services->IsWindowInitialized())
        {
            throw std::runtime_error(
                "WindowSubsystem must be initialized before RenderSubsystem");
        }

        NativeSurfaceDesc surface = m_services->CaptureRenderSurface();
        const RHIBackendType backend = ResolveBackend();
        if (!surface.IsValidFor(backend))
        {
            throw std::runtime_error(
                "WindowSubsystem returned an invalid initial render surface");
        }
        if (backend == RHIBackendType::OpenGL)
        {
            m_services->ReleaseGraphicsContextFromUpdateThread();
        }
        m_services->ConfigureRender(m_config, surface);
        m_surface = surface;
        m_stats.surfaceGeneration = surface.generation;
        m_renderConfigured = true;
    }

    void RenderRuntimeComposition::TickAfterWorlds(
        World* activeWorld,
        float32 deltaTime,
        float32 absoluteTime)
    {
        if (m_shutdown || !m_renderConfigured || m_services == nullptr)
        {
            return;
        }

        AcknowledgePublishedOneShotValues();
        RouteSurfaceUpdate();
        if (activeWorld == nullptr || !m_services->IsRenderReady())
        {
            return;
        }

        RenderRuntimeCompositionFrameInput input;
        input.sequence = m_nextFrameSequence++;
        input.worldRevision = m_worldRevision;
        input.temporalEpoch = m_temporalEpoch;
        input.explicitDiscontinuity = m_temporalResetPending;
        input.view.viewportWidth = m_surface.width;
        input.view.viewportHeight = m_surface.height;
        input.view.absoluteTime = absoluteTime;
        input.view.deltaTime = deltaTime;
        input.settings = m_settings;
        input.settings.temporal.resetHistory =
            m_temporalResetPending || m_settings.temporal.resetHistory;
        input.captureRequest = m_pendingCapture;
        input.world = activeWorld;

        ++m_stats.extractionAttempts;
        m_stats.lastAttemptedSequence = input.sequence;
        RenderFrameExtractionResult extraction =
            m_services->ExtractFrame(input);
        m_stats.lastExtractionCode = extraction.code;
        if (!extraction.IsComplete())
        {
            return;
        }

        ++m_stats.extractionCompleted;
        RenderFramePublishResult publication =
            m_services->PublishFrame(std::move(extraction.packet));
        m_stats.lastPublishResult = publication;
        if (publication.code == RenderFramePublishCode::Accepted ||
            publication.code == RenderFramePublishCode::ReplacedOlder)
        {
            ++m_stats.publicationAccepted;
            if (input.explicitDiscontinuity ||
                input.settings.temporal.resetHistory)
            {
                m_temporalResetPublishedSequence = publication.sequence;
            }
        }
        else
        {
            ++m_stats.publicationRejected;
        }
    }

    void RenderRuntimeComposition::OnActiveWorldChanged() noexcept
    {
        ++m_worldRevision;
        m_stats.worldRevision = m_worldRevision;
        m_temporalResetPending = true;
    }

    uint64 RenderRuntimeComposition::RequestTemporalReset() noexcept
    {
        ++m_temporalEpoch;
        m_stats.temporalEpoch = m_temporalEpoch;
        m_temporalResetPending = true;
        return m_temporalEpoch;
    }

    bool RenderRuntimeComposition::SetFrameSettings(
        const RenderFrameSettings& settings) noexcept
    {
        if (!IsValidRenderFrameSettings(settings))
        {
            return false;
        }
        m_settings = settings;
        return true;
    }

    bool RenderRuntimeComposition::QueueCapture(
        const RenderFrameCaptureRequest& request) noexcept
    {
        if (!IsCaptureRequestValid(request) ||
            m_pendingCapture.requestId != 0 ||
            request.requestId <= m_lastAcknowledgedCaptureRequestId)
        {
            return false;
        }
        m_pendingCapture = request;
        return true;
    }

    bool RenderRuntimeComposition::RequestSurfaceResize(uint32 width,
                                                        uint32 height)
    {
        if (m_shutdown || !m_renderConfigured || m_services == nullptr ||
            width == 0 || height == 0)
        {
            return false;
        }

        NativeSurfaceDesc surface = m_surface;
        surface.width = width;
        surface.height = height;
        ++surface.generation;
        ++m_stats.resizeRequests;
        const RenderResizeResult result =
            m_services->RequestResize(surface);
        m_stats.lastResizeResult = result;
        if (result.code != RenderResizeCode::Accepted &&
            result.code != RenderResizeCode::CoalescedOlder)
        {
            return false;
        }
        m_surface = surface;
        m_stats.surfaceGeneration = surface.generation;
        m_temporalResetPending = true;
        return true;
    }

    RenderShutdownResult RenderRuntimeComposition::Shutdown()
    {
        if (m_shutdown)
        {
            return m_stats.lastShutdownResult;
        }
        m_shutdown = true;
        if (m_services == nullptr)
        {
            return m_stats.lastShutdownResult;
        }

        m_services->BeginResourceShutdown();
        m_stats.lastShutdownResult = m_services->StopRender();
        m_services->DrainTerminalRenderRequests();
        return m_stats.lastShutdownResult;
    }

    bool RenderRuntimeComposition::IsCaptureRequestValid(
        const RenderFrameCaptureRequest& request) noexcept
    {
        return request.kind != RenderFrameCaptureKind::None &&
               IsValidRenderFrameCaptureRequest(request);
    }

    RHIBackendType RenderRuntimeComposition::ResolveBackend() const noexcept
    {
        return m_config.backendType == RHIBackendType::Auto
                   ? SelectBestBackend()
                   : m_config.backendType;
    }

    void RenderRuntimeComposition::AcknowledgePublishedOneShotValues()
    {
        if (m_pendingCapture.requestId == 0 &&
            m_temporalResetPublishedSequence == 0)
        {
            return;
        }

        const RenderDiagnosticsSnapshot diagnostics =
            m_services->GetRenderDiagnostics();
        if (m_pendingCapture.requestId != 0 &&
            diagnostics.lastCapture.requestId ==
                m_pendingCapture.requestId &&
            diagnostics.lastCapture.code !=
                RenderFrameCaptureResultCode::None)
        {
            m_lastAcknowledgedCaptureRequestId =
                m_pendingCapture.requestId;
            m_pendingCapture = {};
        }
        if (m_temporalResetPublishedSequence != 0 &&
            diagnostics.lastAppliedFrameSequence >=
                m_temporalResetPublishedSequence)
        {
            m_temporalResetPending = false;
            m_settings.temporal.resetHistory = false;
            m_temporalResetPublishedSequence = 0;
        }
    }

    void RenderRuntimeComposition::RouteSurfaceUpdate()
    {
        const NativeSurfaceDesc surface = m_services->CaptureRenderSurface();
        if (surface.generation <= m_surface.generation)
        {
            return;
        }

        ++m_stats.resizeRequests;
        const RenderResizeResult result = m_services->RequestResize(surface);
        m_stats.lastResizeResult = result;
        if (result.code == RenderResizeCode::Accepted ||
            result.code == RenderResizeCode::CoalescedOlder)
        {
            m_surface = surface;
            m_stats.surfaceGeneration = surface.generation;
        }
    }

    std::unique_ptr<IRenderRuntimeCompositionServices>
        CreateEngineRenderRuntimeCompositionServices(
            Resource::ResourceSubsystem& resources,
            RenderSubsystem& render,
            WindowSubsystem& window,
            std::function<bool()> stopRender)
    {
        return std::make_unique<EngineRenderRuntimeCompositionServices>(
            resources,
            render,
            window,
            std::move(stopRender));
    }

} // namespace RVX
