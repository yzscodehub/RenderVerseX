#pragma once

/**
 * @file RuntimeFrameDriver.h
 * @brief Bounded value-only frame progress helper for runtime samples.
 */

#include "Core/Types.h"
#include "Render/RenderDiagnostics.h"

#include <chrono>

namespace RVX
{
    class Engine;
    class RenderSubsystem;

    enum class RuntimeFrameWaitCode : uint8
    {
        Reached = 0,
        EngineStopped = 1,
        RuntimeFailed = 2,
        TickBudgetExceeded = 3,
        TimedOut = 4,
        CaptureFailed = 5
    };

    struct RuntimeFrameWaitRequest
    {
        uint64 minimumSubmittedSequence = 0;
        uint64 minimumPresentedSequence = 0;
        uint64 captureRequestId = 0;
        uint32 maxTicks = 120;
        float32 deltaTime = 1.0f / 60.0f;
        std::chrono::milliseconds timeout{5000};
        bool advanceEngine = true;
    };

    struct RuntimeFrameWaitResult
    {
        RuntimeFrameWaitCode code =
            RuntimeFrameWaitCode::TickBudgetExceeded;
        uint32 ticks = 0;
        RenderDiagnosticsSnapshot diagnostics{};

        [[nodiscard]] bool Reached() const noexcept
        {
            return code == RuntimeFrameWaitCode::Reached;
        }
    };

    /** @brief Advances Engine and observes only immutable Render diagnostics. */
    class RuntimeFrameDriver final : public NonMovable
    {
    public:
        RuntimeFrameDriver(Engine& engine, RenderSubsystem& render) noexcept;

        [[nodiscard]] RenderDiagnosticsSnapshot TickOnce(float32 deltaTime);
        [[nodiscard]] RuntimeFrameWaitResult WaitFor(
            const RuntimeFrameWaitRequest& request);

    private:
        [[nodiscard]] static bool HasReached(
            const RuntimeFrameWaitRequest& request,
            const RenderDiagnosticsSnapshot& diagnostics) noexcept;

        Engine& m_engine;
        RenderSubsystem& m_render;
    };
} // namespace RVX
