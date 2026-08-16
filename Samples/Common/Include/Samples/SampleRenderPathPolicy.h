#pragma once

/** @file SampleRenderPathPolicy.h @brief Value-only sample render-path policy helpers. */

#include "RenderContracts/RenderFrameTypes.h"
#include "Samples/SampleCLI.h"
#include "Samples/SampleInfo.h"

#include <string>

namespace RVX
{
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
    [[nodiscard]] inline bool IsSampleRenderPathExecutionQualified(
        SampleRenderPath requestedPath,
        const SampleRenderDiagnostics& diagnostics)
    {
        const char* expectedMode = nullptr;
        switch (requestedPath)
        {
            case SampleRenderPath::Auto:
                expectedMode = "Auto";
                break;
            case SampleRenderPath::Direct:
                expectedMode = "ForceDisabled";
                break;
            case SampleRenderPath::GPUDriven:
                expectedMode = "ForceEnabled";
                break;
            default:
                return false;
        }

        const uint64 frameSequence = diagnostics.renderPolicyRequestFrameSequence;
        const bool completedFramePolicy =
            diagnostics.renderPolicyRequestAvailable &&
            diagnostics.renderPolicyPlanAvailable &&
            diagnostics.renderPolicyReportAvailable &&
            frameSequence != 0 &&
            frameSequence == diagnostics.renderPolicyPlanFrameSequence &&
            frameSequence == diagnostics.renderPolicyReportFrameSequence &&
            frameSequence == diagnostics.completedPresentedFrameSequence &&
            diagnostics.renderPolicyRequestedMode == expectedMode &&
            diagnostics.renderPolicyExecutionStatus == "Completed" &&
            diagnostics.renderPolicySelectedTier ==
                diagnostics.renderPolicyExecutedTier;
        if (!completedFramePolicy)
        {
            return false;
        }

        const bool directTier = diagnostics.renderPolicyExecutedTier == "Direct";
        const bool directExecution =
            directTier &&
            !diagnostics.gpuDrivenEnabled &&
            !diagnostics.gpuDrivenOpaqueIndirectRequested &&
            !diagnostics.gpuDrivenOpaqueIndirectEligible &&
            !diagnostics.gpuDrivenOpaqueIndirectSubmitted &&
            diagnostics.gpuDrivenOpaqueIndirectBatchCount == 0 &&
            diagnostics.gpuDrivenOpaqueDirectDrawCount > 0 &&
            diagnostics.opaqueExecutionCompleted &&
            diagnostics.opaqueExecutedDrawCountAvailable &&
            diagnostics.opaqueExecutedDrawCount > 0;

        const bool indirectTier =
            diagnostics.renderPolicyExecutedTier == "IndirectGrouped" ||
            diagnostics.renderPolicyExecutedTier == "GPUResidentScene";
        const bool indirectExecution =
            indirectTier &&
            diagnostics.gpuDrivenEnabled &&
            diagnostics.gpuDrivenGraphPassAdded &&
            diagnostics.gpuDrivenGraphPassRecorded &&
            diagnostics.gpuDrivenExecutionRecorded &&
            diagnostics.gpuDrivenOpaqueIndirectRequested &&
            diagnostics.gpuDrivenOpaqueIndirectEligible &&
            diagnostics.gpuDrivenOpaqueIndirectSubmitted &&
            diagnostics.gpuDrivenOpaqueIndirectBatchCount > 0 &&
            diagnostics.gpuDrivenOpaqueIndirectDrawUpperBound > 0 &&
            diagnostics.gpuDrivenOpaqueDirectDrawCount == 0 &&
            diagnostics.opaqueExecutionCompleted;

        switch (requestedPath)
        {
            case SampleRenderPath::Direct:
                return directExecution;
            case SampleRenderPath::GPUDriven:
                return indirectExecution;
            case SampleRenderPath::Auto:
                return directExecution || indirectExecution;
            default:
                return false;
        }
    }
} // namespace RVX
