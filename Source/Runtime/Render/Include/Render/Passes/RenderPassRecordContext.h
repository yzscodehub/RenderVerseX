#pragma once

/**
 * @file RenderPassRecordContext.h
 * @brief Immutable frame/view inputs captured while registering scene passes.
 */

#include "Render/Graph/RenderGraph.h"
#include "Render/Graph/RenderGraphExecutionReceipt.h"
#include "Render/Animation/RenderDeformationFrameResources.h"
#include "Render/GPUDriven/GPUDrivenDiagnostics.h"
#include "Render/Lighting/LightingFramePlan.h"
#include "Render/Lighting/LocalShadowAtlas.h"
#include "Render/Lighting/VolumetricLightingFramePlan.h"
#include "Render/Passes/MeshPassProcessor.h"
#include "Render/PipelineCache.h"
#include "Render/Policy/RenderFrameExecutionPlan.h"
#include "Render/Renderer/RenderDrawItem.h"
#include "Render/Renderer/RenderDrawPlan.h"
#include "Render/Renderer/RenderScene.h"
#include "Render/Renderer/ViewData.h"
#include "Render/RenderUploadWorkDiagnostics.h"
#include "Render/RenderDiagnostics.h"
#include "Render/Submission/RenderInstanceBatchPlan.h"
#include "Render/Submission/OpaqueRasterSubmission.h"
#include "Render/Visibility/RenderVisibility.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <tuple>
#include <vector>

namespace RVX
{
    class GPUCulling;
    class GPUCullingRecordedState;
    class DirectRasterReadbackQualification;
    class LocalShadowPass;
    class RenderGraph;
    struct RenderPassRecordIdentity;
    enum class RayTracedReflectionCompositeSource : uint8;

    struct DepthPrepassDrawStats
    {
        uint32 directDrawCount = 0;
        uint32 submittedDrawCount = 0;
        uint32 submittedInstanceCount = 0;
        uint32 instancedBatchCount = 0;
        uint32 instancingFallbackBatchCount = 0;
        uint64 directInstanceUploadBytes = 0;
        uint64 directInstanceIndexUploadBytes = 0;
        uint32 directInstancePatchedRowCount = 0;
        uint32 directInstanceIndexPatchedRowCount = 0;
        uint32 directInstanceActiveCount = 0;
        uint32 directInstanceActiveCapacity = 0;
        bool directInstanceFullMaterialization = false;
        bool directInstanceIndexFullMaterialization = false;
        RenderUploadWorkDiagnostics directInstanceUploadWork{};
        RenderUploadWorkDiagnostics directInstanceIndexUploadWork{};
        DirectRasterMutationTotals directMutationTotals{};
        bool directMutationTotalsSaturated = false;
        uint32 gpuDrivenIndirectBatchCount = 0;
        uint32 gpuDrivenIndirectSubmittedDrawUpperBound = 0;
        bool gpuDrivenIndirectExecutedDrawCountAvailable = false;
        uint32 gpuDrivenIndirectDrawCount = 0;
        bool gpuDrivenRequested = false;
        bool gpuDrivenEligible = false;
        bool planRequested = false;
        bool planValidated = false;
        bool directPacketPathUsed = false;
        uint32 plannedPacketCount = 0;
        uint32 compiledPacketCount = 0;
        uint32 executedPacketCount = 0;
        uint32 skippedMissingUVCount = 0;
        uint32 skippedMaterialBindingCount = 0;
        RenderPolicyReason failureReason =
            RenderPolicyReason::ConservativeDefault;
    };

    struct ShadowPassStats
    {
        uint32 configuredCascadeCount = 0;
        uint32 declaredCascadeResourceCount = 0;
        uint32 resolvedCascadeViewCount = 0;
        uint32 shadowCasterCount = 0;
        uint32 drawCount = 0;
        uint32 submittedInstanceCount = 0;
        uint32 instancedBatchCount = 0;
        uint32 instancingFallbackBatchCount = 0;
        uint64 directInstanceUploadBytes = 0;
        uint64 directInstanceIndexUploadBytes = 0;
        uint32 directInstancePatchedRowCount = 0;
        uint32 directInstanceIndexPatchedRowCount = 0;
        uint32 directInstanceActiveCount = 0;
        uint32 directInstanceActiveCapacity = 0;
        bool directInstanceFullMaterialization = false;
        bool directInstanceIndexFullMaterialization = false;
        RenderUploadWorkDiagnostics directInstanceUploadWork{};
        RenderUploadWorkDiagnostics directInstanceIndexUploadWork{};
        DirectRasterMutationTotals directMutationTotals{};
        bool directMutationTotalsSaturated = false;
        /** Exact immutable deformation resource consumed by recorded skin draws. */
        uint64 deformationContentHash = 0;
        uint32 skinnedDrawCount = 0;
        uint32 currentPaletteBoundDrawCount = 0;
    };

    struct OpaquePassShadowStats
    {
        bool requested = false;
        bool renderGraphReadDeclared = false;
        bool frameShadowReady = false;
        bool rayTracedRequested = false;
        bool rayTracedRenderGraphReadDeclared = false;
        bool rayTracedFrameMaskReady = false;
        bool rayTracedExecutionReady = false;
        bool rayTracedExecutionFailed = false;
        uint32 receiverCandidateDrawItemCount = 0;
        uint32 shadowReceivingDrawItemCount = 0;
        uint32 shadowReceiverOptOutDrawItemCount = 0;
    };

    /** @brief Per-recording object motion-vector diagnostics. */
    struct ObjectVelocityPassStats
    {
        bool requested = false;
        bool supported = false;
        bool velocityTargetAvailable = false;
        bool depthAvailable = false;
        bool previousViewProjectionAvailable = false;
        bool drawItemsAvailable = false;
        bool outputDeclared = false;
        bool velocityRecorded = false;
        uint32 width = 0;
        uint32 height = 0;
        uint32 drawItemCount = 0;
        uint32 opaqueDrawItemCount = 0;
        uint32 maskedDrawItemCount = 0;
        uint32 objectsWithHistory = 0;
        uint32 drawCount = 0;
        uint32 maskedDrawCount = 0;
        uint32 skippedNoHistoryCount = 0;
        uint32 skippedMissingResourceCount = 0;
        uint32 skippedMissingUVCount = 0;
        uint32 skippedMaterialBindingCount = 0;
        uint64 deformationContentHash = 0;
        uint32 skinnedDrawCount = 0;
        uint32 currentPaletteBoundDrawCount = 0;
        uint32 previousPaletteBoundDrawCount = 0;
        /** Exact RenderGraph texture slot written by this recording. */
        uint32 velocityTargetResourceIndex = RVX_INVALID_INDEX;
        /** Exact graph provenance paired with the written texture slot. */
        uint64 velocityTargetGraphIdentity = 0;
        uint64 velocityTargetRecordingGeneration = 0;
        RHIFormat outputFormat = RHIFormat::Unknown;

        [[nodiscard]] bool MatchesVelocityTarget(
            const RGTextureHandle& handle,
            const RenderPassRecordIdentity& expected) const noexcept;
    };


    /** @brief Per-recording ray-traced-shadow diagnostics. */
    struct RayTracedShadowPassStats
    {
        bool requested = false;
        bool supported = false;
        bool tlasAvailable = false;
        bool materialMetadataAvailable = false;
        bool materialTextureTableAvailable = false;
        bool alphaMetadataAvailable = false;
        bool alphaTextureTableAvailable = false;
        bool alphaGeometryTableAvailable = false;
        bool depthAvailable = false;
        bool velocityAvailable = false;
        bool historyAvailable = false;
        bool depthHistoryAvailable = false;
        bool normalHistoryAvailable = false;
        bool historyReset = false;
        bool historyRecreated = false;
        bool historyResolutionChanged = false;
        bool historyConfigChanged = false;
        bool temporalAccumulated = false;
        bool outputDeclared = false;
        bool resourceViewsAvailable = false;
        bool descriptorSetAvailable = false;
        bool constantsUploaded = false;
        bool dispatchRecorded = false;
        bool executionFailed = false;
        bool gpuTimingSupported = false;
        bool gpuTimingQueriesRecorded = false;
        bool gpuTimingResolveRecorded = false;
        bool gpuTimingReadbackBufferAvailable = false;
        bool gpuTimingResultAvailable = false;
        uint32 gpuTimingStartQueryIndex = 0;
        uint32 gpuTimingEndQueryIndex = 1;
        uint64 gpuTimestampFrequency = 0;
        uint64 gpuTimingReadbackBytes = 0;
        uint64 gpuTimingStartTimestamp = 0;
        uint64 gpuTimingEndTimestamp = 0;
        uint64 gpuTimingElapsedTicks = 0;
        float32 gpuTimingElapsedMs = 0.0f;
        uint32 gpuTimingReadbackBufferCount = 0;
        uint32 gpuTimingReadbackFrameIndex = RVX_INVALID_INDEX;
        uint32 samplesPerPixel = 1;
        uint64 dispatchPixelCount = 0;
        uint64 estimatedRayCount = 0;
        uint32 materialTextureCount = 0;
        uint32 materialTexturesBound = 0;
        uint32 alphaTextureCount = 0;
        uint32 alphaTexturesBound = 0;
        uint32 alphaIndexBufferCount = 0;
        uint32 alphaUVBufferCount = 0;
        uint32 width = 0;
        uint32 height = 0;
    };

    /** @brief Per-recording ray-traced-reflection diagnostics. */
    struct RayTracedReflectionPassStats
    {
        bool requested = false;
        bool supported = false;
        bool tlasAvailable = false;
        bool materialMetadataAvailable = false;
        bool materialTextureTableAvailable = false;
        bool geometryMetadataAvailable = false;
        bool geometryTableAvailable = false;
        bool sceneColorAvailable = false;
        bool depthAvailable = false;
        bool velocityAvailable = false;
        bool exposureStateAvailable = false;
        bool exposureStateReadDeclared = false;
        bool exposureStateDescriptorBound = false;
        uint64 exposureStateContentHash = 0;
        bool historyAvailable = false;
        bool depthHistoryAvailable = false;
        bool normalHistoryAvailable = false;
        bool historyReset = false;
        bool historyRecreated = false;
        bool historyResolutionChanged = false;
        bool historyConfigChanged = false;
        bool historyReservationRecorded = false;
        bool historySubmitted = false;
        /** Recorded temporal resolve/history-store work, independent of history sampling. */
        bool temporalResolveRecorded = false;
        bool temporalAccumulated = false;
        bool outputDeclared = false;
        bool resourceViewsAvailable = false;
        bool descriptorSetAvailable = false;
        bool constantsUploaded = false;
        bool dispatchRecorded = false;
        bool gpuTimingSupported = false;
        bool gpuTimingQueriesRecorded = false;
        bool gpuTimingResolveRecorded = false;
        bool gpuTimingReadbackBufferAvailable = false;
        bool gpuTimingResultAvailable = false;
        uint32 gpuTimingStartQueryIndex = 0;
        uint32 gpuTimingEndQueryIndex = 1;
        uint64 gpuTimestampFrequency = 0;
        uint64 gpuTimingReadbackBytes = 0;
        uint64 gpuTimingStartTimestamp = 0;
        uint64 gpuTimingEndTimestamp = 0;
        uint64 gpuTimingElapsedTicks = 0;
        float gpuTimingElapsedMs = 0.0f;
        uint32 gpuTimingReadbackBufferCount = 0;
        uint32 gpuTimingReadbackFrameIndex = RVX_INVALID_INDEX;
        float resolutionScale = 1.0f;
        uint32 samplesPerPixel = 1;
        float intensity = 1.0f;
        float maxTraceDistance = 0.0f;
        float maxRoughness = 1.0f;
        float distanceFadeStart = 0.8f;
        float roughnessConeSpread = 1.0f;
        float normalBias = 0.02f;
        float rayMinT = 0.001f;
        float fireflyClamp = 64.0f;
        float temporalBlendFactor = 0.0f;
        float historyDepthThreshold = 0.01f;
        float historyNormalThreshold = 0.85f;
        float historyLuminanceTolerance = 4.0f;
        float historyConfidenceThreshold = 0.05f;
        float historyVelocityRejectionScale = 0.0f;
        uint32 materialTextureCount = 0;
        uint32 materialTexturesBound = 0;
        uint32 geometryIndexBufferCount = 0;
        uint32 geometryUVBufferCount = 0;
        uint32 geometryNormalBufferCount = 0;
        uint32 geometryTangentBufferCount = 0;
        uint64 dispatchPixelCount = 0;
        uint64 estimatedRayCount = 0;
        uint32 width = 0;
        uint32 height = 0;
    };

