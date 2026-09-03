/** @file SampleScenarioDiagnostics.cpp @brief Scenario diagnostic projection. */

#include "Samples/SampleScenarioDiagnostics.h"

#include "Samples/SampleCLI.h"

#include <algorithm>

namespace RVX
{
    SampleRasterScenarioDiagnostics ProjectSampleRasterScenarioDiagnostics(
        const SampleRenderDiagnostics& diagnostics)
    {
        SampleRasterScenarioDiagnostics result;
        result.visibleObjectCount = diagnostics.visibleObjectCount;
        result.textureIBLEnabled = diagnostics.textureIBLEnabled;
        result.opaqueExecutionCompleted =
            diagnostics.opaqueExecutionCompleted;
        result.opaqueExecutedDrawCountAvailable =
            diagnostics.opaqueExecutedDrawCountAvailable;
        result.opaqueExecutedDrawCount =
            diagnostics.opaqueExecutedDrawCount;
        result.opaqueInstancingExecutedPacketCount =
            diagnostics.opaqueInstancingExecutedPacketCount;
        result.opaqueInstancingSubmittedDrawCount =
            diagnostics.opaqueInstancingSubmittedDrawCount;
        result.opaqueMaterialBindingsAvailable =
            diagnostics.opaqueMaterialBindingsAvailable;
        result.opaqueMaterialBindingCount =
            diagnostics.opaqueMaterialBindingCount;
        result.opaqueMaterialFallbackBindingCount =
            diagnostics.opaqueMaterialFallbackBindingCount;
        result.opaqueMaterialTextureFlags =
            diagnostics.opaqueMaterialTextureFlags;
        result.opaqueMaterialFallbackTextureFlags =
            diagnostics.opaqueMaterialFallbackTextureFlags;
        result.presentedMaterialBindingsAvailable =
            diagnostics.presentedMaterialBindingsAvailable;
        result.presentedMaterialBindingsOverflow =
            diagnostics.presentedMaterialBindingsOverflow;
        result.presentedMaterialBindings.reserve(
            diagnostics.presentedMaterialBindings.size());
        for (const SamplePresentedMaterialBindingReceipt& receipt :
             diagnostics.presentedMaterialBindings)
        {
            result.presentedMaterialBindings.push_back(
                {receipt.parameterLayoutHash,
                 receipt.featureMask,
                 receipt.shadingModel,
                 receipt.surfacePathEligibility,
                 receipt.fallbackUsed,
                 receipt.frameSequence,
                 receipt.presentationSequence});
        }
        result.gpuDrivenPolicyDecisionAvailable =
            diagnostics.gpuDrivenPolicyDecisionAvailable;
        result.gpuDrivenRequestedMode =
            diagnostics.gpuDrivenRequestedMode;
        result.gpuDrivenPolicyReason =
            diagnostics.gpuDrivenPolicyReason;
        result.gpuDrivenEnabled = diagnostics.gpuDrivenEnabled;
        result.gpuDrivenGraphPassRecorded =
            diagnostics.gpuDrivenGraphPassRecorded;
        result.gpuDrivenExecutionRecorded =
            diagnostics.gpuDrivenExecutionRecorded;
        result.gpuDrivenOpaqueIndirectRequested =
            diagnostics.gpuDrivenOpaqueIndirectRequested;
        result.gpuDrivenOpaqueIndirectEligible =
            diagnostics.gpuDrivenOpaqueIndirectEligible;
        result.gpuDrivenOpaqueIndirectSubmitted =
            diagnostics.gpuDrivenOpaqueIndirectSubmitted;
        result.gpuDrivenOpaqueIndirectBatchCount =
            diagnostics.gpuDrivenOpaqueIndirectBatchCount;
        result.gpuDrivenOpaqueSubmittedSourcePacketCount =
            diagnostics.gpuDrivenOpaqueSubmittedSourcePacketCount;
        result.gpuDrivenOpaqueDirectDrawCount =
            diagnostics.gpuDrivenOpaqueDirectDrawCount;
        return result;
    }

    bool HasPresentedOpaqueStandardLitRasterEvidence(
        const SampleRasterScenarioDiagnostics& diagnostics) noexcept
    {
        if (!diagnostics.opaqueExecutionCompleted ||
            !diagnostics.presentedMaterialBindingsAvailable ||
            diagnostics.presentedMaterialBindingsOverflow ||
            diagnostics.presentedMaterialBindings.empty())
        {
            return false;
        }

        bool observedOpaqueStandardLit = false;
        for (const SamplePresentedMaterialBindingEvidence& receipt :
             diagnostics.presentedMaterialBindings)
        {
            if (!receipt.IsValid() || receipt.fallbackUsed ||
                receipt.frameSequence != receipt.presentationSequence)
            {
                return false;
            }
            observedOpaqueStandardLit |=
                receipt.shadingModel == MaterialShadingModel::StandardLit &&
                (receipt.surfacePathEligibility ==
                     SurfacePathEligibility::DeferredOpaque ||
                 receipt.surfacePathEligibility ==
                     SurfacePathEligibility::ForwardSpecialOpaque);
        }
        return observedOpaqueStandardLit;
    }

    bool HasPresentedMaterialFeatureEvidence(
        const SampleRasterScenarioDiagnostics& diagnostics,
        const uint64 requiredFeatureMask) noexcept
    {
        if (!HasPresentedOpaqueStandardLitRasterEvidence(diagnostics) ||
            requiredFeatureMask == 0)
        {
            return false;
        }

        uint64 observedMask = 0;
        for (const SamplePresentedMaterialBindingEvidence& receipt :
             diagnostics.presentedMaterialBindings)
        {
            if (receipt.shadingModel == MaterialShadingModel::StandardLit &&
                (receipt.surfacePathEligibility ==
                     SurfacePathEligibility::DeferredOpaque ||
                 receipt.surfacePathEligibility ==
                     SurfacePathEligibility::ForwardSpecialOpaque))
            {
                observedMask |= receipt.featureMask;
            }
        }
        return (observedMask & requiredFeatureMask) == requiredFeatureMask;
    }
} // namespace RVX
