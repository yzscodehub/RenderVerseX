#pragma once

/**
 * @file RenderDiagnostics.h
 * @brief Immutable value-only render runtime diagnostics contract.
 */

#include "Core/Diagnostics/DiagnosticValue.h"
#include "Render/Context/RenderFrameTiming.h"
#include "Render/Material/MaterialSystem.h"
#include "Render/RenderRuntimeTypes.h"
#include "Render/RenderUploadWorkDiagnostics.h"
#include "Render/GPUDriven/GPUDrivenDiagnostics.h"
#include "Render/GPUDriven/GPUDrivenPolicy.h"
#include "Render/GPUScene/GPUSceneDiagnostics.h"
#include "Render/Policy/RenderPolicyDiagnostics.h"
#include "RenderContracts/RenderIdentity.h"
#include "RenderContracts/RenderFrameTypes.h"

#include <array>
#include <string>
#include <vector>

namespace RVX
{
    inline constexpr uint8 RVX_RENDER_DIAGNOSTICS_UNAVAILABLE_INDEX = 0xFFU;
    inline constexpr uint32 RVX_RENDER_DIAGNOSTICS_TRANSITION_CAPACITY = 8U;
    inline constexpr uint32 RVX_RENDER_PRESENTED_MATERIAL_RECEIPT_CAPACITY = 512U;
    inline constexpr uint32 RVX_RENDER_PRESENTED_SKINNING_PALETTE_RECEIPT_CAPACITY =
        512U;

    /** @brief Raster lane which actually uploaded and consumed a palette. */
    enum class RenderSkinningPaletteExecutionLane : uint8
    {
        Direct = 0,
    };

    [[nodiscard]] inline const char* GetRenderSkinningPaletteExecutionLaneName(
        RenderSkinningPaletteExecutionLane lane) noexcept
    {
        switch (lane)
        {
            case RenderSkinningPaletteExecutionLane::Direct: return "Direct";
            default: return "Invalid";
        }
    }

    /** @brief Completion-qualified receipt for a fully uploaded skinning palette. */
    struct RenderPresentedSkinningPaletteReceipt
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

    /**
     * @brief Exact material generation and registry revision used by a frame
     * which completed presentation.  This is intentionally value-only and
     * cannot expose a mutable RHI descriptor or Resource object.
     */
    struct RenderPresentedMaterialBindingReceipt
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

    /** @brief Durable retained-scene mutation work, accumulated by RenderScene. */
    struct RenderSceneMutationTotals
    {
        uint64 fullRebuildCount = 0;
        uint64 incrementalCommitCount = 0;
        uint64 rebuiltObjectCount = 0;
        uint64 removedObjectCount = 0;
    };

    /** @brief Durable CPU GPU-scene publication work, accumulated by GPUSceneUpdate. */
    struct GPUScenePublicationMutationTotals
    {
        uint64 fullPublicationCount = 0;
        uint64 incrementalPublicationCount = 0;
        uint64 identityOnlyPublicationCount = 0;
        uint64 materializedObjectCount = 0;
        uint64 addCount = 0;
        uint64 updateCount = 0;
        uint64 removeCount = 0;
        uint64 noOpCount = 0;
    };

    /** @brief Durable resident GPU-scene upload work, accumulated after submission evidence. */
    struct GPUSceneUploadMutationTotals
    {
        uint64 submittedUploadCount = 0;
        uint64 uploadBytes = 0;
        uint64 uploadRangeCount = 0;
        uint64 fullUploadCount = 0;
        /** Exact rows copied by accepted submissions, indexed by GPUSceneDiagnosticsTable. */
        std::array<uint64, GPU_SCENE_DIAGNOSTICS_TABLE_COUNT>
            submittedUploadedRowCount{};
    };

    /** @brief Durable canonical and resident GPU-culling stream work. */
    struct GPUCullingCanonicalMutationTotals
    {
        uint64 instancePatchedRowCount = 0;
        uint64 candidatePatchedRowCount = 0;
        uint64 activeRowPatchedRowCount = 0;
        uint64 instanceUploadedRowCount = 0;
        uint64 candidateUploadedRowCount = 0;
        uint64 activeRowUploadedRowCount = 0;
        uint64 instanceUploadBytes = 0;
        uint64 candidateUploadBytes = 0;
        uint64 activeRowUploadBytes = 0;
        uint64 instanceFullMaterializationCount = 0;
        uint64 candidateFullMaterializationCount = 0;
        uint64 activeRowFullMaterializationCount = 0;
        uint64 continuityFullMaterializationCount = 0;
        uint64 capacityFullMaterializationCount = 0;
    };

    /** @brief Durable Direct raster stream work, accumulated by each persistent cache. */
    struct DirectRasterMutationTotals
    {
        uint64 instancePatchedRowCount = 0;
        uint64 indexPatchedRowCount = 0;
        uint64 instanceUploadBytes = 0;
        uint64 indexUploadBytes = 0;
        uint64 instanceFullMaterializationCount = 0;
        uint64 indexFullMaterializationCount = 0;
    };

    /** @brief Completion-qualified immutable boundary for cumulative mutation evidence. */
    struct RenderMutationCompletionWatermark
    {
        bool available = false;
        bool saturated = false;
        uint64 evidenceEpoch = 0;
        /** Monotonic count of successful accepted-frame presentation boundaries. */
        uint64 completedPresentationCount = 0;
        uint64 completedFrameSequence = 0;
        uint64 requiredSceneRevision = 0;
        uint64 appliedSceneRevision = 0;
    };

    /**
     * @brief Owner-owned mutation evidence frozen only after a successful Present.
     *
     * Per-frame diagnostics remain useful for local debugging but may be skipped
     * by the latest-only transport. These monotonic counters preserve committed
     * work across those skipped frames.
     */
    struct RenderMutationEvidenceDiagnostics
    {
        RenderMutationCompletionWatermark completion{};
        uint32 gpuCullingOwnerCount = 0;
        uint32 directRasterOwnerCount = 0;
        RenderSceneMutationTotals scene{};
        GPUScenePublicationMutationTotals gpuScenePublication{};
        GPUSceneUploadMutationTotals gpuSceneUpload{};
        GPUCullingCanonicalMutationTotals gpuCulling{};
        DirectRasterMutationTotals directRaster{};
    };

