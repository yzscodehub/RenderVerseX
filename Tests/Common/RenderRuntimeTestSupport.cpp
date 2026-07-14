#include "Common/RenderRuntimeTestSupport.h"

#include "Runtime/RenderThreadGuard.h"

#include <condition_variable>
#include <mutex>

namespace RVX
{
namespace
{
    class InlineRenderExecutor final : public IRenderExecutor,
                                       public NonMovable
    {
    public:
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
    };
} // namespace

    std::unique_ptr<IRenderExecutor> CreateInlineRenderExecutor()
    {
        return std::make_unique<InlineRenderExecutor>();
    }
} // namespace RVX
