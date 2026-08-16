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

    RenderDiagnosticsSnapshot RuntimeFrameDriver::PumpRenderProgressOnce()
    {
        static_cast<void>(m_render.RequestCompletionPoll());
        // Completion belongs to the dedicated Render owner. Sleeping for one
        // bounded scheduling quantum preserves the value-only boundary and
        // leaves publication open for later ECS removal snapshots.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
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

            const EngineRenderRuntimeDiagnostics engineDiagnostics =
                m_engine.GetRenderRuntimeDiagnostics();
            switch (SelectTickAction(
                request, result.diagnostics, engineDiagnostics))
            {
                case TickAction::Full:
                    result.diagnostics = TickOnce(request.deltaTime);
                    break;
                case TickAction::Observe:
                    result.diagnostics = m_render.GetDiagnosticsSnapshot();
                    break;
                case TickAction::ProgressPoll:
                    result.diagnostics = PumpRenderProgressOnce();
                    break;
            }
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
                    RenderFrameCaptureResultCode::None &&
                result.diagnostics.lastCapture.code !=
                    RenderFrameCaptureResultCode::Completed)
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

            // Give the dedicated Render executor a bounded scheduling
            // opportunity before the next wait iteration.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        result.code = RuntimeFrameWaitCode::TickBudgetExceeded;
        return result;
    }

    RuntimeFrameDriver::TickAction RuntimeFrameDriver::SelectTickAction(
        const RuntimeFrameWaitRequest& request,
        const RenderDiagnosticsSnapshot& diagnostics,
        const EngineRenderRuntimeDiagnostics& engineDiagnostics) noexcept
    {
        if (HasReached(request, diagnostics))
        {
            return TickAction::Observe;
        }
        if (request.pumpRenderProgressOnly)
        {
            return TickAction::ProgressPoll;
        }
        if (!request.advanceEngine)
        {
            return TickAction::Observe;
        }
        const bool publicationStillRequired =
            request.minimumPublishedSequence != 0 &&
            diagnostics.lastPublishedFrameSequence <
                request.minimumPublishedSequence &&
            (!engineDiagnostics.available ||
             engineDiagnostics.requiredSceneFrameSequence <
                 request.minimumPublishedSequence);
        if (publicationStillRequired)
        {
            return TickAction::Full;
        }
        return TickAction::ProgressPoll;
    }

    bool RuntimeFrameDriver::HasReached(
        const RuntimeFrameWaitRequest& request,
        const RenderDiagnosticsSnapshot& diagnostics) noexcept
    {
        const bool published =
            request.minimumPublishedSequence == 0 ||
            diagnostics.lastPublishedFrameSequence >=
                request.minimumPublishedSequence;
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
        return published && submitted && presented && captured;
    }
} // namespace RVX
