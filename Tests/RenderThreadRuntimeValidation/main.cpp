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
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

namespace RVX
{
    void RenderThreadRuntimeTestAccess::ReportTransportFatal(
        RenderThreadRuntime& runtime,
        const char* message) noexcept
    {
        {
            std::lock_guard lock(runtime.m_publicationMutex);
            RenderThreadRuntime::RuntimeFatalThunk(&runtime, message);
        }
        runtime.NotifyExecutor();
    }

    void RenderThreadRuntimeTestAccess::
        ForceReleasePublicationInvariantFailure(
            RenderThreadRuntime& runtime) noexcept
    {
        RenderReleaseQueue& queue =
            runtime.m_resourceGateway->m_releaseQueue;
        std::lock_guard lock(queue.m_mutex);
        queue.m_forceNextPublicationFailureForTest = true;
    }

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

    class BlockingRuntimeBootstrapHook final
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

    class BlockingPublicationHook final : public IRenderPublicationHook,
                                          public NonMovable
    {
    public:
        explicit BlockingPublicationHook(RenderPublicationPath path)
            : m_path(path)
        {
        }

        void BeforeMutation(RenderPublicationPath path) noexcept override
        {
            if (path != m_path)
            {
                return;
            }
            std::unique_lock lock(m_mutex);
            m_entered = true;
            m_cv.notify_all();
            m_cv.wait(lock, [this]() { return m_released; });
        }

        void BeforeSeal() noexcept override
        {
            {
                std::lock_guard lock(m_mutex);
                m_sealEntered = true;
            }
            m_cv.notify_all();
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

        [[nodiscard]] bool WaitUntilSealEntered(
            std::chrono::milliseconds timeout)
        {
            std::unique_lock lock(m_mutex);
            return m_cv.wait_for(lock,
                                 timeout,
                                 [this]() { return m_sealEntered; });
        }

    private:
        RenderPublicationPath m_path;
        std::mutex m_mutex;
        std::condition_variable m_cv;
        bool m_entered = false;
        bool m_released = false;
        bool m_sealEntered = false;
    };

    class CompletionSignal final : public NonMovable
    {
    public:
        void Complete()
        {
            {
                std::lock_guard lock(m_mutex);
                m_complete = true;
            }
            m_cv.notify_all();
        }

        [[nodiscard]] bool Wait(std::chrono::milliseconds timeout)
        {
            std::unique_lock lock(m_mutex);
            return m_cv.wait_for(lock,
                                 timeout,
                                 [this]() { return m_complete; });
        }

    private:
        std::mutex m_mutex;
        std::condition_variable m_cv;
        bool m_complete = false;
    };

    class BlockingWaitPredicateHook final : public IRenderWaitHook,
                                            public NonMovable
    {
    public:
        void AfterFalseWaitPredicate() noexcept override
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

    size_t CountOccurrences(const std::string& value,
                            const std::string& needle)
    {
        size_t count = 0;
        size_t offset = 0;
        while ((offset = value.find(needle, offset)) != std::string::npos)
        {
            ++count;
            offset += needle.size();
        }
        return count;
    }

    struct PendingGatewayWork
    {
        RenderResourceHandle handle;
        ResourceUploadRequestRef request;
    };

    PendingGatewayWork PrepareGatewayWork(RenderThreadRuntime& runtime,
                                          uint64 assetValue)
    {
        const AssetId assetId{assetValue};
        const RenderResourceReserveResult reserve =
            runtime.ReserveResource(assetId, RenderResourceKind::Material);
        EXPECT_EQ(reserve.code, RenderResourceReserveCode::Reserved);

        ResourceUploadRequestCreateInfo requestInfo;
        requestInfo.sequence = 1;
        requestInfo.assetId = assetId;
        requestInfo.handle = reserve.handle;
        requestInfo.kind = RenderResourceKind::Material;
        requestInfo.payload = MaterialUploadPayload{};
        requestInfo.declaredPayloadBytes = 0;
        const ResourceUploadRequestCreateResult request =
            ResourceUploadRequest::Create(std::move(requestInfo));
        EXPECT_EQ(request.code, ResourceUploadRequestCreateCode::Created);
        return PendingGatewayWork{reserve.handle, request.request};
    }

    void ExpectTerminalPublicationSealed(RenderThreadRuntime& runtime,
                                         const PendingGatewayWork& work,
                                         uint64 nextAssetValue)
    {
        EXPECT_EQ(runtime.ReserveResource(
                      AssetId{nextAssetValue},
                      RenderResourceKind::Material)
                      .code,
                  RenderResourceReserveCode::ShuttingDown);
        EXPECT_EQ(runtime.TryEnqueueUpload(work.request).code,
                  RenderUploadEnqueueCode::ShuttingDown);
        EXPECT_EQ(runtime.RequestRelease(work.handle).code,
                  RenderReleaseCode::ShuttingDown);
        const RenderResourceStatus status =
            runtime.QueryResourceStatus(work.handle);
        EXPECT_EQ(status.code, RenderResourceStatusCode::Current);
        EXPECT_EQ(status.state, RenderResourcePublicState::Reserved);
        EXPECT_EQ(runtime.TryPublishFrame(MakePacket(4000)).code,
                  RenderFramePublishCode::ShuttingDown);
        EXPECT_EQ(runtime.RequestResize(MakeSurface(4000)).code,
                  RenderResizeCode::ShuttingDown);
        const RenderDiagnosticsSnapshot diagnostics =
            runtime.GetDiagnosticsSnapshot();
        EXPECT_EQ(diagnostics.frameTransport.currentUsage, 0U);
        EXPECT_EQ(diagnostics.uploadTransport.currentRequestUsage, 0U);
        EXPECT_EQ(diagnostics.releaseTransport.currentUsage, 0U);
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

    bool WaitForTransportHighWaterAndDrain(
        const RenderThreadRuntime& runtime)
    {
        const auto deadline =
            std::chrono::steady_clock::now() + RVX_TEST_TIMEOUT;
        while (std::chrono::steady_clock::now() < deadline)
        {
            const RenderDiagnosticsSnapshot diagnostics =
                runtime.GetDiagnosticsSnapshot();
            if (diagnostics.frameTransport.highWaterMark >= 1U &&
                diagnostics.frameTransport.currentUsage == 0U &&
                diagnostics.uploadTransport.requestHighWaterMark >= 1U &&
                diagnostics.uploadTransport.currentRequestUsage == 0U &&
                diagnostics.releaseTransport.highWaterMark >= 1U &&
                diagnostics.releaseTransport.currentUsage == 0U)
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

    TEST(RenderThreadRuntimeValidation,
         CompletedShutdownRemainsFirstWinnerAcrossRepeatedStop)
    {
        auto probe = std::make_shared<RenderFrameConsumerTestProbe>();
        RenderRuntimeConfig config;
        config.backendType = RHIBackendType::DX11;
        RenderThreadRuntime runtime(
            config,
            MakeSurface(),
            RenderExecutorKind::InlineTest,
            CreateInlineRenderExecutor(),
            CreateRecordingRenderFrameConsumer(probe));
        ASSERT_EQ(runtime.Start().code, RenderRuntimeCode::Running);

        const RenderShutdownResult first = runtime.Stop();
        const RenderShutdownResult second = runtime.Stop();

        EXPECT_EQ(first.code, RenderShutdownCode::Completed);
        EXPECT_EQ(second.code, RenderShutdownCode::Completed);
        EXPECT_EQ(runtime.GetLastShutdownResult().code,
                  RenderShutdownCode::Completed);
        EXPECT_EQ(second.terminalCause, RenderTerminalCause::NormalStop);
        EXPECT_EQ(second.teardownMode, RenderTeardownMode::NormalDrain);
        const RenderFailureDiagnostics failure =
            runtime.GetDiagnosticsSnapshot().lastFailure;
        EXPECT_FALSE(failure.available);
        EXPECT_EQ(failure.shutdown.code, RenderShutdownCode::None);
        EXPECT_TRUE(failure.context.empty());
    }

    TEST(RenderThreadRuntimeValidation,
         ConsumerValueDiagnosticsArePublishedAfterFrameConsumption)
    {
        auto probe = std::make_shared<RenderFrameConsumerTestProbe>();
        probe->frameDiagnostics.available = true;
        probe->frameDiagnostics.frameSequence = 7;
        probe->frameDiagnostics.rendered = true;
        probe->frameDiagnostics.renderGraphTotalPasses = 11;
        probe->frameDiagnostics.visibleObjectCount = 3;

        RenderRuntimeConfig config;
        config.backendType = RHIBackendType::DX11;
        RenderThreadRuntime runtime(
            config,
            MakeSurface(),
            RenderExecutorKind::InlineTest,
            CreateInlineRenderExecutor(),
            CreateRecordingRenderFrameConsumer(probe));
        ASSERT_EQ(runtime.Start().code, RenderRuntimeCode::Running);
        ASSERT_EQ(runtime.TryPublishFrame(MakePacket(7)).code,
                  RenderFramePublishCode::Accepted);

        const RenderDiagnosticsSnapshot diagnostics =
            runtime.GetDiagnosticsSnapshot();
        EXPECT_EQ(diagnostics.lastPresentedFrameSequence, 7U);
        EXPECT_TRUE(diagnostics.frameFeatures.available);
        EXPECT_EQ(diagnostics.frameFeatures.frameSequence, 7U);
        EXPECT_TRUE(diagnostics.frameFeatures.rendered);
        EXPECT_EQ(diagnostics.frameFeatures.renderGraphTotalPasses, 11U);
        EXPECT_EQ(diagnostics.frameFeatures.visibleObjectCount, 3U);
        EXPECT_EQ(runtime.Stop().code, RenderShutdownCode::Completed);
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
         StartupFailurePublishesNormalizedOwnerCleanupResult)
    {
        auto probe = std::make_shared<RenderFrameConsumerTestProbe>();
        probe->startupCode = RenderRuntimeCode::DeviceCreationFailed;
        probe->startupNativeError = 71U;
        probe->startupMessage = "device initialization failed";
        probe->shutdownCode = RenderShutdownCode::DeviceLost;
        probe->shutdownBackend = RHIBackendType::DX12;
        probe->shutdownNativeError = 812U;
        probe->shutdownMessage = "device lost during startup cleanup";
        RenderRuntimeConfig config;
        config.backendType = RHIBackendType::DX11;
        RenderThreadRuntime runtime(
            config,
            MakeSurface(),
            RenderExecutorKind::InlineTest,
            CreateInlineRenderExecutor(),
            CreateRecordingRenderFrameConsumer(probe));

        const RenderRuntimeResult startup = runtime.Start();
        const RenderShutdownResult cleanup =
            runtime.GetLastShutdownResult();

        EXPECT_EQ(startup.code,
                  RenderRuntimeCode::DeviceCreationFailed);
        EXPECT_EQ(startup.nativeError, 71U);
        EXPECT_EQ(cleanup.code, RenderShutdownCode::DeviceLost);
        EXPECT_EQ(cleanup.resultClass, RenderResultClass::RuntimeFatal);
        EXPECT_EQ(cleanup.lifecycle, RenderLifecycleState::Failed);
        EXPECT_EQ(cleanup.terminalCause,
                  RenderTerminalCause::DeviceLost);
        EXPECT_EQ(cleanup.teardownMode,
                  RenderTeardownMode::DeviceLostTeardown);
        EXPECT_EQ(cleanup.backend, RHIBackendType::DX12);
        EXPECT_EQ(cleanup.nativeError, 812U);
        EXPECT_EQ(cleanup.message,
                  "device lost during startup cleanup");
        const RenderFailureDiagnostics failure =
            runtime.GetDiagnosticsSnapshot().lastFailure;
        EXPECT_EQ(failure.runtime.code,
                  RenderRuntimeCode::DeviceCreationFailed);
        EXPECT_EQ(failure.shutdown.code, RenderShutdownCode::DeviceLost);
        EXPECT_EQ(runtime.GetLastRuntimeResult().code,
                  RenderRuntimeCode::DeviceCreationFailed);
    }

    TEST(RenderThreadRuntimeValidation,
         UnhandledCleanupFatalIsAtomicWithFailureDiagnostics)
    {
        auto probe = std::make_shared<RenderFrameConsumerTestProbe>();
        probe->shutdownCode = RenderShutdownCode::DeviceLost;
        probe->shutdownNativeError = 901U;
        probe->shutdownMessage = "unhandled cleanup device lost";
        probe->throwOnFrame = true;
        RenderRuntimeConfig config;
        config.backendType = RHIBackendType::DX11;
        RenderThreadRuntime runtime(
            config,
            MakeSurface(),
            RenderExecutorKind::InlineTest,
            CreateInlineRenderExecutor(),
            CreateRecordingRenderFrameConsumer(probe));
        ASSERT_EQ(runtime.Start().code, RenderRuntimeCode::Running);

        ASSERT_EQ(runtime.TryPublishFrame(MakePacket(900)).code,
                  RenderFramePublishCode::Accepted);

        const RenderShutdownResult shutdown =
            runtime.GetLastShutdownResult();
        const RenderFailureDiagnostics failure =
            runtime.GetDiagnosticsSnapshot().lastFailure;
        EXPECT_EQ(shutdown.code, RenderShutdownCode::DeviceLost);
        EXPECT_EQ(failure.runtime.code,
                  RenderRuntimeCode::UnhandledException);
        EXPECT_EQ(failure.shutdown.code, shutdown.code);
        EXPECT_EQ(failure.shutdown.nativeError, shutdown.nativeError);
        EXPECT_NE(failure.context.find(
                      "Unhandled exception escaped the render pump"),
                  std::string::npos);
        EXPECT_EQ(CountOccurrences(failure.context,
                                   probe->shutdownMessage),
                  1U);
    }

    TEST(RenderThreadRuntimeValidation,
         StartupTimeoutCleanupFatalIsAtomicWithFailureDiagnostics)
    {
        auto probe = std::make_shared<RenderFrameConsumerTestProbe>();
        probe->BlockStartup();
        probe->shutdownCode = RenderShutdownCode::TimedOut;
        probe->shutdownNativeError = 902U;
        probe->shutdownMessage = "startup timeout cleanup timed out";
        RenderRuntimeConfig config;
        config.backendType = RHIBackendType::DX11;
        config.startupWatchdog = 20ms;
        config.shutdownWatchdog = RVX_TEST_TIMEOUT;
        RenderThreadRuntime runtime(
            config,
            MakeSurface(),
            RenderExecutorKind::Dedicated,
            CreateDedicatedRenderExecutor(),
            CreateRecordingRenderFrameConsumer(probe));

        std::thread releaseStartup([&]() {
            (void)probe->WaitForEventCount(RenderRuntimeTestEvent::Started,
                                           1U,
                                           RVX_TEST_TIMEOUT);
            const auto deadline =
                std::chrono::steady_clock::now() + RVX_TEST_TIMEOUT;
            while (std::chrono::steady_clock::now() < deadline &&
                   runtime.GetLastRuntimeResult().code !=
                       RenderRuntimeCode::StartupTimedOut)
            {
                std::this_thread::yield();
            }
            probe->ReleaseStartup();
        });
        const RenderRuntimeResult start = runtime.Start();
        releaseStartup.join();

        ASSERT_EQ(start.code, RenderRuntimeCode::StartupTimedOut);
        const RenderShutdownResult shutdown =
            runtime.GetLastShutdownResult();
        const RenderFailureDiagnostics failure =
            runtime.GetDiagnosticsSnapshot().lastFailure;
        EXPECT_EQ(shutdown.code, RenderShutdownCode::TimedOut);
        EXPECT_EQ(failure.runtime.code,
                  RenderRuntimeCode::StartupTimedOut);
        EXPECT_EQ(failure.shutdown.code, shutdown.code);
        EXPECT_EQ(failure.shutdown.nativeError, shutdown.nativeError);
        EXPECT_NE(failure.context.find(
                      "Render runtime startup watchdog expired"),
                  std::string::npos);
        EXPECT_EQ(CountOccurrences(failure.context,
                                   probe->shutdownMessage),
                  1U);
    }

    TEST(RenderThreadRuntimeValidation,
         RuntimeFatalCleanupIsAtomicWithFailureDiagnostics)
    {
        auto probe = std::make_shared<RenderFrameConsumerTestProbe>();
        probe->frameCode = RenderRuntimeCode::DeviceLost;
        probe->frameMessage = "frame reported device lost";
        probe->shutdownCode = RenderShutdownCode::TimedOut;
        probe->shutdownNativeError = 903U;
        probe->shutdownMessage = "device-lost cleanup timed out";
        RenderRuntimeConfig config;
        config.backendType = RHIBackendType::DX11;
        RenderThreadRuntime runtime(
            config,
            MakeSurface(),
            RenderExecutorKind::InlineTest,
            CreateInlineRenderExecutor(),
            CreateRecordingRenderFrameConsumer(probe));
        ASSERT_EQ(runtime.Start().code, RenderRuntimeCode::Running);

        ASSERT_EQ(runtime.TryPublishFrame(MakePacket(901)).code,
                  RenderFramePublishCode::Accepted);

        const RenderShutdownResult shutdown =
            runtime.GetLastShutdownResult();
        const RenderFailureDiagnostics failure =
            runtime.GetDiagnosticsSnapshot().lastFailure;
        EXPECT_EQ(shutdown.code, RenderShutdownCode::TimedOut);
        EXPECT_EQ(failure.runtime.code, RenderRuntimeCode::DeviceLost);
        EXPECT_EQ(failure.shutdown.code, shutdown.code);
        EXPECT_EQ(failure.shutdown.nativeError, shutdown.nativeError);
        EXPECT_EQ(CountOccurrences(failure.context,
                                   probe->frameMessage),
                  1U);
        EXPECT_EQ(CountOccurrences(failure.context,
                                   probe->shutdownMessage),
                  1U);
    }

    TEST(RenderThreadRuntimeValidation,
         DedicatedBootstrapIsCoveredByRuntimeStartupWatchdog)
    {
        auto consumerProbe =
            std::make_shared<RenderFrameConsumerTestProbe>();
        auto bootstrapHook =
            std::make_shared<BlockingRuntimeBootstrapHook>();
        auto joinProbe = std::make_shared<RenderExecutorJoinTestProbe>();
        RenderRuntimeConfig config;
        config.backendType = RHIBackendType::DX11;
        config.startupWatchdog = 20ms;
        config.shutdownWatchdog = RVX_TEST_TIMEOUT;
        RenderThreadRuntime runtime(
            config,
            MakeSurface(),
            RenderExecutorKind::Dedicated,
            CreateJoinRecordingDedicatedExecutor(joinProbe, bootstrapHook),
            CreateRecordingRenderFrameConsumer(consumerProbe));

        RenderRuntimeResult startResult;
        CompletionSignal startCompletion;
        std::thread starter([&]() {
            startResult = runtime.Start();
            startCompletion.Complete();
        });

        ASSERT_TRUE(bootstrapHook->WaitUntilEntered(RVX_TEST_TIMEOUT));
        const auto terminalDeadline =
            std::chrono::steady_clock::now() + 250ms;
        bool terminalPublished = false;
        while (std::chrono::steady_clock::now() < terminalDeadline)
        {
            if (runtime.GetLastRuntimeResult().code ==
                RenderRuntimeCode::StartupTimedOut)
            {
                terminalPublished = true;
                break;
            }
            std::this_thread::yield();
        }
        EXPECT_TRUE(terminalPublished);
        EXPECT_FALSE(startCompletion.Wait(1ms));

        bootstrapHook->Release();
        starter.join();
        EXPECT_EQ(startResult.code, RenderRuntimeCode::StartupTimedOut);
        EXPECT_EQ(runtime.GetLastRuntimeResult().code,
                  RenderRuntimeCode::StartupTimedOut);
        EXPECT_EQ(joinProbe->GetCallCount(), 1U);
        EXPECT_EQ(joinProbe->GetLastCode(), RenderExecutorJoinCode::Joined);
        EXPECT_EQ(consumerProbe->GetShutdownThread(),
                  consumerProbe->GetDestructionThread());
        EXPECT_NE(consumerProbe->GetShutdownThread(),
                  std::this_thread::get_id());
    }

    TEST(RenderThreadRuntimeValidation,
         WakeCannotBeLostBetweenPredicateAndWaitRegistration)
    {
        auto probe = std::make_shared<RenderFrameConsumerTestProbe>();
        auto publicationHook =
            std::make_shared<BlockingPublicationHook>(
                RenderPublicationPath::Frame);
        auto waitHook = std::make_shared<BlockingWaitPredicateHook>();
        RenderRuntimeConfig config;
        config.backendType = RHIBackendType::DX11;
        RenderThreadRuntime runtime(
            config,
            MakeSurface(),
            RenderExecutorKind::Dedicated,
            CreateDedicatedRenderExecutor(),
            CreateRecordingRenderFrameConsumer(probe),
            nullptr,
            nullptr,
            publicationHook,
            waitHook);
        ASSERT_EQ(runtime.Start().code, RenderRuntimeCode::Running);

        const bool waitWindowEntered =
            waitHook->WaitUntilEntered(RVX_TEST_TIMEOUT);
        if (!waitWindowEntered)
        {
            waitHook->Release();
            EXPECT_EQ(runtime.Stop().code, RenderShutdownCode::Completed);
            FAIL() << "Render thread did not enter the guarded wait window";
            return;
        }
        RenderFramePublishResult publishResult;
        CompletionSignal publicationComplete;
        std::thread publisher([&]() {
            publishResult = runtime.TryPublishFrame(MakePacket(7401));
            publicationComplete.Complete();
        });
        const bool mutationEntered =
            publicationHook->WaitUntilEntered(RVX_TEST_TIMEOUT);
        if (!mutationEntered)
        {
            publicationHook->Release();
            waitHook->Release();
            publisher.join();
            EXPECT_EQ(runtime.Stop().code, RenderShutdownCode::Completed);
            FAIL() << "Publisher did not enter the guarded mutation window";
            return;
        }
        publicationHook->Release();
        const bool completedBeforeWaitRelease =
            publicationComplete.Wait(100ms);
        waitHook->Release();
        publisher.join();

        EXPECT_FALSE(completedBeforeWaitRelease);
        EXPECT_EQ(publishResult.code, RenderFramePublishCode::Accepted);
        EXPECT_TRUE(probe->WaitForEventCount(RenderRuntimeTestEvent::Frame,
                                             1U,
                                             RVX_TEST_TIMEOUT));
        EXPECT_EQ(runtime.Stop().code, RenderShutdownCode::Completed);
    }

    TEST(RenderThreadRuntimeValidation,
         TerminalSealWaitsForInFlightFramePublication)
    {
        auto probe = std::make_shared<RenderFrameConsumerTestProbe>();
        auto publicationHook = std::make_shared<BlockingPublicationHook>(
            RenderPublicationPath::Frame);
        RenderRuntimeConfig config;
        config.backendType = RHIBackendType::DX11;
        RenderThreadRuntime runtime(
            config,
            MakeSurface(),
            RenderExecutorKind::Dedicated,
            CreateDedicatedRenderExecutor(),
            CreateRecordingRenderFrameConsumer(probe),
            nullptr,
            nullptr,
            publicationHook);
        ASSERT_EQ(runtime.Start().code, RenderRuntimeCode::Running);

        RenderFramePublishResult publishResult;
        std::thread publisher([&]() {
            publishResult = runtime.TryPublishFrame(MakePacket(7001));
        });
        ASSERT_TRUE(publicationHook->WaitUntilEntered(RVX_TEST_TIMEOUT));

        RenderShutdownResult shutdown;
        CompletionSignal stopCompletion;
        std::thread stopper([&]() {
            shutdown = runtime.Stop();
            stopCompletion.Complete();
        });
        ASSERT_TRUE(publicationHook->WaitUntilSealEntered(RVX_TEST_TIMEOUT));
        EXPECT_FALSE(stopCompletion.Wait(100ms));

        publicationHook->Release();
        publisher.join();
        stopper.join();
        EXPECT_EQ(publishResult.code, RenderFramePublishCode::Accepted);
        EXPECT_EQ(shutdown.code, RenderShutdownCode::Completed);
        EXPECT_EQ(runtime.TryPublishFrame(MakePacket(7002)).code,
                  RenderFramePublishCode::ShuttingDown);
    }

    TEST(RenderThreadRuntimeValidation,
         TerminalSealWaitsForInFlightResizePublication)
    {
        auto probe = std::make_shared<RenderFrameConsumerTestProbe>();
        auto publicationHook = std::make_shared<BlockingPublicationHook>(
            RenderPublicationPath::Resize);
        RenderRuntimeConfig config;
        config.backendType = RHIBackendType::DX11;
        RenderThreadRuntime runtime(
            config,
            MakeSurface(),
            RenderExecutorKind::Dedicated,
            CreateDedicatedRenderExecutor(),
            CreateRecordingRenderFrameConsumer(probe),
            nullptr,
            nullptr,
            publicationHook);
        ASSERT_EQ(runtime.Start().code, RenderRuntimeCode::Running);

        RenderResizeResult resizeResult;
        std::thread publisher([&]() {
            resizeResult = runtime.RequestResize(MakeSurface(2, 80, 80));
        });
        ASSERT_TRUE(publicationHook->WaitUntilEntered(RVX_TEST_TIMEOUT));

        RenderShutdownResult shutdown;
        CompletionSignal stopCompletion;
        std::thread stopper([&]() {
            shutdown = runtime.Stop();
            stopCompletion.Complete();
        });
        ASSERT_TRUE(publicationHook->WaitUntilSealEntered(RVX_TEST_TIMEOUT));
        EXPECT_FALSE(stopCompletion.Wait(100ms));

        publicationHook->Release();
        publisher.join();
        stopper.join();
        EXPECT_EQ(resizeResult.code, RenderResizeCode::Accepted);
        EXPECT_EQ(shutdown.code, RenderShutdownCode::Completed);
        EXPECT_EQ(runtime.RequestResize(MakeSurface(3, 96, 96)).code,
                  RenderResizeCode::ShuttingDown);
    }

    TEST(RenderThreadRuntimeValidation,
         TerminalSealWaitsForInFlightUploadAndRetainsCpuPayload)
    {
        auto probe = std::make_shared<RenderFrameConsumerTestProbe>();
        auto publicationHook = std::make_shared<BlockingPublicationHook>(
            RenderPublicationPath::Upload);
        RenderRuntimeConfig config;
        config.backendType = RHIBackendType::DX11;
        auto runtime = std::make_unique<RenderThreadRuntime>(
            config,
            MakeSurface(),
            RenderExecutorKind::Dedicated,
            CreateDedicatedRenderExecutor(),
            CreateRecordingRenderFrameConsumer(probe),
            nullptr,
            nullptr,
            publicationHook);
        ASSERT_EQ(runtime->Start().code, RenderRuntimeCode::Running);
        PendingGatewayWork work = PrepareGatewayWork(*runtime, 7201);
        std::weak_ptr<const ResourceUploadRequest> payload = work.request;

        RenderUploadEnqueueResult uploadResult;
        std::thread publisher([&]() {
            uploadResult = runtime->TryEnqueueUpload(work.request);
        });
        ASSERT_TRUE(publicationHook->WaitUntilEntered(RVX_TEST_TIMEOUT));

        RenderShutdownResult shutdown;
        CompletionSignal stopCompletion;
        std::thread stopper([&]() {
            shutdown = runtime->Stop();
            stopCompletion.Complete();
        });
        ASSERT_TRUE(publicationHook->WaitUntilSealEntered(RVX_TEST_TIMEOUT));
        EXPECT_FALSE(stopCompletion.Wait(100ms));

        publicationHook->Release();
        publisher.join();
        stopper.join();
        EXPECT_EQ(uploadResult.code, RenderUploadEnqueueCode::Accepted);
        EXPECT_EQ(shutdown.code, RenderShutdownCode::Completed);
        EXPECT_EQ(runtime->TryEnqueueUpload(work.request).code,
                  RenderUploadEnqueueCode::ShuttingDown);
        work.request.reset();
        EXPECT_TRUE(payload.expired());
        runtime.reset();
        EXPECT_TRUE(payload.expired());
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
        const PendingGatewayWork work = PrepareGatewayWork(runtime, 5001);

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
        ExpectTerminalPublicationSealed(runtime, work, 5002);
    }

    TEST(RenderThreadRuntimeValidation,
         StartupTimeoutWinsPreAcknowledgementPublicationWindow)
    {
        auto consumerProbe =
            std::make_shared<RenderFrameConsumerTestProbe>();
        auto lifecycleHook =
            std::make_shared<RenderRuntimeLifecycleTestHook>();
        auto joinProbe = std::make_shared<RenderExecutorJoinTestProbe>();
        RenderRuntimeConfig config;
        config.backendType = RHIBackendType::DX11;
        config.startupWatchdog = 50ms;
        config.shutdownWatchdog = RVX_TEST_TIMEOUT;
        RenderThreadRuntime runtime(
            config,
            MakeSurface(),
            RenderExecutorKind::Dedicated,
            CreateJoinRecordingDedicatedExecutor(joinProbe),
            CreateRecordingRenderFrameConsumer(consumerProbe),
            lifecycleHook);

        std::atomic<bool> hookEntered = false;
        std::atomic<bool> timeoutPublishedBeforeRelease = false;
        std::thread releasePublication([&]() {
            hookEntered.store(
                lifecycleHook->WaitUntilEntered(RVX_TEST_TIMEOUT),
                std::memory_order_release);
            const auto deadline =
                std::chrono::steady_clock::now() + 1s;
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (runtime.GetLastRuntimeResult().code ==
                    RenderRuntimeCode::StartupTimedOut)
                {
                    timeoutPublishedBeforeRelease.store(
                        true, std::memory_order_release);
                    break;
                }
                std::this_thread::yield();
            }
            lifecycleHook->Release();
        });

        const auto startTime = std::chrono::steady_clock::now();
        const RenderRuntimeResult result = runtime.Start();
        const auto elapsed =
            std::chrono::steady_clock::now() - startTime;
        releasePublication.join();
        if (result.code == RenderRuntimeCode::Running)
        {
            (void)runtime.Stop();
        }

        EXPECT_TRUE(hookEntered.load(std::memory_order_acquire));
        EXPECT_TRUE(timeoutPublishedBeforeRelease.load(
            std::memory_order_acquire));
        EXPECT_LT(elapsed, 500ms);
        EXPECT_EQ(result.code, RenderRuntimeCode::StartupTimedOut);
        EXPECT_EQ(runtime.GetLastRuntimeResult().code,
                  RenderRuntimeCode::StartupTimedOut);
        EXPECT_EQ(runtime.GetDiagnosticsSnapshot().lifecycle,
                  RenderLifecycleState::Failed);
        EXPECT_EQ(joinProbe->GetCallCount(), 1U);
        EXPECT_EQ(joinProbe->GetLastCode(),
                  RenderExecutorJoinCode::Joined);
        EXPECT_EQ(consumerProbe->GetShutdownThread(),
                  consumerProbe->GetStartupThread());
    }

    TEST(RenderThreadRuntimeValidation,
         StartupJoinFailureInvokesFatalPolicyAfterDiagnostics)
    {
        auto consumerProbe =
            std::make_shared<RenderFrameConsumerTestProbe>();
        consumerProbe->startupCode =
            RenderRuntimeCode::DeviceCreationFailed;
        auto joinProbe = std::make_shared<RenderExecutorJoinTestProbe>();
        auto fatalPolicy =
            std::make_shared<RenderFatalPolicyTestProbe>();
        RenderRuntimeConfig config;
        config.backendType = RHIBackendType::DX11;
        RenderThreadRuntime runtime(
            config,
            MakeSurface(),
            RenderExecutorKind::InlineTest,
            CreateSequencedJoinInlineRenderExecutor(
                {RenderExecutorJoinCode::TimedOut,
                 RenderExecutorJoinCode::Joined},
                joinProbe),
            CreateRecordingRenderFrameConsumer(consumerProbe),
            nullptr,
            fatalPolicy);

        bool intercepted = false;
        try
        {
            (void)runtime.Start();
        }
        catch (const RenderFatalPolicyIntercept&)
        {
            intercepted = true;
        }

        EXPECT_TRUE(intercepted);
        EXPECT_EQ(fatalPolicy->GetCallCount(), 1U);
        EXPECT_EQ(joinProbe->GetCallCount(), 1U);
        EXPECT_EQ(runtime.GetLastRuntimeResult().code,
                  RenderRuntimeCode::DeviceCreationFailed);
        EXPECT_EQ(runtime.GetLastShutdownResult().code,
                  RenderShutdownCode::TimedOut);
        const RenderDiagnosticsSnapshot fatalDiagnostics =
            fatalPolicy->GetDiagnostics();
        EXPECT_EQ(fatalDiagnostics.lastFailure.runtime.code,
                  RenderRuntimeCode::DeviceCreationFailed);
        EXPECT_EQ(fatalDiagnostics.lastFailure.shutdown.code,
                  RenderShutdownCode::TimedOut);

        const RenderShutdownResult retry = runtime.Stop();
        EXPECT_EQ(joinProbe->GetCallCount(), 2U);
        EXPECT_EQ(retry.code, RenderShutdownCode::TimedOut);
        EXPECT_EQ(runtime.GetLastShutdownResult().code,
                  RenderShutdownCode::TimedOut);
    }

    TEST(RenderThreadRuntimeValidation,
         StartupPublicationWindowUsesBoundedFatalPolicy)
    {
        auto consumerProbe =
            std::make_shared<RenderFrameConsumerTestProbe>();
        auto publicationHook =
            std::make_shared<RenderRuntimePublicationTestHook>();
        auto fatalPolicy =
            std::make_shared<RenderFatalPolicyTestProbe>();
        RenderRuntimeConfig config;
        config.backendType = RHIBackendType::DX11;
        config.startupWatchdog = 20ms;
        config.shutdownWatchdog = 50ms;
        RenderThreadRuntime runtime(
            config,
            MakeSurface(),
            RenderExecutorKind::Dedicated,
            CreateDedicatedRenderExecutor(),
            CreateRecordingRenderFrameConsumer(consumerProbe),
            publicationHook,
            fatalPolicy);

        bool intercepted = false;
        try
        {
            (void)runtime.Start();
        }
        catch (const RenderFatalPolicyIntercept&)
        {
            intercepted = true;
        }

        ASSERT_TRUE(publicationHook->WaitUntilEntered(RVX_TEST_TIMEOUT));
        EXPECT_TRUE(intercepted);
        EXPECT_EQ(fatalPolicy->GetCallCount(), 1U);
        const RenderDiagnosticsSnapshot fatalDiagnostics =
            fatalPolicy->GetDiagnostics();
        EXPECT_EQ(fatalDiagnostics.lastFailure.runtime.code,
                  RenderRuntimeCode::StartupTimedOut);
        EXPECT_EQ(fatalDiagnostics.lastFailure.shutdown.code,
                  RenderShutdownCode::TimedOut);

        publicationHook->Release();
        ASSERT_TRUE(consumerProbe->WaitForEventCount(
            RenderRuntimeTestEvent::Shutdown, 1U, RVX_TEST_TIMEOUT));
        const RenderShutdownResult retry = runtime.Stop();
        EXPECT_EQ(retry.code, RenderShutdownCode::TimedOut);
        EXPECT_EQ(runtime.GetLastRuntimeResult().code,
                  RenderRuntimeCode::StartupTimedOut);
        EXPECT_EQ(runtime.GetLastShutdownResult().code,
                  RenderShutdownCode::TimedOut);
    }

    TEST(RenderThreadRuntimeValidation,
         StartupJoinRejectionInvokesFatalPolicyAfterDiagnostics)
    {
        auto consumerProbe =
            std::make_shared<RenderFrameConsumerTestProbe>();
        consumerProbe->startupCode =
            RenderRuntimeCode::DeviceCreationFailed;
        auto joinProbe = std::make_shared<RenderExecutorJoinTestProbe>();
        auto fatalPolicy =
            std::make_shared<RenderFatalPolicyTestProbe>();
        RenderRuntimeConfig config;
        config.backendType = RHIBackendType::DX11;
        RenderThreadRuntime runtime(
            config,
            MakeSurface(),
            RenderExecutorKind::InlineTest,
            CreateSequencedJoinInlineRenderExecutor(
                {RenderExecutorJoinCode::NotStarted,
                 RenderExecutorJoinCode::Joined},
                joinProbe),
            CreateRecordingRenderFrameConsumer(consumerProbe),
            nullptr,
            fatalPolicy);

        bool intercepted = false;
        try
        {
            (void)runtime.Start();
        }
        catch (const RenderFatalPolicyIntercept&)
        {
            intercepted = true;
        }

        EXPECT_TRUE(intercepted);
        EXPECT_EQ(fatalPolicy->GetCallCount(), 1U);
        EXPECT_EQ(joinProbe->GetCallCount(), 1U);
        EXPECT_EQ(runtime.GetLastRuntimeResult().code,
                  RenderRuntimeCode::DeviceCreationFailed);
        EXPECT_EQ(runtime.GetLastShutdownResult().code,
                  RenderShutdownCode::ExecutorJoinFailed);
        const RenderDiagnosticsSnapshot fatalDiagnostics =
            fatalPolicy->GetDiagnostics();
        EXPECT_EQ(fatalDiagnostics.lastFailure.runtime.code,
                  RenderRuntimeCode::DeviceCreationFailed);
        EXPECT_EQ(fatalDiagnostics.lastFailure.shutdown.code,
                  RenderShutdownCode::ExecutorJoinFailed);

        const RenderShutdownResult retry = runtime.Stop();
        EXPECT_EQ(joinProbe->GetCallCount(), 2U);
        EXPECT_EQ(retry.code, RenderShutdownCode::ExecutorJoinFailed);
        EXPECT_EQ(runtime.GetLastRuntimeResult().code,
                  RenderRuntimeCode::DeviceCreationFailed);
    }

    TEST(RenderThreadRuntimeValidation,
         NormalShutdownTimeoutInvokesFatalPolicyAndRetryPreservesFailure)
    {
        auto consumerProbe =
            std::make_shared<RenderFrameConsumerTestProbe>();
        auto joinProbe = std::make_shared<RenderExecutorJoinTestProbe>();
        auto fatalPolicy =
            std::make_shared<RenderFatalPolicyTestProbe>();
        RenderRuntimeConfig config;
        config.backendType = RHIBackendType::DX11;
        RenderThreadRuntime runtime(
            config,
            MakeSurface(),
            RenderExecutorKind::InlineTest,
            CreateSequencedJoinInlineRenderExecutor(
                {RenderExecutorJoinCode::TimedOut,
                 RenderExecutorJoinCode::Joined},
                joinProbe),
            CreateRecordingRenderFrameConsumer(consumerProbe),
            nullptr,
            fatalPolicy);
        ASSERT_EQ(runtime.Start().code, RenderRuntimeCode::Running);

        bool intercepted = false;
        try
        {
            (void)runtime.Stop();
        }
        catch (const RenderFatalPolicyIntercept&)
        {
            intercepted = true;
        }

        EXPECT_TRUE(intercepted);
        EXPECT_EQ(fatalPolicy->GetCallCount(), 1U);
        EXPECT_EQ(joinProbe->GetCallCount(), 1U);
        EXPECT_EQ(runtime.GetLastRuntimeResult().code,
                  RenderRuntimeCode::ShutdownTimedOut);
        EXPECT_EQ(runtime.GetLastShutdownResult().code,
                  RenderShutdownCode::TimedOut);
        EXPECT_EQ(runtime.GetDiagnosticsSnapshot().lifecycle,
                  RenderLifecycleState::Failed);
        const RenderDiagnosticsSnapshot fatalDiagnostics =
            fatalPolicy->GetDiagnostics();
        EXPECT_EQ(fatalDiagnostics.lastFailure.runtime.code,
                  RenderRuntimeCode::ShutdownTimedOut);
        EXPECT_EQ(fatalDiagnostics.lastFailure.shutdown.code,
                  RenderShutdownCode::TimedOut);

        const RenderShutdownResult retry = runtime.Stop();
        EXPECT_EQ(joinProbe->GetCallCount(), 2U);
        EXPECT_EQ(retry.code, RenderShutdownCode::TimedOut);
        EXPECT_EQ(runtime.GetLastShutdownResult().code,
                  RenderShutdownCode::TimedOut);
    }

    TEST(RenderThreadRuntimeValidation,
         NormalJoinRejectionInvokesFatalPolicyAfterDiagnostics)
    {
        auto consumerProbe =
            std::make_shared<RenderFrameConsumerTestProbe>();
        auto joinProbe = std::make_shared<RenderExecutorJoinTestProbe>();
        auto fatalPolicy =
            std::make_shared<RenderFatalPolicyTestProbe>();
        RenderRuntimeConfig config;
        config.backendType = RHIBackendType::DX11;
        RenderThreadRuntime runtime(
            config,
            MakeSurface(),
            RenderExecutorKind::InlineTest,
            CreateSequencedJoinInlineRenderExecutor(
                {RenderExecutorJoinCode::NotStarted,
                 RenderExecutorJoinCode::Joined},
                joinProbe),
            CreateRecordingRenderFrameConsumer(consumerProbe),
            nullptr,
            fatalPolicy);
        ASSERT_EQ(runtime.Start().code, RenderRuntimeCode::Running);

        bool intercepted = false;
        try
        {
            (void)runtime.Stop();
        }
        catch (const RenderFatalPolicyIntercept&)
        {
            intercepted = true;
        }

        EXPECT_TRUE(intercepted);
        EXPECT_EQ(fatalPolicy->GetCallCount(), 1U);
        EXPECT_EQ(joinProbe->GetCallCount(), 1U);
        EXPECT_EQ(runtime.GetLastRuntimeResult().code,
                  RenderRuntimeCode::OwnershipViolation);
        EXPECT_EQ(runtime.GetLastShutdownResult().code,
                  RenderShutdownCode::ExecutorJoinFailed);
        const RenderDiagnosticsSnapshot fatalDiagnostics =
            fatalPolicy->GetDiagnostics();
        EXPECT_EQ(fatalDiagnostics.lastFailure.runtime.code,
                  RenderRuntimeCode::OwnershipViolation);
        EXPECT_EQ(fatalDiagnostics.lastFailure.shutdown.code,
                  RenderShutdownCode::ExecutorJoinFailed);

        const RenderShutdownResult retry = runtime.Stop();
        EXPECT_EQ(joinProbe->GetCallCount(), 2U);
        EXPECT_EQ(retry.code, RenderShutdownCode::ExecutorJoinFailed);
        EXPECT_EQ(runtime.GetLastRuntimeResult().code,
                  RenderRuntimeCode::OwnershipViolation);
    }

    TEST(RenderThreadRuntimeValidation,
         StartupTimeoutIdentitySurvivesLateUnhandledException)
    {
        auto consumerProbe =
            std::make_shared<RenderFrameConsumerTestProbe>();
        auto lifecycleHook =
            std::make_shared<RenderRuntimeLifecycleTestHook>();
        RenderRuntimeConfig config;
        config.backendType = RHIBackendType::DX11;
        config.startupWatchdog = 50ms;
        config.shutdownWatchdog = RVX_TEST_TIMEOUT;
        RenderThreadRuntime runtime(
            config,
            MakeSurface(),
            RenderExecutorKind::Dedicated,
            CreateDedicatedRenderExecutor(),
            CreateRecordingRenderFrameConsumer(consumerProbe),
            lifecycleHook);

        std::atomic<bool> timeoutObserved = false;
        std::atomic<bool> hookEntered = false;
        std::thread lateUnhandled([&]() {
            hookEntered.store(
                lifecycleHook->WaitUntilEntered(RVX_TEST_TIMEOUT),
                std::memory_order_release);
            const auto deadline =
                std::chrono::steady_clock::now() + RVX_TEST_TIMEOUT;
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (runtime.GetLastRuntimeResult().code ==
                    RenderRuntimeCode::StartupTimedOut)
                {
                    timeoutObserved.store(true, std::memory_order_release);
                    runtime.OnUnhandledExecutorException();
                    break;
                }
                std::this_thread::yield();
            }
            lifecycleHook->Release();
        });

        const RenderRuntimeResult result = runtime.Start();
        lateUnhandled.join();

        ASSERT_TRUE(hookEntered.load(std::memory_order_acquire));
        ASSERT_TRUE(timeoutObserved.load(std::memory_order_acquire));
        EXPECT_EQ(result.code, RenderRuntimeCode::StartupTimedOut);
        EXPECT_EQ(runtime.GetLastRuntimeResult().code,
                  RenderRuntimeCode::StartupTimedOut);
        EXPECT_EQ(runtime.GetDiagnosticsSnapshot().lastFailure.runtime.code,
                  RenderRuntimeCode::StartupTimedOut);
    }

    TEST(RenderThreadRuntimeValidation,
         TransportFatalFailsOnOwnerAndSealsPublication)
    {
        auto consumerProbe =
            std::make_shared<RenderFrameConsumerTestProbe>();
        RenderRuntimeConfig config;
        config.backendType = RHIBackendType::DX11;
        RenderThreadRuntime runtime(
            config,
            MakeSurface(),
            RenderExecutorKind::Dedicated,
            CreateDedicatedRenderExecutor(),
            CreateRecordingRenderFrameConsumer(consumerProbe));
        const PendingGatewayWork work = PrepareGatewayWork(runtime, 5101);
        ASSERT_EQ(runtime.Start().code, RenderRuntimeCode::Running);

        RenderThreadRuntimeTestAccess::ReportTransportFatal(
            runtime, "forced transport invariant failure");
        ASSERT_TRUE(consumerProbe->WaitForEventCount(
            RenderRuntimeTestEvent::Shutdown, 1U, RVX_TEST_TIMEOUT));
        (void)runtime.Stop();

        EXPECT_EQ(runtime.GetLastRuntimeResult().code,
                  RenderRuntimeCode::OwnershipViolation);
        EXPECT_EQ(runtime.GetLastRuntimeResult().lifecycle,
                  RenderLifecycleState::Failed);
        EXPECT_EQ(runtime.GetDiagnosticsSnapshot().lastFailure.runtime.code,
                  RenderRuntimeCode::OwnershipViolation);
        ExpectTerminalPublicationSealed(runtime, work, 5102);
    }

    TEST(RenderThreadRuntimeValidation,
         ReleaseInvariantFatalDoesNotReenterPublicationGate)
    {
        EXPECT_EXIT(
            {
                auto probe =
                    std::make_shared<RenderFrameConsumerTestProbe>();
                RenderRuntimeConfig config;
                config.backendType = RHIBackendType::DX11;
                RenderThreadRuntime runtime(
                    config,
                    MakeSurface(),
                    RenderExecutorKind::Dedicated,
                    CreateDedicatedRenderExecutor(),
                    CreateRecordingRenderFrameConsumer(probe));
                if (runtime.Start().code != RenderRuntimeCode::Running)
                {
                    std::_Exit(81);
                }
                const RenderResourceReserveResult reserve =
                    runtime.ReserveResource(
                        AssetId{5151}, RenderResourceKind::Texture);
                if (reserve.code != RenderResourceReserveCode::Reserved)
                {
                    std::_Exit(82);
                }
                RenderThreadRuntimeTestAccess::
                    ForceReleasePublicationInvariantFailure(runtime);

                std::thread watchdog([probe]() {
                    if (!probe->WaitForEventCount(
                            RenderRuntimeTestEvent::Shutdown,
                            1U,
                            RVX_TEST_TIMEOUT))
                    {
                        std::_Exit(89);
                    }
                });

                if (runtime.RequestRelease(reserve.handle).code !=
                    RenderReleaseCode::Accepted)
                {
                    std::_Exit(83);
                }
                watchdog.join();
                const RenderShutdownResult shutdown = runtime.Stop();
                if (shutdown.code != RenderShutdownCode::Completed ||
                    runtime.GetLastRuntimeResult().code !=
                        RenderRuntimeCode::OwnershipViolation ||
                    probe->GetShutdownThread() !=
                        probe->GetDestructionThread() ||
                    probe->GetShutdownThread() == std::this_thread::get_id())
                {
                    std::_Exit(84);
                }
                std::_Exit(0);
            },
            testing::ExitedWithCode(0),
            "");
    }

    TEST(RenderThreadRuntimeValidation,
         GatewayIsSealedAfterEveryTerminalStartupFailure)
    {
        {
            auto probe = std::make_shared<RenderFrameConsumerTestProbe>();
            RenderRuntimeConfig config;
            config.backendType = RHIBackendType::DX11;
            config.frameBuffering = 1;
            RenderThreadRuntime runtime(
                config,
                MakeSurface(),
                RenderExecutorKind::InlineTest,
                CreateInlineRenderExecutor(),
                CreateRecordingRenderFrameConsumer(probe));
            const PendingGatewayWork work =
                PrepareGatewayWork(runtime, 5201);
            ASSERT_EQ(runtime.Start().code,
                      RenderRuntimeCode::InvalidConfiguration);
            ExpectTerminalPublicationSealed(runtime, work, 5202);
        }
        {
            auto probe = std::make_shared<RenderFrameConsumerTestProbe>();
            RenderRuntimeConfig config;
            config.backendType = RHIBackendType::DX11;
            NativeSurfaceDesc invalidSurface = MakeSurface();
            invalidSurface.nativeWindow = 0;
            RenderThreadRuntime runtime(
                config,
                invalidSurface,
                RenderExecutorKind::InlineTest,
                CreateInlineRenderExecutor(),
                CreateRecordingRenderFrameConsumer(probe));
            const PendingGatewayWork work =
                PrepareGatewayWork(runtime, 5301);
            ASSERT_EQ(runtime.Start().code,
                      RenderRuntimeCode::InvalidSurface);
            ExpectTerminalPublicationSealed(runtime, work, 5302);
        }
        {
            auto probe = std::make_shared<RenderFrameConsumerTestProbe>();
            RenderRuntimeConfig config;
            config.backendType = RHIBackendType::DX11;
            RenderThreadRuntime runtime(
                config,
                MakeSurface(),
                RenderExecutorKind::Dedicated,
                CreateFailingRenderExecutor(77U),
                CreateRecordingRenderFrameConsumer(probe));
            const PendingGatewayWork work =
                PrepareGatewayWork(runtime, 5401);
            ASSERT_EQ(runtime.Start().code,
                      RenderRuntimeCode::ExecutorStartFailed);
            ExpectTerminalPublicationSealed(runtime, work, 5402);
        }
        {
            auto probe = std::make_shared<RenderFrameConsumerTestProbe>();
            probe->startupCode = RenderRuntimeCode::DeviceCreationFailed;
            RenderRuntimeConfig config;
            config.backendType = RHIBackendType::DX11;
            RenderThreadRuntime runtime(
                config,
                MakeSurface(),
                RenderExecutorKind::InlineTest,
                CreateInlineRenderExecutor(),
                CreateRecordingRenderFrameConsumer(probe));
            const PendingGatewayWork work =
                PrepareGatewayWork(runtime, 5501);
            ASSERT_EQ(runtime.Start().code,
                      RenderRuntimeCode::DeviceCreationFailed);
            ExpectTerminalPublicationSealed(runtime, work, 5502);
        }
    }

    TEST(RenderThreadRuntimeValidation,
         UnknownConsumerRuntimeCodeNormalizesToOwnershipViolation)
    {
        auto probe = std::make_shared<RenderFrameConsumerTestProbe>();
        probe->startupCode = static_cast<RenderRuntimeCode>(255);
        RenderRuntimeConfig config;
        config.backendType = RHIBackendType::DX11;
        RenderThreadRuntime runtime(
            config,
            MakeSurface(),
            RenderExecutorKind::InlineTest,
            CreateInlineRenderExecutor(),
            CreateRecordingRenderFrameConsumer(probe));

        const RenderRuntimeResult result = runtime.Start();

        EXPECT_EQ(result.code, RenderRuntimeCode::OwnershipViolation);
        EXPECT_EQ(result.resultClass, RenderResultClass::RuntimeFatal);
        EXPECT_EQ(result.lifecycle, RenderLifecycleState::Failed);
        EXPECT_EQ(result.terminalCause,
                  RenderTerminalCause::OwnershipViolation);
        EXPECT_NE(result.message.find("undeclared"), std::string::npos);
        EXPECT_TRUE(IsDeclaredRenderRuntimeCode(result.code));
    }

    TEST(RenderThreadRuntimeValidation,
         UnknownConfiguredExecutorAndBackendAreInvalidConfiguration)
    {
        {
            auto probe = std::make_shared<RenderFrameConsumerTestProbe>();
            RenderRuntimeConfig config;
            config.backendType = RHIBackendType::DX11;
            RenderThreadRuntime runtime(
                config,
                MakeSurface(),
                static_cast<RenderExecutorKind>(255),
                CreateInlineRenderExecutor(),
                CreateRecordingRenderFrameConsumer(probe));

            const RenderRuntimeResult result = runtime.Start();
            EXPECT_EQ(result.code,
                      RenderRuntimeCode::InvalidConfiguration);
            EXPECT_EQ(result.executor, RenderExecutorKind::None);
            EXPECT_EQ(result.backend, RHIBackendType::DX11);
            const RenderDiagnosticsSnapshot diagnostics =
                runtime.GetDiagnosticsSnapshot();
            EXPECT_EQ(diagnostics.executor, RenderExecutorKind::None);
            EXPECT_EQ(diagnostics.backend, RHIBackendType::DX11);
        }
        {
            auto probe = std::make_shared<RenderFrameConsumerTestProbe>();
            RenderRuntimeConfig config;
            config.backendType = static_cast<RHIBackendType>(255);
            RenderThreadRuntime runtime(
                config,
                MakeSurface(),
                RenderExecutorKind::InlineTest,
                CreateInlineRenderExecutor(),
                CreateRecordingRenderFrameConsumer(probe));

            const RenderRuntimeResult result = runtime.Start();
            EXPECT_EQ(result.code,
                      RenderRuntimeCode::InvalidConfiguration);
            EXPECT_EQ(result.executor, RenderExecutorKind::InlineTest);
            EXPECT_EQ(result.backend, RHIBackendType::None);
            const RenderDiagnosticsSnapshot diagnostics =
                runtime.GetDiagnosticsSnapshot();
            EXPECT_EQ(diagnostics.executor,
                      RenderExecutorKind::InlineTest);
            EXPECT_EQ(diagnostics.backend, RHIBackendType::None);
        }
    }

    TEST(RenderThreadRuntimeValidation,
         UnknownConsumerRuntimeBackendBecomesOwnershipViolation)
    {
        auto probe = std::make_shared<RenderFrameConsumerTestProbe>();
        probe->startupBackend = static_cast<RHIBackendType>(255);
        RenderRuntimeConfig config;
        config.backendType = RHIBackendType::DX11;
        RenderThreadRuntime runtime(
            config,
            MakeSurface(),
            RenderExecutorKind::InlineTest,
            CreateInlineRenderExecutor(),
            CreateRecordingRenderFrameConsumer(probe));

        const RenderRuntimeResult result = runtime.Start();

        EXPECT_EQ(result.code, RenderRuntimeCode::OwnershipViolation);
        EXPECT_EQ(result.resultClass, RenderResultClass::RuntimeFatal);
        EXPECT_EQ(result.backend, RHIBackendType::None);
        EXPECT_NE(result.message.find("backend"), std::string::npos);
        EXPECT_EQ(runtime.GetDiagnosticsSnapshot().backend,
                  RHIBackendType::DX11);
    }

    TEST(RenderThreadRuntimeValidation,
         UnknownConsumerShutdownCodeNormalizesToFatalShutdown)
    {
        auto probe = std::make_shared<RenderFrameConsumerTestProbe>();
        probe->shutdownCode = static_cast<RenderShutdownCode>(255);
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

        EXPECT_EQ(result.code, RenderShutdownCode::ExecutorJoinFailed);
        EXPECT_EQ(result.resultClass, RenderResultClass::RuntimeFatal);
        EXPECT_EQ(result.lifecycle, RenderLifecycleState::Failed);
        EXPECT_NE(result.message.find("undeclared"), std::string::npos);
        EXPECT_TRUE(IsDeclaredRenderShutdownCode(result.code));
        EXPECT_EQ(runtime.GetLastRuntimeResult().code,
                  RenderRuntimeCode::OwnershipViolation);
        EXPECT_EQ(runtime.GetDiagnosticsSnapshot().lastFailure.runtime.code,
                  RenderRuntimeCode::OwnershipViolation);
    }

    TEST(RenderThreadRuntimeValidation,
         UnknownConsumerShutdownBackendBecomesContractFatal)
    {
        auto probe = std::make_shared<RenderFrameConsumerTestProbe>();
        probe->shutdownBackend = static_cast<RHIBackendType>(255);
        probe->shutdownNativeError = 919U;
        probe->shutdownMessage = "cleanup returned unknown backend";
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

        EXPECT_EQ(result.code,
                  RenderShutdownCode::ExecutorJoinFailed);
        EXPECT_EQ(result.resultClass, RenderResultClass::RuntimeFatal);
        EXPECT_EQ(result.backend, RHIBackendType::None);
        EXPECT_EQ(result.nativeError, 919U);
        EXPECT_NE(result.message.find("backend"), std::string::npos);
        EXPECT_EQ(runtime.GetLastRuntimeResult().code,
                  RenderRuntimeCode::OwnershipViolation);
        EXPECT_EQ(runtime.GetLastRuntimeResult().backend,
                  RHIBackendType::None);
    }

    TEST(RenderThreadRuntimeValidation,
         ThrowingConsumerBecomesUnhandledAndTearsDownOnOwner)
    {
        EXPECT_EXIT(
            {
                auto probe =
                    std::make_shared<RenderFrameConsumerTestProbe>();
                probe->throwOnFrame = true;
                RenderRuntimeConfig config;
                config.backendType = RHIBackendType::DX11;
                RenderThreadRuntime runtime(
                    config,
                    MakeSurface(),
                    RenderExecutorKind::InlineTest,
                    CreateInlineRenderExecutor(),
                    CreateRecordingRenderFrameConsumer(probe));
                if (runtime.Start().code != RenderRuntimeCode::Running)
                {
                    std::_Exit(11);
                }
                if (runtime.TryPublishFrame(MakePacket(6001)).code !=
                    RenderFramePublishCode::Accepted)
                {
                    std::_Exit(12);
                }
                if (runtime.GetLastRuntimeResult().code !=
                    RenderRuntimeCode::UnhandledException)
                {
                    std::_Exit(13);
                }
                if (probe->GetEventCount(RenderRuntimeTestEvent::Shutdown) !=
                        1U ||
                    probe->GetShutdownThread() != probe->GetStartupThread() ||
                    probe->GetDestructionThread() !=
                        probe->GetStartupThread())
                {
                    std::_Exit(14);
                }
                if (runtime.ReserveResource(
                               AssetId{6002},
                               RenderResourceKind::Material)
                        .code != RenderResourceReserveCode::ShuttingDown)
                {
                    std::_Exit(15);
                }
                (void)runtime.Stop();
                std::_Exit(0);
            },
            testing::ExitedWithCode(0),
            "");
    }

    TEST(RenderThreadRuntimeValidation,
         ThrowingDedicatedConsumerSealsEveryPublicationPath)
    {
        auto probe = std::make_shared<RenderFrameConsumerTestProbe>();
        probe->throwOnFrame = true;
        RenderRuntimeConfig config;
        config.backendType = RHIBackendType::DX11;
        RenderThreadRuntime runtime(
            config,
            MakeSurface(),
            RenderExecutorKind::Dedicated,
            CreateDedicatedRenderExecutor(),
            CreateRecordingRenderFrameConsumer(probe));
        const PendingGatewayWork work = PrepareGatewayWork(runtime, 6003);
        ASSERT_EQ(runtime.Start().code, RenderRuntimeCode::Running);
        ASSERT_EQ(runtime.TryPublishFrame(MakePacket(6003)).code,
                  RenderFramePublishCode::Accepted);
        const bool shutdownObserved = probe->WaitForEventCount(
            RenderRuntimeTestEvent::Shutdown, 1U, RVX_TEST_TIMEOUT);
        const RenderDiagnosticsSnapshot timeoutDiagnostics =
            runtime.GetDiagnosticsSnapshot();
        ASSERT_TRUE(shutdownObserved)
            << "frame events="
            << probe->GetEventCount(RenderRuntimeTestEvent::Frame)
            << ", lifecycle="
            << static_cast<uint32>(timeoutDiagnostics.lifecycle)
            << ", runtimeCode="
            << static_cast<uint32>(
                   runtime.GetLastRuntimeResult().code)
            << ", pumpIterations="
            << timeoutDiagnostics.pumpIterationCount
            << ", lastAcquired="
            << timeoutDiagnostics.lastAcquiredFrameSequence;
        (void)runtime.Stop();

        EXPECT_EQ(runtime.GetLastRuntimeResult().code,
                  RenderRuntimeCode::UnhandledException);
        EXPECT_EQ(probe->GetShutdownThread(), probe->GetStartupThread());
        EXPECT_EQ(probe->GetDestructionThread(), probe->GetStartupThread());
        ExpectTerminalPublicationSealed(runtime, work, 6004);
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
        EXPECT_FALSE(subsystem.IsReady());
    }

    TEST(RenderThreadRuntimeValidation,
         ConfigureAfterEitherLegacyInitializeEntryIsRejected)
    {
        RenderConfig legacyConfig;
#if RVX_ENABLE_METAL
        legacyConfig.backendType = RHIBackendType::DX11;
#else
        legacyConfig.backendType = RHIBackendType::Metal;
#endif
        legacyConfig.autoBindWindow = false;

        {
            RenderSubsystem subsystem;
            subsystem.SetConfig(legacyConfig);
            subsystem.Initialize();
            EXPECT_THROW(
                subsystem.Configure(RenderRuntimeConfig{}, MakeSurface()),
                std::logic_error);
            EXPECT_FALSE(subsystem.IsReady());
        }
        {
            RenderSubsystem subsystem;
            subsystem.Initialize(legacyConfig);
            EXPECT_THROW(
                subsystem.Configure(RenderRuntimeConfig{}, MakeSurface()),
                std::logic_error);
            EXPECT_FALSE(subsystem.IsReady());
        }
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

    TEST(RenderThreadRuntimeValidation,
         RuntimeReadinessUsesAcknowledgedRunningValueState)
    {
        auto probe = std::make_shared<RenderFrameConsumerTestProbe>();
        RenderRuntimeConfig config;
        config.backendType = RHIBackendType::DX11;
        RenderThreadRuntime runtime(
            config,
            MakeSurface(),
            RenderExecutorKind::InlineTest,
            CreateInlineRenderExecutor(),
            CreateRecordingRenderFrameConsumer(probe));
        ASSERT_EQ(runtime.Start().code, RenderRuntimeCode::Running);
        ASSERT_TRUE(runtime.IsReady());

        probe->frameCode =
            RenderRuntimeCode::RenderGraphValidationFailed;
        ASSERT_EQ(runtime.TryPublishFrame(MakePacket(7301)).code,
                  RenderFramePublishCode::Accepted);
        const RenderRuntimeResult frameResult =
            runtime.GetLastRuntimeResult();
        ASSERT_EQ(frameResult.code,
                  RenderRuntimeCode::RenderGraphValidationFailed);
        ASSERT_EQ(frameResult.lifecycle,
                  RenderLifecycleState::Running);
        EXPECT_TRUE(runtime.IsReady());
        const RenderDiagnosticsSnapshot rejectedDiagnostics =
            runtime.GetDiagnosticsSnapshot();
        EXPECT_EQ(rejectedDiagnostics.lastAppliedFrameSequence, 0U);
        EXPECT_EQ(rejectedDiagnostics.lastSubmittedFrameSequence, 0U);
        EXPECT_EQ(rejectedDiagnostics.lastPresentedFrameSequence, 0U);

        EXPECT_EQ(runtime.Stop().code, RenderShutdownCode::Completed);
        EXPECT_FALSE(runtime.IsReady());
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
        ASSERT_TRUE(WaitForSurfaceGeneration(runtime, 3U));
        EXPECT_GE(probe->GetEventCount(RenderRuntimeTestEvent::Poll), 2U);
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

    TEST(RenderThreadRuntimeValidation, UploadOnlyPumpNeverConsumesOrPresentsFrame)
    {
        auto probe = std::make_shared<RenderFrameConsumerTestProbe>();
        RenderRuntimeConfig config;
        config.backendType = RHIBackendType::DX11;
        RenderThreadRuntime runtime(
            config,
            MakeSurface(),
            RenderExecutorKind::InlineTest,
            CreateInlineRenderExecutor(),
            CreateRecordingRenderFrameConsumer(probe));
        ASSERT_EQ(runtime.Start().code, RenderRuntimeCode::Running);

        const PendingGatewayWork upload = PrepareGatewayWork(runtime, 5101);
        ASSERT_EQ(runtime.TryEnqueueUpload(upload.request).code,
                  RenderUploadEnqueueCode::Accepted);
        ASSERT_TRUE(probe->WaitForEventCount(RenderRuntimeTestEvent::Upload,
                                             1U,
                                             RVX_TEST_TIMEOUT));

        const RenderDiagnosticsSnapshot diagnostics =
            runtime.GetDiagnosticsSnapshot();
        EXPECT_EQ(probe->GetEventCount(RenderRuntimeTestEvent::Frame), 0U);
        EXPECT_EQ(diagnostics.lastAppliedFrameSequence, 0U);
        EXPECT_EQ(diagnostics.lastSubmittedFrameSequence, 0U);
        EXPECT_EQ(diagnostics.lastPresentedFrameSequence, 0U);
        EXPECT_EQ(runtime.Stop().code, RenderShutdownCode::Completed);
    }

    TEST(RenderThreadRuntimeValidation,
         DedicatedFastDrainPreservesTransportHighWaterDiagnostics)
    {
        auto probe = std::make_shared<RenderFrameConsumerTestProbe>();
        RenderRuntimeConfig config;
        config.backendType = RHIBackendType::DX11;
        RenderThreadRuntime runtime(config,
                                    MakeSurface(),
                                    RenderExecutorKind::Dedicated,
                                    CreateDedicatedRenderExecutor(),
                                    CreateRecordingRenderFrameConsumer(probe));
        ASSERT_EQ(runtime.Start().code, RenderRuntimeCode::Running);

        const PendingGatewayWork upload = PrepareGatewayWork(runtime, 6101);
        const RenderResourceReserveResult release =
            runtime.ReserveResource(AssetId{6102},
                                    RenderResourceKind::Texture);
        ASSERT_EQ(release.code, RenderResourceReserveCode::Reserved);

        ASSERT_EQ(runtime.TryPublishFrame(MakePacket(6101)).code,
                  RenderFramePublishCode::Accepted);
        ASSERT_EQ(runtime.TryEnqueueUpload(upload.request).code,
                  RenderUploadEnqueueCode::Accepted);
        ASSERT_EQ(runtime.RequestRelease(release.handle).code,
                  RenderReleaseCode::Accepted);

        ASSERT_TRUE(probe->WaitForEventCount(RenderRuntimeTestEvent::Frame,
                                             1U,
                                             RVX_TEST_TIMEOUT));
        ASSERT_TRUE(probe->WaitForEventCount(RenderRuntimeTestEvent::Upload,
                                             1U,
                                             RVX_TEST_TIMEOUT));
        ASSERT_TRUE(probe->WaitForEventCount(RenderRuntimeTestEvent::Release,
                                             1U,
                                             RVX_TEST_TIMEOUT));
        ASSERT_TRUE(WaitForTransportHighWaterAndDrain(runtime));

        const RenderDiagnosticsSnapshot diagnostics =
            runtime.GetDiagnosticsSnapshot();
        EXPECT_GE(diagnostics.frameTransport.highWaterMark, 1U);
        EXPECT_GE(diagnostics.uploadTransport.requestHighWaterMark, 1U);
        EXPECT_GE(diagnostics.releaseTransport.highWaterMark, 1U);
        EXPECT_EQ(runtime.Stop().code, RenderShutdownCode::Completed);
    }

    TEST(RenderThreadRuntimeValidation,
         StopJoinsBeforeQueuedCpuPayloadDestruction)
    {
        auto probe = std::make_shared<RenderFrameConsumerTestProbe>();
        probe->BlockFrames();
        auto joinProbe = std::make_shared<RenderExecutorJoinTestProbe>();
        RenderRuntimeConfig config;
        config.backendType = RHIBackendType::DX11;
        auto runtime = std::make_shared<RenderThreadRuntime>(
            config,
            MakeSurface(),
            RenderExecutorKind::Dedicated,
            CreateJoinRecordingDedicatedExecutor(joinProbe),
            CreateRecordingRenderFrameConsumer(probe));
        ASSERT_EQ(runtime->Start().code, RenderRuntimeCode::Running);
        ASSERT_EQ(runtime->TryPublishFrame(MakePacket(6201)).code,
                  RenderFramePublishCode::Accepted);
        ASSERT_TRUE(probe->WaitForEventCount(RenderRuntimeTestEvent::Frame,
                                             1U,
                                             RVX_TEST_TIMEOUT));

        PendingGatewayWork upload = PrepareGatewayWork(*runtime, 6201);
        std::weak_ptr<const ResourceUploadRequest> queuedPayload =
            upload.request;
        ASSERT_EQ(runtime->TryEnqueueUpload(upload.request).code,
                  RenderUploadEnqueueCode::Accepted);
        upload.request.reset();
        const RenderResourceReserveResult release =
            runtime->ReserveResource(AssetId{6202},
                                     RenderResourceKind::Texture);
        ASSERT_EQ(release.code, RenderResourceReserveCode::Reserved);
        ASSERT_EQ(runtime->RequestRelease(release.handle).code,
                  RenderReleaseCode::Accepted);
        ASSERT_EQ(runtime->TryPublishFrame(MakePacket(6202)).code,
                  RenderFramePublishCode::Accepted);
        ASSERT_FALSE(queuedPayload.expired());

        RenderShutdownResult shutdown;
        bool retainedAfterJoin = false;
        std::thread stopper(
            [heldRuntime = runtime,
             &shutdown,
             &retainedAfterJoin,
             queuedPayload]() mutable {
                shutdown = heldRuntime->Stop();
                retainedAfterJoin = !queuedPayload.expired();
            });
        ASSERT_TRUE(WaitForRuntimeLifecycle(
            *runtime,
            RenderLifecycleState::StopRequested));
        runtime.reset();
        probe->ReleaseFrames();
        stopper.join();

        EXPECT_EQ(shutdown.code, RenderShutdownCode::Completed);
        EXPECT_FALSE(retainedAfterJoin);
        EXPECT_TRUE(queuedPayload.expired());
        EXPECT_EQ(joinProbe->GetCallCount(), 1U);
        EXPECT_EQ(joinProbe->GetLastCode(),
                  RenderExecutorJoinCode::Joined);
        EXPECT_EQ(probe->GetEventCount(RenderRuntimeTestEvent::Upload), 0U);
        EXPECT_EQ(probe->GetEventCount(RenderRuntimeTestEvent::Release), 1U);
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

    TEST(RenderThreadRuntimeValidation,
         WatchdogDefaultsAndInjectedMonotonicClockAreStable)
    {
        const RenderRuntimeConfig defaults;
        EXPECT_EQ(defaults.startupWatchdog, 60s);
        EXPECT_EQ(defaults.shutdownWatchdog, 30s);

        auto clock = std::make_shared<FakeRenderMonotonicClock>();
        const IRenderMonotonicClock::TimePoint before = clock->Now();
        clock->Advance(125ms);
        EXPECT_EQ(clock->Now() - before, 125ms);

        auto probe = std::make_shared<RenderFrameConsumerTestProbe>();
        probe->BlockStartup();
        auto timeoutClock = std::make_shared<FakeRenderMonotonicClock>();
        timeoutClock->Advance(-10ms);
        RenderRuntimeConfig config;
        config.backendType = RHIBackendType::DX11;
        config.startupWatchdog = 1ms;
        config.shutdownWatchdog = 2s;
        RenderThreadRuntime runtime(
            config,
            MakeSurface(),
            RenderExecutorKind::Dedicated,
            CreateDedicatedRenderExecutor(),
            CreateRecordingRenderFrameConsumer(probe),
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            timeoutClock);
        std::thread startupRelease([probe]() {
            if (probe->WaitForEventCount(RenderRuntimeTestEvent::Started,
                                         1U,
                                         500ms))
            {
                probe->ReleaseStartup();
            }
        });
        EXPECT_EQ(runtime.Start().code, RenderRuntimeCode::StartupTimedOut);
        startupRelease.join();
    }

    TEST(RenderThreadRuntimeValidation,
         FatalDiagnosticsArtifactIsWrittenBeforeTerminationBoundary)
    {
        RenderDiagnosticsSnapshot snapshot;
        snapshot.publicationSequence = 41U;
        snapshot.lifecycle = RenderLifecycleState::Failed;
        snapshot.backend = RHIBackendType::Vulkan;
        snapshot.lastFailure.available = true;
        snapshot.lastFailure.runtime.code = RenderRuntimeCode::DeviceLost;
        snapshot.lastFailure.runtime.nativeError = 77U;
        snapshot.lastFailure.context =
            "device lost\nbefore \"join\"\\cleanup\t";

        const std::filesystem::path path =
            std::filesystem::temp_directory_path() /
            "rvx_render_runtime_fatal_diagnostics_test.json";
        ASSERT_TRUE(RenderDiagnosticsPublisher::SaveArtifact(
            snapshot, path.string()));
        std::ifstream file(path);
        const std::string content((std::istreambuf_iterator<char>(file)),
                                  std::istreambuf_iterator<char>());
        EXPECT_NE(content.find("RenderRuntimeFatalDiagnostics"),
                  std::string::npos);
        EXPECT_NE(content.find(
                      "device lost\\nbefore \\\"join\\\"\\\\cleanup\\t"),
                  std::string::npos);
        EXPECT_NE(content.find("\"runtimeNativeError\": 77"),
                  std::string::npos);
        std::error_code removeError;
        std::filesystem::remove(path, removeError);
    }

    TEST(RenderThreadRuntimeValidation,
         RuntimeFactoryConstructsAndDestroysConsumerOnOwnerThread)
    {
        auto probe = std::make_shared<RenderRuntimeFaultProbe>();
        RenderRuntimeConfig config;
        config.backendType = RHIBackendType::DX11;
        RenderThreadRuntime runtime(
            config,
            MakeSurface(),
            RenderExecutorKind::Dedicated,
            CreateDedicatedRenderExecutor(),
            CreateFaultPlanRuntimeFactory({}, probe));

        ASSERT_EQ(runtime.Start().code, RenderRuntimeCode::Running);
        EXPECT_EQ(runtime.Stop().code, RenderShutdownCode::Completed);
        EXPECT_EQ(probe->GetLiveObjectCount(), 0U);
        EXPECT_EQ(probe->GetConstructionThread(),
                  probe->GetDestructionThread());
        EXPECT_NE(probe->GetConstructionThread(), std::this_thread::get_id());
    }

    TEST(RenderThreadRuntimeValidation,
         StartupFaultMatrixIsTerminalSealedAndOwnerCleaned)
    {
        const std::array startupFaults = {
            RenderRuntimeFaultPoint::FactoryCreation,
            RenderRuntimeFaultPoint::DeviceCreation,
            RenderRuntimeFaultPoint::SurfaceCreation,
            RenderRuntimeFaultPoint::ContextCreation,
            RenderRuntimeFaultPoint::RendererCreation,
            RenderRuntimeFaultPoint::ResourceCreation,
        };

        for (RenderRuntimeFaultPoint point : startupFaults)
        {
            SCOPED_TRACE(static_cast<uint32>(point));
            auto probe = std::make_shared<RenderRuntimeFaultProbe>();
            RenderRuntimeConfig config;
            config.backendType = RHIBackendType::DX11;
            RenderThreadRuntime runtime(
                config,
                MakeSurface(),
                RenderExecutorKind::Dedicated,
                CreateDedicatedRenderExecutor(),
                CreateFaultPlanRuntimeFactory(
                    RenderRuntimeFaultPlan{point, 1U, 0x15000000U +
                                                         static_cast<uint32>(point)},
                    probe));

            const RenderRuntimeResult start = runtime.Start();
            EXPECT_EQ(start.lifecycle, RenderLifecycleState::Failed);
            EXPECT_EQ(start.resultClass, RenderResultClass::RuntimeFatal);
            EXPECT_EQ(runtime.TryPublishFrame(MakePacket(8800)).code,
                      RenderFramePublishCode::ShuttingDown);
            EXPECT_EQ(probe->GetLiveObjectCount(), 0U);
            if (point != RenderRuntimeFaultPoint::FactoryCreation)
            {
                EXPECT_EQ(probe->GetConstructionThread(),
                          probe->GetDestructionThread());
            }
        }
    }

    TEST(RenderThreadRuntimeValidation,
         DeviceLossFaultMatrixUsesDistinctTeardownAndSealsPublication)
    {
        const std::array frameFaults = {
            RenderRuntimeFaultPoint::Present,
            RenderRuntimeFaultPoint::DeviceLossBeforeSubmission,
            RenderRuntimeFaultPoint::DeviceLossInFlight,
        };

        for (RenderRuntimeFaultPoint point : frameFaults)
        {
            SCOPED_TRACE(static_cast<uint32>(point));
            auto probe = std::make_shared<RenderRuntimeFaultProbe>();
            RenderRuntimeConfig config;
            config.backendType = RHIBackendType::DX11;
            RenderThreadRuntime runtime(
                config,
                MakeSurface(),
                RenderExecutorKind::Dedicated,
                CreateDedicatedRenderExecutor(),
                CreateFaultPlanRuntimeFactory(
                    RenderRuntimeFaultPlan{point, 1U, 0xD1500000U +
                                                         static_cast<uint32>(point)},
                    probe));
            ASSERT_EQ(runtime.Start().code, RenderRuntimeCode::Running);
            ASSERT_EQ(runtime.TryPublishFrame(MakePacket(8900)).code,
                      RenderFramePublishCode::Accepted);
            ASSERT_TRUE(WaitForRuntimeLifecycle(
                runtime, RenderLifecycleState::Failed));
            EXPECT_EQ(runtime.GetLastRuntimeResult().code,
                      RenderRuntimeCode::DeviceLost);
            EXPECT_EQ(runtime.GetLastRuntimeResult().terminalCause,
                      RenderTerminalCause::DeviceLost);
            EXPECT_EQ(probe->GetShutdownMode(),
                      RenderTeardownMode::DeviceLostTeardown);
            EXPECT_EQ(runtime.TryPublishFrame(MakePacket(8901)).code,
                      RenderFramePublishCode::ShuttingDown);
            static_cast<void>(runtime.Stop());
            EXPECT_EQ(probe->GetLiveObjectCount(), 0U);
        }
    }

    TEST(RenderThreadRuntimeValidation,
         FrameExceptionAndShutdownDeviceLossDoNotEscapeOwnerThread)
    {
        {
            auto probe = std::make_shared<RenderRuntimeFaultProbe>();
            RenderRuntimeConfig config;
            config.backendType = RHIBackendType::DX11;
            RenderThreadRuntime runtime(
                config,
                MakeSurface(),
                RenderExecutorKind::Dedicated,
                CreateDedicatedRenderExecutor(),
                CreateFaultPlanRuntimeFactory(
                    RenderRuntimeFaultPlan{
                        RenderRuntimeFaultPoint::FrameException},
                    probe));
            ASSERT_EQ(runtime.Start().code, RenderRuntimeCode::Running);
            ASSERT_EQ(runtime.TryPublishFrame(MakePacket(9000)).code,
                      RenderFramePublishCode::Accepted);
            ASSERT_TRUE(WaitForRuntimeLifecycle(
                runtime, RenderLifecycleState::Failed));
            EXPECT_EQ(runtime.GetLastRuntimeResult().code,
                      RenderRuntimeCode::UnhandledException);
            static_cast<void>(runtime.Stop());
            EXPECT_EQ(probe->GetLiveObjectCount(), 0U);
        }

        {
            auto probe = std::make_shared<RenderRuntimeFaultProbe>();
            RenderRuntimeConfig config;
            config.backendType = RHIBackendType::DX11;
            RenderThreadRuntime runtime(
                config,
                MakeSurface(),
                RenderExecutorKind::Dedicated,
                CreateDedicatedRenderExecutor(),
                CreateFaultPlanRuntimeFactory(
                    RenderRuntimeFaultPlan{
                        RenderRuntimeFaultPoint::DeviceLossDuringShutdown},
                    probe));
            ASSERT_EQ(runtime.Start().code, RenderRuntimeCode::Running);
            EXPECT_EQ(runtime.Stop().code, RenderShutdownCode::DeviceLost);
            EXPECT_EQ(runtime.GetLastRuntimeResult().code,
                      RenderRuntimeCode::DeviceLost);
            EXPECT_EQ(probe->GetLiveObjectCount(), 0U);
        }
    }

    TEST(RenderThreadRuntimeValidation,
         ResizeUploadAndCompletionFaultsConvergeToDeviceLoss)
    {
        {
            auto probe = std::make_shared<RenderRuntimeFaultProbe>();
            RenderRuntimeConfig config;
            config.backendType = RHIBackendType::DX11;
            RenderThreadRuntime runtime(
                config,
                MakeSurface(),
                RenderExecutorKind::Dedicated,
                CreateDedicatedRenderExecutor(),
                CreateFaultPlanRuntimeFactory(
                    RenderRuntimeFaultPlan{RenderRuntimeFaultPoint::Resize},
                    probe));
            ASSERT_EQ(runtime.Start().code, RenderRuntimeCode::Running);
            ASSERT_EQ(runtime.RequestResize(MakeSurface(2, 96, 96)).code,
                      RenderResizeCode::Accepted);
            ASSERT_TRUE(WaitForRuntimeLifecycle(
                runtime, RenderLifecycleState::Failed));
            EXPECT_EQ(runtime.GetLastRuntimeResult().code,
                      RenderRuntimeCode::DeviceLost);
            static_cast<void>(runtime.Stop());
            EXPECT_EQ(probe->GetLiveObjectCount(), 0U);
        }

        {
            auto probe = std::make_shared<RenderRuntimeFaultProbe>();
            RenderRuntimeConfig config;
            config.backendType = RHIBackendType::DX11;
            RenderThreadRuntime runtime(
                config,
                MakeSurface(),
                RenderExecutorKind::Dedicated,
                CreateDedicatedRenderExecutor(),
                CreateFaultPlanRuntimeFactory(
                    RenderRuntimeFaultPlan{
                        RenderRuntimeFaultPoint::UploadSubmission},
                    probe));
            ASSERT_EQ(runtime.Start().code, RenderRuntimeCode::Running);
            PendingGatewayWork work = PrepareGatewayWork(runtime, 9100);
            ASSERT_EQ(runtime.TryEnqueueUpload(work.request).code,
                      RenderUploadEnqueueCode::Accepted);
            work.request.reset();
            ASSERT_TRUE(WaitForRuntimeLifecycle(
                runtime, RenderLifecycleState::Failed));
            EXPECT_EQ(runtime.GetLastRuntimeResult().code,
                      RenderRuntimeCode::DeviceLost);
            static_cast<void>(runtime.Stop());
            EXPECT_EQ(probe->GetLiveObjectCount(), 0U);
        }

        for (RenderRuntimeFaultPoint point : {
                 RenderRuntimeFaultPoint::FencePoll,
                 RenderRuntimeFaultPoint::FenceWait})
        {
            SCOPED_TRACE(static_cast<uint32>(point));
            auto probe = std::make_shared<RenderRuntimeFaultProbe>();
            RenderRuntimeConfig config;
            config.backendType = RHIBackendType::DX11;
            RenderThreadRuntime runtime(
                config,
                MakeSurface(),
                RenderExecutorKind::Dedicated,
                CreateDedicatedRenderExecutor(),
                CreateFaultPlanRuntimeFactory(
                    RenderRuntimeFaultPlan{point}, probe));
            const RenderRuntimeResult start = runtime.Start();
            EXPECT_TRUE(start.code == RenderRuntimeCode::Running ||
                        start.code == RenderRuntimeCode::DeviceLost);
            ASSERT_TRUE(WaitForRuntimeLifecycle(
                runtime, RenderLifecycleState::Failed));
            EXPECT_EQ(runtime.GetLastRuntimeResult().code,
                      RenderRuntimeCode::DeviceLost);
            static_cast<void>(runtime.Stop());
            EXPECT_EQ(probe->GetLiveObjectCount(), 0U);
        }
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
