#include "Runtime/RenderThreadRuntime.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <utility>

namespace RVX
{
namespace
{
    template <typename Value>
    void AtomicMax(std::atomic<Value>& target, Value value) noexcept
    {
        Value current = target.load(std::memory_order_relaxed);
        while (current < value &&
               !target.compare_exchange_weak(current,
                                             value,
                                             std::memory_order_relaxed,
                                             std::memory_order_relaxed))
        {
        }
    }

    RenderRuntimeResult MakeRuntimeResult(RenderRuntimeCode code,
                                          RenderExecutorKind executor,
                                          RHIBackendType backend,
                                          uint64 surfaceGeneration)
    {
        RenderRuntimeResult result;
        result.code = code;
        result.executor = executor;
        result.backend = backend;
        result.surfaceGeneration = surfaceGeneration;

        switch (code)
        {
            case RenderRuntimeCode::None:
                break;
            case RenderRuntimeCode::Running:
                result.lifecycle = RenderLifecycleState::Running;
                break;
            case RenderRuntimeCode::StopRequested:
                result.lifecycle = RenderLifecycleState::StopRequested;
                result.terminalCause = RenderTerminalCause::NormalStop;
                result.teardownMode = RenderTeardownMode::NormalDrain;
                break;
            case RenderRuntimeCode::Stopped:
                result.lifecycle = RenderLifecycleState::Stopped;
                result.terminalCause = RenderTerminalCause::NormalStop;
                result.teardownMode = RenderTeardownMode::NormalDrain;
                break;
            case RenderRuntimeCode::InvalidConfiguration:
            case RenderRuntimeCode::InvalidSurface:
            case RenderRuntimeCode::ExecutorStartFailed:
            case RenderRuntimeCode::DeviceCreationFailed:
            case RenderRuntimeCode::SurfaceCreationFailed:
                result.resultClass = RenderResultClass::RuntimeFatal;
                result.lifecycle = RenderLifecycleState::Failed;
                result.terminalCause = RenderTerminalCause::StartupFailure;
                result.teardownMode = RenderTeardownMode::NormalDrain;
                break;
            case RenderRuntimeCode::RenderGraphValidationFailed:
                result.resultClass = RenderResultClass::FrameFatal;
                result.lifecycle = RenderLifecycleState::Running;
                break;
            case RenderRuntimeCode::DeviceLost:
                result.resultClass = RenderResultClass::RuntimeFatal;
                result.lifecycle = RenderLifecycleState::Failed;
                result.terminalCause = RenderTerminalCause::DeviceLost;
                result.teardownMode = RenderTeardownMode::DeviceLostTeardown;
                break;
            case RenderRuntimeCode::UnhandledException:
                result.resultClass = RenderResultClass::RuntimeFatal;
                result.lifecycle = RenderLifecycleState::Failed;
                result.terminalCause = RenderTerminalCause::UnhandledException;
                result.teardownMode = RenderTeardownMode::NormalDrain;
                break;
            case RenderRuntimeCode::OwnershipViolation:
                result.resultClass = RenderResultClass::RuntimeFatal;
                result.lifecycle = RenderLifecycleState::Failed;
                result.terminalCause = RenderTerminalCause::OwnershipViolation;
                result.teardownMode = RenderTeardownMode::NormalDrain;
                break;
            case RenderRuntimeCode::StartupTimedOut:
            case RenderRuntimeCode::ShutdownTimedOut:
                result.resultClass = RenderResultClass::RuntimeFatal;
                result.lifecycle = RenderLifecycleState::Failed;
                result.terminalCause = RenderTerminalCause::WatchdogTimeout;
                result.teardownMode = RenderTeardownMode::FatalTimeout;
                break;
        }
        return result;
    }

    RenderRuntimeResult NormalizeRuntimeResult(
        const RenderRuntimeResult& source,
        RenderExecutorKind executor,
        RHIBackendType fallbackBackend,
        uint64 surfaceGeneration)
    {
        RenderRuntimeResult result = MakeRuntimeResult(
            source.code, executor, fallbackBackend, surfaceGeneration);
        result.backend = source.backend == RHIBackendType::None
                             ? fallbackBackend
                             : source.backend;
        result.frameSequence = source.frameSequence;
        result.requestSequence = source.requestSequence;
        result.assetId = source.assetId;
        result.handle = source.handle;
        result.nativeError = source.nativeError;
        result.message = source.message;
        return result;
    }

    RenderShutdownResult MakeShutdownResult(RenderShutdownCode code,
                                            RHIBackendType backend,
                                            uint64 frameSequence,
                                            uint64 surfaceGeneration)
    {
        RenderShutdownResult result;
        result.code = code;
        result.backend = backend;
        result.lastSubmittedFrameSequence = frameSequence;
        result.surfaceGeneration = surfaceGeneration;

        switch (code)
        {
            case RenderShutdownCode::None:
                break;
            case RenderShutdownCode::Completed:
                result.terminalCause = RenderTerminalCause::NormalStop;
                result.teardownMode = RenderTeardownMode::NormalDrain;
                break;
            case RenderShutdownCode::AlreadyStopped:
                result.terminalCause = RenderTerminalCause::NormalStop;
                break;
            case RenderShutdownCode::DeviceLost:
                result.resultClass = RenderResultClass::RuntimeFatal;
                result.lifecycle = RenderLifecycleState::Failed;
                result.terminalCause = RenderTerminalCause::DeviceLost;
                result.teardownMode = RenderTeardownMode::DeviceLostTeardown;
                break;
            case RenderShutdownCode::TimedOut:
                result.resultClass = RenderResultClass::RuntimeFatal;
                result.lifecycle = RenderLifecycleState::Failed;
                result.terminalCause = RenderTerminalCause::WatchdogTimeout;
                result.teardownMode = RenderTeardownMode::FatalTimeout;
                break;
            case RenderShutdownCode::ExecutorJoinFailed:
                result.resultClass = RenderResultClass::RuntimeFatal;
                result.lifecycle = RenderLifecycleState::Failed;
                result.terminalCause = RenderTerminalCause::ExecutorFailure;
                result.teardownMode = RenderTeardownMode::FatalTimeout;
                break;
        }
        return result;
    }

