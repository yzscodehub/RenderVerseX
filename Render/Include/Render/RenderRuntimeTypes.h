#pragma once

/**
 * @file RenderRuntimeTypes.h
 * @brief Public value-only render runtime configuration and result contracts.
 */

#include "Render/RenderTransportTypes.h"
#include "RenderContracts/RenderIdentity.h"
#include "RHI/RHIDefinitions.h"

#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>

namespace RVX
{
    enum class RenderExecutorKind : uint8
    {
        None = 0,
        Dedicated = 1,
        InlineTest = 2
    };

    enum class RenderLifecycleState : uint8
    {
        Stopped = 0,
        Starting = 1,
        Running = 2,
        StopRequested = 3,
        Draining = 4,
        Failed = 5
    };

    enum class RenderResultClass : uint8
    {
        Success = 0,
        ExpectedPressure = 1,
        RecoverableFrame = 2,
        FrameFatal = 3,
        RuntimeFatal = 4
    };

    enum class RenderFramePublishCode : uint8
    {
        Accepted = 0,
        ReplacedOlder = 1,
        InvalidPacket = 2,
        OutOfOrder = 3,
        ShuttingDown = 4,
        NotRunning = 5
    };

    struct RenderFramePublishResult
    {
        RenderFramePublishCode code = RenderFramePublishCode::InvalidPacket;
        RenderResultClass resultClass = RenderResultClass::RecoverableFrame;
        uint64 sequence = 0;
        uint64 replacedSequence = 0;
    };

    enum class RenderResizeCode : uint8
    {
        Accepted = 0,
        CoalescedOlder = 1,
        StaleGeneration = 2,
        InvalidSurface = 3,
        ShuttingDown = 4,
        NotRunning = 5
    };

    struct RenderResizeResult
    {
        RenderResizeCode code = RenderResizeCode::InvalidSurface;
        RenderResultClass resultClass = RenderResultClass::RecoverableFrame;
        uint64 generation = 0;
        uint64 replacedGeneration = 0;
    };

    enum class RenderRuntimeCode : uint8
    {
        None = 0,
        Running = 1,
        StopRequested = 2,
        Stopped = 3,
        InvalidConfiguration = 4,
        InvalidSurface = 5,
        ExecutorStartFailed = 6,
        DeviceCreationFailed = 7,
        SurfaceCreationFailed = 8,
        RenderGraphValidationFailed = 9,
        DeviceLost = 10,
        UnhandledException = 11,
        OwnershipViolation = 12,
        StartupTimedOut = 13,
        ShutdownTimedOut = 14
    };

    enum class RenderTerminalCause : uint8
    {
        None = 0,
        NormalStop = 1,
        StartupFailure = 2,
        DeviceLost = 3,
        UnhandledException = 4,
        OwnershipViolation = 5,
        WatchdogTimeout = 6,
        ExecutorFailure = 7
    };

    enum class RenderTeardownMode : uint8
    {
        None = 0,
        NormalDrain = 1,
        DeviceLostTeardown = 2,
        FatalTimeout = 3
    };

    struct RenderRuntimeResult
    {
        RenderRuntimeCode code = RenderRuntimeCode::None;
        RenderResultClass resultClass = RenderResultClass::Success;
        RenderLifecycleState lifecycle = RenderLifecycleState::Stopped;
        RenderExecutorKind executor = RenderExecutorKind::None;
        RenderTerminalCause terminalCause = RenderTerminalCause::None;
        RenderTeardownMode teardownMode = RenderTeardownMode::None;
        RHIBackendType backend = RHIBackendType::None;
        uint64 frameSequence = 0;
        uint64 requestSequence = 0;
        AssetId assetId{};
        RenderResourceHandle handle{};
        uint64 surfaceGeneration = 0;
        uint32 nativeError = 0;
        std::string message{};
    };

    enum class RenderShutdownCode : uint8
    {
        None = 0,
        Completed = 1,
        AlreadyStopped = 2,
        DeviceLost = 3,
        TimedOut = 4,
        ExecutorJoinFailed = 5
    };

