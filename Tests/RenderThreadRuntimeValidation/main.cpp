#include "Common/RenderRuntimeTestSupport.h"
#include "Render/RenderDiagnostics.h"
#include "Render/RenderRuntimeTypes.h"
#include "Render/RenderSubsystem.h"
#include "RenderExtraction/RenderFramePacketBuilder.h"
#include "Runtime/DedicatedRenderExecutor.h"
#include "Runtime/RenderDiagnosticsPublisher.h"
#include "Runtime/RenderThreadRuntime.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

namespace RVX
{
namespace
{
    using namespace std::chrono_literals;

    static_assert(std::is_same_v<std::underlying_type_t<RenderExecutorKind>, uint8>);
    static_assert(static_cast<uint8>(RenderExecutorKind::None) == 0);
    static_assert(static_cast<uint8>(RenderExecutorKind::Dedicated) == 1);
    static_assert(static_cast<uint8>(RenderExecutorKind::InlineTest) == 2);

    static_assert(std::is_same_v<std::underlying_type_t<RenderLifecycleState>, uint8>);
    static_assert(static_cast<uint8>(RenderLifecycleState::Stopped) == 0);
    static_assert(static_cast<uint8>(RenderLifecycleState::Starting) == 1);
    static_assert(static_cast<uint8>(RenderLifecycleState::Running) == 2);
    static_assert(static_cast<uint8>(RenderLifecycleState::StopRequested) == 3);
    static_assert(static_cast<uint8>(RenderLifecycleState::Draining) == 4);
    static_assert(static_cast<uint8>(RenderLifecycleState::Failed) == 5);

    static_assert(std::is_same_v<std::underlying_type_t<RenderResultClass>, uint8>);
    static_assert(static_cast<uint8>(RenderResultClass::Success) == 0);
    static_assert(static_cast<uint8>(RenderResultClass::ExpectedPressure) == 1);
    static_assert(static_cast<uint8>(RenderResultClass::RecoverableFrame) == 2);
    static_assert(static_cast<uint8>(RenderResultClass::FrameFatal) == 3);
    static_assert(static_cast<uint8>(RenderResultClass::RuntimeFatal) == 4);

    static_assert(std::is_same_v<std::underlying_type_t<RenderFramePublishCode>, uint8>);
    static_assert(static_cast<uint8>(RenderFramePublishCode::Accepted) == 0);
    static_assert(static_cast<uint8>(RenderFramePublishCode::ReplacedOlder) == 1);
    static_assert(static_cast<uint8>(RenderFramePublishCode::InvalidPacket) == 2);
    static_assert(static_cast<uint8>(RenderFramePublishCode::OutOfOrder) == 3);
    static_assert(static_cast<uint8>(RenderFramePublishCode::ShuttingDown) == 4);
    static_assert(static_cast<uint8>(RenderFramePublishCode::NotRunning) == 5);

    static_assert(std::is_same_v<std::underlying_type_t<RenderResizeCode>, uint8>);
    static_assert(static_cast<uint8>(RenderResizeCode::Accepted) == 0);
    static_assert(static_cast<uint8>(RenderResizeCode::CoalescedOlder) == 1);
    static_assert(static_cast<uint8>(RenderResizeCode::StaleGeneration) == 2);
    static_assert(static_cast<uint8>(RenderResizeCode::InvalidSurface) == 3);
    static_assert(static_cast<uint8>(RenderResizeCode::ShuttingDown) == 4);
    static_assert(static_cast<uint8>(RenderResizeCode::NotRunning) == 5);

    static_assert(std::is_same_v<std::underlying_type_t<RenderRuntimeCode>, uint8>);
    static_assert(static_cast<uint8>(RenderRuntimeCode::None) == 0);
    static_assert(static_cast<uint8>(RenderRuntimeCode::Running) == 1);
    static_assert(static_cast<uint8>(RenderRuntimeCode::StopRequested) == 2);
    static_assert(static_cast<uint8>(RenderRuntimeCode::Stopped) == 3);
    static_assert(static_cast<uint8>(RenderRuntimeCode::InvalidConfiguration) == 4);
    static_assert(static_cast<uint8>(RenderRuntimeCode::InvalidSurface) == 5);
    static_assert(static_cast<uint8>(RenderRuntimeCode::ExecutorStartFailed) == 6);
    static_assert(static_cast<uint8>(RenderRuntimeCode::DeviceCreationFailed) == 7);
    static_assert(static_cast<uint8>(RenderRuntimeCode::SurfaceCreationFailed) == 8);
    static_assert(static_cast<uint8>(RenderRuntimeCode::RenderGraphValidationFailed) == 9);
    static_assert(static_cast<uint8>(RenderRuntimeCode::DeviceLost) == 10);
    static_assert(static_cast<uint8>(RenderRuntimeCode::UnhandledException) == 11);
    static_assert(static_cast<uint8>(RenderRuntimeCode::OwnershipViolation) == 12);
    static_assert(static_cast<uint8>(RenderRuntimeCode::StartupTimedOut) == 13);
    static_assert(static_cast<uint8>(RenderRuntimeCode::ShutdownTimedOut) == 14);

    static_assert(std::is_same_v<std::underlying_type_t<RenderTerminalCause>, uint8>);
    static_assert(static_cast<uint8>(RenderTerminalCause::None) == 0);
    static_assert(static_cast<uint8>(RenderTerminalCause::NormalStop) == 1);
    static_assert(static_cast<uint8>(RenderTerminalCause::StartupFailure) == 2);
    static_assert(static_cast<uint8>(RenderTerminalCause::DeviceLost) == 3);
    static_assert(static_cast<uint8>(RenderTerminalCause::UnhandledException) == 4);
    static_assert(static_cast<uint8>(RenderTerminalCause::OwnershipViolation) == 5);
    static_assert(static_cast<uint8>(RenderTerminalCause::WatchdogTimeout) == 6);
    static_assert(static_cast<uint8>(RenderTerminalCause::ExecutorFailure) == 7);

