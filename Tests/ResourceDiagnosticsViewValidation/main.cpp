#include "Core/Log.h"
#include "Resource/ResourceDiagnosticsView.h"
#include "Resource/ResourceSubsystem.h"

#include <gtest/gtest.h>

#include <thread>

namespace
{
    using namespace RVX;
    using namespace RVX::Resource;

    class LogEnvironment final : public ::testing::Environment
    {
    public:
        void SetUp() override { Log::Initialize(); }
        void TearDown() override { Log::Shutdown(); }
    };

    [[maybe_unused]] ::testing::Environment* const g_logEnvironment =
        ::testing::AddGlobalTestEnvironment(new LogEnvironment());

    TEST(ResourceDiagnosticsViewValidation,
         ReturnsUnavailableOutsideTheInitializedLifetime)
    {
        ResourceSubsystem subsystem;
        const IResourceDiagnosticsView& view = subsystem;

        const ResourceDiagnosticsQueryResult beforeInitialize =
            view.QueryResourceDiagnostics();
        EXPECT_EQ(beforeInitialize.code,
                  ResourceDiagnosticsQueryCode::NotInitialized);
        EXPECT_FALSE(beforeInitialize.IsAvailable());
        EXPECT_FALSE(beforeInitialize.snapshot.IsAvailable());
        EXPECT_FALSE(beforeInitialize.snapshot.GetReason().empty());

        subsystem.Initialize();
        ASSERT_TRUE(view.QueryResourceDiagnostics().IsAvailable());
        subsystem.Deinitialize();

        const ResourceDiagnosticsQueryResult afterDeinitialize =
            view.QueryResourceDiagnostics();
        EXPECT_EQ(afterDeinitialize.code,
                  ResourceDiagnosticsQueryCode::NotInitialized);
        EXPECT_FALSE(afterDeinitialize.IsAvailable());
        EXPECT_FALSE(afterDeinitialize.snapshot.GetReason().empty());
    }

    TEST(ResourceDiagnosticsViewValidation,
         ReturnsACompleteValueSnapshotOnTheUpdateOwnerThread)
    {
        ResourceSubsystem subsystem;
        subsystem.Initialize();
        const IResourceDiagnosticsView& view = subsystem;

        const ResourceDiagnosticsQueryResult result =
            view.QueryResourceDiagnostics();
        ASSERT_EQ(result.code, ResourceDiagnosticsQueryCode::Available);
        ASSERT_TRUE(result.IsAvailable());
        ASSERT_TRUE(result.snapshot.GetValue().has_value());

        const ResourceDiagnosticsSnapshot snapshot =
            *result.snapshot.GetValue();
        EXPECT_GE(snapshot.decodePeakReservedBytes,
                  snapshot.decodeReservedBytes);
        EXPECT_FALSE(snapshot.sourceReadOperationCount.IsAvailable());
        EXPECT_FALSE(snapshot.sourceReadOperationCount.GetReason().empty());
        EXPECT_FALSE(snapshot.sourceReadBytes.IsAvailable());
        EXPECT_FALSE(snapshot.sourceReadBytes.GetReason().empty());

        subsystem.Deinitialize();

        // The returned snapshot is value-semantic and remains readable after
        // the subsystem has released its mutable ResourceManager state.
        EXPECT_GE(snapshot.decodeBudgetBytes, snapshot.decodeReservedBytes);
    }

    TEST(ResourceDiagnosticsViewValidation,
         RejectsWrongThreadBeforeReadingUpdateOwnedState)
    {
        ResourceSubsystem subsystem;
        subsystem.Initialize();
        const IResourceDiagnosticsView& view = subsystem;

        ResourceDiagnosticsQueryResult workerResult;
        std::thread worker(
            [&view, &workerResult]
            {
                workerResult = view.QueryResourceDiagnostics();
            });
        worker.join();

        EXPECT_EQ(workerResult.code, ResourceDiagnosticsQueryCode::WrongThread);
        EXPECT_FALSE(workerResult.IsAvailable());
        EXPECT_FALSE(workerResult.snapshot.IsAvailable());
        EXPECT_FALSE(workerResult.snapshot.GetReason().empty());

        const ResourceDiagnosticsQueryResult ownerResult =
            view.QueryResourceDiagnostics();
        EXPECT_EQ(ownerResult.code, ResourceDiagnosticsQueryCode::Available);
        EXPECT_TRUE(ownerResult.IsAvailable());

        subsystem.Deinitialize();
    }
} // namespace