    struct RenderLifecycleTransitionDiagnostics
    {
        RenderLifecycleState from = RenderLifecycleState::Stopped;
        RenderLifecycleState to = RenderLifecycleState::Stopped;
        uint64 sequence = 0;
        std::string message{};
    };

    struct RenderFrameTransportDiagnostics
    {
        uint32 capacity = 0;
        uint32 currentUsage = 0;
        uint32 highWaterMark = 0;
        uint64 replacements = 0;
        uint64 surfaceIncompatibleDrops = 0;
        uint64 invalidPackets = 0;
        uint64 outOfOrderRejections = 0;
    };

    struct RenderUploadTransportDiagnostics
    {
        uint32 requestCapacity = 0;
        uint64 byteCapacity = 0;
        uint32 currentRequestUsage = 0;
        uint64 currentByteUsage = 0;
        uint32 requestHighWaterMark = 0;
        uint64 byteHighWaterMark = 0;
        uint64 pressureOutcomes = 0;
        uint64 acceptedCount = 0;
        uint64 completedCount = 0;
        uint64 failedCount = 0;
    };

    struct RenderReleaseTransportDiagnostics
    {
        uint32 capacity = 0;
        uint32 currentUsage = 0;
        uint32 highWaterMark = 0;
        uint64 acceptedCount = 0;
        uint64 completedCount = 0;
        uint64 staleCount = 0;
        uint32 oldestPendingGeneration = 0;
    };

    struct RenderResourceDiagnostics
    {
        std::array<uint64, 7> stateCounts{};
        uint64 staleGenerationAttempts = 0;
        uint64 fallbackCount = 0;
        uint64 skippedDrawCount = 0;
    };

    /**
     * @brief Render-owned scene values sampled at successful v5 consumption.
     *
     * This is intentionally not a live Update-thread view. All fields describe
     * the packet and persistent scene database consumed at one owner-thread
     * boundary.
     */
    struct RenderSceneValueDiagnostics
    {
        bool available = false;
        uint64 frameSequence = 0;
        uint64 appliedSceneRevision = 0;
        uint64 requiredSceneRevision = 0;
        uint32 objectCount = 0;
        uint32 lightCount = 0;
        uint64 lightStateHash = 0;
    };

    /** @brief CPU durations captured around one successfully presented frame. */
    struct RenderCpuFramePhaseDurations
    {
        uint64 frameApply = 0;
        uint64 frameBeginAcquire = 0;
        uint64 renderPrepare = 0;
        uint64 policy = 0;
        uint64 graphBuild = 0;
        uint64 graphCompile = 0;
        uint64 graphRealizeRecord = 0;
        uint64 frameEndSubmit = 0;
        uint64 present = 0;
        uint64 acceptedFrameTotal = 0;
    };

    /**
     * @brief Owner-thread CPU timing for one complete accepted frame.
     *
     * Availability belongs to the whole coherent phase set. A measured zero is
     * valid, while an unavailable value always carries a diagnostic reason.
     */
    struct RenderCpuFrameTimingDiagnostics
    {
        uint64 sourceFrameSequence = 0;
        uint64 requiredSceneRevision = 0;
        uint64 appliedSceneRevision = 0;
        DiagnosticValue<RenderCpuFramePhaseDurations> phases =
            DiagnosticValue<RenderCpuFramePhaseDurations>::Unavailable(
                "No successfully presented frame CPU timing sample is available.");
    };

    struct RenderQueueTimelineDiagnostics
    {
        bool available = false;
        uint8 logicalQueue = RVX_RENDER_DIAGNOSTICS_UNAVAILABLE_INDEX;
        uint8 physicalDomain = RVX_RENDER_DIAGNOSTICS_UNAVAILABLE_INDEX;
        uint8 completionMode = RVX_RENDER_DIAGNOSTICS_UNAVAILABLE_INDEX;
        uint8 timelineState = RVX_RENDER_DIAGNOSTICS_UNAVAILABLE_INDEX;
        uint64 lastSubmittedValue = 0;
        uint64 lastCompletedValue = 0;
    };

    struct RenderRetirementDiagnostics
    {
        uint32 entryCount = 0;
        uint64 estimatedBytes = 0;
        std::array<uint64, 3> oldestPendingCompletionValues{};
    };

    /** @brief Value-only CPU descriptor allocator telemetry. */
    struct RenderDescriptorAllocatorDiagnostics
    {
        uint32 currentPages = 0;
        uint32 peakPages = 0;
        uint32 activeDescriptors = 0;
        uint32 peakActiveDescriptors = 0;
        uint64 allocationFailures = 0;
        uint64 validationFailures = 0;
    };

    /** @brief Descriptor families relevant to sustained frame qualification. */
    struct RenderDescriptorLifetimeDiagnostics
    {
        RenderDescriptorAllocatorDiagnostics resourceViews{};
        RenderDescriptorAllocatorDiagnostics samplers{};
        RenderDescriptorAllocatorDiagnostics renderTargets{};
        RenderDescriptorAllocatorDiagnostics depthStencils{};
    };

    /** @brief Value-only native API validation/debug-layer telemetry. */
    struct RenderNativeValidationDiagnostics
    {
        bool available = false;
        bool enabled = false;
        bool readComplete = true;
        uint64 messageCount = 0;
        uint64 warningCount = 0;
        uint64 errorCount = 0;
        uint64 corruptionCount = 0;
    };

    /**
     * @brief Immutable post-submit RenderGraph lifetime snapshot.
     *
     * Unlike per-plan diagnostics, these values are sampled from the live
     * transient pool and RHI device after frame submission. They are therefore
     * suitable for detecting allocation, lease, view, and descriptor growth
     * across a sustained run without exposing RenderGraph or RHI objects to
     * the Update thread.
     */
    struct RenderGraphLifetimeDiagnostics
    {
        bool available = false;
        uint64 frameSequence = 0;
        uint64 planHash = 0;
        uint32 physicalRealizationCount = 0;
        uint32 partialRealizationRollbackCount = 0;