    static_assert(std::is_same_v<std::underlying_type_t<RenderTeardownMode>, uint8>);
    static_assert(static_cast<uint8>(RenderTeardownMode::None) == 0);
    static_assert(static_cast<uint8>(RenderTeardownMode::NormalDrain) == 1);
    static_assert(static_cast<uint8>(RenderTeardownMode::DeviceLostTeardown) == 2);
    static_assert(static_cast<uint8>(RenderTeardownMode::FatalTimeout) == 3);

    static_assert(std::is_same_v<std::underlying_type_t<RenderShutdownCode>, uint8>);
    static_assert(static_cast<uint8>(RenderShutdownCode::None) == 0);
    static_assert(static_cast<uint8>(RenderShutdownCode::Completed) == 1);
    static_assert(static_cast<uint8>(RenderShutdownCode::AlreadyStopped) == 2);
    static_assert(static_cast<uint8>(RenderShutdownCode::DeviceLost) == 3);
    static_assert(static_cast<uint8>(RenderShutdownCode::TimedOut) == 4);
    static_assert(static_cast<uint8>(RenderShutdownCode::ExecutorJoinFailed) == 5);

    constexpr auto RVX_TEST_TIMEOUT = 2s;

    NativeSurfaceDesc MakeSurface(uint64 generation = 1,
                                  uint32 width = 64,
                                  uint32 height = 64)
    {
        NativeSurfaceDesc surface;
        surface.platform = NativeSurfacePlatform::Win32;
        surface.nativeWindow = 1;
        surface.width = width;
        surface.height = height;
        surface.generation = generation;
        return surface;
    }

    std::unique_ptr<const RenderFramePacket> MakePacket(uint64 sequence)
    {
        RenderFramePacketBuilder builder;
        RenderFrameHeader header;
        header.sequence = sequence;
        RenderViewSnapshot view;
        view.viewportWidth = 64;
        view.viewportHeight = 64;
        RenderFeatureSnapshot features;
        features.BeginBuild(sequence);
        features.MarkComplete();
        RenderExtractionDiagnostics extraction;
        extraction.complete = true;

        EXPECT_TRUE(builder.SetHeader(header));
        EXPECT_TRUE(builder.SetView(view));
        EXPECT_TRUE(builder.SetSky(RenderSkySnapshot{}));
        EXPECT_TRUE(builder.SetEnvironment(RenderEnvironmentSnapshot{}));
        EXPECT_TRUE(builder.SetSettings(RenderFrameSettings{}));
        EXPECT_TRUE(builder.SetCaptureRequest(RenderFrameCaptureRequest{}));
        EXPECT_TRUE(builder.SetFeatures(std::move(features)));
        EXPECT_TRUE(builder.SetExtractionDiagnostics(extraction));
        return builder.Seal();
    }

    bool WaitForPresentedFrame(const RenderThreadRuntime& runtime,
                               uint64 sequence)
    {
        const auto deadline =
            std::chrono::steady_clock::now() + RVX_TEST_TIMEOUT;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (runtime.GetDiagnosticsSnapshot().lastPresentedFrameSequence ==
                sequence)
            {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        }
        return false;
    }

    bool WaitForSurfaceGeneration(const RenderThreadRuntime& runtime,
                                  uint64 generation)
    {
        const auto deadline =
            std::chrono::steady_clock::now() + RVX_TEST_TIMEOUT;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (runtime.GetDiagnosticsSnapshot().surfaceGeneration ==
                generation)
            {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        }
        return false;
    }

    bool WaitForRuntimeLifecycle(const RenderThreadRuntime& runtime,
                                 RenderLifecycleState lifecycle)
    {
        const auto deadline =
            std::chrono::steady_clock::now() + RVX_TEST_TIMEOUT;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (runtime.GetLastRuntimeResult().lifecycle == lifecycle)
            {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        }
        return false;
    }

