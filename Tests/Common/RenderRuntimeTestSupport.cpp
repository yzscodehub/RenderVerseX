#include "Common/RenderRuntimeTestSupport.h"

#include "Runtime/DedicatedRenderExecutor.h"
#include "Runtime/RenderThreadGuard.h"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace RVX
{
namespace
{
    class InlineRenderExecutor final : public IRenderExecutor,
                                       public NonMovable
    {
    public:
        InlineRenderExecutor() = default;

        InlineRenderExecutor(
            std::vector<RenderExecutorJoinCode> joinCodes,
            std::shared_ptr<RenderExecutorJoinTestProbe> joinProbe)
            : m_joinCodes(std::move(joinCodes)),
              m_joinProbe(std::move(joinProbe))
        {
        }

        RenderExecutorStartResult Start(IRenderExecutorPump& pump) override
        {
            std::lock_guard lock(m_stateMutex);
            if (m_started)
            {
                return RenderExecutorStartResult{
                    RenderExecutorStartCode::AlreadyStarted,
                    0};
            }

            if (m_threadGuard.BindCurrentThread() !=
                RenderThreadGuardCode::Owner)
            {
                return RenderExecutorStartResult{
                    RenderExecutorStartCode::ThreadCreationFailed,
                    0};
            }

            m_pump = &pump;
            m_started = true;
            return RenderExecutorStartResult{
                RenderExecutorStartCode::Started,
                0};
        }

        void NotifyWork() noexcept override
        {
            IRenderExecutorPump* pump = nullptr;
            {
                std::lock_guard lock(m_stateMutex);
                if (!m_started || m_exited || m_pump == nullptr)
                {
                    return;
                }
                pump = m_pump;
            }

            if (m_threadGuard.ValidateCurrentThread() !=
                RenderThreadGuardCode::Owner)
            {
                pump->OnUnhandledExecutorException();
                PublishExit();
                return;
            }

            try
            {
                while (true)
                {
                    const RenderPumpDecision decision = pump->PumpOnce();
                    if (decision == RenderPumpDecision::Idle)
                    {
                        return;
                    }
                    if (decision == RenderPumpDecision::Stop)
                    {
                        PublishExit();
                        return;
                    }
                }
            }
            catch (...)
            {
                pump->OnUnhandledExecutorException();
                PublishExit();
            }
        }

        RenderExecutorJoinResult JoinUntil(
            std::chrono::steady_clock::time_point deadline) override
        {
            if (m_nextJoinCode < m_joinCodes.size())
            {
                const RenderExecutorJoinCode code =
                    m_joinCodes[m_nextJoinCode++];
                if (m_joinProbe != nullptr)
                {
                    m_joinProbe->Record(code);
                }
                if (code != RenderExecutorJoinCode::Joined)
                {
                    return RenderExecutorJoinResult{code};
                }
            }

            std::unique_lock lock(m_stateMutex);
            if (!m_started)
            {
                return RenderExecutorJoinResult{
                    RenderExecutorJoinCode::NotStarted};
            }
            if (!m_exitCv.wait_until(lock,
                                     deadline,
                                     [this]() { return m_exited; }))
            {
                return RenderExecutorJoinResult{
                    RenderExecutorJoinCode::TimedOut};
            }
            return RenderExecutorJoinResult{RenderExecutorJoinCode::Joined};
        }

    private:
        void PublishExit() noexcept
        {
            {
                std::lock_guard lock(m_stateMutex);
                m_exited = true;
            }
            m_exitCv.notify_all();
        }

        RenderThreadGuard m_threadGuard;
        std::mutex m_stateMutex;
        std::condition_variable m_exitCv;
        IRenderExecutorPump* m_pump = nullptr;
        bool m_started = false;
        bool m_exited = false;
        std::vector<RenderExecutorJoinCode> m_joinCodes;
        size_t m_nextJoinCode = 0;
        std::shared_ptr<RenderExecutorJoinTestProbe> m_joinProbe;
    };

    class FailingRenderExecutor final : public IRenderExecutor,
                                        public NonMovable
    {
    public:
        explicit FailingRenderExecutor(uint32 nativeError)
            : m_nativeError(nativeError)
        {
        }

        RenderExecutorStartResult Start(IRenderExecutorPump&) override
        {
            return RenderExecutorStartResult{
                RenderExecutorStartCode::ThreadCreationFailed,
                m_nativeError};
        }

        void NotifyWork() noexcept override
        {
        }

        RenderExecutorJoinResult JoinUntil(
            std::chrono::steady_clock::time_point) override
        {
            return RenderExecutorJoinResult{
                RenderExecutorJoinCode::NotStarted};
        }

    private:
        uint32 m_nativeError = 0;
    };

    class JoinRecordingDedicatedExecutor final : public IRenderExecutor,
                                                 public NonMovable
    {
    public:
        explicit JoinRecordingDedicatedExecutor(
            std::shared_ptr<RenderExecutorJoinTestProbe> probe,
            std::shared_ptr<IDedicatedRenderExecutorBootstrapHook>
                bootstrapHook = nullptr)
            : m_probe(std::move(probe)),
              m_executor(bootstrapHook != nullptr
                             ? CreateDedicatedRenderExecutor(
                                   std::move(bootstrapHook))
                             : CreateDedicatedRenderExecutor())
        {
        }

        ~JoinRecordingDedicatedExecutor() override
        {
            if (m_started && !m_joined)
            {
                (void)m_executor->JoinUntil(
                    std::chrono::steady_clock::now() +
                    std::chrono::seconds(2));
            }
        }

        RenderExecutorStartResult Start(
            IRenderExecutorPump& pump) override
        {
            const RenderExecutorStartResult result = m_executor->Start(pump);
            m_started = result.code == RenderExecutorStartCode::Started;
            return result;
        }

        void NotifyWork() noexcept override
        {
            m_executor->NotifyWork();
        }

        RenderExecutorJoinResult JoinUntil(
            std::chrono::steady_clock::time_point deadline) override
        {
            const RenderExecutorJoinResult result =
                m_executor->JoinUntil(deadline);
            m_probe->Record(result.code);
            m_joined = result.code == RenderExecutorJoinCode::Joined;
            return result;
        }

    private:
        std::shared_ptr<RenderExecutorJoinTestProbe> m_probe;
        std::unique_ptr<IRenderExecutor> m_executor;
        bool m_started = false;
        bool m_joined = false;
    };

    class RecordingRenderFrameConsumer final : public IRenderFrameConsumer,
                                               public NonMovable
    {
    public:
        explicit RecordingRenderFrameConsumer(
            std::shared_ptr<RenderFrameConsumerTestProbe> probe)
            : m_probe(std::move(probe))
        {
        }

        ~RecordingRenderFrameConsumer() override
        {
            m_probe->SetDestructionThread(std::this_thread::get_id());
        }

        RenderRuntimeResult Initialize(
            const RenderRuntimeConfig& config,
            const NativeSurfaceDesc& surface,
            RenderResourceStatusTable&) override
        {
            m_probe->SetStartupThread(std::this_thread::get_id());
            m_probe->Record(RenderRuntimeTestEvent::Started);
            m_probe->WaitWhileStartupBlocked();
            RenderRuntimeResult result;
            result.code = m_probe->startupCode;
            result.backend = m_probe->startupBackend == RHIBackendType::None
                                 ? config.backendType
                                 : m_probe->startupBackend;
            result.surfaceGeneration = surface.generation;
            result.nativeError = m_probe->startupNativeError;
            result.message = m_probe->startupMessage;
            return result;
        }

        RenderRuntimeResult ApplySurface(
            const NativeSurfaceDesc& surface) override
        {
            m_probe->Record(RenderRuntimeTestEvent::Surface);
            RenderRuntimeResult result;
            result.code = RenderRuntimeCode::Running;
            result.surfaceGeneration = surface.generation;
            return result;
        }

        void ProcessRelease(RenderResourceHandle) override
        {
            m_probe->Record(RenderRuntimeTestEvent::Release);
        }

        void ProcessUpload(ResourceUploadRequestRef) override
        {
            m_probe->Record(RenderRuntimeTestEvent::Upload);
        }

        RenderRuntimeResult ConsumeFrameV5(
            const RenderFramePacketV5& packet,
            const RenderSceneDatabase& scene) override
        {
            m_probe->Record(RenderRuntimeTestEvent::Frame);
            m_probe->RecordConsumedSceneDatabase(scene);
            if (m_probe->throwOnFrame)
            {
                throw std::runtime_error("forced consumer frame exception");
            }
            m_probe->WaitWhileFrameBlocked();
            RenderRuntimeResult result;
            result.code = m_probe->frameCode;
            result.frameSequence = packet.GetHeader().sequence;
            result.message = m_probe->frameMessage;
            return result;
        }

        void PollCompletion() override
        {
            m_probe->Record(RenderRuntimeTestEvent::Poll);
        }

        void RetireCompleted() override
        {
            m_probe->Record(RenderRuntimeTestEvent::Retire);
        }

        void PopulateDiagnostics(
            RenderDiagnosticsSnapshot& outDiagnostics) const override
        {
            outDiagnostics.frameFeatures = m_probe->frameDiagnostics;
        }

        RenderShutdownResult Shutdown(
            RenderTeardownMode) noexcept override
        {
            m_probe->SetShutdownThread(std::this_thread::get_id());
            m_probe->Record(RenderRuntimeTestEvent::Shutdown);
            RenderShutdownResult result;
            result.code = m_probe->shutdownCode;
            result.backend = m_probe->shutdownBackend;
            result.nativeError = m_probe->shutdownNativeError;
            result.message = m_probe->shutdownMessage;
            return result;
        }

    private:
        std::shared_ptr<RenderFrameConsumerTestProbe> m_probe;
    };

    class FaultPlanFrameConsumer final : public IRenderFrameConsumer,
                                         public NonMovable
    {
    public:
        FaultPlanFrameConsumer(
            RenderRuntimeFaultPlan plan,
            std::shared_ptr<RenderRuntimeFaultProbe> probe)
            : m_plan(plan), m_probe(std::move(probe))
        {
            m_probe->RecordConstruction();
        }

        ~FaultPlanFrameConsumer() override
        {
            m_probe->RecordDestruction();
        }

        RenderRuntimeResult Initialize(
            const RenderRuntimeConfig& config,
            const NativeSurfaceDesc& surface,
            RenderResourceStatusTable&) override
        {
            m_surfaceGeneration = surface.generation;
            RenderRuntimeResult result = MakeRunning(config.backendType,
                                                     surface.generation);
            if (ShouldFail(RenderRuntimeFaultPoint::DeviceCreation) ||
                ShouldFail(RenderRuntimeFaultPoint::ContextCreation) ||
                ShouldFail(RenderRuntimeFaultPoint::RendererCreation) ||
                ShouldFail(RenderRuntimeFaultPoint::ResourceCreation))
            {
                result.code = RenderRuntimeCode::DeviceCreationFailed;
                result.nativeError = m_plan.nativeError;
                result.message = "Injected render startup construction failure";
            }
            else if (ShouldFail(RenderRuntimeFaultPoint::SurfaceCreation))
            {
                result.code = RenderRuntimeCode::SurfaceCreationFailed;
                result.nativeError = m_plan.nativeError;
                result.message = "Injected render surface creation failure";
            }
            return result;
        }

        RenderRuntimeResult ApplySurface(
            const NativeSurfaceDesc& surface) override
        {
            m_surfaceGeneration = surface.generation;
            RenderRuntimeResult result = MakeRunning(RHIBackendType::DX11,
                                                     surface.generation);
            if (ShouldFail(RenderRuntimeFaultPoint::Resize))
            {
                return MakeDeviceLost(surface.generation,
                                      "Injected resize device loss");
            }
            return result;
        }

        void ProcessRelease(RenderResourceHandle) override
        {
        }

        void ProcessUpload(ResourceUploadRequestRef) override
        {
            if (ShouldFail(RenderRuntimeFaultPoint::UploadSubmission))
            {
                m_deviceLost.store(true, std::memory_order_release);
            }
        }

        RenderRuntimeResult ConsumeFrameV5(
            const RenderFramePacketV5& packet,
            const RenderSceneDatabase&) override
        {
            if (ShouldFail(RenderRuntimeFaultPoint::FrameException))
            {
                throw std::runtime_error("Injected frame exception");
            }
            if (ShouldFail(RenderRuntimeFaultPoint::Present) ||
                ShouldFail(
                    RenderRuntimeFaultPoint::DeviceLossBeforeSubmission))
            {
                return MakeDeviceLost(m_surfaceGeneration,
                                      "Injected frame device loss");
            }
            if (ShouldFail(RenderRuntimeFaultPoint::DeviceLossInFlight))
            {
                m_deviceLost.store(true, std::memory_order_release);
            }
            RenderRuntimeResult result = MakeRunning(
                RHIBackendType::DX11,
                m_surfaceGeneration);
            result.frameSequence = packet.GetHeader().sequence;
            return result;
        }

        void PollCompletion() override
        {
            if (ShouldFail(RenderRuntimeFaultPoint::FencePoll))
            {
                m_deviceLost.store(true, std::memory_order_release);
            }
        }

        void RetireCompleted() override
        {
            if (ShouldFail(RenderRuntimeFaultPoint::FenceWait))
            {
                m_deviceLost.store(true, std::memory_order_release);
            }
        }

        RenderRuntimeResult QueryRuntimeStatus() const override
        {
            return m_deviceLost.load(std::memory_order_acquire)
                       ? MakeDeviceLost(0, "Injected asynchronous device loss")
                       : MakeRunning(RHIBackendType::DX11, 0);
        }

        RenderShutdownResult Shutdown(
            RenderTeardownMode mode) noexcept override
        {
            m_probe->RecordShutdown(mode);
            RenderShutdownResult result;
            result.code = RenderShutdownCode::Completed;
            result.backend = RHIBackendType::DX11;
            if (m_plan.point ==
                RenderRuntimeFaultPoint::DeviceLossDuringShutdown)
            {
                result.code = RenderShutdownCode::DeviceLost;
                result.nativeError = m_plan.nativeError;
                result.message = "Injected shutdown device loss";
            }
            return result;
        }

    private:
        [[nodiscard]] bool ShouldFail(RenderRuntimeFaultPoint point)
        {
            if (m_plan.point != point)
            {
                return false;
            }
            ++m_observationCount;
            return m_observationCount == m_plan.occurrence;
        }

        [[nodiscard]] static RenderRuntimeResult MakeRunning(
            RHIBackendType backend,
            uint64 surfaceGeneration)
        {
            RenderRuntimeResult result;
            result.code = RenderRuntimeCode::Running;
            result.lifecycle = RenderLifecycleState::Running;
            result.backend = backend;
            result.surfaceGeneration = surfaceGeneration;
            return result;
        }

        [[nodiscard]] RenderRuntimeResult MakeDeviceLost(
            uint64 surfaceGeneration,
            const char* message) const
        {
            RenderRuntimeResult result;
            result.code = RenderRuntimeCode::DeviceLost;
            result.backend = RHIBackendType::DX11;
            result.surfaceGeneration = surfaceGeneration;
            result.nativeError = m_plan.nativeError;
            result.message = message;
            return result;
        }

        RenderRuntimeFaultPlan m_plan;
        std::shared_ptr<RenderRuntimeFaultProbe> m_probe;
        std::atomic<bool> m_deviceLost = false;
        uint32 m_observationCount = 0;
        uint64 m_surfaceGeneration = 0;
    };

    class FaultPlanRuntimeFactory final : public IRenderRuntimeFactory,
                                          public NonMovable
    {
    public:
        FaultPlanRuntimeFactory(
            RenderRuntimeFaultPlan plan,
            std::shared_ptr<RenderRuntimeFaultProbe> probe)
            : m_plan(plan), m_probe(std::move(probe))
        {
        }

        [[nodiscard]] std::unique_ptr<IRenderFrameConsumer>
            CreateFrameConsumer() override
        {
            if (m_plan.point == RenderRuntimeFaultPoint::FactoryCreation)
            {
                return nullptr;
            }
            return std::make_unique<FaultPlanFrameConsumer>(m_plan, m_probe);
        }

    private:
        RenderRuntimeFaultPlan m_plan;
        std::shared_ptr<RenderRuntimeFaultProbe> m_probe;
    };
} // namespace

    std::unique_ptr<IRenderExecutor> CreateInlineRenderExecutor()
    {
        return std::make_unique<InlineRenderExecutor>();
    }

    std::unique_ptr<IRenderExecutor> CreateFailingRenderExecutor(
        uint32 nativeError)
    {
        return std::make_unique<FailingRenderExecutor>(nativeError);
    }

    void RenderExecutorJoinTestProbe::Record(
        RenderExecutorJoinCode code) noexcept
    {
        m_lastCode.store(code, std::memory_order_release);
        m_callCount.fetch_add(1, std::memory_order_release);
    }

    uint32 RenderExecutorJoinTestProbe::GetCallCount() const noexcept
    {
        return m_callCount.load(std::memory_order_acquire);
    }

    RenderExecutorJoinCode
        RenderExecutorJoinTestProbe::GetLastCode() const noexcept
    {
        return m_lastCode.load(std::memory_order_acquire);
    }

    std::unique_ptr<IRenderExecutor> CreateJoinRecordingDedicatedExecutor(
        std::shared_ptr<RenderExecutorJoinTestProbe> probe)
    {
        return std::make_unique<JoinRecordingDedicatedExecutor>(
            std::move(probe));
    }

    std::unique_ptr<IRenderExecutor> CreateJoinRecordingDedicatedExecutor(
        std::shared_ptr<RenderExecutorJoinTestProbe> probe,
        std::shared_ptr<IDedicatedRenderExecutorBootstrapHook> bootstrapHook)
    {
        return std::make_unique<JoinRecordingDedicatedExecutor>(
            std::move(probe), std::move(bootstrapHook));
    }

    std::unique_ptr<IRenderExecutor>
        CreateSequencedJoinInlineRenderExecutor(
            std::vector<RenderExecutorJoinCode> joinCodes,
            std::shared_ptr<RenderExecutorJoinTestProbe> probe)
    {
        return std::make_unique<InlineRenderExecutor>(
            std::move(joinCodes), std::move(probe));
    }

    void RenderRuntimeLifecycleTestHook::BeforeStartupAcknowledgement()
        noexcept
    {
        std::unique_lock lock(m_mutex);
        m_entered = true;
        m_cv.notify_all();
        m_cv.wait(lock, [this]() { return m_released; });
    }

    bool RenderRuntimeLifecycleTestHook::WaitUntilEntered(
        std::chrono::milliseconds timeout) const
    {
        std::unique_lock lock(m_mutex);
        return m_cv.wait_for(lock, timeout, [this]() { return m_entered; });
    }

    void RenderRuntimeLifecycleTestHook::Release()
    {
        {
            std::lock_guard lock(m_mutex);
            m_released = true;
        }
        m_cv.notify_all();
    }

    void RenderRuntimePublicationTestHook::BeforeStartupAcknowledgement()
        noexcept
    {
    }

    void RenderRuntimePublicationTestHook::DuringStartupPublication() noexcept
    {
        std::unique_lock lock(m_mutex);
        m_entered = true;
        m_cv.notify_all();
        m_cv.wait(lock, [this]() { return m_released; });
    }

    bool RenderRuntimePublicationTestHook::WaitUntilEntered(
        std::chrono::milliseconds timeout) const
    {
        std::unique_lock lock(m_mutex);
        return m_cv.wait_for(lock, timeout, [this]() { return m_entered; });
    }

    void RenderRuntimePublicationTestHook::Release()
    {
        {
            std::lock_guard lock(m_mutex);
            m_released = true;
        }
        m_cv.notify_all();
    }

    void RenderFatalPolicyTestProbe::Terminate(
        const RenderDiagnosticsSnapshot& diagnostics)
    {
        {
            std::lock_guard lock(m_mutex);
            ++m_callCount;
            m_diagnostics = diagnostics;
        }
        throw RenderFatalPolicyIntercept{};
    }

    uint32 RenderFatalPolicyTestProbe::GetCallCount() const noexcept
    {
        std::lock_guard lock(m_mutex);
        return m_callCount;
    }

    RenderDiagnosticsSnapshot
        RenderFatalPolicyTestProbe::GetDiagnostics() const
    {
        std::lock_guard lock(m_mutex);
        return m_diagnostics;
    }

    void RenderFrameConsumerTestProbe::Record(RenderRuntimeTestEvent event)
    {
        {
            std::lock_guard lock(m_mutex);
            ++m_eventCounts[static_cast<size_t>(event)];
            m_events.push_back(event);
        }
        m_cv.notify_all();
    }

    std::vector<RenderRuntimeTestEvent>
        RenderFrameConsumerTestProbe::GetEvents() const
    {
        std::lock_guard lock(m_mutex);
        return m_events;
    }

    void RenderFrameConsumerTestProbe::ClearEvents()
    {
        std::lock_guard lock(m_mutex);
        m_eventCounts = {};
        m_events.clear();
    }

    void RenderFrameConsumerTestProbe::RecordConsumedSceneDatabase(
        const RenderSceneDatabase& scene)
    {
        std::lock_guard lock(m_mutex);
        m_lastConsumedPrimitiveIds.clear();
        m_lastConsumedPrimitiveIds.reserve(scene.GetPrimitives().size());
        for (const auto& [objectId, primitive] : scene.GetPrimitives())
        {
            static_cast<void>(primitive);
            m_lastConsumedPrimitiveIds.push_back(objectId);
        }
        std::sort(m_lastConsumedPrimitiveIds.begin(),
                  m_lastConsumedPrimitiveIds.end());
    }

    std::vector<uint64>
        RenderFrameConsumerTestProbe::GetLastConsumedPrimitiveIds() const
    {
        std::lock_guard lock(m_mutex);
        return m_lastConsumedPrimitiveIds;
    }

    uint32 RenderFrameConsumerTestProbe::GetEventCount(
        RenderRuntimeTestEvent event) const
    {
        std::lock_guard lock(m_mutex);
        return m_eventCounts[static_cast<size_t>(event)];
    }

    bool RenderFrameConsumerTestProbe::WaitForEventCount(
        RenderRuntimeTestEvent event,
        uint32 count,
        std::chrono::milliseconds timeout) const
    {
        std::unique_lock lock(m_mutex);
        return m_cv.wait_for(lock, timeout, [this, event, count]() {
            return m_eventCounts[static_cast<size_t>(event)] >= count;
        });
    }

    void RenderFrameConsumerTestProbe::BlockFrames()
    {
        std::lock_guard lock(m_mutex);
        m_blockFrames = true;
    }

    void RenderFrameConsumerTestProbe::ReleaseFrames()
    {
        {
            std::lock_guard lock(m_mutex);
            m_blockFrames = false;
        }
        m_cv.notify_all();
    }

    void RenderFrameConsumerTestProbe::WaitWhileFrameBlocked()
    {
        std::unique_lock lock(m_mutex);
        m_cv.wait(lock, [this]() { return !m_blockFrames; });
    }

    void RenderFrameConsumerTestProbe::BlockStartup()
    {
        std::lock_guard lock(m_mutex);
        m_blockStartup = true;
    }

    void RenderFrameConsumerTestProbe::ReleaseStartup()
    {
        {
            std::lock_guard lock(m_mutex);
            m_blockStartup = false;
        }
        m_cv.notify_all();
    }

    void RenderFrameConsumerTestProbe::WaitWhileStartupBlocked()
    {
        std::unique_lock lock(m_mutex);
        m_cv.wait(lock, [this]() { return !m_blockStartup; });
    }

    void RenderFrameConsumerTestProbe::SetStartupThread(std::thread::id thread)
    {
        std::lock_guard lock(m_mutex);
        m_startupThread = thread;
    }

    void RenderFrameConsumerTestProbe::SetShutdownThread(std::thread::id thread)
    {
        std::lock_guard lock(m_mutex);
        m_shutdownThread = thread;
    }

    void RenderFrameConsumerTestProbe::SetDestructionThread(
        std::thread::id thread)
    {
        std::lock_guard lock(m_mutex);
        m_destructionThread = thread;
    }

    std::thread::id RenderFrameConsumerTestProbe::GetStartupThread() const
    {
        std::lock_guard lock(m_mutex);
        return m_startupThread;
    }

    std::thread::id RenderFrameConsumerTestProbe::GetShutdownThread() const
    {
        std::lock_guard lock(m_mutex);
        return m_shutdownThread;
    }

    std::thread::id RenderFrameConsumerTestProbe::GetDestructionThread() const
    {
        std::lock_guard lock(m_mutex);
        return m_destructionThread;
    }

    std::unique_ptr<IRenderFrameConsumer>
        CreateRecordingRenderFrameConsumer(
            std::shared_ptr<RenderFrameConsumerTestProbe> probe)
    {
        return std::make_unique<RecordingRenderFrameConsumer>(
            std::move(probe));
    }

    void RenderRuntimeFaultProbe::RecordConstruction()
    {
        {
            std::lock_guard lock(m_mutex);
            m_constructionThread = std::this_thread::get_id();
        }
        m_liveObjectCount.fetch_add(1, std::memory_order_release);
    }

    void RenderRuntimeFaultProbe::RecordDestruction()
    {
        {
            std::lock_guard lock(m_mutex);
            m_destructionThread = std::this_thread::get_id();
        }
        m_liveObjectCount.fetch_sub(1, std::memory_order_release);
    }

    void RenderRuntimeFaultProbe::RecordShutdown(RenderTeardownMode mode)
    {
        m_shutdownMode.store(mode, std::memory_order_release);
    }

    uint32 RenderRuntimeFaultProbe::GetLiveObjectCount() const noexcept
    {
        return m_liveObjectCount.load(std::memory_order_acquire);
    }

    std::thread::id RenderRuntimeFaultProbe::GetConstructionThread() const
    {
        std::lock_guard lock(m_mutex);
        return m_constructionThread;
    }

    std::thread::id RenderRuntimeFaultProbe::GetDestructionThread() const
    {
        std::lock_guard lock(m_mutex);
        return m_destructionThread;
    }

    RenderTeardownMode RenderRuntimeFaultProbe::GetShutdownMode() const noexcept
    {
        return m_shutdownMode.load(std::memory_order_acquire);
    }

    std::unique_ptr<IRenderRuntimeFactory> CreateFaultPlanRuntimeFactory(
        RenderRuntimeFaultPlan plan,
        std::shared_ptr<RenderRuntimeFaultProbe> probe)
    {
        return std::make_unique<FaultPlanRuntimeFactory>(
            plan, std::move(probe));
    }

    FakeRenderMonotonicClock::FakeRenderMonotonicClock()
        : m_ticks(std::chrono::steady_clock::now()
                      .time_since_epoch()
                      .count())
    {
    }

    IRenderMonotonicClock::TimePoint
        FakeRenderMonotonicClock::Now() const noexcept
    {
        return TimePoint(TimePoint::duration(
            m_ticks.load(std::memory_order_acquire)));
    }

    void FakeRenderMonotonicClock::Advance(
        std::chrono::milliseconds duration) noexcept
    {
        m_ticks.fetch_add(
            std::chrono::duration_cast<TimePoint::duration>(duration).count(),
            std::memory_order_acq_rel);
    }
} // namespace RVX
