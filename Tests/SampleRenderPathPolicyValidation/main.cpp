#include "Samples/SampleRenderPathPolicy.h"

#include <gtest/gtest.h>

namespace RVX
{
    namespace
    {
        SampleRenderDiagnostics MakeCompletedPolicyFrame(
            const char* requestedMode,
            const char* tier)
        {
            SampleRenderDiagnostics diagnostics;
            diagnostics.renderPolicyRequestAvailable = true;
            diagnostics.renderPolicyPlanAvailable = true;
            diagnostics.renderPolicyReportAvailable = true;
            diagnostics.renderPolicyRequestFrameSequence = 41;
            diagnostics.renderPolicyPlanFrameSequence = 41;
            diagnostics.renderPolicyReportFrameSequence = 41;
            diagnostics.completedPresentedFrameSequence = 41;
            diagnostics.renderPolicyRequestedMode = requestedMode;
            diagnostics.renderPolicyExecutionStatus = "Completed";
            diagnostics.renderPolicySelectedTier = tier;
            diagnostics.renderPolicyExecutedTier = tier;
            diagnostics.opaqueExecutionCompleted = true;
            return diagnostics;
        }
    } // namespace

    TEST(SampleRenderPathPolicyValidation,
         AppliesOnlyDocumentedModesAndLeavesInvalidInputUnchanged)
    {
        RenderGPUCullingSettings settings;
        settings.mode = RenderGPUDrivenMode::ForceEnabled;
        std::string error;

        EXPECT_TRUE(ApplySampleRenderPathPolicy(
            SampleRenderPath::Direct, settings, error));
        EXPECT_EQ(settings.mode, RenderGPUDrivenMode::ForceDisabled);

        EXPECT_FALSE(ApplySampleRenderPathPolicy(
            static_cast<SampleRenderPath>(0xff), settings, error));
        EXPECT_EQ(settings.mode, RenderGPUDrivenMode::ForceDisabled);
        EXPECT_FALSE(error.empty());
    }

    TEST(SampleRenderPathPolicyValidation,
         DirectQualificationRequiresTheExactPresentedPolicyFrame)
    {
        SampleRenderDiagnostics diagnostics =
            MakeCompletedPolicyFrame("ForceDisabled", "Direct");
        diagnostics.gpuDrivenOpaqueDirectDrawCount = 1;
        diagnostics.opaqueExecutedDrawCountAvailable = true;
        diagnostics.opaqueExecutedDrawCount = 1;

        EXPECT_TRUE(IsSampleRenderPathExecutionQualified(
            SampleRenderPath::Direct, diagnostics));

        diagnostics.renderPolicyReportFrameSequence = 40;
        EXPECT_FALSE(IsSampleRenderPathExecutionQualified(
            SampleRenderPath::Direct, diagnostics));
    }

    TEST(SampleRenderPathPolicyValidation,
         GPUDrivenQualificationRequiresExclusiveIndirectExecution)
    {
        SampleRenderDiagnostics diagnostics =
            MakeCompletedPolicyFrame("ForceEnabled", "GPUResidentScene");
        diagnostics.gpuDrivenEnabled = true;
        diagnostics.gpuDrivenGraphPassAdded = true;
        diagnostics.gpuDrivenGraphPassRecorded = true;
        diagnostics.gpuDrivenExecutionRecorded = true;
        diagnostics.gpuDrivenOpaqueIndirectRequested = true;
        diagnostics.gpuDrivenOpaqueIndirectEligible = true;
        diagnostics.gpuDrivenOpaqueIndirectSubmitted = true;
        diagnostics.gpuDrivenOpaqueIndirectBatchCount = 1;
        diagnostics.gpuDrivenOpaqueIndirectDrawUpperBound = 1;

        EXPECT_TRUE(IsSampleRenderPathExecutionQualified(
            SampleRenderPath::GPUDriven, diagnostics));

        diagnostics.gpuDrivenOpaqueDirectDrawCount = 1;
        EXPECT_FALSE(IsSampleRenderPathExecutionQualified(
            SampleRenderPath::GPUDriven, diagnostics));
    }

    TEST(SampleRenderPathPolicyValidation,
         AutoStillRequiresOneCompletedEngineOwnedExecutionTier)
    {
        SampleRenderDiagnostics diagnostics =
            MakeCompletedPolicyFrame("Auto", "Direct");
        diagnostics.gpuDrivenOpaqueDirectDrawCount = 1;
        diagnostics.opaqueExecutedDrawCountAvailable = true;
        diagnostics.opaqueExecutedDrawCount = 1;

        EXPECT_TRUE(IsSampleRenderPathExecutionQualified(
            SampleRenderPath::Auto, diagnostics));

        diagnostics.renderPolicyExecutionStatus = "Skipped";
        EXPECT_FALSE(IsSampleRenderPathExecutionQualified(
            SampleRenderPath::Auto, diagnostics));
    }
} // namespace RVX