    RenderShutdownResult NormalizeShutdownResult(
        const RenderShutdownResult& source,
        RHIBackendType fallbackBackend,
        uint64 frameSequence,
        uint64 surfaceGeneration)
    {
        RenderShutdownResult result = MakeShutdownResult(
            source.code, fallbackBackend, frameSequence, surfaceGeneration);
        result.backend = source.backend == RHIBackendType::None
                             ? fallbackBackend
                             : source.backend;
        result.nativeError = source.nativeError;
        result.message = source.message;
        return result;
    }
} // namespace

    RenderThreadRuntime::RenderThreadRuntime(
        RenderRuntimeConfig config,
        NativeSurfaceDesc surface,
        RenderExecutorKind executorKind,
        std::unique_ptr<IRenderExecutor> executor,
        std::unique_ptr<IRenderFrameConsumer> consumer)
        : m_config(std::move(config)),
          m_initialSurface(surface),
          m_executorKind(executorKind),
          m_executor(std::move(executor)),
          m_consumer(std::move(consumer)),
          m_currentSurface(surface),
          m_latestResizeGeneration(surface.generation)
    {
        if (m_config.transports.IsValid())
        {
            m_frameMailbox = std::make_unique<RenderFrameMailbox>(
                m_config.transports.frameCapacity, &WakeThunk, this);
            m_controlMailbox = std::make_unique<SurfaceControlMailbox>(
                &WakeThunk, this);
            m_resourceGateway = std::make_unique<RenderResourceGateway>(
                m_config.transports,
                &WakeThunk,
                this,
                &RuntimeFatalThunk,
                this);
        }

        m_diagnosticsState.executor = m_executorKind;
        m_diagnosticsState.backend = m_config.backendType;
        m_diagnosticsState.surfaceGeneration = surface.generation;
        m_diagnosticsState.surfaceWidth = surface.width;
        m_diagnosticsState.surfaceHeight = surface.height;
        m_diagnosticsState.frameTransport.capacity =
            m_config.transports.frameCapacity;
        m_diagnosticsState.uploadTransport.requestCapacity =
            m_config.transports.uploadRequestCapacity;
        m_diagnosticsState.uploadTransport.byteCapacity =
            m_config.transports.uploadByteCapacity;
        m_diagnosticsState.releaseTransport.capacity =
            m_config.transports.statusSlotCapacity == 0U
                ? 0U
                : m_config.transports.statusSlotCapacity - 1U;
        PublishDiagnostics();
    }

    RenderThreadRuntime::~RenderThreadRuntime()
    {
        const RenderLifecycleState lifecycle =
            m_lifecycle.load(std::memory_order_acquire);
        if (lifecycle == RenderLifecycleState::Running ||
            lifecycle == RenderLifecycleState::Starting ||
            lifecycle == RenderLifecycleState::StopRequested ||
            lifecycle == RenderLifecycleState::Draining)
        {
            (void)Stop();
        }
    }

    RenderRuntimeResult RenderThreadRuntime::Start()
    {
        const RenderLifecycleState lifecycle =
            m_lifecycle.load(std::memory_order_acquire);
        if (lifecycle != RenderLifecycleState::Stopped ||
            m_started.load(std::memory_order_acquire))
        {
            return GetLastRuntimeResult();
        }

        TransitionTo(RenderLifecycleState::Starting, "startup requested");

        if (!IsConfigurationValid())
        {
            RenderRuntimeResult result = MakeRuntimeResult(
                RenderRuntimeCode::InvalidConfiguration,
                m_executorKind,
                m_config.backendType,
                m_initialSurface.generation);
            result.message = "Invalid render runtime configuration";
            StoreRuntimeResult(result);
            RecordFailure(result);
            TransitionTo(RenderLifecycleState::Failed,
                         "configuration validation failed");
            PublishDiagnostics();
            return result;
        }
        if (!IsSurfaceValid(m_initialSurface))
        {
            RenderRuntimeResult result = MakeRuntimeResult(
                RenderRuntimeCode::InvalidSurface,
                m_executorKind,
                m_config.backendType,
                m_initialSurface.generation);
            result.message = "Invalid initial native surface";
            StoreRuntimeResult(result);
            RecordFailure(result);
            TransitionTo(RenderLifecycleState::Failed,
                         "surface validation failed");
            PublishDiagnostics();
            return result;
        }

        const RenderExecutorStartResult executorStart = m_executor->Start(*this);
        if (executorStart.code != RenderExecutorStartCode::Started)
        {
            RenderRuntimeResult result = MakeRuntimeResult(
                RenderRuntimeCode::ExecutorStartFailed,
                m_executorKind,
                m_config.backendType,
                m_initialSurface.generation);
            result.nativeError = executorStart.nativeError;
            result.message = "Render executor failed to start";
            StoreRuntimeResult(result);
            RecordFailure(result);
            TransitionTo(RenderLifecycleState::Failed,
                         "executor start failed");
            PublishDiagnostics();
            return result;
        }

        m_started.store(true, std::memory_order_release);
        NotifyExecutor();

        const auto deadline =
            std::chrono::steady_clock::now() + m_config.startupWatchdog;
        std::unique_lock lock(m_stateMutex);
        bool startupTimedOut = false;
        if (!m_startupCv.wait_until(lock,
                                    deadline,
                                    [this]() { return m_startupAcknowledged; }))
        {
            StartupResolution expected = StartupResolution::Pending;
            if (m_startupResolution.compare_exchange_strong(
                    expected,
                    StartupResolution::TimedOut,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire))
            {
                startupTimedOut = true;
                RenderRuntimeResult timeout = MakeRuntimeResult(
                    RenderRuntimeCode::StartupTimedOut,
                    m_executorKind,
                    m_config.backendType,
                    m_initialSurface.generation);
                timeout.message =
                    "Render runtime startup watchdog expired";
                m_lastRuntimeResult = timeout;
                m_diagnosticsState.lastFailure.available = true;
                m_diagnosticsState.lastFailure.runtime = timeout;
                m_diagnosticsState.lastFailure.context = timeout.message;
            }
            else
            {
                m_startupCv.wait(
                    lock,
                    [this]() { return m_startupAcknowledged; });
            }
        }
        const RenderRuntimeResult result = m_lastRuntimeResult;
        lock.unlock();

        if (startupTimedOut)
        {
            TransitionTo(RenderLifecycleState::Failed,
                         "startup watchdog expired");
            m_resourceGateway->BeginShutdown();
            m_controlMailbox->RequestStop();
            PublishDiagnostics();
            NotifyExecutor();

            const RenderExecutorJoinResult join = m_executor->JoinUntil(
                std::chrono::steady_clock::now() +
                m_config.shutdownWatchdog);
            if (join.code == RenderExecutorJoinCode::Joined)
            {
                m_joined.store(true, std::memory_order_release);
                return result;
            }

            RenderShutdownResult shutdown = MakeShutdownResult(
                join.code == RenderExecutorJoinCode::TimedOut
                    ? RenderShutdownCode::TimedOut
                    : RenderShutdownCode::ExecutorJoinFailed,
                result.backend,
                m_lastSubmittedFrameSequence.load(
                    std::memory_order_acquire),
                result.surfaceGeneration);
            shutdown.message =
                join.code == RenderExecutorJoinCode::TimedOut
                    ? "Render executor join watchdog expired"
                    : "Render executor rejected join";
            StoreShutdownResult(shutdown);
            RecordFailure(shutdown);
            PublishDiagnostics();
            return result;
        }

        if (result.code != RenderRuntimeCode::Running)
        {
            const RenderExecutorJoinResult join = m_executor->JoinUntil(
                std::chrono::steady_clock::now() + m_config.shutdownWatchdog);
            m_joined.store(join.code == RenderExecutorJoinCode::Joined,
                           std::memory_order_release);
        }
        return result;
    }

