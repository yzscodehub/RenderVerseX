#pragma once

/**
 * @file RenderRuntimeTestSupport.h
 * @brief Test-only factories for render runtime conformance tests.
 */

#include "Runtime/IRenderExecutor.h"
#include "Runtime/RenderThreadRuntime.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace RVX
{
    class IDedicatedRenderExecutorBootstrapHook;

    /** @brief Narrow access to private runtime callbacks for validation. */
    struct RenderThreadRuntimeTestAccess
    {
        static void ReportTransportFatal(RenderThreadRuntime& runtime,
                                         const char* message) noexcept;
        static void ForceReleasePublicationInvariantFailure(
            RenderThreadRuntime& runtime) noexcept;
    };

    /** @brief Create the synchronous executor used only by validation tests. */
    std::unique_ptr<IRenderExecutor> CreateInlineRenderExecutor();
    /** @brief Create a deterministic executor-start failure for mapping tests. */
    std::unique_ptr<IRenderExecutor> CreateFailingRenderExecutor(
        uint32 nativeError);

    /** @brief Observable join calls around the real dedicated executor. */
    class RenderExecutorJoinTestProbe final : public NonMovable
    {
    public:
        void Record(RenderExecutorJoinCode code) noexcept;
        [[nodiscard]] uint32 GetCallCount() const noexcept;
        [[nodiscard]] RenderExecutorJoinCode GetLastCode() const noexcept;

    private:
        std::atomic<uint32> m_callCount = 0;
        std::atomic<RenderExecutorJoinCode> m_lastCode =
            RenderExecutorJoinCode::NotStarted;
    };

    /** @brief Decorate the real dedicated executor with join observations. */
    std::unique_ptr<IRenderExecutor> CreateJoinRecordingDedicatedExecutor(
        std::shared_ptr<RenderExecutorJoinTestProbe> probe);
    std::unique_ptr<IRenderExecutor> CreateJoinRecordingDedicatedExecutor(
        std::shared_ptr<RenderExecutorJoinTestProbe> probe,
        std::shared_ptr<IDedicatedRenderExecutorBootstrapHook> bootstrapHook);
    /** @brief Create an inline pump with deterministic JoinUntil outcomes. */
    std::unique_ptr<IRenderExecutor> CreateSequencedJoinInlineRenderExecutor(
        std::vector<RenderExecutorJoinCode> joinCodes,
        std::shared_ptr<RenderExecutorJoinTestProbe> probe);

    /** @brief Latch at the private startup publication boundary. */
    class RenderRuntimeLifecycleTestHook final
        : public IRenderRuntimeLifecycleHook,
          public NonMovable
    {
    public:
        void BeforeStartupAcknowledgement() noexcept override;
        [[nodiscard]] bool WaitUntilEntered(
            std::chrono::milliseconds timeout) const;
        void Release();

    private:
        mutable std::mutex m_mutex;
        mutable std::condition_variable m_cv;
        bool m_entered = false;
        bool m_released = false;
    };

    /** @brief Latch while the Render side owns startup arbitration. */
    class RenderRuntimePublicationTestHook final
        : public IRenderRuntimeLifecycleHook,
          public NonMovable
    {
    public:
        void BeforeStartupAcknowledgement() noexcept override;
        void DuringStartupPublication() noexcept override;
        [[nodiscard]] bool WaitUntilEntered(
            std::chrono::milliseconds timeout) const;
        void Release();

    private:
        mutable std::mutex m_mutex;
        mutable std::condition_variable m_cv;
        bool m_entered = false;
        bool m_released = false;
    };

    struct RenderFatalPolicyIntercept final
    {
    };

    /** @brief Record fatal diagnostics and intercept process termination. */
    class RenderFatalPolicyTestProbe final : public IRenderFatalPolicy,
                                             public NonMovable
    {
    public:
        void Terminate(
            const RenderDiagnosticsSnapshot& diagnostics) override;
        [[nodiscard]] uint32 GetCallCount() const noexcept;
        [[nodiscard]] RenderDiagnosticsSnapshot GetDiagnostics() const;

    private:
        mutable std::mutex m_mutex;
        uint32 m_callCount = 0;
        RenderDiagnosticsSnapshot m_diagnostics{};
    };

    enum class RenderRuntimeTestEvent : uint8
    {
        Started = 0,
        Surface = 1,
        Release = 2,
        Upload = 3,
        Frame = 4,
        Poll = 5,
        Retire = 6,
        Shutdown = 7
    };

    /** @brief Shared observations for the real runtime conformance fixtures. */
    class RenderFrameConsumerTestProbe final : public NonMovable
    {
    public:
        void Record(RenderRuntimeTestEvent event);
        [[nodiscard]] uint32 GetEventCount(
            RenderRuntimeTestEvent event) const;
        [[nodiscard]] bool WaitForEventCount(
            RenderRuntimeTestEvent event,
            uint32 count,
            std::chrono::milliseconds timeout) const;
        [[nodiscard]] std::vector<RenderRuntimeTestEvent> GetEvents() const;
        void ClearEvents();

        void BlockFrames();
        void ReleaseFrames();
        void WaitWhileFrameBlocked();
        void BlockStartup();
        void ReleaseStartup();
        void WaitWhileStartupBlocked();

        void SetStartupThread(std::thread::id thread);
        void SetShutdownThread(std::thread::id thread);
        void SetDestructionThread(std::thread::id thread);
        [[nodiscard]] std::thread::id GetStartupThread() const;
        [[nodiscard]] std::thread::id GetShutdownThread() const;
        [[nodiscard]] std::thread::id GetDestructionThread() const;

        RenderRuntimeCode startupCode = RenderRuntimeCode::Running;
        RHIBackendType startupBackend = RHIBackendType::None;
        uint32 startupNativeError = 0;
        std::string startupMessage;
        RenderRuntimeCode frameCode = RenderRuntimeCode::Running;
        std::string frameMessage;
        RenderShutdownCode shutdownCode = RenderShutdownCode::Completed;
        RHIBackendType shutdownBackend = RHIBackendType::None;
        uint32 shutdownNativeError = 0;
        std::string shutdownMessage;
        bool throwOnFrame = false;

    private:
        static constexpr size_t EVENT_COUNT = 8;
        mutable std::mutex m_mutex;
        mutable std::condition_variable m_cv;
        std::array<uint32, EVENT_COUNT> m_eventCounts{};
        std::vector<RenderRuntimeTestEvent> m_events;
        bool m_blockFrames = false;
        bool m_blockStartup = false;
        std::thread::id m_startupThread{};
        std::thread::id m_shutdownThread{};
        std::thread::id m_destructionThread{};
    };

    /** @brief Create a value-only consumer that records real pump behavior. */
    std::unique_ptr<IRenderFrameConsumer>
        CreateRecordingRenderFrameConsumer(
            std::shared_ptr<RenderFrameConsumerTestProbe> probe);

    enum class RenderRuntimeFaultPoint : uint8
    {
        None = 0,
        FactoryCreation,
        DeviceCreation,
        SurfaceCreation,
        ContextCreation,
        RendererCreation,
        ResourceCreation,
        UploadSubmission,
        FencePoll,
        FenceWait,
        Resize,
        Present,
        FrameException,
        DeviceLossBeforeSubmission,
        DeviceLossInFlight,
        DeviceLossDuringShutdown,
    };

    struct RenderRuntimeFaultPlan
    {
        RenderRuntimeFaultPoint point = RenderRuntimeFaultPoint::None;
        uint32 occurrence = 1;
        uint32 nativeError = 0xD15EA5EDU;
    };

    /** @brief Lifetime and ownership observations for fault-matrix fixtures. */
    class RenderRuntimeFaultProbe final : public NonMovable
    {
    public:
        void RecordConstruction();
        void RecordDestruction();
        void RecordShutdown(RenderTeardownMode mode);
        [[nodiscard]] uint32 GetLiveObjectCount() const noexcept;
        [[nodiscard]] std::thread::id GetConstructionThread() const;
        [[nodiscard]] std::thread::id GetDestructionThread() const;
        [[nodiscard]] RenderTeardownMode GetShutdownMode() const noexcept;

    private:
        mutable std::mutex m_mutex;
        std::atomic<uint32> m_liveObjectCount = 0;
        std::atomic<RenderTeardownMode> m_shutdownMode =
            RenderTeardownMode::None;
        std::thread::id m_constructionThread{};
        std::thread::id m_destructionThread{};
    };

    /** @brief Create a fault-injecting consumer on the executor owner thread. */
    std::unique_ptr<IRenderRuntimeFactory> CreateFaultPlanRuntimeFactory(
        RenderRuntimeFaultPlan plan,
        std::shared_ptr<RenderRuntimeFaultProbe> probe);

    /** @brief Manually advanced monotonic source used by watchdog tests. */
    class FakeRenderMonotonicClock final : public IRenderMonotonicClock,
                                           public NonMovable
    {
    public:
        FakeRenderMonotonicClock();
        [[nodiscard]] TimePoint Now() const noexcept override;
        void Advance(std::chrono::milliseconds duration) noexcept;

    private:
        std::atomic<TimePoint::duration::rep> m_ticks = 0;
    };
} // namespace RVX
