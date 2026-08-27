#pragma once

/** @file SampleRenderPathPolicy.h @brief Value-only sample render-path policy helpers. */

#include "RenderContracts/RenderFrameTypes.h"
#include "Samples/SampleFeatureReporter.h"
#include "Samples/SampleInfo.h"

#include <string>

namespace RVX
{
    struct SampleRenderDiagnostics;
    /**
     * @brief Apply one sample-facing path request to frame policy values.
     *
     * Samples may select a path, but policy resolution and execution remain
     * engine-owned. Invalid values leave the supplied settings unchanged.
     */
    [[nodiscard]] inline bool ApplySampleRenderPathPolicy(
        SampleRenderPath requestedPath,
        RenderGPUCullingSettings& gpuCulling,
        std::string& outError)
    {
        RenderGPUDrivenMode requestedMode = RenderGPUDrivenMode::Auto;
        switch (requestedPath)
        {
            case SampleRenderPath::Auto:
                requestedMode = RenderGPUDrivenMode::Auto;
                break;
            case SampleRenderPath::Direct:
                requestedMode = RenderGPUDrivenMode::ForceDisabled;
                break;
            case SampleRenderPath::GPUDriven:
                requestedMode = RenderGPUDrivenMode::ForceEnabled;
                break;
            default:
                outError = "Sample received an invalid render path";
                return false;
        }

        gpuCulling.mode = requestedMode;
        return true;
    }

    /** @brief Append the stable report evidence for one accepted path request. */
    inline void AppendSampleRenderPathPolicyReport(
        SampleRenderPath requestedPath,
        SampleFeatureReporter& reporter)
    {
        reporter.Enable("RenderPathPolicySelection");
        reporter.ResourceDiagnostic(
            "render path=" +
            std::string(GetSampleRenderPathName(requestedPath)));

        switch (requestedPath)
        {
            case SampleRenderPath::Direct:
                reporter.Enable("DirectPathRequested");
                break;
            case SampleRenderPath::GPUDriven:
                reporter.Enable("GPUDrivenPathRequested");
                break;
            case SampleRenderPath::Auto:
                reporter.Enable("AutomaticRenderPathRequested");
                break;
            default:
                break;
        }
    }

    /**
     * @brief Return whether one fully presented frame proves the requested path.
     *
     * The request, selected plan and execution report must all be for the
     * exact completed presentation. Direct and GPU-driven qualification then
     * require mutually exclusive execution evidence from the opaque lane.
     */
    [[nodiscard]] bool IsSampleRenderPathExecutionQualified(
        SampleRenderPath requestedPath,
        const SampleRenderDiagnostics& diagnostics);
} // namespace RVX
