#pragma once

/**
 * @file RenderThreadRuntime.h
 * @brief Private render-thread runtime shell and frame-consumer seam.
 */

#include "Render/RenderDiagnostics.h"
#include "RenderContracts/IRenderResourceGateway.h"
#include "RenderContracts/RenderFramePacket.h"
#include "RHI/RHINativeSurface.h"
#include "Runtime/IRenderExecutor.h"
#include "Runtime/RenderControlMailbox.h"
#include "Runtime/RenderDiagnosticsPublisher.h"
#include "Runtime/RenderFrameMailbox.h"
#include "Runtime/RenderResourceGateway.h"
#include "Runtime/RenderThreadGuard.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace RVX
{
namespace RenderRuntimeDetail
{
    struct ReleaseDiagnosticsObservation
    {
        uint32 currentUsage = 0;
        uint32 oldestPendingGeneration = 0;
    };

    [[nodiscard]] constexpr ReleaseDiagnosticsObservation
        ObserveReleaseDiagnostics(uint64 accepted,
                                  uint64 dequeued,
                                  RenderReleaseQueueSnapshot queue) noexcept
    {
        (void)accepted;
        (void)dequeued;
        return ReleaseDiagnosticsObservation{
            queue.pendingCount,
            queue.pendingCount == 0U
                ? 0U
                : queue.oldestPendingGeneration};
    }
} // namespace RenderRuntimeDetail

    /** @brief Render-owned operations invoked only by the runtime owner thread. */
    class IRenderFrameConsumer
    {
    public:
        virtual ~IRenderFrameConsumer() = default;

        virtual RenderRuntimeResult Initialize(
            const RenderRuntimeConfig& config,
            const NativeSurfaceDesc& surface) = 0;
        virtual RenderRuntimeResult ApplySurface(
            const NativeSurfaceDesc& surface) = 0;
        virtual void ProcessRelease(RenderResourceHandle handle) = 0;
        virtual void ProcessUpload(
            const ResourceUploadRequestRef& request) = 0;
        virtual RenderRuntimeResult ConsumeFrame(
            const RenderFramePacket& packet) = 0;
        virtual void PollCompletion() = 0;
        virtual void RetireCompleted() = 0;
        virtual RenderShutdownResult Shutdown(
            RenderTeardownMode mode) noexcept = 0;
    };

    /** @brief Private lifecycle observation seam used around runtime publication. */
    class IRenderRuntimeLifecycleHook
    {
    public:
        virtual ~IRenderRuntimeLifecycleHook() = default;

        virtual void BeforeStartupAcknowledgement() noexcept = 0;
        virtual void DuringStartupPublication() noexcept
        {
        }
    };

    /** @brief Private process-fatal boundary for an unjoined render executor. */
    class IRenderFatalPolicy
    {
    public:
        virtual ~IRenderFatalPolicy() = default;

        virtual void Terminate(
            const RenderDiagnosticsSnapshot& diagnostics) = 0;
    };

    enum class RenderPublicationPath : uint8
    {
        Frame = 0,
        Resize = 1,
        Reserve = 2,
        Upload = 3,
        Release = 4
    };

    /** @brief Private observation seam after admission and before mutation. */
    class IRenderPublicationHook
    {
    public:
        virtual ~IRenderPublicationHook() = default;

        virtual void BeforeMutation(RenderPublicationPath path) noexcept = 0;
        virtual void BeforeSeal() noexcept
        {
        }
    };

    /** @brief Private latch at the wait predicate-to-registration boundary. */
    class IRenderWaitHook
    {
    public:
        virtual ~IRenderWaitHook() = default;

        virtual void AfterFalseWaitPredicate() noexcept = 0;
    };

    class RenderThreadRuntime final : public IRenderExecutorPump,
                                      public IRenderResourceGateway,
                                      public NonMovable
    {
    public:
        RenderThreadRuntime(RenderRuntimeConfig config,
                            NativeSurfaceDesc surface,
                            RenderExecutorKind executorKind,
                            std::unique_ptr<IRenderExecutor> executor,
                            std::unique_ptr<IRenderFrameConsumer> consumer,
                            std::shared_ptr<IRenderRuntimeLifecycleHook>
                                lifecycleHook = nullptr,
                            std::shared_ptr<IRenderFatalPolicy> fatalPolicy =
                                nullptr,
                            std::shared_ptr<IRenderPublicationHook>
                                publicationHook = nullptr,
                            std::shared_ptr<IRenderWaitHook> waitHook =
                                nullptr);
        ~RenderThreadRuntime() override;

        [[nodiscard]] RenderRuntimeResult Start();
        [[nodiscard]] RenderShutdownResult Stop();

        RenderFramePublishResult TryPublishFrame(
            std::unique_ptr<const RenderFramePacket> packet);
        RenderResizeResult RequestResize(const NativeSurfaceDesc& surface);
        [[nodiscard]] RenderDiagnosticsSnapshot
            GetDiagnosticsSnapshot() const;
        [[nodiscard]] RenderRuntimeResult GetLastRuntimeResult() const;
        [[nodiscard]] RenderShutdownResult GetLastShutdownResult() const;
        [[nodiscard]] bool IsReady() const;

        RenderResourceReserveResult ReserveResource(
            AssetId assetId,
            RenderResourceKind kind) noexcept override;
        RenderUploadEnqueueResult TryEnqueueUpload(
            const ResourceUploadRequestRef& request) noexcept override;
        RenderReleaseResult RequestRelease(
            RenderResourceHandle handle) noexcept override;
        [[nodiscard]] RenderResourceStatus QueryResourceStatus(
            RenderResourceHandle handle) const noexcept override;

        RenderPumpDecision PumpOnce() noexcept override;
        void WaitForWork() noexcept override;
        void Wake() noexcept override;
        void OnUnhandledExecutorException() noexcept override;

    private:
        friend struct RenderThreadRuntimeTestAccess;

        enum class StartupResolution : uint8
        {
            Pending = 0,
            RenderCompleted = 1,
            TimedOut = 2
        };

        using SurfaceControlMailbox =
            BasicRenderControlMailbox<NativeSurfaceDesc, NativeSurfaceDesc>;

        static void GatewayPublicationThunk(
            void* context,
            RenderGatewayPublicationPath path) noexcept;
        static void RuntimeFatalThunk(void* context,
                                      const char* message) noexcept;

        [[nodiscard]] bool IsConfigurationValid() const noexcept;
        [[nodiscard]] bool IsSurfaceValid(
            const NativeSurfaceDesc& surface) const noexcept;
        [[nodiscard]] bool IsRunningForPublication() const noexcept;
        [[nodiscard]] bool IsShuttingDownForPublication() const noexcept;
        [[nodiscard]] NativeSurfaceDesc GetCurrentSurfaceSnapshot() const;
        void NotifyExecutor() noexcept;
        void TransitionTo(RenderLifecycleState lifecycle,
                          const char* message);
        [[nodiscard]] bool TryClaimTerminalResult(
            const RenderRuntimeResult& result);
        [[nodiscard]] bool TryClaimTerminalResultLocked(
            const RenderRuntimeResult& result);
        [[nodiscard]] bool HasTerminalResult() const;
        void SealPublication() noexcept;
        void SealPublicationLocked() noexcept;
        void StoreRuntimeResult(RenderRuntimeResult result);
        void StoreShutdownResult(RenderShutdownResult result);
        void RecordFailure(const RenderRuntimeResult& result);
        void RecordFailure(const RenderShutdownResult& result);
        void PublishDiagnostics();
        [[noreturn]] void TerminateAfterFatalDiagnostics();
        RenderPumpDecision InitializeOnRenderThread();
        RenderPumpDecision FinishTimedOutStartupOnRenderThread();
        RenderPumpDecision StopOnRenderThread();
        RenderPumpDecision FailOnRenderThread(RenderRuntimeResult result);

        RenderRuntimeConfig m_config;
        NativeSurfaceDesc m_initialSurface;
        RenderExecutorKind m_executorKind = RenderExecutorKind::None;
        std::unique_ptr<IRenderExecutor> m_executor;
        std::unique_ptr<IRenderFrameConsumer> m_consumer;
        std::shared_ptr<IRenderRuntimeLifecycleHook> m_lifecycleHook;
        std::shared_ptr<IRenderFatalPolicy> m_fatalPolicy;
        std::shared_ptr<IRenderPublicationHook> m_publicationHook;
        std::shared_ptr<IRenderWaitHook> m_waitHook;
        std::unique_ptr<RenderFrameMailbox> m_frameMailbox;
        std::unique_ptr<SurfaceControlMailbox> m_controlMailbox;
        std::unique_ptr<RenderResourceGateway> m_resourceGateway;
        RenderDiagnosticsPublisher m_diagnosticsPublisher;
        RenderThreadGuard m_renderThreadGuard;

        std::atomic<RenderLifecycleState> m_lifecycle =
            RenderLifecycleState::Stopped;
        std::atomic<StartupResolution> m_startupResolution =
            StartupResolution::Pending;
        std::atomic<bool> m_startupAttempted = false;
        std::atomic<bool> m_started = false;
        std::atomic<bool> m_joined = false;
        std::atomic<bool> m_wakePending = false;
        std::atomic<bool> m_publicationSealed = false;
        std::atomic<bool> m_ownerFatalPending = false;

        mutable std::mutex m_stateMutex;
        std::timed_mutex m_startupArbitrationMutex;
        std::condition_variable m_startupCv;
        bool m_startupAcknowledged = false;
        bool m_terminalResultClaimed = false;
        RenderRuntimeResult m_lastRuntimeResult{};
        RenderShutdownResult m_lastShutdownResult{};
        RenderDiagnosticsSnapshot m_diagnosticsState{};

        // Startup arbitration precedes publication and state; publication
        // precedes state or a transport lock. Release-queue fatal callbacks
        // drop the queue lock before reaching state while publication remains
        // held. No state, wait, or transport owner may re-enter publication.
        mutable std::mutex m_publicationMutex;
        NativeSurfaceDesc m_currentSurface;
        uint64 m_latestResizeGeneration = 0;
        uint64 m_pendingResizeGeneration = 0;

        std::mutex m_waitMutex;
        std::condition_variable m_waitCv;

        std::atomic<uint64> m_publicationSequence = 0;
        std::atomic<uint64> m_pumpIterationCount = 0;
        std::atomic<uint64> m_idleWaitCount = 0;
        std::atomic<uint64> m_lastPublishedFrameSequence = 0;
        std::atomic<uint64> m_lastAcquiredFrameSequence = 0;
        std::atomic<uint64> m_lastAppliedFrameSequence = 0;
        std::atomic<uint64> m_lastSubmittedFrameSequence = 0;
        std::atomic<uint64> m_lastPresentedFrameSequence = 0;
        std::atomic<uint64> m_frameReplacementCount = 0;
        std::atomic<uint64> m_invalidFrameCount = 0;
        std::atomic<uint64> m_outOfOrderFrameCount = 0;
        std::atomic<uint64> m_resizeAcceptedCount = 0;
        std::atomic<uint64> m_resizeCoalescedCount = 0;
        std::atomic<uint64> m_resizeRejectedCount = 0;
        std::atomic<uint64> m_uploadAcceptedCount = 0;
        std::atomic<uint64> m_uploadPressureCount = 0;
        std::atomic<uint64> m_uploadCompletedCount = 0;
        std::atomic<uint64> m_uploadFailedCount = 0;
        std::atomic<uint64> m_releaseAcceptedCount = 0;
        std::atomic<uint64> m_releaseDequeuedCount = 0;
        std::atomic<uint64> m_releaseCompletedCount = 0;
        std::atomic<uint64> m_releaseStaleCount = 0;
    };
} // namespace RVX
