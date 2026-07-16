#include "Common/RenderRuntimeTestSupport.h"
#include "Runtime/DedicatedRenderExecutor.h"
#include "Runtime/IRenderExecutor.h"
#include "Runtime/RenderThreadGuard.h"

#include <gtest/gtest.h>

#if defined(_WIN32)
    #include <Windows.h>
#else
    #include <pthread.h>
    #include <sched.h>
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

namespace RVX
{
namespace
{
    static_assert(std::is_same_v<
                  std::underlying_type_t<RenderPumpDecision>, uint8>);
    static_assert(static_cast<uint8>(RenderPumpDecision::Progressed) == 0);
    static_assert(static_cast<uint8>(RenderPumpDecision::Idle) == 1);
    static_assert(static_cast<uint8>(RenderPumpDecision::Stop) == 2);

    static_assert(std::is_same_v<
                  std::underlying_type_t<RenderExecutorStartCode>, uint8>);
    static_assert(static_cast<uint8>(RenderExecutorStartCode::Started) == 0);
    static_assert(
        static_cast<uint8>(RenderExecutorStartCode::AlreadyStarted) == 1);
    static_assert(
        static_cast<uint8>(RenderExecutorStartCode::ThreadCreationFailed) == 2);

    static_assert(std::is_same_v<
                  std::underlying_type_t<RenderExecutorJoinCode>, uint8>);
    static_assert(static_cast<uint8>(RenderExecutorJoinCode::Joined) == 0);
    static_assert(static_cast<uint8>(RenderExecutorJoinCode::NotStarted) == 1);
    static_assert(static_cast<uint8>(RenderExecutorJoinCode::TimedOut) == 2);

    static_assert(std::is_same_v<
                  std::underlying_type_t<RenderThreadGuardCode>, uint8>);
    static_assert(static_cast<uint8>(RenderThreadGuardCode::Owner) == 0);
    static_assert(static_cast<uint8>(RenderThreadGuardCode::Unbound) == 1);
    static_assert(static_cast<uint8>(RenderThreadGuardCode::WrongThread) == 2);

    static_assert(noexcept(
        std::declval<IRenderExecutorPump&>().PumpOnce()));
    static_assert(noexcept(
        std::declval<IRenderExecutorPump&>().WaitForWork()));
    static_assert(noexcept(std::declval<IRenderExecutorPump&>().Wake()));
    static_assert(noexcept(std::declval<IRenderExecutorPump&>()
                               .OnUnhandledExecutorException()));
    static_assert(noexcept(std::declval<IRenderExecutor&>().NotifyWork()));

    constexpr auto RVX_TEST_TIMEOUT = std::chrono::seconds(2);

    class BlockingBootstrapHook final
        : public IDedicatedRenderExecutorBootstrapHook,
          public NonMovable
    {
    public:
        void BeforePlatformBootstrap() noexcept override
        {
            std::unique_lock lock(m_mutex);
            m_entered = true;
            m_cv.notify_all();
            m_cv.wait(lock, [this]() { return m_released; });
        }

        [[nodiscard]] bool WaitUntilEntered(
            std::chrono::milliseconds timeout)
        {
            std::unique_lock lock(m_mutex);
            return m_cv.wait_for(lock,
                                 timeout,
                                 [this]() { return m_entered; });
        }

        void Release()
        {
            {
                std::lock_guard lock(m_mutex);
                m_released = true;
            }
            m_cv.notify_all();
        }

    private:
        std::mutex m_mutex;
        std::condition_variable m_cv;
        bool m_entered = false;
        bool m_released = false;
    };

    struct PlatformObservation
    {
        std::array<char, 64> name{};
        int schedulingValue = 0;
    };

    PlatformObservation ObserveCurrentPlatformThread() noexcept
    {
        PlatformObservation observation;
#if defined(_WIN32)
        PWSTR wideName = nullptr;
        if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &wideName)) &&
            wideName != nullptr)
        {
            WideCharToMultiByte(CP_UTF8,
                                0,
                                wideName,
                                -1,
                                observation.name.data(),
                                static_cast<int>(observation.name.size()),
                                nullptr,
                                nullptr);
            LocalFree(wideName);
        }
        observation.schedulingValue = GetThreadPriority(GetCurrentThread());
#else
        pthread_getname_np(pthread_self(),
                           observation.name.data(),
                           observation.name.size());
        int policy = 0;
        sched_param parameters{};
        if (pthread_getschedparam(pthread_self(), &policy, &parameters) == 0)
        {
            observation.schedulingValue = policy;
        }