    /** @brief Per-recording spatial-denoise diagnostics for RT reflections. */
    struct RayTracedReflectionDenoisePassStats
    {
        bool requested = false;
        bool supported = false;
        bool sourcePassEnabled = false;
        bool reflectionHandleAvailable = false;
        bool depthAvailable = false;
        bool normalGuideAvailable = false;
        bool outputDeclared = false;
        bool constantsUploaded = false;
        bool denoiseRecorded = false;
        uint32 radius = 0;
        float depthSigma = 0.0f;
        float normalThreshold = 0.0f;
        float confidencePower = 0.0f;
        float centerWeight = 0.0f;
        float lowConfidenceDepthScale = 0.0f;
        uint32 kernelTapCount = 0;
        uint64 dispatchPixelCount = 0;
        uint64 estimatedTapCount = 0;
        uint32 width = 0;
        uint32 height = 0;
    };

    /** @brief Per-recording composite diagnostics for RT reflections. */
    struct RayTracedReflectionCompositePassStats
    {
        bool requested = false;
        bool supported = false;
        bool sourcePassEnabled = false;
        bool denoiseRequested = false;
        bool denoisedSourceAvailable = false;
        bool denoisedSourceUsed = false;
        bool rawSourceUsed = false;
        bool denoiseFallbackToRaw = false;
        bool reflectionHandleAvailable = false;
        bool outputDeclared = false;
        bool constantsUploaded = false;
        bool compositeRecorded = false;
        RayTracedReflectionCompositeSource source{};
        uint32 width = 0;
        uint32 height = 0;
    };

    struct OpaquePassDrawStats
    {
        RenderInstancingMode instancingMode = RenderInstancingMode::Disabled;
        bool instancingPlanAvailable = false;
        bool instancingPreflightSucceeded = false;
        uint32 plannedInstancingPacketCount = 0;
        uint32 plannedInstancingDrawCount = 0;
        uint32 plannedInstancingInstanceCount = 0;
        uint32 plannedInstancingBatchCount = 0;
        uint32 directDrawCount = 0;
        uint32 submittedDrawCount = 0;
        uint32 submittedInstanceCount = 0;
        uint32 instancedBatchCount = 0;
        uint32 instancingFallbackBatchCount = 0;
        uint64 directInstanceUploadBytes = 0;
        uint64 directInstanceIndexUploadBytes = 0;
        uint32 directInstancePatchedRowCount = 0;
        uint32 directInstanceIndexPatchedRowCount = 0;
        uint32 directInstanceActiveCount = 0;
        uint32 directInstanceActiveCapacity = 0;
        bool directInstanceFullMaterialization = false;
        bool directInstanceIndexFullMaterialization = false;
        RenderUploadWorkDiagnostics directInstanceUploadWork{};
        RenderUploadWorkDiagnostics directInstanceIndexUploadWork{};
        DirectRasterMutationTotals directMutationTotals{};
        bool directMutationTotalsSaturated = false;
        /** Actual Opaque Direct stream order, reconstructed from resident rows. */
        RasterTranscriptDigest directRasterTranscript{};
        bool gpuDrivenRequested = false;
        bool gpuDrivenCullingReady = false;
        bool gpuDrivenPipelineReady = false;
        bool gpuDrivenEligible = false;
        bool gpuDrivenSubmitted = false;
        uint32 gpuDrivenIndirectBatchCount = 0;
        uint32 gpuDrivenIndirectSubmittedDrawUpperBound = 0;
        bool gpuDrivenIndirectExecutedDrawCountAvailable = false;
        uint32 gpuDrivenIndirectDrawCount = 0;
        GPUDrivenDrawFallbackReason gpuDrivenFallbackReason =
            GPUDrivenDrawFallbackReason::Disabled;
        uint32 skippedMaterialBindingCount = 0;
        uint32 materialBindingCount = 0;
        uint32 materialFallbackBindingCount = 0;
        uint32 materialTextureFlags = 0;
        uint32 materialFallbackTextureFlags = 0;
        /** Completed Toon bindings, never requested-material counts. */
        uint32 toonMaterialCount = 0;
        /** Exact successfully recorded Toon draw submissions when observable. */
        uint32 toonDrawCount = 0;
        /** Successful Toon draws whose material binding used a fallback resource. */
        uint32 toonFallbackDrawCount = 0;
        /** Stable value identity of the actually bound Toon program family. */
        uint64 toonProgramFamilyHash = 0;
        /** Exact committed material table transcript used by successful Toon work. */
        uint64 materialTableContentHash = 0;
        /** ABI schema paired with materialTableContentHash. */
        uint32 materialTableSchemaVersion = 0;
        uint64 deformationContentHash = 0;
        uint32 skinnedDrawCount = 0;
        uint32 currentPaletteBoundDrawCount = 0;
        bool planRequested = false;
        bool planValidated = false;
        bool directPacketPathUsed = false;
        uint32 plannedPacketCount = 0;
        uint32 compiledPacketCount = 0;
        uint32 executedPacketCount = 0;
        RenderPolicyReason failureReason =
            RenderPolicyReason::ConservativeDefault;
    };

    /**
     * @brief Per-route evidence for a split deferred opaque recording.
     *
     * The legacy aggregate remains available for existing diagnostics, but a
     * deferred frame preserves these two independently recorded subsets so a
     * caller cannot mistake GBuffer work for post-lighting forward work.
     */
    struct OpaqueSurfaceRouteExecutionEvidence
    {
        OpaqueRasterSurfaceRoute route = OpaqueRasterSurfaceRoute::AllOpaque;
        OpaquePassDrawStats drawStats{};
        OpaquePassShadowStats shadowStats{};
        RenderExecutionStatus status = RenderExecutionStatus::NotAttempted;
        bool available = false;
    };

    /** @brief Graph-owned transparent-pass recording and execution facts. */
    struct TransparentPassDrawStats
    {
        bool requested = false;
        /** A typed empty transparent list is a successful no-work pass. */
        bool noWork = false;
        bool preflightFailed = false;
        bool executionFailed = false;
        uint32 candidateDrawItemCount = 0;
        uint32 preparedDrawItemCount = 0;
        /** Exact DrawIndexed calls issued by this graph execution. */
        uint32 executedPacketCount = 0;
        uint32 executedDrawCount = 0;
        uint32 alphaBlendDrawCount = 0;
        uint32 premultipliedDrawCount = 0;
        uint32 additiveDrawCount = 0;
        uint32 thinTransmissionDrawCount = 0;
        bool opaqueSceneSnapshotReady = false;
        bool opaqueSceneSnapshotCopied = false;
        bool reactiveMaskAttachmentBound = false;
        uint32 reactiveMaskWriteDrawCount = 0;
        uint32 translucencyPriorityBucketCount = 0;
        uint32 translucencyFallbackCount = 0;
        uint64 transparentOrderHash = 0;
        bool transparentOrderValid = false;
        uint32 skippedMaterialBindingCount = 0;
        uint32 skippedResourceCount = 0;
        uint32 skippedExecutionDrawCount = 0;
        uint32 materialBindingCount = 0;
        uint32 materialFallbackBindingCount = 0;
        uint32 materialTextureFlags = 0;
        uint32 materialFallbackTextureFlags = 0;
        RenderPolicyReason failureReason =
            RenderPolicyReason::ConservativeDefault;
    };

    /** @brief Identity attached to every graph-owned GPU input slice. */
    struct RenderPassRecordIdentity
    {
        RenderGraph* graph = nullptr;
        uint64 graphIdentity = 0;
        uint64 graphRecordingGeneration = 0;
        uint64 frameSequence = 0;
        uint32 viewOrdinal = 0;
        uint64 recordEpoch = 0;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return graph != nullptr && graphIdentity != 0 &&
                   graphRecordingGeneration != 0 && frameSequence != 0 &&
                   recordEpoch != 0;
        }

        bool operator==(const RenderPassRecordIdentity&) const = default;

