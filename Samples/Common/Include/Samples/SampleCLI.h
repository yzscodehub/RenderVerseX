#pragma once

/**
 * @file SampleCLI.h
 * @brief Shared command-line and JSON report helpers for sample applications.
 */

#include "Core/Diagnostics/DiagnosticValue.h"
#include "Core/Types.h"
#include "Render/GPUScene/GPUSceneDiagnostics.h"
#include "Render/RenderDiagnostics.h"
#include "RenderContracts/RenderExposure.h"
#include "RenderContracts/RenderIdentity.h"
#include "RHI/RHIDefinitions.h"
#include "Resource/ResourceContentIdentity.h"
#include "Samples/SampleAssetCatalog.h"
#include "Samples/SampleInfo.h"

#include <array>
#include <cmath>
#include <filesystem>
#include <iosfwd>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace RVX
{
    /** @brief Current JSON schema version written by sample report producers. */
    inline constexpr uint32 RVX_SAMPLE_REPORT_SCHEMA_VERSION = 29;
    inline constexpr uint32
        RVX_SAMPLE_REPORT_PRE_RENDER_EXECUTION_RECEIPT_SCHEMA_VERSION = 20;
    inline constexpr uint32
        RVX_SAMPLE_REPORT_PRE_POST_PROCESS_EXECUTION_SCHEMA_VERSION = 21;
    inline constexpr uint32
        RVX_SAMPLE_REPORT_PRE_TEMPORAL_AA_EXECUTION_SCHEMA_VERSION = 22;
    inline constexpr uint32
        RVX_SAMPLE_REPORT_PRE_EXPOSURE_EXECUTION_SCHEMA_VERSION = 23;
    inline constexpr uint32
        RVX_SAMPLE_REPORT_PRE_AMBIENT_OCCLUSION_EXECUTION_SCHEMA_VERSION = 24;
    inline constexpr uint32
        RVX_SAMPLE_REPORT_PRE_AMBIENT_OCCLUSION_MOTION_REPROJECTION_SCHEMA_VERSION =
            25;
    inline constexpr uint32
        RVX_SAMPLE_REPORT_PRE_TOON_EXECUTION_SCHEMA_VERSION = 26;
    inline constexpr uint32
        RVX_SAMPLE_REPORT_PRE_TRANSLUCENCY_EXECUTION_SCHEMA_VERSION = 27;
    inline constexpr uint32
        RVX_SAMPLE_REPORT_PRE_COOKED_ONLY_ADMISSION_SCHEMA_VERSION = 28;
    inline constexpr uint32 RVX_SAMPLE_REPORT_PRE_VISUAL_CONTRACT_SCHEMA_VERSION = 19;
    inline constexpr uint32 RVX_SAMPLE_REPORT_SCENARIO_SCHEMA_VERSION = 12;
    inline constexpr uint32 RVX_SAMPLE_REPORT_ECS_DIAGNOSTICS_SCHEMA_VERSION = 13;
    inline constexpr uint32 RVX_SAMPLE_REPORT_PIXEL_PROBE_SCHEMA_VERSION = 16;

    /** @brief Serialized value-only receipt evidence for one render upload stream. */
    struct SampleRenderUploadWorkDiagnostics
    {
        uint64 cpuCopiedPayloadBytes = 0;
        uint64 committedPayloadBytes = 0;
        uint32 mappedRangeCount = 0;
        uint32 committedRangeCount = 0;
        DiagnosticValue<uint64> hostVisibilitySynchronizedBytes =
            DiagnosticValue<uint64>::Unavailable(
                "No committed mapped host-write receipts were recorded");
        std::array<uint64,
                   RVX_RENDER_HOST_WRITE_SYNCHRONIZATION_SCOPE_COUNT>
            hostVisibilitySynchronizationScopeRangeCounts{};
        std::array<uint64,
                   RVX_RENDER_HOST_WRITE_SYNCHRONIZATION_SCOPE_COUNT>
            hostVisibilitySynchronizationScopeBytes{};
        uint64 gpuCopyBytes = 0;
        uint32 gpuCopyRangeCount = 0;
    };

    /** @brief Ordered and multiset summaries of actual raster dereferences. */
    struct SampleRasterTranscript
    {
        bool available = false;
        uint32 entryCount = 0;
        uint64 orderedIdentityHash = 0;
        uint64 consumedPayloadHash = 0;
        uint64 unorderedIdentityHash = 0;
        uint64 unorderedIdentityHashSecondary = 0;
        uint64 unorderedConsumedPayloadHash = 0;
        uint64 unorderedConsumedPayloadHashSecondary = 0;
    };

    /** @brief Scalar post-fence GPU-scene culling qualification evidence. */
    struct SampleGPUSceneCullingQualification
    {
        bool requested = false;
        bool required = false;
        bool readbackAllocated = false;
        bool copyRecorded = false;
        bool submissionAccepted = false;
        bool completionObserved = false;
        bool compared = false;
        bool matched = false;
        bool inputCoverageCompared = false;
        bool inputCoverageMatched = false;
        bool directVisibilityCoverageCompared = false;
        bool directVisibilityCoverageMatched = false;
        bool cullOutputsCompared = false;
        bool cullOutputsMatched = false;
        bool indirectArgumentsCompared = false;
        bool indirectArgumentsMatched = false;
        bool rasterPayloadCompared = false;
        bool rasterPayloadMatched = false;
        std::string mismatch = "NotRequested";
        uint32 activeRowCount = 0;
        uint32 drawGroupCount = 0;
        uint32 expectedInputPacketCount = 0;
        uint32 expectedGPUInputPacketCount = 0;
        uint32 observedGPUInputPacketCount = 0;
        uint32 expectedDirectVisiblePacketCount = 0;
        uint32 observedGPUVisiblePacketCount = 0;
        uint32 missingDirectVisiblePacketCount = 0;
        uint32 gpuOnlyVisiblePacketCount = 0;
        uint32 directInputPacketCount = 0;
        uint32 skippedInputPacketCount = 0;
        uint32 expectedVisibleInstanceCount = 0;
        uint32 observedVisibleInstanceCount = 0;
        uint32 expectedSubmittedDrawCount = 0;
        uint32 observedSubmittedDrawCount = 0;
        uint32 firstMismatchActiveRow = RVX_INVALID_INDEX;
        uint32 firstMismatchDrawGroup = RVX_INVALID_INDEX;
        uint32 firstMismatchResidentRow = RVX_INVALID_INDEX;
        uint32 expectedValue = 0;
        uint32 observedValue = 0;
        uint64 expectedGPUInputIdentityHash = 0;
        uint64 observedGPUInputIdentityHash = 0;
        uint64 planPacketIdentityHash = 0;
        uint64 expectedDirectVisibleIdentityHash = 0;
        uint64 observedGPUVisibleIdentityHash = 0;
        uint64 expectedRasterPayloadHash = 0;
        uint64 observedRasterPayloadHash = 0;
        uint64 expectedIndirectArgumentsHash = 0;
        uint64 observedIndirectArgumentsHash = 0;
        SampleRasterTranscript tierOneRasterTranscript{};
        SampleRasterTranscript tierOneRasterTranscriptReference{};
        bool tierOneRasterTranscriptCompared = false;
        bool tierOneRasterTranscriptMatched = false;
        uint32 firstRasterTranscriptMismatchEntry = RVX_INVALID_INDEX;
        uint64 expectedRasterTranscriptIdentityHash = 0;
        uint64 observedRasterTranscriptIdentityHash = 0;
        uint64 expectedRasterTranscriptPayloadHash = 0;
        uint64 observedRasterTranscriptPayloadHash = 0;
        uint64 cpuPayloadBytes = 0;
        std::string capturedTier = "Direct";
        uint64 frameSequence = 0;
        uint64 recordEpoch = 0;
        uint64 gpuSceneLeaseVersion = 0;
        uint64 candidateVersion = 0;
        uint64 activeRowVersion = 0;
        uint64 completionValue = 0;
    };

    /** @brief Scalar post-fence Direct Opaque physical readback evidence. */
    struct SampleDirectRasterReadbackQualification
    {
        bool requested = false;
        bool required = false;
        bool readbackAllocated = false;
        bool copyRecorded = false;
        bool submissionAccepted = false;
        bool completionObserved = false;
        bool compared = false;
        bool matched = false;
        bool identity = false;
        bool allDirectDrawsInstanced = false;
        std::string mismatch = "NotRequested";
        uint64 frameSequence = 0;
        uint64 recordEpoch = 0;
        uint32 sourceFrameSlot = RVX_INVALID_INDEX;
        uint32 firstMismatchIndex = RVX_INVALID_INDEX;
        uint32 firstMismatchRow = RVX_INVALID_INDEX;
        uint64 cpuPayloadBytes = 0;
        uint64 completionValue = 0;
        SampleRasterTranscript expectedTranscript{};
        SampleRasterTranscript observedTranscript{};
    };

    /**
     * @brief Completion-qualified owner cumulative mutation evidence.
     *
     * This intentionally stays in the in-process Sample diagnostics contract.
     * Per-frame Sample report JSON retains its established schema while stress
     * qualification can compare durable completed-frame watermarks.
     */
    struct SampleRenderMutationEvidenceDiagnostics
    {
        bool available = false;
        bool saturated = false;
        uint64 evidenceEpoch = 0;
        uint64 completedPresentationCount = 0;
        uint64 completedFrameSequence = 0;
        uint64 requiredSceneRevision = 0;
        uint64 appliedSceneRevision = 0;

        uint64 sceneFullRebuildCount = 0;
        uint64 sceneIncrementalCommitCount = 0;
        uint64 sceneRebuiltObjectCount = 0;
        uint64 sceneRemovedObjectCount = 0;

        uint64 gpuSceneFullPublicationCount = 0;
        uint64 gpuSceneIncrementalPublicationCount = 0;
        uint64 gpuSceneIdentityOnlyPublicationCount = 0;
        uint64 gpuSceneMaterializedObjectCount = 0;
        uint64 gpuSceneAddCount = 0;
        uint64 gpuSceneUpdateCount = 0;
        uint64 gpuSceneRemoveCount = 0;
        uint64 gpuSceneNoOpCount = 0;

        uint64 gpuSceneSubmittedUploadCount = 0;
        uint64 gpuSceneUploadBytes = 0;
        uint64 gpuSceneUploadRangeCount = 0;
        uint64 gpuSceneFullUploadCount = 0;
        std::array<uint64, GPU_SCENE_DIAGNOSTICS_TABLE_COUNT>
            gpuSceneSubmittedUploadedRowCount{};

        uint64 gpuCullingInstancePatchedRowCount = 0;
        uint64 gpuCullingCandidatePatchedRowCount = 0;
        uint64 gpuCullingActiveRowPatchedRowCount = 0;
        uint64 gpuCullingInstanceUploadedRowCount = 0;
        uint64 gpuCullingCandidateUploadedRowCount = 0;
        uint64 gpuCullingActiveRowUploadedRowCount = 0;
        uint64 gpuCullingInstanceUploadBytes = 0;
        uint64 gpuCullingCandidateUploadBytes = 0;
        uint64 gpuCullingActiveRowUploadBytes = 0;
        uint64 gpuCullingInstanceFullMaterializationCount = 0;
        uint64 gpuCullingCandidateFullMaterializationCount = 0;
        uint64 gpuCullingActiveRowFullMaterializationCount = 0;
        uint64 gpuCullingContinuityFullMaterializationCount = 0;
        uint64 gpuCullingCapacityFullMaterializationCount = 0;

        uint64 directRasterInstancePatchedRowCount = 0;
        uint64 directRasterIndexPatchedRowCount = 0;
        uint64 directRasterInstanceUploadBytes = 0;
        uint64 directRasterIndexUploadBytes = 0;
        uint64 directRasterInstanceFullMaterializationCount = 0;
        uint64 directRasterIndexFullMaterializationCount = 0;

        uint32 gpuCullingOwnerCount = 0;
        uint32 directRasterOwnerCount = 0;
    };

    /** @brief Completion-qualified value receipt for an actually rendered material. */
    struct SamplePresentedMaterialBindingReceipt
    {
        RenderResourceHandle material{};
        uint64 contentRevision = 0;
        uint64 descriptorContentKey = 0;
        uint64 descriptorRevision = 0;
        std::vector<MaterialBindingTextureEntry> textureEntries{};
        bool fallbackUsed = false;
        uint64 frameSequence = 0;
        uint64 presentationSequence = 0;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return material.IsValid() && contentRevision != 0 &&
                   descriptorContentKey != 0 && descriptorRevision != 0 &&
                   frameSequence != 0 && presentationSequence != 0;
        }
    };

    /** @brief Completion-qualified value receipt for a fully uploaded palette. */
    struct SamplePresentedSkinningPaletteReceipt
    {
        uint64 providerComponentId = 0;
        uint64 sourceModelResourceId = 0;
        uint64 poseSequence = 0;
        uint64 paletteHash = 0;
        uint32 paletteCount = 0;
        RenderSkinningPaletteExecutionLane lane =
            RenderSkinningPaletteExecutionLane::Direct;
        uint64 frameSequence = 0;
        uint64 presentationSequence = 0;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return providerComponentId != 0 && sourceModelResourceId != 0 &&
                   poseSequence != 0 && paletteHash != 0 &&
                   paletteCount != 0 &&
                   lane == RenderSkinningPaletteExecutionLane::Direct &&
                   frameSequence != 0 && presentationSequence != 0;
        }
    };

    struct SampleCLIOptions
    {
        RHIBackendType backend = RHIBackendType::Auto;
        bool smoke = false;
        uint32 frames = 0;
        std::filesystem::path screenshotPath;
        std::filesystem::path reportPath;
        bool pixelProbeEnabled = false;
        uint32 pixelProbeX = 0;
        uint32 pixelProbeY = 0;
        /** @brief Explicit one-shot post-fence GPUScene culling readback. */
        bool gpuSceneCullingQualificationEnabled = false;
        /** @brief Explicit one-shot Direct Opaque physical readback. */
        bool directOpaqueRasterReadbackQualificationEnabled = false;
        /** @brief Explicit one-shot post-Present exposure-state readback. */
        bool exposureQualificationProbeEnabled = false;
        /** @brief Explicit one-shot post-Present GTAO readback. */
        bool ambientOcclusionQualificationProbeEnabled = false;
        /**
         * @brief Explicit one-shot Toon/outline presentation probe.
         *
         * The runner accepts this intent now, but it must remain fail-closed
         * until the Render facade publishes the matching immutable receipt and
         * implements RequestToonOutlineQualificationProbe().
         */
        bool toonOutlineQualificationProbeEnabled = false;
        /** GFX15 qualification intent; execution evidence remains renderer-owned. */
        bool translucencyQualificationProbeEnabled = false;
        uint32 width = 1280;
        uint32 height = 720;
        std::string quality = "default";
        bool diagnostics = false;
        bool enableValidation = true;
        bool showHelp = false;
    };

    /**
     * @brief Narrow, presentation-bound post-process execution evidence.
     *
     * This is deliberately a value projection rather than a copy of
     * RenderExecutionReceipt. Sample scenarios may evaluate the public
     * contract without receiving renderer-private graph or pass objects.
     */
    struct SamplePostProcessExecutionDiagnostics
    {
        bool available = false;
        uint32 executionReceiptSchemaVersion = 0;
        bool executionReceiptStructurallyValid = false;
        bool executionReceiptAvailable = false;
        bool executionReceiptGraphComposed = false;
        bool executionReceiptExecutionSucceeded = false;
        bool executionReceiptPresented = false;
        bool executionReceiptSucceeded = false;
        uint64 frameFeatureSequence = 0;
        uint64 receiptFrameSequence = 0;
        uint64 lastPresentedFrameSequence = 0;
        bool presentedFrameAligned = false;
        bool mainView = false;
        uint64 sceneRuntimeId = 0;
        uint64 cameraEntityPacked = 0;
        uint32 subViewIndex = 0;

        bool postProcessFeatureFound = false;
        bool postProcessFeatureSelected = false;
        bool postProcessFeatureExecuted = false;
        bool postProcessFeatureFallbackUsed = false;
        uint32 postProcessFeatureGraphPassCount = 0;
        uint32 postProcessFeatureExecutedGraphPassCount = 0;

        bool postProcessReceiptStructurallyValid = false;
        uint64 planContentHash = 0;
        /** Renderer-projected immutable exposure state; never scenario-authored. */
        uint64 exposureStateContentHash = 0;
        uint64 toneMapExposureStateHash = 0;
        uint64 bloomExposureStateHash = 0;
        uint64 taaExposureStateHash = 0;
        RenderExposureMode exposureMode = RenderExposureMode::ManualEV100;
        bool exposureRequested = false;
        bool exposurePipelineReady = false;
        bool exposureInputsReady = false;
        bool exposureExecuted = false;
        uint32 exposureHistogramPassCount = 0;
        uint32 exposureAdaptationPassCount = 0;
        bool exposureHistoryAvailableBefore = false;
        bool exposureHistorySampled = false;
        bool exposureHistoryCandidateProduced = false;
        bool exposureHistoryCommitted = false;
        bool exposureHistoryAbandoned = false;
        RenderHistoryResetReason exposureHistoryResetReasons =
            RenderHistoryResetReason::None;
        bool exposureProbeRequested = false;
        bool exposureProbeReady = false;
        float32 targetEV100 = std::numeric_limits<float32>::quiet_NaN();
        float32 adaptedEV100 = std::numeric_limits<float32>::quiet_NaN();
        float32 exposureScale = std::numeric_limits<float32>::quiet_NaN();
        float32 preExposure = std::numeric_limits<float32>::quiet_NaN();
        float32 previousPreExposure =
            std::numeric_limits<float32>::quiet_NaN();
        uint32 toneMappingPassCount = 0;
        uint32 bloomPassCount = 0;
        uint32 colorGradingPassCount = 0;
        uint32 fxaaPassCount = 0;
        uint32 taaPassCount = 0;
        RenderAntiAliasingMode antiAliasingMode =
            RenderAntiAliasingMode::None;
        bool taaRequested = false;
        bool taaPipelineReady = false;
        bool taaInputsReady = false;
        bool taaExecuted = false;
        bool historyAvailableBefore = false;
        bool historySampled = false;
        bool historyCandidateProduced = false;
        bool historyCommitted = false;
        bool historyAbandoned = false;
        RenderHistoryResetReason historyResetReasons =
            RenderHistoryResetReason::None;
        Vec2 currentJitterPixels = Vec2(0.0f);
        Vec2 previousJitterPixels = Vec2(0.0f);
        uint32 taaResolveGraphPassCount = 0;
        uint32 taaHistoryStoreGraphPassCount = 0;
        PostProcessColorDomain inputColorDomain =
            PostProcessColorDomain::Unknown;
        PostProcessColorDomain plannedOutputColorDomain =
            PostProcessColorDomain::Unknown;
        PostProcessColorDomain storedOutputColorDomain =
            PostProcessColorDomain::Unknown;
        RHIFormat finalOutputFormat = RHIFormat::Unknown;
        PostProcessTransfer toneMappingTransfer = PostProcessTransfer::None;
        PostProcessTransfer presentationTransfer = PostProcessTransfer::None;
        bool hdrInputReady = false;
        bool displayLinearOutputReady = false;
        bool encodedOutputReady = false;
        bool fallbackUsed = false;
        std::string diagnostic;

        /** @brief True only for a structurally valid, exactly presented main view. */
        [[nodiscard]] bool HasQualifiedPresentation() const noexcept
        {
            const bool presentedMainView =
                presentedFrameAligned && frameFeatureSequence != 0 &&
                frameFeatureSequence == receiptFrameSequence &&
                receiptFrameSequence == lastPresentedFrameSequence && mainView;
            const bool commonColorContract =
                inputColorDomain == PostProcessColorDomain::SceneLinearHDR &&
                storedOutputColorDomain ==
                    PostProcessColorDomain::DisplayEncodedLDR &&
                finalOutputFormat != RHIFormat::Unknown;
            const bool srgbPresentationContract =
                commonColorContract &&
                plannedOutputColorDomain ==
                    PostProcessColorDomain::DisplayLinearLDR &&
                IsSRGBFormat(finalOutputFormat) &&
                toneMappingTransfer ==
                    PostProcessTransfer::SceneLinearToDisplayLinear &&
                presentationTransfer ==
                    PostProcessTransfer::DisplayLinearToEncoded &&
                displayLinearOutputReady;
            const bool unormPresentationContract =
                commonColorContract &&
                plannedOutputColorDomain ==
                    PostProcessColorDomain::DisplayEncodedLDR &&
                !IsSRGBFormat(finalOutputFormat) &&
                toneMappingTransfer ==
                    PostProcessTransfer::SceneLinearToDisplayEncoded &&
                presentationTransfer ==
                    PostProcessTransfer::SceneLinearToDisplayEncoded &&
                !displayLinearOutputReady;

            const bool supportedExecutionReceipt =
                executionReceiptSchemaVersion == 8 ||
                executionReceiptSchemaVersion == 10;
            return available && supportedExecutionReceipt &&
                   executionReceiptStructurallyValid &&
                   executionReceiptAvailable && executionReceiptGraphComposed &&
                   executionReceiptExecutionSucceeded &&
                   executionReceiptPresented && executionReceiptSucceeded &&
                   presentedMainView &&
                   postProcessFeatureFound && postProcessFeatureSelected &&
                   postProcessFeatureExecuted && !postProcessFeatureFallbackUsed &&
                   postProcessFeatureGraphPassCount != 0 &&
                   postProcessFeatureExecutedGraphPassCount ==
                       postProcessFeatureGraphPassCount &&
                   postProcessReceiptStructurallyValid &&
                   planContentHash != 0 && toneMappingPassCount == 1 &&
                   hdrInputReady &&
                   encodedOutputReady && !fallbackUsed &&
                   (srgbPresentationContract || unormPresentationContract);
        }

        /** @brief Exact immutable exposure-state identity shared by HDR consumers. */
        [[nodiscard]] bool HasSharedExposureState() const noexcept
        {
            const bool bloomMatches = bloomPassCount == 0
                ? bloomExposureStateHash == 0
                : bloomExposureStateHash == exposureStateContentHash;
            const bool taaMatches = antiAliasingMode != RenderAntiAliasingMode::TAA
                ? taaExposureStateHash == 0
                : taaExposureStateHash == exposureStateContentHash;
            return exposureStateContentHash != 0 &&
                   toneMapExposureStateHash == exposureStateContentHash &&
                   bloomMatches && taaMatches;
        }

        /** @brief Probe values obey the sole public EV100 calibration. */
        [[nodiscard]] bool HasPhysicallyConsistentExposureProbe(
            float32 exposureCompensationEV = 0.0f) const noexcept
        {
            if (!exposureProbeRequested || !exposureProbeReady ||
                targetEV100 < RVX_RENDER_EXPOSURE_MIN_EV100 ||
                targetEV100 > RVX_RENDER_EXPOSURE_MAX_EV100 ||
                adaptedEV100 < RVX_RENDER_EXPOSURE_MIN_EV100 ||
                adaptedEV100 > RVX_RENDER_EXPOSURE_MAX_EV100)
            {
                return false;
            }
            float32 expectedScale = 0.0f;
            if (!TryCalculateRenderExposureScale(
                    adaptedEV100, exposureCompensationEV, expectedScale))
            {
                return false;
            }
            return AreRenderExposureScalesEquivalent(
                       exposureScale, expectedScale) &&
                   IsValidRenderExposureScale(preExposure) &&
                   IsValidRenderExposureScale(previousPreExposure);
        }

        /** @brief Exact GFX12 Manual EV100 receipt with qualification readback. */
        [[nodiscard]] bool IsManualEV100ExposureQualificationReady() const noexcept
        {
            return HasQualifiedPresentation() && HasSharedExposureState() &&
                   exposureMode == RenderExposureMode::ManualEV100 &&
                   exposureRequested && exposurePipelineReady &&
                   exposureInputsReady && exposureExecuted &&
                   exposureHistogramPassCount == 0 &&
                   exposureAdaptationPassCount == 1 &&
                   exposureHistoryAvailableBefore && exposureHistorySampled &&
                   exposureHistoryCandidateProduced &&
                   exposureHistoryCommitted && !exposureHistoryAbandoned &&
                   exposureHistoryResetReasons == RenderHistoryResetReason::None &&
                   HasPhysicallyConsistentExposureProbe() &&
                   AreRenderExposureScalesEquivalent(
                       preExposure, exposureScale) &&
                   AreRenderExposureScalesEquivalent(
                       previousPreExposure, exposureScale) &&
                   bloomPassCount == 1 &&
                   IsTemporalAAQualificationReady();
        }

        /** @brief Exact GFX12 histogram-auto receipt with Present-safe history. */
        [[nodiscard]] bool IsHistogramAutoExposureQualificationReady() const noexcept
        {
            return HasQualifiedPresentation() && HasSharedExposureState() &&
                   exposureMode == RenderExposureMode::HistogramAuto &&
                   exposureRequested && exposurePipelineReady &&
                   exposureInputsReady && exposureExecuted &&
                   exposureHistogramPassCount == 2 &&
                   exposureAdaptationPassCount == 1 &&
                   exposureHistoryAvailableBefore && exposureHistorySampled &&
                   exposureHistoryCandidateProduced && exposureHistoryCommitted &&
                   !exposureHistoryAbandoned &&
                   exposureHistoryResetReasons == RenderHistoryResetReason::None &&
                   HasPhysicallyConsistentExposureProbe() &&
                   bloomPassCount == 1 &&
                   IsTemporalAAQualificationReady();
        }

        /** @brief Exact GFX10 Bloom/ColorGrade/FXAA qualification contract. */
        [[nodiscard]] bool IsQualificationReady() const noexcept
        {
            return HasQualifiedPresentation() &&
                   antiAliasingMode == RenderAntiAliasingMode::FXAA &&
                   bloomPassCount == 1 && colorGradingPassCount == 1 &&
                   fxaaPassCount == 1 && taaPassCount == 0 && !taaRequested &&
                   !taaPipelineReady && !taaInputsReady && !taaExecuted &&
                   !historyAvailableBefore && !historySampled &&
                   !historyCandidateProduced && !historyCommitted &&
                   !historyAbandoned &&
                   historyResetReasons == RenderHistoryResetReason::None &&
                   currentJitterPixels == Vec2(0.0f) &&
                   previousJitterPixels == Vec2(0.0f) &&
                   taaResolveGraphPassCount == 0 &&
                   taaHistoryStoreGraphPassCount == 0;
        }

        /** @brief Exact GFX11 temporal resolve and present-commit qualification. */
        [[nodiscard]] bool IsTemporalAAQualificationReady() const noexcept
        {
            return HasQualifiedPresentation() &&
                   antiAliasingMode == RenderAntiAliasingMode::TAA &&
                   fxaaPassCount == 0 && taaPassCount == 1 && taaRequested &&
                   taaPipelineReady && taaInputsReady && taaExecuted &&
                   historyAvailableBefore && historySampled &&
                   historyCandidateProduced && historyCommitted &&
                   !historyAbandoned &&
                   historyResetReasons == RenderHistoryResetReason::None &&
                   std::isfinite(currentJitterPixels.x) &&
                   std::isfinite(currentJitterPixels.y) &&
                   std::isfinite(previousJitterPixels.x) &&
                   std::isfinite(previousJitterPixels.y) &&
                   currentJitterPixels != previousJitterPixels &&
                   taaResolveGraphPassCount == 1 &&
                   taaHistoryStoreGraphPassCount == 1;
        }
    };

    /**
     * @brief Narrow, presentation-bound ambient-occlusion execution evidence.
     *
     * This remains a separate projection from post processing: GTAO produces a
     * scene texture consumed by indirect lighting before HDR post processing.
     * Sample scenarios only receive immutable receipt values, never graph
     * handles, pass objects, or renderer-private history resources.
     */
    struct SampleAmbientOcclusionExecutionDiagnostics
    {
        bool available = false;
        uint32 executionReceiptSchemaVersion = 0;
        bool executionReceiptStructurallyValid = false;
        bool executionReceiptAvailable = false;
        bool executionReceiptGraphComposed = false;
        bool executionReceiptExecutionSucceeded = false;
        bool executionReceiptPresented = false;
        bool executionReceiptSucceeded = false;
        uint64 frameFeatureSequence = 0;
        uint64 receiptFrameSequence = 0;
        uint64 lastPresentedFrameSequence = 0;
        bool presentedFrameAligned = false;
        bool mainView = false;
        uint64 sceneRuntimeId = 0;
        uint64 cameraEntityPacked = 0;
        uint32 subViewIndex = 0;

        bool receiptStructurallyValid = false;
        uint64 planContentHash = 0;
        bool requested = false;
        bool pipelineReady = false;
        bool inputsReady = false;
        bool executed = false;
        bool lightingConsumerExecuted = false;
        bool linearDepthInputReady = false;
        bool worldNormalInputReady = false;
        /** @brief The GTAO resolve consumed the view's motion-vector field. */
        bool velocityInputReady = false;
        /** @brief The source velocity pass executed for the presented view. */
        bool velocityProducerExecuted = false;
        bool outputPublished = false;
        RHIFormat outputFormat = RHIFormat::Unknown;
        uint32 horizonSearchGraphPassCount = 0;
        uint32 bilateralDenoiseGraphPassCount = 0;
        uint32 temporalResolveGraphPassCount = 0;
        bool historyAvailableBefore = false;
        bool historySampled = false;
        /** @brief History sampling used motion-vector reprojection. */
        bool motionReprojectionUsed = false;
        /** @brief The qualified frame contains genuine unjittered camera motion. */
        bool viewProjectionMotionObserved = false;
        bool historyCandidateProduced = false;
        bool historyCommitted = false;
        bool historyAbandoned = false;
        RenderHistoryResetReason historyResetReasons =
            RenderHistoryResetReason::None;
        uint64 aoFieldContentHash = 0;
        uint64 lightingAOFieldContentHash = 0;
        uint64 velocityFieldIdentity = 0;
        uint32 consumerMask = 0;
        bool probeRequested = false;
        bool probeReady = false;
        uint32 probeSampleCount = 0;
        uint32 probeOccludedSampleCount = 0;
        float32 probeMinVisibility = std::numeric_limits<float32>::quiet_NaN();
        float32 probeMeanVisibility = std::numeric_limits<float32>::quiet_NaN();
        float32 probeMaxVisibility = std::numeric_limits<float32>::quiet_NaN();
        bool fallbackUsed = false;
        std::string diagnostic;

        /** @brief True once GTAO was genuinely executed for the presented view. */
        [[nodiscard]] bool IsExecutionReady() const noexcept
        {
            const bool presentedMainView =
                presentedFrameAligned && frameFeatureSequence != 0 &&
                frameFeatureSequence == receiptFrameSequence &&
                receiptFrameSequence == lastPresentedFrameSequence && mainView;
            return available && executionReceiptSchemaVersion == 10 &&
                   executionReceiptStructurallyValid &&
                   executionReceiptAvailable && executionReceiptGraphComposed &&
                   executionReceiptExecutionSucceeded &&
                   executionReceiptPresented && executionReceiptSucceeded &&
                   presentedMainView && receiptStructurallyValid &&
                   planContentHash != 0 && requested && pipelineReady &&
                   inputsReady && executed && linearDepthInputReady &&
                   worldNormalInputReady && velocityInputReady &&
                   velocityProducerExecuted && velocityFieldIdentity != 0 &&
                   outputPublished &&
                   outputFormat == RHIFormat::R8_UNORM &&
                   horizonSearchGraphPassCount != 0 &&
                   bilateralDenoiseGraphPassCount >= 2 &&
                   temporalResolveGraphPassCount == 1 && !fallbackUsed;
        }

        /** @brief True only after a non-neutral probe and post-Present history commit. */
        [[nodiscard]] bool IsQualificationReady() const noexcept
        {
            constexpr uint32 ExpectedIndirectOnlyConsumerMask = 0x7u;
            return IsExecutionReady() && historyAvailableBefore &&
                   historySampled && motionReprojectionUsed &&
                   viewProjectionMotionObserved &&
                   historyCandidateProduced &&
                   historyCommitted && !historyAbandoned &&
                   historyResetReasons == RenderHistoryResetReason::None &&
                   lightingConsumerExecuted && aoFieldContentHash != 0 &&
                   aoFieldContentHash == lightingAOFieldContentHash &&
                   consumerMask == ExpectedIndirectOnlyConsumerMask &&
                   probeRequested && probeReady && probeSampleCount != 0 &&
                   probeOccludedSampleCount != 0 &&
                   probeOccludedSampleCount < probeSampleCount &&
                   std::isfinite(probeMinVisibility) &&
                   std::isfinite(probeMeanVisibility) &&
                   std::isfinite(probeMaxVisibility) &&
                   probeMinVisibility >= 0.0f && probeMinVisibility < 1.0f &&
                   probeMeanVisibility >= probeMinVisibility &&
                   probeMeanVisibility < 1.0f &&
                   probeMaxVisibility >= probeMeanVisibility &&
                   probeMaxVisibility <= 1.0f;
        }
    };

    /**
     * @brief Presentation-bound value projection of ToonLit raster evidence.
     *
     * The host constructs this only from an immutable schema-v11 renderer
     * receipt.  The public report intentionally contains only stable material
     * execution facts; it never derives a result from requested sample policy.
     */
    struct SampleToonLightingExecutionDiagnostics
    {
        bool available = false;
        uint32 executionReceiptSchemaVersion = 0;
        bool executionReceiptStructurallyValid = false;
        bool executionReceiptAvailable = false;
        bool executionReceiptGraphComposed = false;
        bool executionReceiptExecutionSucceeded = false;
        bool executionReceiptPresented = false;
        bool executionReceiptSucceeded = false;
        uint64 frameFeatureSequence = 0;
        uint64 receiptFrameSequence = 0;
        uint64 lastPresentedFrameSequence = 0;
        bool presentedFrameAligned = false;
        bool mainView = false;

        bool requested = false;
        bool pipelineReady = false;
        bool inputsReady = false;
        bool executed = false;
        /** Canonical program family resolved from the renderer value identity. */
        std::string programFamily;
        uint32 materialCount = 0;
        uint32 drawCount = 0;
        uint32 forwardShadedDrawCount = 0;
        uint32 deferredShadedDrawCount = 0;
        uint32 standardLitFallbackCount = 0;
        uint32 materialFallbackCount = 0;
        bool bandsApplied = false;
        bool specularThresholdApplied = false;
        bool rimApplied = false;
        bool shadowTintApplied = false;
        uint32 materialTableSchemaVersion = 0;
        uint64 materialTableContentHash = 0;

        [[nodiscard]] bool IsExecutionReady() const noexcept
        {
            const bool presentedMainView =
                presentedFrameAligned && frameFeatureSequence != 0 &&
                frameFeatureSequence == receiptFrameSequence &&
                receiptFrameSequence == lastPresentedFrameSequence && mainView;
            const bool oneRasterPathExecuted =
                (forwardShadedDrawCount != 0) !=
                (deferredShadedDrawCount != 0);
            return available && executionReceiptSchemaVersion == 11 &&
                   executionReceiptStructurallyValid &&
                   executionReceiptAvailable && executionReceiptGraphComposed &&
                   executionReceiptExecutionSucceeded &&
                   executionReceiptPresented && executionReceiptSucceeded &&
                   presentedMainView && requested && pipelineReady && inputsReady &&
                   executed && !programFamily.empty() && materialCount != 0 &&
                   drawCount != 0 &&
                   forwardShadedDrawCount + deferredShadedDrawCount == drawCount &&
                   oneRasterPathExecuted && standardLitFallbackCount == 0 &&
                   materialFallbackCount == 0 && bandsApplied &&
                   specularThresholdApplied && rimApplied && shadowTintApplied &&
                   materialTableSchemaVersion != 0 &&
                   materialTableContentHash != 0;
        }
    };

    /**
     * @brief Presentation-bound value projection of the HDR Toon outline.
     *
     * Qualification-only verifier metadata remains in-process so the public
     * JSON shape stays limited to its documented, capability-facing facts.
     */
    struct SampleToonOutlineExecutionDiagnostics
    {
        bool available = false;
        uint32 executionReceiptSchemaVersion = 0;
        bool executionReceiptStructurallyValid = false;
        bool executionReceiptAvailable = false;
        bool executionReceiptGraphComposed = false;
        bool executionReceiptExecutionSucceeded = false;
        bool executionReceiptPresented = false;
        bool executionReceiptSucceeded = false;
        uint64 frameFeatureSequence = 0;
        uint64 receiptFrameSequence = 0;
        uint64 lastPresentedFrameSequence = 0;
        bool presentedFrameAligned = false;
        bool mainView = false;

        bool requested = false;
        bool pipelineReady = false;
        bool inputsReady = false;
        bool executed = false;
        bool depthInputReady = false;
        bool worldNormalInputReady = false;
        bool materialIdentityInputReady = false;
        bool materialTableInputReady = false;
        bool temporalOrderingValidated = false;
        uint32 taaSequenceIndex = 0;
        uint32 outlineSequenceIndex = 0;
        uint32 toneMapSequenceIndex = 0;
        uint32 eligibleToonMaterialCount = 0;
        uint64 outlinedPixelCount = 0;
        uint64 nonToonAffectedPixelCount = 0;
        bool resolutionAware = false;
        uint64 settingsHash = 0;
        bool fallbackUsed = false;

        /** Internal-only exact verifier state, copied from the renderer. */
        bool qualificationRequested = false;
        bool qualificationReady = false;
        uint32 renderWidth = 0;
        uint32 renderHeight = 0;
        uint64 materialTableContentHash = 0;
        std::string diagnostic;

        [[nodiscard]] bool IsExecutionReady() const noexcept
        {
            const bool presentedMainView =
                presentedFrameAligned && frameFeatureSequence != 0 &&
                frameFeatureSequence == receiptFrameSequence &&
                receiptFrameSequence == lastPresentedFrameSequence && mainView;
            return available && executionReceiptSchemaVersion == 11 &&
                   executionReceiptStructurallyValid &&
                   executionReceiptAvailable && executionReceiptGraphComposed &&
                   executionReceiptExecutionSucceeded &&
                   executionReceiptPresented && executionReceiptSucceeded &&
                   presentedMainView && requested && pipelineReady && inputsReady &&
                   executed && depthInputReady && worldNormalInputReady &&
                   materialIdentityInputReady && materialTableInputReady &&
                   temporalOrderingValidated && eligibleToonMaterialCount != 0 &&
                   outlineSequenceIndex != 0 && toneMapSequenceIndex != 0 &&
                   outlineSequenceIndex < toneMapSequenceIndex &&
                   (taaSequenceIndex == 0 || taaSequenceIndex < outlineSequenceIndex) &&
                   settingsHash != 0 && materialTableContentHash != 0 &&
                   !fallbackUsed;
        }

        [[nodiscard]] bool IsQualificationReady() const noexcept
        {
            return IsExecutionReady() && qualificationRequested &&
                   qualificationReady && taaSequenceIndex != 0 &&
                   taaSequenceIndex < outlineSequenceIndex &&
                   outlineSequenceIndex < toneMapSequenceIndex &&
                   outlinedPixelCount != 0 && nonToonAffectedPixelCount == 0 &&
                   resolutionAware && renderWidth != 0 && renderHeight != 0 &&
                   diagnostic.empty();
        }
    };

    struct SampleRenderDiagnostics
    {
        bool available = false;
        bool renderAttempted = false;
        bool rendered = false;
        bool graphBuilt = false;
        bool graphCompiled = false;
        uint32 renderGraphTotalPasses = 0;
        uint32 visibleObjectCount = 0;
        uint32 renderSceneLightCount = 0;
        /** @brief Owner cumulative values frozen at one successful completion. */
        SampleRenderMutationEvidenceDiagnostics mutationEvidence{};
        bool renderSceneValuesAvailable = false;
        uint64 renderSceneFrameSequence = 0;
        uint64 renderSceneAppliedRevision = 0;
        uint64 renderSceneRequiredRevision = 0;
        uint32 renderSceneObjectCount = 0;
        uint32 renderSceneValueLightCount = 0;
        uint64 renderSceneLightStateHash = 0;
        bool engineRenderRuntimeAvailable = false;
        uint64 engineRequiredSceneFrameSequence = 0;
        uint64 engineRequiredSceneRevision = 0;
        uint64 temporalEpoch = 0;
        uint64 activeCameraIdentity = 0;
        uint64 activeCameraCutRevision = 0;
        uint64 temporalResetCount = 0;
        /**
         * @brief Host-level sequence observations.
         *
         * Zero is a valid value before an accepted frame has completed.  The
         * DiagnosticValue wrapper distinguishes that state from a missing
         * Engine/Render runtime observation.
         */
        DiagnosticValue<uint64> lastSubmittedFrameSequence =
            DiagnosticValue<uint64>::Unavailable(
                "Render runtime diagnostics are unavailable.");
        DiagnosticValue<uint64> lastPresentedFrameSequence =
            DiagnosticValue<uint64>::Unavailable(
                "Render runtime diagnostics are unavailable.");
        /** @brief Number of fully completed Engine update ticks. */
        DiagnosticValue<uint64> engineUpdateTickCount =
            DiagnosticValue<uint64>::Unavailable(
                "Engine update diagnostics are unavailable.");
        /** @brief Direct value projection of Render native-validation telemetry. */
        bool nativeValidationAvailable = false;
        bool nativeValidationEnabled = false;
        bool nativeValidationReadComplete = false;
        uint64 nativeValidationWarningCount = 0;
        uint64 nativeValidationErrorCount = 0;
        uint64 nativeValidationCorruptionCount = 0;
        uint32 requestedPostProcessEffectCount = 0;
        uint32 enabledPostProcessEffectCount = 0;
        uint32 unsupportedPostProcessSkippedCount = 0;
        uint32 postProcessGraphPassCount = 0;
        /** @brief Exact, main-view execution projection for post-process only. */
        SamplePostProcessExecutionDiagnostics postProcessActualExecution{};
        /** @brief Exact, main-view execution projection for GTAO only. */
        SampleAmbientOcclusionExecutionDiagnostics
            ambientOcclusionActualExecution{};
        /** Exact, main-view execution projection for ToonLit raster work. */
        SampleToonLightingExecutionDiagnostics toonLightingActualExecution{};
        /** Exact, main-view execution projection for HDR Toon outlines. */
        SampleToonOutlineExecutionDiagnostics toonOutlineActualExecution{};
        /** Renderer-owned GFX15 sorted-translucency receipt facts. */
        uint32 transparentAlphaBlendDrawCount = 0;
        uint32 transparentPremultipliedDrawCount = 0;
        uint32 transparentAdditiveDrawCount = 0;
        uint32 transparentThinTransmissionDrawCount = 0;
        bool transparentOpaqueSceneSnapshotReady = false;
        bool transparentOpaqueSceneSnapshotCopied = false;
        bool transparentReactiveMaskAttachmentBound = false;
        uint32 transparentReactiveMaskWriteDrawCount = 0;
        uint32 transparentPriorityBucketCount = 0;
        uint32 transparentFallbackCount = 0;
        /** Exact frame identity for the flat GFX15 renderer projection. */
        uint64 transparentFrameFeatureSequence = 0;
        uint64 transparentReceiptFrameSequence = 0;
        uint64 transparentLastPresentedFrameSequence = 0;
        bool transparentPresentedFrameAligned = false;
        bool clusteredLightingInitialized = false;
        uint32 clusteredLightingActiveClusters = 0;
        bool textureIBLEnabled = false;
        bool directionalShadowAvailable = false;
        bool directionalShadowRequested = false;
        bool directionalShadowSupported = false;
        bool directionalShadowOutputReady = false;
        bool directionalShadowSamplingEnabled = false;
        uint32 directionalShadowRequestedCascadeCount = 0;
        uint32 directionalShadowProducedCascadeCount = 0;
        uint32 directionalShadowResolvedCascadeCount = 0;
        uint32 directionalShadowMapSize = 0;
        uint32 directionalShadowCasterCount = 0;
        uint32 directionalShadowDrawCount = 0;
        std::string directionalShadowReason;
        bool localLightingAvailable = false;
        uint32 pointLightRequestedCount = 0;
        uint32 pointLightAdmittedCount = 0;
        uint32 pointLightCapacity = 0;
        uint32 pointLightOverflowCount = 0;
        uint32 spotLightRequestedCount = 0;
        uint32 spotLightAdmittedCount = 0;
        uint32 spotLightCapacity = 0;
        uint32 spotLightOverflowCount = 0;
        uint32 pointShadowRequestedCount = 0;
        bool pointShadowSupported = false;
        std::string pointShadowReason;
        uint32 spotShadowRequestedCount = 0;
        bool spotShadowSupported = false;
        std::string spotShadowReason;
        bool hzbRequested = false;
        bool hzbSupported = false;
        bool hzbEnabled = false;
        std::string hzbReason;
        bool transparentAvailable = false;
        bool transparentOrderValid = false;
        uint64 transparentOrderHash = 0;
        uint32 transparentRejectedNonFiniteDepthCount = 0;
        uint32 transparentCandidateDrawItemCount = 0;
        uint32 transparentPreparedDrawItemCount = 0;
        uint32 transparentExecutedPacketCount = 0;
        uint32 transparentExecutedDrawCount = 0;
        uint32 transparentSkippedMaterialBindingCount = 0;
        uint32 transparentSkippedResourceCount = 0;
        uint32 transparentSkippedExecutionDrawCount = 0;
        uint32 transparentMaterialBindingCount = 0;
        uint32 transparentMaterialFallbackBindingCount = 0;
        uint32 transparentMaterialTextureFlags = 0;
        uint32 transparentMaterialFallbackTextureFlags = 0;
        bool transparentNoWork = false;
        bool transparentPreflightFailed = false;
        bool transparentExecutionFailed = false;
        bool materialReady = false;
        bool materialUsedFallback = false;
        bool materialConstantsUpdated = false;
        bool materialDescriptorSetAvailable = false;
        uint32 materialTextureFlags = 0;
        bool presentedMaterialBindingsAvailable = false;
        bool presentedMaterialBindingsOverflow = false;
        std::vector<SamplePresentedMaterialBindingReceipt>
            presentedMaterialBindings{};
        bool presentedSkinningPalettesAvailable = false;
        bool presentedSkinningPalettesOverflow = false;
        std::vector<SamplePresentedSkinningPaletteReceipt>
            presentedSkinningPalettes{};
        /** Live queue telemetry; never used to invalidate a presented frame. */
        uint32 renderPendingUploadCount = 0;
        uint32 renderRetirementEntryCount = 0;
        bool opaqueExecutionCompleted = false;
        bool opaqueExecutedDrawCountAvailable = false;
        uint32 opaqueExecutedDrawCount = 0;
        std::string instancingRequestedMode;
        bool opaqueInstancingPlanAvailable = false;
        bool opaqueInstancingPreflightSucceeded = false;
        uint32 opaqueInstancingPlannedPacketCount = 0;
        uint32 opaqueInstancingPlannedDrawCount = 0;
        uint32 opaqueInstancingPlannedInstanceCount = 0;
        uint32 opaqueInstancingPlannedBatchCount = 0;
        uint32 opaqueInstancingExecutedPacketCount = 0;
        uint32 opaqueInstancingSubmittedDrawCount = 0;
        uint32 opaqueInstancingSubmittedInstanceCount = 0;
        uint32 opaqueInstancingBatchCount = 0;
        uint32 opaqueInstancingFallbackBatchCount = 0;
        bool opaqueMaterialBindingsAvailable = false;
        uint32 opaqueMaterialBindingCount = 0;
        uint32 opaqueMaterialFallbackBindingCount = 0;
        uint32 opaqueMaterialTextureFlags = 0;
        uint32 opaqueMaterialFallbackTextureFlags = 0;
        bool gpuDrivenPolicyDecisionAvailable = false;
        std::string gpuDrivenRequestedMode;
        std::string gpuDrivenPolicyReason;
        std::string gpuDrivenQualification;
        bool gpuDrivenBackendQualified = false;
        bool gpuDrivenCapabilitiesReady = false;
        bool gpuDrivenPipelineReady = false;
        bool gpuDrivenEnabled = false;
        bool gpuDrivenGraphPassAdded = false;
        bool gpuDrivenGraphPassRecorded = false;
        bool gpuDrivenExecutionRecorded = false;
        uint64 gpuDrivenInstanceUploadBytes = 0;
        uint64 gpuDrivenCandidateUploadBytes = 0;
        uint64 gpuDrivenActiveRowUploadBytes = 0;
        uint32 gpuDrivenActiveRowCount = 0;
        uint32 gpuDrivenActiveRowHighWatermark = 0;
        uint32 gpuDrivenInstancePatchedRowCount = 0;
        uint32 gpuDrivenCandidatePatchedRowCount = 0;
        uint32 gpuDrivenActiveRowPatchedRowCount = 0;
        uint64 gpuDrivenInstanceFullMaterializationCount = 0;
        uint64 gpuDrivenCandidateFullMaterializationCount = 0;
        uint64 gpuDrivenActiveRowFullMaterializationCount = 0;
        uint64 gpuDrivenContinuityFullMaterializationCount = 0;
        uint64 gpuDrivenCapacityFullMaterializationCount = 0;
        uint64 directRasterInstanceUploadBytes = 0;
        uint64 directRasterInstanceIndexUploadBytes = 0;
        SampleRenderUploadWorkDiagnostics gpuDrivenCanonicalInstanceUploadWork{};
        SampleRenderUploadWorkDiagnostics gpuDrivenCanonicalCandidateUploadWork{};
        SampleRenderUploadWorkDiagnostics gpuDrivenCanonicalActiveRowUploadWork{};
        SampleRenderUploadWorkDiagnostics directRasterInstanceUploadWork{};
        SampleRenderUploadWorkDiagnostics directRasterIndexUploadWork{};
        uint32 directRasterInstancePatchedRowCount = 0;
        uint32 directRasterIndexPatchedRowCount = 0;
        uint32 directRasterActiveInstanceCount = 0;
        uint32 directRasterActiveInstanceCapacity = 0;
        uint32 directRasterInstanceFullMaterializationCount = 0;
        uint32 directRasterIndexFullMaterializationCount = 0;
        bool extractionAvailable = false;
        uint32 extractionFullScanCount = 0;
        uint32 extractionChangeFeedChangeCount = 0;
        uint32 extractionActorRebuildCount = 0;
        uint32 extractionProxyVisitCount = 0;
        uint32 extractionComponentVisitCount = 0;
        uint32 extractionFeatureProviderVisitCount = 0;
        bool extractionContinuityLost = false;
        /** @brief Engine producer/transport observation is available. */
        bool acceptedExtractionDiagnosticsAvailable = false;
        /** @brief Accepted producer/transport publication provenance. */
        uint64 acceptedExtractionPublicationCount = 0;
        uint64 acceptedExtractionLastSourceFrameSequence = 0;
        uint64 acceptedExtractionLastSceneRevision = 0;
        /** @brief Monotonic work totals from accepted extraction publications. */
        uint64 acceptedExtractionCumulativeFullScanCount = 0;
        uint64 acceptedExtractionCumulativeChangeFeedChangeCount = 0;
        uint64 acceptedExtractionCumulativeActorRebuildCount = 0;
        uint64 acceptedExtractionCumulativeProxyVisitCount = 0;
        uint64 acceptedExtractionCumulativeComponentVisitCount = 0;
        uint64 acceptedExtractionCumulativeFeatureProviderVisitCount = 0;
        uint64 acceptedExtractionContinuityLossCount = 0;
        uint32 gpuDrivenGraphInputDrawItemCount = 0;
        uint32 gpuDrivenVisibilityCandidateCount = 0;
        uint32 gpuDrivenVisibleCullableDrawItemCount = 0;
        uint32 gpuDrivenCpuReferenceVisibleCount = 0;
        uint32 gpuDrivenCpuReferenceCulledCount = 0;
        bool gpuDrivenOpaqueIndirectRequested = false;
        bool gpuDrivenOpaqueIndirectEligible = false;
        bool gpuDrivenOpaqueIndirectSubmitted = false;
        uint32 gpuDrivenOpaqueDirectDrawCount = 0;
        uint32 gpuDrivenOpaqueIndirectBatchCount = 0;
        uint32 gpuDrivenOpaqueIndirectDrawUpperBound = 0;
        std::string gpuDrivenOpaqueFallbackReason;
        SampleRasterTranscript directOpaqueRasterTranscript{};
        SampleDirectRasterReadbackQualification
            directOpaqueRasterReadbackQualification{};
        SampleGPUSceneCullingQualification gpuSceneDepthQualification{};
        SampleGPUSceneCullingQualification gpuSceneOpaqueQualification{};
        /** Exact final frame selected for the one-shot qualification capture. */
        uint64 gpuSceneQualificationTargetFrameSequence = 0;
        uint64 gpuSceneQualificationTargetPublishedSequence = 0;
        uint64 gpuSceneQualificationTargetSubmittedSequence = 0;
        uint64 gpuSceneQualificationTargetPresentedSequence = 0;
        /** Exact final source frame selected for Direct Opaque readback. */
        uint64 directOpaqueRasterReadbackQualificationTargetFrameSequence = 0;
        uint64 directOpaqueRasterReadbackQualificationTargetPublishedSequence = 0;
        uint64 directOpaqueRasterReadbackQualificationTargetSubmittedSequence = 0;
        uint64 directOpaqueRasterReadbackQualificationTargetPresentedSequence = 0;
        bool sceneWorkAvailable = false;
        uint64 sceneFullRebuildCount = 0;
        uint64 sceneIncrementalUpdateCount = 0;
        uint64 sceneStaticReuseCount = 0;
        uint64 sceneAppliedRevision = 0;
        uint32 sceneLastRebuiltObjectCount = 0;
        uint32 sceneLastRemovedObjectCount = 0;
        uint64 drawPacketResolveCount = 0;
        uint64 drawPacketHitCount = 0;
        uint64 drawPacketMissCount = 0;
        uint64 drawPacketDynamicBypassCount = 0;
        uint64 drawPacketBuildCount = 0;
        uint64 drawPacketEntryCreationCount = 0;
        uint64 drawPacketInvalidationCount = 0;
        uint64 drawPacketObjectRevisionInvalidationCount = 0;
        uint64 drawPacketClearCount = 0;
        uint64 drawPacketEntryCount = 0;
        bool gpuSceneAvailable = false;
        bool gpuScenePublicationAttempted = false;
        bool gpuScenePublicationPublished = false;
        bool gpuScenePublicationFailed = false;
        bool gpuScenePublicationComplete = false;
        uint32 gpuSceneAttemptedObjectCount = 0;
        uint32 gpuScenePublishedObjectCount = 0;
        uint32 gpuSceneAddCount = 0;
        uint32 gpuSceneUpdateCount = 0;
        uint32 gpuSceneRemoveCount = 0;
        uint32 gpuSceneNoOpCount = 0;
        uint64 gpuSceneCommittedVersion = 0;
        uint64 gpuSceneResidentVersion = 0;
        uint64 gpuSceneCpuPayloadBytes = 0;
        uint64 gpuSceneCpuReservedBytes = 0;
        uint64 gpuSceneAllocationBytes = 0;
        uint64 gpuSceneFrameUploadBytes = 0;
        uint64 gpuSceneCumulativeUploadBytes = 0;
        uint32 gpuSceneFrameUploadRangeCount = 0;
        uint32 gpuSceneCurrentBufferSetCount = 0;
        uint32 gpuScenePendingBufferSetCount = 0;
        uint32 gpuSceneInFlightBufferSetCount = 0;
        bool gpuSceneFullUpload = false;
        SampleRenderUploadWorkDiagnostics gpuSceneUploadWork{};
        std::array<SampleRenderUploadWorkDiagnostics,
                   GPU_SCENE_DIAGNOSTICS_TABLE_COUNT>
            gpuSceneTableUploadWork{};
        bool renderPolicyPlanAvailable = false;
        bool renderPolicyReportAvailable = false;
        /** @brief Policy request receipt for the rendered frame. */
        bool renderPolicyRequestAvailable = false;
        uint64 renderPolicyRequestFrameSequence = 0;
        uint64 renderPolicyPlanFrameSequence = 0;
        uint64 renderPolicyReportFrameSequence = 0;
        /** @brief Last completed presentation observed by the Engine runtime. */
        uint64 completedPresentedFrameSequence = 0;
        std::string renderPolicyRequestedMode;
        std::string renderPolicyExecutionStatus;
        std::string renderPolicySelectedTier;
        std::string renderPolicyExecutedTier;
        std::string renderPolicyTierFallbackReason;
    };

    /** @brief One resolved asset and its provenance in a sample report. */
    struct SampleReportAsset
    {
        std::string role;
        std::string id;
        std::filesystem::path path;
        std::string kind;
        std::filesystem::path catalogPath;
        std::filesystem::path assetRoot;
        std::string licenseSpdx;
        std::filesystem::path licenseFile;
        std::string sourceName;
        std::string sourceUri;
        std::string author;
        std::string attribution;
        bool redistributable = false;
        bool loaded = false;
        /** Legacy v11 alias for the verified package identity. */
        AssetContentId sourceContentId;
        /** Canonical package identity declared by catalog v3. */
        AssetContentId packageContentId;
        /** Catalog expectation and owner-thread observed receipt are distinct. */
        Resource::ResourceContentIdentity expectedContentIdentity;
        Resource::ResourceContentIdentity observedContentIdentity;
        Resource::ResourceContentVerificationStatus verificationStatus =
            Resource::ResourceContentVerificationStatus::NotRequested;
        bool verified = false;
        bool cookedAdmissionRequired = false;
        bool cookedAdmissionAccepted = false;
        /** True when the catalog admitted only the staged cooked package. */
        bool cookPackageOnly = false;
        /**
         * @brief Runtime domain observed from the exact loader receipt.
         *
         * Values use RuntimeResourcePolicy spelling (for example
         * CookedArtifact); ResourceContentIdentity fields retain their
         * lower-case identity-domain spelling.
         */
        std::string actualLoadDomain = "Unknown";
        std::string cookedAdmissionCode;
        std::string cookedAdmissionDetail;
        std::filesystem::path cookManifestPath;
        std::filesystem::path cookedRoot;
        Resource::ResourceContentIdentity declaredCookSourceContentIdentity;
        Resource::ResourceContentIdentity declaredCookedContentIdentity;
        Resource::ResourceContentIdentity declaredCookManifestContentIdentity;
        Resource::ResourceContentIdentity observedCookSourceContentIdentity;
        Resource::ResourceContentIdentity observedCookedContentIdentity;
        Resource::ResourceContentIdentity observedCookManifestContentIdentity;
        std::string cookSettingsHash;
        std::string cookRecipeHash;
        std::string cookToolName;
        std::string cookToolVersion;
    };

    /** @brief Bounded sample readiness state recorded by the shared host. */
    struct SampleReportReadiness
    {
        bool waitRequested = false;
        bool ready = true;
        uint32 minimumFrames = 0;
        uint32 maximumFrames = 0;
        uint32 timeoutMs = 0;
        std::string reason;
    };

    /**
     * @brief Compact outcome of an optional framework-assessment sidecar report.
     *
     * The complete assessment remains in RVX.FrameworkAssessmentReport.  This
     * value-only summary deliberately keeps SampleCLI independent from the
     * assessment implementation while allowing report consumers to discover
     * the result and its artifact without opening a second file first.
     */
    struct SampleAssessmentSummary
    {
        bool enabled = false;
        std::filesystem::path reportPath;
        std::string blockGrade = "not-run";
        uint32 findingCount = 0;
        uint32 advisoryCount = 0;
        uint32 blockingCount = 0;
        uint64 droppedEventCount = 0;
        bool pass = true;
    };

    /** @brief One completed, scenario-specific action receipt. */
    struct SampleReportScenarioAction
    {
        std::string name;
        uint64 targetSceneRevision = 0;
        uint64 completedPresentationSequence = 0;
        uint64 appliedSceneRevision = 0;
        bool passed = false;
    };

    /** @brief One scenario invariant and the evidence supporting its result. */
    struct SampleReportScenarioInvariant
    {
        std::string name;
        bool passed = false;
        std::string evidence;
    };

    /** @brief One named, scalar scenario metric. */
    struct SampleReportScenarioMetric
    {
        std::string name;
        uint64 value = 0;
    };

    /** @brief Schema-v12 scenario contract receipts emitted by a sample. */
    struct SampleReportScenario
    {
        /** Zero denotes a report-only producer without a scenario contract. */
        uint32 contractRevision = 0;
        std::string phase = "not-applicable";
        std::vector<SampleReportScenarioAction> actions;
        std::vector<SampleReportScenarioInvariant> invariants;
        std::vector<SampleReportScenarioMetric> metrics;
    };

    /**
     * @brief Completion-qualified evidence for one Sample visual contract.
     *
     * Fixed time remains an exact integer rational. An absent observed
     * pipeline is serialized as "unavailable" and cannot satisfy report
     * validation. Schema v21 additionally binds this contract to the exact
     * presentation-qualified renderer execution receipt.
     */
    struct SampleVisualContractReceipt
    {
        std::string manifestSchema = "RVX.SampleManifest";
        uint32 manifestSchemaVersion = 0;
        std::string manifestContentSha256;
        uint32 visualContractVersion = 0;
        SampleDomain domain = SampleDomain::Rendering;
        SampleIntent intent = SampleIntent::Demonstration;
        SamplePipelineRecipe requestedPipeline =
            SamplePipelineRecipe::Minimal;
        std::optional<SamplePipelineRecipe> observedPipeline;
        SampleQualityTier quality = SampleQualityTier::Default;
        std::string cameraBookmarkId;
        uint32 fixedTimeNumerator = 0;
        uint32 fixedTimeDenominator = 0;
        uint32 deterministicSeed = 0;
        uint32 captureTick = 0;
        uint64 captureCarryingFrameSequence = 0;
        /** @brief Independently observed capture identity for receipt binding. */
        uint64 captureSceneRuntimeId = 0;
        uint64 captureCameraEntityPacked = 0;
        uint64 captureSceneRevision = 0;
        uint64 captureSurfaceGeneration = 0;
        uint64 captureConfigurationRevision = 0;
        bool deterministicQualification = false;
        std::optional<RenderExecutionReceipt> renderExecutionReceipt;
        bool satisfied = false;
    };

    /** @brief Optional raw ToneMapping input/final-output pixel receipt. */
    struct SamplePixelProbeReport
    {
        bool complete = false;
        uint32 resultCode = 0;
        uint64 requestId = 0;
        uint64 frameSequence = 0;
        uint64 requiredSceneRevision = 0;
        uint64 runtimeSurfaceGeneration = 0;
        uint32 x = 0;
        uint32 y = 0;
        std::array<uint16, 4> preToneRGBA16FloatBits{};
        std::array<uint8, 4> finalBGRA8Bits{};
        std::string message;
    };

    /** @brief Pure-value ECS runtime evidence captured by SampleRunner. */
    struct SampleEcsRuntimeDiagnostics
    {
        bool available = false;
        uint64 sceneRuntimeId = 0;
        uint64 sceneFrameSequence = 0;
        uint64 sceneFixedStepSequence = 0;
        uint64 sceneSnapshotRevision = 0;
        uint32 entityCount = 0;
        uint32 pendingDestroyEntityCount = 0;
        uint32 cleanupRequiredEntityCount = 0;
        uint32 retiringEntityCount = 0;
        uint32 recyclableEntityCount = 0;
        uint32 spatialEntryCount = 0;
        uint64 nextStructuralJournalSequence = 0;
        uint64 nextCleanupJournalSequence = 0;
        uint64 cleanupJournalContinuityLossCount = 0;
        uint32 queuedCommandBufferCount = 0;
        uint64 appliedCommandBufferCount = 0;
        uint64 rejectedCommandBufferCount = 0;
        uint64 localTransformWriteVersion = 0;
        uint64 renderWorldTransformWriteVersion = 0;
        uint32 physicsBodySideTableEntryCount = 0;
        uint32 animationBindingSideTableEntryCount = 0;
        uint32 resourceAnimationPlaybackSideTableEntryCount = 0;
        uint32 audioPlaybackSideTableEntryCount = 0;
        uint32 scriptInstanceSideTableEntryCount = 0;
        uint32 particleFeatureSnapshotCount = 0;
        uint32 waterFeatureSnapshotCount = 0;
        uint32 terrainFeatureSnapshotCount = 0;
        uint32 trackedModelRequestCount = 0;
        uint32 trackedEnvironmentRequestCount = 0;
        uint32 trackedAnimationRequestCount = 0;
        /** @brief Must remain zero; no compatibility scene path exists. */
        uint64 legacyFallbackCount = 0;
    };

    struct SampleReport
    {
        std::string schemaId = "RVX.SampleReport";
        uint32 schemaVersion = RVX_SAMPLE_REPORT_SCHEMA_VERSION;
        /** Opaque required-suite identity; empty outside validation runners. */
        std::string validationRunId;
        std::string sampleName;
        std::string category = "sample";
        RHIBackendType requestedBackend = RHIBackendType::Auto;
        RHIBackendType backend = RHIBackendType::Auto;
        uint32 frameCount = 0;
        uint64 submittedFrameSequence = 0;
        uint64 presentedFrameSequence = 0;
        uint32 width = 0;
        uint32 height = 0;
        std::string quality = "default";
        /** Compatibility alias; always identical to requestedRenderPath. */
        std::string renderPath = "auto";
        /** Path configured by the Sample host before the scene is created. */
        std::string requestedRenderPath = "auto";
        /** Completion-qualified path executed by the reported presented frame. */
        std::string actualRenderPath = "unavailable";
        /** Live PhysicsWorld backend receipt, or unavailable without a subsystem. */
        std::string requestedPhysicsBackend = "unavailable";
        std::string actualPhysicsBackend = "unavailable";
        bool physicsBackendFallbackActive = false;
        bool physicsDiagnosticsAvailable = false;
        bool diagnostics = false;
        std::filesystem::path screenshotPath;
        std::string assetId;
        std::filesystem::path assetPath;
        std::string assetKind;
        std::filesystem::path catalogPath;
        std::filesystem::path assetRoot;
        std::string assetLicenseSpdx;
        std::filesystem::path assetLicenseFile;
        std::string assetSourceName;
        std::string assetSourceUri;
        std::string assetAuthor;
        std::string assetAttribution;
        bool assetRedistributable = false;
        bool assetLoaded = false;
        std::vector<SampleReportAsset> assets;
        SampleReportReadiness readiness;
        SampleAssessmentSummary assessment;
        SampleReportScenario scenario;
        std::optional<SampleVisualContractReceipt> visualContractReceipt;
        std::vector<std::string> enabledFeatures;
        std::vector<std::string> unsupportedFeatures;
        std::vector<std::string> fallbackReasons;
        std::vector<std::string> resourceDiagnostics;
        SampleEcsRuntimeDiagnostics ecsDiagnostics;
        SampleRenderDiagnostics renderDiagnostics;
        std::optional<SamplePixelProbeReport> pixelProbe;
        bool pass = false;
    };

    struct SampleAppDesc
    {
        std::string sampleName;
        std::string category = "sample";
        std::vector<std::string> enabledFeatures;
        std::vector<std::string> unsupportedFeatures;
        std::vector<std::string> fallbackReasons;
        std::vector<std::string> resourceDiagnostics;
        SampleRenderDiagnostics renderDiagnostics;
        bool supportsScreenshot = false;
        bool supportsQualityProfiles = false;
    };

    struct SampleRunContext
    {
        SampleCLIOptions options;
        RHIBackendType resolvedBackend = RHIBackendType::Auto;
        uint32 frameCount = 0;
    };

    class SampleFeatureReporter
    {
    public:
        explicit SampleFeatureReporter(SampleReport& report);

        void Enable(std::string feature);
        void Unsupported(std::string feature);
        void Fallback(std::string reason);
        void ResourceDiagnostic(std::string diagnostic);
        void SetScenarioContractRevision(uint32 contractRevision);
        void SetScenarioPhase(std::string phase);
        bool AppendScenarioAction(SampleReportScenarioAction action);
        bool AppendScenarioInvariant(SampleReportScenarioInvariant invariant);
        bool AppendScenarioMetric(SampleReportScenarioMetric metric);

    private:
        SampleReport* m_report = nullptr;
    };
    bool ParseSampleBackend(const std::string& text, RHIBackendType& outBackend);
    const char* GetSampleBackendName(RHIBackendType backend);

    bool ParseSampleCLI(int argc,
                        const char* const* argv,
                        SampleCLIOptions& options,
                        std::string* outError = nullptr);

    void PrintSampleCLIUsage(std::ostream& stream, const char* executableName);

    void WriteSampleReportJson(std::ostream& stream, const SampleReport& report);
    bool WriteSampleReportJson(const SampleReport& report,
                               const std::filesystem::path& path,
                               std::string* outError = nullptr);

    /** @brief Required GPU culling lanes must all reach post-fence comparison. */
    [[nodiscard]] bool HasRequiredGPUCullingQualificationComparison(
        const SampleRenderDiagnostics& diagnostics) noexcept;
    /** @brief Required GPU culling lanes must all match both evidence layers. */
    [[nodiscard]] bool HasRequiredGPUCullingQualificationMatch(
        const SampleRenderDiagnostics& diagnostics) noexcept;

    /**
     * @brief Project renderer-owned GFX14 evidence into the public Sample ABI.
     *
     * These functions fail closed for a non-presented or non-schema-v11
     * receipt.  Sample scenarios must not manufacture the corresponding
     * values from settings, assets, or image output.
     */
    [[nodiscard]] SampleToonLightingExecutionDiagnostics
        ProjectSampleToonLightingExecutionDiagnostics(
            const RenderDiagnosticsSnapshot& renderDiagnostics);
    [[nodiscard]] SampleToonOutlineExecutionDiagnostics
        ProjectSampleToonOutlineExecutionDiagnostics(
            const RenderDiagnosticsSnapshot& renderDiagnostics);

    /** Terminal state of the exact presented Toon-outline qualification target. */
    enum class SampleToonOutlineQualificationTargetState : uint8
    {
        Unrelated = 0,
        Pending,
        Ready,
        Failed,
    };

    /** Fail-closed target evaluation used by the host completion-only pump. */
    struct SampleToonOutlineQualificationTargetEvaluation
    {
        SampleToonOutlineQualificationTargetState state =
            SampleToonOutlineQualificationTargetState::Unrelated;
        std::string diagnostic;

        [[nodiscard]] bool IsReady() const noexcept
        {
            return state ==
                SampleToonOutlineQualificationTargetState::Ready;
        }

        [[nodiscard]] bool IsFailed() const noexcept
        {
            return state ==
                SampleToonOutlineQualificationTargetState::Failed;
        }
    };

    /**
     * @brief Evaluate only the exact frame/view receipt selected for capture.
     *
     * A matching receipt with a terminal renderer diagnostic, completed but
     * semantically invalid pixel evidence, or a missing requested probe is a
     * failure.  Only an exact matching receipt without terminal evidence may
     * remain pending.
     */
    [[nodiscard]] SampleToonOutlineQualificationTargetEvaluation
        EvaluateSampleToonOutlineQualificationTarget(
            const RenderDiagnosticsSnapshot& renderDiagnostics,
            uint64 targetFrameSequence);

    SampleReport BuildSampleReport(const SampleAppDesc& desc, const SampleRunContext& context);
    int RunReportOnlySample(int argc, char* argv[], const SampleAppDesc& desc);
} // namespace RVX
