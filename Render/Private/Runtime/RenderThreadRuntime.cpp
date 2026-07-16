#include "Runtime/RenderThreadRuntime.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <utility>

namespace RVX
{
namespace
{
    [[nodiscard]] bool IsDeclaredExecutorKind(
        RenderExecutorKind kind) noexcept
    {
        switch (kind)
        {
            case RenderExecutorKind::None:
            case RenderExecutorKind::Dedicated:
            case RenderExecutorKind::InlineTest:
                return true;
        }
        return false;
    }

    [[nodiscard]] bool IsDeclaredBackendType(
        RHIBackendType backend) noexcept
    {
        switch (backend)
        {
            case RHIBackendType::None:
            case RHIBackendType::Auto:
            case RHIBackendType::DX11:
            case RHIBackendType::DX12:
            case RHIBackendType::Vulkan:
            case RHIBackendType::Metal:
            case RHIBackendType::OpenGL:
                return true;
        }
        return false;
    }

    class TerminatingRenderFatalPolicy final : public IRenderFatalPolicy
    {
    public:
        void Terminate(const RenderDiagnosticsSnapshot&) override
        {
            std::terminate();
        }
    };

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
        if (!IsDeclaredBackendType(source.backend))
        {
            RenderRuntimeResult result = MakeRuntimeResult(
                RenderRuntimeCode::OwnershipViolation,
                executor,
                RHIBackendType::None,
                source.surfaceGeneration == 0U
                    ? surfaceGeneration
                    : source.surfaceGeneration);
            result.frameSequence = source.frameSequence;
            result.requestSequence = source.requestSequence;
            result.assetId = source.assetId;
            result.handle = source.handle;
            result.nativeError = source.nativeError;
            result.message =
                "Render consumer returned an undeclared backend";
            if (!source.message.empty())
            {
                result.message += " | " + source.message;
            }
            return result;
        }
        if (!IsDeclaredRenderRuntimeCode(source.code))
        {
            RenderRuntimeResult result = MakeRuntimeResult(
                RenderRuntimeCode::OwnershipViolation,
                executor,
                source.backend == RHIBackendType::None
                    ? fallbackBackend
                    : source.backend,
                source.surfaceGeneration == 0U
                    ? surfaceGeneration
                    : source.surfaceGeneration);
            result.frameSequence = source.frameSequence;
            result.requestSequence = source.requestSequence;
            result.assetId = source.assetId;
            result.handle = source.handle;
            result.nativeError = source.nativeError;
            result.message =
                "Render consumer returned an undeclared runtime code";
            return result;
        }
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
        if (!IsDeclaredBackendType(source.backend))
        {
            RenderShutdownResult result = MakeShutdownResult(
                RenderShutdownCode::ExecutorJoinFailed,
                RHIBackendType::None,
                source.lastSubmittedFrameSequence == 0U
                    ? frameSequence
                    : source.lastSubmittedFrameSequence,
                source.surfaceGeneration == 0U
                    ? surfaceGeneration
                    : source.surfaceGeneration);
            result.nativeError = source.nativeError;
            result.message =
                "Render consumer returned an undeclared backend";
            if (!source.message.empty())
            {
                result.message += " | " + source.message;
            }
            return result;
        }
        if (!IsDeclaredRenderShutdownCode(source.code))
        {
            RenderShutdownResult result = MakeShutdownResult(
                RenderShutdownCode::ExecutorJoinFailed,
                source.backend == RHIBackendType::None
                    ? fallbackBackend
                    : source.backend,
                source.lastSubmittedFrameSequence == 0U
                    ? frameSequence
                    : source.lastSubmittedFrameSequence,
                source.surfaceGeneration == 0U
                    ? surfaceGeneration
                    : source.surfaceGeneration);
            result.nativeError = source.nativeError;
            result.message =
                "Render consumer returned an undeclared shutdown code";
            return result;
        }
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
        std::unique_ptr<IRenderFrameConsumer> consumer,
        std::shared_ptr<IRenderRuntimeLifecycleHook> lifecycleHook,
        std::shared_ptr<IRenderFatalPolicy> fatalPolicy,
        std::shared_ptr<IRenderPublicationHook> publicationHook,
        std::shared_ptr<IRenderWaitHook> waitHook)
        : m_config(std::move(config)),
          m_initialSurface(surface),
          m_executorKind(IsDeclaredExecutorKind(executorKind)
                             ? executorKind
                             : RenderExecutorKind::None),
          m_executor(std::move(executor)),
          m_consumer(std::move(consumer)),
          m_lifecycleHook(std::move(lifecycleHook)),
          m_fatalPolicy(std::move(fatalPolicy)),
          m_publicationHook(std::move(publicationHook)),
          m_waitHook(std::move(waitHook)),
          m_currentSurface(surface),
          m_latestResizeGeneration(surface.generation)
    {
        if (!IsDeclaredBackendType(m_config.backendType))
        {
            m_config.backendType = RHIBackendType::None;
        }
        if (m_config.transports.IsValid())
        {
            m_frameMailbox = std::make_unique<RenderFrameMailbox>(
                m_config.transports.frameCapacity);
            m_controlMailbox = std::make_unique<SurfaceControlMailbox>(
                nullptr, nullptr);
            m_resourceGateway = std::make_unique<RenderResourceGateway>(
                m_config.transports,
                nullptr,
                nullptr,
                &RuntimeFatalThunk,
                this,
                &GatewayPublicationThunk,
                this);
        }
        if (m_fatalPolicy == nullptr)
        {
            m_fatalPolicy =
                std::make_shared<TerminatingRenderFatalPolicy>();
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
        if (m_started.load(std::memory_order_acquire) &&
            !m_joined.load(std::memory_order_acquire))
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
            (void)TryClaimTerminalResult(result);
            TransitionTo(RenderLifecycleState::Failed,
                         "configuration validation failed");
            SealPublication();
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
            (void)TryClaimTerminalResult(result);
            TransitionTo(RenderLifecycleState::Failed,
                         "surface validation failed");
            SealPublication();
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
            (void)TryClaimTerminalResult(result);
            TransitionTo(RenderLifecycleState::Failed,
                         "executor start failed");
            SealPublication();
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
            lock.unlock();
            std::unique_lock arbitrationLock(
                m_startupArbitrationMutex, std::defer_lock);
            if (!arbitrationLock.try_lock_until(
                    std::chrono::steady_clock::now() +
                    m_config.shutdownWatchdog))
            {
                RenderRuntimeResult timeout = MakeRuntimeResult(
                    RenderRuntimeCode::StartupTimedOut,
                    m_executorKind,
                    m_config.backendType,
                    m_initialSurface.generation);
                timeout.message =
                    "Render runtime startup publication did not converge";
                (void)TryClaimTerminalResult(timeout);
                TransitionTo(RenderLifecycleState::Failed,
                             "startup publication watchdog expired");
                SealPublication();
                RenderShutdownResult shutdown = MakeShutdownResult(
                    RenderShutdownCode::TimedOut,
                    timeout.backend,
                    m_lastSubmittedFrameSequence.load(
                        std::memory_order_acquire),
                    timeout.surfaceGeneration);
                shutdown.message =
                    "Startup publication fatal policy watchdog expired";
                StoreShutdownResultAndRecordFailure(std::move(shutdown));
                PublishDiagnostics();
                TerminateAfterFatalDiagnostics();
            }

            lock.lock();
            if (!m_startupAcknowledged)
            {
                startupTimedOut = true;
                m_startupResolution.store(StartupResolution::TimedOut,
                                          std::memory_order_release);
                RenderRuntimeResult timeout = MakeRuntimeResult(
                    RenderRuntimeCode::StartupTimedOut,
                    m_executorKind,
                    m_config.backendType,
                    m_initialSurface.generation);
                timeout.message =
                    "Render runtime startup watchdog expired";
                (void)TryClaimTerminalResultLocked(timeout);
            }
        }
        const RenderRuntimeResult result = m_lastRuntimeResult;
        lock.unlock();

        if (startupTimedOut)
        {
            TransitionTo(RenderLifecycleState::Failed,
                         "startup watchdog expired");
            SealPublication();
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
            StoreShutdownResultAndRecordFailure(std::move(shutdown));
            PublishDiagnostics();
            TerminateAfterFatalDiagnostics();
        }

        if (result.code != RenderRuntimeCode::Running)
        {
            const RenderExecutorJoinResult join = m_executor->JoinUntil(
                std::chrono::steady_clock::now() + m_config.shutdownWatchdog);
            if (join.code == RenderExecutorJoinCode::Joined)
            {
                m_joined.store(true, std::memory_order_release);
            }
            else
            {
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
                        ? "Render executor join watchdog expired after startup failure"
                        : "Render executor rejected join after startup failure";
                StoreShutdownResultAndRecordFailure(std::move(shutdown));
                PublishDiagnostics();
                TerminateAfterFatalDiagnostics();
            }
        }
        return result;
    }

