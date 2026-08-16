#pragma once

/**
 * @file EcsRenderRuntimeComposition.h
 * @brief Engine-global owner for the pure ECS frozen-scene render path.
 */

#include "Core/Types.h"
#include "EcsRenderFramePipeline.h"
#include "Render/RenderDiagnostics.h"
#include "Render/RenderRuntimeTypes.h"
#include "RenderContracts/RenderFrameTypes.h"
#include "RHI/RHINativeSurface.h"
#include "Scene/ECS/FrozenSceneSnapshot.h"

#include <functional>
#include <memory>
#include <optional>

namespace RVX
{
    class RenderSubsystem;
    class WindowSubsystem;

    namespace Resource
    {
        class ResourceSubsystem;
    }

    /** @brief Exact value evidence for the latest reliable ECS frame accepted by Render. */
    struct EcsRenderAcceptedFrameEvidence
    {
        ECS::SceneRuntimeId sceneRuntimeId;
        uint64 frozenSnapshotRevision = 0;
        uint64 targetRenderSceneRevision = 0;
        uint64 carryingFrameSequence = 0;
        ECS::EntityHandle selectedCamera = ECS::EntityHandle::Invalid();
        uint64 selectedCameraCutRevision = 0;
        EcsFrozenSceneBridgeResult bridge{};
        RenderExtractionDiagnostics extraction{};

        [[nodiscard]] bool IsValid() const noexcept
        {
            return sceneRuntimeId.IsValid() && frozenSnapshotRevision != 0 &&
                   targetRenderSceneRevision != 0 && carryingFrameSequence != 0 &&
                   selectedCamera.IsValid() &&
                   bridge.code == EcsFrozenSceneBridgeCode::Complete &&
                   extraction.complete;
        }
    };

    /** @brief Engine-facing value diagnostics for the isolated ECS render owner. */
    struct EcsRenderRuntimeCompositionStats
    {
        uint64 publicationAttempts = 0;
        uint64 publicationAccepted = 0;
        uint64 publicationSceneOnly = 0;
        uint64 publicationRejected = 0;
        uint64 completedProgressRejected = 0;
        uint64 resizeRequests = 0;
        uint64 worldRevision = 0;
        uint64 temporalEpoch = 1;
        uint64 temporalResetCount = 0;
        uint64 lastAttemptedSequence = 0;
        uint64 lastAcknowledgedCaptureRequestId = 0;
        uint64 pendingCaptureFrameSequence = 0;
        uint64 pendingTemporalFrameSequence = 0;
        uint64 surfaceGeneration = 0;
        uint64 shutdownStopAttempts = 0;
        uint64 shutdownStopFailures = 0;
        uint64 terminalDrainAttempts = 0;
        EcsRenderFramePipelineCode lastPipelineCode =
            EcsRenderFramePipelineCode::BridgeFailed;
        EcsSceneRenderPublicationProgress completedProgress{};
        RenderResizeResult lastResizeResult{};
        RenderShutdownResult lastShutdownResult{};
        /** Never inferred from attempts, rejected candidates, or scene-only publication. */
        std::optional<EcsRenderAcceptedFrameEvidence> lastAcceptedFrame;
        /** Exact receipt of the latest scene-only transport disposition, if any. */
        std::optional<EcsRenderFramePublicationReceipt> lastSceneUpdatePublication;
        bool prepared = false;
        bool renderConfigured = false;
        bool acceptingPublications = true;
        bool shutdownBlockedByOutstandingProofs = false;
        bool shutdownComplete = false;
    };

    /**
     * @brief Narrow subsystem seam needed by the ECS render owner.
     *
     * No World, Scene, Actor, legacy extractor, or retirement object may cross
     * this boundary.  Every frame path is a frozen snapshot plus immutable
     * render packets owned by EcsRenderFramePipeline.
     */
    class IEcsRenderRuntimeCompositionServices
    {
    public:
        virtual ~IEcsRenderRuntimeCompositionServices() = default;

        virtual void InjectRenderResourceGateway() = 0;
        [[nodiscard]] virtual bool IsWindowInitialized() const noexcept = 0;
        [[nodiscard]] virtual NativeSurfaceDesc CaptureRenderSurface() = 0;
        virtual void ReleaseGraphicsContextFromUpdateThread() = 0;
        virtual void ConfigureRender(const RenderRuntimeConfig& config,
                                     const NativeSurfaceDesc& surface) = 0;
        [[nodiscard]] virtual bool IsRenderReady() const noexcept = 0;
        [[nodiscard]] virtual IEcsRenderFrameTransport& GetFrameTransport() noexcept = 0;
        [[nodiscard]] virtual EcsSceneRenderPublicationProgress
        ReadCompletedProgress() const noexcept = 0;
        /** @brief Latest exact terminal capture value observed from Render. */
        [[nodiscard]] virtual RenderFrameCaptureResult
        ReadLastCaptureResult() const noexcept = 0;
        [[nodiscard]] virtual RenderResourceHandle ResolveRenderAsset(
            AssetId assetId,
            RenderResourceKind kind) const noexcept = 0;
        [[nodiscard]] virtual RenderResizeResult RequestResize(
            const NativeSurfaceDesc& surface) = 0;
        /** @brief Request a native resize; Window owns the resulting generation. */
        [[nodiscard]] virtual bool RequestWindowResize(uint32 width,
                                                        uint32 height) = 0;
        virtual void BeginResourceShutdown() = 0;
        [[nodiscard]] virtual RenderShutdownResult StopRender() = 0;
        virtual void DrainTerminalRenderRequests() = 0;
    };

