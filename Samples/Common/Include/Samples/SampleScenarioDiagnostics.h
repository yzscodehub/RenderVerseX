#pragma once

/**
 * @file SampleScenarioDiagnostics.h
 * @brief Backend-neutral projection of presented raster evidence for scenarios.
 */

#include "Core/Types.h"
#include "RenderContracts/RenderMaterial.h"

#include <string>
#include <vector>

namespace RVX
{
    struct SampleRenderDiagnostics;

    /** Immutable Material V2 facts frozen by the successful present boundary. */
    struct SamplePresentedMaterialBindingEvidence
    {
        uint64 parameterLayoutHash = 0;
        uint64 featureMask = 0;
        MaterialShadingModel shadingModel = MaterialShadingModel::StandardLit;
        SurfacePathEligibility surfacePathEligibility =
            SurfacePathEligibility::Unsupported;
        bool fallbackUsed = false;
        uint64 frameSequence = 0;
        uint64 presentationSequence = 0;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return parameterLayoutHash == GetMaterialParameterLayoutHashV2() &&
                   shadingModel <= MaterialShadingModel::ThinTransmission &&
                   surfacePathEligibility < SurfacePathEligibility::Unsupported &&
                   frameSequence != 0 && presentationSequence != 0;
        }
    };

    /**
     * @brief Minimal value-only evidence consumed by model-oriented scenarios.
     *
     * The host owns the full renderer diagnostic graph. Scenarios receive this
     * projection so their compile boundary never inherits RHI or Render
     * implementation headers merely to evaluate readiness.
     */
    struct SampleRasterScenarioDiagnostics
    {
        uint32 visibleObjectCount = 0;
        bool textureIBLEnabled = false;
        bool opaqueExecutionCompleted = false;
        bool opaqueExecutedDrawCountAvailable = false;
        uint32 opaqueExecutedDrawCount = 0;
        uint32 opaqueInstancingExecutedPacketCount = 0;
        uint32 opaqueInstancingSubmittedDrawCount = 0;
        bool opaqueMaterialBindingsAvailable = false;
        uint32 opaqueMaterialBindingCount = 0;
        uint32 opaqueMaterialFallbackBindingCount = 0;
        uint32 opaqueMaterialTextureFlags = 0;
        uint32 opaqueMaterialFallbackTextureFlags = 0;
        bool presentedMaterialBindingsAvailable = false;
        bool presentedMaterialBindingsOverflow = false;
        std::vector<SamplePresentedMaterialBindingEvidence>
            presentedMaterialBindings{};

        bool gpuDrivenPolicyDecisionAvailable = false;
        std::string gpuDrivenRequestedMode;
        std::string gpuDrivenPolicyReason;
        bool gpuDrivenEnabled = false;
        bool gpuDrivenGraphPassRecorded = false;
        bool gpuDrivenExecutionRecorded = false;
        bool gpuDrivenOpaqueIndirectRequested = false;
        bool gpuDrivenOpaqueIndirectEligible = false;
        bool gpuDrivenOpaqueIndirectSubmitted = false;
        uint32 gpuDrivenOpaqueIndirectBatchCount = 0;
        uint32 gpuDrivenOpaqueSubmittedSourcePacketCount = 0;
        uint32 gpuDrivenOpaqueDirectDrawCount = 0;
    };

    /** Project one immutable host diagnostic snapshot into scenario values. */
    [[nodiscard]] SampleRasterScenarioDiagnostics
    ProjectSampleRasterScenarioDiagnostics(
        const SampleRenderDiagnostics& diagnostics);

    /**
     * @brief True only when a present-bound V2 receipt proves opaque
     * StandardLit work without a fallback or receipt truncation.
     */
    [[nodiscard]] bool HasPresentedOpaqueStandardLitRasterEvidence(
        const SampleRasterScenarioDiagnostics& diagnostics) noexcept;

    /** Require each mask bit to occur in the presented StandardLit set. */
    [[nodiscard]] bool HasPresentedMaterialFeatureEvidence(
        const SampleRasterScenarioDiagnostics& diagnostics,
        uint64 requiredFeatureMask) noexcept;
} // namespace RVX
