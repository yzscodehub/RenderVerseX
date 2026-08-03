#pragma once

/**
 * @file RenderPassRecordContext.h
 * @brief Immutable frame/view inputs captured while registering scene passes.
 */

#include "Render/Graph/RenderGraph.h"
#include "Render/GPUDriven/GPUDrivenDiagnostics.h"
#include "Render/Passes/MeshPassProcessor.h"
#include "Render/Policy/RenderFrameExecutionPlan.h"
#include "Render/Renderer/RenderDrawItem.h"
#include "Render/Renderer/RenderScene.h"
#include "Render/Renderer/ViewData.h"
#include "Render/Visibility/RenderVisibility.h"

#include <atomic>
#include <memory>
#include <vector>

namespace RVX
{
    class GPUCulling;
    class GPUCullingRecordedState;
    class RenderGraph;

    struct DepthPrepassDrawStats
    {
        uint32 directDrawCount = 0;
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
        RHIFormat outputFormat = RHIFormat::Unknown;
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

    struct OpaquePassDrawStats
    {
        uint32 directDrawCount = 0;
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
        bool planRequested = false;
        bool planValidated = false;
        bool directPacketPathUsed = false;
        uint32 plannedPacketCount = 0;
        uint32 compiledPacketCount = 0;
        uint32 executedPacketCount = 0;
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

    [[nodiscard]] inline bool HasCurrentGraphProvenance(
        const RGBufferHandle& handle,
        const RenderPassRecordIdentity& identity) noexcept
    {
        return !handle.IsValid() ||
               (handle.graphIdentity == identity.graphIdentity &&
                handle.recordingGeneration ==
                    identity.graphRecordingGeneration);
    }

    /** @brief Validate every optional RenderGraph resource stored in ViewData. */
    [[nodiscard]] inline bool HasCurrentViewDataGraphResources(
        const ViewData& view,
        const RenderPassRecordIdentity& identity) noexcept
    {
        return HasCurrentGraphProvenance(view.colorTarget, identity) &&
               HasCurrentGraphProvenance(view.depthTarget, identity) &&
               HasCurrentGraphProvenance(view.velocityTarget, identity);
    }

    /** @brief Typed GPU-culling inputs consumed by one graphics pass. */
    struct RenderPassGPUDrivenInputs
    {
        RenderPassRecordIdentity identity{};
        std::shared_ptr<GPUCullingRecordedState> recordedState;
        RGBufferHandle instances{};
        RGBufferHandle instanceIndices{};
        RGBufferHandle indirectDraws{};
        RGBufferHandle drawCount{};

        [[nodiscard]] bool HasCompleteHandles() const noexcept
        {
            return instances.IsValid() && instanceIndices.IsValid() &&
                   indirectDraws.IsValid() && drawCount.IsValid();
        }

        [[nodiscard]] bool IsCompatibleWith(
            const RenderPassRecordIdentity& expected) const noexcept
        {
            return recordedState != nullptr &&
                   identity == expected &&
                   instances.IsValid() && instanceIndices.IsValid() &&
                   indirectDraws.IsValid() && drawCount.IsValid() &&
                   HasCurrentGraphProvenance(instances, expected) &&
                   HasCurrentGraphProvenance(instanceIndices, expected) &&
                   HasCurrentGraphProvenance(indirectDraws, expected) &&
                   HasCurrentGraphProvenance(drawCount, expected);
        }
    };

    /** @brief Value-owned scene state used by typed main-scene pass recording. */
    struct RenderPassFrameSnapshot
    {
        RenderPassRecordIdentity identity{};
        ViewData view{};
        RenderFrameExecutionPlan executionPlan{};
        SceneMeshPassPreparation meshPassPreparation{};
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
    };

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

    /** @brief Lifetime-owned mutable outputs for one graph recording. */
    struct RenderPassRecordResults
    {
        RenderPassRecordIdentity identity{};
        DirectionalShadowRecordOutput directionalShadowOutput{};
        RayTracedShadowRecordOutput rayTracedShadowOutput{};
        RenderFrameExecutionReport executionReport{};
        DepthPrepassDrawStats depthStats{};
        ShadowPassStats shadowStats{};
        RayTracedShadowPassStats rayTracedShadowStats{};
        OpaquePassDrawStats opaqueStats{};
        OpaquePassShadowStats opaqueShadowStats{};
        ObjectVelocityPassStats objectVelocityStats{};
    };