    /**
     * @brief One Engine-global composition that publishes only frozen ECS snapshots.
     *
     * Per-world composition owns all Scene mutation and retirement progress. It
     * must call CanStop() and drain its own work before this owner is allowed to
     * stop Render.  This class independently prevents premature Render stop
     * while its exact ECS presentation-proof gateway retains any token.
     */
    class EcsRenderRuntimeComposition final : public NonMovable
    {
    public:
        EcsRenderRuntimeComposition(
            RenderRuntimeConfig config,
            RenderFrameSettings initialSettings,
            std::unique_ptr<IEcsRenderRuntimeCompositionServices> services);
        ~EcsRenderRuntimeComposition();

        [[nodiscard]] bool PrepareBeforeSubsystemInitialization();
        void BeforeRenderSubsystemInitialize();

        /**
         * @brief Poll Render completion then publish the active immutable snapshot.
         * @return A terminal pipeline result when a snapshot was attempted;
         *         nullopt for no active snapshot, a stopped owner, or an
         *         unavailable renderer.
         */
        [[nodiscard]] std::optional<EcsRenderFramePipelineResult> Tick(
            const SceneECS::FrozenSceneSnapshot* activeSnapshot,
            float32 deltaTime,
            float32 absoluteTime);

        /** @brief Observe Render completion even after BeginShutdown sealed publication. */
        [[nodiscard]] bool PollCompletedProgress();

        [[nodiscard]] uint64 RequestTemporalReset() noexcept;
        [[nodiscard]] bool SetFrameSettings(const RenderFrameSettings& settings) noexcept;
        [[nodiscard]] const RenderFrameSettings& GetFrameSettings() const noexcept
        {
            return m_settings;
        }
        [[nodiscard]] bool QueueCapture(const RenderFrameCaptureRequest& request) noexcept;
        [[nodiscard]] bool RequestSurfaceResize(uint32 width, uint32 height);

        /** Seal new publication.  Safe Render stop still waits for exact proofs. */
        void BeginShutdown() noexcept;
        /** @brief True only after publication is sealed and all ECS proofs are acknowledged. */
        [[nodiscard]] bool CanStop() const noexcept;
        [[nodiscard]] bool HasOutstandingProofs() const noexcept;
        /**
         * @brief Execute ordered Resource -> Render -> terminal-drain shutdown.
         * @return False without side effects while ECS proof ownership remains.
         */
        [[nodiscard]] bool TryShutdown();

        [[nodiscard]] EcsRenderFramePipeline& GetPipeline() noexcept
        {
            return *m_pipeline;
        }
        [[nodiscard]] const EcsRenderFramePipeline& GetPipeline() const noexcept
        {
            return *m_pipeline;
        }
        [[nodiscard]] const EcsRenderRuntimeCompositionStats& GetStats() const noexcept
        {
            return m_stats;
        }

    private:
        struct CameraFingerprint
        {
            ECS::SceneRuntimeId sceneRuntimeId;
            ECS::EntityHandle entity = ECS::EntityHandle::Invalid();
            uint64 cutRevision = 0;

            bool operator==(const CameraFingerprint&) const = default;
        };

        [[nodiscard]] static bool IsCaptureRequestValid(
            const RenderFrameCaptureRequest& request) noexcept;
        [[nodiscard]] static bool IsTerminalShutdown(
            RenderShutdownCode code) noexcept;
        [[nodiscard]] static bool IsTerminalCapture(
            RenderFrameCaptureResultCode code) noexcept;
        [[nodiscard]] static RenderResourceHandle ResolveAsset(
            void* context,
            AssetId assetId,
            RenderResourceKind kind) noexcept;
        [[nodiscard]] RHIBackendType ResolveBackend() const noexcept;
        [[nodiscard]] std::optional<CameraFingerprint> GetActiveCamera(
            const SceneECS::FrozenSceneSnapshot& snapshot) const noexcept;
        void ObserveSnapshotIdentity(const SceneECS::FrozenSceneSnapshot& snapshot) noexcept;
        [[nodiscard]] bool ObserveWindowSurface();
        void ObservePresentedOneShotValues() noexcept;
        void RecordAcceptedOneShotValues(
            const EcsRenderFramePipelineResult& result,
            uint64 inputTemporalEpoch) noexcept;
        void RecordReliablePublicationEvidence(
            const SceneECS::FrozenSceneSnapshot& snapshot,
            const EcsRenderFramePipelineResult& result) noexcept;

        RenderRuntimeConfig m_config{};
        RenderFrameSettings m_settings{};
        RenderFrameCaptureRequest m_pendingCapture{};
        std::unique_ptr<IEcsRenderRuntimeCompositionServices> m_services;
        std::unique_ptr<EcsRenderFramePipeline> m_pipeline;
        NativeSurfaceDesc m_surface{};
        EcsRenderRuntimeCompositionStats m_stats{};
        std::optional<CameraFingerprint> m_activeCamera;
        ECS::SceneRuntimeId m_activeSceneRuntimeId;
        uint64 m_nextFrameSequence = 1;
        uint64 m_temporalEpoch = 1;
        uint64 m_pendingTemporalFrameSequence = 0;
        uint64 m_pendingTemporalEpoch = 0;
        uint64 m_pendingCaptureFrameSequence = 0;
        bool m_temporalResetPending = false;
        bool m_prepared = false;
        bool m_renderConfigured = false;
        bool m_acceptingPublications = true;
        bool m_shutdownRequested = false;
        bool m_resourceShutdownBegun = false;
        bool m_shutdownComplete = false;
    };

    [[nodiscard]] std::unique_ptr<IEcsRenderRuntimeCompositionServices>
    CreateEngineEcsRenderRuntimeCompositionServices(
        Resource::ResourceSubsystem& resources,
        RenderSubsystem& render,
        WindowSubsystem& window,
        std::function<bool()> stopRender);
} // namespace RVX