    bool WaitForReleaseDiagnostics(const RenderThreadRuntime& runtime,
                                   uint64 acceptedCount,
                                   uint32 currentUsage)
    {
        const auto deadline =
            std::chrono::steady_clock::now() + RVX_TEST_TIMEOUT;
        while (std::chrono::steady_clock::now() < deadline)
        {
            const RenderReleaseTransportDiagnostics diagnostics =
                runtime.GetDiagnosticsSnapshot().releaseTransport;
            if (diagnostics.acceptedCount == acceptedCount &&
                diagnostics.currentUsage == currentUsage)
            {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        }
        return false;
    }

    TEST(RenderThreadRuntimeValidation, PublicResultDefaultsAreExact)
    {
        const RenderFramePublishResult frame;
        EXPECT_EQ(frame.code, RenderFramePublishCode::InvalidPacket);
        EXPECT_EQ(frame.resultClass, RenderResultClass::RecoverableFrame);
        EXPECT_EQ(frame.sequence, 0U);
        EXPECT_EQ(frame.replacedSequence, 0U);

        const RenderResizeResult resize;
        EXPECT_EQ(resize.code, RenderResizeCode::InvalidSurface);
        EXPECT_EQ(resize.resultClass, RenderResultClass::RecoverableFrame);
        EXPECT_EQ(resize.generation, 0U);
        EXPECT_EQ(resize.replacedGeneration, 0U);

        const RenderRuntimeResult runtime;
        EXPECT_EQ(runtime.code, RenderRuntimeCode::None);
        EXPECT_EQ(runtime.resultClass, RenderResultClass::Success);
        EXPECT_EQ(runtime.lifecycle, RenderLifecycleState::Stopped);
        EXPECT_EQ(runtime.executor, RenderExecutorKind::None);
        EXPECT_EQ(runtime.terminalCause, RenderTerminalCause::None);
        EXPECT_EQ(runtime.teardownMode, RenderTeardownMode::None);
        EXPECT_EQ(runtime.backend, RHIBackendType::None);
        EXPECT_EQ(runtime.frameSequence, 0U);
        EXPECT_EQ(runtime.requestSequence, 0U);
        EXPECT_FALSE(runtime.assetId.IsValid());
        EXPECT_FALSE(runtime.handle.IsValid());
        EXPECT_EQ(runtime.surfaceGeneration, 0U);
        EXPECT_EQ(runtime.nativeError, 0U);
        EXPECT_TRUE(runtime.message.empty());

        const RenderShutdownResult shutdown;
        EXPECT_EQ(shutdown.code, RenderShutdownCode::None);
        EXPECT_EQ(shutdown.resultClass, RenderResultClass::Success);
        EXPECT_EQ(shutdown.lifecycle, RenderLifecycleState::Stopped);
        EXPECT_EQ(shutdown.terminalCause, RenderTerminalCause::None);
        EXPECT_EQ(shutdown.teardownMode, RenderTeardownMode::None);
        EXPECT_EQ(shutdown.backend, RHIBackendType::None);
        EXPECT_EQ(shutdown.lastSubmittedFrameSequence, 0U);
        EXPECT_EQ(shutdown.surfaceGeneration, 0U);
        EXPECT_EQ(shutdown.nativeError, 0U);
        EXPECT_TRUE(shutdown.message.empty());
    }

    TEST(RenderThreadRuntimeValidation, ExactOutcomeMappingsRejectUnknownValues)
    {
        EXPECT_EQ(ClassifyRenderFramePublishCode(RenderFramePublishCode::Accepted),
                  RenderResultClass::Success);
        EXPECT_EQ(ClassifyRenderFramePublishCode(RenderFramePublishCode::ReplacedOlder),
                  RenderResultClass::ExpectedPressure);
        EXPECT_EQ(ClassifyRenderFramePublishCode(RenderFramePublishCode::InvalidPacket),
                  RenderResultClass::RecoverableFrame);
        EXPECT_EQ(ClassifyRenderFramePublishCode(RenderFramePublishCode::OutOfOrder),
                  RenderResultClass::RecoverableFrame);
        EXPECT_EQ(ClassifyRenderFramePublishCode(RenderFramePublishCode::ShuttingDown),
                  RenderResultClass::ExpectedPressure);
        EXPECT_EQ(ClassifyRenderFramePublishCode(RenderFramePublishCode::NotRunning),
                  RenderResultClass::ExpectedPressure);
        EXPECT_FALSE(IsDeclaredRenderFramePublishCode(static_cast<RenderFramePublishCode>(255)));

        EXPECT_EQ(ClassifyRenderResizeCode(RenderResizeCode::Accepted), RenderResultClass::Success);
        EXPECT_EQ(ClassifyRenderResizeCode(RenderResizeCode::CoalescedOlder), RenderResultClass::ExpectedPressure);
        EXPECT_EQ(ClassifyRenderResizeCode(RenderResizeCode::StaleGeneration), RenderResultClass::RecoverableFrame);
        EXPECT_EQ(ClassifyRenderResizeCode(RenderResizeCode::InvalidSurface), RenderResultClass::RecoverableFrame);
        EXPECT_EQ(ClassifyRenderResizeCode(RenderResizeCode::ShuttingDown), RenderResultClass::ExpectedPressure);
        EXPECT_EQ(ClassifyRenderResizeCode(RenderResizeCode::NotRunning), RenderResultClass::ExpectedPressure);
        EXPECT_FALSE(IsDeclaredRenderResizeCode(static_cast<RenderResizeCode>(255)));

        EXPECT_FALSE(IsDeclaredRenderRuntimeCode(static_cast<RenderRuntimeCode>(255)));
        EXPECT_FALSE(IsDeclaredRenderShutdownCode(static_cast<RenderShutdownCode>(255)));
    }

    TEST(RenderThreadRuntimeValidation, RuntimeCodeMappingsAndNativeContextAreExact)
    {
        struct Mapping
        {
            RenderRuntimeCode code;
            RenderResultClass resultClass;
            RenderLifecycleState lifecycle;
            RenderTerminalCause terminalCause;
            RenderTeardownMode teardownMode;
        };
        const std::array mappings{
            Mapping{RenderRuntimeCode::None, RenderResultClass::Success, RenderLifecycleState::Stopped, RenderTerminalCause::None, RenderTeardownMode::None},
            Mapping{RenderRuntimeCode::Running, RenderResultClass::Success, RenderLifecycleState::Running, RenderTerminalCause::None, RenderTeardownMode::None},
            Mapping{RenderRuntimeCode::StopRequested, RenderResultClass::Success, RenderLifecycleState::StopRequested, RenderTerminalCause::NormalStop, RenderTeardownMode::NormalDrain},
            Mapping{RenderRuntimeCode::Stopped, RenderResultClass::Success, RenderLifecycleState::Stopped, RenderTerminalCause::NormalStop, RenderTeardownMode::NormalDrain},
            Mapping{RenderRuntimeCode::InvalidConfiguration, RenderResultClass::RuntimeFatal, RenderLifecycleState::Failed, RenderTerminalCause::StartupFailure, RenderTeardownMode::NormalDrain},
            Mapping{RenderRuntimeCode::InvalidSurface, RenderResultClass::RuntimeFatal, RenderLifecycleState::Failed, RenderTerminalCause::StartupFailure, RenderTeardownMode::NormalDrain},
            Mapping{RenderRuntimeCode::ExecutorStartFailed, RenderResultClass::RuntimeFatal, RenderLifecycleState::Failed, RenderTerminalCause::StartupFailure, RenderTeardownMode::NormalDrain},
            Mapping{RenderRuntimeCode::DeviceCreationFailed, RenderResultClass::RuntimeFatal, RenderLifecycleState::Failed, RenderTerminalCause::StartupFailure, RenderTeardownMode::NormalDrain},
            Mapping{RenderRuntimeCode::SurfaceCreationFailed, RenderResultClass::RuntimeFatal, RenderLifecycleState::Failed, RenderTerminalCause::StartupFailure, RenderTeardownMode::NormalDrain},
            Mapping{RenderRuntimeCode::RenderGraphValidationFailed, RenderResultClass::FrameFatal, RenderLifecycleState::Running, RenderTerminalCause::None, RenderTeardownMode::None},
            Mapping{RenderRuntimeCode::DeviceLost, RenderResultClass::RuntimeFatal, RenderLifecycleState::Failed, RenderTerminalCause::DeviceLost, RenderTeardownMode::DeviceLostTeardown},
            Mapping{RenderRuntimeCode::UnhandledException, RenderResultClass::RuntimeFatal, RenderLifecycleState::Failed, RenderTerminalCause::UnhandledException, RenderTeardownMode::NormalDrain},
            Mapping{RenderRuntimeCode::OwnershipViolation, RenderResultClass::RuntimeFatal, RenderLifecycleState::Failed, RenderTerminalCause::OwnershipViolation, RenderTeardownMode::NormalDrain},
            Mapping{RenderRuntimeCode::StartupTimedOut, RenderResultClass::RuntimeFatal, RenderLifecycleState::Failed, RenderTerminalCause::WatchdogTimeout, RenderTeardownMode::FatalTimeout},
            Mapping{RenderRuntimeCode::ShutdownTimedOut, RenderResultClass::RuntimeFatal, RenderLifecycleState::Failed, RenderTerminalCause::WatchdogTimeout, RenderTeardownMode::FatalTimeout}};

        for (const Mapping& mapping : mappings)
        {
            SCOPED_TRACE(static_cast<uint32>(mapping.code));
            auto probe = std::make_shared<RenderFrameConsumerTestProbe>();
            probe->startupCode = mapping.code;
            probe->startupNativeError = 73U;
            RenderRuntimeConfig config;
            config.backendType = RHIBackendType::DX11;
            RenderThreadRuntime runtime(
                config,
                MakeSurface(),
                RenderExecutorKind::InlineTest,
                CreateInlineRenderExecutor(),
                CreateRecordingRenderFrameConsumer(probe));
            const RenderRuntimeResult result = runtime.Start();
            EXPECT_EQ(result.code, mapping.code);
            EXPECT_EQ(result.resultClass, mapping.resultClass);
            EXPECT_EQ(result.lifecycle, mapping.lifecycle);
            EXPECT_EQ(result.terminalCause, mapping.terminalCause);
            EXPECT_EQ(result.teardownMode, mapping.teardownMode);
            EXPECT_EQ(result.executor, RenderExecutorKind::InlineTest);
            EXPECT_EQ(result.backend, RHIBackendType::DX11);
            EXPECT_EQ(result.surfaceGeneration, 1U);
            EXPECT_EQ(result.nativeError, 73U);
            if (mapping.code == RenderRuntimeCode::Running)
            {
                EXPECT_EQ(runtime.Stop().code, RenderShutdownCode::Completed);
            }
        }
    }

    TEST(RenderThreadRuntimeValidation, ShutdownCodeMappingsAndNativeContextAreExact)
    {
        struct Mapping
        {
            RenderShutdownCode code;
            RenderResultClass resultClass;
            RenderLifecycleState lifecycle;
            RenderTerminalCause terminalCause;
            RenderTeardownMode teardownMode;
        };
        const std::array mappings{
            Mapping{RenderShutdownCode::None, RenderResultClass::Success, RenderLifecycleState::Stopped, RenderTerminalCause::None, RenderTeardownMode::None},
            Mapping{RenderShutdownCode::Completed, RenderResultClass::Success, RenderLifecycleState::Stopped, RenderTerminalCause::NormalStop, RenderTeardownMode::NormalDrain},
            Mapping{RenderShutdownCode::AlreadyStopped, RenderResultClass::Success, RenderLifecycleState::Stopped, RenderTerminalCause::NormalStop, RenderTeardownMode::None},
            Mapping{RenderShutdownCode::DeviceLost, RenderResultClass::RuntimeFatal, RenderLifecycleState::Failed, RenderTerminalCause::DeviceLost, RenderTeardownMode::DeviceLostTeardown},
            Mapping{RenderShutdownCode::TimedOut, RenderResultClass::RuntimeFatal, RenderLifecycleState::Failed, RenderTerminalCause::WatchdogTimeout, RenderTeardownMode::FatalTimeout},
            Mapping{RenderShutdownCode::ExecutorJoinFailed, RenderResultClass::RuntimeFatal, RenderLifecycleState::Failed, RenderTerminalCause::ExecutorFailure, RenderTeardownMode::FatalTimeout}};

        for (const Mapping& mapping : mappings)
        {
            SCOPED_TRACE(static_cast<uint32>(mapping.code));
            auto probe = std::make_shared<RenderFrameConsumerTestProbe>();
            probe->shutdownCode = mapping.code;
            probe->shutdownNativeError = 91U;
            RenderRuntimeConfig config;
            config.backendType = RHIBackendType::DX11;
            RenderThreadRuntime runtime(
                config,
                MakeSurface(),
                RenderExecutorKind::InlineTest,
                CreateInlineRenderExecutor(),
                CreateRecordingRenderFrameConsumer(probe));
            ASSERT_EQ(runtime.Start().code, RenderRuntimeCode::Running);
            const RenderShutdownResult result = runtime.Stop();
            EXPECT_EQ(result.code, mapping.code);
            EXPECT_EQ(result.resultClass, mapping.resultClass);
            EXPECT_EQ(result.lifecycle, mapping.lifecycle);
            EXPECT_EQ(result.terminalCause, mapping.terminalCause);
            EXPECT_EQ(result.teardownMode, mapping.teardownMode);
            EXPECT_EQ(result.backend, RHIBackendType::DX11);
            EXPECT_EQ(result.surfaceGeneration, 1U);
            EXPECT_EQ(result.nativeError, 91U);
        }
    }

    TEST(RenderThreadRuntimeValidation, ExecutorStartFailurePreservesNativeError)
    {
        auto probe = std::make_shared<RenderFrameConsumerTestProbe>();
        RenderRuntimeConfig config;
        config.backendType = RHIBackendType::DX11;
        RenderThreadRuntime runtime(
            config,
            MakeSurface(),
            RenderExecutorKind::Dedicated,
            CreateFailingRenderExecutor(1234U),
            CreateRecordingRenderFrameConsumer(probe));
        const RenderRuntimeResult result = runtime.Start();
        EXPECT_EQ(result.code, RenderRuntimeCode::ExecutorStartFailed);
        EXPECT_EQ(result.nativeError, 1234U);
        EXPECT_EQ(result.resultClass, RenderResultClass::RuntimeFatal);
        EXPECT_EQ(result.terminalCause, RenderTerminalCause::StartupFailure);
    }

    TEST(RenderThreadRuntimeValidation,
         StartupTimeoutRejectsLateAcknowledgementAndJoinsExecutor)
    {
        auto consumerProbe =
            std::make_shared<RenderFrameConsumerTestProbe>();
        consumerProbe->BlockStartup();
        auto joinProbe = std::make_shared<RenderExecutorJoinTestProbe>();
        RenderRuntimeConfig config;
        config.backendType = RHIBackendType::DX11;
        config.startupWatchdog = 20ms;
        config.shutdownWatchdog = RVX_TEST_TIMEOUT;
        RenderThreadRuntime runtime(
            config,
            MakeSurface(),
            RenderExecutorKind::Dedicated,
            CreateJoinRecordingDedicatedExecutor(joinProbe),
            CreateRecordingRenderFrameConsumer(consumerProbe));

        bool observedTimeout = false;
        std::thread lateAcknowledgement([&]() {
            const auto deadline =
                std::chrono::steady_clock::now() + RVX_TEST_TIMEOUT;
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (runtime.GetLastRuntimeResult().code ==
                    RenderRuntimeCode::StartupTimedOut)
                {
                    observedTimeout = true;
                    break;
                }
                std::this_thread::yield();
            }
            consumerProbe->ReleaseStartup();
        });

        const RenderRuntimeResult result = runtime.Start();
        lateAcknowledgement.join();
        ASSERT_TRUE(consumerProbe->WaitForEventCount(
            RenderRuntimeTestEvent::Shutdown, 1U, RVX_TEST_TIMEOUT));

        ASSERT_TRUE(observedTimeout);
        EXPECT_EQ(result.code, RenderRuntimeCode::StartupTimedOut);
        EXPECT_EQ(result.resultClass, RenderResultClass::RuntimeFatal);
        EXPECT_EQ(result.lifecycle, RenderLifecycleState::Failed);
        EXPECT_EQ(result.terminalCause,
                  RenderTerminalCause::WatchdogTimeout);
        EXPECT_EQ(result.teardownMode, RenderTeardownMode::FatalTimeout);
        EXPECT_EQ(runtime.GetLastRuntimeResult().code,
                  RenderRuntimeCode::StartupTimedOut);
        EXPECT_EQ(runtime.GetDiagnosticsSnapshot().lifecycle,
                  RenderLifecycleState::Failed);
        EXPECT_EQ(joinProbe->GetCallCount(), 1U);
        EXPECT_EQ(joinProbe->GetLastCode(),
                  RenderExecutorJoinCode::Joined);
        EXPECT_EQ(consumerProbe->GetShutdownThread(),
                  consumerProbe->GetStartupThread());
        EXPECT_EQ(consumerProbe->GetDestructionThread(),
                  consumerProbe->GetStartupThread());
    }

