#pragma once

/**
 * @file IRenderExecutor.h
 * @brief Internal shared execution contract for the render runtime.
 */

#include "Core/Types.h"

#include <chrono>

namespace RVX
{
    enum class RenderPumpDecision : uint8
    {
        Progressed = 0,
        Idle = 1,
        Stop = 2
    };

    enum class RenderExecutorStartCode : uint8
    {
        Started = 0,
        AlreadyStarted = 1,
        ThreadCreationFailed = 2
    };

    struct RenderExecutorStartResult
    {
        RenderExecutorStartCode code =
            RenderExecutorStartCode::ThreadCreationFailed;
        uint32 nativeError = 0;
    };

    enum class RenderExecutorJoinCode : uint8
    {
        Joined = 0,
        NotStarted = 1,
        TimedOut = 2
    };

    struct RenderExecutorJoinResult
    {
        RenderExecutorJoinCode code = RenderExecutorJoinCode::NotStarted;
    };

    class IRenderExecutorPump
    {
    public:
        virtual ~IRenderExecutorPump() = default;

        virtual RenderPumpDecision PumpOnce() noexcept = 0;
        virtual void WaitForWork() noexcept = 0;
        virtual void Wake() noexcept = 0;
        virtual void OnUnhandledExecutorException() noexcept = 0;
    };

    class IRenderExecutor
    {
    public:
        virtual ~IRenderExecutor() = default;

        virtual RenderExecutorStartResult Start(IRenderExecutorPump& pump) = 0;
        virtual void NotifyWork() noexcept = 0;
        virtual RenderExecutorJoinResult JoinUntil(
            std::chrono::steady_clock::time_point deadline) = 0;
    };
} // namespace RVX
