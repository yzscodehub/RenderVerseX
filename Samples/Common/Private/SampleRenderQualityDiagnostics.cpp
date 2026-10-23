/**
 * @file SampleRenderQualityDiagnostics.cpp
 * @brief Renderer-backed predicates for the RenderQualityLab sample contract.
 */

#include "Samples/SampleRenderQualityDiagnostics.h"

#include "Samples/SampleCLI.h"
#include "Samples/SampleParticleVfxDiagnostics.h"
#include "Samples/SampleWaterSurfaceDiagnostics.h"

#include <algorithm>

namespace RVX
{
    bool IsSampleRenderQualityFrameAvailable(
        const SampleRenderDiagnostics& diagnostics) noexcept
    {
        return diagnostics.available && diagnostics.rendered;
    }

    bool HasSampleRenderQualityDirectionalShadowExecution(
        const SampleRenderDiagnostics& diagnostics) noexcept
    {
        return diagnostics.directionalShadowRequested &&
               diagnostics.directionalShadowSupported &&
               diagnostics.directionalShadowOutputReady &&
               diagnostics.directionalShadowSamplingEnabled &&
               diagnostics.directionalShadowRequestedCascadeCount != 0 &&
               diagnostics.directionalShadowProducedCascadeCount ==
                   diagnostics.directionalShadowRequestedCascadeCount &&
               diagnostics.directionalShadowResolvedCascadeCount ==
                   diagnostics.directionalShadowRequestedCascadeCount &&
               diagnostics.directionalShadowCasterCount != 0 &&
               diagnostics.directionalShadowDrawCount != 0;
    }

    bool HasSampleTransparentTemporalExecution(
        const RenderExecutionReceipt& receipt, uint64 expectedPresentedFrame) noexcept
    {
            if (!receipt.available || !receipt.graphComposed || !receipt.executionSucceeded ||
                !receipt.presented || !receipt.succeeded ||
                receipt.frameIdentity.frameSequence == 0 ||
                receipt.frameIdentity.frameSequence != expectedPresentedFrame)
                return false;
            const RenderFeatureExecutionReceipt* temporal = nullptr;
            for (const auto& feature : receipt.features)
            {
                if (feature.featureId != BuiltinRenderFeatureIds::Translucency) continue;
                if (temporal) return false;
                temporal = &feature;
            }
            if (!temporal || !temporal->selected || !temporal->readiness.IsReadyToExecute() ||
                !temporal->readiness.executed || temporal->fallbackUsed ||
                temporal->graphPassCount == 0 ||
                temporal->executedGraphPassCount != temporal->graphPassCount ||
                temporal->graphPassNames.size() != temporal->graphPassCount) return false;
            for (const char* name : {"TransparentMeshReactiveClear", "TransparentTemporalCandidates", "TransparentTemporalResolve"})
                if (std::count(temporal->graphPassNames.begin(), temporal->graphPassNames.end(), name) != 1)
                    return false;
        return true;
    }

    bool HasSampleRenderQualityTranslucencyExecution(
        const SampleRenderDiagnostics& diagnostics) noexcept
    {
        if (diagnostics.postProcessActualExecution.taaRequested &&
            !diagnostics.transparentTemporalExecutionQualified) return false;
        return diagnostics.transparentAlphaBlendDrawCount != 0 &&
               diagnostics.transparentPremultipliedDrawCount != 0 &&
               diagnostics.transparentAdditiveDrawCount != 0 &&
               diagnostics.transparentThinTransmissionDrawCount != 0 &&
               diagnostics.transparentOpaqueSceneColorPyramidReady &&
               diagnostics.transparentOpaqueSceneColorPyramidBuilt &&
               diagnostics.transparentReactiveMaskAttachmentBound &&
               diagnostics.transparentReactiveMaskWriteDrawCount != 0 &&
               diagnostics.transparentPriorityBucketCount >= 4u &&
               diagnostics.transparentFallbackCount == 0;
    }

    bool HasSampleMixedWeightedBlendedOITExecution(
        const SampleRenderDiagnostics& diagnostics) noexcept
    {
        const auto& execution = diagnostics.weightedBlendedOITActualExecution;
        return execution.HasQualifiedExecution() &&
               execution.meshWeightedGraphPassCount >= 1u &&
               execution.particleWeightedGraphPassCount >= 1u;
    }

    bool HasSampleRenderQualityCompositionExecution(
        const SampleRenderDiagnostics& diagnostics) noexcept
    {
        if (!IsSampleRenderQualityFrameAvailable(diagnostics))
        {
            return false;
        }

        return diagnostics.postProcessActualExecution
                   .HasQualifiedPresentation() &&
               diagnostics.postProcessActualExecution
                   .IsTemporalAAQualificationReady() &&
               HasSampleRenderQualityDirectionalShadowExecution(diagnostics) &&
               HasSampleRenderQualityTranslucencyExecution(diagnostics) &&
               GetSampleParticleVfxExecutionDiagnostics(diagnostics)
                   .HasFireAndSmokeExecution() &&
               GetSampleWaterSurfaceExecutionDiagnostics(diagnostics)
                   .HasWaterExecution();
    }

    bool HasSampleRenderQualityPresentedScene(
        const SampleRenderDiagnostics& diagnostics,
        const uint32 minimumVisibleObjectCount) noexcept
    {
        return IsSampleRenderQualityFrameAvailable(diagnostics) &&
               diagnostics.graphBuilt && diagnostics.graphCompiled &&
               diagnostics.visibleObjectCount >= minimumVisibleObjectCount;
    }
} // namespace RVX
