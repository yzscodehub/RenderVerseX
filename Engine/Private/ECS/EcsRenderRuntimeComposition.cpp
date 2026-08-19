/**
 * @file EcsRenderRuntimeComposition.cpp
 * @brief Engine-global pure ECS frozen-scene render composition.
 */

#include "EcsRenderRuntimeComposition.h"

#include "Core/Assert.h"
#include "Render/RenderDiagnostics.h"
#include "Render/RenderSubsystem.h"
#include "RenderContracts/RenderFrameValidation.h"
#include "Resource/ResourceSubsystem.h"
#include "Runtime/Window/WindowSubsystem.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <stdexcept>
#include <utility>

namespace RVX
{
namespace
{
    class EngineEcsRenderRuntimeCompositionServices final
        : public IEcsRenderRuntimeCompositionServices,
          public NonMovable
    {
    public:
        EngineEcsRenderRuntimeCompositionServices(
            Resource::ResourceSubsystem& resources,
            RenderSubsystem& render,
            WindowSubsystem& window,
            std::function<bool()> stopRender)
            : m_resources(resources),
              m_render(render),
              m_window(window),
              m_stopRender(std::move(stopRender)),
              m_transport(render)
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

        IEcsRenderFrameTransport& GetFrameTransport() noexcept override
        {
            return m_transport;
        }

        EcsSceneRenderPublicationProgress
        ReadCompletedProgress() const noexcept override
        {
            const RenderDiagnosticsSnapshot diagnostics =
                m_render.GetDiagnosticsSnapshot();
            const RenderRuntimeResult runtime = m_render.GetLastRuntimeResult();
            return {
                .appliedRenderSceneRevision = diagnostics.sceneValues.available
                                                  ? diagnostics.sceneValues.appliedSceneRevision
                                                  : 0,
                .presentedFrameSequence = diagnostics.lastPresentedFrameSequence,
                .deviceLost = runtime.code == RenderRuntimeCode::DeviceLost ||
                              (diagnostics.lastFailure.available &&
                               diagnostics.lastFailure.runtime.code ==
                                   RenderRuntimeCode::DeviceLost),
            };
        }

        RenderFrameCaptureResult ReadLastCaptureResult() const noexcept override
        {
            return m_render.GetDiagnosticsSnapshot().lastCapture;
        }

        RenderResourceHandle ResolveRenderAsset(
            AssetId assetId,
            RenderResourceKind kind) const noexcept override
        {
            const Resource::RenderResourceResolveResult resolved =
                m_resources.ResolveRenderResource(assetId, kind);
            return resolved.code == Resource::RenderResourceResolveCode::Resolved &&
                           resolved.handle.IsValid() &&
                           resolved.status.code == RenderResourceStatusCode::Current &&
                           resolved.status.state == RenderResourcePublicState::GPUReady
                       ? resolved.handle
                       : RenderResourceHandle{};
        }

        RenderResizeResult RequestResize(const NativeSurfaceDesc& surface) override
        {
            return m_render.RequestResize(surface);
        }

        bool RequestWindowResize(uint32 width, uint32 height) override
        {
            return m_window.RequestResize(width, height);
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
        EcsRenderSubsystemFrameTransport m_transport;
    };
} // namespace

EcsRenderRuntimeComposition::EcsRenderRuntimeComposition(
    RenderRuntimeConfig config,
    RenderFrameSettings initialSettings,
    std::unique_ptr<IEcsRenderRuntimeCompositionServices> services)
    : m_config(std::move(config)),
      m_settings(std::move(initialSettings)),
      m_services(std::move(services))
{
    if (m_services == nullptr || !IsValidRenderFrameSettings(m_settings))
    {
        throw std::invalid_argument(
            "EcsRenderRuntimeComposition requires services and valid frame settings");
    }
    m_pipeline = std::make_unique<EcsRenderFramePipeline>(
        m_services->GetFrameTransport());
    if (m_settings.temporal.resetHistory)
    {
        m_settings.temporal.resetHistory = false;
        static_cast<void>(RequestTemporalReset());
    }
    m_stats.temporalEpoch = m_temporalEpoch;
}

EcsRenderRuntimeComposition::~EcsRenderRuntimeComposition()
{
    // Proof ownership is value-owned by the pipeline.  Releasing this Engine
    // owner while a coordinator can still hold an exact proof token would
    // silently discard the only evidence that its Render removal presented.
    RVX_ASSERT_MSG(!HasOutstandingProofs(),
                   "ECS render owner destroyed with outstanding presentation proofs");
    RVX_ASSERT_MSG(!m_shutdownRequested || m_shutdownComplete,
                   "ECS render owner destroyed after an incomplete shutdown, last code={}",
                   static_cast<uint32>(m_stats.lastShutdownResult.code));
    RVX_ASSERT_MSG(!m_renderConfigured || m_shutdownComplete,
                   "Configured ECS render owner destroyed before terminal shutdown");
}

bool EcsRenderRuntimeComposition::PrepareBeforeSubsystemInitialization()
{
    if (m_prepared || m_services == nullptr || m_shutdownRequested)
    {
        return false;
    }
    m_services->InjectRenderResourceGateway();
    m_prepared = true;
    m_stats.prepared = true;
    return true;
}

void EcsRenderRuntimeComposition::BeforeRenderSubsystemInitialize()
{
    if (!m_prepared || m_services == nullptr)
    {
        throw std::logic_error(
            "ECS render runtime composition was not prepared before initialization");
    }
    if (m_renderConfigured || m_shutdownRequested)
    {
        return;
    }
    if (!m_services->IsWindowInitialized())
    {
        throw std::runtime_error(
            "WindowSubsystem must be initialized before RenderSubsystem");
    }
    const NativeSurfaceDesc surface = m_services->CaptureRenderSurface();
    if (!surface.IsValidFor(ResolveBackend()))
    {
        throw std::runtime_error(
            "WindowSubsystem returned an invalid initial render surface");
    }
    if (ResolveBackend() == RHIBackendType::OpenGL)
    {
        m_services->ReleaseGraphicsContextFromUpdateThread();
    }
    m_services->ConfigureRender(m_config, surface);
    m_surface = surface;
    m_renderConfigured = true;
    m_stats.renderConfigured = true;
    m_stats.surfaceGeneration = m_surface.generation;
}

std::optional<EcsRenderFramePipelineResult> EcsRenderRuntimeComposition::Tick(
    const SceneECS::FrozenSceneSnapshot* activeSnapshot,
    float32 deltaTime,
    float32 absoluteTime)
{
    if (!m_renderConfigured || m_services == nullptr || m_pipeline == nullptr)
    {
        return std::nullopt;
    }
    if (!PollCompletedProgress())
    {
        return std::nullopt;
    }
    if (m_acceptingPublications && !ObserveWindowSurface())
    {
        return std::nullopt;
    }
    if (!m_acceptingPublications || activeSnapshot == nullptr ||
        !m_services->IsRenderReady())
    {
        return std::nullopt;
    }

    ObserveSnapshotIdentity(*activeSnapshot);
    EcsFrameExtractionInput input;
    input.sequence = m_nextFrameSequence++;
    input.worldRevision = m_stats.worldRevision;
    input.temporalEpoch = m_temporalEpoch;
    input.explicitDiscontinuity = m_temporalResetPending;
    input.outputWidth = m_surface.width;
    input.outputHeight = m_surface.height;
    input.absoluteTime = absoluteTime;
    input.deltaTime = deltaTime;
    input.settings = m_settings;
    input.settings.temporal.resetHistory =
        m_temporalResetPending || m_settings.temporal.resetHistory;
    input.captureRequest = m_pendingCapture;
    input.assetResolver = {.context = m_services.get(), .resolve = &ResolveAsset};

    ++m_stats.publicationAttempts;
    m_stats.lastAttemptedSequence = input.sequence;
    EcsRenderFramePipelineResult result = m_pipeline->Publish(*activeSnapshot, input);
    m_stats.lastPipelineCode = result.code;
    m_stats.completedProgress = m_pipeline->GetCompletedProgress();
    if (result.code == EcsRenderFramePipelineCode::Published)
    {
        ++m_stats.publicationAccepted;
        RecordReliablePublicationEvidence(*activeSnapshot, result);
        RecordAcceptedOneShotValues(result, input.temporalEpoch);
        ObservePresentedOneShotValues();
    }
    else if (result.code ==
             EcsRenderFramePipelineCode::SceneUpdatePublishedWithoutFrame)
    {
        ++m_stats.publicationSceneOnly;
        if (result.publication.has_value() &&
            result.publication->IsValid() &&
            result.publication->disposition ==
                EcsFramePublicationDisposition::SceneUpdateAcceptedWithoutFrame)
        {
            m_stats.lastSceneUpdatePublication = result.publication;
        }
    }
    else
    {
        ++m_stats.publicationRejected;
    }
    return result;
}

bool EcsRenderRuntimeComposition::PollCompletedProgress()
{
    if (!m_renderConfigured || m_services == nullptr || m_pipeline == nullptr)
    {
        return false;
    }
    const EcsSceneRenderPublicationProgress completed =
        m_services->ReadCompletedProgress();
    if (!m_pipeline->ObserveCompletedProgress(completed))
    {
        ++m_stats.completedProgressRejected;
        return false;
    }
    m_stats.completedProgress = completed;
    ObservePresentedOneShotValues();
    return true;
}

uint64 EcsRenderRuntimeComposition::RequestTemporalReset() noexcept
{
    if (m_temporalEpoch != std::numeric_limits<uint64>::max())
    {
        ++m_temporalEpoch;
        ++m_stats.temporalResetCount;
    }
    m_stats.temporalEpoch = m_temporalEpoch;
    m_temporalResetPending = true;
    m_pendingTemporalEpoch = m_temporalEpoch;
    m_pendingTemporalFrameSequence = 0;
    m_stats.pendingTemporalFrameSequence = 0;
    return m_temporalEpoch;
}

bool EcsRenderRuntimeComposition::SetFrameSettings(
    const RenderFrameSettings& settings) noexcept
{
    if (!IsValidRenderFrameSettings(settings) || m_shutdownRequested)
    {
        return false;
    }
    m_settings = settings;
    const bool requestTemporalReset = m_settings.temporal.resetHistory;
    m_settings.temporal.resetHistory = false;
    if (requestTemporalReset)
    {
        static_cast<void>(RequestTemporalReset());
    }
    return true;
}

bool EcsRenderRuntimeComposition::QueueCapture(
    const RenderFrameCaptureRequest& request) noexcept
{
    if (m_shutdownRequested || !IsCaptureRequestValid(request) ||
        m_pendingCapture.requestId != 0 ||
        request.requestId <= m_stats.lastAcknowledgedCaptureRequestId)
    {
        return false;
    }
    m_pendingCapture = request;
    return true;
}

bool EcsRenderRuntimeComposition::RequestSurfaceResize(uint32 width, uint32 height)
{
    if (!m_renderConfigured || m_shutdownRequested || m_services == nullptr ||
        width == 0 || height == 0)
    {
        return false;
    }
    ++m_stats.resizeRequests;
    return m_services->RequestWindowResize(width, height);
}

void EcsRenderRuntimeComposition::BeginShutdown() noexcept
{
    m_shutdownRequested = true;
    m_acceptingPublications = false;
    m_stats.acceptingPublications = false;
}

bool EcsRenderRuntimeComposition::CanStop() const noexcept
{
    return m_shutdownRequested && !HasOutstandingProofs();
}

bool EcsRenderRuntimeComposition::HasOutstandingProofs() const noexcept
{
    return m_pipeline != nullptr && m_pipeline->HasOutstandingProofs();
}

bool EcsRenderRuntimeComposition::TryShutdown()
{
    BeginShutdown();
    if (m_shutdownComplete)
    {
        return true;
    }
    static_cast<void>(PollCompletedProgress());
    if (!CanStop())
    {
        m_stats.shutdownBlockedByOutstandingProofs = true;
        return false;
    }
    if (m_services == nullptr)
    {
        return false;
    }
    if (!m_resourceShutdownBegun)
    {
        m_services->BeginResourceShutdown();
        m_resourceShutdownBegun = true;
    }
    ++m_stats.shutdownStopAttempts;
    m_stats.lastShutdownResult = m_services->StopRender();
    // Resource owns retained request payloads.  It must see the exact Render
    // terminal/failure result for every stop attempt before this owner either
    // retries or records a successful terminal stop.
    m_services->DrainTerminalRenderRequests();
    ++m_stats.terminalDrainAttempts;
    // InitializeAll can unwind a later failing subsystem after this owner has
    // configured Render but before a dedicated render runtime becomes ready.
    // In that narrow rollback state, None is an exact "never started" result,
    // not an incomplete terminal proof. Treat it as stopped only while the
    // service confirms Render is no longer ready.
    if (m_stats.lastShutdownResult.code == RenderShutdownCode::None &&
        !m_services->IsRenderReady())
    {
        m_stats.lastShutdownResult.code = RenderShutdownCode::AlreadyStopped;
    }
    if (!IsTerminalShutdown(m_stats.lastShutdownResult.code))
    {
        ++m_stats.shutdownStopFailures;
        return false;
    }
    m_stats.shutdownBlockedByOutstandingProofs = false;
    m_shutdownComplete = true;
    m_stats.shutdownComplete = true;
    return true;
}

bool EcsRenderRuntimeComposition::IsCaptureRequestValid(
    const RenderFrameCaptureRequest& request) noexcept
{
    return request.kind != RenderFrameCaptureKind::None &&
           IsValidRenderFrameCaptureRequest(request);
}

bool EcsRenderRuntimeComposition::IsTerminalCapture(
    RenderFrameCaptureResultCode code) noexcept
{
    return code >= RenderFrameCaptureResultCode::Completed &&
           code <= RenderFrameCaptureResultCode::MapFailed;
}

bool EcsRenderRuntimeComposition::IsTerminalShutdown(
    RenderShutdownCode code) noexcept
{
    return code == RenderShutdownCode::Completed ||
           code == RenderShutdownCode::AlreadyStopped ||
           code == RenderShutdownCode::DeviceLost;
}

RenderResourceHandle EcsRenderRuntimeComposition::ResolveAsset(
    void* context,
    AssetId assetId,
    RenderResourceKind kind) noexcept
{
    const auto* services =
        static_cast<const IEcsRenderRuntimeCompositionServices*>(context);
    return services != nullptr ? services->ResolveRenderAsset(assetId, kind) :
                               RenderResourceHandle{};
}

RHIBackendType EcsRenderRuntimeComposition::ResolveBackend() const noexcept
{
    return m_config.backendType == RHIBackendType::Auto ? SelectBestBackend() :
                                                          m_config.backendType;
}

std::optional<EcsRenderRuntimeComposition::CameraFingerprint>
EcsRenderRuntimeComposition::GetActiveCamera(
    const SceneECS::FrozenSceneSnapshot& snapshot) const noexcept
{
    if (!snapshot.sceneRuntimeId.IsValid() || !snapshot.selectedCamera.has_value() ||
        snapshot.selectedCamera->type != SceneECS::FrozenSceneObjectType::Camera ||
        snapshot.selectedCamera->sceneRuntimeId != snapshot.sceneRuntimeId ||
        !snapshot.selectedCamera->entity.IsValid())
    {
        return std::nullopt;
    }
    const auto selected = std::find_if(
        snapshot.cameras.begin(), snapshot.cameras.end(),
        [&snapshot](const SceneECS::FrozenSceneCamera& camera)
        {
            return camera.id == *snapshot.selectedCamera;
        });
    if (selected == snapshot.cameras.end())
    {
        return std::nullopt;
    }
    return CameraFingerprint{
        .sceneRuntimeId = snapshot.sceneRuntimeId,
        .entity = selected->id.entity,
        .cutRevision = selected->source.cutRevision,
    };
}

void EcsRenderRuntimeComposition::ObserveSnapshotIdentity(
    const SceneECS::FrozenSceneSnapshot& snapshot) noexcept
{
    const std::optional<CameraFingerprint> nextCamera = GetActiveCamera(snapshot);
    const bool worldChanged = snapshot.sceneRuntimeId.IsValid() &&
                              snapshot.sceneRuntimeId != m_activeSceneRuntimeId;
    if (worldChanged)
    {
        m_activeSceneRuntimeId = snapshot.sceneRuntimeId;
        if (m_stats.worldRevision != std::numeric_limits<uint64>::max())
        {
            ++m_stats.worldRevision;
        }
        m_activeCamera = nextCamera;
        static_cast<void>(RequestTemporalReset());
        return;
    }
    if (nextCamera != m_activeCamera)
    {
        m_activeCamera = nextCamera;
        static_cast<void>(RequestTemporalReset());
    }
}

bool EcsRenderRuntimeComposition::ObserveWindowSurface()
{
    if (m_services == nullptr || !m_services->IsWindowInitialized())
    {
        return false;
    }
    const NativeSurfaceDesc observed = m_services->CaptureRenderSurface();
    if (!observed.IsValidFor(ResolveBackend()) ||
        observed.generation < m_surface.generation)
    {
        return false;
    }
    if (observed.generation == m_surface.generation)
    {
        return observed.width == m_surface.width &&
               observed.height == m_surface.height &&
               observed.nativeWindow == m_surface.nativeWindow &&
               observed.nativeDisplay == m_surface.nativeDisplay &&
               observed.nativeLayer == m_surface.nativeLayer &&
               observed.backendWindow == m_surface.backendWindow;
    }
    const RenderResizeResult receipt = m_services->RequestResize(observed);
    m_stats.lastResizeResult = receipt;
    if ((receipt.code != RenderResizeCode::Accepted &&
         receipt.code != RenderResizeCode::CoalescedOlder) ||
        receipt.generation != observed.generation)
    {
        return false;
    }
    m_surface = observed;
    m_stats.surfaceGeneration = observed.generation;
    static_cast<void>(RequestTemporalReset());
    return true;
}

void EcsRenderRuntimeComposition::RecordAcceptedOneShotValues(
    const EcsRenderFramePipelineResult& result,
    uint64 inputTemporalEpoch) noexcept
{
    if (!result.publication.has_value() ||
        result.publication->disposition != EcsFramePublicationDisposition::Accepted)
    {
        return;
    }
    const uint64 sequence = result.publication->carryingFrameSequence;
    if (m_temporalResetPending && inputTemporalEpoch == m_temporalEpoch)
    {
        m_pendingTemporalEpoch = inputTemporalEpoch;
        m_pendingTemporalFrameSequence = sequence;
        m_stats.pendingTemporalFrameSequence = sequence;
    }
    if (m_pendingCapture.requestId != 0)
    {
        m_pendingCaptureFrameSequence = sequence;
        m_stats.pendingCaptureFrameSequence = sequence;
    }
}

void EcsRenderRuntimeComposition::RecordReliablePublicationEvidence(
    const SceneECS::FrozenSceneSnapshot& snapshot,
    const EcsRenderFramePipelineResult& result) noexcept
{
    if (!result.publication.has_value() ||
        !result.publication->IsValid() ||
        result.publication->disposition != EcsFramePublicationDisposition::Accepted ||
        !result.completedExtractionDiagnostics.has_value() ||
        result.bridge.code != EcsFrozenSceneBridgeCode::Complete ||
        !result.completedExtractionDiagnostics->complete ||
        result.publication->sourceSceneRuntimeId != snapshot.sceneRuntimeId ||
        result.publication->frozenSourceSnapshotRevision != snapshot.revision)
    {
        return;
    }
    const std::optional<CameraFingerprint> selectedCamera =
        GetActiveCamera(snapshot);
    if (!selectedCamera.has_value())
    {
        return;
    }
    EcsRenderAcceptedFrameEvidence evidence;
    evidence.sceneRuntimeId = result.publication->sourceSceneRuntimeId;
    evidence.frozenSnapshotRevision =
        result.publication->frozenSourceSnapshotRevision;
    evidence.targetRenderSceneRevision =
        result.publication->targetRenderSceneRevision;
    evidence.carryingFrameSequence =
        result.publication->carryingFrameSequence;
    evidence.selectedCamera = selectedCamera->entity;
    evidence.selectedCameraCutRevision = selectedCamera->cutRevision;
    evidence.bridge = result.bridge;
    evidence.extraction = *result.completedExtractionDiagnostics;
    if (!evidence.IsValid())
    {
        return;
    }
    m_stats.lastAcceptedFrame = std::move(evidence);
}

void EcsRenderRuntimeComposition::ObservePresentedOneShotValues() noexcept
{
    if (m_pipeline == nullptr || m_services == nullptr)
    {
        return;
    }
    const EcsSceneRenderPublicationProgress progress =
        m_pipeline->GetCompletedProgress();
    if (m_temporalResetPending && m_pendingTemporalFrameSequence != 0 &&
        m_pendingTemporalEpoch == m_temporalEpoch &&
        progress.presentedFrameSequence >= m_pendingTemporalFrameSequence)
    {
        m_temporalResetPending = false;
        m_pendingTemporalFrameSequence = 0;
        m_pendingTemporalEpoch = 0;
        m_stats.pendingTemporalFrameSequence = 0;
    }
    if (m_pendingCapture.requestId == 0 || m_pendingCaptureFrameSequence == 0 ||
        progress.presentedFrameSequence < m_pendingCaptureFrameSequence)
    {
        return;
    }
    const RenderFrameCaptureResult capture = m_services->ReadLastCaptureResult();
    if (!IsTerminalCapture(capture.code) ||
        capture.requestId != m_pendingCapture.requestId ||
        capture.frameSequence != m_pendingCaptureFrameSequence)
    {
        return;
    }
    m_stats.lastAcknowledgedCaptureRequestId = m_pendingCapture.requestId;
    m_pendingCapture = {};
    m_pendingCaptureFrameSequence = 0;
    m_stats.pendingCaptureFrameSequence = 0;
}

std::unique_ptr<IEcsRenderRuntimeCompositionServices>
CreateEngineEcsRenderRuntimeCompositionServices(
    Resource::ResourceSubsystem& resources,
    RenderSubsystem& render,
    WindowSubsystem& window,
    std::function<bool()> stopRender)
{
    return std::make_unique<EngineEcsRenderRuntimeCompositionServices>(
        resources, render, window, std::move(stopRender));
}
} // namespace RVX