    TEST(RenderThreadRuntimeValidation, DiagnosticsPublicationIsImmutableAndOwned)
    {
        RenderDiagnosticsPublisher publisher;
        RenderDiagnosticsSnapshot first;
        first.publicationSequence = 1;
        first.lastTransition.message = "starting";
        publisher.Publish(first);
        const auto retained = publisher.AcquireShared();

        RenderDiagnosticsSnapshot second;
        second.publicationSequence = 2;
        second.lastTransition.message = "running";
        publisher.Publish(second);

        ASSERT_NE(retained, nullptr);
        EXPECT_EQ(retained->publicationSequence, 1U);
        EXPECT_EQ(retained->lastTransition.message, "starting");
        EXPECT_EQ(publisher.GetSnapshot().publicationSequence, 2U);
        EXPECT_EQ(publisher.GetSnapshot().lastTransition.message, "running");
    }

    TEST(RenderThreadRuntimeValidation, FutureDiagnosticsAreExplicitlyUnavailable)
    {
        const RenderDiagnosticsSnapshot snapshot;
        for (const RenderQueueTimelineDiagnostics& queue : snapshot.queues)
        {
            EXPECT_FALSE(queue.available);
            EXPECT_EQ(queue.logicalQueue, RVX_RENDER_DIAGNOSTICS_UNAVAILABLE_INDEX);
            EXPECT_EQ(queue.physicalDomain, RVX_RENDER_DIAGNOSTICS_UNAVAILABLE_INDEX);
            EXPECT_EQ(queue.completionMode, RVX_RENDER_DIAGNOSTICS_UNAVAILABLE_INDEX);
            EXPECT_EQ(queue.timelineState, RVX_RENDER_DIAGNOSTICS_UNAVAILABLE_INDEX);
            EXPECT_EQ(queue.lastSubmittedValue, 0U);
            EXPECT_EQ(queue.lastCompletedValue, 0U);
        }
        EXPECT_EQ(snapshot.resources.stateCounts,
                  (std::array<uint64, 7>{}));
        EXPECT_EQ(snapshot.retirement.entryCount, 0U);
        EXPECT_EQ(snapshot.retirement.estimatedBytes, 0U);
        EXPECT_EQ(snapshot.retirement.oldestPendingCompletionValues,
                  (std::array<uint64, 3>{}));
        EXPECT_FALSE(snapshot.lastFailure.available);
    }

