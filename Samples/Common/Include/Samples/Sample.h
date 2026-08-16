#pragma once

/** @file Sample.h @brief Backend-neutral sample scene lifecycle contract. */

#include "Core/Types.h"
#include "Samples/FrameworkAssessment.h"
#include "Samples/SampleInfo.h"
#include "Samples/SampleWorldRequirements.h"

#include <string>
#include <utility>

namespace RVX
{
    struct SampleContext;
    class SampleFeatureReporter;
    struct SampleRenderDiagnostics;

    enum class SampleReadinessState : uint8
    {
        Pending = 0,
        Ready,
        Failed
    };

    /** @brief Non-ambiguous asynchronous sample readiness result. */
    struct SampleReadiness
    {
        SampleReadinessState state = SampleReadinessState::Pending;
        std::string reason;

        [[nodiscard]] bool IsReady() const noexcept
        {
            return state == SampleReadinessState::Ready;
        }

        [[nodiscard]] bool IsFailed() const noexcept
        {
            return state == SampleReadinessState::Failed;
        }

        [[nodiscard]] static SampleReadiness Ready()
        {
            return {SampleReadinessState::Ready, {}};
        }

        [[nodiscard]] static SampleReadiness Pending(std::string reason)
        {
            return {SampleReadinessState::Pending, std::move(reason)};
        }

        [[nodiscard]] static SampleReadiness Failed(std::string reason)
        {
            return {SampleReadinessState::Failed, std::move(reason)};
        }
    };

    /** @brief One independently testable scene hosted by SampleRunner. */
    class ISample
    {
    public:
        virtual ~ISample() = default;

        [[nodiscard]] virtual const SampleInfo& GetInfo() const noexcept = 0;
        /** @brief Declare World requirements before Engine creates the World. */
        [[nodiscard]] virtual SampleWorldRequirements
            GetWorldRequirements() const
        {
            return {};
        }
        /** @brief Declare the framework contracts exercised by this sample. */
        [[nodiscard]] virtual SampleAssessmentContract
            GetAssessmentContract() const
        {
            return MakeDefaultSampleAssessmentContract();
        }
        virtual bool Setup(SampleContext& context, std::string& outError) = 0;
        virtual void Update(SampleContext& context, float deltaTime) = 0;
        virtual void OnInput(SampleContext& context) = 0;
        virtual void OnViewportResize(SampleContext& context,
                                      uint32 width,
                                      uint32 height)
        {
            static_cast<void>(context);
            static_cast<void>(width);
            static_cast<void>(height);
        }
        /**
         * @brief Put an interaction qualification into a deterministic capture state.
         *
         * Called once after the sustained interaction window and any surface
         * resize have completed, one frame before the capture is submitted.
         */
        virtual bool PrepareQualificationCapture(SampleContext& context,
                                                 std::string& outError)
        {
            static_cast<void>(context);
            static_cast<void>(outError);
            return true;
        }
        virtual void AppendReport(SampleFeatureReporter& reporter) const = 0;
        /**
         * @brief Observe one immutable, backend-neutral completed-frame snapshot.
         *
         * Called after Render diagnostics are projected into Sample-facing
         * values and before readiness is evaluated. Implementations may emit
         * assessment observations, but must not retain either reference.
         */
        virtual void ObserveDiagnostics(
            const SampleRenderDiagnostics& diagnostics,
            SampleAssessmentChannel& assessment)
        {
            static_cast<void>(diagnostics);
            static_cast<void>(assessment);
        }
        virtual SampleReadiness GetReadiness(
            const SampleRenderDiagnostics& diagnostics) const
        {
            static_cast<void>(diagnostics);
            return SampleReadiness::Ready();
        }
        /**
         * @brief Request one final presentation followed by a render-only drain.
         *
         * The host calls this only once all non-render terminal conditions are
         * already presentation-covered. Ordinary samples retain the existing
         * frame progression because the default is false.
         */
        [[nodiscard]] virtual bool ShouldBeginFinalRenderDrain(
            const SampleRenderDiagnostics& diagnostics) const
        {
            static_cast<void>(diagnostics);
            return false;
        }
        virtual bool ValidateResult(const SampleRenderDiagnostics& diagnostics,
                                    std::string& outError) const
        {
            static_cast<void>(diagnostics);
            static_cast<void>(outError);
            return true;
        }
        virtual void Shutdown(SampleContext& context) = 0;
    };
} // namespace RVX
