#include "Runtime/DedicatedRenderExecutor.h"

#include "Core/Assert.h"
#include "Runtime/RenderThreadPlatform.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <system_error>
#include <thread>

namespace RVX
{
namespace
{
    class DedicatedRenderExecutor final : public IRenderExecutor,
                                          public NonMovable
    {
    public:
        DedicatedRenderExecutor() = default;
        explicit DedicatedRenderExecutor(
            std::shared_ptr<IDedicatedRenderExecutorBootstrapHook>
                bootstrapHook)
            : m_bootstrapHook(std::move(bootstrapHook))
        {
        }

        ~DedicatedRenderExecutor() override
        {
            if (!m_thread.joinable())
            {
                return;
            }

            bool exited = false;
            {
                std::lock_guard lock(m_stateMutex);
                exited = m_exited;
            }
            RVX_ASSERT_MSG(exited,
                           "Dedicated render executor destroyed before stop");
            m_thread.join();
        }

        RenderExecutorStartResult Start(IRenderExecutorPump& pump) override
        {
            std::unique_lock lock(m_stateMutex);
            if (m_started)
            {
                return RenderExecutorStartResult{
                    RenderExecutorStartCode::AlreadyStarted,
                    0};
            }

            m_pump.store(&pump, std::memory_order_release);
            m_exited = false;
            m_joined = false;
            m_workerEnteredBootstrapBoundary = false;
            try
            {
                m_thread = std::thread(&DedicatedRenderExecutor::ThreadMain,
                                       this);
            }
            catch (const std::system_error& error)
            {
                m_pump.store(nullptr, std::memory_order_release);
                return RenderExecutorStartResult{
                    RenderExecutorStartCode::ThreadCreationFailed,
                    static_cast<uint32>(error.code().value())};
            }

            m_started = true;
            // This handshake confirms only that the worker was scheduled and
            // entered the platform-bootstrap boundary. Bootstrap hooks,
            // platform setup, and consumer initialization remain asynchronous
            // so RenderThreadRuntime's startup watchdog covers all of them.
            m_workerEntryCv.wait(lock, [this]() {
                return m_workerEnteredBootstrapBoundary;
            });
            return RenderExecutorStartResult{
                RenderExecutorStartCode::Started,
                0};
        }

        void NotifyWork() noexcept override
        {
            IRenderExecutorPump* pump =
                m_pump.load(std::memory_order_acquire);
            if (pump != nullptr)
            {
                pump->Wake();
            }
        }

        RenderExecutorJoinResult JoinUntil(
            std::chrono::steady_clock::time_point deadline) override
        {
            std::unique_lock lock(m_stateMutex);
            if (!m_started)
            {
                return RenderExecutorJoinResult{
                    RenderExecutorJoinCode::NotStarted};
            }
            if (m_joined)
            {
                return RenderExecutorJoinResult{
                    RenderExecutorJoinCode::Joined};
            }
            if (!m_exitCv.wait_until(lock,
                                     deadline,
                                     [this]() { return m_exited; }))
            {
                return RenderExecutorJoinResult{
                    RenderExecutorJoinCode::TimedOut};
            }

            lock.unlock();
            m_thread.join();
            lock.lock();
            m_joined = true;
            m_pump.store(nullptr, std::memory_order_release);
            return RenderExecutorJoinResult{RenderExecutorJoinCode::Joined};
        }

    private:
        void ThreadMain() noexcept
        {
            IRenderExecutorPump* pump =
                m_pump.load(std::memory_order_acquire);
            {
                std::lock_guard lock(m_stateMutex);
                m_workerEnteredBootstrapBoundary = true;
            }
            m_workerEntryCv.notify_all();
            try
            {
                if (m_bootstrapHook != nullptr)
                {
                    m_bootstrapHook->OnWorkerEntry();
                    m_bootstrapHook->BeforePlatformBootstrap();
                }
                RenderThreadPlatformBootstrap platformBootstrap;

                while (true)
                {
                    RenderPumpDecision decision = RenderPumpDecision::Idle;
                    {
                        RenderThreadPlatformIterationScope iterationScope;
                        decision = pump->PumpOnce();
                    }

                    if (decision == RenderPumpDecision::Stop)
                    {
                        break;
                    }
                    if (decision == RenderPumpDecision::Idle)
                    {
                        pump->WaitForWork();
                    }
                }
            }
            catch (...)
            {
                pump->OnUnhandledExecutorException();
            }

            {
                std::lock_guard lock(m_stateMutex);
                m_exited = true;
            }
            m_exitCv.notify_all();
        }

        std::atomic<IRenderExecutorPump*> m_pump = nullptr;
        std::shared_ptr<IDedicatedRenderExecutorBootstrapHook>
            m_bootstrapHook;
        std::thread m_thread;
        std::mutex m_stateMutex;
        std::condition_variable m_workerEntryCv;
        std::condition_variable m_exitCv;
        bool m_started = false;
        bool m_workerEnteredBootstrapBoundary = false;
        bool m_exited = false;
        bool m_joined = false;
    };
} // namespace

    std::unique_ptr<IRenderExecutor> CreateDedicatedRenderExecutor()
    {
        return std::make_unique<DedicatedRenderExecutor>();
    }

    std::unique_ptr<IRenderExecutor> CreateDedicatedRenderExecutor(
        std::shared_ptr<IDedicatedRenderExecutorBootstrapHook> bootstrapHook)
    {
        return std::make_unique<DedicatedRenderExecutor>(
            std::move(bootstrapHook));
    }
} // namespace RVX
