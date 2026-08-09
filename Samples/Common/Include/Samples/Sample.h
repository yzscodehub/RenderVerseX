#pragma once

/** @file Sample.h @brief Backend-neutral sample scene lifecycle contract. */

#include "Core/Types.h"
#include "Samples/SampleInfo.h"

#include <string>

namespace RVX
{
    struct SampleContext;
    class SampleFeatureReporter;
    struct SampleRenderDiagnostics;

    /** @brief One independently testable scene hosted by SampleRunner. */
    class ISample
    {
    public:
        virtual ~ISample() = default;

        [[nodiscard]] virtual const SampleInfo& GetInfo() const noexcept = 0;
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
        virtual void AppendReport(SampleFeatureReporter& reporter) const = 0;
        virtual bool IsReady(const SampleRenderDiagnostics& diagnostics,
                             std::string& outPendingReason) const
        {
            static_cast<void>(diagnostics);
            static_cast<void>(outPendingReason);
            return true;
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
