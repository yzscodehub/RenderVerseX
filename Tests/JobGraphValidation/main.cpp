#include "Core/Job/JobGraph.h"
#include "Core/Job/JobSystem.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

using namespace RVX;

namespace
{
    class ScopedJobSystemShutdown
    {
    public:
        ScopedJobSystemShutdown()
        {
            JobSystem::Get().Shutdown();
        }

        ~ScopedJobSystemShutdown()
        {
            JobSystem::Get().Shutdown();
        }
    };
} // namespace

TEST(JobGraphValidation, JobHandleTracksCategoryAndTimeoutWait)
{
    ScopedJobSystemShutdown shutdown;
    JobSystem::Get().Initialize(1);

    std::promise<void> releasePromise;
    std::shared_future<void> releaseFuture = releasePromise.get_future().share();

    JobSubmissionDesc desc;
    desc.category = "Resource.Load";
    desc.priority = JobPriority::High;

    JobHandle handle = JobSystem::Get().Submit([releaseFuture]() {
        releaseFuture.wait();
    }, desc);

    EXPECT_EQ(handle.GetCategory(), "Resource.Load");
    EXPECT_EQ(handle.GetPriority(), JobPriority::High);
    EXPECT_FALSE(handle.WaitFor(5));

    releasePromise.set_value();
    EXPECT_TRUE(handle.WaitFor(1000));
    EXPECT_TRUE(handle.IsComplete());
}

TEST(JobGraphValidation, MainThreadCompletionRunsOnlyWhenPumped)
{
    ScopedJobSystemShutdown shutdown;
    JobSystem::Get().Initialize(1);

    const std::thread::id pumpThreadId = std::this_thread::get_id();
    std::thread::id callbackThreadId;
    std::atomic<bool> jobRan{false};
    bool callbackCalled = false;

    JobSubmissionDesc desc;
    desc.category = "Resource.Load";
    desc.completionDispatch = JobCompletionDispatch::MainThread;
    desc.continuation = [&]() {
        callbackThreadId = std::this_thread::get_id();
        callbackCalled = true;
    };

    JobHandle handle = JobSystem::Get().Submit([&jobRan]() {
        jobRan.store(true, std::memory_order_release);
    }, desc);

    handle.Wait();
    EXPECT_TRUE(jobRan.load(std::memory_order_acquire));
    EXPECT_FALSE(callbackCalled);
    EXPECT_EQ(JobSystem::Get().GetPendingMainThreadCompletionCount(), 1u);

    EXPECT_EQ(JobSystem::Get().ProcessMainThreadCompletions(), 1u);
    EXPECT_TRUE(callbackCalled);
    EXPECT_EQ(callbackThreadId, pumpThreadId);
    EXPECT_EQ(JobSystem::Get().GetPendingMainThreadCompletionCount(), 0u);
}

TEST(JobGraphValidation, ResultJobQueuesMainThreadContinuationAfterResultReady)
{
    ScopedJobSystemShutdown shutdown;
    JobSystem::Get().Initialize(1);

    bool continuationCalled = false;

    JobSubmissionDesc desc;
    desc.category = "Resource.Load";
    desc.completionDispatch = JobCompletionDispatch::MainThread;
    desc.continuation = [&continuationCalled]() {
        continuationCalled = true;
    };

    std::future<int> future = JobSystem::Get().SubmitWithResult([]() {
        return 42;
    }, desc);

    ASSERT_EQ(future.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_EQ(future.get(), 42);
    EXPECT_FALSE(continuationCalled);

    EXPECT_EQ(JobSystem::Get().ProcessMainThreadCompletions(), 1u);
    EXPECT_TRUE(continuationCalled);
}

TEST(JobGraphValidation, ExecuteRunsDependencyChainWhenJobSystemFallsBackInline)
{
    ScopedJobSystemShutdown shutdown;

    JobGraph graph;
    std::vector<int> order;

    auto load = graph.AddJob("Load", [&order]() { order.push_back(1); });
    auto build = graph.AddJob("Build", [&order]() { order.push_back(2); });
    auto publish = graph.AddJob("Publish", [&order]() { order.push_back(3); });

    build->DependsOn(load);
    publish->DependsOn(build);

    graph.Execute();
    graph.Wait();

    EXPECT_TRUE(graph.IsComplete());
    EXPECT_EQ(3u, graph.GetCompletedJobCount());
    EXPECT_EQ((std::vector<int>{1, 2, 3}), order);
}

TEST(JobGraphValidation, WaitReturnsAfterWorkerJobSignalsCompletion)
{
    ScopedJobSystemShutdown shutdown;
    JobSystem::Get().Initialize(1);

    JobGraph graph;
    std::atomic<bool> finished{false};

    graph.AddJob("Delayed", [&finished]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        finished.store(true);
    });

    graph.Execute();
    graph.Wait();

    EXPECT_TRUE(finished.load());
    EXPECT_TRUE(graph.IsComplete());
}

TEST(JobGraphValidation, WorkerExecutionPreservesDependencyOrder)
{
    ScopedJobSystemShutdown shutdown;
    JobSystem::Get().Initialize(2);

    JobGraph graph;
    std::mutex orderMutex;
    std::vector<int> order;

    auto record = [&orderMutex, &order](int value) {
        std::lock_guard<std::mutex> lock(orderMutex);
        order.push_back(value);
    };

    auto prepare = graph.AddJob("Prepare", [&record]() { record(1); });
    auto upload = graph.AddJob("Upload", [&record]() { record(2); });
    auto publish = graph.AddJob("Publish", [&record]() { record(3); });

    upload->DependsOn(prepare);
    publish->DependsOn(upload);

    graph.Execute();
    graph.Wait();

    EXPECT_EQ((std::vector<int>{1, 2, 3}), order);
    EXPECT_EQ(3u, graph.GetCompletedJobCount());
}

TEST(JobGraphValidation, CompletedGraphCanExecuteAgainWithoutExplicitWait)
{
    ScopedJobSystemShutdown shutdown;

    JobGraph graph;
    int runCount = 0;
    graph.AddJob("Run", [&runCount]() { ++runCount; });

    graph.Execute();
    ASSERT_TRUE(graph.IsComplete());

    graph.Execute();
    graph.Wait();

    EXPECT_EQ(2, runCount);
    EXPECT_TRUE(graph.IsComplete());
}
