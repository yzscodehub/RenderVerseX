#pragma once

/**
 * @file SampleRenderQualityDiagnostics.h
 * @brief Opaque, scenario-facing predicates for RenderQualityLab evidence.
 *
 * The complete SampleRenderDiagnostics value is owned by the Sample host and
 * is intentionally not made a compile dependency of scenario-only targets.
 * These predicates keep the scenario contract value-only while leaving the
 * renderer-backed projection in SampleCommon.
 */

#include "Core/Types.h"

namespace RVX
{
    struct SampleRenderDiagnostics;

    [[nodiscard]] bool IsSampleRenderQualityFrameAvailable(
        const SampleRenderDiagnostics& diagnostics) noexcept;

    [[nodiscard]] bool HasSampleRenderQualityDirectionalShadowExecution(
        const SampleRenderDiagnostics& diagnostics) noexcept;

    [[nodiscard]] bool HasSampleRenderQualityTranslucencyExecution(
        const SampleRenderDiagnostics& diagnostics) noexcept;

    [[nodiscard]] bool HasSampleRenderQualityCompositionExecution(
        const SampleRenderDiagnostics& diagnostics) noexcept;

    [[nodiscard]] bool HasSampleMixedWeightedBlendedOITExecution(
        const SampleRenderDiagnostics& diagnostics) noexcept;

    [[nodiscard]] bool HasSampleRenderQualityPresentedScene(
        const SampleRenderDiagnostics& diagnostics,
        uint32 minimumVisibleObjectCount) noexcept;
} // namespace RVX