    RenderShutdownResult RenderThreadRuntime::Stop()
    {
        const RenderLifecycleState lifecycle =
            m_lifecycle.load(std::memory_order_acquire);
        if (!m_started.load(std::memory_order_acquire) ||
            m_joined.load(std::memory_order_acquire) ||
            lifecycle == RenderLifecycleState::Stopped ||
            lifecycle == RenderLifecycleState::Failed)
        {
            const RenderRuntimeResult runtime = GetLastRuntimeResult();
            RenderShutdownResult result = MakeShutdownResult(
                RenderShutdownCode::AlreadyStopped,
                runtime.backend,
                m_lastSubmittedFrameSequence.load(std::memory_order_acquire),
                runtime.surfaceGeneration);
            StoreShutdownResult(result);
            return result;
        }

        TransitionTo(RenderLifecycleState::StopRequested, "stop requested");
        const NativeSurfaceDesc currentSurface = GetCurrentSurfaceSnapshot();
        StoreRuntimeResult(MakeRuntimeResult(
            RenderRuntimeCode::StopRequested,
            m_executorKind,
            GetLastRuntimeResult().backend,
            currentSurface.generation));
        m_resourceGateway->BeginShutdown();
        m_controlMailbox->RequestStop();
        NotifyExecutor();

        const RenderExecutorJoinResult join = m_executor->JoinUntil(
            std::chrono::steady_clock::now() + m_config.shutdownWatchdog);
        if (join.code == RenderExecutorJoinCode::Joined)
        {
            m_joined.store(true, std::memory_order_release);
            return GetLastShutdownResult();
        }

        const RenderShutdownCode code =
            join.code == RenderExecutorJoinCode::TimedOut
                ? RenderShutdownCode::TimedOut
                : RenderShutdownCode::ExecutorJoinFailed;
        RenderShutdownResult result = MakeShutdownResult(
            code,
            GetLastRuntimeResult().backend,
            m_lastSubmittedFrameSequence.load(std::memory_order_acquire),
            currentSurface.generation);
        result.message = join.code == RenderExecutorJoinCode::TimedOut
                             ? "Render executor join watchdog expired"
                             : "Render executor rejected join";
        StoreShutdownResult(result);
        RecordFailure(result);
        return result;
    }

    RenderFramePublishResult RenderThreadRuntime::TryPublishFrame(
        std::unique_ptr<const RenderFramePacket> packet)
    {
        RenderFramePublishResult result;
        result.sequence = packet != nullptr ? packet->GetHeader().sequence : 0U;
        if (IsShuttingDownForPublication())
        {
            result.code = RenderFramePublishCode::ShuttingDown;
        }
        else if (!IsRunningForPublication())
        {
            result.code = RenderFramePublishCode::NotRunning;
        }
        else
        {
            const RenderFrameMailboxPublishResult mailboxResult =
                m_frameMailbox->TryPublish(std::move(packet));
            switch (mailboxResult.code)
            {
                case RenderFrameMailboxPublishCode::Accepted:
                    result.code = RenderFramePublishCode::Accepted;
                    break;
                case RenderFrameMailboxPublishCode::ReplacedOldest:
                    result.code = RenderFramePublishCode::ReplacedOlder;
                    result.replacedSequence = mailboxResult.replacedSequence;
                    m_frameReplacementCount.fetch_add(1,
                                                      std::memory_order_relaxed);
                    break;
                case RenderFrameMailboxPublishCode::InvalidPacket:
                    result.code = RenderFramePublishCode::InvalidPacket;
                    m_invalidFrameCount.fetch_add(1,
                                                  std::memory_order_relaxed);
                    break;
                case RenderFrameMailboxPublishCode::OutOfOrder:
                    result.code = RenderFramePublishCode::OutOfOrder;
                    m_outOfOrderFrameCount.fetch_add(1,
                                                     std::memory_order_relaxed);
                    break;
            }
        }
        result.resultClass = ClassifyRenderFramePublishCode(result.code);
        if (result.code == RenderFramePublishCode::Accepted ||
            result.code == RenderFramePublishCode::ReplacedOlder)
        {
            m_lastPublishedFrameSequence.store(result.sequence,
                                               std::memory_order_release);
            AtomicMax(m_frameHighWaterMark,
                      m_frameMailbox->GetPendingCount());
            NotifyExecutor();
        }
        return result;
    }

