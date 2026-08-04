#pragma once

/**
 * @file RenderDiagnostics.h
 * @brief Immutable value-only render runtime diagnostics contract.
 */

#include "Render/RenderRuntimeTypes.h"
#include "Render/GPUDriven/GPUDrivenDiagnostics.h"
#include "Render/GPUDriven/GPUDrivenPolicy.h"
#include "Render/GPUScene/GPUSceneDiagnostics.h"
#include "Render/Policy/RenderPolicyDiagnostics.h"
#include "RenderContracts/RenderFramePacket.h"

#include <array>
#include <string>
#include <vector>

namespace RVX
{
    inline constexpr uint8 RVX_RENDER_DIAGNOSTICS_UNAVAILABLE_INDEX = 0xFFU;
    inline constexpr uint32 RVX_RENDER_DIAGNOSTICS_TRANSITION_CAPACITY = 8U;

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
        std::string message{};

        [[nodiscard]] bool IsComplete() const noexcept
        {
            return code == RenderFrameCaptureResultCode::Completed &&
                   requestId != 0 && frameSequence != 0 && width != 0 &&
                   height != 0 && rowPitch != 0 && bytesPerPixel != 0 &&
                   !bytes.empty();
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
        bool samplingEnabled = false;
        std::string reason{};
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
        RenderPassFeatureDiagnostics skybox{};
        RenderDirectionalShadowDiagnostics directionalShadow{};
        RenderGPUDrivenCullingDiagnostics gpuDrivenCulling{};
        GPUSceneDiagnostics gpuScene{};
        RenderPolicyDiagnostics policy{};
        RenderParticleFeatureDiagnostics particles{};
        RenderMaterialFeatureDiagnostics material{};
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
        std::array<RenderQueueTimelineDiagnostics, 3> queues{};
        RenderRetirementDiagnostics retirement{};
        RenderFrameFeatureDiagnostics frameFeatures{};
        RenderFrameCaptureResult lastCapture{};
        RenderFailureDiagnostics lastFailure{};

        uint64 pumpIterationCount = 0;
        uint64 idleWaitCount = 0;
    };
} // namespace RVX