    TEST(RenderThreadRuntimeValidation,
         ReleaseDiagnosticsTolerateConsumerObservationAheadOfProducerStats)
    {
        const auto caughtUp =
            RenderRuntimeDetail::ObserveReleaseDiagnostics(
                0U, 1U, RenderReleaseQueueSnapshot{});
        EXPECT_EQ(caughtUp.currentUsage, 0U);
        EXPECT_EQ(caughtUp.oldestPendingGeneration, 0U);

        const auto pending =
            RenderRuntimeDetail::ObserveReleaseDiagnostics(
                2U, 1U, RenderReleaseQueueSnapshot{1U, 91U});
        EXPECT_EQ(pending.currentUsage, 1U);
        EXPECT_EQ(pending.oldestPendingGeneration, 91U);
    }

    TEST(RenderThreadRuntimeValidation, InvalidConfiguredStartupIsStructuredAndTerminal)
    {
        RenderSubsystem subsystem;
        RenderRuntimeConfig config;
        config.frameBuffering = 1;
        subsystem.Configure(config, MakeSurface());

        EXPECT_THROW(subsystem.Initialize(), RenderSubsystemInitializationError);
        const RenderRuntimeResult result = subsystem.GetLastRuntimeResult();
        EXPECT_EQ(result.code, RenderRuntimeCode::InvalidConfiguration);
        EXPECT_EQ(result.resultClass, RenderResultClass::RuntimeFatal);
        EXPECT_EQ(result.lifecycle, RenderLifecycleState::Failed);
        EXPECT_EQ(result.terminalCause, RenderTerminalCause::StartupFailure);
        EXPECT_EQ(result.teardownMode, RenderTeardownMode::NormalDrain);
        EXPECT_THROW(subsystem.Configure(RenderRuntimeConfig{}, MakeSurface()),
                     std::logic_error);
        EXPECT_EQ(subsystem.GetDevice(), nullptr);
    }