        uint32 physicalTextureAllocationCount = 0;
        uint32 physicalBufferAllocationCount = 0;
        uint64 totalPooledMemoryBytes = 0;
        uint32 texturePoolMissCount = 0;
        uint32 bufferPoolMissCount = 0;
        uint32 transientViewCount = 0;
        uint32 transientViewMissCount = 0;
        uint64 transientViewCreationFailureCount = 0;

        uint32 recordingTextureLeases = 0;
        uint32 recordingBufferLeases = 0;
        uint32 inFlightTextureLeases = 0;
        uint32 inFlightBufferLeases = 0;
        uint64 leaseCommitCount = 0;
        uint64 leaseAbortCount = 0;
        uint64 leaseDeviceLostCount = 0;
        uint64 leaseValidationFailureCount = 0;
        uint64 completionRetirementCount = 0;

        RenderDescriptorLifetimeDiagnostics descriptors{};
    };

    struct RenderFailureDiagnostics
    {
        bool available = false;
        RenderRuntimeResult runtime{};
        RenderShutdownResult shutdown{};
        std::string context{};
    };

    enum class RenderFrameCaptureResultCode : uint8
    {
        None = 0,
        Completed = 1,
        UnsupportedKind = 2,
        UnsupportedExtent = 3,
        UnsupportedFormat = 4,
        ResourceCreationFailed = 5,
        MapFailed = 6
    };

    /** @brief Completion result for an opt-in ToneMapping input pixel probe. */
    enum class RenderFramePixelProbeResultCode : uint8
    {
        None = 0,
        Completed = 1,
        InvalidRequest = 2,
        OutOfBounds = 3,
        UnsupportedFormat = 4,
        ResourceCreationFailed = 5,
        NotRecorded = 6,
        ForeignFrame = 7,
        MapFailed = 8,
        FinalCaptureUnavailable = 9
    };

    /**
     * @brief Raw, completion-qualified values for one screen-space pixel.
     *
     * The HDR values are the four IEEE-754 binary16 channel bit patterns from
     * the RGBA16_FLOAT input consumed by ToneMapping. The final values are the
     * four bytes from the matching BGRA8 screenshot readback. No color-space
     * conversion is performed when publishing either array.
     */
    struct RenderFramePixelProbeResult
    {
        RenderFramePixelProbeResultCode code =
            RenderFramePixelProbeResultCode::None;
        uint64 requestId = 0;
        uint64 frameSequence = 0;
        uint64 requiredSceneRevision = 0;
        uint64 runtimeSurfaceGeneration = 0;
        uint32 x = 0;
        uint32 y = 0;
        RHIFormat preToneFormat = RHIFormat::Unknown;
        std::array<uint16, 4> preToneRGBA16FloatBits{};
        std::array<uint8, 4> finalBGRA8Bits{};
        std::string message{};

        [[nodiscard]] bool IsComplete() const noexcept
        {
            return code == RenderFramePixelProbeResultCode::Completed &&
                   requestId != 0 && frameSequence != 0 &&
                   requiredSceneRevision != 0 &&
                   runtimeSurfaceGeneration != 0 &&
                   preToneFormat == RHIFormat::RGBA16_FLOAT;
        }
    };

    /** @brief Owned capture result published from Render to Update/Tools. */
    struct RenderFrameCaptureResult
    {
        RenderFrameCaptureResultCode code =
            RenderFrameCaptureResultCode::None;
        uint64 requestId = 0;
        uint64 frameSequence = 0;
        RenderFrameCaptureKind kind = RenderFrameCaptureKind::None;
        uint32 width = 0;
        uint32 height = 0;
        uint32 rowPitch = 0;
        uint32 bytesPerPixel = 0;
        RHIFormat format = RHIFormat::Unknown;
        bool originBottomLeft = false;
        std::vector<uint8> bytes{};
        RenderFramePixelProbeResult pixelProbe{};
        std::string message{};

        [[nodiscard]] bool IsComplete() const noexcept
        {
            return code == RenderFrameCaptureResultCode::Completed &&
                   requestId != 0 && frameSequence != 0 && width != 0 &&
                   height != 0 && rowPitch != 0 && bytesPerPixel != 0 &&
                   !bytes.empty();
        }

        /** @brief Verify that a probe and final screenshot came from one frame. */
        [[nodiscard]] bool HasMatchingCompletedPixelProbe() const noexcept
        {
            return IsComplete() && pixelProbe.IsComplete() &&
                   pixelProbe.requestId == requestId &&
                   pixelProbe.frameSequence == frameSequence;
        }
    };

    struct RenderPassFeatureDiagnostics
    {
        bool requested = false;
        bool supported = false;
        bool enabled = false;
        std::string reason{};
    };

    struct RenderDirectionalShadowDiagnostics
    {
        bool available = false;
        bool requested = false;
        bool supported = false;
        bool outputReady = false;
        bool samplingEnabled = false;
        uint32 requestedCascadeCount = 0;
        uint32 producedCascadeCount = 0;
        uint32 resolvedCascadeCount = 0;
        uint32 shadowMapSize = 0;
        uint32 shadowCasterCount = 0;
        uint32 drawCount = 0;
        std::string reason{};
    };

    /** @brief Value-only texture IBL request and binding outcome. */
    struct RenderEnvironmentIBLDiagnostics
    {
        bool requested = false;
        bool enabled = false;
        uint32 prefilteredMipLevels = 1;
        float32 intensity = 1.0f;
        std::string reason{};
    };

    /**
     * @brief Whether a requested texture-IBL binding failed and must appear in
     * aggregate fallback diagnostics.
     */
    [[nodiscard]] inline bool HasEnvironmentIBLFallback(
        const RenderEnvironmentIBLDiagnostics& diagnostics) noexcept
    {
        return diagnostics.requested && !diagnostics.enabled &&
               !diagnostics.reason.empty();
    }