    RenderShutdownResult RenderThreadRuntime::Stop()
    {
        const bool started = m_started.load(std::memory_order_acquire);
        const bool joined = m_joined.load(std::memory_order_acquire);
        if (!started || joined)
        {
            const RenderShutdownResult existing = GetLastShutdownResult();
            if (existing.code != RenderShutdownCode::None)
            {
                return existing;
            }
            const RenderRuntimeResult runtime = GetLastRuntimeResult();
            RenderShutdownResult result = MakeShutdownResult(
                RenderShutdownCode::AlreadyStopped,
                runtime.backend,
                m_lastSubmittedFrameSequence.load(std::memory_order_acquire),
                runtime.surfaceGeneration);
            StoreShutdownResult(result);
            return result;
        }

        const RenderLifecycleState lifecycle =
            m_lifecycle.load(std::memory_order_acquire);
        if (lifecycle != RenderLifecycleState::Failed &&
            lifecycle != RenderLifecycleState::Stopped)
        {
            TransitionTo(RenderLifecycleState::StopRequested,
                         "stop requested");
            const RHIBackendType backend = GetLastRuntimeResult().backend;
            SealPublication();
            const NativeSurfaceDesc currentSurface =
                GetCurrentSurfaceSnapshot();
            StoreRuntimeResult(MakeRuntimeResult(
                RenderRuntimeCode::StopRequested,
                m_executorKind,
                backend,
                currentSurface.generation));
            m_controlMailbox->RequestStop();
            NotifyExecutor();
        }

        const NativeSurfaceDesc currentSurface = GetCurrentSurfaceSnapshot();

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
        const RenderShutdownResult existingShutdown =
            GetLastShutdownResult();
        RenderShutdownResult result = MakeShutdownResult(
            code,
            GetLastRuntimeResult().backend,
            m_lastSubmittedFrameSequence.load(std::memory_order_acquire),
            currentSurface.generation);
        result.message = join.code == RenderExecutorJoinCode::TimedOut
                             ? "Render executor join watchdog expired"
                             : "Render executor rejected join";
        if (existingShutdown.resultClass != RenderResultClass::RuntimeFatal)
        {
            if (join.code == RenderExecutorJoinCode::TimedOut)
            {
                RenderRuntimeResult runtime = MakeRuntimeResult(
                    RenderRuntimeCode::ShutdownTimedOut,
                    m_executorKind,
                    GetLastRuntimeResult().backend,
                    currentSurface.generation);
                runtime.message = result.message;
                (void)TryClaimTerminalResult(runtime);
            }
            else if (GetLastRuntimeResult().resultClass !=
                     RenderResultClass::RuntimeFatal)
            {
                RenderRuntimeResult runtime = MakeRuntimeResult(
                    RenderRuntimeCode::OwnershipViolation,
                    m_executorKind,
                    GetLastRuntimeResult().backend,
                    currentSurface.generation);
                runtime.message = result.message;
                (void)TryClaimTerminalResult(runtime);
            }
            StoreShutdownResultAndRecordFailure(std::move(result));
        }
        TransitionTo(RenderLifecycleState::Failed,
                     "render executor did not join");
        PublishDiagnostics();
        TerminateAfterFatalDiagnostics();
    }