    TEST(RenderThreadRuntimeValidation, SubsystemDelegatesItsResourceGatewayCore)
    {
        RenderSubsystem subsystem;
        RenderRuntimeConfig config;
        config.backendType = RHIBackendType::DX11;
        subsystem.Configure(config, MakeSurface());
        IRenderResourceGateway& gateway = subsystem;
        const RenderResourceReserveResult reserve =
            gateway.ReserveResource(AssetId{17}, RenderResourceKind::Material);
        ASSERT_EQ(reserve.code, RenderResourceReserveCode::Reserved);
        ASSERT_TRUE(reserve.handle.IsValid());
        const RenderResourceStatus status =
            gateway.QueryResourceStatus(reserve.handle);
        EXPECT_EQ(status.code, RenderResourceStatusCode::Current);
        EXPECT_EQ(status.state, RenderResourcePublicState::Reserved);
    }

    struct RuntimeCase
    {
        const char* name = nullptr;
        std::unique_ptr<IRenderExecutor> (*createExecutor)() = nullptr;
        RenderExecutorKind kind = RenderExecutorKind::None;
        bool dedicated = false;
    };

    void PrintTo(const RuntimeCase& value, std::ostream* stream)
    {
        *stream << value.name;
    }

    class RuntimeConformanceTest : public testing::TestWithParam<RuntimeCase>
    {
    };

    TEST_P(RuntimeConformanceTest, StartupFrameWakeIdleAndStopAreAcknowledged)
    {
        auto probe = std::make_shared<RenderFrameConsumerTestProbe>();
        RenderRuntimeConfig config;
        config.backendType = RHIBackendType::DX11;
        RenderThreadRuntime runtime(config,
                                    MakeSurface(),
                                    GetParam().kind,
                                    GetParam().createExecutor(),
                                    CreateRecordingRenderFrameConsumer(probe));

        EXPECT_EQ(runtime.GetLastRuntimeResult().lifecycle,
                  RenderLifecycleState::Stopped);
        EXPECT_EQ(runtime.TryPublishFrame(MakePacket(1)).code,
                  RenderFramePublishCode::NotRunning);

        const RenderRuntimeResult start = runtime.Start();
        ASSERT_EQ(start.code, RenderRuntimeCode::Running);
        ASSERT_TRUE(probe->WaitForEventCount(RenderRuntimeTestEvent::Started,
                                             1,
                                             RVX_TEST_TIMEOUT));
        RenderDiagnosticsSnapshot running = runtime.GetDiagnosticsSnapshot();
        EXPECT_EQ(running.executor, GetParam().kind);
        EXPECT_EQ(running.lifecycle, RenderLifecycleState::Running);
        ASSERT_GE(running.transitionCount, 2U);
        EXPECT_EQ(running.transitions[0].from, RenderLifecycleState::Stopped);
        EXPECT_EQ(running.transitions[0].to, RenderLifecycleState::Starting);
        EXPECT_EQ(running.transitions[1].from, RenderLifecycleState::Starting);
        EXPECT_EQ(running.transitions[1].to, RenderLifecycleState::Running);

        EXPECT_EQ(runtime.TryPublishFrame(nullptr).code,
                  RenderFramePublishCode::InvalidPacket);

        const uint32 framePollCount =
            probe->GetEventCount(RenderRuntimeTestEvent::Poll);
        const RenderFramePublishResult publish =
            runtime.TryPublishFrame(MakePacket(2));
        EXPECT_EQ(publish.code, RenderFramePublishCode::Accepted);
        EXPECT_EQ(publish.resultClass, RenderResultClass::Success);
        EXPECT_EQ(publish.sequence, 2U);
        EXPECT_EQ(runtime.TryPublishFrame(MakePacket(1)).code,
                  RenderFramePublishCode::OutOfOrder);
        ASSERT_TRUE(probe->WaitForEventCount(RenderRuntimeTestEvent::Frame,
                                             1,
                                             RVX_TEST_TIMEOUT));

        ASSERT_TRUE(probe->WaitForEventCount(RenderRuntimeTestEvent::Poll,
                                             framePollCount + 1U,
                                             RVX_TEST_TIMEOUT));
        ASSERT_TRUE(WaitForPresentedFrame(runtime, 2U));

        const uint64 presented = runtime.GetDiagnosticsSnapshot().lastPresentedFrameSequence;
        std::this_thread::sleep_for(25ms);
        EXPECT_EQ(runtime.GetDiagnosticsSnapshot().lastPresentedFrameSequence,
                  presented);
        EXPECT_EQ(probe->GetEventCount(RenderRuntimeTestEvent::Frame), 1U);

        const RenderShutdownResult shutdown = runtime.Stop();
        EXPECT_EQ(shutdown.code, RenderShutdownCode::Completed);
        EXPECT_EQ(shutdown.lifecycle, RenderLifecycleState::Stopped);
        EXPECT_EQ(shutdown.terminalCause, RenderTerminalCause::NormalStop);
        EXPECT_EQ(shutdown.teardownMode, RenderTeardownMode::NormalDrain);
        EXPECT_EQ(shutdown.lastSubmittedFrameSequence, 2U);
        EXPECT_EQ(probe->GetStartupThread(), probe->GetShutdownThread());
        EXPECT_EQ(probe->GetStartupThread(), probe->GetDestructionThread());
        if (GetParam().dedicated)
        {
            EXPECT_NE(probe->GetStartupThread(), std::this_thread::get_id());
            EXPECT_GT(runtime.GetDiagnosticsSnapshot().idleWaitCount, 0U);
        }
        else
        {
            EXPECT_EQ(probe->GetStartupThread(), std::this_thread::get_id());
        }
    }