    struct RenderShutdownResult
    {
        RenderShutdownCode code = RenderShutdownCode::None;
        RenderResultClass resultClass = RenderResultClass::Success;
        RenderLifecycleState lifecycle = RenderLifecycleState::Stopped;
        RenderTerminalCause terminalCause = RenderTerminalCause::None;
        RenderTeardownMode teardownMode = RenderTeardownMode::None;
        RHIBackendType backend = RHIBackendType::None;
        uint64 lastSubmittedFrameSequence = 0;
        uint64 surfaceGeneration = 0;
        uint32 nativeError = 0;
        std::string message{};
    };

    struct RenderRuntimeConfig
    {
        RHIBackendType backendType = RHIBackendType::Auto;
        bool enableValidation = true;
        bool enableGPUValidation = false;
        uint32 frameBuffering = 2;
        RenderTransportConfig transports{};
        RenderIterationBudgets iterationBudgets{};
        std::chrono::milliseconds startupWatchdog{60000};
        std::chrono::milliseconds shutdownWatchdog{30000};
    };

    /** @brief Structured exception raised only after runtime startup fails. */
    class RenderSubsystemInitializationError final : public std::runtime_error
    {
    public:
        explicit RenderSubsystemInitializationError(RenderRuntimeResult result)
            : std::runtime_error(result.message.empty()
                                     ? "Render subsystem initialization failed"
                                     : result.message),
              m_result(std::move(result))
        {
        }

        [[nodiscard]] const RenderRuntimeResult& GetResult() const noexcept
        {
            return m_result;
        }

    private:
        RenderRuntimeResult m_result;
    };

    [[nodiscard]] constexpr bool IsDeclaredRenderFramePublishCode(
        RenderFramePublishCode code) noexcept
    {
        return code >= RenderFramePublishCode::Accepted &&
               code <= RenderFramePublishCode::NotRunning;
    }

    [[nodiscard]] constexpr RenderResultClass ClassifyRenderFramePublishCode(
        RenderFramePublishCode code) noexcept
    {
        switch (code)
        {
            case RenderFramePublishCode::Accepted:
                return RenderResultClass::Success;
            case RenderFramePublishCode::ReplacedOlder:
            case RenderFramePublishCode::ShuttingDown:
            case RenderFramePublishCode::NotRunning:
                return RenderResultClass::ExpectedPressure;
            case RenderFramePublishCode::InvalidPacket:
            case RenderFramePublishCode::OutOfOrder:
                return RenderResultClass::RecoverableFrame;
        }
        return RenderResultClass::RecoverableFrame;
    }

    [[nodiscard]] constexpr bool IsDeclaredRenderResizeCode(
        RenderResizeCode code) noexcept
    {
        return code >= RenderResizeCode::Accepted &&
               code <= RenderResizeCode::NotRunning;
    }

    [[nodiscard]] constexpr RenderResultClass ClassifyRenderResizeCode(
        RenderResizeCode code) noexcept
    {
        switch (code)
        {
            case RenderResizeCode::Accepted:
                return RenderResultClass::Success;
            case RenderResizeCode::CoalescedOlder:
            case RenderResizeCode::ShuttingDown:
            case RenderResizeCode::NotRunning:
                return RenderResultClass::ExpectedPressure;
            case RenderResizeCode::StaleGeneration:
            case RenderResizeCode::InvalidSurface:
                return RenderResultClass::RecoverableFrame;
        }
        return RenderResultClass::RecoverableFrame;
    }

    [[nodiscard]] constexpr bool IsDeclaredRenderRuntimeCode(
        RenderRuntimeCode code) noexcept
    {
        return code >= RenderRuntimeCode::None &&
               code <= RenderRuntimeCode::ShutdownTimedOut;
    }

    [[nodiscard]] constexpr bool IsDeclaredRenderShutdownCode(
        RenderShutdownCode code) noexcept
    {
        return code >= RenderShutdownCode::None &&
               code <= RenderShutdownCode::ExecutorJoinFailed;
    }
} // namespace RVX