#endif
        return observation;
    }

    class ScriptedPump final : public IRenderExecutorPump
    {
    public:
        ScriptedPump() = default;

        ScriptedPump(std::initializer_list<RenderPumpDecision> decisions)
            : m_decisions(decisions)
        {
        }

        RenderPumpDecision PumpOnce() noexcept override
        {
            if (m_guard.BindCurrentThread() != RenderThreadGuardCode::Owner)
            {
                m_guardViolationCount.fetch_add(1,
                                                std::memory_order_relaxed);
            }

            if (!m_threadProbe.has_value())
            {
                m_threadProbe.emplace(*this);
            }

            RenderPumpDecision decision = RenderPumpDecision::Idle;
            {
                std::lock_guard lock(m_decisionMutex);
                if (!m_decisions.empty())
                {
                    decision = m_decisions.front();
                    m_decisions.pop_front();
                }
            }

            if (decision == RenderPumpDecision::Stop)
            {
                m_threadProbe.reset();
            }

            {
                std::lock_guard lock(m_observationMutex);
                if (m_pumpCount == 0U)
                {
                    m_pumpThread = std::this_thread::get_id();
                    m_platform = ObserveCurrentPlatformThread();
                }
                if (m_pumpCount < m_returnedDecisions.size())
                {
                    m_returnedDecisions[m_pumpCount] = decision;
                }
                ++m_pumpCount;
            }
            m_observationCv.notify_all();
            return decision;
        }

        void WaitForWork() noexcept override
        {
            std::unique_lock lock(m_wakeMutex);
            ++m_waitCount;
            m_wakeCv.notify_all();
            m_wakeCv.wait(lock, [this]() {
                return m_consumedWakeCount < m_publishedWakeCount;
            });
            m_consumedWakeCount = m_publishedWakeCount;
        }

        void Wake() noexcept override
        {
            {
                std::lock_guard lock(m_wakeMutex);
                ++m_publishedWakeCount;
            }
            m_wakeCv.notify_all();
        }

        void OnUnhandledExecutorException() noexcept override
        {
            m_unhandledExceptionCount.fetch_add(1,
                                                std::memory_order_relaxed);
        }

        void PushDecision(RenderPumpDecision decision)
        {
            std::lock_guard lock(m_decisionMutex);
            m_decisions.push_back(decision);
        }

        bool WaitForPumpCount(uint32 count)
        {
            std::unique_lock lock(m_observationMutex);
            return m_observationCv.wait_for(lock, RVX_TEST_TIMEOUT, [this, count]() {
                return m_pumpCount >= count;
            });
        }

        bool WaitForWaitCount(uint32 count)
        {
            std::unique_lock lock(m_wakeMutex);
            return m_wakeCv.wait_for(lock, RVX_TEST_TIMEOUT, [this, count]() {
                return m_waitCount >= count;
            });
        }

        [[nodiscard]] uint32 GetPumpCount() const
        {
            std::lock_guard lock(m_observationMutex);
            return m_pumpCount;
        }

        [[nodiscard]] uint32 GetWaitCount() const
        {
            std::lock_guard lock(m_wakeMutex);
            return m_waitCount;
        }

        [[nodiscard]] RenderPumpDecision GetReturnedDecision(
            uint32 index) const
        {
            std::lock_guard lock(m_observationMutex);
            return m_returnedDecisions[index];
        }

        [[nodiscard]] std::thread::id GetPumpThread() const
        {
            std::lock_guard lock(m_observationMutex);
            return m_pumpThread;
        }

        [[nodiscard]] std::thread::id GetProbeConstructionThread() const
        {
            std::lock_guard lock(m_observationMutex);
            return m_probeConstructionThread;
        }

        [[nodiscard]] std::thread::id GetProbeDestructionThread() const
        {
            std::lock_guard lock(m_observationMutex);
            return m_probeDestructionThread;
        }

        [[nodiscard]] std::string GetThreadName() const
        {
            std::lock_guard lock(m_observationMutex);
            return m_platform.name.data();
        }

        [[nodiscard]] int GetSchedulingValue() const
        {
            std::lock_guard lock(m_observationMutex);
            return m_platform.schedulingValue;
        }

        [[nodiscard]] uint32 GetGuardViolationCount() const noexcept
        {
            return m_guardViolationCount.load(std::memory_order_relaxed);
        }

        [[nodiscard]] uint32 GetUnhandledExceptionCount() const noexcept
        {
            return m_unhandledExceptionCount.load(std::memory_order_relaxed);
        }

    private:
        class ThreadProbe final
        {
        public:
            explicit ThreadProbe(ScriptedPump& owner) noexcept
                : m_owner(owner)
            {
                std::lock_guard lock(m_owner.m_observationMutex);
                m_owner.m_probeConstructionThread =
                    std::this_thread::get_id();
            }

            ~ThreadProbe()
            {
                std::lock_guard lock(m_owner.m_observationMutex);
                m_owner.m_probeDestructionThread =
                    std::this_thread::get_id();
            }

        private:
            ScriptedPump& m_owner;
        };

        RenderThreadGuard m_guard;
        mutable std::mutex m_decisionMutex;
        std::deque<RenderPumpDecision> m_decisions;

        mutable std::mutex m_observationMutex;
        std::condition_variable m_observationCv;
        std::array<RenderPumpDecision, 16> m_returnedDecisions{};
        uint32 m_pumpCount = 0;
        std::thread::id m_pumpThread{};
        std::thread::id m_probeConstructionThread{};
        std::thread::id m_probeDestructionThread{};
        PlatformObservation m_platform{};
        std::optional<ThreadProbe> m_threadProbe;

        mutable std::mutex m_wakeMutex;
        std::condition_variable m_wakeCv;
        uint64 m_publishedWakeCount = 0;
        uint64 m_consumedWakeCount = 0;
        uint32 m_waitCount = 0;

        std::atomic<uint32> m_guardViolationCount = 0;
        std::atomic<uint32> m_unhandledExceptionCount = 0;
    };

    struct ExecutorCase
    {
        const char* name = nullptr;
        std::unique_ptr<IRenderExecutor> (*create)() = nullptr;
        bool dedicated = false;
    };

    void PrintTo(const ExecutorCase& value, std::ostream* stream)
    {
        *stream << value.name;
    }

    void StopExecutorAndJoin(IRenderExecutor& executor, ScriptedPump& pump)
    {
        const uint32 expectedPumpCount = pump.GetPumpCount() + 1U;
        pump.PushDecision(RenderPumpDecision::Stop);
        executor.NotifyWork();
        ASSERT_TRUE(pump.WaitForPumpCount(expectedPumpCount));

        const RenderExecutorJoinResult result = executor.JoinUntil(
            std::chrono::steady_clock::now() + RVX_TEST_TIMEOUT);
        EXPECT_EQ(result.code, RenderExecutorJoinCode::Joined);
    }

    class RenderExecutorConformanceTest
        : public testing::TestWithParam<ExecutorCase>
    {
    protected:
        void DriveInitialWork(IRenderExecutor& executor) const
        {
            if (!GetParam().dedicated)
            {
                executor.NotifyWork();
            }
        }

    };

    TEST(RenderExecutorValidation, DefaultsAndStableValuesMatchThePlan)
    {
        const RenderExecutorStartResult startResult;
        EXPECT_EQ(startResult.code,
                  RenderExecutorStartCode::ThreadCreationFailed);
        EXPECT_EQ(startResult.nativeError, 0U);

        const RenderExecutorJoinResult joinResult;
        EXPECT_EQ(joinResult.code, RenderExecutorJoinCode::NotStarted);
    }

    TEST(RenderExecutorValidation, GuardRecordsOneOwnerAndReportsOtherThreads)
    {
        RenderThreadGuard guard;
        EXPECT_EQ(guard.QueryCurrentThread(), RenderThreadGuardCode::Unbound);
        EXPECT_EQ(guard.BindCurrentThread(), RenderThreadGuardCode::Owner);
        EXPECT_EQ(guard.ValidateCurrentThread(), RenderThreadGuardCode::Owner);
        EXPECT_EQ(guard.GetOwnerThreadId(), std::this_thread::get_id());

        std::atomic<RenderThreadGuardCode> otherThreadCode =
            RenderThreadGuardCode::Unbound;
        std::thread otherThread([&guard, &otherThreadCode]() {
            otherThreadCode.store(guard.QueryCurrentThread(),
                                  std::memory_order_release);
        });
        otherThread.join();

        EXPECT_EQ(otherThreadCode.load(std::memory_order_acquire),
                  RenderThreadGuardCode::WrongThread);
        EXPECT_EQ(guard.GetOwnerThreadId(), std::this_thread::get_id());
    }

    TEST_P(RenderExecutorConformanceTest,
           RunsTheSameProgressIdleStopSequenceUnderOneThreadOwner)
    {
        const std::thread::id updateThread = std::this_thread::get_id();
        std::unique_ptr<IRenderExecutor> executor = GetParam().create();
        ASSERT_NE(executor, nullptr);
        ScriptedPump pump{
            RenderPumpDecision::Progressed,
            RenderPumpDecision::Idle};

        const RenderExecutorStartResult start = executor->Start(pump);
        ASSERT_EQ(start.code, RenderExecutorStartCode::Started);
        EXPECT_EQ(start.nativeError, 0U);
        DriveInitialWork(*executor);
        ASSERT_TRUE(pump.WaitForPumpCount(2U));
        if (GetParam().dedicated)
        {
            ASSERT_TRUE(pump.WaitForWaitCount(1U));
        }

        EXPECT_EQ(pump.GetReturnedDecision(0U),
                  RenderPumpDecision::Progressed);
        EXPECT_EQ(pump.GetReturnedDecision(1U), RenderPumpDecision::Idle);
        EXPECT_EQ(pump.GetGuardViolationCount(), 0U);
        EXPECT_EQ(pump.GetUnhandledExceptionCount(), 0U);

        StopExecutorAndJoin(*executor, pump);
        EXPECT_EQ(pump.GetReturnedDecision(2U), RenderPumpDecision::Stop);
        EXPECT_EQ(pump.GetProbeConstructionThread(), pump.GetPumpThread());
        EXPECT_EQ(pump.GetProbeDestructionThread(), pump.GetPumpThread());

        if (GetParam().dedicated)
        {
            EXPECT_NE(pump.GetPumpThread(), updateThread);
        }
        else
        {
            EXPECT_EQ(pump.GetPumpThread(), updateThread);
            EXPECT_EQ(pump.GetWaitCount(), 0U);
        }
    }

    TEST_P(RenderExecutorConformanceTest,
           IdleDoesNotRepeatAndNotifyWakesProgress)
    {
        std::unique_ptr<IRenderExecutor> executor = GetParam().create();
        ScriptedPump pump{RenderPumpDecision::Idle};
        ASSERT_EQ(executor->Start(pump).code, RenderExecutorStartCode::Started);
        DriveInitialWork(*executor);
        ASSERT_TRUE(pump.WaitForPumpCount(1U));
        if (GetParam().dedicated)
        {
            ASSERT_TRUE(pump.WaitForWaitCount(1U));
        }

        EXPECT_EQ(pump.GetPumpCount(), 1U);
        pump.PushDecision(RenderPumpDecision::Progressed);
        pump.PushDecision(RenderPumpDecision::Idle);
        executor->NotifyWork();
        ASSERT_TRUE(pump.WaitForPumpCount(3U));
        if (GetParam().dedicated)
        {
            ASSERT_TRUE(pump.WaitForWaitCount(2U));
        }
        EXPECT_EQ(pump.GetPumpCount(), 3U);

        StopExecutorAndJoin(*executor, pump);
    }

    TEST_P(RenderExecutorConformanceTest,
           StartAndJoinEdgeCasesDoNotReplaceTheOriginalPump)
    {
        std::unique_ptr<IRenderExecutor> executor = GetParam().create();
        ScriptedPump firstPump{RenderPumpDecision::Idle};
        ScriptedPump replacementPump{RenderPumpDecision::Stop};

        EXPECT_EQ(executor->JoinUntil(std::chrono::steady_clock::now()).code,
                  RenderExecutorJoinCode::NotStarted);
        ASSERT_EQ(executor->Start(firstPump).code,
                  RenderExecutorStartCode::Started);
        DriveInitialWork(*executor);
        ASSERT_TRUE(firstPump.WaitForPumpCount(1U));

        const RenderExecutorStartResult repeatedStart =
            executor->Start(replacementPump);
        EXPECT_EQ(repeatedStart.code,
                  RenderExecutorStartCode::AlreadyStarted);
        EXPECT_EQ(repeatedStart.nativeError, 0U);
        EXPECT_EQ(replacementPump.GetPumpCount(), 0U);

        EXPECT_EQ(executor->JoinUntil(std::chrono::steady_clock::now()).code,
                  RenderExecutorJoinCode::TimedOut);
        EXPECT_EQ(replacementPump.GetPumpCount(), 0U);
        StopExecutorAndJoin(*executor, firstPump);
    }

    TEST_P(RenderExecutorConformanceTest,
           TimedOutIsNotReportedBeforeTheRequestedDeadline)
    {
        std::unique_ptr<IRenderExecutor> executor = GetParam().create();
        ScriptedPump pump{RenderPumpDecision::Idle};
        ASSERT_EQ(executor->Start(pump).code,
                  RenderExecutorStartCode::Started);
        DriveInitialWork(*executor);
        ASSERT_TRUE(pump.WaitForPumpCount(1U));

        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(25);
        const RenderExecutorJoinResult result = executor->JoinUntil(deadline);
        EXPECT_EQ(result.code, RenderExecutorJoinCode::TimedOut);
        EXPECT_GE(std::chrono::steady_clock::now(), deadline);

        StopExecutorAndJoin(*executor, pump);
    }

    TEST(RenderExecutorValidation,
         UsesASeparateNamedNonRealtimeThreadAndJoinsAfterStop)
    {
        const std::thread::id updateThread = std::this_thread::get_id();
        std::unique_ptr<IRenderExecutor> executor =
            CreateDedicatedRenderExecutor();
        ScriptedPump pump{RenderPumpDecision::Idle};

        ASSERT_EQ(executor->Start(pump).code,
                  RenderExecutorStartCode::Started);
        ASSERT_TRUE(pump.WaitForPumpCount(1U));
        ASSERT_TRUE(pump.WaitForWaitCount(1U));
        EXPECT_NE(pump.GetPumpThread(), updateThread);
        EXPECT_EQ(pump.GetThreadName(), "RVX Render");
#if defined(_WIN32)
        EXPECT_EQ(pump.GetSchedulingValue(), THREAD_PRIORITY_BELOW_NORMAL);
#else
        EXPECT_EQ(pump.GetSchedulingValue(), SCHED_OTHER);
#endif

        StopExecutorAndJoin(*executor, pump);
        EXPECT_EQ(pump.GetProbeDestructionThread(), pump.GetPumpThread());
    }

    TEST(RenderExecutorValidation,
         DedicatedStartOnlyAcknowledgesThreadCreation)
    {
        auto bootstrapHook = std::make_shared<BlockingBootstrapHook>();
        std::unique_ptr<IRenderExecutor> executor =
            CreateDedicatedRenderExecutor(bootstrapHook);
        ScriptedPump pump{RenderPumpDecision::Idle};

        std::mutex startMutex;
        std::condition_variable startCv;
        bool startReturned = false;
        RenderExecutorStartResult startResult;
        std::thread starter([&]() {
            startResult = executor->Start(pump);
            {
                std::lock_guard lock(startMutex);
                startReturned = true;
            }
            startCv.notify_all();
        });

        ASSERT_TRUE(bootstrapHook->WaitUntilEntered(RVX_TEST_TIMEOUT));
        {
            std::unique_lock lock(startMutex);
            EXPECT_TRUE(startCv.wait_for(lock,
                                         std::chrono::milliseconds(100),
                                         [&]() { return startReturned; }));
        }

        bootstrapHook->Release();
        starter.join();
        ASSERT_EQ(startResult.code, RenderExecutorStartCode::Started);
        ASSERT_TRUE(pump.WaitForPumpCount(1U));
        ASSERT_TRUE(pump.WaitForWaitCount(1U));
        StopExecutorAndJoin(*executor, pump);
    }

    TEST(RenderExecutorValidation,
         RunsSynchronouslyAndNeverCallsTheBlockingWaitHook)
    {
        const std::thread::id updateThread = std::this_thread::get_id();
        std::unique_ptr<IRenderExecutor> executor =
            CreateInlineRenderExecutor();
        ScriptedPump pump{
            RenderPumpDecision::Progressed,
            RenderPumpDecision::Idle};

        ASSERT_EQ(executor->Start(pump).code,
                  RenderExecutorStartCode::Started);
        EXPECT_EQ(pump.GetPumpCount(), 0U);
        executor->NotifyWork();
        EXPECT_EQ(pump.GetPumpCount(), 2U);
        EXPECT_EQ(pump.GetPumpThread(), updateThread);
        EXPECT_EQ(pump.GetWaitCount(), 0U);
        StopExecutorAndJoin(*executor, pump);
    }

    INSTANTIATE_TEST_SUITE_P(
        RenderExecutorValidationShared,
        RenderExecutorConformanceTest,
        testing::Values(
            ExecutorCase{"Inline", &CreateInlineRenderExecutor, false},
            ExecutorCase{"Dedicated", &CreateDedicatedRenderExecutor, true}),
        [](const testing::TestParamInfo<ExecutorCase>& info) {
            return info.param.name;
        });
} // namespace
} // namespace RVX
