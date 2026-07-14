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
            m_bootstrapEntered = false;
            m_exited = false;
            m_joined = false;
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
            m_bootstrapCv.wait(lock, [this]() { return m_bootstrapEntered; });
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
            try
            {
                RenderThreadPlatformBootstrap platformBootstrap;
                {
                    std::lock_guard lock(m_stateMutex);
                    m_bootstrapEntered = true;
                }
                m_bootstrapCv.notify_all();

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
        std::thread m_thread;
        std::mutex m_stateMutex;
        std::condition_variable m_bootstrapCv;
        std::condition_variable m_exitCv;
        bool m_started = false;
        bool m_bootstrapEntered = false;
        bool m_exited = false;
        bool m_joined = false;
    };
} // namespace

    std::unique_ptr<IRenderExecutor> CreateDedicatedRenderExecutor()
    {
        return std::make_unique<DedicatedRenderExecutor>();
    }
} // namespace RVX