    RenderResizeResult RenderThreadRuntime::RequestResize(
        const NativeSurfaceDesc& surface)
    {
        RenderResizeResult result;
        result.generation = surface.generation;
        if (IsShuttingDownForPublication())
        {
            result.code = RenderResizeCode::ShuttingDown;
        }
        else if (!IsRunningForPublication())
        {
            result.code = RenderResizeCode::NotRunning;
        }
        else if (!IsSurfaceValid(surface))
        {
            result.code = RenderResizeCode::InvalidSurface;
            m_resizeRejectedCount.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            std::lock_guard lock(m_publicationMutex);
            if (surface.generation <= m_latestResizeGeneration)
            {
                result.code = RenderResizeCode::StaleGeneration;
                m_resizeRejectedCount.fetch_add(1,
                                                std::memory_order_relaxed);
            }
            else
            {
                result.replacedGeneration = m_pendingResizeGeneration;
                const bool published = m_controlMailbox->TryPublishResize(
                    surface.generation, surface);
                if (!published)
                {
                    result.code = RenderResizeCode::StaleGeneration;
                    m_resizeRejectedCount.fetch_add(1,
                                                    std::memory_order_relaxed);
                }
                else
                {
                    m_latestResizeGeneration = surface.generation;
                    m_pendingResizeGeneration = surface.generation;
                    if (result.replacedGeneration == 0U)
                    {
                        result.code = RenderResizeCode::Accepted;
                        m_resizeAcceptedCount.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                    else
                    {
                        result.code = RenderResizeCode::CoalescedOlder;
                        m_resizeCoalescedCount.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                }
            }
        }
        result.resultClass = ClassifyRenderResizeCode(result.code);
        if (result.code == RenderResizeCode::Accepted ||
            result.code == RenderResizeCode::CoalescedOlder)
        {
            NotifyExecutor();
        }
        return result;
    }

    RenderDiagnosticsSnapshot
        RenderThreadRuntime::GetDiagnosticsSnapshot() const
    {
        return m_diagnosticsPublisher.GetSnapshot();
    }

    RenderRuntimeResult RenderThreadRuntime::GetLastRuntimeResult() const
    {
        std::lock_guard lock(m_stateMutex);
        return m_lastRuntimeResult;
    }

    RenderShutdownResult RenderThreadRuntime::GetLastShutdownResult() const
    {
        std::lock_guard lock(m_stateMutex);
        return m_lastShutdownResult;
    }

    RenderResourceReserveResult RenderThreadRuntime::ReserveResource(
        AssetId assetId,
        RenderResourceKind kind) noexcept
    {
        if (m_resourceGateway == nullptr)
        {
            return {};
        }
        return m_resourceGateway->ReserveResource(assetId, kind);
    }

    RenderUploadEnqueueResult RenderThreadRuntime::TryEnqueueUpload(
        const ResourceUploadRequestRef& request) noexcept
    {
        if (m_resourceGateway == nullptr)
        {
            return {};
        }
        const RenderUploadEnqueueResult result =
            m_resourceGateway->TryEnqueueUpload(request);
        if (result.code == RenderUploadEnqueueCode::Accepted)
        {
            m_uploadAcceptedCount.fetch_add(1, std::memory_order_relaxed);
            AtomicMax(m_uploadRequestHighWaterMark,
                      m_resourceGateway->GetRetainedUploadCount());
            AtomicMax(m_uploadByteHighWaterMark,
                      m_resourceGateway->GetRetainedUploadBytes());
            NotifyExecutor();
        }
        else if (result.code == RenderUploadEnqueueCode::QueueFullByCount ||
                 result.code == RenderUploadEnqueueCode::QueueFullByBytes)
        {
            m_uploadPressureCount.fetch_add(1, std::memory_order_relaxed);
        }
        return result;
    }

    RenderReleaseResult RenderThreadRuntime::RequestRelease(
        RenderResourceHandle handle) noexcept
    {
        if (m_resourceGateway == nullptr)
        {
            return {};
        }
        const RenderReleaseResult result =
            m_resourceGateway->RequestRelease(handle);
        if (result.code == RenderReleaseCode::Accepted)
        {
            m_releaseAcceptedCount.fetch_add(1,
                                             std::memory_order_relaxed);
            const RenderReleaseQueueSnapshot queue =
                m_resourceGateway->GetReleaseQueueSnapshot();
            AtomicMax(m_releaseHighWaterMark,
                      queue.pendingCount);
            NotifyExecutor();
        }
        else if (result.code == RenderReleaseCode::StaleGeneration)
        {
            m_releaseStaleCount.fetch_add(1, std::memory_order_relaxed);
        }
        return result;
    }

    RenderResourceStatus RenderThreadRuntime::QueryResourceStatus(
        RenderResourceHandle handle) const noexcept
    {
        return m_resourceGateway != nullptr
                   ? m_resourceGateway->QueryResourceStatus(handle)
                   : RenderResourceStatus{};
    }

    RenderPumpDecision RenderThreadRuntime::PumpOnce() noexcept
    {
        try
        {
            m_wakePending.store(false, std::memory_order_release);
            m_pumpIterationCount.fetch_add(1, std::memory_order_relaxed);

            if (!m_startupAttempted.exchange(true,
                                             std::memory_order_acq_rel))
            {
                return InitializeOnRenderThread();
            }
            const RenderLifecycleState lifecycle =
                m_lifecycle.load(std::memory_order_acquire);
            if (m_controlMailbox->IsStopRequested() ||
                lifecycle == RenderLifecycleState::StopRequested ||
                lifecycle == RenderLifecycleState::Draining)
            {
                return StopOnRenderThread();
            }
            if (lifecycle != RenderLifecycleState::Running)
            {
                return RenderPumpDecision::Stop;
            }
            if (m_renderThreadGuard.ValidateCurrentThread() !=
                RenderThreadGuardCode::Owner)
            {
                RenderRuntimeResult result = MakeRuntimeResult(
                    RenderRuntimeCode::OwnershipViolation,
                    m_executorKind,
                    GetLastRuntimeResult().backend,
                    GetCurrentSurfaceSnapshot().generation);
                result.message = "Render pump executed on a non-owner thread";
                return FailOnRenderThread(std::move(result));
            }

            bool progressed = false;
            BasicRenderControlBatch<NativeSurfaceDesc, NativeSurfaceDesc>
                controls;
            {
                std::lock_guard lock(m_publicationMutex);
                controls = m_controlMailbox->AcquireLatest();
                if (controls.resize.has_value())
                {
                    m_pendingResizeGeneration = 0;
                }
            }
            if (controls.stopRequested)
            {
                return StopOnRenderThread();
            }
            if (controls.resize.has_value())
            {
                const NativeSurfaceDesc& surface = controls.resize->value;
                RenderRuntimeResult surfaceResult = NormalizeRuntimeResult(
                    m_consumer->ApplySurface(surface),
                    m_executorKind,
                    GetLastRuntimeResult().backend,
                    surface.generation);
                if (surfaceResult.code != RenderRuntimeCode::Running)
                {
                    return FailOnRenderThread(std::move(surfaceResult));
                }
                {
                    std::lock_guard lock(m_publicationMutex);
                    m_currentSurface = surface;
                }
                progressed = true;
            }

            RenderFrameAcquireResult frame = m_frameMailbox->AcquireLatest();
            m_frameReplacementCount.fetch_add(frame.discardedCount,
                                              std::memory_order_relaxed);
            if (frame.packet != nullptr)
            {
                m_lastAcquiredFrameSequence.store(
                    frame.packet->GetHeader().sequence,
                    std::memory_order_release);
                progressed = true;
            }

            const auto releaseDeadline =
                std::chrono::steady_clock::now() +
                m_config.iterationBudgets.releaseTime;
            for (uint32 count = 0;
                 count < m_config.iterationBudgets.releaseCount &&
                 std::chrono::steady_clock::now() < releaseDeadline;
                 ++count)
            {
                const RenderResourceHandle handle =
                    m_resourceGateway->TryDequeueRelease();
                if (!handle.IsValid())
                {
                    break;
                }
                m_consumer->ProcessRelease(handle);
                m_releaseDequeuedCount.fetch_add(
                    1, std::memory_order_relaxed);
                progressed = true;
            }

            const auto uploadDeadline =
                std::chrono::steady_clock::now() +
                m_config.iterationBudgets.uploadTime;
            uint64 uploadBytes = 0;
            for (uint32 count = 0;
                 count < m_config.iterationBudgets.uploadRequestCount &&
                 uploadBytes < m_config.iterationBudgets.uploadBytes &&
                 std::chrono::steady_clock::now() < uploadDeadline;
                 ++count)
            {
                ResourceUploadRequestRef request =
                    m_resourceGateway->TryDequeueUpload();
                if (request == nullptr)
                {
                    break;
                }
                uploadBytes += request->GetDerivedPayloadBytes();
                m_consumer->ProcessUpload(request);
                progressed = true;
            }

            if (frame.packet != nullptr)
            {
                const uint64 sequence = frame.packet->GetHeader().sequence;
                RenderRuntimeResult frameResult = NormalizeRuntimeResult(
                    m_consumer->ConsumeFrame(*frame.packet),
                    m_executorKind,
                    GetLastRuntimeResult().backend,
                    GetCurrentSurfaceSnapshot().generation);
                frameResult.frameSequence = sequence;
                if (frameResult.code == RenderRuntimeCode::Running)
                {
                    m_lastAppliedFrameSequence.store(sequence,
                                                     std::memory_order_release);
                    m_lastSubmittedFrameSequence.store(
                        sequence, std::memory_order_release);
                    m_lastPresentedFrameSequence.store(
                        sequence, std::memory_order_release);
                }
                else if (frameResult.resultClass ==
                         RenderResultClass::RuntimeFatal)
                {
                    return FailOnRenderThread(std::move(frameResult));
                }
                else
                {
                    StoreRuntimeResult(frameResult);
                    RecordFailure(frameResult);
                }
            }

            m_consumer->PollCompletion();
            m_consumer->RetireCompleted();
            PublishDiagnostics();
            return progressed ? RenderPumpDecision::Progressed
                              : RenderPumpDecision::Idle;
        }
        catch (...)
        {
            OnUnhandledExecutorException();
            return RenderPumpDecision::Stop;
        }
    }

    void RenderThreadRuntime::WaitForWork() noexcept
    {
        try
        {
            m_idleWaitCount.fetch_add(1, std::memory_order_relaxed);
            PublishDiagnostics();
        }
        catch (...)
        {
        }
        std::unique_lock lock(m_waitMutex);
        m_waitCv.wait(lock, [this]() {
            return m_wakePending.load(std::memory_order_acquire) ||
                   (m_controlMailbox != nullptr &&
                    m_controlMailbox->IsStopRequested());
        });
    }

    void RenderThreadRuntime::Wake() noexcept
    {
        m_wakePending.store(true, std::memory_order_release);
        m_waitCv.notify_all();
    }

    void RenderThreadRuntime::OnUnhandledExecutorException() noexcept
    {
        try
        {
            RenderRuntimeResult result = MakeRuntimeResult(
                RenderRuntimeCode::UnhandledException,
                m_executorKind,
                GetLastRuntimeResult().backend,
                GetCurrentSurfaceSnapshot().generation);
            result.message = "Unhandled exception escaped the render pump";
            StoreRuntimeResult(result);
            RecordFailure(result);
            TransitionTo(RenderLifecycleState::Failed,
                         "unhandled executor exception");
            if (m_consumer != nullptr &&
                m_renderThreadGuard.QueryCurrentThread() ==
                    RenderThreadGuardCode::Owner)
            {
                (void)m_consumer->Shutdown(RenderTeardownMode::NormalDrain);
                m_consumer.reset();
            }
            {
                std::lock_guard lock(m_stateMutex);
                m_startupAcknowledged = true;
            }
            m_startupCv.notify_all();
            PublishDiagnostics();
        }
        catch (...)
        {
        }
        Wake();
    }

    void RenderThreadRuntime::WakeThunk(void* context) noexcept
    {
        static_cast<RenderThreadRuntime*>(context)->Wake();
    }

    void RenderThreadRuntime::RuntimeFatalThunk(void* context,
                                                const char* message) noexcept
    {
        auto* runtime = static_cast<RenderThreadRuntime*>(context);
        try
        {
            RenderRuntimeResult result = MakeRuntimeResult(
                RenderRuntimeCode::OwnershipViolation,
                runtime->m_executorKind,
                runtime->GetLastRuntimeResult().backend,
                runtime->GetCurrentSurfaceSnapshot().generation);
            result.message = message != nullptr ? message :
                                                  "Render transport invariant failed";
            runtime->StoreRuntimeResult(result);
            runtime->RecordFailure(result);
            if (runtime->m_controlMailbox != nullptr)
            {
                runtime->m_controlMailbox->RequestStop();
            }
            runtime->NotifyExecutor();
        }
        catch (...)
        {
            std::terminate();
        }
    }

    bool RenderThreadRuntime::IsConfigurationValid() const noexcept
    {
        return m_config.backendType != RHIBackendType::None &&
               m_config.frameBuffering >= 2U &&
               m_config.frameBuffering <= 4U &&
               m_config.transports.IsValid() &&
               m_config.iterationBudgets.IsValid() &&
               m_config.startupWatchdog >
                   std::chrono::milliseconds::zero() &&
               m_config.shutdownWatchdog >
                   std::chrono::milliseconds::zero() &&
               m_executorKind != RenderExecutorKind::None &&
               m_executor != nullptr && m_consumer != nullptr &&
               m_frameMailbox != nullptr && m_controlMailbox != nullptr &&
               m_resourceGateway != nullptr;
    }

    bool RenderThreadRuntime::IsSurfaceValid(
        const NativeSurfaceDesc& surface) const noexcept
    {
        RHIBackendType backend = GetLastRuntimeResult().backend;
        if (backend == RHIBackendType::None)
        {
            backend = m_config.backendType;
        }
        if (backend != RHIBackendType::Auto)
        {
            return surface.IsValidFor(backend);
        }
        return surface.platform != NativeSurfacePlatform::None &&
               surface.width != 0U && surface.height != 0U &&
               surface.generation != 0U && surface.contentScale > 0.0f &&
               surface.preferredFormat != RHIFormat::Unknown;
    }

    bool RenderThreadRuntime::IsRunningForPublication() const noexcept
    {
        return m_lifecycle.load(std::memory_order_acquire) ==
               RenderLifecycleState::Running;
    }

    bool RenderThreadRuntime::IsShuttingDownForPublication() const noexcept
    {
        const RenderLifecycleState lifecycle =
            m_lifecycle.load(std::memory_order_acquire);
        return lifecycle == RenderLifecycleState::StopRequested ||
               lifecycle == RenderLifecycleState::Draining;
    }

    NativeSurfaceDesc RenderThreadRuntime::GetCurrentSurfaceSnapshot() const
    {
        std::lock_guard lock(m_publicationMutex);
        return m_currentSurface;
    }

    void RenderThreadRuntime::NotifyExecutor() noexcept
    {
        if (m_executor != nullptr)
        {
            m_executor->NotifyWork();
        }
    }

    void RenderThreadRuntime::TransitionTo(RenderLifecycleState lifecycle,
                                           const char* message)
    {
        const RenderLifecycleState previous =
            m_lifecycle.exchange(lifecycle, std::memory_order_acq_rel);
        if (previous == lifecycle)
        {
            return;
        }

        std::lock_guard lock(m_stateMutex);
        RenderLifecycleTransitionDiagnostics transition;
        transition.from = previous;
        transition.to = lifecycle;
        transition.sequence = m_diagnosticsState.transitionCount + 1U;
        transition.message = message != nullptr ? message : "";
        const uint64 index = std::min<uint64>(
            m_diagnosticsState.transitionCount,
            RVX_RENDER_DIAGNOSTICS_TRANSITION_CAPACITY - 1U);
        if (m_diagnosticsState.transitionCount >=
            RVX_RENDER_DIAGNOSTICS_TRANSITION_CAPACITY)
        {
            std::move(m_diagnosticsState.transitions.begin() + 1,
                      m_diagnosticsState.transitions.end(),
                      m_diagnosticsState.transitions.begin());
        }
        m_diagnosticsState.transitions[index] = transition;
        ++m_diagnosticsState.transitionCount;
        m_diagnosticsState.lastTransition = std::move(transition);
        m_diagnosticsState.lifecycle = lifecycle;
    }

    void RenderThreadRuntime::StoreRuntimeResult(RenderRuntimeResult result)
    {
        std::lock_guard lock(m_stateMutex);
        m_lastRuntimeResult = std::move(result);
    }

    void RenderThreadRuntime::StoreShutdownResult(RenderShutdownResult result)
    {
        std::lock_guard lock(m_stateMutex);
        m_lastShutdownResult = std::move(result);
    }

    void RenderThreadRuntime::RecordFailure(
        const RenderRuntimeResult& result)
    {
        std::lock_guard lock(m_stateMutex);
        m_diagnosticsState.lastFailure.available = true;
        m_diagnosticsState.lastFailure.runtime = result;
        m_diagnosticsState.lastFailure.context = result.message;
    }

    void RenderThreadRuntime::RecordFailure(
        const RenderShutdownResult& result)
    {
        std::lock_guard lock(m_stateMutex);
        m_diagnosticsState.lastFailure.available = true;
        m_diagnosticsState.lastFailure.shutdown = result;
        m_diagnosticsState.lastFailure.context = result.message;
    }

    void RenderThreadRuntime::PublishDiagnostics()
    {
        RenderDiagnosticsSnapshot snapshot;
        {
            std::lock_guard lock(m_stateMutex);
            snapshot = m_diagnosticsState;
            snapshot.executor = m_executorKind;
            snapshot.lifecycle =
                m_lifecycle.load(std::memory_order_acquire);
            const RHIBackendType resultBackend = m_lastRuntimeResult.backend;
            snapshot.backend = resultBackend == RHIBackendType::None
                                   ? m_config.backendType
                                   : resultBackend;
        }
        {
            std::lock_guard lock(m_publicationMutex);
            snapshot.surfaceGeneration = m_currentSurface.generation;
            snapshot.surfaceWidth = m_currentSurface.width;
            snapshot.surfaceHeight = m_currentSurface.height;
        }

        snapshot.publicationSequence =
            m_publicationSequence.fetch_add(1, std::memory_order_relaxed) + 1U;
        snapshot.pumpIterationCount =
            m_pumpIterationCount.load(std::memory_order_relaxed);
        snapshot.idleWaitCount =
            m_idleWaitCount.load(std::memory_order_relaxed);
        snapshot.lastPublishedFrameSequence =
            m_lastPublishedFrameSequence.load(std::memory_order_acquire);
        snapshot.lastAcquiredFrameSequence =
            m_lastAcquiredFrameSequence.load(std::memory_order_acquire);
        snapshot.lastAppliedFrameSequence =
            m_lastAppliedFrameSequence.load(std::memory_order_acquire);
        snapshot.lastSubmittedFrameSequence =
            m_lastSubmittedFrameSequence.load(std::memory_order_acquire);
        snapshot.lastPresentedFrameSequence =
            m_lastPresentedFrameSequence.load(std::memory_order_acquire);
        snapshot.resizeAcceptedCount =
            m_resizeAcceptedCount.load(std::memory_order_relaxed);
        snapshot.resizeCoalescedCount =
            m_resizeCoalescedCount.load(std::memory_order_relaxed);
        snapshot.resizeRejectedCount =
            m_resizeRejectedCount.load(std::memory_order_relaxed);

        if (m_frameMailbox != nullptr)
        {
            snapshot.frameTransport.currentUsage =
                m_frameMailbox->GetPendingCount();
        }
        snapshot.frameTransport.highWaterMark =
            m_frameHighWaterMark.load(std::memory_order_relaxed);
        snapshot.frameTransport.replacements =
            m_frameReplacementCount.load(std::memory_order_relaxed);
        snapshot.frameTransport.invalidPackets =
            m_invalidFrameCount.load(std::memory_order_relaxed);
        snapshot.frameTransport.outOfOrderRejections =
            m_outOfOrderFrameCount.load(std::memory_order_relaxed);

        if (m_resourceGateway != nullptr)
        {
            snapshot.uploadTransport.currentRequestUsage =
                m_resourceGateway->GetRetainedUploadCount();
            snapshot.uploadTransport.currentByteUsage =
                m_resourceGateway->GetRetainedUploadBytes();
        }
        snapshot.uploadTransport.requestHighWaterMark =
            m_uploadRequestHighWaterMark.load(std::memory_order_relaxed);
        snapshot.uploadTransport.byteHighWaterMark =
            m_uploadByteHighWaterMark.load(std::memory_order_relaxed);
        snapshot.uploadTransport.pressureOutcomes =
            m_uploadPressureCount.load(std::memory_order_relaxed);
        snapshot.uploadTransport.acceptedCount =
            m_uploadAcceptedCount.load(std::memory_order_relaxed);
        snapshot.uploadTransport.completedCount =
            m_uploadCompletedCount.load(std::memory_order_relaxed);
        snapshot.uploadTransport.failedCount =
            m_uploadFailedCount.load(std::memory_order_relaxed);
        snapshot.releaseTransport.highWaterMark =
            m_releaseHighWaterMark.load(std::memory_order_relaxed);
        const uint64 releaseAccepted =
            m_releaseAcceptedCount.load(std::memory_order_relaxed);
        const uint64 releaseDequeued =
            m_releaseDequeuedCount.load(std::memory_order_relaxed);
        const RenderReleaseQueueSnapshot releaseQueue =
            m_resourceGateway != nullptr
                ? m_resourceGateway->GetReleaseQueueSnapshot()
                : RenderReleaseQueueSnapshot{};
        const RenderRuntimeDetail::ReleaseDiagnosticsObservation
            releaseObservation =
                RenderRuntimeDetail::ObserveReleaseDiagnostics(
                    releaseAccepted,
                    releaseDequeued,
                    releaseQueue);
        snapshot.releaseTransport.currentUsage =
            releaseObservation.currentUsage;
        snapshot.releaseTransport.acceptedCount =
            releaseAccepted;
        snapshot.releaseTransport.completedCount =
            m_releaseCompletedCount.load(std::memory_order_relaxed);
        snapshot.releaseTransport.staleCount =
            m_releaseStaleCount.load(std::memory_order_relaxed);
        snapshot.releaseTransport.oldestPendingGeneration =
            releaseObservation.oldestPendingGeneration;

        m_diagnosticsPublisher.Publish(std::move(snapshot));
    }

    RenderPumpDecision RenderThreadRuntime::InitializeOnRenderThread()
    {
        if (m_renderThreadGuard.BindCurrentThread() !=
            RenderThreadGuardCode::Owner)
        {
            RenderRuntimeResult result = MakeRuntimeResult(
                RenderRuntimeCode::OwnershipViolation,
                m_executorKind,
                m_config.backendType,
                m_initialSurface.generation);
            result.message = "Render thread ownership could not be established";
            return FailOnRenderThread(std::move(result));
        }

        {
            std::lock_guard lock(m_stateMutex);
            m_diagnosticsState.renderThreadIdentityHash =
                static_cast<uint64>(std::hash<std::thread::id>{}(
                    m_renderThreadGuard.GetOwnerThreadId()));
        }

        if (m_startupResolution.load(std::memory_order_acquire) ==
            StartupResolution::TimedOut)
        {
            return FinishTimedOutStartupOnRenderThread();
        }

        RenderRuntimeResult result = NormalizeRuntimeResult(
            m_consumer->Initialize(m_config, m_initialSurface),
            m_executorKind,
            m_config.backendType,
            m_initialSurface.generation);
        StartupResolution expected = StartupResolution::Pending;
        if (!m_startupResolution.compare_exchange_strong(
                expected,
                StartupResolution::RenderResultClaimed,
                std::memory_order_acq_rel,
                std::memory_order_acquire))
        {
            return FinishTimedOutStartupOnRenderThread();
        }
        if (result.code != RenderRuntimeCode::Running)
        {
            (void)m_consumer->Shutdown(result.teardownMode);
            m_consumer.reset();
            StoreRuntimeResult(result);
            RecordFailure(result);
            TransitionTo(RenderLifecycleState::Failed, "startup failed");
            PublishDiagnostics();
            {
                std::lock_guard lock(m_stateMutex);
                m_startupAcknowledged = true;
            }
            m_startupCv.notify_all();
            return RenderPumpDecision::Stop;
        }

        StoreRuntimeResult(result);
        TransitionTo(RenderLifecycleState::Running, "startup acknowledged");
        PublishDiagnostics();
        {
            std::lock_guard lock(m_stateMutex);
            m_startupAcknowledged = true;
        }
        m_startupCv.notify_all();
        return RenderPumpDecision::Progressed;
    }

    RenderPumpDecision
        RenderThreadRuntime::FinishTimedOutStartupOnRenderThread()
    {
        const RenderRuntimeResult runtime = GetLastRuntimeResult();
        RenderShutdownResult shutdown =
            m_consumer != nullptr
                ? NormalizeShutdownResult(
                      m_consumer->Shutdown(RenderTeardownMode::FatalTimeout),
                      runtime.backend,
                      m_lastSubmittedFrameSequence.load(
                          std::memory_order_acquire),
                      runtime.surfaceGeneration)
                : MakeShutdownResult(
                      RenderShutdownCode::Completed,
                      runtime.backend,
                      m_lastSubmittedFrameSequence.load(
                          std::memory_order_acquire),
                      runtime.surfaceGeneration);
        m_consumer.reset();
        StoreShutdownResult(std::move(shutdown));
        TransitionTo(RenderLifecycleState::Failed,
                     "late startup acknowledgement rejected");
        PublishDiagnostics();
        {
            std::lock_guard lock(m_stateMutex);
            m_startupAcknowledged = true;
        }
        m_startupCv.notify_all();
        return RenderPumpDecision::Stop;
    }

    RenderPumpDecision RenderThreadRuntime::StopOnRenderThread()
    {
        TransitionTo(RenderLifecycleState::Draining, "draining accepted work");
        const RenderRuntimeResult runtime = GetLastRuntimeResult();
        const NativeSurfaceDesc currentSurface = GetCurrentSurfaceSnapshot();
        const RenderTeardownMode mode =
            runtime.terminalCause == RenderTerminalCause::DeviceLost
                ? RenderTeardownMode::DeviceLostTeardown
                : RenderTeardownMode::NormalDrain;
        RenderShutdownResult shutdown = m_consumer != nullptr
                                            ? NormalizeShutdownResult(
                                                  m_consumer->Shutdown(mode),
                                                  runtime.backend,
                                                  m_lastSubmittedFrameSequence.load(
                                                      std::memory_order_acquire),
                                                  currentSurface.generation)
                                            : MakeShutdownResult(
                                                  RenderShutdownCode::Completed,
                                                  runtime.backend,
                                                  m_lastSubmittedFrameSequence.load(
                                                      std::memory_order_acquire),
                                                  currentSurface.generation);
        m_consumer.reset();
        StoreShutdownResult(shutdown);
        if (shutdown.resultClass == RenderResultClass::RuntimeFatal)
        {
            RecordFailure(shutdown);
            TransitionTo(RenderLifecycleState::Failed, "shutdown failed");
        }
        else
        {
            StoreRuntimeResult(MakeRuntimeResult(
                RenderRuntimeCode::Stopped,
                m_executorKind,
                runtime.backend,
                currentSurface.generation));
            TransitionTo(RenderLifecycleState::Stopped, "stop acknowledged");
        }
        PublishDiagnostics();
        return RenderPumpDecision::Stop;
    }

    RenderPumpDecision RenderThreadRuntime::FailOnRenderThread(
        RenderRuntimeResult result)
    {
        const NativeSurfaceDesc currentSurface = GetCurrentSurfaceSnapshot();
        StoreRuntimeResult(result);
        RecordFailure(result);
        TransitionTo(RenderLifecycleState::Failed, "runtime fatal failure");
        if (m_resourceGateway != nullptr)
        {
            m_resourceGateway->BeginShutdown();
        }
        if (m_consumer != nullptr)
        {
            RenderShutdownResult shutdown = NormalizeShutdownResult(
                m_consumer->Shutdown(result.teardownMode),
                result.backend,
                m_lastSubmittedFrameSequence.load(std::memory_order_acquire),
                currentSurface.generation);
            StoreShutdownResult(shutdown);
            m_consumer.reset();
        }
        {
            std::lock_guard lock(m_stateMutex);
            m_startupAcknowledged = true;
        }
        m_startupCv.notify_all();
        PublishDiagnostics();
        return RenderPumpDecision::Stop;
    }
} // namespace RVX