    /** @brief Value-only local-light admission and shadow-capability state. */
    struct RenderLocalLightingDiagnostics
    {
        bool available = false;
        uint32 pointLightRequested = 0;
        uint32 pointLightAdmitted = 0;
        uint32 pointLightCapacity = 0;
        uint32 pointLightOverflow = 0;
        uint32 spotLightRequested = 0;
        uint32 spotLightAdmitted = 0;
        uint32 spotLightCapacity = 0;
        uint32 spotLightOverflow = 0;
        uint32 pointShadowRequested = 0;
        bool pointShadowSupported = false;
        std::string pointShadowReason{};
        uint32 spotShadowRequested = 0;
        bool spotShadowSupported = false;
        std::string spotShadowReason{};
    };

    /** @brief Value-only transparent blending order and Direct-lane telemetry. */
    struct RenderTransparentPassDiagnostics
    {
        bool available = false;
        bool orderValid = true;
        uint64 orderHash = 0;
        uint32 rejectedNonFiniteDepthCount = 0;
        uint32 candidateDrawItemCount = 0;
        uint32 preparedDrawItemCount = 0;
        uint32 executedPacketCount = 0;
        uint32 executedDrawCount = 0;
        uint32 skippedMaterialBindingCount = 0;
        uint32 skippedResourceCount = 0;
        uint32 skippedExecutionDrawCount = 0;
        uint32 materialBindingCount = 0;
        uint32 materialFallbackBindingCount = 0;
        uint32 materialTextureFlags = 0;
        uint32 materialFallbackTextureFlags = 0;
        bool noWork = false;
        bool preflightFailed = false;
        bool executionFailed = false;
    };

    /** @brief First semantic stage whose GPU-scene culling readback diverged. */
    enum class GPUSceneCullingQualificationMismatch : uint8
    {
        None = 0,
        NotRequested,
        ReferenceUnavailable,
        ReadbackAllocationFailed,
        CopyNotRecorded,
        CompletionRejected,
        CompletionPending,
        CompletionLost,
        ReadbackMapFailed,
        InputCoverage,
        DirectVisibilityCoverage,
        Visibility,
        CompactedResidentRows,
        InstanceCounts,
        DrawCounts,
        IndirectArguments,
        RasterPayload,
    };

    /**
     * @brief Ordered and multiset, cross-process comparable raster evidence.
     *
     * Values summarize the payload actually dereferenced by raster without
     * retaining per-frame entry arrays. Implementations exclude frame-local
     * indices, physical resident rows, and resource-handle generations.
     */
    struct RasterTranscriptDigest
    {
        bool available = false;
        uint32 entryCount = 0;
        uint64 orderedIdentityHash = 0;
        uint64 consumedPayloadHash = 0;
        /** Commutative primary accumulator; duplicates contribute repeatedly. */
        uint64 unorderedIdentityHash = 0;
        /** Independent commutative identity accumulator for collision resistance. */
        uint64 unorderedIdentityHashSecondary = 0;
        /** Commutative primary payload accumulator; duplicates contribute repeatedly. */
        uint64 unorderedConsumedPayloadHash = 0;
        /** Independent commutative payload accumulator for collision resistance. */
        uint64 unorderedConsumedPayloadHashSecondary = 0;
    };

    /** @brief First Direct Opaque physical-readback stage that diverged. */
    enum class DirectRasterReadbackQualificationMismatch : uint8
    {
        None = 0,
        NotRequested,
        DirectLaneUnavailable,
        UnsupportedNonInstancedDraw,
        IdentityRejected,
        ReferenceUnavailable,
        ReadbackAllocationFailed,
        CopyNotRecorded,
        CompletionRejected,
        CompletionPending,
        CompletionLost,
        ReadbackMapFailed,
        InstanceIndexPayload,
        InstancePayload,
        RasterTranscript,
    };

    [[nodiscard]] inline const char*
    GetDirectRasterReadbackQualificationMismatchName(
        DirectRasterReadbackQualificationMismatch mismatch) noexcept
    {
        switch (mismatch)
        {
            case DirectRasterReadbackQualificationMismatch::None: return "None";
            case DirectRasterReadbackQualificationMismatch::NotRequested:
                return "NotRequested";
            case DirectRasterReadbackQualificationMismatch::DirectLaneUnavailable:
                return "DirectLaneUnavailable";
            case DirectRasterReadbackQualificationMismatch::UnsupportedNonInstancedDraw:
                return "UnsupportedNonInstancedDraw";
            case DirectRasterReadbackQualificationMismatch::IdentityRejected:
                return "IdentityRejected";
            case DirectRasterReadbackQualificationMismatch::ReferenceUnavailable:
                return "ReferenceUnavailable";
            case DirectRasterReadbackQualificationMismatch::ReadbackAllocationFailed:
                return "ReadbackAllocationFailed";
            case DirectRasterReadbackQualificationMismatch::CopyNotRecorded:
                return "CopyNotRecorded";
            case DirectRasterReadbackQualificationMismatch::CompletionRejected:
                return "CompletionRejected";
            case DirectRasterReadbackQualificationMismatch::CompletionPending:
                return "CompletionPending";
            case DirectRasterReadbackQualificationMismatch::CompletionLost:
                return "CompletionLost";
            case DirectRasterReadbackQualificationMismatch::ReadbackMapFailed:
                return "ReadbackMapFailed";
            case DirectRasterReadbackQualificationMismatch::InstanceIndexPayload:
                return "InstanceIndexPayload";
            case DirectRasterReadbackQualificationMismatch::InstancePayload:
                return "InstancePayload";
            case DirectRasterReadbackQualificationMismatch::RasterTranscript:
                return "RasterTranscript";
        }
        return "Unknown";
    }