    RenderFramePublishResult RenderThreadRuntime::TryPublishFrame(
        std::unique_ptr<const RenderFramePacket> packet)
    {
        RenderFramePublishResult result;
        result.sequence = packet != nullptr ? packet->GetHeader().sequence : 0U;
        {
            std::lock_guard lock(m_publicationMutex);
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
                if (m_publicationHook != nullptr)
                {
                    m_publicationHook->BeforeMutation(
                        RenderPublicationPath::Frame);
                }
                const RenderFrameMailboxPublishResult mailboxResult =
                    m_frameMailbox->TryPublish(std::move(packet));
                switch (mailboxResult.code)
                {
                    case RenderFrameMailboxPublishCode::Accepted:
                        result.code = RenderFramePublishCode::Accepted;
                        break;
                    case RenderFrameMailboxPublishCode::ReplacedOldest:
                        result.code = RenderFramePublishCode::ReplacedOlder;
                        result.replacedSequence =
                            mailboxResult.replacedSequence;
                        m_frameReplacementCount.fetch_add(
                            1, std::memory_order_relaxed);
                        break;
                    case RenderFrameMailboxPublishCode::InvalidPacket:
                        result.code = RenderFramePublishCode::InvalidPacket;
                        m_invalidFrameCount.fetch_add(
                            1, std::memory_order_relaxed);
                        break;
                    case RenderFrameMailboxPublishCode::OutOfOrder:
                        result.code = RenderFramePublishCode::OutOfOrder;
                        m_outOfOrderFrameCount.fetch_add(
                            1, std::memory_order_relaxed);
                        break;
                }
                if (result.code == RenderFramePublishCode::Accepted ||
                    result.code == RenderFramePublishCode::ReplacedOlder)
                {
                    m_lastPublishedFrameSequence.store(
                        result.sequence, std::memory_order_release);
                }
            }
        }
        result.resultClass = ClassifyRenderFramePublishCode(result.code);
        if (result.code == RenderFramePublishCode::Accepted ||
            result.code == RenderFramePublishCode::ReplacedOlder)
        {
            NotifyExecutor();
        }
        return result;
    }

    RenderResizeResult RenderThreadRuntime::RequestResize(
        const NativeSurfaceDesc& surface)
    {
        RenderResizeResult result;
        result.generation = surface.generation;
        {
            std::lock_guard lock(m_publicationMutex);
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
                m_resizeRejectedCount.fetch_add(1,
                                                std::memory_order_relaxed);
            }
            else
            {
                if (m_publicationHook != nullptr)
                {
                    m_publicationHook->BeforeMutation(
                        RenderPublicationPath::Resize);
                }
                if (surface.generation <= m_latestResizeGeneration)
                {
                    result.code = RenderResizeCode::StaleGeneration;
                    m_resizeRejectedCount.fetch_add(
                        1, std::memory_order_relaxed);
                }
                else
                {
                    result.replacedGeneration = m_pendingResizeGeneration;
                    const bool published = m_controlMailbox->TryPublishResize(
                        surface.generation, surface);
                    if (!published)
                    {
                        result.code = RenderResizeCode::StaleGeneration;
                        m_resizeRejectedCount.fetch_add(
                            1, std::memory_order_relaxed);
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

    bool RenderThreadRuntime::IsReady() const
    {
        std::lock_guard lock(m_publicationMutex);
        if (m_publicationSealed.load(std::memory_order_acquire) ||
            m_lifecycle.load(std::memory_order_acquire) !=
                RenderLifecycleState::Running)
        {
            return false;
        }
        const RenderRuntimeResult result = GetLastRuntimeResult();
        return result.lifecycle == RenderLifecycleState::Running &&
               result.terminalCause == RenderTerminalCause::None &&
               result.teardownMode == RenderTeardownMode::None;
    }

    RenderResourceReserveResult RenderThreadRuntime::ReserveResource(
        AssetId assetId,
        RenderResourceKind kind) noexcept
    {
        std::lock_guard lock(m_publicationMutex);
        if (m_publicationSealed.load(std::memory_order_acquire))
        {
            RenderResourceReserveResult result;
            result.code = RenderResourceReserveCode::ShuttingDown;
            return result;
        }
        return m_resourceGateway != nullptr
                   ? m_resourceGateway->ReserveResource(assetId, kind)
                   : RenderResourceReserveResult{};
    }

    RenderUploadEnqueueResult RenderThreadRuntime::TryEnqueueUpload(
        const ResourceUploadRequestRef& request) noexcept
    {
        {
            std::lock_guard lock(m_publicationMutex);
            if (m_publicationSealed.load(std::memory_order_acquire))
            {
                return RenderUploadEnqueueResult{
                    RenderUploadEnqueueCode::ShuttingDown};
            }
            if (m_resourceGateway == nullptr)
            {
                return {};
            }
            const RenderGatewayUploadEnqueueResult observed =
                m_resourceGateway->TryEnqueueUploadObserved(request);
            const RenderUploadEnqueueResult result = observed.result;
            if (result.code == RenderUploadEnqueueCode::Accepted)
            {
                m_uploadAcceptedCount.fetch_add(
                    1, std::memory_order_relaxed);
            }
            else if (result.code ==
                         RenderUploadEnqueueCode::QueueFullByCount ||
                     result.code ==
                         RenderUploadEnqueueCode::QueueFullByBytes)
            {
                m_uploadPressureCount.fetch_add(
                    1, std::memory_order_relaxed);
            }
            if (result.code != RenderUploadEnqueueCode::Accepted)
            {
                return result;
            }
        }
        NotifyExecutor();
        return RenderUploadEnqueueResult{RenderUploadEnqueueCode::Accepted};
    }

    RenderReleaseResult RenderThreadRuntime::RequestRelease(
        RenderResourceHandle handle) noexcept
    {
        {
            std::lock_guard lock(m_publicationMutex);
            if (m_publicationSealed.load(std::memory_order_acquire))
            {
                return RenderReleaseResult{
                    RenderReleaseCode::ShuttingDown};
            }
            if (m_resourceGateway == nullptr)
            {
                return {};
            }
            const RenderGatewayReleaseResult observed =
                m_resourceGateway->RequestReleaseObserved(handle);
            const RenderReleaseResult result = observed.result;
            if (result.code == RenderReleaseCode::Accepted)
            {
                m_releaseAcceptedCount.fetch_add(
                    1, std::memory_order_relaxed);
            }
            else if (result.code == RenderReleaseCode::StaleGeneration)
            {
                m_releaseStaleCount.fetch_add(
                    1, std::memory_order_relaxed);
            }
            if (result.code != RenderReleaseCode::Accepted)
            {
                return result;
            }
        }
        NotifyExecutor();
        return RenderReleaseResult{RenderReleaseCode::Accepted};
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
            if (m_ownerFatalPending.exchange(false,
                                             std::memory_order_acq_rel))
            {
                return FailOnRenderThread(GetLastRuntimeResult());
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
            const bool shouldWake =
                m_wakePending.load(std::memory_order_acquire) ||
                (m_controlMailbox != nullptr &&
                 m_controlMailbox->IsStopRequested());
            if (!shouldWake && m_waitHook != nullptr)
            {
                m_waitHook->AfterFalseWaitPredicate();
            }
            return shouldWake;
        });
    }

    void RenderThreadRuntime::Wake() noexcept
    {
        {
            std::lock_guard lock(m_waitMutex);
            m_wakePending.store(true, std::memory_order_release);
        }
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
            (void)TryClaimTerminalResult(result);
            const RenderRuntimeResult terminal = GetLastRuntimeResult();
            SealPublication();
            TransitionTo(RenderLifecycleState::Failed,
                         "unhandled executor exception");
            const bool onOwnerThread =
                m_renderThreadGuard.QueryCurrentThread() ==
                RenderThreadGuardCode::Owner;
            if (m_consumer != nullptr && onOwnerThread)
            {
                RenderShutdownResult shutdown = NormalizeShutdownResult(
                    m_consumer->Shutdown(terminal.teardownMode),
                    terminal.backend,
                    m_lastSubmittedFrameSequence.load(
                        std::memory_order_acquire),
                    terminal.surfaceGeneration);
                StoreShutdownResultAndRecordFailure(std::move(shutdown));
                m_consumer.reset();
            }
            const bool acknowledgeStartup =
                onOwnerThread || m_consumer == nullptr;
            if (acknowledgeStartup)
            {
                std::lock_guard lock(m_stateMutex);
                m_startupAcknowledged = true;
            }
            if (acknowledgeStartup)
            {
                m_startupCv.notify_all();
            }
            PublishDiagnostics();
        }
        catch (...)
        {
        }
        Wake();
    }

    void RenderThreadRuntime::GatewayPublicationThunk(
        void* context,
        RenderGatewayPublicationPath path) noexcept
    {
        auto* runtime = static_cast<RenderThreadRuntime*>(context);
        if (runtime->m_publicationHook == nullptr)
        {
            return;
        }

        RenderPublicationPath runtimePath = RenderPublicationPath::Reserve;
        if (path == RenderGatewayPublicationPath::Upload)
        {
            runtimePath = RenderPublicationPath::Upload;
        }
        else if (path == RenderGatewayPublicationPath::Release)
        {
            runtimePath = RenderPublicationPath::Release;
        }
        runtime->m_publicationHook->BeforeMutation(runtimePath);
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
                runtime->m_currentSurface.generation);
            result.message = message != nullptr ? message :
                                                  "Render transport invariant failed";
            (void)runtime->TryClaimTerminalResult(result);
            runtime->SealPublicationLocked();
            runtime->m_ownerFatalPending.store(true,
                                               std::memory_order_release);
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
        if (m_publicationSealed.load(std::memory_order_acquire))
        {
            return true;
        }
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

    bool RenderThreadRuntime::TryClaimTerminalResult(
        const RenderRuntimeResult& result)
    {
        std::lock_guard lock(m_stateMutex);
        return TryClaimTerminalResultLocked(result);
    }

    bool RenderThreadRuntime::TryClaimTerminalResultLocked(
        const RenderRuntimeResult& result)
    {
        if (m_terminalResultClaimed)
        {
            if (!result.message.empty() &&
                m_diagnosticsState.lastFailure.context.find(result.message) ==
                    std::string::npos)
            {
                if (!m_diagnosticsState.lastFailure.context.empty())
                {
                    m_diagnosticsState.lastFailure.context += " | ";
                }
                m_diagnosticsState.lastFailure.context += result.message;
            }
            return false;
        }

        m_terminalResultClaimed = true;
        m_lastRuntimeResult = result;
        m_diagnosticsState.lastFailure.available = true;
        m_diagnosticsState.lastFailure.runtime = result;
        m_diagnosticsState.lastFailure.context = result.message;
        return true;
    }

    bool RenderThreadRuntime::HasTerminalResult() const
    {
        std::lock_guard lock(m_stateMutex);
        return m_terminalResultClaimed;
    }

    void RenderThreadRuntime::SealPublication() noexcept
    {
        if (m_publicationHook != nullptr)
        {
            m_publicationHook->BeforeSeal();
        }
        std::lock_guard lock(m_publicationMutex);
        SealPublicationLocked();
    }

    void RenderThreadRuntime::SealPublicationLocked() noexcept
    {
        m_publicationSealed.store(true, std::memory_order_release);
        if (m_resourceGateway != nullptr)
        {
            m_resourceGateway->BeginShutdown();
        }
    }

    void RenderThreadRuntime::StoreRuntimeResult(RenderRuntimeResult result)
    {
        std::lock_guard lock(m_stateMutex);
        if (m_terminalResultClaimed)
        {
            return;
        }
        m_lastRuntimeResult = std::move(result);
    }

    void RenderThreadRuntime::StoreShutdownResult(RenderShutdownResult result)
    {
        std::lock_guard lock(m_stateMutex);
        if (m_lastShutdownResult.resultClass ==
            RenderResultClass::RuntimeFatal)
        {
            return;
        }
        m_lastShutdownResult = std::move(result);
    }

    void RenderThreadRuntime::StoreShutdownResultAndRecordFailure(
        RenderShutdownResult result)
    {
        std::lock_guard lock(m_stateMutex);
        const bool alreadyFatal =
            m_lastShutdownResult.resultClass ==
            RenderResultClass::RuntimeFatal;
        const std::string supplementalContext = result.message;
        if (!alreadyFatal)
        {
            m_lastShutdownResult = std::move(result);
        }

        if (m_lastShutdownResult.resultClass !=
            RenderResultClass::RuntimeFatal)
        {
            return;
        }

        m_diagnosticsState.lastFailure.available = true;
        m_diagnosticsState.lastFailure.shutdown = m_lastShutdownResult;
        if (!supplementalContext.empty() &&
            m_diagnosticsState.lastFailure.context.find(
                supplementalContext) == std::string::npos)
        {
            if (!m_diagnosticsState.lastFailure.context.empty())
            {
                m_diagnosticsState.lastFailure.context += " | ";
            }
            m_diagnosticsState.lastFailure.context += supplementalContext;
        }
    }

    void RenderThreadRuntime::RecordFailure(
        const RenderRuntimeResult& result)
    {
        std::lock_guard lock(m_stateMutex);
        if (m_terminalResultClaimed &&
            m_diagnosticsState.lastFailure.runtime.code != result.code)
        {
            if (!result.message.empty())
            {
                if (!m_diagnosticsState.lastFailure.context.empty())
                {
                    m_diagnosticsState.lastFailure.context += " | ";
                }
                m_diagnosticsState.lastFailure.context += result.message;
            }
            return;
        }
        m_diagnosticsState.lastFailure.available = true;
        m_diagnosticsState.lastFailure.runtime = result;
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
            const RenderFrameMailboxSnapshot frameQueue =
                m_frameMailbox->GetSnapshot();
            snapshot.frameTransport.currentUsage = frameQueue.pendingCount;
            snapshot.frameTransport.highWaterMark =
                frameQueue.highWaterMark;
        }
        snapshot.frameTransport.replacements =
            m_frameReplacementCount.load(std::memory_order_relaxed);
        snapshot.frameTransport.invalidPackets =
            m_invalidFrameCount.load(std::memory_order_relaxed);
        snapshot.frameTransport.outOfOrderRejections =
            m_outOfOrderFrameCount.load(std::memory_order_relaxed);

        if (m_resourceGateway != nullptr)
        {
            const RenderUploadQueueSnapshot uploadQueue =
                m_resourceGateway->GetUploadQueueSnapshot();
            snapshot.uploadTransport.currentRequestUsage =
                uploadQueue.retainedCount;
            snapshot.uploadTransport.currentByteUsage =
                uploadQueue.retainedBytes;
            snapshot.uploadTransport.requestHighWaterMark =
                uploadQueue.requestHighWaterMark;
            snapshot.uploadTransport.byteHighWaterMark =
                uploadQueue.byteHighWaterMark;
        }
        snapshot.uploadTransport.pressureOutcomes =
            m_uploadPressureCount.load(std::memory_order_relaxed);
        snapshot.uploadTransport.acceptedCount =
            m_uploadAcceptedCount.load(std::memory_order_relaxed);
        snapshot.uploadTransport.completedCount =
            m_uploadCompletedCount.load(std::memory_order_relaxed);
        snapshot.uploadTransport.failedCount =
            m_uploadFailedCount.load(std::memory_order_relaxed);
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
        snapshot.releaseTransport.highWaterMark =
            releaseQueue.highWaterMark;
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

    [[noreturn]] void RenderThreadRuntime::TerminateAfterFatalDiagnostics()
    {
        m_fatalPolicy->Terminate(GetDiagnosticsSnapshot());
        std::terminate();
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
        if (m_lifecycleHook != nullptr)
        {
            m_lifecycleHook->BeforeStartupAcknowledgement();
        }

        std::unique_lock arbitrationLock(m_startupArbitrationMutex);
        if (m_lifecycleHook != nullptr)
        {
            m_lifecycleHook->DuringStartupPublication();
        }
        if (m_startupResolution.load(std::memory_order_acquire) ==
            StartupResolution::TimedOut)
        {
            arbitrationLock.unlock();
            return FinishTimedOutStartupOnRenderThread();
        }
        if (HasTerminalResult())
        {
            arbitrationLock.unlock();
            return FailOnRenderThread(GetLastRuntimeResult());
        }
        if (result.code != RenderRuntimeCode::Running)
        {
            (void)TryClaimTerminalResult(result);
            SealPublication();
            RenderShutdownResult shutdown = NormalizeShutdownResult(
                m_consumer->Shutdown(result.teardownMode),
                result.backend,
                m_lastSubmittedFrameSequence.load(
                    std::memory_order_acquire),
                result.surfaceGeneration);
            StoreShutdownResultAndRecordFailure(std::move(shutdown));
            m_consumer.reset();
            TransitionTo(RenderLifecycleState::Failed, "startup failed");
            PublishDiagnostics();
            m_startupResolution.store(StartupResolution::RenderCompleted,
                                      std::memory_order_release);
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
        m_startupResolution.store(StartupResolution::RenderCompleted,
                                  std::memory_order_release);
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
        StoreShutdownResultAndRecordFailure(std::move(shutdown));
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
        StoreShutdownResultAndRecordFailure(shutdown);
        if (shutdown.resultClass == RenderResultClass::RuntimeFatal ||
            HasTerminalResult())
        {
            if (shutdown.resultClass == RenderResultClass::RuntimeFatal)
            {
                if (!HasTerminalResult())
                {
                    RenderRuntimeCode runtimeCode =
                        RenderRuntimeCode::OwnershipViolation;
                    if (shutdown.code == RenderShutdownCode::DeviceLost)
                    {
                        runtimeCode = RenderRuntimeCode::DeviceLost;
                    }
                    else if (shutdown.code == RenderShutdownCode::TimedOut)
                    {
                        runtimeCode = RenderRuntimeCode::ShutdownTimedOut;
                    }
                    RenderRuntimeResult shutdownRuntime = MakeRuntimeResult(
                        runtimeCode,
                        m_executorKind,
                        shutdown.backend,
                        currentSurface.generation);
                    shutdownRuntime.nativeError = shutdown.nativeError;
                    shutdownRuntime.message = shutdown.message;
                    (void)TryClaimTerminalResult(shutdownRuntime);
                }
            }
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
        (void)TryClaimTerminalResult(result);
        result = GetLastRuntimeResult();
        SealPublication();
        TransitionTo(RenderLifecycleState::Failed, "runtime fatal failure");
        if (m_consumer != nullptr)
        {
            RenderShutdownResult shutdown = NormalizeShutdownResult(
                m_consumer->Shutdown(result.teardownMode),
                result.backend,
                m_lastSubmittedFrameSequence.load(std::memory_order_acquire),
                currentSurface.generation);
            StoreShutdownResultAndRecordFailure(std::move(shutdown));
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