        [[nodiscard]] bool Matches(const RenderGraph& expectedGraph) const noexcept
        {
            return IsValid() && graph == &expectedGraph &&
                   graphIdentity == expectedGraph.GetGraphIdentity() &&
                   graphRecordingGeneration ==
                       expectedGraph.GetRecordingGeneration();
        }
    };

    [[nodiscard]] inline bool HasCurrentGraphProvenance(
        const RGTextureHandle& handle,
        const RenderPassRecordIdentity& identity) noexcept
    {
        return !handle.IsValid() ||
               (handle.graphIdentity == identity.graphIdentity &&
                handle.recordingGeneration ==
                    identity.graphRecordingGeneration);
    }

    /** @brief Compare the complete identity of two graph texture handles. */
    [[nodiscard]] inline bool AreSameGraphTextureHandle(
        const RGTextureHandle& left,
        const RGTextureHandle& right) noexcept
    {
        if (left.index != right.index ||
            left.graphIdentity != right.graphIdentity ||
            left.recordingGeneration != right.recordingGeneration ||
            left.hasSubresourceRange != right.hasSubresourceRange)
        {
            return false;
        }
        if (!left.hasSubresourceRange)
        {
            return true;
        }
        return left.subresourceRange.baseMipLevel ==
                   right.subresourceRange.baseMipLevel &&
               left.subresourceRange.mipLevelCount ==
                   right.subresourceRange.mipLevelCount &&
               left.subresourceRange.baseArrayLayer ==
                   right.subresourceRange.baseArrayLayer &&
               left.subresourceRange.arrayLayerCount ==
                   right.subresourceRange.arrayLayerCount &&
               left.subresourceRange.aspect == right.subresourceRange.aspect;
    }

    /**
     * @brief Explicit final HDR attachment selected after the frame snapshot.
     *
     * A frame snapshot is deliberately frozen before graph features begin to
     * publish late outputs.  A later feature must therefore never mutate that
     * snapshot just to redirect a surface pass.  This narrow value contract
     * carries the one final HDR attachment across that boundary and makes the
     * replacement independently provenance-checkable.
     */
    struct RenderPassLateBoundHdrAttachment
    {
        RenderPassRecordIdentity identity{};
        RGTextureHandle finalHdrSceneColor{};

        [[nodiscard]] bool IsEmpty() const noexcept
        {
            return !identity.IsValid() && !finalHdrSceneColor.IsValid();
        }

        [[nodiscard]] bool IsCompatibleWith(
            const RenderPassRecordIdentity& expected) const noexcept
        {
            return identity == expected && finalHdrSceneColor.IsValid() &&
                   HasCurrentGraphProvenance(finalHdrSceneColor, expected);
        }
    };

    [[nodiscard]] inline bool HasCurrentGraphProvenance(
        const RGBufferHandle& handle,
        const RenderPassRecordIdentity& identity) noexcept
    {
        return !handle.IsValid() ||
               (handle.graphIdentity == identity.graphIdentity &&
                    handle.recordingGeneration ==
                    identity.graphRecordingGeneration);
    }

    /**
     * @brief Frozen TLAS storage identity shared by build and trace passes.
     *
     * RenderGraph tracks the real acceleration-structure backing buffer so a
     * build write creates an explicit dependency for every DispatchRays read.
     */
    struct RayTracingSceneRecordInput
    {
        RenderPassRecordIdentity identity{};
        RHIAccelerationStructureRef topLevelAS{};
        RHIResourceInstanceId storageResourceId{};
        RGBufferHandle topLevelASStorage{};
        uint64 preparationGeneration = 0;
        uint64 structuralHash = 0;
        bool required = false;

        [[nodiscard]] bool IsEmpty() const noexcept
        {
            return !identity.IsValid() && !topLevelAS &&
                   !storageResourceId.IsValid() &&
                   !topLevelASStorage.IsValid() &&
                   preparationGeneration == 0 && structuralHash == 0 &&
                   !required;
        }

        [[nodiscard]] bool IsCompatibleWith(
            const RenderPassRecordIdentity& expected) const noexcept
        {
            if (identity != expected || !topLevelAS ||
                topLevelAS->GetType() !=
                    RHIAccelerationStructureType::TopLevel ||
                !storageResourceId.IsValid() ||
                !topLevelASStorage.IsValid() ||
                preparationGeneration == 0 || structuralHash == 0 ||
                !HasCurrentGraphProvenance(topLevelASStorage, expected))
            {
                return false;
            }

            RHIBuffer* const storage = topLevelAS->GetStorageBuffer();
            return storage != nullptr &&
                   storage->GetResourceInstanceId() == storageResourceId &&
                   HasFlag(storage->GetUsage(),
                           RHIBufferUsage::AccelerationStructureStorage);
        }
    };

    /**
     * @brief Identity-bound HDR scene input consumed by TransparentPass.
     *
     * An empty value is valid when no transparent consumer is recorded. Any
     * populated value must exactly match its graph recording and carry both a
     * graph-owned texture and the receipt produced by its copy pass.
     */
    struct OpaqueSceneSnapshotRecordInput
    {
        RenderPassRecordIdentity identity{};
        RGTextureHandle texture{};
        std::shared_ptr<OpaqueSceneSnapshotCopyReceipt> receipt;

        [[nodiscard]] bool IsEmpty() const noexcept
        {
            return !identity.IsValid() && !texture.IsValid() && !receipt;
        }

        [[nodiscard]] bool IsCompatibleWith(
            const RenderPassRecordIdentity& expected) const noexcept
        {
            return identity == expected && texture.IsValid() &&
                   HasCurrentGraphProvenance(texture, expected) &&
                   receipt != nullptr;
        }
    };

    /**
     * @brief Immutable graph-owned D32 surface shared by special surfaces.
     *
     * This value is produced after opaque Terrain/Sky work and before Water.
     * It is late-bound graph state rather than ViewData, so Water and Particle
     * must borrow this exact texture and execution receipt instead of creating
     * their own copies.
     */
    struct SceneDepthSnapshotRecordInput
    {
        RenderPassRecordIdentity identity{};
        RGTextureHandle texture{};
        RenderOutputExtent extent{};
        RHIFormat format = RHIFormat::Unknown;
        RHISampleCount sampleCount = RHISampleCount::Count1;
        std::shared_ptr<SceneDepthSnapshotCopyReceipt> receipt{};

        [[nodiscard]] bool IsEmpty() const noexcept
        {
            return !identity.IsValid() && !texture.IsValid() &&
                   !extent.IsValid() && format == RHIFormat::Unknown &&
                   sampleCount == RHISampleCount::Count1 && !receipt;
        }

        [[nodiscard]] bool IsCompatibleWith(
            const RenderPassRecordIdentity& expected,
            RenderOutputExtent expectedExtent) const noexcept
        {
            return identity == expected && texture.IsValid() &&
                   HasCurrentGraphProvenance(texture, expected) &&
                   extent == expectedExtent && format == RHIFormat::D32_FLOAT &&
                   sampleCount == RHISampleCount::Count1 && receipt != nullptr;
        }
    };

    /**
     * Graph-owned inputs consumed exclusively by WaterFeaturePass.
     *
     * Water samples immutable copies instead of the active HDR/depth
     * attachments. This keeps refraction/thickness legal on explicit APIs and
     * makes accidental sampled/render-target aliasing structurally impossible.
     * The exact committed exposure-state handle is frozen alongside the HDR
     * snapshot: the snapshot is already pre-exposed, while authored water
     * radiance must enter that same domain exactly once.
     */
    struct WaterSurfaceRecordInput
    {
        RenderPassRecordIdentity identity{};
        OpaqueSceneSnapshotRecordInput opaqueSceneSnapshot{};
        SceneDepthSnapshotRecordInput depthSnapshot{};
        /** Exact committed producer state that authored opaqueSceneSnapshot. */
        RGBufferHandle exposureState{};
        /** Semantic identity of the frozen committed exposure state. */
        uint64 exposureStateContentHash = 0;

        [[nodiscard]] bool IsEmpty() const noexcept
        {
            return !identity.IsValid() && opaqueSceneSnapshot.IsEmpty() &&
                   depthSnapshot.IsEmpty() && !exposureState.IsValid() &&
                   exposureStateContentHash == 0;
        }

        [[nodiscard]] bool IsCompatibleWith(
            const RenderPassRecordIdentity& expected,
            RenderOutputExtent expectedExtent) const noexcept
        {
            return identity == expected &&
                   opaqueSceneSnapshot.IsCompatibleWith(expected) &&
                   depthSnapshot.IsCompatibleWith(expected, expectedExtent) &&
                   exposureState.IsValid() &&
                   HasCurrentGraphProvenance(exposureState, expected) &&
                   exposureStateContentHash != 0;
        }
    };

    /**
     * Graph-owned inputs reserved for the particle VFX chain.
     *
     * The color destination is deliberately distinct from the particle color
     * attachment: DistortionComposite samples the latter and writes the
     * former, so graph recording can reject accidental HDR read/write aliasing.
     * The committed exposure state is frozen with the authored particle
     * packets: fire/smoke radiance enters the HDR producer domain once, while
     * scene/depth/froxel graph inputs already retain their own domains.
     */
    struct ParticleVfxRecordInput
    {
        RenderPassRecordIdentity identity{};
        SceneDepthSnapshotRecordInput softDepthSnapshot{};
        RGTextureHandle distortionVectors{};
        RGTextureHandle compositedSceneColor{};
        /** Exact committed producer state for this particle frame. */
        RGBufferHandle exposureState{};
        /** Semantic identity paired with the frozen committed state. */
        uint64 exposureStateContentHash = 0;
        bool softDepthRequested = false;
        bool softDepthRequired = false;
        bool reactiveMaskRequested = false;
        bool distortionRequested = false;

        [[nodiscard]] bool IsEmpty() const noexcept
        {
            return !identity.IsValid() && softDepthSnapshot.IsEmpty() &&
                   !distortionVectors.IsValid() && !compositedSceneColor.IsValid() &&
                   !exposureState.IsValid() && exposureStateContentHash == 0 &&
                   !softDepthRequested && !softDepthRequired &&
                   !reactiveMaskRequested && !distortionRequested;
        }

        [[nodiscard]] bool IsCompatibleWith(
            const RenderPassRecordIdentity& expected,
            RenderOutputExtent expectedExtent) const noexcept
        {
            if (softDepthRequired && !softDepthRequested)
            {
                return false;
            }
            const bool softDepthCompatible = !softDepthRequested
                ? softDepthSnapshot.IsEmpty()
                : softDepthSnapshot.IsEmpty()
                ? !softDepthRequired
                : softDepthSnapshot.IsCompatibleWith(expected, expectedExtent);
            const bool distortionCompatible = !distortionRequested ||
                (HasCurrentGraphProvenance(distortionVectors, expected) &&
                 HasCurrentGraphProvenance(compositedSceneColor, expected));
            return identity == expected && softDepthCompatible &&
                   distortionCompatible && exposureState.IsValid() &&
                   HasCurrentGraphProvenance(exposureState, expected) &&
                   exposureStateContentHash != 0;
        }
    };

    inline bool ObjectVelocityPassStats::MatchesVelocityTarget(
        const RGTextureHandle& handle,
        const RenderPassRecordIdentity& expected) const noexcept
    {
        return handle.IsValid() && expected.IsValid() &&
            velocityTargetResourceIndex == handle.index &&
            velocityTargetGraphIdentity == expected.graphIdentity &&
            velocityTargetRecordingGeneration ==
                expected.graphRecordingGeneration &&
            handle.graphIdentity == expected.graphIdentity &&
            handle.recordingGeneration ==
                expected.graphRecordingGeneration;
    }

    /** @brief Per-recording fullscreen camera motion-vector diagnostics. */
    struct CameraVelocityPassStats
    {
        bool requested = false;
        bool supported = false;
        bool depthAvailable = false;
        bool previousViewProjectionAvailable = false;
        /** Current and previous unjittered view projections differ. */
        bool viewProjectionMotionObserved = false;
        bool velocityTargetAvailable = false;
        bool outputDeclared = false;
        bool constantsUploaded = false;
        bool velocityRecorded = false;
        uint32 width = 0;
        uint32 height = 0;
        /** Exact RenderGraph texture slot written by this recording. */
        uint32 velocityTargetResourceIndex = RVX_INVALID_INDEX;
        /** Exact graph provenance paired with the written texture slot. */
        uint64 velocityTargetGraphIdentity = 0;
        uint64 velocityTargetRecordingGeneration = 0;
        RHIFormat outputFormat = RHIFormat::Unknown;

        [[nodiscard]] bool MatchesVelocityTarget(
            const RGTextureHandle& handle,
            const RenderPassRecordIdentity& expected) const noexcept
        {
            return handle.IsValid() && expected.IsValid() &&
                velocityTargetResourceIndex == handle.index &&
                velocityTargetGraphIdentity == expected.graphIdentity &&
                velocityTargetRecordingGeneration ==
                    expected.graphRecordingGeneration &&
                handle.graphIdentity == expected.graphIdentity &&
                handle.recordingGeneration ==
                    expected.graphRecordingGeneration;
        }
    };

    /**
     * @brief Exact graph-owned ambient-occlusion field consumed by lighting.
     *
     * The field is injected only after the GTAO feature has published its
     * typed blackboard output. It is deliberately separate from ViewData and
     * the frozen scene snapshot: a later RenderGraph producer must not mutate
     * an already-frozen frame merely to attach its lighting dependency.
     */
    struct AmbientOcclusionRecordInput
    {
        RenderPassRecordIdentity identity{};
        RGTextureHandle filteredOcclusion{};
        RenderOutputExtent extent{};
        RHIFormat format = RHIFormat::Unknown;
        RHISampleCount sampleCount = RHISampleCount::Count1;
        uint64 settingsHash = 0;
        uint64 fieldContentHash = 0;
        bool requested = false;

        [[nodiscard]] bool IsCompatibleWith(
            const RenderPassRecordIdentity& expected) const noexcept
        {
            return identity == expected && filteredOcclusion.IsValid() &&
                   HasCurrentGraphProvenance(filteredOcclusion, expected) &&
                   extent.IsValid() && format == RHIFormat::R8_UNORM &&
                   sampleCount == RHISampleCount::Count1 &&
                   settingsHash != 0 && fieldContentHash != 0;
        }
    };

    /**
     * @brief Exact accumulated froxel field shared by every late surface.
     *
     * The value is graph-neutral policy plus one graph handle; feature targets
     * never depend on RenderLighting implementation types.  An identity route
     * intentionally carries no texture, allowing consumers to bind their
     * immutable neutral fallback without advertising GPU execution.
     */
    struct VolumetricLightingRecordInput
    {
        RenderPassRecordIdentity identity{};
        uint64 planIdentity = 0;
        FroxelGridMetadata froxelGrid{};
        RGTextureHandle integratedScatteringTransmittance{};
        bool identityRoute = true;

        [[nodiscard]] bool IsEmpty() const noexcept
        {
            return !identity.IsValid() && planIdentity == 0 &&
                   !froxelGrid.IsValid() &&
                   !integratedScatteringTransmittance.IsValid() &&
                   identityRoute;
        }

        [[nodiscard]] bool IsCompatibleWith(
            const RenderPassRecordIdentity& expected) const noexcept
        {
            if (identity != expected || planIdentity == 0 ||
                !froxelGrid.IsValid())
            {
                return false;
            }
            return identityRoute
                ? !integratedScatteringTransmittance.IsValid()
                : HasCurrentGraphProvenance(
                      integratedScatteringTransmittance, expected);
        }
    };

    /** @brief Validate every optional RenderGraph resource stored in ViewData. */
    [[nodiscard]] inline bool HasCurrentViewDataGraphResources(
        const ViewData& view,
        const RenderPassRecordIdentity& identity) noexcept
    {
        return HasCurrentGraphProvenance(view.colorTarget, identity) &&
               HasCurrentGraphProvenance(view.depthTarget, identity) &&
               HasCurrentGraphProvenance(view.velocityTarget, identity) &&
               HasCurrentGraphProvenance(
                   view.reactiveMaskTarget, identity) &&
               HasCurrentGraphProvenance(
                   view.environmentSkyTexture, identity) &&
               HasCurrentGraphProvenance(
                   view.environmentIrradianceTexture, identity) &&
               HasCurrentGraphProvenance(
                   view.environmentPrefilteredTexture, identity) &&
               HasCurrentGraphProvenance(
                   view.environmentBRDFLUTTexture, identity) &&
               HasCurrentGraphProvenance(view.exposureState, identity);
    }

    /** @brief Typed GPU-culling inputs consumed by one graphics pass. */
    struct RenderPassGPUDrivenInputs
    {
        RenderPassRecordIdentity identity{};
        std::shared_ptr<GPUCullingRecordedState> recordedState;
        RGBufferHandle instances{};
        RGBufferHandle gpuSceneCandidates{};
        RGBufferHandle gpuScenePrimitives{};
        RGBufferHandle gpuSceneTransforms{};
        RGBufferHandle instanceIndices{};
        RGBufferHandle indirectDraws{};
        RGBufferHandle drawCount{};
        std::shared_ptr<const GPUSceneRasterBindingSnapshot> gpuSceneRasterBinding;
        std::shared_ptr<std::atomic_bool> gpuSceneRecordingFailure;
        uint64 gpuSceneLeaseVersion = 0;
        bool gpuSceneRasterEnabled = false;

        [[nodiscard]] bool HasCompleteHandles() const noexcept
        {
            const bool commonHandles = instanceIndices.IsValid() &&
                indirectDraws.IsValid() && drawCount.IsValid();
            if (!gpuSceneRasterEnabled)
            {
                return commonHandles && instances.IsValid();
            }
            return commonHandles && gpuSceneCandidates.IsValid() &&
                gpuScenePrimitives.IsValid() && gpuSceneTransforms.IsValid() &&
                gpuSceneRasterBinding != nullptr &&
                gpuSceneRecordingFailure != nullptr && gpuSceneLeaseVersion != 0;
        }

        [[nodiscard]] bool IsCompatibleWith(
            const RenderPassRecordIdentity& expected) const noexcept
        {
            if (recordedState == nullptr || identity != expected ||
                !HasCompleteHandles() ||
                !HasCurrentGraphProvenance(instanceIndices, expected) ||
                !HasCurrentGraphProvenance(indirectDraws, expected) ||
                !HasCurrentGraphProvenance(drawCount, expected))
            {
                return false;
            }

            if (!gpuSceneRasterEnabled)
            {
                return HasCurrentGraphProvenance(instances, expected);
            }

            return gpuSceneRasterBinding->IsReadyForBinding() &&
                gpuSceneRasterBinding->leaseVersion == gpuSceneLeaseVersion &&
                HasCurrentGraphProvenance(gpuSceneCandidates, expected) &&
                HasCurrentGraphProvenance(gpuScenePrimitives, expected) &&
                HasCurrentGraphProvenance(gpuSceneTransforms, expected);
        }
    };

    /**
     * @brief The single directional light selected for one graph recording.
     *
     * This value is the authoritative primary-light choice for DefaultLit and
     * both directional shadow implementations. ViewData is only a derived
     * projection for shader constants; pass recording must not re-select from
     * scene lights or mutable pass state.
     */
    struct PrimaryDirectionalLightRecordInput
    {
        bool selected = false;
        bool castsShadow = false;
        Vec3 direction{0.5f, -0.8f, 0.3f};
        float32 intensity = 4.0f;
        Vec3 color{1.0f, 1.0f, 1.0f};

        [[nodiscard]] bool IsShadowEligible() const noexcept
        {
            return selected && castsShadow && intensity > 0.0f;
        }
    };

    /**
     * @brief Select the single directional-light record for one scene frame.
     *
     * Scene order is authoritative. The first positive-intensity directional
     * light is retained even when it does not cast shadows, so later casters
     * cannot split DefaultLit and shadow semantics.
     */
    [[nodiscard]] inline PrimaryDirectionalLightRecordInput
    SelectPrimaryDirectionalLightRecordInput(
        const RenderScene& scene,
        uint32 cullingMask = ~0U)
    {
        for (const RenderLight& light : scene.GetLights())
        {
            if (light.type != RenderLight::Type::Directional ||
                !(light.intensity > 0.0f) ||
                !IsRenderLayerVisible(light.layerMask, cullingMask))
            {
                continue;
            }

            PrimaryDirectionalLightRecordInput result;
            result.selected = true;
            result.castsShadow = light.castsShadow;
            result.direction = light.direction;
            result.color = light.color;
            result.intensity = light.intensity;
            return result;
        }
        return {};
    }

    /**
     * @brief Strongly-owned RHI inputs derived from one immutable lighting plan.
     *
     * The directional shadow view is graph-produced and therefore supplied by
     * each consumer at execution. Every other mutable frame-light input is
     * frozen here before graph callbacks are registered.
     */
    struct RenderLightingFrameResourceSnapshot
    {
        uint64 lightingPlanIdentity = 0;
        /** Exact byte-frozen allocations shared by graph imports and descriptors. */
        FrozenFrameLightResources frozenLightResources{};
        /** Current-recording handles for the six frozen light allocations. */
        RGBufferHandle lightConstants{};
        RGBufferHandle pointLights{};
        RGBufferHandle spotLights{};
        RGBufferHandle clusterConstants{};
        RGBufferHandle clusterData{};
        RGBufferHandle clusterLightIndices{};
        RHITextureViewRef environmentIBLIrradianceView;
        RHITextureViewRef environmentIBLPrefilteredView;
        RHITextureViewRef environmentIBLBRDFLUTView;
        /** Exact existing graph imports paired with the retained IBL views. */
        RGTextureHandle environmentIBLIrradiance{};
        RGTextureHandle environmentIBLPrefiltered{};
        RGTextureHandle environmentIBLBRDFLUT{};
        bool textureIBLEnabled = false;

        [[nodiscard]] bool IsComplete() const noexcept
        {
            const bool lightsComplete = lightingPlanIdentity != 0 &&
                frozenLightResources.IsComplete() && lightConstants.IsValid() &&
                pointLights.IsValid() && spotLights.IsValid() &&
                clusterConstants.IsValid() && clusterData.IsValid() &&
                clusterLightIndices.IsValid();
            const bool iblComplete = !textureIBLEnabled ||
                (environmentIBLIrradianceView &&
                 environmentIBLPrefilteredView &&
                 environmentIBLBRDFLUTView &&
                 environmentIBLIrradiance.IsValid() &&
                 environmentIBLPrefiltered.IsValid() &&
                 environmentIBLBRDFLUT.IsValid());
            return lightsComplete && iblComplete;
        }

        [[nodiscard]] bool HasCurrentGraphResources(
            const RenderPassRecordIdentity& identity) const noexcept
        {
            const bool localLightsCurrent =
                   HasCurrentGraphProvenance(lightConstants, identity) &&
                   HasCurrentGraphProvenance(pointLights, identity) &&
                   HasCurrentGraphProvenance(spotLights, identity) &&
                   HasCurrentGraphProvenance(clusterConstants, identity) &&
                   HasCurrentGraphProvenance(clusterData, identity) &&
                   HasCurrentGraphProvenance(clusterLightIndices, identity);
            const bool iblCurrent = !textureIBLEnabled ||
                (HasCurrentGraphProvenance(
                     environmentIBLIrradiance, identity) &&
                 HasCurrentGraphProvenance(
                     environmentIBLPrefiltered, identity) &&
                 HasCurrentGraphProvenance(
                     environmentIBLBRDFLUT, identity));
            return localLightsCurrent && iblCurrent;
        }

        [[nodiscard]] bool MatchesEnvironmentGraphHandles(
            const ViewData& view) const noexcept
        {
            if ((view.textureIBLEnabled != 0) != textureIBLEnabled)
            {
                return false;
            }
            if (!textureIBLEnabled)
            {
                return true;
            }
            const auto sameTexture = [](
                const RGTextureHandle& left,
                const RGTextureHandle& right) noexcept
            {
                return left.IsValid() && right.IsValid() &&
                    left.index == right.index &&
                    left.graphIdentity == right.graphIdentity &&
                    left.recordingGeneration == right.recordingGeneration;
            };
            return sameTexture(
                       view.environmentIrradianceTexture,
                       environmentIBLIrradiance) &&
                   sameTexture(
                       view.environmentPrefilteredTexture,
                       environmentIBLPrefiltered) &&
                   sameTexture(
                       view.environmentBRDFLUTTexture,
                       environmentIBLBRDFLUT);
        }

        /**
         * @brief Resolve graph-owned views and prove that descriptors sample
         *        the exact allocations whose transitions were declared.
         */
        [[nodiscard]] bool ResolveEnvironmentResources(
            RHITextureView* irradiance,
            RHITextureView* prefiltered,
            RHITextureView* brdfLUT,
            EnvironmentIBLFrameResources& outResources) const noexcept
        {
            outResources = {};
            if (!textureIBLEnabled)
            {
                return true;
            }

            const auto isExactView = [](
                RHITextureView* graphView,
                const RHITextureViewRef& retainedView) noexcept
            {
                if (graphView == nullptr || !retainedView ||
                    graphView->GetTexture() != retainedView->GetTexture() ||
                    graphView->GetFormat() != retainedView->GetFormat())
                {
                    return false;
                }
                const RHISubresourceRange& graphRange =
                    graphView->GetSubresourceRange();
                const RHISubresourceRange& retainedRange =
                    retainedView->GetSubresourceRange();
                return graphRange.baseMipLevel == retainedRange.baseMipLevel &&
                    graphRange.mipLevelCount == retainedRange.mipLevelCount &&
                    graphRange.baseArrayLayer == retainedRange.baseArrayLayer &&
                    graphRange.arrayLayerCount == retainedRange.arrayLayerCount &&
                    graphRange.aspect == retainedRange.aspect;
            };

            if (!isExactView(irradiance, environmentIBLIrradianceView) ||
                !isExactView(prefiltered, environmentIBLPrefilteredView) ||
                !isExactView(brdfLUT, environmentIBLBRDFLUTView))
            {
                return false;
            }

            outResources.enabled = true;
            outResources.irradianceView = irradiance;
            outResources.prefilteredEnvironmentView = prefiltered;
            outResources.brdfLUTView = brdfLUT;
            return true;
        }
    };

    /** @brief Project a pass-record identity into the deformation resource ABI. */
    [[nodiscard]] inline RenderDeformationFrameIdentity
    MakeRenderDeformationFrameIdentity(
        const RenderPassRecordIdentity& identity) noexcept
    {
        return RenderDeformationFrameIdentity{
            identity.graphIdentity,
            identity.graphRecordingGeneration,
            identity.frameSequence,
            identity.viewOrdinal,
            identity.recordEpoch};
    }

    /** @brief Value-owned scene state used by typed main-scene pass recording. */
    struct RenderPassFrameSnapshot
    {
        RenderPassRecordIdentity identity{};
        /** Exact immutable lighting selection shared by every surface path. */
        std::shared_ptr<const LightingFramePlan> lightingPlan;
        /** Strong RHI realization of that exact lighting plan. */
        std::shared_ptr<const RenderLightingFrameResourceSnapshot>
            lightingResources;
        /** Exact current/previous skin-palette buffers for this recording. */
        std::shared_ptr<const RenderDeformationFrameResources>
            deformationResources;
        PrimaryDirectionalLightRecordInput primaryDirectionalLight{};
        ViewData view{};
        RenderFrameExecutionPlan executionPlan{};
        RenderViewDrawPlan drawPlan{};
        SceneMeshPassPreparation meshPassPreparation{};
        SceneRenderInstanceBatchPlans instanceBatchPlans{};
        RenderVisibilityResult visibility{};
        RenderScene scene{};
        // Skybox recording must consume the frame-owned value rather than the
        // mutable SkyboxPass compatibility setters used before graph recording.
        RenderSkySnapshot sky{};
        std::vector<RenderDrawItem> opaqueDrawItems{};
        std::vector<RenderDrawItem> maskedDrawItems{};
        // Transparent work must preserve the renderer's back-to-front order.
        // The value copy keeps a later frame's sort/rebuild from changing a
        // graph that is still waiting to execute.
        std::vector<RenderDrawItem> transparentDrawItems{};
    };

    /** @brief Graph-owned directional-shadow output produced for Opaque. */
    struct DirectionalShadowRecordOutput
    {
        RenderPassRecordIdentity identity{};
        /** Identity of the LightingFramePlan that requested this output. */
        uint64 lightingPlanIdentity = 0;
        bool enabled = false;
        RGTextureHandle shadowMap{};
        uint32 shadowMapSize = 0;
        float32 cascadeBlendRatio = 0.0f;
        float32 shadowBias = 0.0f;
        float32 normalBias = 0.0f;
        float32 filterRadiusTexels = 0.0f;
        std::vector<Mat4> cascadeViewProjections{};
        std::vector<float32> cascadeSplitDepths{};

        [[nodiscard]] bool IsCompatibleWith(
            const RenderPassRecordIdentity& expected) const noexcept
        {
            if (identity != expected ||
                !HasCurrentGraphProvenance(shadowMap, expected))
            {
                return false;
            }
            if (!enabled)
            {
                return true;
            }
            const size_t cascadeCount = cascadeViewProjections.size();
            return shadowMap.IsValid() && shadowMapSize != 0 &&
                   cascadeCount != 0 &&
                   cascadeCount <= RVX_MAX_DIRECTIONAL_SHADOW_CASCADES &&
                   cascadeSplitDepths.size() == cascadeCount;
        }

        [[nodiscard]] bool IsCompatibleWith(
            const RenderPassRecordIdentity& expected,
            uint64 expectedLightingPlanIdentity) const noexcept
        {
            return expectedLightingPlanIdentity != 0 &&
                   lightingPlanIdentity == expectedLightingPlanIdentity &&
                   IsCompatibleWith(expected);
        }
    };

    /**
     * @brief Graph-owned point/spot depth-atlas output produced before lighting.
     *
     * The atlas plan identity and generation are deliberately retained beside
     * the RenderGraph provenance.  A local-light consumer therefore cannot
     * accidentally sample a page selected for another frame, view, or atlas
     * generation.
     */
    struct LocalShadowRecordOutput
    {
        RenderPassRecordIdentity identity{};
        RenderFrameIdentity frameIdentity{};
        RenderViewIdentity viewIdentity{};
        uint64 lightingPlanIdentity = 0;
        uint64 localShadowPlanIdentity = 0;
        uint64 atlasPlanIdentity = 0;
        uint64 atlasGeneration = 0;
        RGTextureHandle depthAtlas{};
        std::vector<RGTextureHandle> pageDepthLayers{};
        uint32 pageResolution = 0;
        uint32 pageCount = 0;
        bool enabled = false;

        [[nodiscard]] bool IsCompatibleWith(
            const RenderPassRecordIdentity& expected,
            const RenderFrameIdentity& expectedFrame,
            const RenderViewIdentity& expectedView,
            uint64 expectedLightingPlanIdentity) const noexcept
        {
            if (identity != expected || frameIdentity != expectedFrame ||
                viewIdentity != expectedView ||
                expectedLightingPlanIdentity == 0 ||
                lightingPlanIdentity != expectedLightingPlanIdentity ||
                !HasCurrentGraphProvenance(depthAtlas, expected))
            {
                return false;
            }
            if (!enabled)
            {
                return !depthAtlas.IsValid() && pageDepthLayers.empty() &&
                       pageResolution == 0 && pageCount == 0 &&
                       localShadowPlanIdentity == 0 && atlasPlanIdentity == 0 &&
                       atlasGeneration == 0;
            }
            if (!frameIdentity.IsValid() || !viewIdentity.IsValid() ||
                localShadowPlanIdentity == 0 || atlasPlanIdentity == 0 ||
                atlasGeneration == 0 || !depthAtlas.IsValid() ||
                pageResolution == 0 || pageCount == 0 ||
                pageDepthLayers.size() != pageCount)
            {
                return false;
            }
            return std::all_of(
                pageDepthLayers.begin(), pageDepthLayers.end(),
                [this, &expected](const RGTextureHandle& page)
                {
                    return page.IsValid() &&
                        page.index == depthAtlas.index &&
                        HasCurrentGraphProvenance(page, expected) &&
                        page.hasSubresourceRange &&
                        page.subresourceRange.baseMipLevel == 0 &&
                        page.subresourceRange.mipLevelCount == 1 &&
                        page.subresourceRange.arrayLayerCount == 1 &&
                        page.subresourceRange.aspect == RHITextureAspect::Depth;
                });
        }

        /**
         * @brief Proves that every graph layer is the exact page sealed by the
         * immutable atlas plan, rather than merely another view of the atlas.
         */
        [[nodiscard]] bool MatchesAtlasPlan(
            const LocalShadowAtlasPlan& plan) const noexcept
        {
            const auto& pages = plan.GetPages();
            if (!plan.IsValid() || !enabled ||
                frameIdentity != plan.GetFrameIdentity() ||
                viewIdentity != plan.GetViewIdentity() ||
                lightingPlanIdentity != plan.GetLightingPlanIdentity() ||
                localShadowPlanIdentity != plan.GetLocalShadowPlanIdentity() ||
                atlasPlanIdentity != plan.GetIdentityHash() ||
                atlasGeneration != plan.GetAtlasGeneration() ||
                pageCount != pages.size() ||
                pageDepthLayers.size() != pages.size() ||
                pageResolution != plan.GetTextureContract().pageResolution)
            {
                return false;
            }

            for (size_t index = 0; index < pages.size(); ++index)
            {
                const RGTextureHandle& layer = pageDepthLayers[index];
                if (!layer.IsValid() || layer.index != depthAtlas.index ||
                    !layer.hasSubresourceRange ||
                    layer.subresourceRange.baseMipLevel != 0 ||
                    layer.subresourceRange.mipLevelCount != 1 ||
                    layer.subresourceRange.baseArrayLayer !=
                        pages[index].pageIndex ||
                    layer.subresourceRange.arrayLayerCount != 1 ||
                    layer.subresourceRange.aspect != RHITextureAspect::Depth)
                {
                    return false;
                }
            }
            return true;
        }
    };

    /**
     * @brief Project one plan-bound CSM output into shader-visible ViewData.
     *
     * Opaque, transparent, and deferred lighting use this single conversion so
     * cascade splits/fades cannot diverge between surface paths.
     */
    [[nodiscard]] inline bool ResolveDirectionalShadowViewData(
        ViewData& view,
        const DirectionalShadowRecordOutput& shadow,
        const RenderPassRecordIdentity& identity,
        uint64 lightingPlanIdentity) noexcept
    {
        view.directionalShadowEnabled = 0;
        view.directionalShadowCascadeCount = 0;
        view.directionalShadowCascadeSplits = Vec4(0.0f);
        view.directionalShadowCascadeFadeDistances = Vec4(0.0f);
        view.directionalShadowViewProjection = Mat4Identity();
        view.directionalShadowInvMapSize = 0.0f;
        view.directionalShadowDepthBias = 0.0f;
        view.directionalShadowNormalBias = 0.0f;
        view.directionalShadowFilterRadiusTexels = 1.0f;
        view.directionalShadowStrength = 1.0f;

        if (!shadow.IsCompatibleWith(identity, lightingPlanIdentity))
        {
            return false;
        }
        if (!shadow.enabled)
        {
            return true;
        }

        const uint32 cascadeCount = std::min(
            static_cast<uint32>(shadow.cascadeViewProjections.size()),
            RVX_MAX_DIRECTIONAL_SHADOW_CASCADES);
        if (cascadeCount == 0)
        {
            return false;
        }
        const float32 nearClip = std::max(0.001f, view.nearPlane);
        const float32 farClip = std::max(nearClip + 1.0f, view.farPlane);
        const float32 clipRange = farClip - nearClip;
        view.directionalShadowEnabled = 1;
        view.directionalShadowCascadeCount = cascadeCount;
        for (uint32 cascade = 0; cascade < cascadeCount; ++cascade)
        {
            view.directionalShadowViewProjections[cascade] =
                shadow.cascadeViewProjections[cascade];
            const float32 normalizedSplit = cascade <
                    shadow.cascadeSplitDepths.size()
                ? shadow.cascadeSplitDepths[cascade]
                : 0.0f;
            view.directionalShadowCascadeSplits[cascade] =
                nearClip + normalizedSplit * clipRange;
        }
        const float32 blendRatio = std::clamp(
            shadow.cascadeBlendRatio, 0.0f, 1.0f);
        for (uint32 cascade = 0; cascade + 1 < cascadeCount; ++cascade)
        {
            const float32 previousSplit = cascade == 0
                ? nearClip
                : view.directionalShadowCascadeSplits[cascade - 1];
            const float32 span = std::max(
                0.0f,
                view.directionalShadowCascadeSplits[cascade] - previousSplit);
            view.directionalShadowCascadeFadeDistances[cascade] =
                std::min(span, span * blendRatio);
        }
        view.directionalShadowViewProjection =
            shadow.cascadeViewProjections.front();
        view.directionalShadowDepthBias = shadow.shadowBias;
        view.directionalShadowNormalBias = shadow.normalBias;
        view.directionalShadowInvMapSize = shadow.shadowMapSize != 0
            ? 1.0f / static_cast<float32>(shadow.shadowMapSize)
            : 0.0f;
        view.directionalShadowFilterRadiusTexels =
            shadow.filterRadiusTexels;
        return true;
    }

    /** @brief Per-record execution gate shared by producer and consumers. */
    struct RayTracedShadowExecutionState
    {
        RenderPassRecordIdentity identity{};
        std::atomic<bool> dispatchReady{false};
        std::atomic<bool> executionFailed{false};
    };

    /** @brief Graph-owned ray-traced-shadow output produced for Opaque. */
    struct RayTracedShadowRecordOutput
    {
        RenderPassRecordIdentity identity{};
        bool enabled = false;
        RGTextureHandle shadowMask{};
        float32 filterRadiusTexels = 0.0f;
        RayTracedShadowMode mode = RayTracedShadowMode::ComplementRaster;
        std::shared_ptr<RayTracedShadowExecutionState> executionState;

        [[nodiscard]] bool IsCompatibleWith(
            const RenderPassRecordIdentity& expected) const noexcept
        {
            return identity == expected &&
                   HasCurrentGraphProvenance(shadowMask, expected) &&
                   (!enabled || (shadowMask.IsValid() && executionState != nullptr &&
                                 executionState->identity == expected));
        }
    };

    /**
     * @brief Exact external texture state produced by one graph recording.
     *
     * The resource registry is updated only after the owning frame obtains a
     * terminal submission token. Until then SceneRenderer retains the prior
     * snapshot for rollback.
     */
    struct RenderGraphExternalTextureAccess
    {
        RenderResourceHandle resource{};
        RGTextureHandle graphTexture{};
    };

    /**
     * @brief Graph-recording receipt for a material which a raster draw
     * actually recorded.  Later presentation qualification supplies the
     * frame/presentation sequence; until then this receipt is not observable
     * in completed-frame diagnostics.
     */
    struct RenderRecordedMaterialBindingReceipt
    {
        RenderResourceHandle material{};
        uint64 contentRevision = 0;
        uint64 descriptorContentKey = 0;
        uint64 descriptorRevision = 0;
        std::vector<MaterialBindingTextureEntry> textureEntries{};
        bool fallbackUsed = false;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return material.IsValid() && contentRevision != 0 &&
                   descriptorContentKey != 0 && descriptorRevision != 0 &&
                   std::all_of(textureEntries.begin(), textureEntries.end(),
                       [](const MaterialBindingTextureEntry& entry)
                       {
                           return entry.IsValid();
                       });
        }
    };

    /** @brief Graph-recording receipt for a complete palette used by a Direct draw. */
    struct RenderRecordedSkinningPaletteReceipt
    {
        RenderSkinningPaletteMetadata metadata{};
        RenderSkinningPaletteExecutionLane lane =
            RenderSkinningPaletteExecutionLane::Direct;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return metadata.providerComponentId != 0 &&
                   metadata.sourceModelResourceId != 0 &&
                   metadata.poseSequence != 0 && metadata.paletteHash != 0 &&
                   metadata.paletteCount != 0 &&
                   lane == RenderSkinningPaletteExecutionLane::Direct;
        }
    };

    enum class RenderLightingSurfaceRoute : uint8
    {
        ForwardOpaque = 0,
        DeferredOpaque,
        ForwardSpecialOpaque,
        Transparent,
        Count,
    };

    /** @brief Actual work evidence emitted only after a lighting consumer ran. */
    struct RenderLightingSurfaceExecutionEvidence
    {
        uint64 lightingPlanIdentity = 0;
        RenderLightingSurfaceRoute route =
            RenderLightingSurfaceRoute::ForwardOpaque;
        /** Bits 0..5 correspond to the six frozen light/cluster buffers. */
        uint8 frozenBufferMask = 0;
        bool directionalShadowBound = false;
        bool localShadowBound = false;
        uint64 localShadowPlanIdentity = 0;
        uint64 localShadowAtlasPlanIdentity = 0;
        uint64 localShadowAtlasGeneration = 0;
        uint32 localShadowPageCount = 0;
        bool textureIBLBound = false;
        bool exactIBLViews = false;
        /** Exact R8 visibility field consumed by opaque indirect lighting. */
        uint64 ambientOcclusionFieldContentHash = 0;
        uint64 workCount = 0;
        bool executed = false;

        [[nodiscard]] bool IsValid() const noexcept
        {
            const bool requiresAmbientOcclusion =
                route == RenderLightingSurfaceRoute::ForwardOpaque ||
                route == RenderLightingSurfaceRoute::DeferredOpaque;
            const bool localShadowEvidenceValid = localShadowBound
                ? localShadowPlanIdentity != 0 &&
                    localShadowAtlasPlanIdentity != 0 &&
                    localShadowAtlasGeneration != 0 &&
                    localShadowPageCount != 0
                : localShadowPlanIdentity == 0 &&
                    localShadowAtlasPlanIdentity == 0 &&
                    localShadowAtlasGeneration == 0 &&
                    localShadowPageCount == 0;
            return lightingPlanIdentity != 0 &&
                route < RenderLightingSurfaceRoute::Count &&
                frozenBufferMask == 0x3fu && workCount != 0 && executed &&
                localShadowEvidenceValid &&
                (!textureIBLBound || exactIBLViews) &&
                (requiresAmbientOcclusion
                     ? ambientOcclusionFieldContentHash != 0
                     : ambientOcclusionFieldContentHash == 0);
        }
    };

    /** @brief Lifetime-owned mutable outputs for one graph recording. */
    struct RenderPassRecordResults
    {
        RenderPassRecordIdentity identity{};
        DirectionalShadowRecordOutput directionalShadowOutput{};
        LocalShadowRecordOutput localShadowOutput{};
        RayTracedShadowRecordOutput rayTracedShadowOutput{};
        RenderFrameExecutionReport executionReport{};
        DepthPrepassDrawStats depthStats{};
        ShadowPassStats shadowStats{};
        RayTracedShadowPassStats rayTracedShadowStats{};
        RayTracedReflectionPassStats rayTracedReflectionStats{};
        RayTracedReflectionDenoisePassStats rayTracedReflectionDenoiseStats{};
        RayTracedReflectionCompositePassStats rayTracedReflectionCompositeStats{};
        OpaquePassDrawStats opaqueStats{};
        OpaquePassShadowStats opaqueShadowStats{};
        OpaqueSurfaceRouteExecutionEvidence deferredOpaqueRoute{};
        OpaqueSurfaceRouteExecutionEvidence forwardSpecialOpaqueRoute{};
        TransparentPassDrawStats transparentStats{};
        ObjectVelocityPassStats objectVelocityStats{};
        CameraVelocityPassStats cameraVelocityStats{};
        std::array<RenderLightingSurfaceExecutionEvidence,
                   static_cast<size_t>(RenderLightingSurfaceRoute::Count)>
            lightingSurfaceEvidence{};
        /** Sorted and de-duplicated by exact slot/generation. */
        std::vector<RenderRecordedMaterialBindingReceipt>
            successfulMaterialBindings{};
        bool successfulMaterialBindingsOverflow = false;
        /** Sorted and de-duplicated by the complete receipt value tuple. */
        std::vector<RenderRecordedSkinningPaletteReceipt>
            successfulSkinningPalettes{};
        bool successfulSkinningPalettesOverflow = false;
        std::vector<RenderGraphExternalTextureAccess>
            externalTextureAccesses{};

        [[nodiscard]] bool RecordLightingSurfaceExecution(
            const RenderLightingSurfaceExecutionEvidence& evidence) noexcept
        {
            if (!evidence.IsValid())
            {
                return false;
            }
            const size_t index = static_cast<size_t>(evidence.route);
            if (index >= lightingSurfaceEvidence.size())
            {
                return false;
            }
            if (lightingSurfaceEvidence[index].IsValid())
            {
                return false;
            }
            lightingSurfaceEvidence[index] = evidence;
            return true;
        }

        /**
         * Record an exact successful fallback-or-ready material binding. A
         * newer descriptor revision/key for the same material generation
         * supersedes the prior receipt; a different generation remains
         * deliberately distinct.
         */
        [[nodiscard]] bool RecordSuccessfulMaterialBinding(
            const MaterialBindingResult& binding)
        {
            RenderRecordedMaterialBindingReceipt receipt;
            receipt.material = binding.material;
            receipt.contentRevision = binding.contentRevision;
            receipt.descriptorContentKey = binding.descriptorContentKey;
            receipt.descriptorRevision = binding.descriptorRevision;
            receipt.textureEntries = binding.textureEntries;
            receipt.fallbackUsed = binding.usedFallback;
            if (!binding.IsDrawable() || !receipt.IsValid())
            {
                return false;
            }

            const auto position = std::lower_bound(
                successfulMaterialBindings.begin(),
                successfulMaterialBindings.end(),
                receipt.material,
                [](const RenderRecordedMaterialBindingReceipt& receipt,
                   RenderResourceHandle candidate)
                {
                    return receipt.material < candidate;
                });
            if (position != successfulMaterialBindings.end() &&
                position->material == receipt.material)
            {
                if (receipt.descriptorRevision > position->descriptorRevision ||
                    (receipt.descriptorRevision == position->descriptorRevision &&
                     receipt.descriptorContentKey >
                         position->descriptorContentKey))
                {
                    *position = std::move(receipt);
                }
                return true;
            }
            if (successfulMaterialBindings.size() >=
                RVX_RENDER_PRESENTED_MATERIAL_RECEIPT_CAPACITY)
            {
                successfulMaterialBindingsOverflow = true;
                return false;
            }
            successfulMaterialBindings.insert(
                position, std::move(receipt));
            return true;
        }

        /** Record a palette only after its exact Direct draw was recorded. */
        [[nodiscard]] bool RecordSuccessfulSkinningPalette(
            const RenderSkinningPaletteMetadata& metadata,
            RenderSkinningPaletteExecutionLane lane)
        {
            RenderRecordedSkinningPaletteReceipt receipt{metadata, lane};
            if (!receipt.IsValid())
            {
                return false;
            }
            const auto tupleFor = [](const RenderRecordedSkinningPaletteReceipt& value)
            {
                return std::tie(value.metadata.providerComponentId,
                                value.metadata.sourceModelResourceId,
                                value.metadata.poseSequence,
                                value.metadata.paletteHash,
                                value.metadata.paletteCount,
                                value.lane);
            };
            const auto position = std::lower_bound(
                successfulSkinningPalettes.begin(),
                successfulSkinningPalettes.end(),
                receipt,
                [&tupleFor](const RenderRecordedSkinningPaletteReceipt& left,
                             const RenderRecordedSkinningPaletteReceipt& right)
                {
                    return tupleFor(left) < tupleFor(right);
                });
            if (position != successfulSkinningPalettes.end() &&
                tupleFor(*position) == tupleFor(receipt))
            {
                return true;
            }
            if (successfulSkinningPalettes.size() >=
                RVX_RENDER_PRESENTED_SKINNING_PALETTE_RECEIPT_CAPACITY)
            {
                successfulSkinningPalettesOverflow = true;
                return false;
            }
            successfulSkinningPalettes.insert(position, std::move(receipt));
            return true;
        }
    };

    /**
     * @brief Exact immutable material-table identity routed into one opaque pass.
     *
     * This is a value contract so pass execution never reaches back into a
     * mutable adapter/snapshot. It is empty for routes without a classification
     * table and becomes observable in draw statistics only after a successful
     * material binding and draw submission.
     */
    struct OpaqueMaterialTableExecutionIdentity
    {
        uint32 schemaVersion = 0;
        uint32 materialTableSchemaVersion = 0;
        uint32 stride = 0;
        uint32 slotCount = 0;
        uint64 contentHash = 0;

        [[nodiscard]] bool IsEmpty() const noexcept
        {
            return schemaVersion == 0 && materialTableSchemaVersion == 0 &&
                   stride == 0 && slotCount == 0 && contentHash == 0;
        }

        [[nodiscard]] bool IsStructurallyValid() const noexcept
        {
            return schemaVersion == 1 && materialTableSchemaVersion != 0 &&
                   stride != 0 && slotCount != 0 && contentHash != 0;
        }
    };

    /**
     * @brief Immutable source context used to create graph-owned pass data.
     *
     * The ViewData snapshot is intentionally value-owned. Scene references are
     * frame-retained by the renderer and are copied into typed pass data before
     * callbacks are registered.
     */
    struct RenderPassRecordContext
    {
        ViewData view{};
        PrimaryDirectionalLightRecordInput primaryDirectionalLight{};
        RenderPassRecordIdentity identity{};
        /** Full accepted-frame identity paired with this recording. */
        RenderFrameIdentity featureFrameIdentity{};
        /** Frozen simulation clock shared by all deterministic render effects. */
        EffectFrameTime effectTime{};
        /** Exact logical view identity paired with this recording. */
        RenderViewIdentity featureViewIdentity{};
        const RenderFrameExecutionPlan* executionPlan = nullptr;
        const RenderViewDrawPlan* drawPlan = nullptr;
        const SceneMeshPassPreparation* meshPassPreparation = nullptr;
        const SceneRenderInstanceBatchPlans* instanceBatchPlans = nullptr;
        const RenderVisibilityResult* visibility = nullptr;
        RenderFrameExecutionReport* executionReport = nullptr;
        const RenderScene* renderScene = nullptr;
        const std::vector<RenderDrawItem>* opaqueDrawItems = nullptr;
        const std::vector<RenderDrawItem>* maskedDrawItems = nullptr;
        const std::vector<RenderDrawItem>* transparentDrawItems = nullptr;
        /** Explicit owner for an optional Direct Opaque post-fence readback. */
        DirectRasterReadbackQualification* directOpaqueRasterReadbackQualification =
            nullptr;
        /** Frozen physical RHI device slot selected for this graph recording. */
        uint32 directOpaqueRasterReadbackSourceFrameSlot = RVX_INVALID_INDEX;
        RenderPassGPUDrivenInputs depthGPUDriven{};
        RenderPassGPUDrivenInputs opaqueGPUDriven{};
        /** Renderer-issued immutable lighting plan for this exact frame/view. */
        std::shared_ptr<const LightingFramePlan> lightingPlan;
        std::shared_ptr<const RenderLightingFrameResourceSnapshot>
            lightingResources;
        std::shared_ptr<const RenderDeformationFrameResources>
            deformationResources;
        /** Immutable local point/spot atlas selected for this exact view. */
        std::shared_ptr<const LocalShadowAtlasPlan> localShadowAtlasPlan;
        /** Renderer-owned recorder retained through the accepted graph frame. */
        std::shared_ptr<LocalShadowPass> localShadowPass;
        DirectionalShadowRecordOutput directionalShadow{};
        RayTracedShadowRecordOutput rayTracedShadow{};
        /** Frozen TLAS build-to-dispatch dependency for Hybrid RT. */
        RayTracingSceneRecordInput rayTracingScene{};
        /** Screen-space AO selected by the typed lighting dependency edge. */
        AmbientOcclusionRecordInput ambientOcclusion{};
        /** Shared froxel field selected before water/mesh/particle translucency. */
        VolumetricLightingRecordInput volumetricLighting{};
        /** Final HDR surface resolved after the frozen frame snapshot. */
        RenderPassLateBoundHdrAttachment lateBoundHdrAttachment{};
        /** Opaque HDR snapshot late-bound after the immutable frame snapshot. */
        OpaqueSceneSnapshotRecordInput opaqueSceneSnapshot{};
        /** Single D32 snapshot shared by Water and Particle for this graph view. */
        SceneDepthSnapshotRecordInput sceneDepthSnapshot{};
        /** Opaque HDR plus copied D32 inputs late-bound for deterministic water. */
        WaterSurfaceRecordInput waterSurface{};
        /** Graph-owned soft-depth/distortion chain late-bound by composition. */
        ParticleVfxRecordInput particleVfx{};
        /** Explicit value identity for the route's immutable material table. */
        OpaqueMaterialTableExecutionIdentity opaqueMaterialTable{};
        /** Sealed reflection plan requires materialized receiver MRT outputs. */
        bool requiresReflectionSurfaceData = false;
        std::shared_ptr<const RenderPassFrameSnapshot> frameSnapshot;
        std::shared_ptr<RenderPassRecordResults> results;
        /// True only for the legacy ViewData adapter.  Migrated scene passes
        /// reject it and require a renderer-issued recording epoch.
        bool legacyAdapter = false;

        [[nodiscard]] bool MatchesTargetGraph(
            const RenderGraph& targetGraph) const noexcept
        {
            return identity.Matches(targetGraph) &&
                   HasCurrentViewDataGraphResources(view, identity);
        }

        [[nodiscard]] bool IsFrameIdentityValid() const noexcept
        {
            if (!identity.IsValid() || !MatchesTargetGraph(*identity.graph))
            {
                return false;
            }
            if (view.renderFrameExecutionPlan != executionPlan ||
                view.renderDrawPlan != drawPlan ||
                view.meshPassPreparation != meshPassPreparation ||
                view.instanceBatchPlans != instanceBatchPlans ||
                view.renderVisibility != visibility ||
                view.renderFrameExecutionReport != executionReport)
            {
                return false;
            }
            if (executionPlan != nullptr &&
                (executionPlan->frameSequence != identity.frameSequence ||
                 executionPlan->viewOrdinal != identity.viewOrdinal))
            {
                return false;
            }
            if (drawPlan != nullptr &&
                (drawPlan->frameSequence != identity.frameSequence ||
                 drawPlan->viewOrdinal != identity.viewOrdinal))
            {
                return false;
            }
            if (executionReport != nullptr && executionReport->frameSequence != 0 &&
                executionReport->frameSequence != identity.frameSequence)
            {
                return false;
            }
            if (lightingPlan)
            {
                if (!lightingPlan->IsValid() ||
                    lightingPlan->GetFrameIdentity() != featureFrameIdentity ||
                    lightingPlan->GetViewIdentity() != featureViewIdentity)
                {
                    return false;
                }
                if (lightingResources &&
                    (!lightingResources->IsComplete() ||
                     lightingResources->lightingPlanIdentity !=
                         lightingPlan->GetIdentityHash() ||
                     !lightingResources->MatchesEnvironmentGraphHandles(view)))
                {
                    return false;
                }
            }
            if ((localShadowAtlasPlan == nullptr) !=
                (localShadowPass == nullptr))
            {
                return false;
            }
            if (localShadowAtlasPlan &&
                (!localShadowAtlasPlan->IsValid() ||
                 localShadowAtlasPlan->GetFrameIdentity() !=
                     featureFrameIdentity ||
                 localShadowAtlasPlan->GetViewIdentity() !=
                     featureViewIdentity ||
                 lightingPlan == nullptr ||
                 localShadowAtlasPlan->GetLightingPlanIdentity() !=
                     lightingPlan->GetIdentityHash()))
            {
                return false;
            }
            const bool sceneRequiresDeformation = renderScene != nullptr &&
                std::any_of(renderScene->GetObjects().begin(),
                            renderScene->GetObjects().end(),
                            [](const RenderObject& object)
                            {
                                return object.HasSkinningData();
                            });
            if ((sceneRequiresDeformation && !deformationResources) ||
                (deformationResources &&
                 (!deformationResources->MatchesIdentity(
                      MakeRenderDeformationFrameIdentity(identity)) ||
                  !deformationResources->MatchesGraphProvenance(
                      *identity.graph))))
            {
                return false;
            }
            if (!opaqueMaterialTable.IsEmpty() &&
                !opaqueMaterialTable.IsStructurallyValid())
            {
                return false;
            }
            if (!opaqueSceneSnapshot.IsEmpty() &&
                !opaqueSceneSnapshot.IsCompatibleWith(identity))
            {
                return false;
            }
            if (!volumetricLighting.IsEmpty() &&
                !volumetricLighting.IsCompatibleWith(identity))
            {
                return false;
            }
            if (!lateBoundHdrAttachment.IsEmpty() &&
                (!lateBoundHdrAttachment.IsCompatibleWith(identity) ||
                 !AreSameGraphTextureHandle(
                     view.colorTarget,
                     lateBoundHdrAttachment.finalHdrSceneColor)))
            {
                return false;
            }
            if (!rayTracingScene.IsEmpty() &&
                !rayTracingScene.IsCompatibleWith(identity))
            {
                return false;
            }
            return true;
        }
    };

    /** @brief Common graph-owned execution payload copied from a record context. */
    struct RenderPassExecutionData
    {
        ViewData view{};
        RenderPassRecordIdentity identity{};
        std::shared_ptr<const RenderPassFrameSnapshot> frameSnapshot;
        std::shared_ptr<RenderPassRecordResults> results;
        std::shared_ptr<const LightingFramePlan> lightingPlan;
        std::shared_ptr<const RenderLightingFrameResourceSnapshot>
            lightingResources;
        std::shared_ptr<const RenderDeformationFrameResources>
            deformationResources;
        std::shared_ptr<const LocalShadowAtlasPlan> localShadowAtlasPlan;
        std::shared_ptr<LocalShadowPass> localShadowPass;
        /** Immutable local-atlas result copied from the producer record. */
        LocalShadowRecordOutput localShadowOutput{};
        DirectionalShadowRecordOutput directionalShadow{};
        RayTracedShadowRecordOutput rayTracedShadow{};
        RayTracingSceneRecordInput rayTracingScene{};
        AmbientOcclusionRecordInput ambientOcclusion{};
        VolumetricLightingRecordInput volumetricLighting{};
        RenderPassLateBoundHdrAttachment lateBoundHdrAttachment{};
        OpaqueSceneSnapshotRecordInput opaqueSceneSnapshot{};
        OpaqueMaterialTableExecutionIdentity opaqueMaterialTable{};

        [[nodiscard]] const RenderFrameExecutionPlan* GetExecutionPlan() const
        {
            return frameSnapshot ? &frameSnapshot->executionPlan : nullptr;
        }
        [[nodiscard]] const SceneMeshPassPreparation* GetMeshPassPreparation() const
        {
            return frameSnapshot ? &frameSnapshot->meshPassPreparation : nullptr;
        }
        [[nodiscard]] const RenderViewDrawPlan* GetDrawPlan() const
        {
            return frameSnapshot && frameSnapshot->drawPlan.IsValid()
                       ? &frameSnapshot->drawPlan
                       : nullptr;
        }
        [[nodiscard]] const RenderVisibilityResult* GetVisibility() const
        {
            return frameSnapshot ? &frameSnapshot->visibility : nullptr;
        }
        [[nodiscard]] RenderFrameExecutionReport* GetExecutionReport() const
        {
            return results ? &results->executionReport : nullptr;
        }

        [[nodiscard]] bool IsFrameIdentityValid() const noexcept
        {
            if (!identity.IsValid() || !identity.Matches(*identity.graph) ||
                !HasCurrentViewDataGraphResources(view, identity))
            {
                return false;
            }
            if (!opaqueSceneSnapshot.IsEmpty() &&
                !opaqueSceneSnapshot.IsCompatibleWith(identity))
            {
                return false;
            }
            if (!volumetricLighting.IsEmpty() &&
                !volumetricLighting.IsCompatibleWith(identity))
            {
                return false;
            }
            if (!lateBoundHdrAttachment.IsEmpty() &&
                (!lateBoundHdrAttachment.IsCompatibleWith(identity) ||
                 !AreSameGraphTextureHandle(
                     view.colorTarget,
                     lateBoundHdrAttachment.finalHdrSceneColor)))
            {
                return false;
            }
            if (!rayTracingScene.IsEmpty() &&
                !rayTracingScene.IsCompatibleWith(identity))
            {
                return false;
            }
            if (!frameSnapshot || !results ||
                frameSnapshot->identity != identity ||
                results->identity != identity ||
                frameSnapshot->lightingPlan != lightingPlan ||
                frameSnapshot->lightingResources != lightingResources ||
                frameSnapshot->deformationResources != deformationResources ||
                view.renderFrameExecutionPlan != &frameSnapshot->executionPlan ||
                view.renderDrawPlan != GetDrawPlan() ||
                view.meshPassPreparation != &frameSnapshot->meshPassPreparation ||
                view.instanceBatchPlans != &frameSnapshot->instanceBatchPlans ||
                view.renderVisibility != &frameSnapshot->visibility ||
                view.renderFrameExecutionReport != &results->executionReport)
            {
                return false;
            }
            if ((localShadowAtlasPlan == nullptr) !=
                (localShadowPass == nullptr) ||
                (localShadowAtlasPlan &&
                 (lightingPlan == nullptr || frameSnapshot->lightingPlan == nullptr ||
                  !localShadowAtlasPlan->IsValid() ||
                  localShadowAtlasPlan->GetFrameIdentity() !=
                      frameSnapshot->lightingPlan->GetFrameIdentity() ||
                  localShadowAtlasPlan->GetViewIdentity() !=
                      frameSnapshot->lightingPlan->GetViewIdentity() ||
                  localShadowAtlasPlan->GetLightingPlanIdentity() !=
                      lightingPlan->GetIdentityHash())))
            {
                return false;
            }
            if (lightingPlan != nullptr &&
                !localShadowOutput.IsCompatibleWith(
                    identity,
                    lightingPlan->GetFrameIdentity(),
                    lightingPlan->GetViewIdentity(),
                    lightingPlan->GetIdentityHash()))
            {
                return false;
            }
            if (lightingPlan == nullptr &&
                (localShadowOutput.enabled || localShadowAtlasPlan != nullptr))
            {
                return false;
            }
            const RenderFrameExecutionPlan* executionPlan = GetExecutionPlan();
            if (executionPlan == nullptr ||
                executionPlan->frameSequence != identity.frameSequence ||
                executionPlan->viewOrdinal != identity.viewOrdinal)
            {
                return false;
            }
            RenderFrameExecutionReport* executionReport = GetExecutionReport();
            return executionReport != nullptr &&
                   (!deformationResources ||
                    (deformationResources->MatchesIdentity(
                         MakeRenderDeformationFrameIdentity(identity)) &&
                     deformationResources->MatchesGraphProvenance(
                         *identity.graph))) &&
                   (executionReport->frameSequence == 0 ||
                    executionReport->frameSequence == identity.frameSequence);
        }

        [[nodiscard]] bool MatchesTargetGraph(
            const RenderGraph& targetGraph) const noexcept
        {
            return identity.Matches(targetGraph) &&
                   frameSnapshot != nullptr && results != nullptr &&
                   frameSnapshot->identity == identity &&
                   results->identity == identity &&
                   HasCurrentViewDataGraphResources(view, identity);
        }
    };

    [[nodiscard]] inline std::shared_ptr<const RenderPassFrameSnapshot>
    MakeRenderPassFrameSnapshot(const RenderPassRecordContext& context,
                                RenderPassRecordResults& results)
    {
        auto snapshot = std::make_shared<RenderPassFrameSnapshot>();
        snapshot->identity = context.identity;
        snapshot->lightingPlan = context.lightingPlan;
        snapshot->lightingResources = context.lightingResources;
        snapshot->deformationResources = context.deformationResources;
        snapshot->primaryDirectionalLight = context.primaryDirectionalLight;
        snapshot->view = context.view;
        snapshot->view.directionalLightDirection =
            snapshot->primaryDirectionalLight.direction;
        snapshot->view.directionalLightColor =
            snapshot->primaryDirectionalLight.color;
        snapshot->view.directionalLightIntensity =
            snapshot->primaryDirectionalLight.intensity;
        if (context.executionPlan != nullptr)
        {
            snapshot->executionPlan = *context.executionPlan;
        }
        else
        {
            // The legacy adapter has no renderer-issued plan, but its
            // value-owned execution snapshot must still carry the same
            // recording identity as the graph that owns it.
            snapshot->executionPlan.frameSequence = context.identity.frameSequence;
            snapshot->executionPlan.viewOrdinal = context.identity.viewOrdinal;
        }
        if (context.drawPlan != nullptr)
        {
            snapshot->drawPlan = *context.drawPlan;
        }
        if (context.meshPassPreparation != nullptr)
        {
            snapshot->meshPassPreparation = *context.meshPassPreparation;
        }
        if (context.instanceBatchPlans != nullptr)
        {
            snapshot->instanceBatchPlans = *context.instanceBatchPlans;
        }
        if (context.visibility != nullptr)
        {
            snapshot->visibility = *context.visibility;
        }
        if (context.opaqueDrawItems != nullptr)
        {
            snapshot->opaqueDrawItems = *context.opaqueDrawItems;
        }
        if (context.maskedDrawItems != nullptr)
        {
            snapshot->maskedDrawItems = *context.maskedDrawItems;
        }
        if (context.transparentDrawItems != nullptr)
        {
            snapshot->transparentDrawItems = *context.transparentDrawItems;
        }
        if (context.renderScene != nullptr)
        {
            snapshot->sky = context.renderScene->GetSky();
            for (const RenderObject& object : context.renderScene->GetObjects())
            {
                snapshot->scene.AddObject(object);
            }
        }

        results.identity = context.identity;
        results.directionalShadowOutput = {};
        results.directionalShadowOutput.identity = context.identity;
        results.directionalShadowOutput.lightingPlanIdentity =
            context.lightingPlan ? context.lightingPlan->GetIdentityHash() : 0;
        results.localShadowOutput = {};
        results.localShadowOutput.identity = context.identity;
        results.localShadowOutput.frameIdentity = context.featureFrameIdentity;
        results.localShadowOutput.viewIdentity = context.featureViewIdentity;
        results.localShadowOutput.lightingPlanIdentity =
            context.lightingPlan ? context.lightingPlan->GetIdentityHash() : 0;
        results.rayTracedShadowOutput = {};
        results.rayTracedShadowOutput.identity = context.identity;
        results.rayTracedShadowStats = {};
        results.rayTracedReflectionStats = {};
        results.rayTracedReflectionDenoiseStats = {};
        results.rayTracedReflectionCompositeStats = {};
        results.lightingSurfaceEvidence = {};
        results.externalTextureAccesses.clear();
        results.executionReport = context.executionReport != nullptr
            ? *context.executionReport : RenderFrameExecutionReport{};
        if (results.executionReport.frameSequence == 0)
        {
            results.executionReport.frameSequence = context.identity.frameSequence;
        }
        snapshot->view.renderFrameExecutionPlan = &snapshot->executionPlan;
        snapshot->view.renderDrawPlan = snapshot->drawPlan.IsValid()
                                            ? &snapshot->drawPlan
                                            : nullptr;
        snapshot->view.meshPassPreparation = &snapshot->meshPassPreparation;
        snapshot->view.instanceBatchPlans = &snapshot->instanceBatchPlans;
        snapshot->view.renderVisibility = &snapshot->visibility;
        snapshot->view.renderFrameExecutionReport = &results.executionReport;
        return snapshot;
    }

    [[nodiscard]] inline RenderPassExecutionData MakeRenderPassExecutionData(
        const RenderPassRecordContext& context)
    {
        RenderPassExecutionData result;
        result.identity = context.identity;
        result.results = context.results
            ? context.results : std::make_shared<RenderPassRecordResults>();
        result.frameSnapshot = context.frameSnapshot
            ? context.frameSnapshot
            : MakeRenderPassFrameSnapshot(context, *result.results);
        result.lightingPlan = context.lightingPlan
            ? context.lightingPlan
            : (result.frameSnapshot ? result.frameSnapshot->lightingPlan : nullptr);
        result.lightingResources = context.lightingResources
            ? context.lightingResources
            : (result.frameSnapshot
                   ? result.frameSnapshot->lightingResources
                   : nullptr);
        result.deformationResources = context.deformationResources
            ? context.deformationResources
            : (result.frameSnapshot
                   ? result.frameSnapshot->deformationResources
                   : nullptr);
        result.localShadowAtlasPlan = context.localShadowAtlasPlan;
        result.localShadowPass = context.localShadowPass;
        if (context.results)
        {
            result.localShadowOutput = context.results->localShadowOutput;
        }
        else
        {
            result.localShadowOutput.identity = context.identity;
        }
        if (result.frameSnapshot)
        {
            result.view = result.frameSnapshot->view;
        }
        result.directionalShadow = context.directionalShadow;
        result.rayTracedShadow = context.rayTracedShadow;
        result.rayTracingScene = context.rayTracingScene;
        result.ambientOcclusion = context.ambientOcclusion;
        result.volumetricLighting = context.volumetricLighting;
        result.lateBoundHdrAttachment = context.lateBoundHdrAttachment;
        // The value-owned frame snapshot intentionally retains the original
        // attachment.  Only an explicit, current-graph late-bound contract is
        // permitted to replace it for recording; never infer a target from a
        // pass or mutate the frozen snapshot.
        if (result.lateBoundHdrAttachment.IsCompatibleWith(result.identity))
        {
            result.view.colorTarget =
                result.lateBoundHdrAttachment.finalHdrSceneColor;
        }
        result.opaqueSceneSnapshot = context.opaqueSceneSnapshot;
        result.opaqueMaterialTable = context.opaqueMaterialTable;
        return result;
    }

    [[nodiscard]] inline RenderPassRecordContext MakeRenderPassRecordContext(
        RenderGraph& graph,
        const ViewData& view)
    {
        RenderPassRecordContext context;
        context.view = view;
        context.identity.graph = &graph;
        context.identity.graphIdentity = graph.GetGraphIdentity();
        context.identity.graphRecordingGeneration =
            graph.GetRecordingGeneration();
        context.identity.frameSequence = view.renderFrameExecutionPlan != nullptr
            ? view.renderFrameExecutionPlan->frameSequence
            : (view.frameNumber != 0 ? view.frameNumber : 1);
        context.identity.viewOrdinal = view.renderFrameExecutionPlan != nullptr
            ? view.renderFrameExecutionPlan->viewOrdinal
            : 0;
        // The legacy adapter has no renderer-owned epoch.  It still captures a
        // value snapshot and is intentionally not used by migrated scene passes.
        context.identity.recordEpoch = 1;
        context.executionPlan = view.renderFrameExecutionPlan;
        context.drawPlan = view.renderDrawPlan;
        context.meshPassPreparation = view.meshPassPreparation;
        context.instanceBatchPlans = view.instanceBatchPlans;
        context.visibility = view.renderVisibility;
        context.executionReport = view.renderFrameExecutionReport;
        // The legacy ViewData adapter is intentionally bounded to one graph.
        // Typed scene recording supplies this value directly from the renderer.
        context.primaryDirectionalLight.selected = true;
        context.primaryDirectionalLight.castsShadow = true;
        context.primaryDirectionalLight.direction = view.directionalLightDirection;
        context.primaryDirectionalLight.color = view.directionalLightColor;
        context.primaryDirectionalLight.intensity = view.directionalLightIntensity;
        context.legacyAdapter = true;
        return context;
    }
} // namespace RVX