    TEST(RenderThreadRuntimeValidation, ResizeValidationAndCoalescingAreExplicit)
    {
        auto probe = std::make_shared<RenderFrameConsumerTestProbe>();
        probe->BlockFrames();
        RenderRuntimeConfig config;
        config.backendType = RHIBackendType::DX11;
        RenderThreadRuntime runtime(config,
                                    MakeSurface(),
                                    RenderExecutorKind::Dedicated,
                                    CreateDedicatedRenderExecutor(),
                                    CreateRecordingRenderFrameConsumer(probe));
        ASSERT_EQ(runtime.Start().code, RenderRuntimeCode::Running);
        ASSERT_EQ(runtime.TryPublishFrame(MakePacket(1)).code,
                  RenderFramePublishCode::Accepted);
        ASSERT_TRUE(probe->WaitForEventCount(RenderRuntimeTestEvent::Frame,
                                             1,
                                             RVX_TEST_TIMEOUT));

        EXPECT_EQ(runtime.TryPublishFrame(MakePacket(2)).code,
                  RenderFramePublishCode::Accepted);
        EXPECT_EQ(runtime.TryPublishFrame(MakePacket(3)).code,
                  RenderFramePublishCode::Accepted);
        EXPECT_EQ(runtime.TryPublishFrame(MakePacket(4)).code,
                  RenderFramePublishCode::Accepted);
        const RenderFramePublishResult replacement =
            runtime.TryPublishFrame(MakePacket(5));
        EXPECT_EQ(replacement.code, RenderFramePublishCode::ReplacedOlder);
        EXPECT_EQ(replacement.resultClass, RenderResultClass::ExpectedPressure);
        EXPECT_EQ(replacement.replacedSequence, 2U);
        EXPECT_EQ(runtime.TryPublishFrame(MakePacket(4)).code,
                  RenderFramePublishCode::OutOfOrder);

        const RenderResizeResult accepted = runtime.RequestResize(MakeSurface(2, 80, 80));
        EXPECT_EQ(accepted.code, RenderResizeCode::Accepted);
        const RenderResizeResult coalesced = runtime.RequestResize(MakeSurface(3, 96, 96));
        EXPECT_EQ(coalesced.code, RenderResizeCode::CoalescedOlder);
        EXPECT_EQ(coalesced.replacedGeneration, 2U);
        EXPECT_EQ(runtime.RequestResize(MakeSurface(2, 80, 80)).code,
                  RenderResizeCode::StaleGeneration);
        NativeSurfaceDesc invalid = MakeSurface(4);
        invalid.nativeWindow = 0;
        EXPECT_EQ(runtime.RequestResize(invalid).code,
                  RenderResizeCode::InvalidSurface);

        probe->ReleaseFrames();
        ASSERT_TRUE(probe->WaitForEventCount(RenderRuntimeTestEvent::Surface,
                                             1,
                                             RVX_TEST_TIMEOUT));
        const uint32 pollCount =
            probe->GetEventCount(RenderRuntimeTestEvent::Poll);
        ASSERT_TRUE(probe->WaitForEventCount(RenderRuntimeTestEvent::Poll,
                                             pollCount + 1U,
                                             RVX_TEST_TIMEOUT));
        ASSERT_TRUE(WaitForSurfaceGeneration(runtime, 3U));
        EXPECT_EQ(runtime.Stop().code, RenderShutdownCode::Completed);
    }