    /** @brief Completion-qualified Direct Opaque physical stream evidence. */
    struct DirectRasterReadbackQualificationDiagnostics
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
        DirectRasterReadbackQualificationMismatch mismatch =
            DirectRasterReadbackQualificationMismatch::NotRequested;
        uint64 frameSequence = 0;
        uint64 recordEpoch = 0;
        uint32 sourceFrameSlot = RVX_INVALID_INDEX;
        uint32 firstMismatchIndex = RVX_INVALID_INDEX;
        uint32 firstMismatchRow = RVX_INVALID_INDEX;
        uint64 cpuPayloadBytes = 0;
        uint64 completionValue = 0;
        RasterTranscriptDigest expectedTranscript{};
        RasterTranscriptDigest observedTranscript{};
    };

    [[nodiscard]] inline const char* GetGPUSceneCullingQualificationMismatchName(
        GPUSceneCullingQualificationMismatch mismatch) noexcept
    {
        switch (mismatch)
        {
            case GPUSceneCullingQualificationMismatch::None: return "None";
            case GPUSceneCullingQualificationMismatch::NotRequested:
                return "NotRequested";
            case GPUSceneCullingQualificationMismatch::ReferenceUnavailable:
                return "ReferenceUnavailable";
            case GPUSceneCullingQualificationMismatch::ReadbackAllocationFailed:
                return "ReadbackAllocationFailed";
            case GPUSceneCullingQualificationMismatch::CopyNotRecorded:
                return "CopyNotRecorded";
            case GPUSceneCullingQualificationMismatch::CompletionRejected:
                return "CompletionRejected";
            case GPUSceneCullingQualificationMismatch::CompletionPending:
                return "CompletionPending";
            case GPUSceneCullingQualificationMismatch::CompletionLost:
                return "CompletionLost";
            case GPUSceneCullingQualificationMismatch::ReadbackMapFailed:
                return "ReadbackMapFailed";
            case GPUSceneCullingQualificationMismatch::InputCoverage:
                return "InputCoverage";
            case GPUSceneCullingQualificationMismatch::DirectVisibilityCoverage:
                return "DirectVisibilityCoverage";
            case GPUSceneCullingQualificationMismatch::Visibility:
                return "Visibility";
            case GPUSceneCullingQualificationMismatch::CompactedResidentRows:
                return "CompactedResidentRows";
            case GPUSceneCullingQualificationMismatch::InstanceCounts:
                return "InstanceCounts";
            case GPUSceneCullingQualificationMismatch::DrawCounts:
                return "DrawCounts";
            case GPUSceneCullingQualificationMismatch::IndirectArguments:
                return "IndirectArguments";
            case GPUSceneCullingQualificationMismatch::RasterPayload:
                return "RasterPayload";
        }
        return "Unknown";
    }

    /**
     * @brief Completion-qualified value evidence for one GPU-scene culling lane.
     *
     * This is intentionally scalar-only: it pinpoints the first divergent
     * active row or draw group without retaining an unbounded GPU readback in
     * ordinary frame diagnostics.
     */
    struct GPUSceneCullingQualificationDiagnostics
    {
        bool requested = false;
        /** True only for a lane selected by the actual frame execution plan. */
        bool required = false;
        bool readbackAllocated = false;
        bool copyRecorded = false;
        bool submissionAccepted = false;
        bool completionObserved = false;
        bool compared = false;
        bool matched = false;
        /** Immutable packet-plan to owner/candidate coverage result. */
        bool inputCoverageCompared = false;
        bool inputCoverageMatched = false;
        /** Canonical Direct-visible identities must be present in GPU visibility. */
        bool directVisibilityCoverageCompared = false;
        bool directVisibilityCoverageMatched = false;
        /** Post-fence visibility/compaction/finalizer output result. */
        bool cullOutputsCompared = false;
        bool cullOutputsMatched = false;
        /** All five final indirect command fields are compared post-fence. */
        bool indirectArgumentsCompared = false;
        bool indirectArgumentsMatched = false;
        /** Tier1 fresh-active-to-resident integrity check; not cross-path equivalence. */
        bool rasterPayloadCompared = false;
        bool rasterPayloadMatched = false;
        GPUSceneCullingQualificationMismatch mismatch =
            GPUSceneCullingQualificationMismatch::NotRequested;
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
        /** Actual Tier1 raster sequence reconstructed post-fence. */
        RasterTranscriptDigest tierOneRasterTranscript{};
        /** Fresh-active reference in the same final indirect order. */
        RasterTranscriptDigest tierOneRasterTranscriptReference{};
        bool tierOneRasterTranscriptCompared = false;
        bool tierOneRasterTranscriptMatched = false;
        uint32 firstRasterTranscriptMismatchEntry = RVX_INVALID_INDEX;
        uint64 expectedRasterTranscriptIdentityHash = 0;
        uint64 observedRasterTranscriptIdentityHash = 0;
        uint64 expectedRasterTranscriptPayloadHash = 0;
        uint64 observedRasterTranscriptPayloadHash = 0;
        /** CPU bytes retained only for this explicitly armed capture. */
        uint64 cpuPayloadBytes = 0;
        GPUDrivenTier capturedTier = GPUDrivenTier::Direct;
        uint64 frameSequence = 0;
        uint64 recordEpoch = 0;
        uint64 gpuSceneLeaseVersion = 0;
        uint64 candidateVersion = 0;
        uint64 activeRowVersion = 0;
        uint64 completionValue = 0;
    };

    struct RenderGPUDrivenCullingDiagnostics
    {
        bool policyDecisionAvailable = false;
        GPUDrivenPolicyDecision policyDecision;
        bool enabled = false;
        bool graphPassAdded = false;
        bool graphPassRecorded = false;
        uint32 gpuCullingGraphPassCount = 0;
        bool gpuExecutionRecorded = false;
        uint32 graphInputDrawItemCount = 0;
        uint32 visibilityCandidateCount = 0;
        uint32 cpuVisibleCandidateCount = 0;
        uint32 passVisibilityCandidateCount = 0;
        uint32 gpuPlannedVisibilityCandidateCount = 0;
        uint32 invalidVisibilityBoundsCount = 0;
        uint32 gpuDeferredVisibilityCandidateCount = 0;
        bool gpuVisibilityReadbackPerformed = false;
        bool occlusionRequestedButUnavailable = false;
        /** @brief Actual per-frame CPU copy bytes for culling instance inputs. */
        uint64 instanceUploadBytes = 0;
        /** @brief Actual per-frame CPU copy bytes for GPU-scene candidates. */
        uint64 gpuSceneCandidateUploadBytes = 0;
        /** @brief Actual per-frame CPU copy bytes for active-row indirection. */
        uint64 activeRowUploadBytes = 0;
        uint32 activeRowCount = 0;
        uint32 activeRowHighWatermark = 0;
        uint32 instancePatchedRowCount = 0;
        uint32 gpuSceneCandidatePatchedRowCount = 0;
        uint32 activeRowPatchedRowCount = 0;
        uint64 instanceFullMaterializationCount = 0;
        uint64 gpuSceneCandidateFullMaterializationCount = 0;
        uint64 activeRowFullMaterializationCount = 0;
        uint64 continuityFullMaterializationCount = 0;
        uint64 capacityFullMaterializationCount = 0;
        /** @brief Full Direct-lane instance-stream CPU copies this frame. */
        uint64 directRasterInstanceUploadBytes = 0;
        /** @brief Direct-lane identity-index stream CPU copies this frame. */
        uint64 directRasterInstanceIndexUploadBytes = 0;
        RenderUploadWorkDiagnostics canonicalInstanceUploadWork{};
        RenderUploadWorkDiagnostics canonicalCandidateUploadWork{};
        RenderUploadWorkDiagnostics canonicalActiveRowUploadWork{};
        RenderUploadWorkDiagnostics directRasterInstanceUploadWork{};
        RenderUploadWorkDiagnostics directRasterIndexUploadWork{};
        /** @brief Successfully committed Direct-lane instance rows this frame. */
        uint32 directRasterInstancePatchedRowCount = 0;
        /** @brief Successfully committed Direct-lane draw-order rows this frame. */
        uint32 directRasterIndexPatchedRowCount = 0;
        /** @brief Sum of active Direct stream rows across raster pass owners. */
        uint32 directRasterActiveInstanceCount = 0;
        /** @brief Sum of Direct stream capacities across raster pass owners. */
        uint32 directRasterActiveInstanceCapacity = 0;
        /** @brief Direct streams that fully materialized instance rows this frame. */
        uint32 directRasterInstanceFullMaterializationCount = 0;
        /** @brief Direct streams that fully materialized draw-order rows this frame. */
        uint32 directRasterIndexFullMaterializationCount = 0;
        bool gpuVisibilityCountsAvailable = false;
        uint32 visibleCullableDrawItemCount = 0;
        uint32 frustumCulledDrawItemCount = 0;
        uint32 distanceCulledDrawItemCount = 0;
        uint32 cpuReferenceVisibleCullableDrawItemCount = 0;
        uint32 cpuReferenceCulledDrawItemCount = 0;
        uint32 skippedMissingGpuDataCount = 0;
        bool opaqueIndirectRequested = false;
        bool opaqueCullingReady = false;
        bool opaquePipelineReady = false;
        bool opaqueIndirectEligible = false;
        bool opaqueIndirectSubmitted = false;
        uint32 opaqueDirectDrawCount = 0;
        uint32 opaqueGpuDrivenIndirectBatchCount = 0;
        uint32 opaqueGpuDrivenIndirectSubmittedDrawUpperBound = 0;
        bool opaqueGpuDrivenExecutedDrawCountAvailable = false;
        uint32 opaqueGpuDrivenIndirectDrawCount = 0;
        GPUDrivenDrawFallbackReason opaqueFallbackReason =
            GPUDrivenDrawFallbackReason::Disabled;
        /** Direct Opaque's actual instance-index dereference order. */
        RasterTranscriptDigest directOpaqueRasterTranscript{};
        /** Explicit Direct Opaque slot-6/index + instance post-fence evidence. */
        DirectRasterReadbackQualificationDiagnostics
            directOpaqueRasterReadbackQualification{};
        /** Explicitly armed, post-fence GPU-scene culling readback evidence. */
        GPUSceneCullingQualificationDiagnostics gpuSceneDepthQualification{};
        GPUSceneCullingQualificationDiagnostics gpuSceneOpaqueQualification{};
    };

    struct RenderParticleFeatureDiagnostics
    {
        bool requested = false;
        bool supported = false;
        bool enabled = false;
        bool graphPassScheduled = false;
        bool drawSubmitted = false;
        uint32 itemCount = 0;
        uint32 renderPayloadReadyItemCount = 0;
        uint32 totalAliveParticles = 0;
        std::string reason{};
    };

    struct RenderMaterialFeatureDiagnostics
    {
        bool ready = false;
        bool usedFallback = false;
        bool constantsUpdated = false;
        bool descriptorSetAvailable = false;
        uint32 textureFlags = 0;
        uint32 requiredTextureFlags = 0;
        std::string materialName{};
        std::string message{};
        /** Exact fallback-or-ready bindings frozen only after a successful Present. */
        bool presentedBindingsAvailable = false;
        bool presentedBindingsOverflow = false;
        std::vector<RenderPresentedMaterialBindingReceipt>
            presentedBindings{};
    };

    /** Completion-qualified palette evidence, intentionally separate from material state. */
    struct RenderSkinningPaletteDiagnostics
    {
        bool presentedReceiptsAvailable = false;
        bool presentedReceiptsOverflow = false;
        std::vector<RenderPresentedSkinningPaletteReceipt> presentedReceipts{};
    };

    /** @brief Honest execution counts for the backend-neutral Direct instancing path. */
    struct RenderInstancingDiagnostics
    {
        RenderInstancingMode requestedMode = RenderInstancingMode::Disabled;
        bool opaquePlanAvailable = false;
        bool opaquePreflightSucceeded = false;
        uint32 opaquePlannedPacketCount = 0;
        uint32 opaquePlannedDrawCount = 0;
        uint32 opaquePlannedInstanceCount = 0;
        uint32 opaquePlannedBatchCount = 0;
        uint32 opaqueExecutedPacketCount = 0;
        uint32 opaqueSubmittedDrawCount = 0;
        uint32 opaqueSubmittedInstanceCount = 0;
        uint32 opaqueInstancedBatchCount = 0;
        uint32 opaqueFallbackBatchCount = 0;
    };

    /** @brief Stable value projection of Render-owned ray tracing state. */
    struct RenderRayTracingDiagnostics
    {
        bool scenePrepared = false;
        bool tlasAvailable = false;
        bool shadowRequested = false;
        bool shadowSupported = false;
        bool shadowRecorded = false;
        bool reflectionRequested = false;
        bool reflectionSupported = false;
        bool reflectionRecorded = false;
        bool reflectionDenoiseRequested = false;
        bool reflectionDenoiseSupported = false;
        bool reflectionDenoiseRecorded = false;
        bool reflectionCompositeRequested = false;
        bool reflectionCompositeSupported = false;
        bool reflectionCompositeRecorded = false;
        bool reflectionMaterialTextureTableAvailable = false;
        bool reflectionGeometryMetadataAvailable = false;
        bool reflectionGeometryTableAvailable = false;
        bool shadowHistoryAvailable = false;
        bool shadowDepthHistoryAvailable = false;
        bool shadowNormalHistoryAvailable = false;
        bool shadowHistoryReset = false;
        bool shadowHistoryRecreated = false;
        bool shadowHistoryResolutionChanged = false;
        bool shadowHistoryConfigChanged = false;
        bool shadowTemporalAccumulated = false;
        bool shadowMaterialTextureTableAvailable = false;
        bool shadowAlphaMetadataAvailable = false;
        bool shadowAlphaTextureTableAvailable = false;
        bool shadowAlphaGeometryTableAvailable = false;
        bool reflectionHistoryAvailable = false;
        bool reflectionDepthHistoryAvailable = false;
        bool reflectionNormalHistoryAvailable = false;
        bool reflectionHistoryReset = false;
        bool reflectionHistoryRecreated = false;
        bool reflectionHistoryResolutionChanged = false;
        bool reflectionHistoryConfigChanged = false;
        bool reflectionTemporalAccumulated = false;
        bool denoiseFallbackToRaw = false;
        bool shadowGpuTimingSupported = false;
        bool shadowGpuTimingQueriesRecorded = false;
        bool shadowGpuTimingResolveRecorded = false;
        bool shadowGpuTimingReadbackBufferAvailable = false;
        bool shadowGpuTimingResultAvailable = false;
        bool reflectionGpuTimingSupported = false;
        bool reflectionGpuTimingQueriesRecorded = false;
        bool reflectionGpuTimingResolveRecorded = false;
        bool reflectionGpuTimingReadbackBufferAvailable = false;
        bool reflectionGpuTimingResultAvailable = false;
        bool budgetEnabled = false;
        bool budgetApplied = false;
        bool rayBudgetExceeded = false;
        bool denoiseTapBudgetExceeded = false;
        bool resourceBudgetExceeded = false;
        bool resourceBudgetEvictionAttempted = false;
        bool resourceByteAccountingOverflowed = false;
        bool gpuTimeBudgetExceeded = false;
        bool gpuTimeBudgetApplied = false;
        bool shadowGpuTimeBudgetExceeded = false;
        bool shadowGpuTimeBudgetApplied = false;
        bool reflectionGpuTimeBudgetExceeded = false;
        bool reflectionGpuTimeBudgetApplied = false;
        bool measuredGpuTimeAvailable = false;
        uint64 rayBudget = 0;
        uint64 denoiseTapBudget = 0;
        uint64 trackedResourceBudget = 0;
        uint64 estimatedShadowRayCount = 0;
        uint64 estimatedReflectionRayCount = 0;
        uint64 estimatedTotalRayCount = 0;
        uint64 estimatedReflectionDenoiseTapCount = 0;
        uint64 shadowGpuTimestampFrequency = 0;
        uint64 reflectionGpuTimestampFrequency = 0;
        float32 shadowGpuTimingElapsedMs = 0.0f;
        float32 reflectionGpuTimingElapsedMs = 0.0f;
        float32 gpuTimeBudget = 0.0f;
        float32 shadowGpuTimeBudget = 0.0f;
        float32 reflectionGpuTimeBudget = 0.0f;
        float32 measuredGpuTimeForBudgetMs = 0.0f;
        float32 measuredShadowGpuTimeForBudgetMs = 0.0f;
        float32 measuredReflectionGpuTimeForBudgetMs = 0.0f;
        float32 gpuTimeBudgetQualityScale = 1.0f;
        float32 shadowGpuTimeBudgetQualityScale = 1.0f;
        float32 reflectionGpuTimeBudgetQualityScale = 1.0f;
        uint32 gpuTimeBudgetOverBudgetFrameCount = 0;
        uint32 reflectionGpuTimeBudgetOverBudgetFrameCount = 0;
        uint32 reflectionGpuTimeBudgetUnderBudgetFrameCount = 0;
        uint64 blasCacheEvictionFrameThreshold = 0;
        uint32 cachedBLASCount = 0;
        uint32 evictedBLASCount = 0;
        uint32 resourceBudgetEvictedBLASCount = 0;
        uint32 releasedBLASScratchCount = 0;
        uint32 pendingBLASScratchReleaseCount = 0;
        uint64 cachedBLASAccelerationStructureBytes = 0;
        uint64 cachedBLASScratchBytes = 0;
        uint64 releasedBLASScratchBytes = 0;
        uint64 topLevelAccelerationStructureBytes = 0;
        uint64 topLevelScratchBytes = 0;
        uint64 instanceBufferBytes = 0;
        uint64 materialMetadataBufferBytes = 0;
        uint64 alphaMetadataBufferBytes = 0;
        uint64 totalTrackedResourceBytes = 0;
        uint32 shadowMaterialTextureCount = 0;
        uint32 shadowMaterialTexturesBound = 0;
        uint32 shadowAlphaTextureCount = 0;
        uint32 shadowAlphaTexturesBound = 0;
        uint32 shadowAlphaIndexBufferCount = 0;
        uint32 shadowAlphaUVBufferCount = 0;
        uint32 reflectionMaterialTextureCount = 0;
        uint32 reflectionMaterialTexturesBound = 0;
        uint32 reflectionGeometryIndexBufferCount = 0;
        uint32 reflectionGeometryUVBufferCount = 0;
        uint32 reflectionGeometryNormalBufferCount = 0;
        uint32 reflectionGeometryTangentBufferCount = 0;
        float32 requestedReflectionResolutionScale = 0.0f;
        float32 reflectionResolutionScale = 0.0f;
        uint32 requestedShadowSamplesPerPixel = 0;
        uint32 requestedReflectionSamplesPerPixel = 0;
        uint32 requestedReflectionDenoiseRadius = 0;
        uint32 shadowSamplesPerPixel = 0;
        uint32 reflectionSamplesPerPixel = 0;
        uint32 reflectionDenoiseRadius = 0;
        uint32 shadowWidth = 0;
        uint32 shadowHeight = 0;
        uint32 reflectionWidth = 0;
        uint32 reflectionHeight = 0;
    };

    /**
     * @brief Value-only evidence for retained-scene and draw-packet reuse work.
     *
     * Counts are cumulative except for the explicitly named `last*` fields.
     * A static accepted scene therefore advances `staticReuseCount` without
     * advancing rebuild, removal, packet-build, or invalidation work.
     */
    struct RenderSceneWorkDiagnostics
    {
        bool available = false;
        uint64 fullRebuildCount = 0;
        uint64 incrementalUpdateCount = 0;
        uint64 staticReuseCount = 0;
        uint64 appliedSceneRevision = 0;
        uint32 lastRebuiltObjectCount = 0;
        uint32 lastRemovedObjectCount = 0;

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
    };

    /** @brief Stable value summary of Render-owned frame feature execution. */
    struct RenderFrameFeatureDiagnostics
    {
        bool available = false;
        uint64 frameSequence = 0;
        bool renderAttempted = false;
        bool rendered = false;
        bool graphBuilt = false;
        bool graphCompiled = false;
        uint32 renderGraphTotalPasses = 0;
        uint32 visibleObjectCount = 0;
        uint32 renderSceneLightCount = 0;
        uint32 requestedPostProcessEffectCount = 0;
        uint32 enabledPostProcessEffectCount = 0;
        uint32 unsupportedPostProcessSkippedCount = 0;
        uint32 postProcessGraphPassCount = 0;
        bool clusteredLightingInitialized = false;
        uint32 clusteredLightingActiveClusters = 0;
        bool textureIBLEnabled = false;
        /** Explicitly distinguishes no texture-IBL request from a failed binding. */
        RenderEnvironmentIBLDiagnostics environmentIBL{};
        RenderPassFeatureDiagnostics skybox{};
        RenderDirectionalShadowDiagnostics directionalShadow{};
        RenderLocalLightingDiagnostics localLighting{};
        RenderPassFeatureDiagnostics hzb{};
        RenderTransparentPassDiagnostics transparent{};
        RenderGPUDrivenCullingDiagnostics gpuDrivenCulling{};
        /** @brief Extraction work for the last successfully consumed frame. */
        RenderExtractionDiagnostics extraction{};
        RenderSceneWorkDiagnostics sceneWork{};
        GPUSceneDiagnostics gpuScene{};
        RenderMutationEvidenceDiagnostics mutationEvidence{};
        RenderPolicyDiagnostics policy{};
        RenderParticleFeatureDiagnostics particles{};
        RenderMaterialFeatureDiagnostics material{};
        RenderSkinningPaletteDiagnostics skinning{};
        RenderInstancingDiagnostics instancing{};
        RenderRayTracingDiagnostics rayTracing{};
        uint64 gpuMemoryBudget = 0;
        uint64 gpuUsedMemory = 0;
        uint32 residentMeshCount = 0;
        uint32 residentTextureCount = 0;
        uint32 pendingUploadCount = 0;
        uint32 queuedUploadCount = 0;
        uint32 failedUploadCount = 0;
        std::vector<std::string> fallbackReasons{};
        std::vector<std::string> unsupportedFeatures{};
    };

    struct RenderDiagnosticsSnapshot
    {
        uint64 publicationSequence = 0;
        RenderExecutorKind executor = RenderExecutorKind::None;
        RenderLifecycleState lifecycle = RenderLifecycleState::Stopped;
        uint64 renderThreadIdentityHash = 0;
        uint64 transitionCount = 0;
        RenderLifecycleTransitionDiagnostics lastTransition{};
        std::array<RenderLifecycleTransitionDiagnostics,
                   RVX_RENDER_DIAGNOSTICS_TRANSITION_CAPACITY>
            transitions{};

        RHIBackendType backend = RHIBackendType::None;
        std::string adapterName{};
        std::string driverVersion{};
        uint64 surfaceGeneration = 0;
        uint32 surfaceWidth = 0;
        uint32 surfaceHeight = 0;
        uint64 resizeAcceptedCount = 0;
        uint64 resizeCoalescedCount = 0;
        uint64 resizeRejectedCount = 0;

        uint64 lastPublishedFrameSequence = 0;
        uint64 lastAcquiredFrameSequence = 0;
        uint64 lastAppliedFrameSequence = 0;
        uint64 lastSubmittedFrameSequence = 0;
        uint64 lastPresentedFrameSequence = 0;
        RenderFrameTransportDiagnostics frameTransport{};
        RenderUploadTransportDiagnostics uploadTransport{};
        RenderReleaseTransportDiagnostics releaseTransport{};
        RenderResourceDiagnostics resources{};
        RenderSceneValueDiagnostics sceneValues{};
        RenderCpuFrameTimingDiagnostics cpuFrameTiming{};
        /** Completion-owned and intentionally independent of frameFeatures. */
        RenderGpuFrameTimingDiagnostics gpuFrameTiming{};
        std::array<RenderQueueTimelineDiagnostics, 3> queues{};
        RenderRetirementDiagnostics retirement{};
        RenderGraphLifetimeDiagnostics renderGraphLifetime{};
        RenderNativeValidationDiagnostics nativeValidation{};
        RenderMutationEvidenceDiagnostics mutationEvidence{};
        RenderFrameFeatureDiagnostics frameFeatures{};
        RenderFrameCaptureResult lastCapture{};
        RenderFailureDiagnostics lastFailure{};

        uint64 pumpIterationCount = 0;
        uint64 idleWaitCount = 0;
    };
} // namespace RVX
