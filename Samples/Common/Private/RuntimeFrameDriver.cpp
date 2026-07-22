/**
 * @file RuntimeFrameDriver.cpp
 * @brief RuntimeFrameDriver implementation.
 */

#include "Samples/RuntimeFrameDriver.h"

#include "Engine/Engine.h"
#include "Render/RenderSubsystem.h"

#include <thread>

namespace RVX
{
    RuntimeFrameDriver::RuntimeFrameDriver(
        Engine& engine,
        RenderSubsystem& render) noexcept
        : m_engine(engine), m_render(render)
    {
    }

    RenderDiagnosticsSnapshot RuntimeFrameDriver::TickOnce(
        float32 deltaTime)
    {
        m_engine.Tick(deltaTime);
        std::this_thread::yield();
        return m_render.GetDiagnosticsSnapshot();
    }

    RuntimeFrameWaitResult RuntimeFrameDriver::WaitFor(
        const RuntimeFrameWaitRequest& request)
    {
        RuntimeFrameWaitResult result;
        result.diagnostics = m_render.GetDiagnosticsSnapshot();
        if (HasReached(request, result.diagnostics))
        {
            result.code = RuntimeFrameWaitCode::Reached;
            return result;
        }

        const auto deadline =
            std::chrono::steady_clock::now() + request.timeout;
        while (result.ticks < request.maxTicks)
        {
            if (m_engine.ShouldShutdown())
            {
                result.code = RuntimeFrameWaitCode::EngineStopped;
                return result;
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                result.code = RuntimeFrameWaitCode::TimedOut;
                return result;
            }

            result.diagnostics = request.advanceEngine
                                     ? TickOnce(request.deltaTime)
                                     : m_render.GetDiagnosticsSnapshot();
            ++result.ticks;
            if (HasReached(request, result.diagnostics))
            {
                result.code = RuntimeFrameWaitCode::Reached;
                return result;
            }
            if (request.captureRequestId != 0 &&
                result.diagnostics.lastCapture.requestId ==
                    request.captureRequestId &&
                result.diagnostics.lastCapture.code !=
                    RenderFrameCaptureResultCode::None)
            {
                result.code = RuntimeFrameWaitCode::CaptureFailed;
                return result;
            }
            if (result.diagnostics.lifecycle ==
                    RenderLifecycleState::Failed ||
                (result.diagnostics.lastFailure.available &&
                 result.diagnostics.lastFailure.runtime.resultClass >=
                     RenderResultClass::FrameFatal))
            {
                result.code = RuntimeFrameWaitCode::RuntimeFailed;
                return result;
            }

            // Engine::Tick only publishes value-owned work. Give the dedicated
            // Render executor a bounded scheduling opportunity before producing
            // another frame so waiters do not overwhelm the latest-frame mailbox.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        result.code = RuntimeFrameWaitCode::TickBudgetExceeded;
        return result;
    }

    bool RuntimeFrameDriver::HasReached(
        const RuntimeFrameWaitRequest& request,
        const RenderDiagnosticsSnapshot& diagnostics) noexcept
    {
        const bool submitted =
            request.minimumSubmittedSequence == 0 ||
            diagnostics.lastSubmittedFrameSequence >=
                request.minimumSubmittedSequence;
        const bool presented =
            request.minimumPresentedSequence == 0 ||
            diagnostics.lastPresentedFrameSequence >=
                request.minimumPresentedSequence;
        const bool captured =
            request.captureRequestId == 0 ||
            (diagnostics.lastCapture.requestId == request.captureRequestId &&
             diagnostics.lastCapture.IsComplete());
        return submitted && presented && captured;
    }
} // namespace RVX