    /**
     * @brief Immutable source context used to create graph-owned pass data.
     *
     * The ViewData snapshot is intentionally value-owned.  Scene references are
     * frame-retained by the renderer and are copied into typed pass data before
     * callbacks are registered.
     */
    struct RenderPassRecordContext
    {
        ViewData view{};
        RenderPassRecordIdentity identity{};
        const RenderFrameExecutionPlan* executionPlan = nullptr;
        const SceneMeshPassPreparation* meshPassPreparation = nullptr;
        const RenderVisibilityResult* visibility = nullptr;
        RenderFrameExecutionReport* executionReport = nullptr;
        const RenderScene* renderScene = nullptr;
        const std::vector<RenderDrawItem>* opaqueDrawItems = nullptr;
        const std::vector<RenderDrawItem>* maskedDrawItems = nullptr;
        const std::vector<RenderDrawItem>* transparentDrawItems = nullptr;
        RenderPassGPUDrivenInputs depthGPUDriven{};
        RenderPassGPUDrivenInputs opaqueGPUDriven{};
        DirectionalShadowRecordOutput directionalShadow{};
        RayTracedShadowRecordOutput rayTracedShadow{};
        std::shared_ptr<const RenderPassFrameSnapshot> frameSnapshot;
        std::shared_ptr<RenderPassRecordResults> results;
        /// True only for the legacy ViewData adapter.  Migrated scene passes
        /// reject it and require a renderer-issued recording epoch.
        bool legacyAdapter = false;

        [[nodiscard]] bool MatchesTargetGraph(
            const RenderGraph& targetGraph) const noexcept
        {
            return identity.Matches(targetGraph) &&
                   (view.renderGraph == nullptr ||
                    view.renderGraph == &targetGraph) &&
                   HasCurrentViewDataGraphResources(view, identity);
        }

        [[nodiscard]] bool IsFrameIdentityValid() const noexcept
        {
            if (!identity.IsValid() || !MatchesTargetGraph(*identity.graph))
            {
                return false;
            }
            if (view.renderGraph != nullptr && view.renderGraph != identity.graph)
            {
                return false;
            }
            if (view.renderFrameExecutionPlan != executionPlan ||
                view.meshPassPreparation != meshPassPreparation ||
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
            if (executionReport != nullptr && executionReport->frameSequence != 0 &&
                executionReport->frameSequence != identity.frameSequence)
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
        DirectionalShadowRecordOutput directionalShadow{};
        RayTracedShadowRecordOutput rayTracedShadow{};

        [[nodiscard]] const RenderFrameExecutionPlan* GetExecutionPlan() const
        {
            return frameSnapshot ? &frameSnapshot->executionPlan : nullptr;
        }
        [[nodiscard]] const SceneMeshPassPreparation* GetMeshPassPreparation() const
        {
            return frameSnapshot ? &frameSnapshot->meshPassPreparation : nullptr;
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
                view.renderGraph != identity.graph ||
                !HasCurrentViewDataGraphResources(view, identity))
            {
                return false;
            }
            if (!frameSnapshot || !results ||
                frameSnapshot->identity != identity ||
                results->identity != identity ||
                view.renderFrameExecutionPlan != &frameSnapshot->executionPlan ||
                view.meshPassPreparation != &frameSnapshot->meshPassPreparation ||
                view.renderVisibility != &frameSnapshot->visibility ||
                view.renderFrameExecutionReport != &results->executionReport)
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
                   view.renderGraph == &targetGraph &&
                   HasCurrentViewDataGraphResources(view, identity);
        }
    };

    [[nodiscard]] inline std::shared_ptr<const RenderPassFrameSnapshot>
    MakeRenderPassFrameSnapshot(const RenderPassRecordContext& context,
                                RenderPassRecordResults& results)
    {
        auto snapshot = std::make_shared<RenderPassFrameSnapshot>();
        snapshot->identity = context.identity;
        snapshot->view = context.view;
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
        if (context.meshPassPreparation != nullptr)
        {
            snapshot->meshPassPreparation = *context.meshPassPreparation;
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
        results.rayTracedShadowOutput = {};
        results.rayTracedShadowOutput.identity = context.identity;
        results.rayTracedShadowStats = {};
        results.executionReport = context.executionReport != nullptr
            ? *context.executionReport : RenderFrameExecutionReport{};
        if (results.executionReport.frameSequence == 0)
        {
            results.executionReport.frameSequence = context.identity.frameSequence;
        }
        snapshot->view.renderGraph = context.identity.graph;
        snapshot->view.renderFrameExecutionPlan = &snapshot->executionPlan;
        snapshot->view.meshPassPreparation = &snapshot->meshPassPreparation;
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
        if (result.frameSnapshot)
        {
            result.view = result.frameSnapshot->view;
        }
        result.directionalShadow = context.directionalShadow;
        result.rayTracedShadow = context.rayTracedShadow;
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
        context.meshPassPreparation = view.meshPassPreparation;
        context.visibility = view.renderVisibility;
        context.executionReport = view.renderFrameExecutionReport;
        context.legacyAdapter = true;
        return context;
    }
} // namespace RVX
