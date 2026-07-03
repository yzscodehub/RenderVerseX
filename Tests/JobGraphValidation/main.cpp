#include "Core/Job/JobGraph.h"
#include "Core/Job/JobSystem.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
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
