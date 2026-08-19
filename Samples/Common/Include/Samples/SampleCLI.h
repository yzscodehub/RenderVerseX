#pragma once

/**
 * @file SampleCLI.h
 * @brief Shared command-line and JSON report helpers for sample applications.
 */

#include "Core/Diagnostics/DiagnosticValue.h"
#include "Core/Types.h"
#include "Render/GPUScene/GPUSceneDiagnostics.h"
#include "Render/RenderDiagnostics.h"
#include "RenderContracts/RenderIdentity.h"
#include "RHI/RHIDefinitions.h"
#include "Resource/ResourceContentIdentity.h"
#include "Samples/SampleAssetCatalog.h"

#include <array>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace RVX
{
    /** @brief Current JSON schema version written by sample report producers. */
    inline constexpr uint32 RVX_SAMPLE_REPORT_SCHEMA_VERSION = 19;
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
        uint32 width = 1280;
        uint32 height = 720;
        std::string quality = "default";
        bool diagnostics = false;
        bool enableValidation = true;
        bool showHelp = false;
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

    SampleReport BuildSampleReport(const SampleAppDesc& desc, const SampleRunContext& context);
    int RunReportOnlySample(int argc, char* argv[], const SampleAppDesc& desc);
} // namespace RVX