    TEST(RenderThreadRuntimeValidation, PumpProcessesReleaseBeforeUpload)
    {
        auto probe = std::make_shared<RenderFrameConsumerTestProbe>();
        probe->BlockFrames();
        RenderRuntimeConfig config;
        config.backendType = RHIBackendType::DX11;
        RenderThreadRuntime runtime(config,
                                    MakeSurface(),
                                    RenderExecutorKind::Dedicated,
                                    CreateDedicatedRenderExecutor(),
                                    CreateRecordingRenderFrameConsumer(probe));
        ASSERT_EQ(runtime.Start().code, RenderRuntimeCode::Running);
        ASSERT_EQ(runtime.TryPublishFrame(MakePacket(1)).code,
                  RenderFramePublishCode::Accepted);
        ASSERT_TRUE(probe->WaitForEventCount(RenderRuntimeTestEvent::Frame,
                                             1,
                                             RVX_TEST_TIMEOUT));
        probe->ClearEvents();

        const RenderResourceReserveResult uploadReserve =
            runtime.ReserveResource(AssetId{31}, RenderResourceKind::Material);
        ASSERT_EQ(uploadReserve.code, RenderResourceReserveCode::Reserved);
        ResourceUploadRequestCreateInfo requestInfo;
        requestInfo.sequence = 1;
        requestInfo.assetId = AssetId{31};
        requestInfo.handle = uploadReserve.handle;
        requestInfo.kind = RenderResourceKind::Material;
        requestInfo.payload = MaterialUploadPayload{};
        requestInfo.declaredPayloadBytes = 0;
        const ResourceUploadRequestCreateResult request =
            ResourceUploadRequest::Create(std::move(requestInfo));
        ASSERT_EQ(request.code, ResourceUploadRequestCreateCode::Created);
        ASSERT_EQ(runtime.TryEnqueueUpload(request.request).code,
                  RenderUploadEnqueueCode::Accepted);

        const RenderResourceReserveResult releaseReserve =
            runtime.ReserveResource(AssetId{32}, RenderResourceKind::Texture);
        ASSERT_EQ(releaseReserve.code, RenderResourceReserveCode::Reserved);
        ASSERT_EQ(runtime.RequestRelease(releaseReserve.handle).code,
                  RenderReleaseCode::Accepted);

        probe->ReleaseFrames();
        ASSERT_TRUE(probe->WaitForEventCount(RenderRuntimeTestEvent::Release,
                                             1,
                                             RVX_TEST_TIMEOUT));
        ASSERT_TRUE(probe->WaitForEventCount(RenderRuntimeTestEvent::Upload,
                                             1,
                                             RVX_TEST_TIMEOUT));
        const std::vector<RenderRuntimeTestEvent> events = probe->GetEvents();
        const auto release = std::find(events.begin(),
                                       events.end(),
                                       RenderRuntimeTestEvent::Release);
        const auto upload = std::find(events.begin(),
                                      events.end(),
                                      RenderRuntimeTestEvent::Upload);
        ASSERT_NE(release, events.end());
        ASSERT_NE(upload, events.end());
        EXPECT_LT(release, upload);
        ASSERT_TRUE(WaitForReleaseDiagnostics(runtime, 1U, 0U));
        const RenderReleaseTransportDiagnostics releaseDiagnostics =
            runtime.GetDiagnosticsSnapshot().releaseTransport;
        EXPECT_EQ(releaseDiagnostics.acceptedCount, 1U);
        EXPECT_EQ(releaseDiagnostics.completedCount, 0U);
        EXPECT_EQ(releaseDiagnostics.currentUsage, 0U);
        EXPECT_EQ(releaseDiagnostics.oldestPendingGeneration, 0U);
        EXPECT_EQ(runtime.Stop().code, RenderShutdownCode::Completed);
    }

    TEST(RenderThreadRuntimeValidation, StopPreemptsQueuedFrameAndSurfaceWork)
    {
        auto probe = std::make_shared<RenderFrameConsumerTestProbe>();
        probe->BlockFrames();
        RenderRuntimeConfig config;
        config.backendType = RHIBackendType::DX11;
        RenderThreadRuntime runtime(config,
                                    MakeSurface(),
                                    RenderExecutorKind::Dedicated,
                                    CreateDedicatedRenderExecutor(),
                                    CreateRecordingRenderFrameConsumer(probe));
        ASSERT_EQ(runtime.Start().code, RenderRuntimeCode::Running);
        ASSERT_EQ(runtime.TryPublishFrame(MakePacket(1)).code,
                  RenderFramePublishCode::Accepted);
        ASSERT_TRUE(probe->WaitForEventCount(RenderRuntimeTestEvent::Frame,
                                             1,
                                             RVX_TEST_TIMEOUT));

        RenderShutdownResult shutdown;
        std::thread stopper([&runtime, &shutdown]() {
            shutdown = runtime.Stop();
        });
        ASSERT_TRUE(WaitForRuntimeLifecycle(
            runtime, RenderLifecycleState::StopRequested));
        EXPECT_EQ(runtime.TryPublishFrame(MakePacket(2)).code,
                  RenderFramePublishCode::ShuttingDown);
        EXPECT_EQ(runtime.RequestResize(MakeSurface(2, 80, 80)).code,
                  RenderResizeCode::ShuttingDown);
        probe->ReleaseFrames();
        stopper.join();

        EXPECT_EQ(shutdown.code, RenderShutdownCode::Completed);
        EXPECT_EQ(probe->GetEventCount(RenderRuntimeTestEvent::Frame), 1U);
        EXPECT_EQ(probe->GetEventCount(RenderRuntimeTestEvent::Surface), 0U);
    }

    INSTANTIATE_TEST_SUITE_P(
        RenderThreadRuntimeValidationShared,
        RuntimeConformanceTest,
        testing::Values(
            RuntimeCase{"Inline", &CreateInlineRenderExecutor, RenderExecutorKind::InlineTest, false},
            RuntimeCase{"Dedicated", &CreateDedicatedRenderExecutor, RenderExecutorKind::Dedicated, true}),
        [](const testing::TestParamInfo<RuntimeCase>& info) {
            return info.param.name;
        });
} // namespace
} // namespace RVX
