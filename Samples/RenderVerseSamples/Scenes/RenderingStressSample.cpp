/** @file RenderingStressSample.cpp @brief Deterministic retained-scene stress probe. */

#include "Scenes/RenderingStressSample.h"

#include "Core/MathTypes.h"
#include "Scene/ECS/Fragments.h"
#include "Scene/ECS/RenderFragments.h"
#include "Samples/SampleCLI.h"
#include "Samples/SampleContext.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace RVX
{
    namespace
    {
        constexpr uint32 MaxStaticStreamWarmupFrames = 8u;

        [[nodiscard]] uint64 SaturatingMetricSum(uint64 lhs, uint64 rhs) noexcept
        {
            return lhs > std::numeric_limits<uint64>::max() - rhs
                       ? std::numeric_limits<uint64>::max()
                       : lhs + rhs;
        }

        [[nodiscard]] bool IsEcsModelFailure(
            const ResourceSceneAdapters::EcsSceneAssetLoadStatus& status) noexcept
        {
            using State = ResourceSceneAdapters::EcsSceneAssetLoadState;
            return status.state == State::Failed ||
                   status.state == State::FailedRetained ||
                   status.state == State::DeviceLost ||
                   status.state == State::Cancelled ||
                   status.state == State::Recycled;
        }

        [[nodiscard]] bool HasAuthorizedTextureStreaming(
            const ResourceSceneAdapters::EcsSceneAssetLoadStatus& status) noexcept
        {
            const std::optional<
                ResourceSceneAdapters::EcsModelTextureStreamingStartReceipt>& receipt =
                status.textureStreamingStartReceipt;
            return receipt.has_value() && receipt->IsAuthorized() &&
                   receipt->authorizedFrozenSourceSnapshotRevision ==
                       status.minimumResidentFrozenSourceSnapshotRevision &&
                   receipt->authorizedRenderSceneRevision ==
                       status.minimumResidentRenderSceneRevision &&
                   receipt->authorizedPresentedFrameSequence ==
                       status.minimumResidentPresentedFrameSequence &&
                   receipt->textureAssetIds ==
                       status.modelMetadata.streamingTextureAssetIds;
        }

        const SampleInfo RenderingStressInfo{
            "rendering-stress",
            "Rendering Stress",
            "Validates retained Scene extraction and GPU-scene deltas with deterministic 1K/10K/100K workloads",
            "interior-rendering-p0a",
            SampleAssetPolicy::Fixed,
            "",
            SampleEnvironmentPolicy::None,
            true,
            SampleRenderPath::Auto,
            {},
            true,
            SampleWorkloadScale::PullRequest};

        const AssessmentAction FirstFrameAction{
            AssessmentCode("RENDER.STRESS.ACTION.FIRST_FRAME_PRESENTED"),
            "A completed frame was presented before the stress model was consumed or workload entities were created."};
        const AssessmentAction SharedAssetAction{
            AssessmentCode("RENDER.STRESS.ACTION.SHARED_ASSET_READY"),
            "One asynchronous ECS model request retained the shared mesh/material identity for all procedural entities."};
        const AssessmentAction InitialBuildAction{
            AssessmentCode("RENDER.STRESS.ACTION.INITIAL_BUILD"),
            "The exact workload entity set was created through SampleSceneLifetimeScope."};
        const AssessmentAction DirtySetAction{
            AssessmentCode("RENDER.STRESS.ACTION.EXACT_DIRTY_SET"),
            "The exact deterministic dirty entity index set changed transform state."};
        const AssessmentAction ChurnDestroyAction{
            AssessmentCode("RENDER.STRESS.ACTION.EXACT_CHURN_DESTROY"),
            "The exact deterministic churn entity index set was destroyed through ECS receipts."};
        const AssessmentAction ChurnRecreateAction{
            AssessmentCode("RENDER.STRESS.ACTION.EXACT_CHURN_RECREATE"),
            "The exact deterministic churn entity index set was recreated through SampleSceneLifetimeScope."};

        const AssessmentInvariant FirstFrameInvariant{
            AssessmentCode("RENDER.STRESS.FIRST_FRAME_BEFORE_WORKLOAD"),
            "The workload cannot be created before a completed presentation and shared asset readiness."};
        const AssessmentInvariant SharedIdentityInvariant{
            AssessmentCode("RENDER.STRESS.SHARED_RESOURCE_IDENTITY"),
            "Every procedural entity retains the model-provided mesh and material identities."};
        const AssessmentInvariant StaticReuseInvariant{
            AssessmentCode("RENDER.STRESS.STATIC_REUSE_EXACT"),
            "Static windows reuse retained Scene and draw-packet state without GPU-scene uploads."};
        const AssessmentInvariant DirtyDeltaInvariant{
            AssessmentCode("RENDER.STRESS.DIRTY_DELTA_EXACT"),
            "The dirty window rebuilds and updates exactly the requested deterministic entity set."};
        const AssessmentInvariant ChurnDeltaInvariant{
            AssessmentCode("RENDER.STRESS.CHURN_DELTA_EXACT"),
            "The churn window removes and adds exactly the requested deterministic entity set."};
        const AssessmentInvariant NoFullRebuildInvariant{
            AssessmentCode("RENDER.STRESS.NO_FULL_REBUILD"),
            "Dirty and churn windows do not trigger a retained-scene full rebuild or GPU-scene full upload."};

        const AssessmentCapability RetainedSceneCapability{
            AssessmentCode("RENDER.CAPABILITY.RETAINED_SCENE_WORK"),
            "Completed-frame diagnostics expose retained Scene and draw-packet work.",
            true,
            "Rendering-stress requires exact retained Scene work diagnostics."};
        const AssessmentCapability GPUSceneCapability{
            AssessmentCode("RENDER.CAPABILITY.GPU_SCENE_DELTA"),
            "Completed-frame diagnostics expose GPU-scene add/update/remove/upload work.",
            true,
            "Rendering-stress requires exact GPU-scene delta diagnostics."};
        const AssessmentCapability ExtractionCapability{
            AssessmentCode("RENDER.CAPABILITY.INCREMENTAL_EXTRACTION"),
            "Engine exposes producer/transport-accepted cumulative incremental extraction work.",
            true,
            "Rendering-stress requires producer/transport-accepted incremental extraction diagnostics."};

        const AssessmentMetric ExtractionFullScanMetric{
            AssessmentCode("RENDER.STRESS.METRIC.EXTRACTION_FULL_SCAN_COUNT"),
            "actors", "Authoritative complete Scene scans in the latest render extraction candidate."};
        const AssessmentMetric ExtractionChangeFeedMetric{
            AssessmentCode("RENDER.STRESS.METRIC.EXTRACTION_CHANGE_FEED_COUNT"),
            "changes", "Change-feed records consumed by the latest render extraction candidate."};
        const AssessmentMetric ExtractionActorRebuildMetric{
            AssessmentCode("RENDER.STRESS.METRIC.EXTRACTION_ACTOR_REBUILD_COUNT"),
            "actors", "Actors rebuilt by the latest incremental extraction candidate."};
        const AssessmentMetric ExtractionProxyVisitMetric{
            AssessmentCode("RENDER.STRESS.METRIC.EXTRACTION_PROXY_VISIT_COUNT"),
            "components", "Proxy components visited by the latest extraction candidate."};
        const AssessmentMetric ExtractionComponentVisitMetric{
            AssessmentCode("RENDER.STRESS.METRIC.EXTRACTION_COMPONENT_VISIT_COUNT"),
            "components", "Non-proxy components visited by the latest extraction candidate."};
        const AssessmentMetric ExtractionProviderVisitMetric{
            AssessmentCode("RENDER.STRESS.METRIC.EXTRACTION_PROVIDER_VISIT_COUNT"),
            "providers", "Feature providers visited by the latest extraction candidate."};
        const AssessmentMetric AcceptedExtractionPublicationMetric{
            AssessmentCode("RENDER.STRESS.METRIC.ACCEPTED_EXTRACTION_PUBLICATION_COUNT"),
            "count", "Producer/transport extraction publications accepted by Engine."};
        const AssessmentMetric AcceptedExtractionLastSourceFrameMetric{
            AssessmentCode("RENDER.STRESS.METRIC.ACCEPTED_EXTRACTION_LAST_SOURCE_FRAME_SEQUENCE"),
            "frame", "Last source frame sequence observed by Engine producer/transport acceptance."};
        const AssessmentMetric AcceptedExtractionLastSceneRevisionMetric{
            AssessmentCode("RENDER.STRESS.METRIC.ACCEPTED_EXTRACTION_LAST_SCENE_REVISION"),
            "revision", "Last Scene revision observed by Engine producer/transport acceptance."};
        const AssessmentMetric AcceptedExtractionFullScanMetric{
            AssessmentCode("RENDER.STRESS.METRIC.ACCEPTED_EXTRACTION_CUMULATIVE_FULL_SCAN_COUNT"),
            "actors", "Cumulative full scans from accepted producer/transport extraction publications."};
        const AssessmentMetric AcceptedExtractionChangeFeedMetric{
            AssessmentCode("RENDER.STRESS.METRIC.ACCEPTED_EXTRACTION_CUMULATIVE_CHANGE_FEED_COUNT"),
            "changes", "Cumulative change-feed records from accepted producer/transport extraction publications."};
        const AssessmentMetric AcceptedExtractionActorRebuildMetric{
            AssessmentCode("RENDER.STRESS.METRIC.ACCEPTED_EXTRACTION_CUMULATIVE_ACTOR_REBUILD_COUNT"),
            "actors", "Cumulative actor rebuilds from accepted producer/transport extraction publications."};
        const AssessmentMetric AcceptedExtractionProxyVisitMetric{
            AssessmentCode("RENDER.STRESS.METRIC.ACCEPTED_EXTRACTION_CUMULATIVE_PROXY_VISIT_COUNT"),
            "components", "Cumulative proxy visits from accepted producer/transport extraction publications."};
        const AssessmentMetric AcceptedExtractionComponentVisitMetric{
            AssessmentCode("RENDER.STRESS.METRIC.ACCEPTED_EXTRACTION_CUMULATIVE_COMPONENT_VISIT_COUNT"),
            "components", "Cumulative component visits from accepted producer/transport extraction publications."};
        const AssessmentMetric AcceptedExtractionProviderVisitMetric{
            AssessmentCode("RENDER.STRESS.METRIC.ACCEPTED_EXTRACTION_CUMULATIVE_PROVIDER_VISIT_COUNT"),
            "providers", "Cumulative feature-provider visits from accepted producer/transport extraction publications."};
        const AssessmentMetric AcceptedExtractionContinuityLossMetric{
            AssessmentCode("RENDER.STRESS.METRIC.ACCEPTED_EXTRACTION_CONTINUITY_LOSS_COUNT"),
            "count", "Cumulative continuity losses from accepted producer/transport extraction publications."};

        const AssessmentMetric MutationActionTargetSceneRevisionMetric{
            AssessmentCode("RENDER.STRESS.METRIC.MUTATION_ACTION_TARGET_SCENE_REVISION"),
            "revision", "Exact Scene revision requested by the qualified mutation action."};
        const AssessmentMetric MutationCompletedFrameSequenceMetric{
            AssessmentCode("RENDER.STRESS.METRIC.MUTATION_COMPLETED_FRAME_SEQUENCE"),
            "frame", "Completion-qualified frame sequence that covered the mutation target revision."};
        const AssessmentMetric MutationAppliedSceneRevisionMetric{
            AssessmentCode("RENDER.STRESS.METRIC.MUTATION_APPLIED_SCENE_REVISION"),
            "revision", "Completed retained Scene revision that covered the mutation target revision."};
        const AssessmentMetric MutationSceneIncrementalDeltaMetric{
            AssessmentCode("RENDER.STRESS.METRIC.MUTATION_SCENE_INCREMENTAL_DELTA"),
            "count", "Cumulative retained Scene incremental commits within the qualified action window."};
        const AssessmentMetric MutationSceneRebuiltDeltaMetric{
            AssessmentCode("RENDER.STRESS.METRIC.MUTATION_SCENE_REBUILT_OBJECT_DELTA"),
            "objects", "Cumulative retained Scene rebuilt objects within the qualified action window."};
        const AssessmentMetric MutationSceneRemovedDeltaMetric{
            AssessmentCode("RENDER.STRESS.METRIC.MUTATION_SCENE_REMOVED_OBJECT_DELTA"),
            "objects", "Cumulative retained Scene removed objects within the qualified action window."};
        const AssessmentMetric MutationGPUSceneAddDeltaMetric{
            AssessmentCode("RENDER.STRESS.METRIC.MUTATION_GPU_SCENE_ADD_DELTA"),
            "objects", "Cumulative GPU-scene additions within the qualified action window."};
        const AssessmentMetric MutationGPUSceneUpdateDeltaMetric{
            AssessmentCode("RENDER.STRESS.METRIC.MUTATION_GPU_SCENE_UPDATE_DELTA"),
            "objects", "Cumulative GPU-scene updates within the qualified action window."};
        const AssessmentMetric MutationGPUSceneRemoveDeltaMetric{
            AssessmentCode("RENDER.STRESS.METRIC.MUTATION_GPU_SCENE_REMOVE_DELTA"),
            "objects", "Cumulative GPU-scene removals within the qualified action window."};
        const AssessmentMetric MutationGPUCullingPatchedRowDeltaMetric{
            AssessmentCode("RENDER.STRESS.METRIC.MUTATION_GPU_CULLING_PATCHED_ROW_DELTA"),
            "rows", "Cumulative canonical GPU-culling instance, candidate, and active-row patches within the qualified action window."};
        const AssessmentMetric MutationDirectRasterPatchedRowDeltaMetric{
            AssessmentCode("RENDER.STRESS.METRIC.MUTATION_DIRECT_RASTER_PATCHED_ROW_DELTA"),
            "rows", "Cumulative Direct raster instance and index row patches within the qualified action window."};

        const AssessmentMetric SceneFullRebuildMetric{
            AssessmentCode("RENDER.STRESS.METRIC.SCENE_FULL_REBUILD_COUNT"),
            "count", "Cumulative retained Scene full rebuild count."};
        const AssessmentMetric SceneIncrementalMetric{
            AssessmentCode("RENDER.STRESS.METRIC.SCENE_INCREMENTAL_UPDATE_COUNT"),
            "count", "Cumulative retained Scene incremental update count."};
        const AssessmentMetric SceneStaticReuseMetric{
            AssessmentCode("RENDER.STRESS.METRIC.SCENE_STATIC_REUSE_COUNT"),
            "count", "Cumulative retained Scene static reuse count."};
        const AssessmentMetric SceneLastRebuiltMetric{
            AssessmentCode("RENDER.STRESS.METRIC.SCENE_LAST_REBUILT_OBJECT_COUNT"),
            "objects", "Objects rebuilt in the completed frame."};
        const AssessmentMetric SceneLastRemovedMetric{
            AssessmentCode("RENDER.STRESS.METRIC.SCENE_LAST_REMOVED_OBJECT_COUNT"),
            "objects", "Objects removed in the completed frame."};
        const AssessmentMetric DrawPacketBuildMetric{
            AssessmentCode("RENDER.STRESS.METRIC.DRAW_PACKET_BUILD_COUNT"),
            "count", "Cumulative draw-packet build count."};
        const AssessmentMetric DrawPacketInvalidationMetric{
            AssessmentCode("RENDER.STRESS.METRIC.DRAW_PACKET_INVALIDATION_COUNT"),
            "count", "Cumulative draw-packet invalidation count."};
        const AssessmentMetric GPUSceneAddMetric{
            AssessmentCode("RENDER.STRESS.METRIC.GPU_SCENE_ADD_COUNT"),
            "objects", "GPU-scene additions in the completed frame."};
        const AssessmentMetric GPUSceneUpdateMetric{
            AssessmentCode("RENDER.STRESS.METRIC.GPU_SCENE_UPDATE_COUNT"),
            "objects", "GPU-scene updates in the completed frame."};
        const AssessmentMetric GPUSceneRemoveMetric{
            AssessmentCode("RENDER.STRESS.METRIC.GPU_SCENE_REMOVE_COUNT"),
            "objects", "GPU-scene removals in the completed frame."};
        const AssessmentMetric GPUSceneNoOpMetric{
            AssessmentCode("RENDER.STRESS.METRIC.GPU_SCENE_NOOP_COUNT"),
            "objects", "GPU-scene no-op rows in the completed frame."};
        const AssessmentMetric GPUSceneUploadMetric{
            AssessmentCode("RENDER.STRESS.METRIC.GPU_SCENE_FRAME_UPLOAD_BYTES"),
            "bytes", "Persistent GPU-scene bytes uploaded in the completed frame."};
        const AssessmentMetric GPUSceneFullUploadMetric{
            AssessmentCode("RENDER.STRESS.METRIC.GPU_SCENE_FULL_UPLOAD"),
            "bool", "Whether the completed frame used a full GPU-scene upload."};
        const AssessmentMetric GPUDrivenInstanceUploadMetric{
            AssessmentCode("RENDER.STRESS.METRIC.GPU_DRIVEN_INSTANCE_UPLOAD_BYTES"),
            "bytes", "GPU-driven instance payload bytes observed in the completed frame."};
        const AssessmentMetric GPUDrivenCandidateUploadMetric{
            AssessmentCode("RENDER.STRESS.METRIC.GPU_DRIVEN_CANDIDATE_UPLOAD_BYTES"),
            "bytes", "GPU-driven candidate payload bytes observed in the completed frame."};
        const AssessmentMetric GPUDrivenActiveRowUploadMetric{
            AssessmentCode("RENDER.STRESS.METRIC.GPU_DRIVEN_ACTIVE_ROW_UPLOAD_BYTES"),
            "bytes", "GPU-driven active-row indirection bytes observed in the completed frame."};
        const AssessmentMetric GPUDrivenActiveRowCountMetric{
            AssessmentCode("RENDER.STRESS.METRIC.GPU_DRIVEN_ACTIVE_ROW_COUNT"),
            "rows", "Active GPU-driven logical rows observed in the completed frame."};
        const AssessmentMetric GPUDrivenActiveRowHighWatermarkMetric{
            AssessmentCode("RENDER.STRESS.METRIC.GPU_DRIVEN_ACTIVE_ROW_HIGH_WATERMARK"),
            "rows", "Highest sparse GPU-driven active-row index observed in the completed frame."};
        const AssessmentMetric GPUDrivenInstancePatchedRowMetric{
            AssessmentCode("RENDER.STRESS.METRIC.GPU_DRIVEN_INSTANCE_PATCHED_ROW_COUNT"),
            "rows", "GPU-driven instance rows patched in the completed frame."};
        const AssessmentMetric GPUDrivenCandidatePatchedRowMetric{
            AssessmentCode("RENDER.STRESS.METRIC.GPU_DRIVEN_CANDIDATE_PATCHED_ROW_COUNT"),
            "rows", "GPU-driven candidate rows patched in the completed frame."};
        const AssessmentMetric GPUDrivenActiveRowPatchedRowMetric{
            AssessmentCode("RENDER.STRESS.METRIC.GPU_DRIVEN_ACTIVE_ROW_PATCHED_ROW_COUNT"),
            "rows", "GPU-driven active-row indirection rows patched in the completed frame."};
        const AssessmentMetric GPUDrivenInstanceFullMaterializationMetric{
            AssessmentCode("RENDER.STRESS.METRIC.GPU_DRIVEN_INSTANCE_FULL_MATERIALIZATION_COUNT"),
            "count", "Cumulative GPU-driven instance full materializations."};
        const AssessmentMetric GPUDrivenCandidateFullMaterializationMetric{
            AssessmentCode("RENDER.STRESS.METRIC.GPU_DRIVEN_CANDIDATE_FULL_MATERIALIZATION_COUNT"),
            "count", "Cumulative GPU-driven candidate full materializations."};
        const AssessmentMetric GPUDrivenActiveRowFullMaterializationMetric{
            AssessmentCode("RENDER.STRESS.METRIC.GPU_DRIVEN_ACTIVE_ROW_FULL_MATERIALIZATION_COUNT"),
            "count", "Cumulative GPU-driven active-row full materializations."};
        const AssessmentMetric GPUDrivenContinuityFullMaterializationMetric{
            AssessmentCode("RENDER.STRESS.METRIC.GPU_DRIVEN_CONTINUITY_FULL_MATERIALIZATION_COUNT"),
            "count", "Cumulative GPU-driven continuity-recovery full materializations."};
        const AssessmentMetric GPUDrivenCapacityFullMaterializationMetric{
            AssessmentCode("RENDER.STRESS.METRIC.GPU_DRIVEN_CAPACITY_FULL_MATERIALIZATION_COUNT"),
            "count", "Cumulative GPU-driven capacity-recovery full materializations."};
        const AssessmentMetric DirectRasterInstanceUploadMetric{
            AssessmentCode("RENDER.STRESS.METRIC.DIRECT_RASTER_INSTANCE_UPLOAD_BYTES"),
            "bytes", "Direct raster instance-stream bytes observed in the completed frame."};
        const AssessmentMetric DirectRasterIndexUploadMetric{
            AssessmentCode("RENDER.STRESS.METRIC.DIRECT_RASTER_INSTANCE_INDEX_UPLOAD_BYTES"),
            "bytes", "Direct raster identity-index bytes observed in the completed frame."};
        const AssessmentMetric DirectRasterInstancePatchedRowMetric{
            AssessmentCode("RENDER.STRESS.METRIC.DIRECT_RASTER_INSTANCE_PATCHED_ROW_COUNT"),
            "rows", "Direct raster instance rows patched in the completed frame."};
        const AssessmentMetric DirectRasterIndexPatchedRowMetric{
            AssessmentCode("RENDER.STRESS.METRIC.DIRECT_RASTER_INDEX_PATCHED_ROW_COUNT"),
            "rows", "Direct raster identity-index rows patched in the completed frame."};
        const AssessmentMetric DirectRasterActiveInstanceCountMetric{
            AssessmentCode("RENDER.STRESS.METRIC.DIRECT_RASTER_ACTIVE_INSTANCE_COUNT"),
            "instances", "Active direct-raster persistent instances in the completed frame."};
        const AssessmentMetric DirectRasterActiveInstanceCapacityMetric{
            AssessmentCode("RENDER.STRESS.METRIC.DIRECT_RASTER_ACTIVE_INSTANCE_CAPACITY"),
            "instances", "Capacity of the direct-raster persistent instance stream."};
        const AssessmentMetric DirectRasterInstanceFullMaterializationMetric{
            AssessmentCode("RENDER.STRESS.METRIC.DIRECT_RASTER_INSTANCE_FULL_MATERIALIZATION_COUNT"),
            "count", "Direct raster instance full materializations in the completed frame."};
        const AssessmentMetric DirectRasterIndexFullMaterializationMetric{
            AssessmentCode("RENDER.STRESS.METRIC.DIRECT_RASTER_INDEX_FULL_MATERIALIZATION_COUNT"),
            "count", "Direct raster identity-index full materializations in the completed frame."};
        const AssessmentMetric ChurnDestroyedTotalMetric{
            AssessmentCode("RENDER.STRESS.METRIC.CHURN_DESTROYED_TOTAL"),
            "objects", "Total deterministic churn actors removed after every bounded batch was verified."};
        const AssessmentMetric ChurnRecreatedTotalMetric{
            AssessmentCode("RENDER.STRESS.METRIC.CHURN_RECREATED_TOTAL"),
            "objects", "Total deterministic churn actors recreated after every bounded batch was verified."};

        [[nodiscard]] bool IsExactProfile(const SampleWorkloadProfile& profile)
        {
            const SampleWorkloadProfile expected =
                BuildSampleWorkloadProfile(profile.scale);
            return profile.objectCount == expected.objectCount &&
                   profile.dirtyObjectCount == expected.dirtyObjectCount &&
                   profile.churnObjectCount == expected.churnObjectCount &&
                   profile.gpuCullingCapacity == expected.gpuCullingCapacity;
        }

        [[nodiscard]] bool IsProfileUsable(const SampleWorkloadProfile& profile)
        {
            return profile.objectCount != 0 &&
                   profile.dirtyObjectCount != 0 &&
                   profile.dirtyObjectCount <= profile.objectCount &&
                   profile.churnObjectCount != 0 &&
                   profile.churnObjectCount <= profile.objectCount &&
                   profile.gpuCullingCapacity >= profile.objectCount;
        }

        void NormalizeExecutionStreamWarmupDiagnostics(
            RenderingStressDiagnosticsSnapshot& snapshot,
            const RenderingStressDiagnosticsSnapshot& baseline) noexcept
        {
            snapshot.gpuDrivenInstanceUploadBytes = 0;
            snapshot.gpuDrivenCandidateUploadBytes = 0;
            snapshot.gpuDrivenActiveRowUploadBytes = 0;
            snapshot.gpuDrivenInstancePatchedRowCount = 0;
            snapshot.gpuDrivenCandidatePatchedRowCount = 0;
            snapshot.gpuDrivenActiveRowPatchedRowCount = 0;
            // GPU-driven full-materialization diagnostics are lifetime-
            // cumulative. Initial completion-safe slot warming may advance
            // them, so normalize only the structural comparison to the
            // rolling baseline. Later windows compare the real counters.
            snapshot.gpuDrivenInstanceFullMaterializationCount =
                baseline.gpuDrivenInstanceFullMaterializationCount;
            snapshot.gpuDrivenCandidateFullMaterializationCount =
                baseline.gpuDrivenCandidateFullMaterializationCount;
            snapshot.gpuDrivenActiveRowFullMaterializationCount =
                baseline.gpuDrivenActiveRowFullMaterializationCount;
            snapshot.gpuDrivenContinuityFullMaterializationCount =
                baseline.gpuDrivenContinuityFullMaterializationCount;
            snapshot.gpuDrivenCapacityFullMaterializationCount =
                baseline.gpuDrivenCapacityFullMaterializationCount;
            snapshot.directRasterInstanceUploadBytes = 0;
            snapshot.directRasterInstanceIndexUploadBytes = 0;
            snapshot.directRasterInstancePatchedRowCount = 0;
            snapshot.directRasterIndexPatchedRowCount = 0;
            snapshot.directRasterInstanceFullMaterializationCount = 0;
            snapshot.directRasterIndexFullMaterializationCount = 0;
        }

        [[nodiscard]] uint32 GetWorkloadExtent(uint32 objectCount)
        {
            uint32 extent = 1;
            while (static_cast<uint64>(extent) * extent < objectCount)
                ++extent;
            return extent;
        }

        [[nodiscard]] float32 GetWorkloadSpan(uint32 objectCount)
        {
            constexpr float32 Spacing = 1.75f;
            return static_cast<float32>(GetWorkloadExtent(objectCount)) *
                   Spacing;
        }

        [[nodiscard]] float32 GetWorkloadFarDistance(uint32 objectCount)
        {
            return std::max(5000.0f, GetWorkloadSpan(objectCount) * 6.0f);
        }

        [[nodiscard]] Vec3 GetEntityPosition(uint32 index, uint32 objectCount)
        {
            constexpr float32 Spacing = 1.75f;
            const uint32 extent = GetWorkloadExtent(objectCount);
            const uint32 row = index / extent;
            const uint32 column = index % extent;
            const float32 center = static_cast<float32>(extent - 1u) * 0.5f;
            return Vec3((static_cast<float32>(column) - center) * Spacing,
                        0.0f,
                        (center - static_cast<float32>(row)) * Spacing);
        }

        [[nodiscard]] RenderingStressWindowEvaluation PassWindow()
        {
            return {true, false, {}};
        }

        [[nodiscard]] RenderingStressWindowEvaluation PendingWindow(
            std::string detail)
        {
            return {false, true, std::move(detail)};
        }

        [[nodiscard]] RenderingStressWindowEvaluation FailWindow(
            std::string detail)
        {
            return {false, false, std::move(detail)};
        }

        [[nodiscard]] bool IsCounterUnchanged(uint64 before, uint64 after)
        {
            return before == after;
        }

        [[nodiscard]] bool HasAcceptedExtractionCounterRegression(
            const RenderingStressDiagnosticsSnapshot& before,
            const RenderingStressDiagnosticsSnapshot& after) noexcept
        {
            return after.acceptedExtractionPublicationCount <
                       before.acceptedExtractionPublicationCount ||
                   after.acceptedExtractionLastSourceFrameSequence <
                       before.acceptedExtractionLastSourceFrameSequence ||
                   after.acceptedExtractionLastSceneRevision <
                       before.acceptedExtractionLastSceneRevision ||
                   after.acceptedExtractionCumulativeFullScanCount <
                       before.acceptedExtractionCumulativeFullScanCount ||
                   after.acceptedExtractionCumulativeChangeFeedChangeCount <
                       before.acceptedExtractionCumulativeChangeFeedChangeCount ||
                   after.acceptedExtractionCumulativeActorRebuildCount <
                       before.acceptedExtractionCumulativeActorRebuildCount ||
                   after.acceptedExtractionCumulativeProxyVisitCount <
                       before.acceptedExtractionCumulativeProxyVisitCount ||
                   after.acceptedExtractionCumulativeComponentVisitCount <
                       before.acceptedExtractionCumulativeComponentVisitCount ||
                   after.acceptedExtractionCumulativeFeatureProviderVisitCount <
                       before.acceptedExtractionCumulativeFeatureProviderVisitCount ||
                   after.acceptedExtractionContinuityLossCount <
                       before.acceptedExtractionContinuityLossCount;
        }

        [[nodiscard]] bool HasAcceptedExtractionWorkGrowth(
            const RenderingStressDiagnosticsSnapshot& before,
            const RenderingStressDiagnosticsSnapshot& after) noexcept
        {
            return after.acceptedExtractionCumulativeFullScanCount !=
                       before.acceptedExtractionCumulativeFullScanCount ||
                   after.acceptedExtractionCumulativeChangeFeedChangeCount !=
                       before.acceptedExtractionCumulativeChangeFeedChangeCount ||
                   after.acceptedExtractionCumulativeActorRebuildCount !=
                       before.acceptedExtractionCumulativeActorRebuildCount ||
                   after.acceptedExtractionCumulativeProxyVisitCount !=
                       before.acceptedExtractionCumulativeProxyVisitCount ||
                   after.acceptedExtractionCumulativeComponentVisitCount !=
                       before.acceptedExtractionCumulativeComponentVisitCount ||
                   after.acceptedExtractionCumulativeFeatureProviderVisitCount !=
                       before.acceptedExtractionCumulativeFeatureProviderVisitCount ||
                   after.acceptedExtractionContinuityLossCount !=
                       before.acceptedExtractionContinuityLossCount;
        }

        [[nodiscard]] RenderingStressWindowEvaluation
            EvaluateAcceptedExtractionStaticWindow(
                const RenderingStressDiagnosticsSnapshot& before,
                const RenderingStressDiagnosticsSnapshot& after)
        {
            if (!before.acceptedExtractionDiagnosticsAvailable ||
                !after.acceptedExtractionDiagnosticsAvailable)
            {
                return FailWindow(
                    "Accepted producer/transport extraction diagnostics are unavailable.");
            }
            if (HasAcceptedExtractionCounterRegression(before, after))
            {
                return FailWindow(
                    "Accepted producer/transport extraction diagnostics regressed.");
            }
            if (HasAcceptedExtractionWorkGrowth(before, after))
            {
                return FailWindow(
                    "Static window advanced accepted producer/transport extraction work totals.");
            }
            return PassWindow();
        }

        [[nodiscard]] RenderingStressWindowEvaluation
            EvaluateAcceptedExtractionMutationWindow(
                const RenderingStressDiagnosticsSnapshot& before,
                const RenderingStressDiagnosticsSnapshot& after,
                uint64 expectedChangeFeedCount,
                uint32 expectedActorRebuildCount,
                uint32 expectedProxyVisitCount)
        {
            if (!before.acceptedExtractionDiagnosticsAvailable ||
                !after.acceptedExtractionDiagnosticsAvailable)
            {
                return FailWindow(
                    "Accepted producer/transport extraction diagnostics are unavailable.");
            }
            if (HasAcceptedExtractionCounterRegression(before, after))
            {
                return FailWindow(
                    "Accepted producer/transport extraction diagnostics regressed.");
            }

            const uint64 fullScanDelta =
                after.acceptedExtractionCumulativeFullScanCount -
                before.acceptedExtractionCumulativeFullScanCount;
            const uint64 changeFeedDelta =
                after.acceptedExtractionCumulativeChangeFeedChangeCount -
                before.acceptedExtractionCumulativeChangeFeedChangeCount;
            const uint64 actorRebuildDelta =
                after.acceptedExtractionCumulativeActorRebuildCount -
                before.acceptedExtractionCumulativeActorRebuildCount;
            const uint64 proxyVisitDelta =
                after.acceptedExtractionCumulativeProxyVisitCount -
                before.acceptedExtractionCumulativeProxyVisitCount;
            const uint64 componentVisitDelta =
                after.acceptedExtractionCumulativeComponentVisitCount -
                before.acceptedExtractionCumulativeComponentVisitCount;
            const uint64 providerVisitDelta =
                after.acceptedExtractionCumulativeFeatureProviderVisitCount -
                before.acceptedExtractionCumulativeFeatureProviderVisitCount;
            const uint64 continuityLossDelta =
                after.acceptedExtractionContinuityLossCount -
                before.acceptedExtractionContinuityLossCount;

            if (fullScanDelta != 0 || continuityLossDelta != 0 ||
                componentVisitDelta != 0 || providerVisitDelta != 0)
            {
                return FailWindow(
                    "Accepted producer/transport extraction reported a full scan, continuity loss, or unexpected component/provider work.");
            }
            if (changeFeedDelta > expectedChangeFeedCount ||
                actorRebuildDelta > expectedActorRebuildCount ||
                proxyVisitDelta > expectedProxyVisitCount)
            {
                return FailWindow(
                    "Accepted producer/transport extraction exceeded its exact change-feed, actor, or proxy work budget.");
            }
            if (changeFeedDelta < expectedChangeFeedCount ||
                actorRebuildDelta < expectedActorRebuildCount ||
                proxyVisitDelta < expectedProxyVisitCount)
            {
                return PendingWindow(
                    "Waiting for accepted producer/transport extraction actor, proxy, or change-feed evidence.");
            }
            return PassWindow();
        }

        enum class MutationActionKind : uint8
        {
            Dirty = 0,
            Remove,
            Add
        };

        [[nodiscard]] constexpr uint64 GetExpectedChangeFeedMultiplier(
            MutationActionKind action) noexcept
        {
            switch (action)
            {
                case MutationActionKind::Dirty: return 4u;
                case MutationActionKind::Remove: return 2u;
                case MutationActionKind::Add: return 8u;
                default: return 0u;
            }
        }

        [[nodiscard]] bool TryMultiplyMutationCount(uint64 value,
                                                     uint64 multiplier,
                                                     uint64& outResult) noexcept
        {
            if (multiplier != 0 &&
                value > std::numeric_limits<uint64>::max() / multiplier)
            {
                return false;
            }
            outResult = value * multiplier;
            return true;
        }

        [[nodiscard]] bool TryAddMutationCount(uint64 lhs,
                                                uint64 rhs,
                                                uint64& outResult) noexcept
        {
            if (lhs > std::numeric_limits<uint64>::max() - rhs)
                return false;
            outResult = lhs + rhs;
            return true;
        }

        [[nodiscard]] bool IsWithinPresentationPropagationBudget(
            const SampleRenderMutationEvidenceDiagnostics& action,
            const SampleRenderMutationEvidenceDiagnostics& current,
            uint32 nonStrictFrameLimit,
            bool terminalStrict) noexcept
        {
            if (action.completedPresentationCount == 0 ||
                current.completedPresentationCount <=
                    action.completedPresentationCount)
            {
                return false;
            }
            const uint64 maximumPresentationDelta =
                static_cast<uint64>(nonStrictFrameLimit) +
                (terminalStrict ? 1u : 0u);
            return current.completedPresentationCount -
                       action.completedPresentationCount <=
                   maximumPresentationDelta;
        }

        [[nodiscard]] bool HasMutationEvidenceCounterRegression(
            const SampleRenderMutationEvidenceDiagnostics& before,
            const SampleRenderMutationEvidenceDiagnostics& after) noexcept
        {
            for (uint32 table = 0;
                 table < GPU_SCENE_DIAGNOSTICS_TABLE_COUNT;
                 ++table)
            {
                if (after.gpuSceneSubmittedUploadedRowCount[table] <
                    before.gpuSceneSubmittedUploadedRowCount[table])
                {
                    return true;
                }
            }
            return after.completedPresentationCount <
                       before.completedPresentationCount ||
                   after.completedFrameSequence < before.completedFrameSequence ||
                   after.requiredSceneRevision < before.requiredSceneRevision ||
                   after.appliedSceneRevision < before.appliedSceneRevision ||
                   after.sceneFullRebuildCount < before.sceneFullRebuildCount ||
                   after.sceneIncrementalCommitCount <
                       before.sceneIncrementalCommitCount ||
                   after.sceneRebuiltObjectCount <
                       before.sceneRebuiltObjectCount ||
                   after.sceneRemovedObjectCount <
                       before.sceneRemovedObjectCount ||
                   after.gpuSceneFullPublicationCount <
                       before.gpuSceneFullPublicationCount ||
                   after.gpuSceneIncrementalPublicationCount <
                       before.gpuSceneIncrementalPublicationCount ||
                   after.gpuSceneIdentityOnlyPublicationCount <
                       before.gpuSceneIdentityOnlyPublicationCount ||
                   after.gpuSceneMaterializedObjectCount <
                       before.gpuSceneMaterializedObjectCount ||
                   after.gpuSceneAddCount < before.gpuSceneAddCount ||
                   after.gpuSceneUpdateCount < before.gpuSceneUpdateCount ||
                   after.gpuSceneRemoveCount < before.gpuSceneRemoveCount ||
                   after.gpuSceneNoOpCount < before.gpuSceneNoOpCount ||
                   after.gpuSceneSubmittedUploadCount <
                       before.gpuSceneSubmittedUploadCount ||
                   after.gpuSceneUploadBytes < before.gpuSceneUploadBytes ||
                   after.gpuSceneUploadRangeCount <
                       before.gpuSceneUploadRangeCount ||
                   after.gpuSceneFullUploadCount <
                       before.gpuSceneFullUploadCount ||
                   after.gpuCullingInstancePatchedRowCount <
                       before.gpuCullingInstancePatchedRowCount ||
                   after.gpuCullingCandidatePatchedRowCount <
                       before.gpuCullingCandidatePatchedRowCount ||
                   after.gpuCullingActiveRowPatchedRowCount <
                       before.gpuCullingActiveRowPatchedRowCount ||
                   after.gpuCullingInstanceUploadedRowCount <
                       before.gpuCullingInstanceUploadedRowCount ||
                   after.gpuCullingCandidateUploadedRowCount <
                       before.gpuCullingCandidateUploadedRowCount ||
                   after.gpuCullingActiveRowUploadedRowCount <
                       before.gpuCullingActiveRowUploadedRowCount ||
                   after.gpuCullingInstanceUploadBytes <
                       before.gpuCullingInstanceUploadBytes ||
                   after.gpuCullingCandidateUploadBytes <
                       before.gpuCullingCandidateUploadBytes ||
                   after.gpuCullingActiveRowUploadBytes <
                       before.gpuCullingActiveRowUploadBytes ||
                   after.gpuCullingInstanceFullMaterializationCount <
                       before.gpuCullingInstanceFullMaterializationCount ||
                   after.gpuCullingCandidateFullMaterializationCount <
                       before.gpuCullingCandidateFullMaterializationCount ||
                   after.gpuCullingActiveRowFullMaterializationCount <
                       before.gpuCullingActiveRowFullMaterializationCount ||
                   after.gpuCullingContinuityFullMaterializationCount <
                       before.gpuCullingContinuityFullMaterializationCount ||
                   after.gpuCullingCapacityFullMaterializationCount <
                       before.gpuCullingCapacityFullMaterializationCount ||
                   after.directRasterInstancePatchedRowCount <
                       before.directRasterInstancePatchedRowCount ||
                   after.directRasterIndexPatchedRowCount <
                       before.directRasterIndexPatchedRowCount ||
                   after.directRasterInstanceUploadBytes <
                       before.directRasterInstanceUploadBytes ||
                   after.directRasterIndexUploadBytes <
                       before.directRasterIndexUploadBytes ||
                   after.directRasterInstanceFullMaterializationCount <
                       before.directRasterInstanceFullMaterializationCount ||
                   after.directRasterIndexFullMaterializationCount <
                       before.directRasterIndexFullMaterializationCount;
        }

        [[nodiscard]] RenderingStressWindowEvaluation
            ValidateMutationEvidenceBoundary(
                const RenderingStressDiagnosticsSnapshot& before,
                const RenderingStressDiagnosticsSnapshot& after,
                uint64 actionTargetSceneRevision)
        {
            const SampleRenderMutationEvidenceDiagnostics& baseline =
                before.mutationEvidence;
            const SampleRenderMutationEvidenceDiagnostics& current =
                after.mutationEvidence;
            if (!baseline.available || !current.available ||
                baseline.saturated || current.saturated ||
                baseline.evidenceEpoch == 0 || current.evidenceEpoch == 0)
            {
                return FailWindow(
                    "Completion-qualified mutation evidence is unavailable or saturated.");
            }
            if (baseline.evidenceEpoch != current.evidenceEpoch)
            {
                return FailWindow(
                    "Completion-qualified mutation evidence crossed an epoch boundary.");
            }
            if (actionTargetSceneRevision == 0 ||
                baseline.completedPresentationCount == 0 ||
                current.completedPresentationCount == 0 ||
                baseline.completedFrameSequence == 0 ||
                current.completedFrameSequence == 0 ||
                baseline.completedFrameSequence != before.sourceFrameSequence ||
                current.completedFrameSequence != after.sourceFrameSequence ||
                baseline.requiredSceneRevision !=
                    before.renderSceneRequiredRevision ||
                current.requiredSceneRevision !=
                    after.renderSceneRequiredRevision ||
                baseline.appliedSceneRevision != before.renderSceneAppliedRevision ||
                current.appliedSceneRevision != after.renderSceneAppliedRevision ||
                baseline.requiredSceneRevision > baseline.appliedSceneRevision ||
                current.requiredSceneRevision > current.appliedSceneRevision)
            {
                return FailWindow(
                    "Completion-qualified mutation evidence has invalid completed-frame provenance.");
            }
            if (HasMutationEvidenceCounterRegression(baseline, current))
            {
                return FailWindow(
                    "Completion-qualified mutation evidence counters regressed.");
            }
            if (baseline.appliedSceneRevision >= actionTargetSceneRevision)
            {
                return FailWindow(
                    "The mutation action baseline already covered its target Scene revision.");
            }
            if (current.appliedSceneRevision < actionTargetSceneRevision ||
                after.acceptedExtractionLastSceneRevision <
                    actionTargetSceneRevision)
            {
                return PendingWindow(
                    "Waiting for completion and accepted extraction to cover the target Scene revision.");
            }
            if (current.completedPresentationCount <=
                    baseline.completedPresentationCount ||
                current.completedFrameSequence <= baseline.completedFrameSequence)
            {
                return FailWindow(
                    "Completion-qualified mutation evidence did not advance beyond its baseline presentation.");
            }
            return PassWindow();
        }

        [[nodiscard]] bool HasNoCumulativeFullMaterializationGrowth(
            const SampleRenderMutationEvidenceDiagnostics& before,
            const SampleRenderMutationEvidenceDiagnostics& after) noexcept
        {
            return after.gpuCullingInstanceFullMaterializationCount ==
                       before.gpuCullingInstanceFullMaterializationCount &&
                   after.gpuCullingCandidateFullMaterializationCount ==
                       before.gpuCullingCandidateFullMaterializationCount &&
                   after.gpuCullingActiveRowFullMaterializationCount ==
                       before.gpuCullingActiveRowFullMaterializationCount &&
                   after.gpuCullingContinuityFullMaterializationCount ==
                       before.gpuCullingContinuityFullMaterializationCount &&
                   after.gpuCullingCapacityFullMaterializationCount ==
                       before.gpuCullingCapacityFullMaterializationCount &&
                   after.directRasterInstanceFullMaterializationCount ==
                       before.directRasterInstanceFullMaterializationCount &&
                   after.directRasterIndexFullMaterializationCount ==
                       before.directRasterIndexFullMaterializationCount;
        }

        [[nodiscard]] bool HasExpectedExecutionPopulation(
            const RenderingStressDiagnosticsSnapshot& snapshot,
            uint32 expectedPopulation,
            uint32 expectedGPUDrivenOwnerCount,
            uint32 expectedDirectRasterOwnerCount) noexcept;

        [[nodiscard]] RenderingStressWindowEvaluation
            EvaluateCumulativeGpuCullingWork(
            const SampleRenderMutationEvidenceDiagnostics& before,
            const SampleRenderMutationEvidenceDiagnostics& after,
            MutationActionKind action,
            uint32 expectedActionCount,
            uint32 expectedOwnerCount)
        {
            if (!HasNoCumulativeFullMaterializationGrowth(before, after))
            {
                return FailWindow(
                    "GPU-culling mutation evidence grew a full materialization counter.");
            }
            if (expectedOwnerCount == 0)
            {
                const bool inactiveRouteIsQuiet =
                    after.gpuCullingInstancePatchedRowCount ==
                        before.gpuCullingInstancePatchedRowCount &&
                    after.gpuCullingCandidatePatchedRowCount ==
                        before.gpuCullingCandidatePatchedRowCount &&
                    after.gpuCullingActiveRowPatchedRowCount ==
                        before.gpuCullingActiveRowPatchedRowCount &&
                    after.gpuCullingInstanceUploadedRowCount ==
                        before.gpuCullingInstanceUploadedRowCount &&
                    after.gpuCullingCandidateUploadedRowCount ==
                        before.gpuCullingCandidateUploadedRowCount &&
                    after.gpuCullingActiveRowUploadedRowCount ==
                        before.gpuCullingActiveRowUploadedRowCount &&
                    after.gpuCullingInstanceUploadBytes ==
                        before.gpuCullingInstanceUploadBytes &&
                    after.gpuCullingCandidateUploadBytes ==
                        before.gpuCullingCandidateUploadBytes &&
                    after.gpuCullingActiveRowUploadBytes ==
                        before.gpuCullingActiveRowUploadBytes;
                return inactiveRouteIsQuiet
                           ? PassWindow()
                           : FailWindow(
                                 "Inactive GPU-culling route recorded cumulative work.");
            }

            uint64 canonicalLimit = 0;
            uint64 physicalLimit = 0;
            if (!TryMultiplyMutationCount(expectedOwnerCount,
                                          expectedActionCount,
                                          canonicalLimit) ||
                !TryMultiplyMutationCount(canonicalLimit,
                                          RVX_MAX_FRAME_COUNT,
                                          physicalLimit))
            {
                return FailWindow(
                    "GPU-culling mutation work expectation overflowed.");
            }
            const uint64 instancePatched =
                after.gpuCullingInstancePatchedRowCount -
                before.gpuCullingInstancePatchedRowCount;
            const uint64 candidatePatched =
                after.gpuCullingCandidatePatchedRowCount -
                before.gpuCullingCandidatePatchedRowCount;
            const uint64 activePatched =
                after.gpuCullingActiveRowPatchedRowCount -
                before.gpuCullingActiveRowPatchedRowCount;
            const uint64 instanceUploaded =
                after.gpuCullingInstanceUploadedRowCount -
                before.gpuCullingInstanceUploadedRowCount;
            const uint64 candidateUploaded =
                after.gpuCullingCandidateUploadedRowCount -
                before.gpuCullingCandidateUploadedRowCount;
            const uint64 activeUploaded =
                after.gpuCullingActiveRowUploadedRowCount -
                before.gpuCullingActiveRowUploadedRowCount;
            const uint64 instanceUploadBytes =
                after.gpuCullingInstanceUploadBytes -
                before.gpuCullingInstanceUploadBytes;
            const uint64 candidateUploadBytes =
                after.gpuCullingCandidateUploadBytes -
                before.gpuCullingCandidateUploadBytes;
            const uint64 activeUploadBytes =
                after.gpuCullingActiveRowUploadBytes -
                before.gpuCullingActiveRowUploadBytes;
            const bool bytesFollowRows =
                (instanceUploaded == 0) == (instanceUploadBytes == 0) &&
                (candidateUploaded == 0) == (candidateUploadBytes == 0) &&
                (activeUploaded == 0) == (activeUploadBytes == 0);
            if (!bytesFollowRows)
            {
                return FailWindow(
                    "GPU-culling uploaded bytes changed without the matching physical rows.");
            }

            const auto hasBoundedPhysicalRows = [physicalLimit](uint64 rows)
            {
                return rows != 0 && rows <= physicalLimit;
            };
            const bool dirtyRowsMatch =
                instancePatched == canonicalLimit && candidatePatched == 0 &&
                activePatched == 0 &&
                hasBoundedPhysicalRows(instanceUploaded) &&
                candidateUploaded == 0 && activeUploaded == 0;
            const bool removeRowsMatch =
                instancePatched == 0 && candidatePatched == 0 &&
                activePatched == canonicalLimit && instanceUploaded == 0 &&
                candidateUploaded == 0 &&
                hasBoundedPhysicalRows(activeUploaded);
            const bool addTierOneRowsMatch =
                instancePatched == canonicalLimit && candidatePatched == 0 &&
                activePatched == canonicalLimit &&
                hasBoundedPhysicalRows(instanceUploaded) &&
                candidateUploaded == 0 &&
                hasBoundedPhysicalRows(activeUploaded);
            const bool addCandidateReadyRowsMatch =
                instancePatched == canonicalLimit &&
                candidatePatched == canonicalLimit &&
                activePatched == canonicalLimit &&
                hasBoundedPhysicalRows(instanceUploaded) &&
                hasBoundedPhysicalRows(candidateUploaded) &&
                hasBoundedPhysicalRows(activeUploaded);

            switch (action)
            {
                case MutationActionKind::Dirty:
                    return dirtyRowsMatch
                               ? PassWindow()
                               : FailWindow(
                                     "Dirty GPU-culling work did not retain its exact instance-only action matrix.");
                case MutationActionKind::Remove:
                    return removeRowsMatch
                               ? PassWindow()
                               : FailWindow(
                                     "Remove GPU-culling work did not retain its exact active-row-only action matrix.");
                case MutationActionKind::Add:
                    if (before.gpuCullingCandidateFullMaterializationCount != 0)
                    {
                        if (addCandidateReadyRowsMatch)
                            return PassWindow();
                        if (addTierOneRowsMatch)
                        {
                            return PendingWindow(
                                "Waiting for warm candidate-row evidence after the Tier-1 GPU-culling add state.");
                        }
                        return FailWindow(
                            "Warm GPU-culling add work did not retain its exact candidate-ready action matrix.");
                    }
                    return addTierOneRowsMatch
                               ? PassWindow()
                               : FailWindow(
                                     "Tier-1-only GPU-culling add work did not retain its exact action matrix.");
                default:
                    return FailWindow(
                        "GPU-culling mutation action kind is invalid.");
            }
        }

        [[nodiscard]] bool HasBoundedCumulativeDirectRasterWork(
            const SampleRenderMutationEvidenceDiagnostics& before,
            const SampleRenderMutationEvidenceDiagnostics& after,
            MutationActionKind action,
            uint32 expectedActionCount,
            uint32 expectedOwnerCount) noexcept
        {
            if (!HasNoCumulativeFullMaterializationGrowth(before, after))
                return false;
            if (expectedOwnerCount == 0)
            {
                return after.directRasterInstancePatchedRowCount ==
                           before.directRasterInstancePatchedRowCount &&
                       after.directRasterIndexPatchedRowCount ==
                           before.directRasterIndexPatchedRowCount &&
                       after.directRasterInstanceUploadBytes ==
                           before.directRasterInstanceUploadBytes &&
                       after.directRasterIndexUploadBytes ==
                           before.directRasterIndexUploadBytes;
            }

            uint64 ownerActionCount = 0;
            uint64 physicalLimit = 0;
            if (!TryMultiplyMutationCount(expectedOwnerCount,
                                          expectedActionCount,
                                          ownerActionCount) ||
                !TryMultiplyMutationCount(ownerActionCount,
                                          RVX_MAX_FRAME_COUNT,
                                          physicalLimit))
            {
                return false;
            }
            const uint64 instancePatched =
                after.directRasterInstancePatchedRowCount -
                before.directRasterInstancePatchedRowCount;
            const uint64 indexPatched =
                after.directRasterIndexPatchedRowCount -
                before.directRasterIndexPatchedRowCount;
            const uint64 instanceUploadBytes =
                after.directRasterInstanceUploadBytes -
                before.directRasterInstanceUploadBytes;
            const uint64 indexUploadBytes =
                after.directRasterIndexUploadBytes -
                before.directRasterIndexUploadBytes;
            const bool instanceMatches = action == MutationActionKind::Remove
                                             ? instancePatched == 0
                                             : instancePatched != 0 &&
                                                   instancePatched <= physicalLimit;
            const bool indexMatches = action == MutationActionKind::Dirty
                                          ? indexPatched == 0
                                          : indexPatched != 0 &&
                                                indexPatched <= physicalLimit;
            const bool bytesFollowRows =
                (instancePatched == 0) == (instanceUploadBytes == 0) &&
                (indexPatched == 0) == (indexUploadBytes == 0);
            return instanceMatches && indexMatches && bytesFollowRows;
        }

        [[nodiscard]] RenderingStressWindowEvaluation
            EvaluateCumulativeMutationWindow(
                const RenderingStressDiagnosticsSnapshot& before,
                const RenderingStressDiagnosticsSnapshot& after,
                uint64 actionTargetSceneRevision,
                MutationActionKind action,
                uint32 expectedActionCount,
                uint32 expectedBeforePopulation,
                uint32 expectedAfterPopulation,
                uint32 expectedGPUDrivenOwnerCount,
                uint32 expectedDirectRasterOwnerCount)
        {
            if (expectedActionCount == 0 || expectedBeforePopulation == 0 ||
                expectedAfterPopulation == 0 || !before.available ||
                !after.available || !before.sceneWorkAvailable ||
                !after.sceneWorkAvailable || !before.gpuSceneAvailable ||
                !after.gpuSceneAvailable ||
                !before.acceptedExtractionDiagnosticsAvailable ||
                !after.acceptedExtractionDiagnosticsAvailable)
            {
                return FailWindow(
                    "Mutation-window diagnostics or expected populations are unavailable.");
            }

            const RenderingStressWindowEvaluation boundary =
                ValidateMutationEvidenceBoundary(before,
                                                 after,
                                                 actionTargetSceneRevision);
            if (!boundary.passed)
                return boundary;
            if (!HasExpectedExecutionPopulation(before,
                                                expectedBeforePopulation,
                                                expectedGPUDrivenOwnerCount,
                                                expectedDirectRasterOwnerCount) ||
                !HasExpectedExecutionPopulation(after,
                                                expectedAfterPopulation,
                                                expectedGPUDrivenOwnerCount,
                                                expectedDirectRasterOwnerCount) ||
                after.renderSceneObjectCount != expectedAfterPopulation ||
                after.gpuScenePublishedObjectCount != expectedAfterPopulation)
            {
                return FailWindow(
                    "Mutation-window completed population does not match its exact action bounds.");
            }

            const SampleRenderMutationEvidenceDiagnostics& baseline =
                before.mutationEvidence;
            const SampleRenderMutationEvidenceDiagnostics& current =
                after.mutationEvidence;
            if (baseline.gpuCullingOwnerCount == 0 ||
                current.gpuCullingOwnerCount !=
                    baseline.gpuCullingOwnerCount)
            {
                return FailWindow(
                    "Mutation-window cumulative GPU-culling owner count is unavailable or changed.");
            }
            if (baseline.directRasterOwnerCount == 0 ||
                current.directRasterOwnerCount !=
                    baseline.directRasterOwnerCount)
            {
                return FailWindow(
                    "Mutation-window cumulative Direct-raster owner count is unavailable or changed.");
            }
            if ((expectedGPUDrivenOwnerCount != 0 &&
                 expectedGPUDrivenOwnerCount >
                     baseline.gpuCullingOwnerCount) ||
                (expectedDirectRasterOwnerCount != 0 &&
                 expectedDirectRasterOwnerCount >
                     baseline.directRasterOwnerCount))
            {
                return FailWindow(
                    "Mutation-window active execution owner count exceeds its stable persistent-cache census.");
            }
            // The watermark owner censuses prove that persistent cache
            // membership was stable. Only executable flows prepare and
            // submit action work, so their active multiplicities retain
            // ownership of canonical and physical work bounds. An inactive
            // route remains exactly work-free even with a nonzero census.
            const uint64 rebuilt = current.sceneRebuiltObjectCount -
                                   baseline.sceneRebuiltObjectCount;
            const uint64 removed = current.sceneRemovedObjectCount -
                                   baseline.sceneRemovedObjectCount;
            const uint64 add = current.gpuSceneAddCount -
                               baseline.gpuSceneAddCount;
            const uint64 update = current.gpuSceneUpdateCount -
                                  baseline.gpuSceneUpdateCount;
            const uint64 remove = current.gpuSceneRemoveCount -
                                  baseline.gpuSceneRemoveCount;
            const uint64 noOp = current.gpuSceneNoOpCount -
                                baseline.gpuSceneNoOpCount;
            const bool hasExpectedSceneDelta =
                current.sceneFullRebuildCount == baseline.sceneFullRebuildCount &&
                current.sceneIncrementalCommitCount ==
                    baseline.sceneIncrementalCommitCount + 1u &&
                ((action == MutationActionKind::Dirty && rebuilt == expectedActionCount &&
                  removed == 0) ||
                 (action == MutationActionKind::Remove && rebuilt == 0 &&
                  removed == expectedActionCount) ||
                 (action == MutationActionKind::Add && rebuilt == expectedActionCount &&
                  removed == 0));
            if (!hasExpectedSceneDelta)
            {
                return FailWindow(
                    "Mutation window did not retain exactly one bounded Scene commit.");
            }

            const uint64 incrementalPublicationDelta =
                current.gpuSceneIncrementalPublicationCount -
                baseline.gpuSceneIncrementalPublicationCount;
            const uint64 identityOnlyPublicationDelta =
                current.gpuSceneIdentityOnlyPublicationCount -
                baseline.gpuSceneIdentityOnlyPublicationCount;
            if (incrementalPublicationDelta < identityOnlyPublicationDelta)
            {
                return FailWindow(
                    "Mutation window recorded more identity-only GPU-scene publications than incremental publications.");
            }
            const uint64 meaningfulIncrementalPublicationDelta =
                incrementalPublicationDelta - identityOnlyPublicationDelta;
            uint64 expectedGPUSceneUploadedRows = 0;
            uint64 actionAddAndUpdateRows = 0;
            if (!TryAddMutationCount(add,
                                     update,
                                     actionAddAndUpdateRows) ||
                !TryAddMutationCount(actionAddAndUpdateRows,
                                     remove,
                                     expectedGPUSceneUploadedRows))
            {
                return FailWindow(
                    "Mutation window GPU-scene uploaded-row expectation overflowed.");
            }
            const uint64 materializedObjectDelta =
                current.gpuSceneMaterializedObjectCount -
                baseline.gpuSceneMaterializedObjectCount;
            const uint64 expectedMaterializedObjectDelta =
                action == MutationActionKind::Remove
                    ? 0u
                    : action == MutationActionKind::Dirty
                          ? update
                          : actionAddAndUpdateRows;
            const bool expectedGpuSceneDelta =
                current.gpuSceneFullPublicationCount ==
                    baseline.gpuSceneFullPublicationCount &&
                materializedObjectDelta == expectedMaterializedObjectDelta &&
                noOp == 0 &&
                ((action == MutationActionKind::Dirty && add == 0 &&
                  remove == 0 &&
                  ((update == expectedActionCount &&
                    meaningfulIncrementalPublicationDelta == 1u) ||
                   (update == static_cast<uint64>(expectedActionCount) * 2u &&
                    meaningfulIncrementalPublicationDelta == 2u))) ||
                 (action == MutationActionKind::Remove &&
                  meaningfulIncrementalPublicationDelta == 1u && add == 0 &&
                  update == 0 && remove == expectedActionCount) ||
                 (action == MutationActionKind::Add && add == expectedActionCount &&
                  remove == 0 &&
                  ((update == 0 && meaningfulIncrementalPublicationDelta == 1u) ||
                   (update == expectedActionCount &&
                    meaningfulIncrementalPublicationDelta == 2u))));
            bool exactGPUSceneUploadedRows = true;
            for (uint32 table = 0;
                 table < GPU_SCENE_DIAGNOSTICS_TABLE_COUNT;
                 ++table)
            {
                if (current.gpuSceneSubmittedUploadedRowCount[table] -
                        baseline.gpuSceneSubmittedUploadedRowCount[table] !=
                    expectedGPUSceneUploadedRows)
                {
                    exactGPUSceneUploadedRows = false;
                    break;
                }
            }
            if (!expectedGpuSceneDelta ||
                !exactGPUSceneUploadedRows ||
                current.gpuSceneFullUploadCount !=
                    baseline.gpuSceneFullUploadCount ||
                current.gpuSceneSubmittedUploadCount -
                        baseline.gpuSceneSubmittedUploadCount !=
                    meaningfulIncrementalPublicationDelta ||
                current.gpuSceneUploadBytes <= baseline.gpuSceneUploadBytes ||
                current.gpuSceneUploadRangeCount <=
                    baseline.gpuSceneUploadRangeCount)
            {
                return FailWindow(
                    "Mutation window did not retain bounded incremental GPU-scene publication and upload work.");
            }

            if (!HasBoundedCumulativeDirectRasterWork(
                    baseline,
                    current,
                    action,
                    expectedActionCount,
                    expectedDirectRasterOwnerCount))
            {
                return FailWindow(
                    "Mutation window exceeded cumulative Direct-raster canonical or physical stream work bounds.");
            }
            const RenderingStressWindowEvaluation gpuCullingWork =
                EvaluateCumulativeGpuCullingWork(baseline,
                                                  current,
                                                  action,
                                                  expectedActionCount,
                                                  expectedGPUDrivenOwnerCount);
            if (!gpuCullingWork.passed && !gpuCullingWork.pending)
                return gpuCullingWork;

            uint64 expectedChangeFeedCount = 0;
            if (!TryMultiplyMutationCount(
                    expectedActionCount,
                    GetExpectedChangeFeedMultiplier(action),
                    expectedChangeFeedCount))
            {
                return FailWindow(
                    "Mutation window change-feed expectation overflowed.");
            }
            const RenderingStressWindowEvaluation acceptedExtraction =
                EvaluateAcceptedExtractionMutationWindow(
                    before,
                    after,
                    expectedChangeFeedCount,
                    expectedActionCount,
                    action == MutationActionKind::Remove ? 0u
                                                         : expectedActionCount);
            if (!acceptedExtraction.passed)
                return acceptedExtraction;
            return gpuCullingWork;
        }

        [[nodiscard]] bool HasMutationTemporalSettle(
            const RenderingStressDiagnosticsSnapshot& before,
            const RenderingStressDiagnosticsSnapshot& after,
            MutationActionKind action,
            uint32 expectedActionCount) noexcept
        {
            if (action == MutationActionKind::Remove)
                return false;

            const uint64 updateDelta = after.mutationEvidence.gpuSceneUpdateCount -
                                       before.mutationEvidence.gpuSceneUpdateCount;
            return action == MutationActionKind::Dirty
                       ? updateDelta == static_cast<uint64>(expectedActionCount) * 2u
                       : updateDelta == expectedActionCount;
        }


        [[nodiscard]] std::optional<uint32> GetExactOwnerCount(
            uint32 activeRowCount,
            uint32 objectCount) noexcept
        {
            if (activeRowCount == 0 || objectCount == 0 ||
                activeRowCount % objectCount != 0)
            {
                return std::nullopt;
            }

            return activeRowCount / objectCount;
        }

        [[nodiscard]] bool HasNoGPUDrivenExecutionWork(
            const RenderingStressDiagnosticsSnapshot& snapshot) noexcept
        {
            return snapshot.gpuDrivenInstanceUploadBytes == 0 &&
                   snapshot.gpuDrivenCandidateUploadBytes == 0 &&
                   snapshot.gpuDrivenActiveRowUploadBytes == 0 &&
                   snapshot.gpuDrivenInstancePatchedRowCount == 0 &&
                   snapshot.gpuDrivenCandidatePatchedRowCount == 0 &&
                   snapshot.gpuDrivenActiveRowPatchedRowCount == 0;
        }

        [[nodiscard]] bool HasDirectRasterCapacity(
            const RenderingStressDiagnosticsSnapshot& snapshot) noexcept;
        [[nodiscard]] bool HasNoDirectRasterWork(
            const RenderingStressDiagnosticsSnapshot& snapshot) noexcept;

        [[nodiscard]] bool HasExpectedGPUDrivenPopulation(
            const RenderingStressDiagnosticsSnapshot& snapshot,
            uint32 expectedPopulation,
            uint32 expectedOwnerCount) noexcept
        {
            if (expectedOwnerCount == 0)
            {
                return snapshot.gpuDrivenActiveRowCount == 0 &&
                       snapshot.gpuDrivenActiveRowHighWatermark == 0 &&
                       HasNoGPUDrivenExecutionWork(snapshot);
            }

            const uint64 expectedActiveRows =
                static_cast<uint64>(expectedPopulation) * expectedOwnerCount;
            return expectedActiveRows <=
                       std::numeric_limits<uint32>::max() &&
                   snapshot.gpuDrivenActiveRowCount == expectedActiveRows &&
                   snapshot.gpuDrivenActiveRowHighWatermark >=
                       snapshot.gpuDrivenActiveRowCount;
        }

        [[nodiscard]] bool HasExpectedDirectRasterPopulation(
            const RenderingStressDiagnosticsSnapshot& snapshot,
            uint32 expectedPopulation,
            uint32 expectedOwnerCount) noexcept
        {
            if (expectedOwnerCount == 0)
            {
                return snapshot.directRasterActiveInstanceCount == 0 &&
                       HasDirectRasterCapacity(snapshot) &&
                       HasNoDirectRasterWork(snapshot);
            }

            const uint64 expectedActiveInstances =
                static_cast<uint64>(expectedPopulation) * expectedOwnerCount;
            return expectedActiveInstances <=
                       std::numeric_limits<uint32>::max() &&
                   snapshot.directRasterActiveInstanceCount ==
                       expectedActiveInstances &&
                   HasDirectRasterCapacity(snapshot);
        }

        [[nodiscard]] bool HasExpectedExecutionPopulation(
            const RenderingStressDiagnosticsSnapshot& snapshot,
            uint32 expectedPopulation,
            uint32 expectedGPUDrivenOwnerCount,
            uint32 expectedDirectRasterOwnerCount) noexcept
        {
            return HasExpectedGPUDrivenPopulation(snapshot,
                                                  expectedPopulation,
                                                  expectedGPUDrivenOwnerCount) &&
                   HasExpectedDirectRasterPopulation(snapshot,
                                                     expectedPopulation,
                                                     expectedDirectRasterOwnerCount);
        }

        [[nodiscard]] bool HasNoExecutionFullMaterializationGrowth(
            const RenderingStressDiagnosticsSnapshot& before,
            const RenderingStressDiagnosticsSnapshot& after) noexcept
        {
            return before.gpuDrivenInstanceFullMaterializationCount ==
                       after.gpuDrivenInstanceFullMaterializationCount &&
                   before.gpuDrivenCandidateFullMaterializationCount ==
                       after.gpuDrivenCandidateFullMaterializationCount &&
                   before.gpuDrivenActiveRowFullMaterializationCount ==
                       after.gpuDrivenActiveRowFullMaterializationCount &&
                   before.gpuDrivenContinuityFullMaterializationCount ==
                       after.gpuDrivenContinuityFullMaterializationCount &&
                   before.gpuDrivenCapacityFullMaterializationCount ==
                       after.gpuDrivenCapacityFullMaterializationCount &&
                   after.directRasterInstanceFullMaterializationCount == 0 &&
                   after.directRasterIndexFullMaterializationCount == 0;
        }

        [[nodiscard]] bool HasExactDirtyStableRowPatches(
            const RenderingStressDiagnosticsSnapshot& snapshot,
            uint32 expectedDirtyCount,
            uint32 expectedObjectCount,
            uint32 expectedOwnerCount) noexcept
        {
            if (!HasExpectedGPUDrivenPopulation(snapshot,
                                                expectedObjectCount,
                                                expectedOwnerCount))
            {
                return false;
            }
            if (expectedOwnerCount == 0)
                return HasNoGPUDrivenExecutionWork(snapshot);
            if (snapshot.gpuDrivenActiveRowPatchedRowCount != 0)
                return false;

            const uint64 expectedPatchedRows =
                static_cast<uint64>(expectedOwnerCount) * expectedDirtyCount;
            const uint64 instancePatchedRows =
                snapshot.gpuDrivenInstancePatchedRowCount;
            const uint64 candidatePatchedRows =
                snapshot.gpuDrivenCandidatePatchedRowCount;
            return instancePatchedRows == expectedPatchedRows &&
                   candidatePatchedRows == 0;
        }

        [[nodiscard]] bool HasBoundedChurnStableRowPatches(
            const RenderingStressDiagnosticsSnapshot& before,
            const RenderingStressDiagnosticsSnapshot& snapshot,
            uint32 expectedChurnCount,
            uint32 expectedBeforePopulation,
            uint32 expectedAfterPopulation,
            uint32 expectedOwnerCount) noexcept
        {
            if (!HasExpectedGPUDrivenPopulation(before,
                                                expectedBeforePopulation,
                                                expectedOwnerCount) ||
                !HasExpectedGPUDrivenPopulation(snapshot,
                                                expectedAfterPopulation,
                                                expectedOwnerCount))
            {
                return false;
            }
            if (expectedOwnerCount == 0)
                return HasNoGPUDrivenExecutionWork(snapshot);

            const uint64 maximumPatchedRows =
                static_cast<uint64>(expectedOwnerCount) * expectedChurnCount;
            return snapshot.gpuDrivenInstancePatchedRowCount <=
                       maximumPatchedRows &&
                   snapshot.gpuDrivenCandidatePatchedRowCount <=
                       maximumPatchedRows &&
                   snapshot.gpuDrivenActiveRowPatchedRowCount <=
                       maximumPatchedRows;
        }

        [[nodiscard]] bool HasDirectRasterCapacity(
            const RenderingStressDiagnosticsSnapshot& snapshot) noexcept
        {
            return snapshot.directRasterActiveInstanceCapacity >=
                   snapshot.directRasterActiveInstanceCount;
        }

        [[nodiscard]] bool HasNoDirectRasterFullMaterialization(
            const RenderingStressDiagnosticsSnapshot& snapshot) noexcept
        {
            return snapshot.directRasterInstanceFullMaterializationCount == 0 &&
                   snapshot.directRasterIndexFullMaterializationCount == 0;
        }

        [[nodiscard]] bool HasNoDirectRasterWork(
            const RenderingStressDiagnosticsSnapshot& snapshot) noexcept
        {
            return snapshot.directRasterInstanceUploadBytes == 0 &&
                   snapshot.directRasterInstanceIndexUploadBytes == 0 &&
                   snapshot.directRasterInstancePatchedRowCount == 0 &&
                   snapshot.directRasterIndexPatchedRowCount == 0 &&
                   HasNoDirectRasterFullMaterialization(snapshot);
        }

        [[nodiscard]] bool HasExactDirtyDirectRasterPatches(
            const RenderingStressDiagnosticsSnapshot& snapshot,
            uint32 expectedDirtyCount,
            uint32 expectedObjectCount,
            uint32 expectedOwnerCount) noexcept
        {
            if (!HasExpectedDirectRasterPopulation(snapshot,
                                                   expectedObjectCount,
                                                   expectedOwnerCount))
            {
                return false;
            }
            if (expectedOwnerCount == 0)
                return HasNoDirectRasterWork(snapshot);

            return snapshot.directRasterInstancePatchedRowCount ==
                       static_cast<uint64>(expectedOwnerCount) *
                           expectedDirtyCount &&
                   snapshot.directRasterIndexPatchedRowCount == 0;
        }

        [[nodiscard]] bool HasBoundedChurnDirectRasterPatches(
            const RenderingStressDiagnosticsSnapshot& before,
            const RenderingStressDiagnosticsSnapshot& after,
            uint32 expectedChurnCount,
            uint32 expectedBeforePopulation,
            uint32 expectedAfterPopulation,
            uint32 expectedOwnerCount) noexcept
        {
            if (!HasExpectedDirectRasterPopulation(before,
                                                   expectedBeforePopulation,
                                                   expectedOwnerCount) ||
                !HasExpectedDirectRasterPopulation(after,
                                                   expectedAfterPopulation,
                                                   expectedOwnerCount))
            {
                return false;
            }
            if (expectedOwnerCount == 0)
                return HasNoDirectRasterWork(after);

            const uint64 maximumPatchedRows =
                static_cast<uint64>(expectedOwnerCount) * expectedChurnCount;
            return after.directRasterInstancePatchedRowCount <=
                       maximumPatchedRows &&
                   after.directRasterIndexPatchedRowCount <=
                       maximumPatchedRows;
        }

        [[nodiscard]] RenderingStressWindowEvaluation
            EvaluateActionDrainWindow(
                const RenderingStressDiagnosticsSnapshot& actionSnapshot,
                const RenderingStressDiagnosticsSnapshot& current,
                uint32 expectedPopulation,
                uint32 expectedActionCount,
                uint32 expectedGPUDrivenOwnerCount,
                uint32 expectedDirectRasterOwnerCount,
                bool allowTemporalSettle,
                bool temporalSettleAlreadyObserved,
                bool allowDirectIndexPatches,
                std::string_view windowName)
        {
            if (expectedPopulation == 0 || expectedActionCount == 0 ||
                !actionSnapshot.available || !current.available ||
                !actionSnapshot.sceneWorkAvailable || !current.sceneWorkAvailable ||
                !actionSnapshot.gpuSceneAvailable || !current.gpuSceneAvailable ||
                !actionSnapshot.acceptedExtractionDiagnosticsAvailable ||
                !current.acceptedExtractionDiagnosticsAvailable)
            {
                return FailWindow(std::string(windowName) +
                                  " diagnostics or expected populations are unavailable.");
            }
            if (actionSnapshot.renderSceneObjectCount != expectedPopulation ||
                current.renderSceneObjectCount != expectedPopulation ||
                actionSnapshot.gpuScenePublishedObjectCount != expectedPopulation ||
                current.gpuScenePublishedObjectCount != expectedPopulation ||
                !HasExpectedExecutionPopulation(actionSnapshot,
                                                expectedPopulation,
                                                expectedGPUDrivenOwnerCount,
                                                expectedDirectRasterOwnerCount) ||
                !HasExpectedExecutionPopulation(current,
                                                expectedPopulation,
                                                expectedGPUDrivenOwnerCount,
                                                expectedDirectRasterOwnerCount))
            {
                return FailWindow(std::string(windowName) +
                                  " changed its exact retained or execution population.");
            }
            if (current.sceneFullRebuildCount !=
                    actionSnapshot.sceneFullRebuildCount ||
                current.sceneIncrementalUpdateCount !=
                    actionSnapshot.sceneIncrementalUpdateCount ||
                current.drawPacketBuildCount != actionSnapshot.drawPacketBuildCount ||
                current.drawPacketInvalidationCount !=
                    actionSnapshot.drawPacketInvalidationCount ||
                current.drawPacketEntryCount != actionSnapshot.drawPacketEntryCount ||
                current.sceneStaticReuseCount <
                    actionSnapshot.sceneStaticReuseCount ||
                current.sceneLastRebuiltObjectCount != 0 ||
                current.sceneLastRemovedObjectCount != 0)
            {
                return FailWindow(std::string(windowName) +
                                  " performed retained Scene or draw-packet work after its action.");
            }
            const RenderingStressWindowEvaluation acceptedExtraction =
                EvaluateAcceptedExtractionStaticWindow(actionSnapshot, current);
            if (!acceptedExtraction.passed)
                return acceptedExtraction;

            if (current.gpuSceneAddCount != 0 ||
                current.gpuSceneRemoveCount != 0 ||
                current.gpuSceneNoOpCount != 0 || current.gpuSceneFullUpload)
            {
                return FailWindow(std::string(windowName) +
                                  " changed non-update GPU-scene rows after its action.");
            }

            const bool temporalSettle = current.gpuSceneUpdateCount != 0 ||
                                        current.gpuSceneFrameUploadBytes != 0;
            if (temporalSettle)
            {
                if (!allowTemporalSettle || temporalSettleAlreadyObserved ||
                    current.sceneStaticReuseCount !=
                        actionSnapshot.sceneStaticReuseCount ||
                    current.gpuSceneUpdateCount != expectedActionCount ||
                    current.gpuSceneFrameUploadBytes == 0)
                {
                    return FailWindow(std::string(windowName) +
                                      " reported an unexpected or non-exact temporal GPU-scene settle.");
                }
            }
            else if (current.sceneStaticReuseCount <=
                         actionSnapshot.sceneStaticReuseCount ||
                     (allowTemporalSettle &&
                      !temporalSettleAlreadyObserved))
            {
                return FailWindow(std::string(windowName) +
                                  " reached retained Scene reuse without its required temporal settle.");
            }

            if (current.gpuDrivenInstancePatchedRowCount != 0 ||
                current.gpuDrivenCandidatePatchedRowCount != 0 ||
                current.gpuDrivenActiveRowPatchedRowCount != 0 ||
                !HasNoExecutionFullMaterializationGrowth(actionSnapshot,
                                                          current))
            {
                return FailWindow(std::string(windowName) +
                                  " changed canonical GPU-driven rows or fully materialized a stream.");
            }

            const uint64 maximumDirectPatchedRows =
                static_cast<uint64>(expectedDirectRasterOwnerCount) *
                expectedActionCount;
            if (current.directRasterInstancePatchedRowCount >
                    maximumDirectPatchedRows ||
                (!allowDirectIndexPatches &&
                 current.directRasterIndexPatchedRowCount != 0) ||
                (allowDirectIndexPatches &&
                 current.directRasterIndexPatchedRowCount >
                     maximumDirectPatchedRows) ||
                current.gpuDrivenActiveRowHighWatermark !=
                    actionSnapshot.gpuDrivenActiveRowHighWatermark ||
                current.directRasterActiveInstanceCapacity !=
                    actionSnapshot.directRasterActiveInstanceCapacity)
            {
                return FailWindow(std::string(windowName) +
                                  " changed topology or exceeded its direct raster per-slot patch budget.");
            }
            return PassWindow();
        }

        [[nodiscard]] bool HasGPUDrivenExecutionEvidence(
            const SampleRenderDiagnostics& diagnostics) noexcept
        {
            return diagnostics.gpuDrivenEnabled &&
                   diagnostics.gpuDrivenGraphPassAdded &&
                   diagnostics.gpuDrivenGraphPassRecorded &&
                   diagnostics.gpuDrivenExecutionRecorded &&
                   diagnostics.gpuDrivenActiveRowCount != 0 &&
                   diagnostics.gpuDrivenActiveRowHighWatermark >=
                       diagnostics.gpuDrivenActiveRowCount;
        }

        [[nodiscard]] bool HasDirectExecutionEvidence(
            const SampleRenderDiagnostics& diagnostics) noexcept
        {
            return !diagnostics.gpuDrivenEnabled &&
                   diagnostics.opaqueExecutionCompleted &&
                   diagnostics.opaqueExecutedDrawCountAvailable &&
                   diagnostics.opaqueExecutedDrawCount != 0 &&
                   diagnostics.directRasterActiveInstanceCount != 0 &&
                   diagnostics.directRasterActiveInstanceCapacity >=
                       diagnostics.directRasterActiveInstanceCount;
        }

        [[nodiscard]] std::optional<RenderingStressExecutionPath>
            ResolveExecutionPath(const SampleRenderDiagnostics& diagnostics,
                                 SampleRenderPath requestedPath) noexcept
        {
            if (!diagnostics.gpuDrivenPolicyDecisionAvailable ||
                diagnostics.gpuDrivenRequestedMode !=
                    GetRenderGPUDrivenModeName(
                        RenderingStressSample::MapRenderPath(requestedPath)))
            {
                return std::nullopt;
            }

            switch (requestedPath)
            {
                case SampleRenderPath::Direct:
                    return HasDirectExecutionEvidence(diagnostics)
                               ? std::optional<RenderingStressExecutionPath>(
                                     RenderingStressExecutionPath::Direct)
                               : std::nullopt;
                case SampleRenderPath::GPUDriven:
                    return HasGPUDrivenExecutionEvidence(diagnostics)
                               ? std::optional<RenderingStressExecutionPath>(
                                     RenderingStressExecutionPath::GPUDriven)
                               : std::nullopt;
                case SampleRenderPath::Auto:
                    if (diagnostics.gpuDrivenEnabled)
                    {
                        return HasGPUDrivenExecutionEvidence(diagnostics)
                                   ? std::optional<RenderingStressExecutionPath>(
                                         RenderingStressExecutionPath::GPUDriven)
                                   : std::nullopt;
                    }
                    return HasDirectExecutionEvidence(diagnostics)
                               ? std::optional<RenderingStressExecutionPath>(
                                     RenderingStressExecutionPath::Direct)
                               : std::nullopt;
                default: return std::nullopt;
            }
        }

        [[nodiscard]] const char* GetExecutionPathName(
            std::optional<RenderingStressExecutionPath> path) noexcept
        {
            if (!path.has_value())
                return "unresolved";

            switch (*path)
            {
                case RenderingStressExecutionPath::Direct: return "direct";
                case RenderingStressExecutionPath::GPUDriven:
                    return "gpu-driven";
                case RenderingStressExecutionPath::Unresolved:
                default: return "unresolved";
            }
        }
    } // namespace

    const SampleInfo& RenderingStressSample::GetInfo() const noexcept
    {
        return RenderingStressInfo;
    }

    SampleAssessmentContract RenderingStressSample::GetAssessmentContract() const
    {
        SampleAssessmentContract contract;
        contract.code = AssessmentCode("SAMPLE.RENDERING_STRESS");
        contract.revision = "9";
        contract.checkpoints = {
            AssessmentCheckpoints::EngineBaseline,
            AssessmentCheckpoints::ScenarioSetup,
            AssessmentCheckpoints::ActionRequested,
            AssessmentCheckpoints::ActionApplied,
            AssessmentCheckpoints::ScenarioStable,
            AssessmentCheckpoints::TeardownBefore,
            AssessmentCheckpoints::TeardownSceneComplete,
            AssessmentCheckpoints::TeardownRenderDrained,
            AssessmentCheckpoints::EngineShutdownComplete};
        contract.actions = {
            FirstFrameAction,
            SharedAssetAction,
            InitialBuildAction,
            DirtySetAction,
            ChurnDestroyAction,
            ChurnRecreateAction};
        contract.invariants = {
            FirstFrameInvariant,
            SharedIdentityInvariant,
            StaticReuseInvariant,
            DirtyDeltaInvariant,
            ChurnDeltaInvariant,
            NoFullRebuildInvariant};
        contract.metrics = {
            ExtractionFullScanMetric,
            ExtractionChangeFeedMetric,
            ExtractionActorRebuildMetric,
            ExtractionProxyVisitMetric,
            ExtractionComponentVisitMetric,
            ExtractionProviderVisitMetric,
            AcceptedExtractionPublicationMetric,
            AcceptedExtractionLastSourceFrameMetric,
            AcceptedExtractionLastSceneRevisionMetric,
            AcceptedExtractionFullScanMetric,
            AcceptedExtractionChangeFeedMetric,
            AcceptedExtractionActorRebuildMetric,
            AcceptedExtractionProxyVisitMetric,
            AcceptedExtractionComponentVisitMetric,
            AcceptedExtractionProviderVisitMetric,
            AcceptedExtractionContinuityLossMetric,
            MutationActionTargetSceneRevisionMetric,
            MutationCompletedFrameSequenceMetric,
            MutationAppliedSceneRevisionMetric,
            MutationSceneIncrementalDeltaMetric,
            MutationSceneRebuiltDeltaMetric,
            MutationSceneRemovedDeltaMetric,
            MutationGPUSceneAddDeltaMetric,
            MutationGPUSceneUpdateDeltaMetric,
            MutationGPUSceneRemoveDeltaMetric,
            MutationGPUCullingPatchedRowDeltaMetric,
            MutationDirectRasterPatchedRowDeltaMetric,
            SceneFullRebuildMetric,
            SceneIncrementalMetric,
            SceneStaticReuseMetric,
            SceneLastRebuiltMetric,
            SceneLastRemovedMetric,
            DrawPacketBuildMetric,
            DrawPacketInvalidationMetric,
            GPUSceneAddMetric,
            GPUSceneUpdateMetric,
            GPUSceneRemoveMetric,
            GPUSceneNoOpMetric,
            GPUSceneUploadMetric,
            GPUSceneFullUploadMetric,
            GPUDrivenInstanceUploadMetric,
            GPUDrivenCandidateUploadMetric,
            GPUDrivenActiveRowUploadMetric,
            GPUDrivenActiveRowCountMetric,
            GPUDrivenActiveRowHighWatermarkMetric,
            GPUDrivenInstancePatchedRowMetric,
            GPUDrivenCandidatePatchedRowMetric,
            GPUDrivenActiveRowPatchedRowMetric,
            GPUDrivenInstanceFullMaterializationMetric,
            GPUDrivenCandidateFullMaterializationMetric,
            GPUDrivenActiveRowFullMaterializationMetric,
            GPUDrivenContinuityFullMaterializationMetric,
            GPUDrivenCapacityFullMaterializationMetric,
            DirectRasterInstanceUploadMetric,
            DirectRasterIndexUploadMetric,
            DirectRasterInstancePatchedRowMetric,
            DirectRasterIndexPatchedRowMetric,
            DirectRasterActiveInstanceCountMetric,
            DirectRasterActiveInstanceCapacityMetric,
            DirectRasterInstanceFullMaterializationMetric,
            DirectRasterIndexFullMaterializationMetric,
            ChurnDestroyedTotalMetric,
            ChurnRecreatedTotalMetric};
        contract.capabilities = {
            RetainedSceneCapability, GPUSceneCapability, ExtractionCapability};
        return contract;
    }

    bool RenderingStressSample::Setup(SampleContext& context,
                                      std::string& outError)
    {
        m_sharedModel = {};
        m_sharedModelPath.clear();
        m_sharedMeshAssetId = {};
        m_sharedMaterialAssetId = {};
        m_sharedMaterialSlots = {};
        m_sharedMeshBounds = {};
        m_workload = {};
        m_phase = RenderingStressPhase::WaitingForFirstPresentation;
        m_workloadEntities.clear();
        m_dirtyIndices.clear();
        m_churnIndices.clear();
        m_churnDestroyReceipts.clear();
        m_latestDiagnostics.reset();
        m_windowBaseline.reset();
        m_dirtyActionBaseline.reset();
        m_churnBatchBaseline.reset();
        m_churnBatchActionSnapshot.reset();
        m_churnDrainBaseline.reset();
        m_churnDrainActionBaseline.reset();
        m_pendingActionRenderEvidence.reset();
        m_pendingActionDrainSnapshots.clear();
        m_failure.clear();
        m_requestedRenderPath = context.options.renderPath;
        m_actualExecutionPath.reset();
        m_firstFramePresented = false;
        m_sharedLoadQueued = false;
        m_sharedAssetReady = false;
        m_workloadBuilt = false;
        m_dirtyApplied = false;
        m_dirtyDeltaObserved = false;
        m_dirtyTemporalSettleObserved = false;
        m_initialTemporalSettleObserved = false;
        m_churnDestroyBatchQueued = false;
        m_churnDestroyReceiptsApplied = false;
        m_churnBatchRemoveObserved = false;
        m_churnRemoveObserved = false;
        m_churnRecreateBatchQueued = false;
        m_churnBatchAddObserved = false;
        m_churnDestroyCursor = 0;
        m_churnRecreateCursor = 0;
        m_churnBatchOffset = 0;
        m_churnBatchCount = 0;
        m_churnDestroyedCount = 0;
        m_churnRecreatedCount = 0;
        m_gpuDrivenInitialOwnerCount = 0;
        m_directRasterInitialOwnerCount = 0;
        m_churnDrainExpectedPopulation = 0;
        m_churnDrainBatchCount = 0;
        m_staticStreamWarmupFrameCount = 0;
        m_dirtyDrainCompletedFrameCount = 0;
        m_churnDrainCompletedFrameCount = 0;
        m_actionTargetSceneRevision = 0;
        m_churnDrainActionTargetSceneRevision = 0;
        m_churnDrainNextPhase = RenderingStressPhase::Failed;
        m_churnDrainTemporalSettleAllowed = false;
        m_churnDrainTemporalSettleObserved = false;
        m_initialExecutionOwnerCountsCaptured = false;
        m_instrumentationObserved = false;
        m_incompleteReported = false;
        m_cameraConfigured = false;

        switch (m_requestedRenderPath)
        {
            case SampleRenderPath::Auto:
            case SampleRenderPath::Direct:
            case SampleRenderPath::GPUDriven:
                break;
            default:
                outError = "Rendering-stress received an invalid render-path selection";
                return false;
        }

        if (!context.options.workloadProfile.has_value())
        {
            outError = "Rendering-stress requires a host-resolved workload profile";
            return false;
        }
        m_workload = *context.options.workloadProfile;
        if (!IsExactProfile(m_workload) || !IsProfileUsable(m_workload))
        {
            outError = "Rendering-stress received a workload profile that does not match its declared scale";
            return false;
        }
        m_dirtyIndices = BuildDeterministicIndexSet(
            m_workload.objectCount, m_workload.dirtyObjectCount, 0u);
        m_churnIndices = BuildDeterministicIndexSet(
            m_workload.objectCount,
            m_workload.churnObjectCount,
            m_workload.objectCount / 3u);
        if (m_dirtyIndices.size() != m_workload.dirtyObjectCount ||
            m_churnIndices.size() != m_workload.churnObjectCount)
        {
            outError = "Rendering-stress could not construct exact deterministic entity sets";
            return false;
        }

        context.renderSettings.gpuCulling.maxVisibleObjects =
            m_workload.gpuCullingCapacity;
        context.renderSettings.gpuCulling.enableDistanceCulling = true;
        context.renderSettings.gpuCulling.maxDrawDistance =
            GetWorkloadFarDistance(m_workload.objectCount);
        context.renderSettings.gpuCulling.mode =
            MapRenderPath(m_requestedRenderPath);

        const float32 aspect = static_cast<float32>(context.options.width) /
                               static_cast<float32>(
                                   std::max(context.options.height, 1u));
        if (!context.cameras.SetPerspective(
                context.camera,
                radians(50.0f),
                aspect,
                0.05f,
                GetWorkloadFarDistance(m_workload.objectCount)) ||
            !context.cameras.SetPose(
                context.camera, {.position = Vec3(0.0f, 4.0f, 12.0f)}) ||
            !context.cameras.LookAt(context.camera, Vec3(0.0f)))
        {
            outError = "Rendering-stress could not configure its ECS camera";
            return false;
        }

        if (context.options.modelPath.empty())
        {
            outError = "Rendering-stress requires a host-resolved shared model path";
            return false;
        }
        m_sharedModelPath = context.options.modelPath;

        static_cast<void>(context.assessment.MarkCheckpoint(
            AssessmentCheckpoints::ScenarioSetup));
        static_cast<void>(context.assessment.MarkCheckpoint(
            AssessmentCheckpoints::ActionRequested));
        return true;
    }

    void RenderingStressSample::Update(SampleContext& context, float deltaTime)
    {
        static_cast<void>(deltaTime);
        if (m_phase == RenderingStressPhase::Failed ||
            m_phase == RenderingStressPhase::Complete)
        {
            return;
        }

        switch (m_phase)
        {
            case RenderingStressPhase::WaitingForFirstPresentation:
                return;
            case RenderingStressPhase::WaitingForSharedAsset:
                if (PrepareSharedAsset(context))
                    m_phase = RenderingStressPhase::InitialBuild;
                return;
            case RenderingStressPhase::InitialBuild:
                if (CanBuildWorkload(m_phase,
                                     m_firstFramePresented,
                                     m_sharedAssetReady) &&
                    BuildInitialWorkload(context))
                {
                    m_phase = RenderingStressPhase::StableBaseline;
                    m_windowBaseline.reset();
                    m_staticStreamWarmupFrameCount = 0;
                    m_initialTemporalSettleObserved = false;
                }
                return;
            case RenderingStressPhase::StableBaseline:
            case RenderingStressPhase::StableAfterDirty:
            case RenderingStressPhase::FinalStable:
                return;
            case RenderingStressPhase::DirtySet:
                if (ApplyDirtySet(context))
                {
                    m_phase = RenderingStressPhase::StableAfterDirty;
                    m_dirtyActionBaseline = m_latestDiagnostics;
                    m_windowBaseline = m_latestDiagnostics;
                    m_pendingActionRenderEvidence.reset();
                    m_staticStreamWarmupFrameCount = 0;
                    m_dirtyTemporalSettleObserved = false;
                    m_dirtyDrainCompletedFrameCount = 0;
                }
                return;
            case RenderingStressPhase::ChurnDestroy:
                if (!m_churnDestroyBatchQueued)
                    static_cast<void>(QueueChurnDestroy(context));
                else if (AreChurnDestroyReceiptsApplied(context) &&
                         m_churnBatchRemoveObserved)
                {
                    static_cast<void>(CompleteChurnDestroyBatch(context));
                }
                return;
            case RenderingStressPhase::ChurnRecreate:
                if (!m_churnRecreateBatchQueued)
                {
                    static_cast<void>(RecreateChurnEntities(context));
                }
                else if (m_churnBatchAddObserved)
                {
                    static_cast<void>(CompleteChurnRecreateBatch(context));
                }
                return;
            case RenderingStressPhase::ChurnDrain:
                return;
            case RenderingStressPhase::Complete:
            case RenderingStressPhase::Failed:
            default:
                return;
        }
    }

    void RenderingStressSample::OnInput(SampleContext& context)
    {
        if (!context.options.smoke && m_cameraConfigured && context.input)
            static_cast<void>(m_orbitCamera.Update(
                *context.input, context.cameras, context.camera));
    }

    void RenderingStressSample::OnViewportResize(SampleContext& context,
                                                  uint32 width,
                                                  uint32 height)
    {
        if (width == 0 || height == 0)
            return;
        const float32 aspect = static_cast<float32>(width) /
                               static_cast<float32>(height);
        static_cast<void>(m_orbitCamera.SetAspectRatio(
            aspect, context.cameras, context.camera));
    }

    void RenderingStressSample::AppendReport(SampleFeatureReporter& reporter) const
    {
        reporter.Enable("DeterministicRenderingStressStateMachine");
        reporter.Enable("SharedModelMetadataProceduralEntities");
        reporter.Enable("RetainedSceneDeltaDiagnostics");
        reporter.Enable("GPUSceneDeltaDiagnostics");
        if (m_sharedAssetReady)
        {
            reporter.Enable("ModelResourceLoad");
            reporter.Enable("EcsPreparedModelAdoption");
        }
        reporter.ResourceDiagnostic(
            "workload scale=" +
            std::string(GetSampleWorkloadScaleName(m_workload.scale)));
        reporter.ResourceDiagnostic(
            "workload objects=" + std::to_string(m_workload.objectCount));
        reporter.ResourceDiagnostic(
            "workload dirty=" + std::to_string(m_workload.dirtyObjectCount));
        reporter.ResourceDiagnostic(
            "workload churn=" + std::to_string(m_workload.churnObjectCount));
        reporter.ResourceDiagnostic(
            "churn batch size=" +
            std::to_string(GetDeterministicChurnBatchSize()));
        reporter.ResourceDiagnostic(
            "churn destroy verified=" +
            std::to_string(m_churnDestroyedCount));
        reporter.ResourceDiagnostic(
            "churn recreate verified=" +
            std::to_string(m_churnRecreatedCount));
        reporter.ResourceDiagnostic(
            "gpu culling capacity=" +
            std::to_string(m_workload.gpuCullingCapacity));
        reporter.ResourceDiagnostic(
            "requested render path=" +
            std::string(GetSampleRenderPathName(m_requestedRenderPath)));
        reporter.ResourceDiagnostic(
            "actual render path=" +
            std::string(GetExecutionPathName(m_actualExecutionPath)));
        reporter.ResourceDiagnostic(
            "gpu-driven initial owners=" +
            std::to_string(m_gpuDrivenInitialOwnerCount));
        reporter.ResourceDiagnostic(
            "direct raster initial owners=" +
            std::to_string(m_directRasterInitialOwnerCount));
        reporter.ResourceDiagnostic(
            "phase=" + std::string(GetPhaseName(m_phase)));
    }

    void RenderingStressSample::ObserveDiagnostics(
        const SampleRenderDiagnostics& diagnostics,
        SampleAssessmentChannel& assessment)
    {
        if (m_phase == RenderingStressPhase::Failed ||
            m_phase == RenderingStressPhase::Complete)
        {
            return;
        }

        if (m_phase == RenderingStressPhase::WaitingForFirstPresentation)
        {
            const std::optional<uint64>& presented =
                diagnostics.lastPresentedFrameSequence.GetValue();
            if (!presented.has_value() || *presented == 0)
                return;

            m_firstFramePresented = true;
            static_cast<void>(assessment.MarkAction(FirstFrameAction));
            MarkInvariant(assessment,
                          FirstFrameInvariant,
                          AssessmentCheckpoints::ActionApplied);
            static_cast<void>(assessment.MarkCheckpoint(
                AssessmentCheckpoints::ActionApplied));
            m_phase = RenderingStressPhase::WaitingForSharedAsset;
            return;
        }

        if (m_phase == RenderingStressPhase::WaitingForSharedAsset ||
            m_phase == RenderingStressPhase::InitialBuild ||
            m_phase == RenderingStressPhase::DirtySet)
        {
            return;
        }

        if (!HasBaseInstrumentation(diagnostics))
        {
            ReportInstrumentationGap(
                assessment,
                "Completed-frame retained Scene, GPU-scene, extraction, or cumulative mutation diagnostics are unavailable.");
            return;
        }
        if (IsWaitingForRequiredSceneRevision(diagnostics))
        {
            // A successfully completed frame can legitimately predate the
            // deterministic workload commands accepted by the update thread.
            // Do not consume path-propagation budget for that stale revision.
            return;
        }

        const RenderingStressDiagnosticsSnapshot current =
            CaptureDiagnostics(diagnostics);
        if (m_latestDiagnostics.has_value() &&
            !ShouldConsumeDiagnosticObservation(
                *m_latestDiagnostics,
                current,
                m_pendingActionRenderEvidence.has_value()))
        {
            return;
        }
        m_latestDiagnostics = current;
        if (!m_instrumentationObserved)
        {
            static_cast<void>(assessment.MarkCapabilityObservation({
                RetainedSceneCapability,
                AssessmentCheckpoints::ActionApplied,
                DiagnosticValue<bool>::Available(true),
                "Retained Scene and draw-packet diagnostics are available."}));
            static_cast<void>(assessment.MarkCapabilityObservation({
                GPUSceneCapability,
                AssessmentCheckpoints::ActionApplied,
                DiagnosticValue<bool>::Available(true),
                "GPU-scene delta diagnostics are available."}));
            static_cast<void>(assessment.MarkCapabilityObservation({
                ExtractionCapability,
                AssessmentCheckpoints::ActionApplied,
                DiagnosticValue<bool>::Available(true),
                "Incremental extraction diagnostics are available."}));
            m_instrumentationObserved = true;
        }

        const bool awaitingInitialPopulation =
            m_phase == RenderingStressPhase::StableBaseline &&
            !m_windowBaseline.has_value() &&
            !HasCompleteInitialPopulation(current, m_workload.objectCount);
        if (awaitingInitialPopulation)
        {
            // The update thread may have applied thousands of deterministic
            // Scene commands while the immutable completed-frame snapshot still
            // describes the pre-workload revision.  An empty selected lane is
            // therefore pending evidence, not a capability failure.
            return;
        }

        const std::optional<RenderingStressExecutionPath> actualPath =
            ResolveExecutionPath(diagnostics, m_requestedRenderPath);
        if (!actualPath.has_value())
        {
            if (m_phase == RenderingStressPhase::StableBaseline &&
                !m_windowBaseline.has_value() &&
                ++m_staticStreamWarmupFrameCount <=
                    MaxStaticStreamWarmupFrames)
            {
                // Persistent populations have arrived, but a completion-safe
                // frame slot may still need to materialize the selected stream.
                return;
            }

            ReportInstrumentationGap(
                assessment,
                "The selected render path did not produce a usable completed-frame execution stream after persistent population and bounded slot propagation. requested=" +
                    std::string(GetSampleRenderPathName(m_requestedRenderPath)) +
                    ", frame=" + std::to_string(current.sourceFrameSequence) +
                    ", render-scene=" +
                    std::to_string(current.renderSceneObjectCount) +
                    ", gpu-scene=" +
                    std::to_string(current.gpuScenePublishedObjectCount) +
                    ", draw-packets=" +
                    std::to_string(current.drawPacketEntryCount) +
                    ", policy=" + diagnostics.gpuDrivenPolicyReason + ".",
                false);
            return;
        }
        if (!m_actualExecutionPath.has_value())
        {
            m_actualExecutionPath = actualPath;
        }
        else if (*m_actualExecutionPath != *actualPath)
        {
            Fail(assessment,
                 StaticReuseInvariant.code,
                 "Completed-frame execution diagnostics changed the resolved render path.");
            return;
        }

        if (!QueuePendingActionDrainObservation(assessment, current))
            return;

        if (m_phase == RenderingStressPhase::StableBaseline)
        {
            if (!m_windowBaseline.has_value())
            {
                // Scene extraction, retained-scene application, draw-packet
                // caching, and GPU-scene publication cross asynchronous frame
                // boundaries.  Their one-frame delta counters therefore need
                // not coincide.  Establish the baseline from the persistent
                // completed populations, then evaluate the next frame's work.
                if (!HasCompleteInitialPopulation(current,
                                                  m_workload.objectCount))
                {
                    return;
                }
                if (!CaptureInitialExecutionOwnerCounts(assessment, current))
                    return;
                m_windowBaseline = current;
                m_staticStreamWarmupFrameCount = 0;
                PublishSnapshot(assessment,
                                AssessmentCheckpoints::ActionApplied,
                                current);
                return;
            }

            if (!ObserveQuiescentStaticWindow(assessment, current, true))
                return;
            PublishSnapshot(assessment, AssessmentCheckpoints::ScenarioStable, current);
            MarkInvariant(assessment,
                          StaticReuseInvariant,
                          AssessmentCheckpoints::ScenarioStable);
            m_phase = RenderingStressPhase::DirtySet;
            m_windowBaseline = current;
            return;
        }

        if (m_phase == RenderingStressPhase::StableAfterDirty)
        {
            if (!m_dirtyDeltaObserved)
            {
                if (!m_windowBaseline.has_value() ||
                    !m_dirtyActionBaseline.has_value() ||
                    m_actionTargetSceneRevision == 0)
                {
                    Fail(assessment,
                         DirtyDeltaInvariant.code,
                         "The dirty window did not retain a pre-action completed baseline and target revision.");
                    return;
                }
                const RenderingStressWindowEvaluation dirty =
                    EvaluateDirtyMutationEvidenceWindow(
                        *m_dirtyActionBaseline,
                        current,
                        m_actionTargetSceneRevision,
                        m_workload.dirtyObjectCount,
                        m_workload.objectCount,
                        m_gpuDrivenInitialOwnerCount,
                        m_directRasterInitialOwnerCount);
                if (dirty.pending)
                {
                    if (current.mutationEvidence.appliedSceneRevision >=
                        m_actionTargetSceneRevision)
                    {
                        // The immutable render completion already covers the
                        // action, but accepted extraction may be refreshed on
                        // this same completed frame. Retain it solely so that
                        // ShouldConsumeDiagnosticObservation admits that
                        // accepted-only update; it is not drain provenance.
                        m_pendingActionRenderEvidence = current;
                    }
                    return;
                }
                if (!dirty.passed)
                {
                    Fail(assessment, DirtyDeltaInvariant.code, dirty.detail);
                    return;
                }
                PublishSnapshot(assessment,
                                AssessmentCheckpoints::ActionApplied,
                                current);
                PublishMutationActionEvidence(
                    assessment,
                    AssessmentCheckpoints::ActionApplied,
                    *m_dirtyActionBaseline,
                    current,
                    m_actionTargetSceneRevision);
                MarkInvariant(assessment,
                              DirtyDeltaInvariant,
                              AssessmentCheckpoints::ActionApplied);
                MarkInvariant(assessment,
                              NoFullRebuildInvariant,
                              AssessmentCheckpoints::ActionApplied);
                m_dirtyDeltaObserved = true;
                m_windowBaseline = current;
                m_pendingActionRenderEvidence.reset();
                m_staticStreamWarmupFrameCount = 0;
                m_dirtyTemporalSettleObserved = HasMutationTemporalSettle(
                    *m_dirtyActionBaseline,
                    current,
                    MutationActionKind::Dirty,
                    m_workload.dirtyObjectCount);
                m_dirtyDrainCompletedFrameCount = 0;
                static_cast<void>(ReplayPendingDirtyDrain(assessment));
                return;
            }

            static_cast<void>(ObserveDirtyDrain(assessment, current));
            return;
        }

        if (m_phase == RenderingStressPhase::ChurnDestroy)
        {
            if (!m_churnDestroyBatchQueued ||
                !m_churnBatchBaseline.has_value())
                return;
            if (!m_churnBatchRemoveObserved)
            {
                if (!m_churnDestroyReceiptsApplied ||
                    m_actionTargetSceneRevision == 0)
                    return;

                const RenderingStressWindowEvaluation removed =
                    EvaluateChurnRemoveMutationEvidenceWindow(
                        *m_churnBatchBaseline,
                        current,
                        m_actionTargetSceneRevision,
                        m_churnBatchCount,
                        m_workload.objectCount - m_churnDestroyedCount +
                            m_churnRecreatedCount,
                        m_workload.objectCount - m_churnDestroyedCount +
                            m_churnRecreatedCount - m_churnBatchCount,
                        m_gpuDrivenInitialOwnerCount,
                        m_directRasterInitialOwnerCount);
                if (removed.pending)
                {
                    if (current.mutationEvidence.appliedSceneRevision >=
                        m_actionTargetSceneRevision)
                    {
                        m_pendingActionRenderEvidence = current;
                    }
                    return;
                }
                if (!removed.passed)
                {
                    Fail(assessment, ChurnDeltaInvariant.code, removed.detail);
                    return;
                }
                // Keep this completed-frame snapshot until Update has verified
                // and acknowledged every destroy receipt. No following batch
                // may be queued until both proofs exist.
                m_churnBatchRemoveObserved = true;
                m_churnBatchActionSnapshot = current;
                m_pendingActionRenderEvidence.reset();
            }
            return;
        }

        if (m_phase == RenderingStressPhase::ChurnRecreate)
        {
            if (!m_churnRecreateBatchQueued ||
                !m_churnBatchBaseline.has_value())
            {
                return;
            }
            if (!m_churnBatchAddObserved)
            {
                if (m_actionTargetSceneRevision == 0)
                    return;

                const RenderingStressWindowEvaluation added =
                    EvaluateChurnAddMutationEvidenceWindow(
                        *m_churnBatchBaseline,
                        current,
                        m_actionTargetSceneRevision,
                        m_churnBatchCount,
                        m_workload.objectCount - m_churnDestroyedCount +
                            m_churnRecreatedCount,
                        m_workload.objectCount - m_churnDestroyedCount +
                            m_churnRecreatedCount + m_churnBatchCount,
                        m_gpuDrivenInitialOwnerCount,
                        m_directRasterInitialOwnerCount);
                if (added.pending)
                {
                    if (current.mutationEvidence.appliedSceneRevision >=
                        m_actionTargetSceneRevision)
                    {
                        if (!IsWithinPresentationPropagationBudget(
                                m_churnBatchBaseline->mutationEvidence,
                                current.mutationEvidence,
                                GetChurnDrainCompletedFrameLimit(),
                                false))
                        {
                            Fail(assessment,
                                 ChurnDeltaInvariant.code,
                                 "Churn recreation action evidence exceeded its completed-presentation budget.");
                            return;
                        }
                        m_pendingActionRenderEvidence = current;
                    }
                    return;
                }
                if (!added.passed)
                {
                    Fail(assessment, ChurnDeltaInvariant.code, added.detail);
                    return;
                }
                m_churnBatchAddObserved = true;
                m_churnBatchActionSnapshot = current;
                m_pendingActionRenderEvidence.reset();
            }
            return;
        }

        if (m_phase == RenderingStressPhase::ChurnDrain)
        {
            static_cast<void>(ObserveChurnDrain(assessment, current));
            return;
        }

        if (m_phase == RenderingStressPhase::FinalStable)
        {
            if (!ObserveQuiescentStaticWindow(assessment, current, false))
                return;
            PublishSnapshot(assessment, AssessmentCheckpoints::ScenarioStable, current);
            MarkInvariant(assessment,
                          StaticReuseInvariant,
                          AssessmentCheckpoints::ScenarioStable);
            static_cast<void>(assessment.MarkCheckpoint(
                AssessmentCheckpoints::ScenarioStable));
            m_phase = RenderingStressPhase::Complete;
        }
    }

    SampleReadiness RenderingStressSample::GetReadiness(
        const SampleRenderDiagnostics& diagnostics) const
    {
        static_cast<void>(diagnostics);
        if (!m_failure.empty())
            return SampleReadiness::Failed(m_failure);
        if (m_phase == RenderingStressPhase::Complete)
            return SampleReadiness::Ready();
        return SampleReadiness::Pending(
            "Rendering-stress is waiting for phase " +
            std::string(GetPhaseName(m_phase)));
    }

    bool RenderingStressSample::ValidateResult(
        const SampleRenderDiagnostics& diagnostics,
        std::string& outError) const
    {
        const SampleReadiness readiness = GetReadiness(diagnostics);
        outError = readiness.reason;
        return readiness.IsReady();
    }

    void RenderingStressSample::Shutdown(SampleContext& context)
    {
        // Churn command receipts are owned by this sample, not by the host
        // lifetime scope. A queued command remains a teardown failure: it
        // would otherwise hide an in-flight ECS structural mutation.
        for (const SceneECS::SceneCommandReceipt& receipt :
             m_churnDestroyReceipts)
        {
            if (receipt.IsQueued())
            {
                Fail(context.assessment,
                     ChurnDeltaInvariant.code,
                     "Shutdown encountered an in-flight bounded churn destroy receipt.");
                continue;
            }
            if (!receipt.IsApplied())
            {
                Fail(context.assessment,
                     ChurnDeltaInvariant.code,
                     "Shutdown observed a rejected or discarded bounded churn destroy receipt.");
            }
        }
        ReportIncomplete(context);
        if (m_sharedModel.request.IsValid() &&
            !context.models.Cancel(m_sharedModel))
        {
            Fail(context.assessment,
                 SharedIdentityInvariant.code,
                 "Shutdown could not cancel the exact ECS shared-model request.");
        }
        m_sharedMeshAssetId = {};
        m_sharedMaterialAssetId = {};
        m_sharedMaterialSlots = {};
        m_sharedMeshBounds = {};
        m_cameraConfigured = false;
        m_workloadEntities.clear();
        m_churnDestroyReceipts.clear();
    }

    std::vector<uint32> RenderingStressSample::BuildDeterministicIndexSet(
        uint32 objectCount,
        uint32 selectedCount,
        uint32 phaseOffset)
    {
        std::vector<uint32> result;
        if (objectCount == 0 || selectedCount == 0 ||
            selectedCount > objectCount)
        {
            return result;
        }
        result.reserve(selectedCount);
        const uint64 offset = phaseOffset % objectCount;
        for (uint32 index = 0; index < selectedCount; ++index)
        {
            const uint64 distributed =
                static_cast<uint64>(index) * objectCount / selectedCount;
            result.push_back(static_cast<uint32>(
                (distributed + offset) % objectCount));
        }
        return result;
    }

    std::vector<RenderingStressChurnBatch>
        RenderingStressSample::BuildChurnBatchPlan(uint32 churnObjectCount)
    {
        std::vector<RenderingStressChurnBatch> batches;
        if (churnObjectCount == 0)
            return batches;

        const uint32 batchSize = GetDeterministicChurnBatchSize();
        batches.reserve(churnObjectCount / batchSize +
                        (churnObjectCount % batchSize == 0 ? 0u : 1u));
        for (uint32 offset = 0; offset < churnObjectCount;)
        {
            const uint32 count =
                std::min(batchSize, churnObjectCount - offset);
            batches.push_back({offset, count});
            offset += count;
        }
        return batches;
    }

    RenderGPUDrivenMode RenderingStressSample::MapRenderPath(
        SampleRenderPath path) noexcept
    {
        switch (path)
        {
            case SampleRenderPath::Auto: return RenderGPUDrivenMode::Auto;
            case SampleRenderPath::Direct:
                return RenderGPUDrivenMode::ForceDisabled;
            case SampleRenderPath::GPUDriven:
                return RenderGPUDrivenMode::ForceEnabled;
            default: return RenderGPUDrivenMode::Auto;
        }
    }

    bool RenderingStressSample::HasRequiredInstrumentation(
        const SampleRenderDiagnostics& diagnostics,
        SampleRenderPath requestedPath) noexcept
    {
        return HasBaseInstrumentation(diagnostics) &&
               ResolveExecutionPath(diagnostics, requestedPath).has_value();
    }

    bool RenderingStressSample::HasBaseInstrumentation(
        const SampleRenderDiagnostics& diagnostics) noexcept
    {
        return diagnostics.available && diagnostics.sceneWorkAvailable &&
               diagnostics.gpuSceneAvailable &&
               diagnostics.extractionAvailable &&
               diagnostics.engineRenderRuntimeAvailable &&
               diagnostics.acceptedExtractionDiagnosticsAvailable &&
               diagnostics.mutationEvidence.available &&
               diagnostics.mutationEvidence.completedPresentationCount != 0 &&
               diagnostics.renderSceneValuesAvailable &&
               diagnostics.renderSceneFrameSequence != 0;
    }

    bool RenderingStressSample::IsWaitingForRequiredSceneRevision(
        const SampleRenderDiagnostics& diagnostics) noexcept
    {
        return diagnostics.engineRenderRuntimeAvailable &&
               diagnostics.renderSceneRequiredRevision != 0 &&
               diagnostics.renderSceneAppliedRevision <
                   diagnostics.renderSceneRequiredRevision;
    }

    bool RenderingStressSample::ShouldConsumeDiagnosticObservation(
        const RenderingStressDiagnosticsSnapshot& previous,
        const RenderingStressDiagnosticsSnapshot& current,
        bool actionRenderEvidencePending) noexcept
    {
        return actionRenderEvidencePending ||
               previous.sourceFrameSequence != current.sourceFrameSequence;
    }

    RenderingStressWindowEvaluation
        RenderingStressSample::AppendPendingActionDrainObservation(
            const RenderingStressDiagnosticsSnapshot& pinnedAction,
            std::vector<RenderingStressDiagnosticsSnapshot>& pending,
            const RenderingStressDiagnosticsSnapshot& current)
    {
        if (pinnedAction.sourceFrameSequence == 0 ||
            current.sourceFrameSequence <= pinnedAction.sourceFrameSequence)
        {
            return FailWindow(
                "Pending action drain observation was not later than its pinned action frame.");
        }
        if (!IsWithinPresentationPropagationBudget(
                pinnedAction.mutationEvidence,
                current.mutationEvidence,
                GetPendingActionReplayFrameLimit(),
                true))
        {
            return FailWindow(
                "Pending action drain observation exceeded its completed-presentation budget.");
        }
        if (!pending.empty())
        {
            const uint64 lastSourceFrameSequence =
                pending.back().sourceFrameSequence;
            if (current.sourceFrameSequence == lastSourceFrameSequence)
                return PassWindow();
            if (current.sourceFrameSequence < lastSourceFrameSequence)
            {
                return FailWindow(
                    "Pending action drain observations arrived out of completed-frame order.");
            }
        }
        if (pending.size() >= GetPendingActionReplayFrameLimit() + 1u)
        {
            return FailWindow(
                "Pending action drain evidence exceeded its bounded completed-frame capacity.");
        }

        try
        {
            pending.push_back(current);
        }
        catch (...)
        {
            return FailWindow(
                "Pending action drain evidence could not retain a completed-frame snapshot.");
        }
        return PassWindow();
    }

    RenderingStressWindowEvaluation
        RenderingStressSample::NormalizeActionDrainObservation(
            const RenderingStressDiagnosticsSnapshot& finalizedAction,
            const RenderingStressDiagnosticsSnapshot& queued,
            RenderingStressDiagnosticsSnapshot& outNormalized)
    {
        if (queued.sourceFrameSequence <= finalizedAction.sourceFrameSequence)
        {
            return FailWindow(
                "Queued action drain evidence lacks a later completed frame.");
        }
        if (!IsWithinPresentationPropagationBudget(
                finalizedAction.mutationEvidence,
                queued.mutationEvidence,
                GetPendingActionReplayFrameLimit(),
                true))
        {
            return FailWindow(
                "Queued action drain evidence exceeded its completed-presentation budget.");
        }

        outNormalized = queued;
        return PassWindow();
    }

    RenderingStressDiagnosticsSnapshot
        RenderingStressSample::CaptureDiagnostics(
            const SampleRenderDiagnostics& diagnostics) noexcept
    {
        return {
            .available = diagnostics.available,
            .sceneWorkAvailable = diagnostics.sceneWorkAvailable,
            .gpuSceneAvailable = diagnostics.gpuSceneAvailable,
            .extractionAvailable = diagnostics.extractionAvailable,
            .sourceFrameSequence = diagnostics.renderSceneFrameSequence,
            .renderSceneAppliedRevision = diagnostics.renderSceneAppliedRevision,
            .renderSceneRequiredRevision =
                diagnostics.renderSceneRequiredRevision,
            .mutationEvidence = diagnostics.mutationEvidence,
            .extractionFullScanCount = diagnostics.extractionFullScanCount,
            .extractionChangeFeedChangeCount =
                diagnostics.extractionChangeFeedChangeCount,
            .extractionActorRebuildCount =
                diagnostics.extractionActorRebuildCount,
            .extractionProxyVisitCount = diagnostics.extractionProxyVisitCount,
            .extractionComponentVisitCount =
                diagnostics.extractionComponentVisitCount,
            .extractionFeatureProviderVisitCount =
                diagnostics.extractionFeatureProviderVisitCount,
            .extractionContinuityLost = diagnostics.extractionContinuityLost,
            .acceptedExtractionDiagnosticsAvailable =
                diagnostics.acceptedExtractionDiagnosticsAvailable,
            .acceptedExtractionPublicationCount =
                diagnostics.acceptedExtractionPublicationCount,
            .acceptedExtractionLastSourceFrameSequence =
                diagnostics.acceptedExtractionLastSourceFrameSequence,
            .acceptedExtractionLastSceneRevision =
                diagnostics.acceptedExtractionLastSceneRevision,
            .acceptedExtractionCumulativeFullScanCount =
                diagnostics.acceptedExtractionCumulativeFullScanCount,
            .acceptedExtractionCumulativeChangeFeedChangeCount =
                diagnostics
                    .acceptedExtractionCumulativeChangeFeedChangeCount,
            .acceptedExtractionCumulativeActorRebuildCount =
                diagnostics.acceptedExtractionCumulativeActorRebuildCount,
            .acceptedExtractionCumulativeProxyVisitCount =
                diagnostics.acceptedExtractionCumulativeProxyVisitCount,
            .acceptedExtractionCumulativeComponentVisitCount =
                diagnostics
                    .acceptedExtractionCumulativeComponentVisitCount,
            .acceptedExtractionCumulativeFeatureProviderVisitCount =
                diagnostics
                    .acceptedExtractionCumulativeFeatureProviderVisitCount,
            .acceptedExtractionContinuityLossCount =
                diagnostics.acceptedExtractionContinuityLossCount,
            .sceneFullRebuildCount = diagnostics.sceneFullRebuildCount,
            .sceneIncrementalUpdateCount = diagnostics.sceneIncrementalUpdateCount,
            .sceneStaticReuseCount = diagnostics.sceneStaticReuseCount,
            .sceneLastRebuiltObjectCount = diagnostics.sceneLastRebuiltObjectCount,
            .sceneLastRemovedObjectCount = diagnostics.sceneLastRemovedObjectCount,
            .drawPacketBuildCount = diagnostics.drawPacketBuildCount,
            .drawPacketInvalidationCount = diagnostics.drawPacketInvalidationCount,
            .drawPacketEntryCount = diagnostics.drawPacketEntryCount,
            .renderSceneObjectCount = diagnostics.renderSceneObjectCount,
            .gpuScenePublishedObjectCount =
                diagnostics.gpuScenePublishedObjectCount,
            .gpuSceneAddCount = diagnostics.gpuSceneAddCount,
            .gpuSceneUpdateCount = diagnostics.gpuSceneUpdateCount,
            .gpuSceneRemoveCount = diagnostics.gpuSceneRemoveCount,
            .gpuSceneNoOpCount = diagnostics.gpuSceneNoOpCount,
            .gpuSceneFrameUploadBytes = diagnostics.gpuSceneFrameUploadBytes,
            .gpuSceneFullUpload = diagnostics.gpuSceneFullUpload,
            .gpuDrivenInstanceUploadBytes = diagnostics.gpuDrivenInstanceUploadBytes,
            .gpuDrivenCandidateUploadBytes = diagnostics.gpuDrivenCandidateUploadBytes,
            .gpuDrivenActiveRowUploadBytes =
                diagnostics.gpuDrivenActiveRowUploadBytes,
            .gpuDrivenActiveRowCount = diagnostics.gpuDrivenActiveRowCount,
            .gpuDrivenActiveRowHighWatermark =
                diagnostics.gpuDrivenActiveRowHighWatermark,
            .gpuDrivenInstancePatchedRowCount =
                diagnostics.gpuDrivenInstancePatchedRowCount,
            .gpuDrivenCandidatePatchedRowCount =
                diagnostics.gpuDrivenCandidatePatchedRowCount,
            .gpuDrivenActiveRowPatchedRowCount =
                diagnostics.gpuDrivenActiveRowPatchedRowCount,
            .gpuDrivenInstanceFullMaterializationCount =
                diagnostics.gpuDrivenInstanceFullMaterializationCount,
            .gpuDrivenCandidateFullMaterializationCount =
                diagnostics.gpuDrivenCandidateFullMaterializationCount,
            .gpuDrivenActiveRowFullMaterializationCount =
                diagnostics.gpuDrivenActiveRowFullMaterializationCount,
            .gpuDrivenContinuityFullMaterializationCount =
                diagnostics.gpuDrivenContinuityFullMaterializationCount,
            .gpuDrivenCapacityFullMaterializationCount =
                diagnostics.gpuDrivenCapacityFullMaterializationCount,
            .directRasterInstanceUploadBytes =
                diagnostics.directRasterInstanceUploadBytes,
            .directRasterInstanceIndexUploadBytes =
                diagnostics.directRasterInstanceIndexUploadBytes,
            .directRasterInstancePatchedRowCount =
                diagnostics.directRasterInstancePatchedRowCount,
            .directRasterIndexPatchedRowCount =
                diagnostics.directRasterIndexPatchedRowCount,
            .directRasterActiveInstanceCount =
                diagnostics.directRasterActiveInstanceCount,
            .directRasterActiveInstanceCapacity =
                diagnostics.directRasterActiveInstanceCapacity,
            .directRasterInstanceFullMaterializationCount =
                diagnostics.directRasterInstanceFullMaterializationCount,
            .directRasterIndexFullMaterializationCount =
                diagnostics.directRasterIndexFullMaterializationCount};
    }

    bool RenderingStressSample::HasCompleteInitialPopulation(
        const RenderingStressDiagnosticsSnapshot& snapshot,
        uint32 expectedObjectCount) noexcept
    {
        return expectedObjectCount != 0 && snapshot.available &&
               snapshot.sceneWorkAvailable && snapshot.gpuSceneAvailable &&
               snapshot.acceptedExtractionDiagnosticsAvailable &&
               snapshot.acceptedExtractionPublicationCount != 0 &&
               snapshot.sourceFrameSequence != 0 &&
               snapshot.renderSceneObjectCount == expectedObjectCount &&
               snapshot.gpuScenePublishedObjectCount == expectedObjectCount &&
               snapshot.drawPacketEntryCount == expectedObjectCount;
    }

    bool RenderingStressSample::AreExecutionStreamsQuiescent(
        const RenderingStressDiagnosticsSnapshot& snapshot) noexcept
    {
        return snapshot.gpuDrivenInstanceUploadBytes == 0 &&
               snapshot.gpuDrivenCandidateUploadBytes == 0 &&
               snapshot.gpuDrivenActiveRowUploadBytes == 0 &&
               snapshot.gpuDrivenInstancePatchedRowCount == 0 &&
               snapshot.gpuDrivenCandidatePatchedRowCount == 0 &&
               snapshot.gpuDrivenActiveRowPatchedRowCount == 0 &&
               snapshot.directRasterInstanceUploadBytes == 0 &&
               snapshot.directRasterInstanceIndexUploadBytes == 0 &&
               snapshot.directRasterInstancePatchedRowCount == 0 &&
               snapshot.directRasterIndexPatchedRowCount == 0 &&
               snapshot.directRasterInstanceFullMaterializationCount == 0 &&
               snapshot.directRasterIndexFullMaterializationCount == 0;
    }

    RenderingStressWindowEvaluation RenderingStressSample::EvaluateStaticWindow(
        const RenderingStressDiagnosticsSnapshot& before,
        const RenderingStressDiagnosticsSnapshot& after)
    {
        if (!before.available || !after.available ||
            !before.sceneWorkAvailable || !after.sceneWorkAvailable ||
            !before.gpuSceneAvailable || !after.gpuSceneAvailable ||
            !before.acceptedExtractionDiagnosticsAvailable ||
            !after.acceptedExtractionDiagnosticsAvailable)
        {
            return FailWindow("Static-window diagnostics are unavailable.");
        }
        if (!IsCounterUnchanged(before.sceneFullRebuildCount,
                                after.sceneFullRebuildCount) ||
            !IsCounterUnchanged(before.sceneIncrementalUpdateCount,
                                after.sceneIncrementalUpdateCount) ||
            !IsCounterUnchanged(before.drawPacketBuildCount,
                                after.drawPacketBuildCount) ||
            !IsCounterUnchanged(before.drawPacketInvalidationCount,
                                after.drawPacketInvalidationCount))
        {
            return FailWindow(
                "Static window advanced retained Scene or draw-packet work.");
        }
        if (after.sceneStaticReuseCount <= before.sceneStaticReuseCount ||
            after.sceneLastRebuiltObjectCount != 0 ||
            after.sceneLastRemovedObjectCount != 0)
        {
            return FailWindow(
                "Static window did not report pure retained Scene reuse.");
        }
        const RenderingStressWindowEvaluation acceptedExtraction =
            EvaluateAcceptedExtractionStaticWindow(before, after);
        if (!acceptedExtraction.passed)
        {
            return acceptedExtraction;
        }
        if (after.gpuSceneAddCount != 0 || after.gpuSceneUpdateCount != 0 ||
            after.gpuSceneRemoveCount != 0 || after.gpuSceneNoOpCount != 0 ||
            after.gpuSceneFrameUploadBytes != 0 || after.gpuSceneFullUpload)
        {
            return FailWindow(
                "Static window changed or uploaded GPU-scene rows.");
        }
        if (after.gpuDrivenInstancePatchedRowCount != 0 ||
            after.gpuDrivenCandidatePatchedRowCount != 0 ||
            after.gpuDrivenActiveRowPatchedRowCount != 0)
        {
            return FailWindow(
                "Static window patched GPU-driven instance, candidate, or active-row streams.");
        }
        if (!HasDirectRasterCapacity(after) ||
            after.directRasterInstancePatchedRowCount != 0 ||
            after.directRasterIndexPatchedRowCount != 0 ||
            !HasNoExecutionFullMaterializationGrowth(before, after))
        {
            return FailWindow(
                "Static window patched, fully materialized, or overran an execution stream.");
        }
        if (!AreExecutionStreamsQuiescent(after))
        {
            return FailWindow(
                "Static window uploaded a full GPU-driven or Direct raster instance stream.");
        }
        return PassWindow();
    }

    RenderingStressWindowEvaluation
        RenderingStressSample::EvaluateInitialStaticWindow(
            const RenderingStressDiagnosticsSnapshot& before,
            const RenderingStressDiagnosticsSnapshot& after,
            uint32 expectedObjectCount,
            bool temporalSettleAlreadyObserved)
    {
        if (expectedObjectCount == 0)
        {
            return FailWindow(
                "Initial static window has no expected persistent population.");
        }

        const SampleRenderMutationEvidenceDiagnostics& baselineEvidence =
            before.mutationEvidence;
        const SampleRenderMutationEvidenceDiagnostics& currentEvidence =
            after.mutationEvidence;
        if (!baselineEvidence.available || !currentEvidence.available ||
            baselineEvidence.saturated || currentEvidence.saturated ||
            baselineEvidence.evidenceEpoch == 0 ||
            currentEvidence.evidenceEpoch == 0 ||
            baselineEvidence.evidenceEpoch != currentEvidence.evidenceEpoch ||
            after.sourceFrameSequence <= before.sourceFrameSequence ||
            baselineEvidence.completedFrameSequence !=
                before.sourceFrameSequence ||
            currentEvidence.completedFrameSequence !=
                after.sourceFrameSequence ||
            currentEvidence.completedPresentationCount <=
                baselineEvidence.completedPresentationCount ||
            HasMutationEvidenceCounterRegression(baselineEvidence,
                                                  currentEvidence) ||
            !IsCounterUnchanged(baselineEvidence.sceneFullRebuildCount,
                                currentEvidence.sceneFullRebuildCount) ||
            !IsCounterUnchanged(baselineEvidence.sceneIncrementalCommitCount,
                                currentEvidence.sceneIncrementalCommitCount) ||
            !IsCounterUnchanged(baselineEvidence.sceneRebuiltObjectCount,
                                currentEvidence.sceneRebuiltObjectCount) ||
            !IsCounterUnchanged(baselineEvidence.sceneRemovedObjectCount,
                                currentEvidence.sceneRemovedObjectCount))
        {
            return FailWindow(
                "Initial static window lacks valid completion mutation provenance.");
        }

        RenderingStressDiagnosticsSnapshot structural = after;
        NormalizeExecutionStreamWarmupDiagnostics(structural, before);
        const RenderingStressWindowEvaluation strict =
            EvaluateStaticWindow(before, structural);
        if (strict.passed)
            return strict;

        if (!before.available || !after.available ||
            !before.sceneWorkAvailable || !after.sceneWorkAvailable ||
            !before.gpuSceneAvailable || !after.gpuSceneAvailable ||
            !HasCompleteInitialPopulation(before, expectedObjectCount) ||
            !HasCompleteInitialPopulation(after, expectedObjectCount))
        {
            return FailWindow(
                "Initial temporal settle diagnostics or persistent population are unavailable.");
        }
        if (!IsCounterUnchanged(before.sceneFullRebuildCount,
                                after.sceneFullRebuildCount) ||
            !IsCounterUnchanged(before.sceneIncrementalUpdateCount,
                                after.sceneIncrementalUpdateCount) ||
            !IsCounterUnchanged(before.drawPacketBuildCount,
                                after.drawPacketBuildCount) ||
            !IsCounterUnchanged(before.drawPacketInvalidationCount,
                                after.drawPacketInvalidationCount) ||
            !IsCounterUnchanged(before.sceneStaticReuseCount,
                                after.sceneStaticReuseCount) ||
            after.sceneLastRebuiltObjectCount != 0 ||
            after.sceneLastRemovedObjectCount != 0)
        {
            return FailWindow(
                "Initial temporal settle changed retained Scene or draw-packet work.");
        }
        const RenderingStressWindowEvaluation acceptedExtraction =
            EvaluateAcceptedExtractionStaticWindow(before, after);
        if (!acceptedExtraction.passed)
            return acceptedExtraction;

        if (after.gpuSceneAddCount != 0 ||
            after.gpuSceneUpdateCount != expectedObjectCount ||
            after.gpuSceneRemoveCount != 0 || after.gpuSceneNoOpCount != 0 ||
            after.gpuSceneFrameUploadBytes == 0 || after.gpuSceneFullUpload)
        {
            return FailWindow(
                "Initial temporal settle did not report its exact GPU-scene update payload.");
        }

        bool hasExactSubmittedRows = true;
        for (uint32 table = 0;
             table < GPU_SCENE_DIAGNOSTICS_TABLE_COUNT;
             ++table)
        {
            if (currentEvidence.gpuSceneSubmittedUploadedRowCount[table] -
                    baselineEvidence.gpuSceneSubmittedUploadedRowCount[table] !=
                expectedObjectCount)
            {
                hasExactSubmittedRows = false;
                break;
            }
        }
        if (currentEvidence.gpuSceneFullPublicationCount !=
                baselineEvidence.gpuSceneFullPublicationCount ||
            currentEvidence.gpuSceneIncrementalPublicationCount -
                    baselineEvidence.gpuSceneIncrementalPublicationCount !=
                1u ||
            currentEvidence.gpuSceneIdentityOnlyPublicationCount !=
                baselineEvidence.gpuSceneIdentityOnlyPublicationCount ||
            currentEvidence.gpuSceneMaterializedObjectCount -
                    baselineEvidence.gpuSceneMaterializedObjectCount !=
                expectedObjectCount ||
            currentEvidence.gpuSceneAddCount !=
                baselineEvidence.gpuSceneAddCount ||
            currentEvidence.gpuSceneUpdateCount -
                    baselineEvidence.gpuSceneUpdateCount !=
                expectedObjectCount ||
            currentEvidence.gpuSceneRemoveCount !=
                baselineEvidence.gpuSceneRemoveCount ||
            currentEvidence.gpuSceneNoOpCount !=
                baselineEvidence.gpuSceneNoOpCount ||
            currentEvidence.gpuSceneSubmittedUploadCount -
                    baselineEvidence.gpuSceneSubmittedUploadCount !=
                1u ||
            currentEvidence.gpuSceneFullUploadCount !=
                baselineEvidence.gpuSceneFullUploadCount ||
            currentEvidence.gpuSceneUploadBytes <=
                baselineEvidence.gpuSceneUploadBytes ||
            currentEvidence.gpuSceneUploadRangeCount <=
                baselineEvidence.gpuSceneUploadRangeCount ||
            !hasExactSubmittedRows)
        {
            return FailWindow(
                "Initial temporal settle lacks exact incremental GPU-scene mutation evidence.");
        }
        if (temporalSettleAlreadyObserved)
        {
            return FailWindow(
                "Initial static window reported more than one temporal GPU-scene settle.");
        }
        return PendingWindow(
            "Initial static window observed its exact temporal GPU-scene settle.");
    }

    bool RenderingStressSample::ObserveQuiescentStaticWindow(
        SampleAssessmentChannel& assessment,
        const RenderingStressDiagnosticsSnapshot& current,
        bool allowInitialSlotWarmup)
    {
        if (!m_windowBaseline.has_value())
        {
            Fail(assessment,
                 StaticReuseInvariant.code,
                 "The static window has no completed-frame baseline.");
            return false;
        }

        if (!m_initialExecutionOwnerCountsCaptured ||
            !HasExpectedExecutionPopulation(current,
                                            m_workload.objectCount,
                                            m_gpuDrivenInitialOwnerCount,
                                            m_directRasterInitialOwnerCount))
        {
            Fail(assessment,
                 StaticReuseInvariant.code,
                 "The static window changed its exact initial execution-stream population.");
            return false;
        }

        const RenderingStressWindowEvaluation stableStructure =
            allowInitialSlotWarmup
                ? EvaluateInitialStaticWindow(*m_windowBaseline,
                                              current,
                                              m_workload.objectCount,
                                              m_initialTemporalSettleObserved)
                : EvaluateStaticWindow(*m_windowBaseline, current);
        if (!stableStructure.passed)
        {
            if (allowInitialSlotWarmup && stableStructure.pending)
            {
                m_initialTemporalSettleObserved = true;
                ++m_staticStreamWarmupFrameCount;
                if (m_staticStreamWarmupFrameCount >
                    MaxStaticStreamWarmupFrames)
                {
                    Fail(assessment,
                         StaticReuseInvariant.code,
                         "Initial temporal GPU-scene settle did not quiesce within " +
                             std::to_string(MaxStaticStreamWarmupFrames) +
                             " distinct completed frames.");
                }
                m_windowBaseline = current;
                return false;
            }
            Fail(assessment,
                 StaticReuseInvariant.code,
                 stableStructure.detail);
            return false;
        }

        if (allowInitialSlotWarmup && !AreExecutionStreamsQuiescent(current))
        {
            ++m_staticStreamWarmupFrameCount;
            if (m_staticStreamWarmupFrameCount > MaxStaticStreamWarmupFrames)
            {
                Fail(assessment,
                     StaticReuseInvariant.code,
                     "Completion-safe execution streams did not quiesce within " +
                         std::to_string(MaxStaticStreamWarmupFrames) +
                         " distinct completed frames.");
            }
            // Only initial physical-slot materialization is allowed to roll
            // its baseline. Every later static window is strict.
            m_windowBaseline = current;
            return false;
        }

        m_staticStreamWarmupFrameCount = 0;
        return true;
    }

    bool RenderingStressSample::ObserveDirtyDrain(
        SampleAssessmentChannel& assessment,
        const RenderingStressDiagnosticsSnapshot& current)
    {
        if (!m_windowBaseline.has_value() ||
            !m_dirtyActionBaseline.has_value() ||
            m_actionTargetSceneRevision == 0)
        {
            Fail(assessment,
                 DirtyDeltaInvariant.code,
                 "The dirty drain lost its pinned completed action snapshot.");
            return false;
        }

        const RenderingStressDiagnosticsSnapshot& actionSnapshot =
            *m_windowBaseline;
        const RenderingStressWindowEvaluation cumulative =
            EvaluateDirtyMutationEvidenceWindow(
                *m_dirtyActionBaseline,
                current,
                m_actionTargetSceneRevision,
                m_workload.dirtyObjectCount,
                m_workload.objectCount,
                m_gpuDrivenInitialOwnerCount,
                m_directRasterInitialOwnerCount);
        if (!cumulative.passed)
        {
            Fail(assessment, DirtyDeltaInvariant.code, cumulative.detail);
            return false;
        }
        const bool currentIsTemporalSettle =
            current.gpuSceneUpdateCount != 0 ||
            current.gpuSceneFrameUploadBytes != 0;
        const bool cumulativeTemporalSettleObserved =
            HasMutationTemporalSettle(*m_dirtyActionBaseline,
                                      current,
                                      MutationActionKind::Dirty,
                                      m_workload.dirtyObjectCount);
        const bool temporalSettleObservedBeforeCurrent =
            m_dirtyTemporalSettleObserved ||
            (!currentIsTemporalSettle && cumulativeTemporalSettleObserved);
        const bool strictStatic =
            EvaluateStaticWindow(actionSnapshot, current).passed;
        if (!IsWithinPresentationPropagationBudget(
                actionSnapshot.mutationEvidence,
                current.mutationEvidence,
                GetDirtyDrainCompletedFrameLimit(),
                strictStatic))
        {
            const uint64 completedPresentationDelta =
                current.mutationEvidence.completedPresentationCount >=
                        actionSnapshot.mutationEvidence.completedPresentationCount
                    ? current.mutationEvidence.completedPresentationCount -
                          actionSnapshot.mutationEvidence.completedPresentationCount
                    : 0u;
            Fail(assessment,
                 DirtyDeltaInvariant.code,
                 "Dirty propagation exceeded its completed-presentation budget: "
                 "action-presentation=" +
                     std::to_string(actionSnapshot.mutationEvidence
                                        .completedPresentationCount) +
                     ", current-presentation=" +
                     std::to_string(current.mutationEvidence
                                        .completedPresentationCount) +
                     ", delta=" + std::to_string(completedPresentationDelta) +
                     ", non-strict-limit=" +
                     std::to_string(GetDirtyDrainCompletedFrameLimit()) +
                     ", terminal-strict=" +
                     std::string(strictStatic ? "true" : "false") + ".");
            return false;
        }
        const RenderingStressWindowEvaluation drain =
            EvaluateDirtyDrainWindow(actionSnapshot,
                                     current,
                                     m_workload.dirtyObjectCount,
                                     m_workload.objectCount,
                                     m_gpuDrivenInitialOwnerCount,
                                     m_directRasterInitialOwnerCount,
                                     temporalSettleObservedBeforeCurrent);
        if (!drain.passed)
        {
            Fail(assessment, DirtyDeltaInvariant.code, drain.detail);
            return false;
        }
        m_dirtyTemporalSettleObserved = m_dirtyTemporalSettleObserved ||
            cumulativeTemporalSettleObserved;

        if (strictStatic)
        {
            if (!m_dirtyTemporalSettleObserved)
            {
                Fail(assessment,
                     DirtyDeltaInvariant.code,
                     "Dirty drain reached a strict static frame without its exact temporal GPU-scene settle.");
                return false;
            }
            m_dirtyTemporalSettleObserved = false;
            m_dirtyDrainCompletedFrameCount = 0;
            PublishSnapshot(assessment,
                            AssessmentCheckpoints::ScenarioStable,
                            current);
            MarkInvariant(assessment,
                          StaticReuseInvariant,
                          AssessmentCheckpoints::ScenarioStable);
            m_phase = RenderingStressPhase::ChurnDestroy;
            m_windowBaseline = current;
            m_dirtyActionBaseline.reset();
            m_actionTargetSceneRevision = 0;
            return true;
        }

        ++m_dirtyDrainCompletedFrameCount;
        if (m_dirtyDrainCompletedFrameCount >
            GetDirtyDrainCompletedFrameLimit())
        {
            Fail(assessment,
                 DirtyDeltaInvariant.code,
                 "Dirty completion-safe slot propagation did not reach a strict static frame within " +
                     std::to_string(GetDirtyDrainCompletedFrameLimit()) +
                     " subsequent distinct completed frames.");
        }
        return false;
    }

    bool RenderingStressSample::QueuePendingActionDrainObservation(
        SampleAssessmentChannel& assessment,
        const RenderingStressDiagnosticsSnapshot& current)
    {
        const RenderingStressDiagnosticsSnapshot* pinnedAction = nullptr;
        AssessmentCode invariantCode = DirtyDeltaInvariant.code;
        if (m_churnBatchActionSnapshot.has_value() &&
            (m_phase == RenderingStressPhase::ChurnDestroy ||
             m_phase == RenderingStressPhase::ChurnRecreate))
        {
            pinnedAction = &*m_churnBatchActionSnapshot;
            invariantCode = ChurnDeltaInvariant.code;
        }
        else
        {
            return true;
        }

        if (current.sourceFrameSequence <= pinnedAction->sourceFrameSequence)
            return true;

        const RenderingStressWindowEvaluation appended =
            AppendPendingActionDrainObservation(*pinnedAction,
                                                m_pendingActionDrainSnapshots,
                                                current);
        if (appended.passed)
            return true;

        Fail(assessment, invariantCode, appended.detail);
        return false;
    }

    bool RenderingStressSample::ReplayPendingDirtyDrain(
        SampleAssessmentChannel& assessment)
    {
        if (!m_windowBaseline.has_value())
        {
            Fail(assessment,
                 DirtyDeltaInvariant.code,
                 "Dirty action drain replay lost its finalized action snapshot.");
            return false;
        }

        const RenderingStressDiagnosticsSnapshot actionSnapshot =
            *m_windowBaseline;
        for (const RenderingStressDiagnosticsSnapshot& queued :
             m_pendingActionDrainSnapshots)
        {
            RenderingStressDiagnosticsSnapshot normalized;
            const RenderingStressWindowEvaluation normalization =
                NormalizeActionDrainObservation(actionSnapshot,
                                                queued,
                                                normalized);
            if (!normalization.passed)
            {
                Fail(assessment, DirtyDeltaInvariant.code, normalization.detail);
                return false;
            }

            if (m_phase == RenderingStressPhase::StableAfterDirty)
            {
                static_cast<void>(ObserveDirtyDrain(assessment, normalized));
                if (m_phase == RenderingStressPhase::Failed)
                    return false;
                continue;
            }

            const RenderingStressWindowEvaluation strict =
                EvaluateStaticWindow(actionSnapshot, normalized);
            if (!strict.passed)
            {
                Fail(assessment,
                     DirtyDeltaInvariant.code,
                     "Queued completed frame after dirty drain reached strict reuse performed work: " +
                         strict.detail);
                return false;
            }
        }

        m_pendingActionDrainSnapshots.clear();
        return true;
    }

    bool RenderingStressSample::ReplayPendingChurnDrain(
        SampleAssessmentChannel& assessment)
    {
        if (!m_churnDrainBaseline.has_value())
        {
            Fail(assessment,
                 ChurnDeltaInvariant.code,
                 "Churn action drain replay lost its finalized action snapshot.");
            return false;
        }

        const RenderingStressDiagnosticsSnapshot actionSnapshot =
            *m_churnDrainBaseline;
        for (const RenderingStressDiagnosticsSnapshot& queued :
             m_pendingActionDrainSnapshots)
        {
            RenderingStressDiagnosticsSnapshot normalized;
            const RenderingStressWindowEvaluation normalization =
                NormalizeActionDrainObservation(actionSnapshot,
                                                queued,
                                                normalized);
            if (!normalization.passed)
            {
                Fail(assessment, ChurnDeltaInvariant.code, normalization.detail);
                return false;
            }

            if (m_phase == RenderingStressPhase::ChurnDrain)
            {
                static_cast<void>(ObserveChurnDrain(assessment, normalized));
                if (m_phase == RenderingStressPhase::Failed)
                    return false;
                continue;
            }

            const RenderingStressWindowEvaluation strict =
                EvaluateStaticWindow(actionSnapshot, normalized);
            if (!strict.passed)
            {
                Fail(assessment,
                     ChurnDeltaInvariant.code,
                     "Queued completed frame after churn drain reached strict reuse performed work: " +
                         strict.detail);
                return false;
            }
        }

        m_pendingActionDrainSnapshots.clear();
        return true;
    }

    RenderingStressWindowEvaluation RenderingStressSample::EvaluateDirtyWindow(
        const RenderingStressDiagnosticsSnapshot& before,
        const RenderingStressDiagnosticsSnapshot& after,
        uint32 expectedDirtyCount,
        uint32 expectedObjectCount,
        uint32 expectedGPUDrivenOwnerCount,
        uint32 expectedDirectRasterOwnerCount)
    {
        if (expectedDirtyCount == 0 || expectedObjectCount == 0)
        {
            return FailWindow(
                "Dirty-window expected dirty and object counts must be non-zero.");
        }
        if (!AreExecutionStreamsQuiescent(before))
        {
            return FailWindow(
                "Dirty window began before prior execution-stream propagation drained.");
        }
        if (after.sceneFullRebuildCount != before.sceneFullRebuildCount ||
            after.sceneIncrementalUpdateCount !=
                before.sceneIncrementalUpdateCount + 1u ||
            after.sceneLastRebuiltObjectCount != expectedDirtyCount ||
            after.sceneLastRemovedObjectCount != 0)
        {
            return FailWindow(
                "Dirty window did not produce exactly one incremental retained Scene update.");
        }
        uint64 expectedChangeFeedCount = 0;
        if (!TryMultiplyMutationCount(
                expectedDirtyCount,
                GetExpectedChangeFeedMultiplier(MutationActionKind::Dirty),
                expectedChangeFeedCount))
        {
            return FailWindow("Dirty-window change-feed expectation overflowed.");
        }
        const RenderingStressWindowEvaluation acceptedExtraction =
            EvaluateAcceptedExtractionMutationWindow(before,
                                                     after,
                                                     expectedChangeFeedCount,
                                                     expectedDirtyCount,
                                                     expectedDirtyCount);
        if (!acceptedExtraction.passed)
        {
            return acceptedExtraction;
        }
        if (after.drawPacketBuildCount != before.drawPacketBuildCount ||
            after.drawPacketInvalidationCount !=
                before.drawPacketInvalidationCount)
        {
            return FailWindow(
                "Transform-only dirty work rebuilt or invalidated draw packets.");
        }
        if (after.gpuSceneAddCount != 0 ||
            after.gpuSceneUpdateCount != expectedDirtyCount ||
            after.gpuSceneRemoveCount != 0 || after.gpuSceneNoOpCount != 0 ||
            after.gpuSceneFrameUploadBytes == 0 || after.gpuSceneFullUpload)
        {
            return FailWindow(
                "Dirty window did not upload exactly the requested incremental GPU-scene update set.");
        }
        if (!HasExpectedExecutionPopulation(before,
                                            expectedObjectCount,
                                            expectedGPUDrivenOwnerCount,
                                            expectedDirectRasterOwnerCount) ||
            !HasExpectedExecutionPopulation(after,
                                            expectedObjectCount,
                                            expectedGPUDrivenOwnerCount,
                                            expectedDirectRasterOwnerCount))
        {
            return FailWindow(
                "Dirty window changed the exact initial execution-stream population.");
        }
        if (!HasExactDirtyStableRowPatches(after,
                                           expectedDirtyCount,
                                           expectedObjectCount,
                                           expectedGPUDrivenOwnerCount))
        {
            return FailWindow(
                "Dirty window patched a non-dirty GPU-driven row set or rebuilt active-row topology.");
        }
        if (!HasExactDirtyDirectRasterPatches(after,
                                              expectedDirtyCount,
                                              expectedObjectCount,
                                              expectedDirectRasterOwnerCount))
        {
            return FailWindow(
                "Dirty window did not patch exactly its direct raster owner rows or rebuilt its index stream.");
        }
        if (!HasNoExecutionFullMaterializationGrowth(before, after))
        {
            return FailWindow(
                "Dirty window unexpectedly fully materialized an execution stream.");
        }
        return PassWindow();
    }

    RenderingStressWindowEvaluation
        RenderingStressSample::EvaluateDirtyMutationEvidenceWindow(
            const RenderingStressDiagnosticsSnapshot& before,
            const RenderingStressDiagnosticsSnapshot& after,
            uint64 actionTargetSceneRevision,
            uint32 expectedDirtyCount,
            uint32 expectedObjectCount,
            uint32 expectedGPUDrivenOwnerCount,
            uint32 expectedDirectRasterOwnerCount)
    {
        return EvaluateCumulativeMutationWindow(
            before,
            after,
            actionTargetSceneRevision,
            MutationActionKind::Dirty,
            expectedDirtyCount,
            expectedObjectCount,
            expectedObjectCount,
            expectedGPUDrivenOwnerCount,
            expectedDirectRasterOwnerCount);
    }

    RenderingStressWindowEvaluation
        RenderingStressSample::EvaluateDirtyDrainWindow(
            const RenderingStressDiagnosticsSnapshot& actionSnapshot,
            const RenderingStressDiagnosticsSnapshot& current,
            uint32 expectedDirtyCount,
            uint32 expectedObjectCount,
            uint32 expectedGPUDrivenOwnerCount,
            uint32 expectedDirectRasterOwnerCount,
            bool temporalSettleAlreadyObserved)
    {
        return EvaluateActionDrainWindow(actionSnapshot,
                                         current,
                                         expectedObjectCount,
                                         expectedDirtyCount,
                                         expectedGPUDrivenOwnerCount,
                                         expectedDirectRasterOwnerCount,
                                         true,
                                         temporalSettleAlreadyObserved,
                                         false,
                                         "Dirty drain");
    }

    RenderingStressWindowEvaluation
        RenderingStressSample::EvaluateChurnRemoveWindow(
            const RenderingStressDiagnosticsSnapshot& before,
            const RenderingStressDiagnosticsSnapshot& after,
            uint32 expectedChurnCount,
            uint32 expectedBeforePopulation,
            uint32 expectedAfterPopulation,
            uint32 expectedGPUDrivenOwnerCount,
            uint32 expectedDirectRasterOwnerCount)
    {
        if (expectedChurnCount == 0 || expectedBeforePopulation == 0 ||
            expectedAfterPopulation == 0 ||
            expectedBeforePopulation !=
                expectedAfterPopulation + expectedChurnCount)
        {
            return FailWindow(
                "Churn-remove expected before/after populations are invalid.");
        }
        if (!AreExecutionStreamsQuiescent(before))
        {
            return FailWindow(
                "Churn destroy began before prior execution-stream propagation drained.");
        }
        if (after.sceneFullRebuildCount != before.sceneFullRebuildCount ||
            after.sceneIncrementalUpdateCount !=
                before.sceneIncrementalUpdateCount + 1u ||
            after.sceneLastRebuiltObjectCount != 0 ||
            after.sceneLastRemovedObjectCount != expectedChurnCount)
        {
            return FailWindow(
                "Churn destroy did not produce exactly one incremental removal update.");
        }
        uint64 expectedChangeFeedCount = 0;
        if (!TryMultiplyMutationCount(
                expectedChurnCount,
                GetExpectedChangeFeedMultiplier(MutationActionKind::Remove),
                expectedChangeFeedCount))
        {
            return FailWindow("Churn-remove change-feed expectation overflowed.");
        }
        const RenderingStressWindowEvaluation acceptedExtraction =
            EvaluateAcceptedExtractionMutationWindow(before,
                                                     after,
                                                     expectedChangeFeedCount,
                                                     expectedChurnCount,
                                                     0u);
        if (!acceptedExtraction.passed)
        {
            return acceptedExtraction;
        }
        if (after.gpuSceneAddCount != 0 || after.gpuSceneUpdateCount != 0 ||
            after.gpuSceneRemoveCount != expectedChurnCount ||
            after.gpuSceneNoOpCount != 0 ||
            after.gpuSceneFrameUploadBytes == 0 || after.gpuSceneFullUpload)
        {
            return FailWindow(
                "Churn destroy did not remove exactly the requested GPU-scene rows.");
        }
        if (!HasNoExecutionFullMaterializationGrowth(before, after))
        {
            return FailWindow(
                "Churn destroy unexpectedly fully materialized an execution stream.");
        }
        if (!HasBoundedChurnStableRowPatches(before,
                                             after,
                                             expectedChurnCount,
                                             expectedBeforePopulation,
                                             expectedAfterPopulation,
                                             expectedGPUDrivenOwnerCount))
        {
            return FailWindow(
                "Churn destroy patched more GPU-driven rows than its bounded batch permits.");
        }
        if (!HasBoundedChurnDirectRasterPatches(before,
                                                after,
                                                expectedChurnCount,
                                                expectedBeforePopulation,
                                                expectedAfterPopulation,
                                                expectedDirectRasterOwnerCount))
        {
            return FailWindow(
                "Churn destroy patched or fully materialized more direct raster rows than its bounded batch permits.");
        }
        return PassWindow();
    }

    RenderingStressWindowEvaluation
        RenderingStressSample::EvaluateChurnRemoveMutationEvidenceWindow(
            const RenderingStressDiagnosticsSnapshot& before,
            const RenderingStressDiagnosticsSnapshot& after,
            uint64 actionTargetSceneRevision,
            uint32 expectedChurnCount,
            uint32 expectedBeforePopulation,
            uint32 expectedAfterPopulation,
            uint32 expectedGPUDrivenOwnerCount,
            uint32 expectedDirectRasterOwnerCount)
    {
        return EvaluateCumulativeMutationWindow(
            before,
            after,
            actionTargetSceneRevision,
            MutationActionKind::Remove,
            expectedChurnCount,
            expectedBeforePopulation,
            expectedAfterPopulation,
            expectedGPUDrivenOwnerCount,
            expectedDirectRasterOwnerCount);
    }

    RenderingStressWindowEvaluation RenderingStressSample::EvaluateChurnAddWindow(
        const RenderingStressDiagnosticsSnapshot& before,
        const RenderingStressDiagnosticsSnapshot& after,
        uint32 expectedChurnCount,
        uint32 expectedBeforePopulation,
        uint32 expectedAfterPopulation,
        uint32 expectedGPUDrivenOwnerCount,
        uint32 expectedDirectRasterOwnerCount)
    {
        if (expectedChurnCount == 0 || expectedBeforePopulation == 0 ||
            expectedAfterPopulation == 0 ||
            expectedAfterPopulation !=
                expectedBeforePopulation + expectedChurnCount)
        {
            return FailWindow(
                "Churn-add expected before/after populations are invalid.");
        }
        if (!AreExecutionStreamsQuiescent(before))
        {
            return FailWindow(
                "Churn recreation began before prior execution-stream propagation drained.");
        }
        if (after.sceneFullRebuildCount != before.sceneFullRebuildCount ||
            after.sceneIncrementalUpdateCount !=
                before.sceneIncrementalUpdateCount + 1u ||
            after.sceneLastRebuiltObjectCount != expectedChurnCount ||
            after.sceneLastRemovedObjectCount != 0)
        {
            return FailWindow(
                "Churn recreation did not produce exactly one incremental addition update.");
        }
        uint64 expectedChangeFeedCount = 0;
        if (!TryMultiplyMutationCount(
                expectedChurnCount,
                GetExpectedChangeFeedMultiplier(MutationActionKind::Add),
                expectedChangeFeedCount))
        {
            return FailWindow("Churn-add change-feed expectation overflowed.");
        }
        const RenderingStressWindowEvaluation acceptedExtraction =
            EvaluateAcceptedExtractionMutationWindow(before,
                                                     after,
                                                     expectedChangeFeedCount,
                                                     expectedChurnCount,
                                                     expectedChurnCount);
        if (!acceptedExtraction.passed)
        {
            return acceptedExtraction;
        }
        if (after.gpuSceneAddCount != expectedChurnCount ||
            after.gpuSceneUpdateCount != 0 || after.gpuSceneRemoveCount != 0 ||
            after.gpuSceneNoOpCount != 0 ||
            after.gpuSceneFrameUploadBytes == 0 || after.gpuSceneFullUpload)
        {
            return FailWindow(
                "Churn recreation did not add exactly the requested GPU-scene rows.");
        }
        if (!HasNoExecutionFullMaterializationGrowth(before, after))
        {
            return FailWindow(
                "Churn recreation unexpectedly fully materialized an execution stream.");
        }
        if (!HasBoundedChurnStableRowPatches(before,
                                             after,
                                             expectedChurnCount,
                                             expectedBeforePopulation,
                                             expectedAfterPopulation,
                                             expectedGPUDrivenOwnerCount))
        {
            return FailWindow(
                "Churn recreation patched more GPU-driven rows than its bounded batch permits.");
        }
        if (!HasBoundedChurnDirectRasterPatches(before,
                                                after,
                                                expectedChurnCount,
                                                expectedBeforePopulation,
                                                expectedAfterPopulation,
                                                expectedDirectRasterOwnerCount))
        {
            return FailWindow(
                "Churn recreation patched or fully materialized more direct raster rows than its bounded batch permits.");
        }
        return PassWindow();
    }

    RenderingStressWindowEvaluation
        RenderingStressSample::EvaluateChurnAddMutationEvidenceWindow(
            const RenderingStressDiagnosticsSnapshot& before,
            const RenderingStressDiagnosticsSnapshot& after,
            uint64 actionTargetSceneRevision,
            uint32 expectedChurnCount,
            uint32 expectedBeforePopulation,
            uint32 expectedAfterPopulation,
            uint32 expectedGPUDrivenOwnerCount,
            uint32 expectedDirectRasterOwnerCount)
    {
        return EvaluateCumulativeMutationWindow(
            before,
            after,
            actionTargetSceneRevision,
            MutationActionKind::Add,
            expectedChurnCount,
            expectedBeforePopulation,
            expectedAfterPopulation,
            expectedGPUDrivenOwnerCount,
            expectedDirectRasterOwnerCount);
    }

    RenderingStressWindowEvaluation
        RenderingStressSample::EvaluateChurnDrainWindow(
            const RenderingStressDiagnosticsSnapshot& actionSnapshot,
            const RenderingStressDiagnosticsSnapshot& current,
            uint32 expectedPopulation,
            uint32 expectedBatchCount,
            uint32 expectedGPUDrivenOwnerCount,
            uint32 expectedDirectRasterOwnerCount,
            bool allowTemporalSettle,
            bool temporalSettleAlreadyObserved)
    {
        return EvaluateActionDrainWindow(actionSnapshot,
                                         current,
                                         expectedPopulation,
                                         expectedBatchCount,
                                         expectedGPUDrivenOwnerCount,
                                         expectedDirectRasterOwnerCount,
                                         allowTemporalSettle,
                                         temporalSettleAlreadyObserved,
                                         true,
                                         "Churn drain");
    }

    bool RenderingStressSample::PrepareSharedAsset(SampleContext& context)
    {
        if (!m_sharedLoadQueued)
        {
            std::string error;
            if (!context.models.Request(m_sharedModelPath,
                                        m_sharedModel,
                                        error))
            {
                Fail(context.assessment,
                     SharedIdentityInvariant.code,
                     "Rendering-stress could not queue its shared model: " +
                         error);
                return false;
            }
            m_sharedLoadQueued = true;
            return false;
        }

        const ResourceSceneAdapters::EcsSceneAssetLoadStatus status =
            context.models.UpdateReadiness(m_sharedModel);
        if (IsEcsModelFailure(status))
        {
            Fail(context.assessment,
                 SharedIdentityInvariant.code,
                 status.diagnostic.empty()
                     ? "The shared model request failed before workload creation."
                     : status.diagnostic);
            return false;
        }
        if (!m_sharedModel.IsFullyResident())
            return false;
        if (!HasAuthorizedTextureStreaming(status))
        {
            Fail(context.assessment,
                 SharedIdentityInvariant.code,
                 "The fully resident shared model has no exact authorized texture-streaming receipt.");
            return false;
        }
        const SceneECS::SceneEntityRef rootEntity =
            m_sharedModel.GetRootEntityRef();
        if (!rootEntity.IsValid() ||
            rootEntity.sceneRuntimeId != context.scene.GetSceneRuntimeId() ||
            !context.scene.GetEntityRef(rootEntity.entity).IsValid() ||
            !status.modelMetadata.HasPublishedSource() ||
            status.modelMetadata.meshAssetIds.empty() ||
            status.modelMetadata.materialAssetIds.empty() ||
            status.modelMetadata.materials.empty())
        {
            Fail(context.assessment,
                 SharedIdentityInvariant.code,
                 "The fully resident shared model has no ECS root, mesh, or material metadata.");
            return false;
        }

        m_sharedMeshAssetId = status.modelMetadata.meshAssetIds.front();
        m_sharedMaterialAssetId = status.modelMetadata.materialAssetIds.front();
        if (!m_sharedMeshAssetId.IsValid() || !m_sharedMaterialAssetId.IsValid() ||
            status.modelMetadata.materials.front().materialAssetId !=
                m_sharedMaterialAssetId)
        {
            Fail(context.assessment,
                 SharedIdentityInvariant.code,
                 "The shared model published invalid mesh or material identities.");
            return false;
        }

        bool foundSharedMeshBounds = false;
        for (const ECS::EntityHandle member : status.members)
        {
            const SceneECS::Mesh* mesh =
                context.scene.GetRegistry().TryGet<SceneECS::Mesh>(member);
            const SceneECS::Bounds* bounds =
                context.scene.GetRegistry().TryGet<SceneECS::Bounds>(member);
            const SceneECS::MaterialSlots* materialSlots =
                context.scene.GetRegistry().TryGet<SceneECS::MaterialSlots>(member);
            if (mesh != nullptr && bounds != nullptr && materialSlots != nullptr &&
                mesh->meshAssetId == m_sharedMeshAssetId)
            {
                if (mesh->submeshCount != materialSlots->count)
                {
                    Fail(context.assessment,
                         SharedIdentityInvariant.code,
                         "The shared mesh ECS material-slot count does not match its submesh count.");
                    return false;
                }
                m_sharedMeshBounds = *bounds;
                m_sharedMaterialSlots = *materialSlots;
                foundSharedMeshBounds = true;
                break;
            }
        }
        if (!foundSharedMeshBounds ||
            m_sharedMaterialSlots.count == 0 ||
            m_sharedMaterialSlots.count > SceneECS::MaterialSlots::MaxSlotCount)
        {
            Fail(context.assessment,
                 SharedIdentityInvariant.code,
                 "The shared mesh did not publish ECS bounds and material slots for workload placement.");
            return false;
        }
        for (uint32 slot = 0; slot < m_sharedMaterialSlots.count; ++slot)
        {
            const AssetId materialAssetId =
                m_sharedMaterialSlots.values[slot].materialAssetId;
            if (!materialAssetId.IsValid() ||
                std::find(status.modelMetadata.materialAssetIds.begin(),
                          status.modelMetadata.materialAssetIds.end(),
                          materialAssetId) == status.modelMetadata.materialAssetIds.end())
            {
                Fail(context.assessment,
                     SharedIdentityInvariant.code,
                     "The shared mesh material slots are not backed by immutable model metadata.");
                return false;
            }
        }

        bool hidSourceRenderable = false;
        for (const ECS::EntityHandle member : status.members)
        {
            const SceneECS::Visibility* visibility =
                context.scene.GetRegistry().TryGet<SceneECS::Visibility>(member);
            if (visibility == nullptr)
                continue;

            SceneECS::Visibility hiddenVisibility = *visibility;
            hiddenVisibility.visible = false;
            if (!context.scene.SetFragment(member, hiddenVisibility))
            {
                Fail(context.assessment,
                     SharedIdentityInvariant.code,
                     "The shared model source renderable could not be hidden through ECS.");
                return false;
            }
            hidSourceRenderable = true;
        }
        if (!hidSourceRenderable)
        {
            Fail(context.assessment,
                 SharedIdentityInvariant.code,
                 "The shared model did not publish any ECS renderable to hide.");
            return false;
        }

        // Keep the shared mesh/material identity while making the product
        // frame spatially informative.  One deterministic, non-shadowing
        // point light exercises the engine-owned local-light path and creates
        // real shading variation across every workload scale.
        const float32 workloadSpan = GetWorkloadSpan(m_workload.objectCount);
        SceneECS::RuntimeEntityDesc lightDesc;
        lightDesc.localTransform.translation = Vec3(-workloadSpan * 0.35f,
                                                    workloadSpan * 0.35f,
                                                    workloadSpan * 0.65f);
        const SceneECS::Light light{
            .type = SceneECS::LightType::Point,
            .color = Vec3(1.0f, 0.78f, 0.55f),
            .intensity = workloadSpan * workloadSpan * 0.5f,
            .range = workloadSpan * 2.0f,
            .castsShadows = false,
        };
        if (!context.sceneLifetime.CreateAndAdoptWithFragments(
                lightDesc, light, SceneECS::Visibility{}).IsValid())
        {
            Fail(context.assessment,
                 SharedIdentityInvariant.code,
                 "Rendering-stress could not create its deterministic point light.");
            return false;
        }

        m_sharedAssetReady = true;
        ConfigureCameraForWorkload(context);
        static_cast<void>(context.assessment.MarkAction(SharedAssetAction));
        MarkInvariant(context.assessment,
                      SharedIdentityInvariant,
                      AssessmentCheckpoints::ActionApplied);
        return true;
    }

    bool RenderingStressSample::BuildInitialWorkload(SampleContext& context)
    {
        std::vector<uint32> allIndices;
        try
        {
            allIndices.reserve(m_workload.objectCount);
            for (uint32 index = 0; index < m_workload.objectCount; ++index)
                allIndices.push_back(index);
        }
        catch (...)
        {
            Fail(context.assessment,
                 SharedIdentityInvariant.code,
                 "Could not allocate the deterministic workload index set.");
            return false;
        }

        if (!CreateProceduralEntities(context, allIndices, m_workloadEntities) ||
            m_workloadEntities.size() != m_workload.objectCount)
        {
            Fail(context.assessment,
                 SharedIdentityInvariant.code,
                 "The procedural workload did not create every ECS entity with the shared resources.");
            return false;
        }
        m_workloadBuilt = true;
        static_cast<void>(context.assessment.MarkAction(InitialBuildAction));
        return true;
    }

    bool RenderingStressSample::ApplyDirtySet(SampleContext& context)
    {
        if (m_dirtyApplied || m_dirtyIndices.size() != m_workload.dirtyObjectCount)
        {
            Fail(context.assessment,
                 DirtyDeltaInvariant.code,
                 "The exact dirty set was unavailable when its action was requested.");
            return false;
        }
        for (const uint32 index : m_dirtyIndices)
        {
            if (index >= m_workloadEntities.size())
            {
                Fail(context.assessment,
                     DirtyDeltaInvariant.code,
                     "A deterministic dirty index did not resolve to a workload entity.");
                return false;
            }
            const SceneECS::SceneEntityRef entity = m_workloadEntities[index];
            if (!entity.IsValid() ||
                entity.sceneRuntimeId != context.scene.GetSceneRuntimeId() ||
                !context.scene.GetEntityRef(entity.entity).IsValid())
            {
                Fail(context.assessment,
                     DirtyDeltaInvariant.code,
                     "A deterministic dirty entity reference became stale.");
                return false;
            }
            const SceneECS::LocalTransform* authoredTransform =
                context.scene.GetRegistry().TryGet<SceneECS::LocalTransform>(entity.entity);
            if (authoredTransform == nullptr)
            {
                Fail(context.assessment,
                     DirtyDeltaInvariant.code,
                     "A deterministic dirty entity has no local transform fragment.");
                return false;
            }
            SceneECS::LocalTransform updatedTransform = *authoredTransform;
            const Vec3 base = GetEntityPosition(index, m_workload.objectCount);
            updatedTransform.translation = base + Vec3(0.0f, 0.25f, 0.0f);
            if (!context.scene.SetLocalTransform(entity.entity, updatedTransform))
            {
                Fail(context.assessment,
                     DirtyDeltaInvariant.code,
                     "The exact dirty set could not update an ECS local transform.");
                return false;
            }
        }
        // Local-transform writes become render-visible only after the next frozen
        // ECS snapshot. Preserve a prior target if this action is retried while
        // awaiting that boundary.
        m_actionTargetSceneRevision = std::max(
            m_actionTargetSceneRevision,
            context.scene.GetDiagnosticsSnapshot().sceneSnapshotRevision + 1u);
        if (m_actionTargetSceneRevision == 0)
        {
            Fail(context.assessment,
                 DirtyDeltaInvariant.code,
                 "The exact dirty set did not produce a nonzero target ECS snapshot revision.");
            return false;
        }
        m_dirtyApplied = true;
        static_cast<void>(context.assessment.MarkAction(DirtySetAction));
        return true;
    }

    bool RenderingStressSample::QueueChurnDestroy(SampleContext& context)
    {
        if (m_churnIndices.size() != m_workload.churnObjectCount ||
            !m_latestDiagnostics.has_value() ||
            m_churnDestroyCursor >= m_churnIndices.size() ||
            m_churnDestroyedCount != m_churnDestroyCursor)
        {
            Fail(context.assessment,
                 ChurnDeltaInvariant.code,
                 "The bounded churn destroy action did not retain a completed-frame baseline.");
            return false;
        }

        m_actionTargetSceneRevision = 0;

        m_churnBatchOffset = m_churnDestroyCursor;
        m_churnBatchCount = std::min(
            GetDeterministicChurnBatchSize(),
            static_cast<uint32>(m_churnIndices.size()) - m_churnBatchOffset);
        if (m_churnBatchCount == 0)
        {
            Fail(context.assessment,
                 ChurnDeltaInvariant.code,
                 "The bounded churn destroy action selected an empty batch.");
            return false;
        }

        try
        {
            m_churnDestroyReceipts.clear();
            m_churnDestroyReceipts.reserve(m_churnBatchCount);
            for (uint32 offset = 0; offset < m_churnBatchCount; ++offset)
            {
                const uint32 index = m_churnIndices[m_churnBatchOffset + offset];
                if (index >= m_workloadEntities.size() ||
                    !m_workloadEntities[index].IsValid() ||
                    m_workloadEntities[index].sceneRuntimeId !=
                        context.scene.GetSceneRuntimeId() ||
                    !context.scene.GetEntityRef(m_workloadEntities[index].entity).IsValid())
                {
                    Fail(context.assessment,
                         ChurnDeltaInvariant.code,
                         "A deterministic churn batch index did not resolve to a live workload entity.");
                    return false;
                }
            }

            SceneECS::SceneCommandBuffer commands = context.scene.CreateCommandBuffer();
            for (uint32 offset = 0; offset < m_churnBatchCount; ++offset)
            {
                const uint32 index = m_churnIndices[m_churnBatchOffset + offset];
                const SceneECS::SceneCommandReceipt receipt =
                    commands.RequestDestroy(m_workloadEntities[index].entity);
                if (!receipt.IsQueued())
                {
                    Fail(context.assessment,
                         ChurnDeltaInvariant.code,
                         "The ECS runtime did not queue a bounded churn destroy receipt.");
                    return false;
                }
                m_churnDestroyReceipts.push_back(receipt);
            }
            const SceneECS::SceneCommandBufferReceipt submission =
                context.scene.SubmitCommandBuffer(std::move(commands));
            if (!submission.IsQueued())
            {
                Fail(context.assessment,
                     ChurnDeltaInvariant.code,
                     "The ECS runtime rejected the bounded churn destroy command buffer.");
                return false;
            }
        }
        catch (...)
        {
            Fail(context.assessment,
                 ChurnDeltaInvariant.code,
                 "Could not retain bounded churn destroy receipts.");
            return false;
        }
        m_actionTargetSceneRevision =
            context.scene.GetDiagnosticsSnapshot().sceneSnapshotRevision + 1u;
        if (m_actionTargetSceneRevision == 0)
        {
            Fail(context.assessment,
                 ChurnDeltaInvariant.code,
                 "The bounded churn destroy action did not target an ECS snapshot revision.");
            return false;
        }
        m_churnBatchBaseline = m_latestDiagnostics;
        m_pendingActionRenderEvidence.reset();
        m_churnDestroyBatchQueued = true;
        m_churnDestroyReceiptsApplied = false;
        m_churnBatchRemoveObserved = false;
        return true;
    }

    bool RenderingStressSample::AreChurnDestroyReceiptsApplied(
        SampleContext& context)
    {
        if (m_churnDestroyReceiptsApplied)
            return true;
        if (!m_churnDestroyBatchQueued ||
            m_churnDestroyReceipts.size() != m_churnBatchCount)
        {
            Fail(context.assessment,
                 ChurnDeltaInvariant.code,
                 "The bounded churn destroy batch lost its receipt ownership state.");
            return false;
        }
        for (uint32 offset = 0; offset < m_churnBatchCount; ++offset)
        {
            const SceneECS::SceneCommandReceipt& receipt =
                m_churnDestroyReceipts[offset];
            if (receipt.IsQueued())
                return false;
            if (!receipt.IsApplied())
            {
                Fail(context.assessment,
                     ChurnDeltaInvariant.code,
                     "A bounded ECS churn destroy request was rejected or discarded.");
                return false;
            }

            const uint32 index = m_churnIndices[m_churnBatchOffset + offset];
            if (index >= m_workloadEntities.size())
            {
                Fail(context.assessment,
                     ChurnDeltaInvariant.code,
                     "An applied bounded churn destroy receipt did not map to its workload entity.");
                return false;
            }

            const ECS::EntityHandle entity = m_workloadEntities[index].entity;
            if (context.scene.GetRegistry().IsAlive(entity))
            {
                const SceneECS::EntityLifecycleState* lifecycle =
                    context.scene.GetRegistry().TryGet<SceneECS::EntityLifecycleState>(entity);
                if (lifecycle == nullptr ||
                    lifecycle->phase == SceneECS::EntityLifecyclePhase::Alive ||
                    context.scene.GetRegistry().IsEnabled(entity))
                {
                    return false;
                }
            }
        }
        if (m_actionTargetSceneRevision == 0)
        {
            Fail(context.assessment,
                 ChurnDeltaInvariant.code,
                 "Applied bounded churn destroy receipts did not expose a target Scene revision.");
            return false;
        }
        m_churnDestroyReceiptsApplied = true;
        return true;
    }

    bool RenderingStressSample::CompleteChurnDestroyBatch(SampleContext& context)
    {
        if (!m_churnDestroyBatchQueued || !m_churnDestroyReceiptsApplied ||
            !m_churnBatchRemoveObserved || m_churnBatchCount == 0 ||
            !m_churnBatchActionSnapshot.has_value() ||
            m_churnBatchCount > m_workload.churnObjectCount ||
            m_churnBatchCount > m_churnIndices.size() ||
            m_churnBatchOffset != m_churnDestroyCursor ||
            m_churnDestroyCursor > m_churnIndices.size() - m_churnBatchCount ||
            m_churnDestroyedCount > m_workload.churnObjectCount -
                                      m_churnBatchCount)
        {
            Fail(context.assessment,
                 ChurnDeltaInvariant.code,
                 "The bounded churn destroy batch reached an invalid completion state.");
            return false;
        }

        const uint32 completedBatchCount = m_churnBatchCount;
        const RenderingStressDiagnosticsSnapshot actionBaseline =
            *m_churnBatchBaseline;
        const RenderingStressDiagnosticsSnapshot actionSnapshot =
            *m_churnBatchActionSnapshot;
        const uint64 actionTargetSceneRevision = m_actionTargetSceneRevision;
        m_churnDestroyedCount += completedBatchCount;
        m_churnDestroyCursor += completedBatchCount;
        m_churnDestroyReceipts.clear();
        m_churnDestroyBatchQueued = false;
        m_churnDestroyReceiptsApplied = false;
        m_churnBatchRemoveObserved = false;
        m_churnBatchBaseline.reset();
        m_churnBatchActionSnapshot.reset();
        m_churnBatchOffset = 0;
        m_churnBatchCount = 0;

        const uint32 expectedPopulation =
            m_workload.objectCount - m_churnDestroyedCount +
            m_churnRecreatedCount;
        if (m_churnDestroyCursor < m_churnIndices.size())
        {
            return BeginChurnDrain(context.assessment,
                                   RenderingStressPhase::ChurnDestroy,
                                   expectedPopulation,
                                   completedBatchCount,
                                   actionBaseline,
                                   actionSnapshot,
                                   actionTargetSceneRevision,
                                   false);
        }
        if (m_churnDestroyCursor != m_churnIndices.size() ||
            m_churnDestroyedCount != m_workload.churnObjectCount ||
            !m_latestDiagnostics.has_value())
        {
            Fail(context.assessment,
                 ChurnDeltaInvariant.code,
                 "Bounded churn destroy batches did not account for the exact requested total.");
            return false;
        }

        return BeginChurnDrain(context.assessment,
                               RenderingStressPhase::ChurnRecreate,
                               expectedPopulation,
                               completedBatchCount,
                               actionBaseline,
                               actionSnapshot,
                               actionTargetSceneRevision,
                               false);
    }

    bool RenderingStressSample::RecreateChurnEntities(SampleContext& context)
    {
        if (!m_churnRemoveObserved || !m_latestDiagnostics.has_value() ||
            m_churnRecreateCursor >= m_churnIndices.size() ||
            m_churnRecreatedCount != m_churnRecreateCursor)
        {
            Fail(context.assessment,
                 ChurnDeltaInvariant.code,
                 "The bounded churn recreation action did not retain a valid completed destroy total.");
            return false;
        }

        m_churnBatchOffset = m_churnRecreateCursor;
        m_churnBatchCount = std::min(
            GetDeterministicChurnBatchSize(),
            static_cast<uint32>(m_churnIndices.size()) - m_churnBatchOffset);
        if (m_churnBatchCount == 0)
        {
            Fail(context.assessment,
                 ChurnDeltaInvariant.code,
                 "The bounded churn recreation action selected an empty batch.");
            return false;
        }

        std::vector<uint32> batchIndices;
        std::vector<SceneECS::SceneEntityRef> replacementEntities;
        try
        {
            batchIndices.assign(
                m_churnIndices.begin() + m_churnBatchOffset,
                m_churnIndices.begin() + m_churnBatchOffset + m_churnBatchCount);
        }
        catch (...)
        {
            Fail(context.assessment,
                 ChurnDeltaInvariant.code,
                 "Could not construct the bounded churn recreation index range.");
            return false;
        }
        if (!CreateProceduralEntities(context, batchIndices, replacementEntities) ||
            replacementEntities.size() != m_churnBatchCount)
        {
            Fail(context.assessment,
                 ChurnDeltaInvariant.code,
                 "A bounded churn batch could not be recreated through the lifetime scope.");
            return false;
        }
        for (uint32 offset = 0; offset < m_churnBatchCount; ++offset)
            m_workloadEntities[batchIndices[offset]] = replacementEntities[offset];

        m_actionTargetSceneRevision =
            context.scene.GetDiagnosticsSnapshot().sceneSnapshotRevision + 1u;
        if (m_actionTargetSceneRevision == 0)
        {
            Fail(context.assessment,
                 ChurnDeltaInvariant.code,
                 "The bounded churn recreation action did not produce a nonzero target ECS snapshot revision.");
            return false;
        }

        m_churnBatchBaseline = m_latestDiagnostics;
        m_pendingActionRenderEvidence.reset();
        m_churnRecreateBatchQueued = true;
        m_churnBatchAddObserved = false;
        return true;
    }

    bool RenderingStressSample::CompleteChurnRecreateBatch(
        SampleContext& context)
    {
        if (!m_churnRecreateBatchQueued || !m_churnBatchAddObserved ||
            m_churnBatchCount == 0 ||
            !m_churnBatchActionSnapshot.has_value() ||
            m_churnBatchCount > m_workload.churnObjectCount ||
            m_churnBatchCount > m_churnIndices.size() ||
            m_churnBatchOffset != m_churnRecreateCursor ||
            m_churnRecreateCursor > m_churnIndices.size() - m_churnBatchCount ||
            m_churnRecreatedCount > m_workload.churnObjectCount -
                                      m_churnBatchCount)
        {
            Fail(context.assessment,
                 ChurnDeltaInvariant.code,
                 "The bounded churn recreation batch reached an invalid completion state.");
            return false;
        }

        const uint32 completedBatchCount = m_churnBatchCount;
        const RenderingStressDiagnosticsSnapshot actionBaseline =
            *m_churnBatchBaseline;
        const RenderingStressDiagnosticsSnapshot actionSnapshot =
            *m_churnBatchActionSnapshot;
        const uint64 actionTargetSceneRevision = m_actionTargetSceneRevision;
        m_churnRecreatedCount += completedBatchCount;
        m_churnRecreateCursor += completedBatchCount;
        m_churnRecreateBatchQueued = false;
        m_churnBatchAddObserved = false;
        m_churnBatchBaseline.reset();
        m_churnBatchActionSnapshot.reset();
        m_churnBatchOffset = 0;
        m_churnBatchCount = 0;

        const uint32 expectedPopulation =
            m_workload.objectCount - m_churnDestroyedCount +
            m_churnRecreatedCount;
        if (m_churnRecreateCursor < m_churnIndices.size())
        {
            return BeginChurnDrain(context.assessment,
                                   RenderingStressPhase::ChurnRecreate,
                                   expectedPopulation,
                                   completedBatchCount,
                                   actionBaseline,
                                   actionSnapshot,
                                   actionTargetSceneRevision,
                                   true);
        }
        if (m_churnRecreateCursor != m_churnIndices.size() ||
            m_churnRecreatedCount != m_workload.churnObjectCount ||
            m_churnDestroyedCount != m_workload.churnObjectCount ||
            !m_latestDiagnostics.has_value())
        {
            Fail(context.assessment,
                 ChurnDeltaInvariant.code,
                 "Bounded churn recreation batches did not account for the exact requested total.");
            return false;
        }

        return BeginChurnDrain(context.assessment,
                               RenderingStressPhase::FinalStable,
                               expectedPopulation,
                               completedBatchCount,
                               actionBaseline,
                               actionSnapshot,
                               actionTargetSceneRevision,
                               true);
    }

    bool RenderingStressSample::CaptureInitialExecutionOwnerCounts(
        SampleAssessmentChannel& assessment,
        const RenderingStressDiagnosticsSnapshot& snapshot)
    {
        if (m_initialExecutionOwnerCountsCaptured)
            return true;
        if (!m_actualExecutionPath.has_value() || m_workload.objectCount == 0)
        {
            Fail(assessment,
                 StaticReuseInvariant.code,
                 "The initial workload had no resolved execution-path evidence.");
            return false;
        }

        if (snapshot.gpuDrivenActiveRowCount != 0)
        {
            const std::optional<uint32> ownerCount = GetExactOwnerCount(
                snapshot.gpuDrivenActiveRowCount,
                m_workload.objectCount);
            if (!ownerCount.has_value() || *ownerCount == 0 ||
                snapshot.gpuDrivenActiveRowHighWatermark <
                    snapshot.gpuDrivenActiveRowCount)
            {
                Fail(assessment,
                     StaticReuseInvariant.code,
                     "The initial GPU-driven active-row population was not an exact nonzero owner multiple.");
                return false;
            }
            m_gpuDrivenInitialOwnerCount = *ownerCount;
        }

        if (snapshot.directRasterActiveInstanceCount != 0)
        {
            const std::optional<uint32> ownerCount = GetExactOwnerCount(
                snapshot.directRasterActiveInstanceCount,
                m_workload.objectCount);
            if (!ownerCount.has_value() || *ownerCount == 0 ||
                !HasDirectRasterCapacity(snapshot))
            {
                Fail(assessment,
                     StaticReuseInvariant.code,
                     "The initial Direct raster population was not an exact nonzero owner multiple.");
                return false;
            }
            m_directRasterInitialOwnerCount = *ownerCount;
        }

        const bool selectedPathHasOwners =
            *m_actualExecutionPath == RenderingStressExecutionPath::GPUDriven
                ? m_gpuDrivenInitialOwnerCount != 0
                : m_directRasterInitialOwnerCount != 0;
        if (!selectedPathHasOwners)
        {
            Fail(assessment,
                 StaticReuseInvariant.code,
                 "The selected execution path did not retain a nonzero exact owner population.");
            return false;
        }

        m_initialExecutionOwnerCountsCaptured = true;
        return true;
    }

    bool RenderingStressSample::BeginChurnDrain(
        SampleAssessmentChannel& assessment,
        RenderingStressPhase nextPhase,
        uint32 expectedPopulation,
        uint32 batchCount,
        const RenderingStressDiagnosticsSnapshot& actionBaseline,
        const RenderingStressDiagnosticsSnapshot& actionSnapshot,
        uint64 actionTargetSceneRevision,
        bool allowTemporalSettle)
    {
        if (actionTargetSceneRevision == 0)
        {
            Fail(assessment,
                 ChurnDeltaInvariant.code,
                 "The bounded churn drain did not retain a nonzero action target Scene revision.");
            return false;
        }
        m_churnDrainBaseline = actionSnapshot;
        m_churnDrainActionBaseline = actionBaseline;
        m_churnDrainActionTargetSceneRevision = actionTargetSceneRevision;
        m_churnDrainNextPhase = nextPhase;
        m_churnDrainExpectedPopulation = expectedPopulation;
        m_churnDrainBatchCount = batchCount;
        m_churnDrainCompletedFrameCount = 0;
        m_churnDrainTemporalSettleAllowed = allowTemporalSettle;
        m_churnDrainTemporalSettleObserved =
            allowTemporalSettle && HasMutationTemporalSettle(
                                      actionBaseline,
                                      actionSnapshot,
                                      MutationActionKind::Add,
                                      batchCount);
        m_phase = RenderingStressPhase::ChurnDrain;
        return ReplayPendingChurnDrain(assessment);
    }

    bool RenderingStressSample::ObserveChurnDrain(
        SampleAssessmentChannel& assessment,
        const RenderingStressDiagnosticsSnapshot& current)
    {
        if (!m_churnDrainBaseline.has_value() ||
            !m_churnDrainActionBaseline.has_value() ||
            m_churnDrainExpectedPopulation == 0 ||
            m_churnDrainBatchCount == 0 ||
            m_churnDrainActionTargetSceneRevision == 0 ||
            (m_churnDrainNextPhase != RenderingStressPhase::ChurnDestroy &&
             m_churnDrainNextPhase != RenderingStressPhase::ChurnRecreate &&
             m_churnDrainNextPhase != RenderingStressPhase::FinalStable))
        {
            Fail(assessment,
                 ChurnDeltaInvariant.code,
                 "The bounded churn drain lost its completed action baseline.");
            return false;
        }

        const MutationActionKind action = m_churnDrainTemporalSettleAllowed
                                              ? MutationActionKind::Add
                                              : MutationActionKind::Remove;
        const RenderingStressWindowEvaluation cumulative =
            EvaluateCumulativeMutationWindow(
                *m_churnDrainActionBaseline,
                current,
                m_churnDrainActionTargetSceneRevision,
                action,
                m_churnDrainBatchCount,
                action == MutationActionKind::Remove
                    ? m_churnDrainExpectedPopulation + m_churnDrainBatchCount
                    : m_churnDrainExpectedPopulation - m_churnDrainBatchCount,
                m_churnDrainExpectedPopulation,
                m_gpuDrivenInitialOwnerCount,
                m_directRasterInitialOwnerCount);
        if (!cumulative.passed)
        {
            Fail(assessment, ChurnDeltaInvariant.code, cumulative.detail);
            return false;
        }
        const bool currentIsTemporalSettle =
            current.gpuSceneUpdateCount != 0 ||
            current.gpuSceneFrameUploadBytes != 0;
        const bool cumulativeTemporalSettleObserved =
            HasMutationTemporalSettle(*m_churnDrainActionBaseline,
                                      current,
                                      action,
                                      m_churnDrainBatchCount);
        const bool temporalSettleObservedBeforeCurrent =
            m_churnDrainTemporalSettleObserved ||
            (!currentIsTemporalSettle && cumulativeTemporalSettleObserved);
        const bool strictStatic =
            EvaluateStaticWindow(*m_churnDrainBaseline, current).passed;
        if (!IsWithinPresentationPropagationBudget(
                m_churnDrainBaseline->mutationEvidence,
                current.mutationEvidence,
                GetChurnDrainCompletedFrameLimit(),
                strictStatic))
        {
            Fail(assessment,
                 ChurnDeltaInvariant.code,
                 "Churn propagation exceeded its completed-presentation budget.");
            return false;
        }

        const RenderingStressWindowEvaluation drain =
            EvaluateChurnDrainWindow(*m_churnDrainBaseline,
                                     current,
                                     m_churnDrainExpectedPopulation,
                                     m_churnDrainBatchCount,
                                     m_gpuDrivenInitialOwnerCount,
                                     m_directRasterInitialOwnerCount,
                                     m_churnDrainTemporalSettleAllowed,
                                     temporalSettleObservedBeforeCurrent);
        if (!drain.passed)
        {
            Fail(assessment, ChurnDeltaInvariant.code, drain.detail);
            return false;
        }
        m_churnDrainTemporalSettleObserved =
            m_churnDrainTemporalSettleObserved ||
            cumulativeTemporalSettleObserved;

        if (!strictStatic)
        {
            ++m_churnDrainCompletedFrameCount;
            if (m_churnDrainCompletedFrameCount >
                GetChurnDrainCompletedFrameLimit())
            {
                Fail(assessment,
                     ChurnDeltaInvariant.code,
                     "Bounded churn execution propagation did not quiesce within " +
                         std::to_string(GetChurnDrainCompletedFrameLimit()) +
                         " subsequent distinct completed frames.");
            }
            return false;
        }

        if (m_churnDrainTemporalSettleAllowed &&
            !m_churnDrainTemporalSettleObserved)
        {
            Fail(assessment,
                 ChurnDeltaInvariant.code,
                 "Churn recreation drain reached a strict static frame without its exact temporal GPU-scene settle.");
            return false;
        }

        const RenderingStressPhase nextPhase = m_churnDrainNextPhase;
        const RenderingStressDiagnosticsSnapshot actionBaseline =
            *m_churnDrainActionBaseline;
        const RenderingStressDiagnosticsSnapshot actionSnapshot =
            *m_churnDrainBaseline;
        const uint64 actionTargetSceneRevision =
            m_churnDrainActionTargetSceneRevision;
        m_churnDrainBaseline.reset();
        m_churnDrainActionBaseline.reset();
        m_churnDrainExpectedPopulation = 0;
        m_churnDrainBatchCount = 0;
        m_churnDrainCompletedFrameCount = 0;
        m_churnDrainNextPhase = RenderingStressPhase::Failed;
        m_churnDrainTemporalSettleAllowed = false;
        m_churnDrainTemporalSettleObserved = false;
        m_churnDrainActionTargetSceneRevision = 0;
        m_actionTargetSceneRevision = 0;

        switch (nextPhase)
        {
            case RenderingStressPhase::ChurnDestroy:
                m_phase = RenderingStressPhase::ChurnDestroy;
                return true;
            case RenderingStressPhase::ChurnRecreate:
                m_churnRemoveObserved = true;
                m_windowBaseline = current;
                static_cast<void>(assessment.MarkAction(ChurnDestroyAction));
                PublishMutationActionEvidence(
                    assessment,
                    AssessmentCheckpoints::ActionApplied,
                    actionBaseline,
                    actionSnapshot,
                    actionTargetSceneRevision);
                PublishSnapshot(assessment,
                                AssessmentCheckpoints::ActionApplied,
                                actionSnapshot);
                m_phase = RenderingStressPhase::ChurnRecreate;
                return true;
            case RenderingStressPhase::FinalStable:
                m_windowBaseline = current;
                m_staticStreamWarmupFrameCount = 0;
                static_cast<void>(assessment.MarkAction(ChurnRecreateAction));
                PublishMutationActionEvidence(
                    assessment,
                    AssessmentCheckpoints::ActionApplied,
                    actionBaseline,
                    actionSnapshot,
                    actionTargetSceneRevision);
                PublishSnapshot(assessment,
                                AssessmentCheckpoints::ActionApplied,
                                actionSnapshot);
                PublishChurnTotals(assessment,
                                   AssessmentCheckpoints::ActionApplied);
                MarkInvariant(assessment,
                              ChurnDeltaInvariant,
                              AssessmentCheckpoints::ActionApplied);
                MarkInvariant(assessment,
                              NoFullRebuildInvariant,
                              AssessmentCheckpoints::ActionApplied);
                m_phase = RenderingStressPhase::FinalStable;
                return true;
            default:
                Fail(assessment,
                     ChurnDeltaInvariant.code,
                     "The bounded churn drain selected an invalid continuation phase.");
                return false;
        }
    }

    bool RenderingStressSample::CreateProceduralEntities(
        SampleContext& context,
        const std::vector<uint32>& entityIndices,
        std::vector<SceneECS::SceneEntityRef>& outEntities)
    {
        if (entityIndices.empty() || !m_sharedMeshAssetId.IsValid() ||
            !m_sharedMaterialAssetId.IsValid())
        {
            return false;
        }

        const size_t initialEntityCount = outEntities.size();
        const size_t requestedEntityCount = entityIndices.size();
        const size_t ownedEntityCount =
            context.sceneLifetime.GetDiagnostics().ownedEntityCount;
        if (requestedEntityCount > std::numeric_limits<size_t>::max() -
                                       ownedEntityCount ||
            requestedEntityCount > std::numeric_limits<size_t>::max() -
                                       initialEntityCount ||
            !context.sceneLifetime.ReserveOwnershipCapacity(
                ownedEntityCount + requestedEntityCount))
        {
            return false;
        }

        try
        {
            outEntities.reserve(initialEntityCount + requestedEntityCount);
        }
        catch (...)
        {
            return false;
        }

        SceneECS::SceneSpawnTransaction transaction =
            context.scene.BeginSpawnTransaction();
        std::vector<SceneECS::SceneSpawnEntityId> pending;
        try
        {
            pending.reserve(requestedEntityCount);
            for (const uint32 index : entityIndices)
            {
                SceneECS::RuntimeEntityDesc desc;
                desc.localTransform.translation =
                    GetEntityPosition(index, m_workload.objectCount);
                desc.bounds = m_sharedMeshBounds;

                const SceneECS::SceneSpawnEntityId entity = transaction.Create(desc);
                if (!entity.IsValid() ||
                    !transaction.Add<SceneECS::Mesh>(
                        entity,
                        {.meshAssetId = m_sharedMeshAssetId,
                         .submeshCount = m_sharedMaterialSlots.count}) ||
                    !transaction.Add<SceneECS::MaterialSlots>(
                        entity, m_sharedMaterialSlots) ||
                    !transaction.Add<SceneECS::Visibility>(entity, {}))
                {
                    return false;
                }
                pending.push_back(entity);
            }
        }
        catch (...)
        {
            return false;
        }

        const SceneECS::SceneSpawnCommitResult result = transaction.Commit();
        if (!result.IsApplied())
            return false;

        std::vector<ECS::EntityHandle> committedHandles;
        std::vector<SceneECS::SceneEntityRef> createdEntities;
        try
        {
            committedHandles.reserve(pending.size());
            createdEntities.reserve(pending.size());
            for (const SceneECS::SceneSpawnEntityId entity : pending)
            {
                const ECS::EntityHandle handle = result.GetEntity(entity);
                if (!handle.IsValid())
                {
                    if (!committedHandles.empty())
                        static_cast<void>(context.scene.RequestDestroyBatch(committedHandles));
                    return false;
                }
                committedHandles.push_back(handle);
            }

            for (const SceneECS::SceneSpawnEntityId entity : pending)
            {
                const SceneECS::SceneEntityRef ref = result.GetEntityRef(entity);
                if (!ref.IsValid() ||
                    ref.sceneRuntimeId != context.scene.GetSceneRuntimeId())
                {
                    static_cast<void>(context.scene.RequestDestroyBatch(committedHandles));
                    return false;
                }
                createdEntities.push_back(ref);
            }
        }
        catch (...)
        {
            if (!committedHandles.empty())
                static_cast<void>(context.scene.RequestDestroyBatch(committedHandles));
            return false;
        }

        if (!context.sceneLifetime.AdoptBatch(createdEntities))
        {
            static_cast<void>(context.scene.RequestDestroyBatch(committedHandles));
            return false;
        }

        outEntities.insert(outEntities.end(),
                           createdEntities.begin(),
                           createdEntities.end());
        return true;
    }

    void RenderingStressSample::ConfigureCameraForWorkload(
        SampleContext& context)
    {
        const float32 span = GetWorkloadSpan(m_workload.objectCount);
        const float32 aspect = static_cast<float32>(context.options.width) /
                               static_cast<float32>(
                                   std::max(context.options.height, 1u));
        const AABB workloadBounds(
            Vec3(-span * 0.5f, -0.55f, -span * 0.5f),
            Vec3(span * 0.5f, 0.80f, span * 0.5f));
        SampleOrbitCameraSettings settings;
        settings.mode = OrbitCameraMode::ExteriorInspect;
        settings.bounds = workloadBounds;
        settings.pivot = Vec3(0.0f, 0.0f, 0.0f);
        settings.distance = span * 1.6f;
        settings.yaw = 0.66f;
        settings.pitch = 0.62f;
        settings.minDistance = std::max(span * 0.55f, 0.001f);
        settings.maxDistance = std::max(span * 4.0f,
                                        settings.minDistance * 2.0f);
        settings.verticalFovRadians = radians(50.0f);
        settings.aspectRatio = aspect;
        settings.fitMargin = 1.10f;
        settings.farClipPaddingScale = 0.50f;
        m_orbitCamera.Initialize(settings, context.input);
        if (m_orbitCamera.IsInitialized())
        {
            static_cast<void>(m_orbitCamera.Apply(context.cameras, context.camera));
            m_cameraConfigured = true;
        }
    }

    void RenderingStressSample::PublishSnapshot(
        SampleAssessmentChannel& assessment,
        const AssessmentCheckpoint& checkpoint,
        const RenderingStressDiagnosticsSnapshot& snapshot) const
    {
        const auto metric = [](const AssessmentMetric& definition, uint64 value)
        {
            return AssessmentMetricValue{
                definition,
                DiagnosticValue<AssessmentScalar>::Available(value)};
        };
        const auto booleanMetric = [](const AssessmentMetric& definition,
                                      bool value)
        {
            return AssessmentMetricValue{
                definition,
                DiagnosticValue<AssessmentScalar>::Available(value)};
        };
        static_cast<void>(assessment.TryPublish(AssessmentSnapshot{
            checkpoint,
            {
                metric(ExtractionFullScanMetric, snapshot.extractionFullScanCount),
                metric(ExtractionChangeFeedMetric,
                       snapshot.extractionChangeFeedChangeCount),
                metric(ExtractionActorRebuildMetric,
                       snapshot.extractionActorRebuildCount),
                metric(ExtractionProxyVisitMetric,
                       snapshot.extractionProxyVisitCount),
                metric(ExtractionComponentVisitMetric,
                       snapshot.extractionComponentVisitCount),
                metric(ExtractionProviderVisitMetric,
                       snapshot.extractionFeatureProviderVisitCount),
                metric(AcceptedExtractionPublicationMetric,
                       snapshot.acceptedExtractionPublicationCount),
                metric(AcceptedExtractionLastSourceFrameMetric,
                       snapshot.acceptedExtractionLastSourceFrameSequence),
                metric(AcceptedExtractionLastSceneRevisionMetric,
                       snapshot.acceptedExtractionLastSceneRevision),
                metric(AcceptedExtractionFullScanMetric,
                       snapshot.acceptedExtractionCumulativeFullScanCount),
                metric(AcceptedExtractionChangeFeedMetric,
                       snapshot
                           .acceptedExtractionCumulativeChangeFeedChangeCount),
                metric(AcceptedExtractionActorRebuildMetric,
                       snapshot
                           .acceptedExtractionCumulativeActorRebuildCount),
                metric(AcceptedExtractionProxyVisitMetric,
                       snapshot
                           .acceptedExtractionCumulativeProxyVisitCount),
                metric(AcceptedExtractionComponentVisitMetric,
                       snapshot
                           .acceptedExtractionCumulativeComponentVisitCount),
                metric(AcceptedExtractionProviderVisitMetric,
                       snapshot
                           .acceptedExtractionCumulativeFeatureProviderVisitCount),
                metric(AcceptedExtractionContinuityLossMetric,
                       snapshot.acceptedExtractionContinuityLossCount),
                metric(SceneFullRebuildMetric, snapshot.sceneFullRebuildCount),
                metric(SceneIncrementalMetric, snapshot.sceneIncrementalUpdateCount),
                metric(SceneStaticReuseMetric, snapshot.sceneStaticReuseCount),
                metric(SceneLastRebuiltMetric, snapshot.sceneLastRebuiltObjectCount),
                metric(SceneLastRemovedMetric, snapshot.sceneLastRemovedObjectCount),
                metric(DrawPacketBuildMetric, snapshot.drawPacketBuildCount),
                metric(DrawPacketInvalidationMetric,
                       snapshot.drawPacketInvalidationCount),
                metric(GPUSceneAddMetric, snapshot.gpuSceneAddCount),
                metric(GPUSceneUpdateMetric, snapshot.gpuSceneUpdateCount),
                metric(GPUSceneRemoveMetric, snapshot.gpuSceneRemoveCount),
                metric(GPUSceneNoOpMetric, snapshot.gpuSceneNoOpCount),
                metric(GPUSceneUploadMetric, snapshot.gpuSceneFrameUploadBytes),
                booleanMetric(GPUSceneFullUploadMetric, snapshot.gpuSceneFullUpload),
                metric(GPUDrivenInstanceUploadMetric,
                       snapshot.gpuDrivenInstanceUploadBytes),
                metric(GPUDrivenCandidateUploadMetric,
                       snapshot.gpuDrivenCandidateUploadBytes),
                metric(GPUDrivenActiveRowUploadMetric,
                       snapshot.gpuDrivenActiveRowUploadBytes),
                metric(GPUDrivenActiveRowCountMetric,
                       snapshot.gpuDrivenActiveRowCount),
                metric(GPUDrivenActiveRowHighWatermarkMetric,
                       snapshot.gpuDrivenActiveRowHighWatermark),
                metric(GPUDrivenInstancePatchedRowMetric,
                       snapshot.gpuDrivenInstancePatchedRowCount),
                metric(GPUDrivenCandidatePatchedRowMetric,
                       snapshot.gpuDrivenCandidatePatchedRowCount),
                metric(GPUDrivenActiveRowPatchedRowMetric,
                       snapshot.gpuDrivenActiveRowPatchedRowCount),
                metric(GPUDrivenInstanceFullMaterializationMetric,
                       snapshot.gpuDrivenInstanceFullMaterializationCount),
                metric(GPUDrivenCandidateFullMaterializationMetric,
                       snapshot.gpuDrivenCandidateFullMaterializationCount),
                metric(GPUDrivenActiveRowFullMaterializationMetric,
                       snapshot.gpuDrivenActiveRowFullMaterializationCount),
                metric(GPUDrivenContinuityFullMaterializationMetric,
                       snapshot.gpuDrivenContinuityFullMaterializationCount),
                metric(GPUDrivenCapacityFullMaterializationMetric,
                       snapshot.gpuDrivenCapacityFullMaterializationCount),
                metric(DirectRasterInstanceUploadMetric,
                       snapshot.directRasterInstanceUploadBytes),
                metric(DirectRasterIndexUploadMetric,
                       snapshot.directRasterInstanceIndexUploadBytes),
                metric(DirectRasterInstancePatchedRowMetric,
                       snapshot.directRasterInstancePatchedRowCount),
                metric(DirectRasterIndexPatchedRowMetric,
                       snapshot.directRasterIndexPatchedRowCount),
                metric(DirectRasterActiveInstanceCountMetric,
                       snapshot.directRasterActiveInstanceCount),
                metric(DirectRasterActiveInstanceCapacityMetric,
                       snapshot.directRasterActiveInstanceCapacity),
                metric(DirectRasterInstanceFullMaterializationMetric,
                       snapshot.directRasterInstanceFullMaterializationCount),
                metric(DirectRasterIndexFullMaterializationMetric,
                       snapshot.directRasterIndexFullMaterializationCount)}}));
    }

    void RenderingStressSample::PublishChurnTotals(
        SampleAssessmentChannel& assessment,
        const AssessmentCheckpoint& checkpoint) const
    {
        static_cast<void>(assessment.TryPublish(AssessmentSnapshot{
            checkpoint,
            {
                {ChurnDestroyedTotalMetric,
                 DiagnosticValue<AssessmentScalar>::Available(
                     static_cast<uint64>(m_churnDestroyedCount))},
                {ChurnRecreatedTotalMetric,
                 DiagnosticValue<AssessmentScalar>::Available(
                     static_cast<uint64>(m_churnRecreatedCount))}}}));
    }

    void RenderingStressSample::PublishMutationActionEvidence(
        SampleAssessmentChannel& assessment,
        const AssessmentCheckpoint& checkpoint,
        const RenderingStressDiagnosticsSnapshot& baseline,
        const RenderingStressDiagnosticsSnapshot& completed,
        uint64 actionTargetSceneRevision) const
    {
        const SampleRenderMutationEvidenceDiagnostics& before =
            baseline.mutationEvidence;
        const SampleRenderMutationEvidenceDiagnostics& after =
            completed.mutationEvidence;
        const uint64 gpuCullingPatchedRows = SaturatingMetricSum(
            SaturatingMetricSum(
                after.gpuCullingInstancePatchedRowCount -
                    before.gpuCullingInstancePatchedRowCount,
                after.gpuCullingCandidatePatchedRowCount -
                    before.gpuCullingCandidatePatchedRowCount),
            after.gpuCullingActiveRowPatchedRowCount -
                before.gpuCullingActiveRowPatchedRowCount);
        const uint64 directRasterPatchedRows = SaturatingMetricSum(
            after.directRasterInstancePatchedRowCount -
                before.directRasterInstancePatchedRowCount,
            after.directRasterIndexPatchedRowCount -
                before.directRasterIndexPatchedRowCount);
        const auto metric = [](const AssessmentMetric& definition, uint64 value)
        {
            return AssessmentMetricValue{
                definition,
                DiagnosticValue<AssessmentScalar>::Available(value)};
        };
        static_cast<void>(assessment.TryPublish(AssessmentSnapshot{
            checkpoint,
            {
                metric(MutationActionTargetSceneRevisionMetric,
                       actionTargetSceneRevision),
                metric(MutationCompletedFrameSequenceMetric,
                       after.completedFrameSequence),
                metric(MutationAppliedSceneRevisionMetric,
                       after.appliedSceneRevision),
                metric(MutationSceneIncrementalDeltaMetric,
                       after.sceneIncrementalCommitCount -
                           before.sceneIncrementalCommitCount),
                metric(MutationSceneRebuiltDeltaMetric,
                       after.sceneRebuiltObjectCount -
                           before.sceneRebuiltObjectCount),
                metric(MutationSceneRemovedDeltaMetric,
                       after.sceneRemovedObjectCount -
                           before.sceneRemovedObjectCount),
                metric(MutationGPUSceneAddDeltaMetric,
                       after.gpuSceneAddCount - before.gpuSceneAddCount),
                metric(MutationGPUSceneUpdateDeltaMetric,
                       after.gpuSceneUpdateCount - before.gpuSceneUpdateCount),
                metric(MutationGPUSceneRemoveDeltaMetric,
                       after.gpuSceneRemoveCount - before.gpuSceneRemoveCount),
                metric(MutationGPUCullingPatchedRowDeltaMetric,
                       gpuCullingPatchedRows),
                metric(MutationDirectRasterPatchedRowDeltaMetric,
                       directRasterPatchedRows)}}));
    }

    void RenderingStressSample::MarkInvariant(
        SampleAssessmentChannel& assessment,
        const AssessmentInvariant& invariant,
        const AssessmentCheckpoint& checkpoint) const
    {
        static_cast<void>(assessment.TryPublish(
            AssessmentInvariantObservation{invariant, checkpoint}));
    }

    void RenderingStressSample::ReportInstrumentationGap(
        SampleAssessmentChannel& assessment,
        std::string detail,
        bool publishCapabilityUnavailable)
    {
        if (!m_failure.empty())
            return;

        if (publishCapabilityUnavailable)
        {
            static_cast<void>(assessment.MarkCapabilityObservation({
                RetainedSceneCapability,
                AssessmentCheckpoints::ActionApplied,
                DiagnosticValue<bool>::Available(false),
                detail}));
            static_cast<void>(assessment.MarkCapabilityObservation({
                GPUSceneCapability,
                AssessmentCheckpoints::ActionApplied,
                DiagnosticValue<bool>::Available(false),
                detail}));
            static_cast<void>(assessment.MarkCapabilityObservation({
                ExtractionCapability,
                AssessmentCheckpoints::ActionApplied,
                DiagnosticValue<bool>::Available(false),
                detail}));
        }

        Finding finding;
        finding.code = AssessmentCode("RENDER.STRESS.INSTRUMENTATION_GAP");
        finding.subsystemCode = AssessmentCode("RENDER.STRESS");
        finding.invariantCode = StaticReuseInvariant.code;
        finding.checkpoint = AssessmentCheckpoints::ActionApplied;
        finding.classification = FindingClass::InstrumentationGap;
        finding.severity = FindingSeverity::Error;
        finding.confidence = FindingConfidence::Confirmed;
        finding.summary = "Rendering-stress cannot evaluate required completed-frame diagnostics.";
        finding.detail = std::move(detail);
        finding.expected = "Incremental extraction, retained Scene, draw-packet, GPU-scene, and GPU-driven upload fields.";
        finding.observed = finding.detail;
        finding.gating = true;
        finding.blockingReason = "Required rendering-stress instrumentation is unavailable.";
        static_cast<void>(assessment.TryPublish(std::move(finding)));
        m_failure = "Required rendering-stress instrumentation is unavailable.";
        m_phase = RenderingStressPhase::Failed;
    }

    void RenderingStressSample::Fail(SampleAssessmentChannel& assessment,
                                     AssessmentCode invariantCode,
                                     std::string message)
    {
        if (!m_failure.empty())
            return;

        m_failure = std::move(message);
        m_phase = RenderingStressPhase::Failed;
        Finding finding;
        finding.code = AssessmentCode("RENDER.STRESS.CONTRACT_FAILED");
        finding.subsystemCode = AssessmentCode("RENDER.STRESS");
        finding.invariantCode = std::move(invariantCode);
        finding.checkpoint = AssessmentCheckpoints::ActionApplied;
        finding.classification = FindingClass::PerformanceRegression;
        finding.severity = FindingSeverity::Error;
        finding.confidence = FindingConfidence::Confirmed;
        finding.summary = "The deterministic rendering-stress contract failed.";
        finding.detail = m_failure;
        finding.expected = "Exact retained Scene and GPU-scene delta behavior.";
        finding.observed = m_failure;
        finding.gating = true;
        finding.blockingReason = "The deterministic rendering-stress contract failed.";
        static_cast<void>(assessment.TryPublish(std::move(finding)));
    }

    void RenderingStressSample::ReportIncomplete(SampleContext& context)
    {
        if (m_incompleteReported || m_phase == RenderingStressPhase::Complete ||
            m_phase == RenderingStressPhase::Failed)
        {
            return;
        }
        m_incompleteReported = true;
        Finding finding;
        finding.code = AssessmentCode("RENDER.STRESS.PHASE.INCOMPLETE");
        finding.subsystemCode = AssessmentCode("RENDER.STRESS");
        finding.invariantCode = AssessmentCode("RENDER.STRESS.PHASE_COMPLETION");
        finding.checkpoint = AssessmentCheckpoints::ScenarioStable;
        finding.classification = FindingClass::PerformanceRegression;
        finding.severity = FindingSeverity::Error;
        finding.confidence = FindingConfidence::StrongEvidence;
        finding.summary = "The deterministic rendering-stress state machine did not reach FinalStable.";
        finding.detail = "Stopped in phase " +
                         std::string(GetPhaseName(m_phase)) + ".";
        finding.expected = "InitialBuild, StableBaseline, DirtySet, StableAfterDirty, ChurnDestroy, ChurnRecreate, ChurnDrain, and FinalStable complete.";
        finding.observed = finding.detail;
        finding.gating = true;
        finding.blockingReason = "Rendering-stress did not complete every required phase.";
        static_cast<void>(context.assessment.TryPublish(std::move(finding)));
    }

    const char* RenderingStressSample::GetPhaseName(
        RenderingStressPhase phase) noexcept
    {
        switch (phase)
        {
            case RenderingStressPhase::WaitingForFirstPresentation:
                return "WaitingForFirstPresentation";
            case RenderingStressPhase::WaitingForSharedAsset:
                return "WaitingForSharedAsset";
            case RenderingStressPhase::InitialBuild: return "InitialBuild";
            case RenderingStressPhase::StableBaseline: return "StableBaseline";
            case RenderingStressPhase::DirtySet: return "DirtySet";
            case RenderingStressPhase::StableAfterDirty: return "StableAfterDirty";
            case RenderingStressPhase::ChurnDestroy: return "ChurnDestroy";
            case RenderingStressPhase::ChurnRecreate: return "ChurnRecreate";
            case RenderingStressPhase::ChurnDrain: return "ChurnDrain";
            case RenderingStressPhase::FinalStable: return "FinalStable";
            case RenderingStressPhase::Complete: return "Complete";
            case RenderingStressPhase::Failed: return "Failed";
            default: return "Invalid";
        }
    }
} // namespace RVX
