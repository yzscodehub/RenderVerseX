/**
 * @file SampleRenderQualityDiagnostics.cpp
 * @brief Renderer-backed predicates for the RenderQualityLab sample contract.
 */

#include "Samples/SampleRenderQualityDiagnostics.h"

#include "Samples/SampleCLI.h"
#include "Samples/SampleParticleVfxDiagnostics.h"
#include "Samples/SampleWaterSurfaceDiagnostics.h"

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

    bool HasSampleRenderQualityTranslucencyExecution(
        const SampleRenderDiagnostics& diagnostics) noexcept
    {
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
