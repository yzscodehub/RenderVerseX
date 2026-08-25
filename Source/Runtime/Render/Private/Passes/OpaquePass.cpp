/**
 * @file OpaquePass.cpp
 * @brief Opaque geometry render pass implementation
 */

#include "Render/Passes/OpaquePass.h"
#include "Render/Animation/RenderDeformationFrameResources.h"
#include "Render/Submission/DirectRasterReadbackQualification.h"
#include "Render/Submission/OpaqueRasterSubmission.h"

#include "Passes/MaterialTextureGraphBindings.h"
#include "Core/Log.h"
#include "Render/GPUDriven/GPUCulling.h"
#include "Render/Graph/ResourceViewCache.h"
#include "Render/Lighting/ClusteredLighting.h"
#include "Render/Lighting/LightingFramePlan.h"
#include "Render/Lighting/LightManager.h"
#include "Render/Material/MaterialSystem.h"
#include "Render/PipelineCache.h"
#include "Render/Passes/DirectDrawPacketBatch.h"
#include "Render/Passes/RenderPassClearValues.h"
#include "Render/Renderer/RenderScene.h"
#include "Render/Renderer/ViewData.h"
#include "Render/Submission/RenderSubmissionStrategy.h"
#include "Resources/RenderResourceResolver.h"
#include "Resources/RenderSubmissionResourceBatch.h"
#include "RHI/RHIRenderPass.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>

namespace RVX
{

namespace
{
    class RequiredOpaqueExecutionGuard final
    {
    public:
        explicit RequiredOpaqueExecutionGuard(
            RenderGraphPassContext& context) noexcept
            : m_context(context)
        {
        }

        ~RequiredOpaqueExecutionGuard()
        {
            if (!m_succeeded)
            {
                m_context.MarkExecutionFailed(
                    "Opaque raster pass did not record its required workload");
            }
        }

        void MarkSucceeded() noexcept
        {
            m_succeeded = true;
        }

    private:
        RenderGraphPassContext& m_context;
        bool m_succeeded = false;
    };

    [[nodiscard]] bool SamePaletteMetadata(
        const RenderSkinningPaletteMetadata& left,
        const RenderSkinningPaletteMetadata& right) noexcept
    {
        return left.providerComponentId == right.providerComponentId &&
               left.sourceModelResourceId == right.sourceModelResourceId &&
               left.poseSequence == right.poseSequence &&
               left.paletteHash == right.paletteHash &&
               left.paletteCount == right.paletteCount;
    }

    [[nodiscard]] bool ResolveSkinningPaletteBinding(
        const RenderObject& object,
        const MeshGPUBuffers& buffers,
        const RenderDeformationFrameResources* resources,
        SkinningPaletteBufferBinding& outBuffers,
        SkinningPaletteSliceBinding& outSlice) noexcept
    {
        outBuffers = {};
        outSlice = {};
        if (!object.HasSkinningData())
            return true;

        if (!buffers.HasSkinningVertexData() || resources == nullptr ||
            !resources->IsComplete() || object.entityId == 0 ||
            !object.HasValidSkinningPalette())
        {
            return false;
        }
        const RenderDeformationPaletteSlice* const slice =
            resources->FindSlice(object.entityId);
        if (slice == nullptr || !slice->IsStructurallyValid() ||
            slice->matrixCount != object.skinningMatrices.size() ||
            !SamePaletteMetadata(slice->currentMetadata, object.skinningPalette) ||
            slice->currentOffset > resources->GetMatrixCount() - slice->matrixCount ||
            slice->previousOffset > resources->GetMatrixCount() - slice->matrixCount)
        {
            return false;
        }
        if (slice->previousValid)
        {
            if (!object.HasValidPreviousSkinningPalette() ||
                !SamePaletteMetadata(
                    slice->previousMetadata, object.previousSkinningPalette))
            {
                return false;
            }
        }
        else if (object.HasValidPreviousSkinningPalette() ||
                 slice->previousMetadata.paletteCount != 0)
        {
            return false;
        }

        outBuffers.currentPaletteBuffer = resources->GetCurrentPaletteBuffer();
        outBuffers.previousPaletteBuffer = resources->GetPreviousPaletteBuffer();
        outSlice.currentOffset = slice->currentOffset;
        outSlice.previousOffset = slice->previousOffset;
        outSlice.matrixCount = slice->matrixCount;
        outSlice.previousValid = slice->previousValid;
        return outBuffers.HasPalettes() && outSlice.IsWellFormed();
    }

    [[nodiscard]] bool HasExactDeformationBuffers(
        RenderGraphPassContext& context,
        const RenderDeformationFrameResources* resources,
        RGBufferHandle currentHandle,
        RGBufferHandle previousHandle) noexcept
    {
        return resources != nullptr && resources->IsComplete() &&
               resources->GetMatrixCount() != 0 &&
               currentHandle.IsValid() && previousHandle.IsValid() &&
               context.GetBuffer(currentHandle) ==
                   resources->GetCurrentPaletteBuffer().Get() &&
               context.GetBuffer(previousHandle) ==
                   resources->GetPreviousPaletteBuffer().Get();
    }

    [[nodiscard]] bool RetainLightingBindingSnapshot(
        RenderGraphPassContext& context,
        const LightingBindingSnapshot& bindings)
    {
        return bindings.IsValid() &&
            context.RetainSubmissionResource(
                Ref<RefCounted>(bindings.viewConstantBuffer.Get())) &&
            context.RetainSubmissionResource(
                Ref<RefCounted>(bindings.directionalShadowView.Get())) &&
            context.RetainSubmissionResource(
                Ref<RefCounted>(bindings.directionalShadowSampler.Get())) &&
            context.RetainSubmissionResource(
                Ref<RefCounted>(bindings.lightConstantsBuffer.Get())) &&
            context.RetainSubmissionResource(
                Ref<RefCounted>(bindings.pointLightsBuffer.Get())) &&
            context.RetainSubmissionResource(
                Ref<RefCounted>(bindings.spotLightsBuffer.Get())) &&
            context.RetainSubmissionResource(
                Ref<RefCounted>(bindings.rayTracedShadowMaskView.Get())) &&
            context.RetainSubmissionResource(
                Ref<RefCounted>(bindings.clusterConstantsBuffer.Get())) &&
            context.RetainSubmissionResource(
                Ref<RefCounted>(bindings.clusterBuffer.Get())) &&
            context.RetainSubmissionResource(
                Ref<RefCounted>(bindings.clusterLightIndexBuffer.Get())) &&
            context.RetainSubmissionResource(
                Ref<RefCounted>(bindings.environmentIBLIrradianceView.Get())) &&
            context.RetainSubmissionResource(
                Ref<RefCounted>(bindings.environmentIBLPrefilteredView.Get())) &&
            context.RetainSubmissionResource(
                Ref<RefCounted>(bindings.environmentIBLBRDFLUTView.Get())) &&
            context.RetainSubmissionResource(
                Ref<RefCounted>(bindings.environmentIBLSampler.Get())) &&
            context.RetainSubmissionResource(
                Ref<RefCounted>(bindings.ambientOcclusionView.Get())) &&
            context.RetainSubmissionResource(
                Ref<RefCounted>(bindings.exposureStateBuffer.Get())) &&
            context.RetainSubmissionResource(
                Ref<RefCounted>(bindings.frameSetLayout.Get())) &&
            context.RetainSubmissionResource(
                Ref<RefCounted>(bindings.frameDescriptorSet.Get()));
    }

    [[nodiscard]] RGTextureViewHandle DeclareExactIBLRead(
        RenderGraphBuilder& builder,
        RGTextureHandle texture,
        const RHITextureViewRef& retainedView,
        const char* debugName)
    {
        const RHITextureDesc* const textureDesc =
            builder.GetTextureDesc(texture);
        if (!texture.IsValid() || textureDesc == nullptr || !retainedView ||
            retainedView->GetTexture() == nullptr)
        {
            return {};
        }

        RHITextureViewDesc viewDesc;
        viewDesc.format = retainedView->GetFormat();
        viewDesc.dimension = textureDesc->dimension;
        viewDesc.subresourceRange = retainedView->GetSubresourceRange();
        viewDesc.type = RHITextureViewType::ShaderResource;
        viewDesc.debugName = debugName;
        RGTextureViewHandle view = builder.CreateTextureView(texture, viewDesc);
        return builder.Read(
            view,
            MakeRGAccessDesc(
                RHIResourceState::ShaderResource,
                RHIShaderStage::Pixel));
    }

    bool HasDrawFlag(RenderDrawFlags flags, RenderDrawFlags flag) noexcept
    {
        return (static_cast<uint32>(flags) & static_cast<uint32>(flag)) != 0;
    }

    bool IsMaskedPacket(const RenderDrawPacket& packet) noexcept
    {
        return packet.pipelineKey.materialVariant == MaterialPipelineVariant::Masked;
    }

    bool IsSkinnedPacket(const RenderDrawPacket& packet) noexcept
    {
        return HasDrawFlag(packet.flags, RenderDrawFlags::Skinned) ||
            packet.pipelineKey.skinned;
    }

    [[nodiscard]] bool IsOpaqueSurfaceEligibility(
        SurfacePathEligibility eligibility) noexcept
    {
        return eligibility == SurfacePathEligibility::DeferredOpaque ||
            eligibility == SurfacePathEligibility::ForwardSpecialOpaque;
    }

    [[nodiscard]] bool IsSurfaceEligibilitySelectedForRoute(
        SurfacePathEligibility eligibility,
        OpaqueRasterSurfaceRoute route) noexcept
    {
        switch (route)
        {
            case OpaqueRasterSurfaceRoute::AllOpaque:
                return IsOpaqueSurfaceEligibility(eligibility);
            case OpaqueRasterSurfaceRoute::DeferredOpaque:
                return eligibility == SurfacePathEligibility::DeferredOpaque;
            case OpaqueRasterSurfaceRoute::ForwardSpecialOpaque:
                return eligibility == SurfacePathEligibility::ForwardSpecialOpaque;
            default:
                return false;
        }
    }

    [[nodiscard]] bool IsPacketSelectedForSurfaceRoute(
        const RenderDrawPacket& packet,
        OpaqueRasterSurfaceRoute route) noexcept
    {
        if (!packet.pipelineKey.materialPolicyValid ||
            !IsOpaqueSurfaceEligibility(packet.pipelineKey.surfacePathEligibility))
        {
            return false;
        }

        return IsSurfaceEligibilitySelectedForRoute(
            packet.pipelineKey.surfacePathEligibility, route);
    }

    [[nodiscard]] bool IsPacketInvalidForOpaqueRoute(
        const RenderDrawPacket& packet) noexcept
    {
        return packet.pass != RenderPassKind::Opaque ||
            packet.pipelineKey.materialVariant ==
                MaterialPipelineVariant::Transparent ||
            !packet.pipelineKey.materialPolicyValid ||
            !IsOpaqueSurfaceEligibility(packet.pipelineKey.surfacePathEligibility);
    }

    [[nodiscard]] bool IsDrawItemSelectedForSurfaceRoute(
        const RenderDrawItem& item,
        OpaqueRasterSurfaceRoute route) noexcept
    {
        return IsPacketSelectedForSurfaceRoute(item.packet, route);
    }

    OpaqueRasterPipelineRequest MakeOpaquePipelineRequest(
        OpaqueRasterOutputFamily outputFamily,
        OpaqueRasterSurfaceRoute surfaceRoute,
        OpaqueRasterSubmissionRoute route,
        MaterialPipelineVariant materialVariant,
        RHIFormat colorTargetFormat,
        MaterialCullMode cullMode,
        bool reverseWinding,
        bool skinned,
        bool usesMaterialParameterTable) noexcept
    {
        OpaqueRasterPipelineRequest request;
        request.outputFamily = outputFamily;
        request.surfaceRoute = surfaceRoute;
        request.route = route;
        request.materialVariant = materialVariant;
        request.cullMode = cullMode;
        request.colorTargetFormat = colorTargetFormat;
        request.reverseWinding = reverseWinding;
        request.skinned = skinned;
        request.usesMaterialParameterTable = usesMaterialParameterTable;
        return request;
    }

    RenderSubmissionLayout MakeExpectedDirectLayout(
        const RenderDrawPacket& packet) noexcept
    {
        RenderSubmissionLayout layout;
        layout.vertexStreams = MeshPassVertexStreams::Position |
            MeshPassVertexStreams::Normal |
            MeshPassVertexStreams::TexCoord |
            MeshPassVertexStreams::Tangent;
        if (packet.pipelineKey.requiresUV1)
        {
            layout.vertexStreams |= MeshPassVertexStreams::TexCoord1;
        }
        layout.bindings = MeshPassBindingRequirements::Frame |
            MeshPassBindingRequirements::Object |
            MeshPassBindingRequirements::Geometry;
        layout.bindings |= HasDrawFlag(
            packet.flags, RenderDrawFlags::MissingMaterial)
            ? MeshPassBindingRequirements::DefaultMaterial
            : MeshPassBindingRequirements::Material;
        if (IsSkinnedPacket(packet))
        {
            layout.vertexStreams |= MeshPassVertexStreams::BoneIndices;
            layout.vertexStreams |= MeshPassVertexStreams::BoneWeights;
            layout.bindings |= MeshPassBindingRequirements::Skinning;
        }
        layout.primitiveDataBinding = PrimitiveDataBinding::PerDrawConstants;
        return layout;
    }

    bool ValidateRetainedObjectIdentityIndex(const RenderScene& scene) noexcept
    {
        for (size_t index = 0; index < scene.GetObjectCount(); ++index)
        {
            const RenderObject& object = scene.GetObject(index);
            if (object.entityId == 0 ||
                scene.FindObject(object.entityId) != &object)
            {
                return false;
            }
        }
        return true;
    }

    const RenderObject* ResolveUniqueObject(const RenderScene& scene,
                                            RenderObjectId objectId,
                                            uint32 primitiveData) noexcept
    {
        if (objectId == 0 || primitiveData >= scene.GetObjectCount())
        {
            return nullptr;
        }

        const RenderObject& indexedObject = scene.GetObject(primitiveData);
        if (indexedObject.entityId != objectId)
        {
            return nullptr;
        }

        const RenderObject* const uniqueObject = scene.FindObject(objectId);
        if (uniqueObject == nullptr || uniqueObject != &indexedObject)
        {
            return nullptr;
        }
        return uniqueObject;
    }

    bool HasMeshBufferSemantic(const RenderMeshResourceData& mesh,
                               RenderMeshBufferSemantic semantic) noexcept
    {
        return std::any_of(mesh.buffers.begin(),
                           mesh.buffers.end(),
                           [semantic](const RenderOwnedBuffer& owned)
                           {
                               return owned.semantic == semantic &&
                                   owned.buffer.Get() != nullptr;
                           });
    }

    const MeshUploadSubmesh* ResolveMeshSubmesh(
        const RenderMeshResourceData& mesh,
        uint32 submeshIndex,
        MeshUploadSubmesh& syntheticSubmesh) noexcept
    {
        if (submeshIndex < mesh.submeshes.size())
        {
            return &mesh.submeshes[submeshIndex];
        }
        if (submeshIndex == 0 && mesh.submeshes.empty() &&
            mesh.createInfo.indexCount != 0)
        {
            syntheticSubmesh.indexCount =
                static_cast<uint32>(mesh.createInfo.indexCount);
            return &syntheticSubmesh;
        }
        return nullptr;
    }

    const RenderPassExecutionPlan* FindPassExecutionPlan(
        const RenderFrameExecutionPlan* plan,
        RenderPassKind pass)
    {
        if (plan == nullptr)
        {
            return nullptr;
        }
        for (const RenderPassExecutionPlan& candidate : plan->passes)
        {
            if (candidate.pass == pass)
            {
                return &candidate;
            }
        }
        return nullptr;
    }

    bool IsGPUDrivenPassPlanned(const RenderFrameExecutionPlan* plan,
                                RenderPassKind pass)
    {
        const RenderPassExecutionPlan* passPlan =
            FindPassExecutionPlan(plan, pass);
        return passPlan != nullptr &&
               passPlan->partition.gpuDrivenPacketCount != 0;
    }

    struct OpaqueRoutePlanCounts
    {
        uint32 gpuPacketCount = 0;
        uint32 directPacketCount = 0;
        uint32 skippedPacketCount = 0;
        uint32 gpuDrawGroupCount = 0;
        bool valid = true;
    };

    [[nodiscard]] bool CountOpaqueRoutePackets(
        const RenderFrameExecutionPlan& plan,
        const RenderPassExecutionPlan& opaquePlan,
        OpaqueRasterSurfaceRoute route,
        OpaqueRoutePlanCounts& outCounts) noexcept
    {
        outCounts = {};
        const auto countRange = [&plan, route, &outCounts](
                                    DrawPacketRange range,
                                    uint32& destination) noexcept
        {
            const uint64 end = static_cast<uint64>(range.first) + range.count;
            if (end > plan.packetReferences.size())
            {
                outCounts.valid = false;
                return;
            }
            for (uint64 index = range.first; index < end; ++index)
            {
                const RenderDrawPacketReference& reference =
                    plan.packetReferences[static_cast<size_t>(index)];
                const RenderDrawPacket& packet = reference.sourceSignature.packet;
                if (reference.pass != RenderPassKind::Opaque ||
                    IsPacketInvalidForOpaqueRoute(packet))
                {
                    outCounts.valid = false;
                    return;
                }
                if (IsPacketSelectedForSurfaceRoute(packet, route))
                {
                    ++destination;
                }
            }
        };

        countRange(opaquePlan.gpuEligiblePackets, outCounts.gpuPacketCount);
        if (!outCounts.valid)
        {
            return false;
        }
        countRange(opaquePlan.directPackets, outCounts.directPacketCount);
        if (!outCounts.valid)
        {
            return false;
        }
        countRange(opaquePlan.skippedPackets, outCounts.skippedPacketCount);
        if (!outCounts.valid)
        {
            return false;
        }
        return true;
    }

    void PublishOpaqueContextFailure(const RenderPassExecutionData& execution,
                                     OpaquePassDrawStats& stats)
    {
        stats = {};
        const RenderFrameExecutionPlan* executionPlan =
            execution.GetExecutionPlan();
        stats.planRequested = executionPlan != nullptr;
        stats.failureReason = RenderPolicyReason::InconsistentFacts;
        RenderFrameExecutionReport* executionReport =
            execution.GetExecutionReport();
        if (executionReport == nullptr)
        {
            return;
        }
        RenderFrameExecutionReport& frameReport = *executionReport;
        frameReport.status = RenderExecutionStatus::Failed;
        if (frameReport.frameSequence == 0)
        {
            frameReport.frameSequence = execution.identity.frameSequence;
        }

        const RenderPassExecutionPlan* passPlan = FindPassExecutionPlan(
            executionPlan, RenderPassKind::Opaque);
        const bool gpuLanePlanned = passPlan != nullptr &&
            passPlan->partition.gpuDrivenPacketCount != 0;
        const bool directLanePlanned = passPlan != nullptr &&
            passPlan->partition.directPacketCount != 0;
        const auto publishLaneFailure = [reason = stats.failureReason](
                                            RenderPassLaneExecutionReport& lane,
                                            bool planned)
        {
            lane.status = planned
                ? RenderExecutionStatus::Failed
                : RenderExecutionStatus::NotAttempted;
            lane.reason = planned ? reason : RenderPolicyReason::None;
            lane.executedCountsAvailable = planned;
            lane.executedPacketCount = 0;
            lane.executedDrawCount = 0;
        };
        for (RenderPassExecutionReport& report : frameReport.passes)
        {
            if (report.pass != RenderPassKind::Opaque)
            {
                continue;
            }
            report.status = RenderExecutionStatus::Failed;
            report.reason = stats.failureReason;
            publishLaneFailure(report.gpuDrivenLane, gpuLanePlanned);
            publishLaneFailure(report.directLane, directLanePlanned);
            break;
        }
    }

    [[nodiscard]] OpaquePassDrawStats CombineOpaqueRouteDrawStats(
        const OpaquePassDrawStats& lhs,
        const OpaquePassDrawStats& rhs) noexcept
    {
        OpaquePassDrawStats result = lhs;
        result.instancingMode = lhs.instancingMode == rhs.instancingMode
            ? lhs.instancingMode
            : RenderInstancingMode::Disabled;
        result.instancingPlanAvailable = lhs.instancingPlanAvailable ||
            rhs.instancingPlanAvailable;
        result.instancingPreflightSucceeded = lhs.instancingPreflightSucceeded &&
            rhs.instancingPreflightSucceeded;
        result.plannedInstancingPacketCount += rhs.plannedInstancingPacketCount;
        result.plannedInstancingDrawCount += rhs.plannedInstancingDrawCount;
        result.plannedInstancingInstanceCount += rhs.plannedInstancingInstanceCount;
        result.plannedInstancingBatchCount += rhs.plannedInstancingBatchCount;
        result.directDrawCount += rhs.directDrawCount;
        result.submittedDrawCount += rhs.submittedDrawCount;
        result.submittedInstanceCount += rhs.submittedInstanceCount;
        result.instancedBatchCount += rhs.instancedBatchCount;
        result.instancingFallbackBatchCount += rhs.instancingFallbackBatchCount;
        result.directInstanceUploadBytes += rhs.directInstanceUploadBytes;
        result.directInstanceIndexUploadBytes += rhs.directInstanceIndexUploadBytes;
        result.directInstancePatchedRowCount += rhs.directInstancePatchedRowCount;
        result.directInstanceIndexPatchedRowCount +=
            rhs.directInstanceIndexPatchedRowCount;
        result.directInstanceActiveCount += rhs.directInstanceActiveCount;
        result.directInstanceActiveCapacity = std::max(
            result.directInstanceActiveCapacity,
            rhs.directInstanceActiveCapacity);
        result.directInstanceFullMaterialization =
            lhs.directInstanceFullMaterialization ||
            rhs.directInstanceFullMaterialization;
        result.directInstanceIndexFullMaterialization =
            lhs.directInstanceIndexFullMaterialization ||
            rhs.directInstanceIndexFullMaterialization;
        // A single transcript cannot represent two independently recorded
        // surface routes. Leave it unavailable rather than publishing one
        // route as if it described the aggregate.
        result.directRasterTranscript = {};
        result.directMutationTotals = {};
        result.directMutationTotalsSaturated =
            lhs.directMutationTotalsSaturated || rhs.directMutationTotalsSaturated;
        result.gpuDrivenRequested = lhs.gpuDrivenRequested ||
            rhs.gpuDrivenRequested;
        result.gpuDrivenCullingReady = lhs.gpuDrivenCullingReady ||
            rhs.gpuDrivenCullingReady;
        result.gpuDrivenPipelineReady = lhs.gpuDrivenPipelineReady ||
            rhs.gpuDrivenPipelineReady;
        result.gpuDrivenEligible = lhs.gpuDrivenEligible ||
            rhs.gpuDrivenEligible;
        result.gpuDrivenSubmitted = lhs.gpuDrivenSubmitted ||
            rhs.gpuDrivenSubmitted;
        result.gpuDrivenIndirectBatchCount += rhs.gpuDrivenIndirectBatchCount;
        result.gpuDrivenIndirectSubmittedDrawUpperBound +=
            rhs.gpuDrivenIndirectSubmittedDrawUpperBound;
        result.gpuDrivenIndirectExecutedDrawCountAvailable =
            lhs.gpuDrivenIndirectExecutedDrawCountAvailable &&
            rhs.gpuDrivenIndirectExecutedDrawCountAvailable;
        result.gpuDrivenIndirectDrawCount =
            result.gpuDrivenIndirectExecutedDrawCountAvailable
                ? lhs.gpuDrivenIndirectDrawCount + rhs.gpuDrivenIndirectDrawCount
                : 0;
        if (lhs.gpuDrivenFallbackReason == GPUDrivenDrawFallbackReason::None ||
            lhs.gpuDrivenFallbackReason == GPUDrivenDrawFallbackReason::Disabled)
        {
            result.gpuDrivenFallbackReason = rhs.gpuDrivenFallbackReason;
        }
        result.skippedMaterialBindingCount += rhs.skippedMaterialBindingCount;
        result.materialBindingCount += rhs.materialBindingCount;
        result.materialFallbackBindingCount += rhs.materialFallbackBindingCount;
        result.materialTextureFlags |= rhs.materialTextureFlags;
        result.materialFallbackTextureFlags |= rhs.materialFallbackTextureFlags;
        result.planRequested = lhs.planRequested || rhs.planRequested;
        result.planValidated = lhs.planValidated && rhs.planValidated;
        result.directPacketPathUsed = lhs.directPacketPathUsed ||
            rhs.directPacketPathUsed;
        result.plannedPacketCount += rhs.plannedPacketCount;
        result.compiledPacketCount += rhs.compiledPacketCount;
        result.executedPacketCount += rhs.executedPacketCount;
        result.failureReason = lhs.failureReason == RenderPolicyReason::None &&
            rhs.failureReason == RenderPolicyReason::None
                ? RenderPolicyReason::None
                : lhs.failureReason != RenderPolicyReason::None
                    ? lhs.failureReason
                    : rhs.failureReason;
        return result;
    }

    [[nodiscard]] OpaquePassShadowStats CombineOpaqueRouteShadowStats(
        const OpaquePassShadowStats& lhs,
        const OpaquePassShadowStats& rhs) noexcept
    {
        OpaquePassShadowStats result;
        result.requested = lhs.requested || rhs.requested;
        result.renderGraphReadDeclared = lhs.renderGraphReadDeclared ||
            rhs.renderGraphReadDeclared;
        result.frameShadowReady = lhs.frameShadowReady || rhs.frameShadowReady;
        result.rayTracedRequested = lhs.rayTracedRequested || rhs.rayTracedRequested;
        result.rayTracedRenderGraphReadDeclared =
            lhs.rayTracedRenderGraphReadDeclared ||
            rhs.rayTracedRenderGraphReadDeclared;
        result.rayTracedFrameMaskReady = lhs.rayTracedFrameMaskReady ||
            rhs.rayTracedFrameMaskReady;
        result.rayTracedExecutionReady = lhs.rayTracedExecutionReady ||
            rhs.rayTracedExecutionReady;
        result.rayTracedExecutionFailed = lhs.rayTracedExecutionFailed ||
            rhs.rayTracedExecutionFailed;
        result.receiverCandidateDrawItemCount =
            lhs.receiverCandidateDrawItemCount + rhs.receiverCandidateDrawItemCount;
        result.shadowReceivingDrawItemCount =
            lhs.shadowReceivingDrawItemCount + rhs.shadowReceivingDrawItemCount;
        result.shadowReceiverOptOutDrawItemCount =
            lhs.shadowReceiverOptOutDrawItemCount +
            rhs.shadowReceiverOptOutDrawItemCount;
        return result;
    }

    void PublishOpaqueRouteResults(
        RenderPassRecordResults& results,
        OpaqueRasterSurfaceRoute route,
        const OpaquePassDrawStats& drawStats,
        const OpaquePassShadowStats& shadowStats,
        RenderExecutionStatus status)
    {
        if (route == OpaqueRasterSurfaceRoute::AllOpaque)
        {
            results.opaqueStats = drawStats;
            results.opaqueShadowStats = shadowStats;
            results.deferredOpaqueRoute = {};
            results.forwardSpecialOpaqueRoute = {};
            return;
        }

        OpaqueSurfaceRouteExecutionEvidence* evidence =
            route == OpaqueRasterSurfaceRoute::DeferredOpaque
                ? &results.deferredOpaqueRoute
                : route == OpaqueRasterSurfaceRoute::ForwardSpecialOpaque
                    ? &results.forwardSpecialOpaqueRoute
                    : nullptr;
        if (evidence == nullptr)
        {
            return;
        }
        evidence->route = route;
        evidence->drawStats = drawStats;
        evidence->shadowStats = shadowStats;
        evidence->status = status;
        evidence->available = true;

        if (results.deferredOpaqueRoute.available &&
            results.forwardSpecialOpaqueRoute.available)
        {
            results.opaqueStats = CombineOpaqueRouteDrawStats(
                results.deferredOpaqueRoute.drawStats,
                results.forwardSpecialOpaqueRoute.drawStats);
            results.opaqueShadowStats = CombineOpaqueRouteShadowStats(
                results.deferredOpaqueRoute.shadowStats,
                results.forwardSpecialOpaqueRoute.shadowStats);
        }
        else
        {
            // Do not expose a partial route as the complete opaque aggregate.
            results.opaqueStats = {};
            results.opaqueShadowStats = {};
        }
    }

    void PublishDeferredOpaqueAggregateExecutionReport(
        const ViewData& view,
        const RenderPassRecordResults& results)
    {
        if (view.renderFrameExecutionReport == nullptr ||
            !results.deferredOpaqueRoute.available ||
            !results.forwardSpecialOpaqueRoute.available)
        {
            return;
        }

        const bool completed =
            results.deferredOpaqueRoute.status == RenderExecutionStatus::Completed &&
            results.forwardSpecialOpaqueRoute.status ==
                RenderExecutionStatus::Completed;
        const OpaquePassDrawStats& aggregate = results.opaqueStats;
        for (RenderPassExecutionReport& report :
             view.renderFrameExecutionReport->passes)
        {
            if (report.pass != RenderPassKind::Opaque)
            {
                continue;
            }
            report.status = completed
                ? RenderExecutionStatus::Completed
                : RenderExecutionStatus::Failed;
            report.reason = completed
                ? RenderPolicyReason::None
                : aggregate.failureReason;
            report.materialBindingsAvailable = completed;
            report.materialBindingCount = aggregate.materialBindingCount;
            report.materialFallbackBindingCount =
                aggregate.materialFallbackBindingCount;
            report.materialTextureFlags = aggregate.materialTextureFlags;
            report.materialFallbackTextureFlags =
                aggregate.materialFallbackTextureFlags;
            report.gpuDrivenLane.status = aggregate.gpuDrivenRequested
                ? (aggregate.gpuDrivenSubmitted
                       ? RenderExecutionStatus::Completed
                       : RenderExecutionStatus::Failed)
                : RenderExecutionStatus::NotAttempted;
            report.gpuDrivenLane.reason =
                report.gpuDrivenLane.status == RenderExecutionStatus::Failed
                    ? RenderPolicyReason::UnexpectedRecordingFailure
                    : RenderPolicyReason::None;
            report.gpuDrivenLane.executedCountsAvailable =
                aggregate.gpuDrivenIndirectExecutedDrawCountAvailable;
            report.gpuDrivenLane.executedPacketCount =
                aggregate.gpuDrivenIndirectExecutedDrawCountAvailable
                    ? aggregate.gpuDrivenIndirectDrawCount
                    : 0;
            report.gpuDrivenLane.executedDrawCount =
                report.gpuDrivenLane.executedPacketCount;
            report.directLane.status = aggregate.directPacketPathUsed
                ? RenderExecutionStatus::Completed
                : RenderExecutionStatus::NotAttempted;
            report.directLane.reason = RenderPolicyReason::None;
            report.directLane.executedCountsAvailable =
                aggregate.directPacketPathUsed;
            report.directLane.executedPacketCount =
                aggregate.executedPacketCount;
            report.directLane.executedDrawCount = aggregate.directDrawCount;
            if (!completed)
            {
                view.renderFrameExecutionReport->status =
                    RenderExecutionStatus::Failed;
            }
            break;
        }
        view.renderFrameExecutionReport->frameSequence =
            view.renderFrameExecutionPlan != nullptr
                ? view.renderFrameExecutionPlan->frameSequence
                : 0;
    }

} // namespace

struct OpaquePass::PlannedOpaqueDraw
{
    DirectDrawPacket packet;
    MeshGPUBuffers buffers;
    SubmeshGPUInfo submesh;
    OpaqueRasterPreparedSubmission rasterSubmission;
    RHIDescriptorSet* frameSet = nullptr;
    ObjectConstantBinding objectBinding;
    MaterialBindingResult materialBinding;
    RenderSkinningPaletteMetadata skinningPalette{};
    SkinningPaletteSliceBinding skinningSlice{};
    bool allowNormalMap = false;
    bool receivesShadow = false;
    bool skinned = false;
    bool usesMaterialParameterTable = false;
    RHIBufferRef instanceIndexBuffer;
    uint32 representedPacketCount = 1;
    bool instanced = false;
};

struct OpaquePass::PlannedGPUDrivenOpaqueDraw
{
    uint32 groupIndex = 0;
    MeshGPUBuffers buffers;
    OpaqueRasterPreparedSubmission rasterSubmission;
    MaterialBindingResult materialBinding;
};

OpaqueRasterPipelinePreflightResult OpaquePass::PreflightRasterPipelines(
    const RenderPassRecordContext& context,
    OpaqueRasterOutputFamily outputFamily,
    OpaqueRasterSurfaceRoute surfaceRoute) const
{
    OpaqueRasterPipelinePreflightResult result;
    const auto fail = [&result](OpaqueRasterPipelinePreflightCode code,
                                std::string diagnostic)
    {
        result.code = code;
        result.diagnostic = std::move(diagnostic);
        return result;
    };
    const auto requestsMatch = [](
                                   const OpaqueRasterPipelineRequest& lhs,
                                   const OpaqueRasterPipelineRequest& rhs) noexcept
    {
        return lhs.outputFamily == rhs.outputFamily &&
               lhs.surfaceRoute == rhs.surfaceRoute &&
               lhs.route == rhs.route &&
               lhs.materialVariant == rhs.materialVariant &&
               lhs.cullMode == rhs.cullMode &&
               lhs.colorTargetFormat == rhs.colorTargetFormat &&
               lhs.reverseWinding == rhs.reverseWinding &&
               lhs.skinned == rhs.skinned &&
               lhs.usesMaterialParameterTable == rhs.usesMaterialParameterTable;
    };
    const auto preflightRequest = [this, &result, &requestsMatch](
                                      const OpaqueRasterPipelineRequest& request,
                                      std::vector<OpaqueRasterPipelineRequest>& requests)
    {
        if (!request.IsValid())
        {
            result.hasFailedRequest = true;
            result.failedRequest = request;
            result.code = OpaqueRasterPipelinePreflightCode::InvalidPacketPartition;
            result.diagnostic =
                "Frozen opaque plan produced an invalid raster pipeline request";
            return false;
        }
        if (std::any_of(requests.begin(), requests.end(),
                        [&requestsMatch, &request](
                            const OpaqueRasterPipelineRequest& existing)
                        {
                            return requestsMatch(existing, request);
                        }))
        {
            return true;
        }

        const OpaqueRasterPipelineSelector selector(*m_pipelineCache);
        if (!selector.Prepare(request).IsReady())
        {
            result.hasFailedRequest = true;
            result.failedRequest = request;
            result.code = OpaqueRasterPipelinePreflightCode::PipelineUnavailable;
            result.diagnostic =
                "Frozen opaque plan requires an unavailable raster pipeline";
            return false;
        }
        requests.push_back(request);
        ++result.uniquePipelineRequestCount;
        return true;
    };

    if (m_pipelineCache == nullptr || m_pipelineCache->GetDevice() == nullptr)
    {
        return fail(OpaqueRasterPipelinePreflightCode::DependenciesUnavailable,
                    "Opaque raster pipeline cache dependency is unavailable");
    }
    if (outputFamily == OpaqueRasterOutputFamily::DeferredGBuffer &&
        (!m_pipelineCache->IsGBufferPipelineFamilyReady() ||
         surfaceRoute != OpaqueRasterSurfaceRoute::DeferredOpaque))
    {
        return fail(OpaqueRasterPipelinePreflightCode::PipelineUnavailable,
                    "Deferred opaque GBuffer pipeline family metadata is unavailable");
    }
    if (outputFamily == OpaqueRasterOutputFamily::ForwardSceneHDR &&
        surfaceRoute == OpaqueRasterSurfaceRoute::DeferredOpaque)
    {
        return fail(OpaqueRasterPipelinePreflightCode::InvalidPacketPartition,
                    "DeferredOpaque surface route requires the GBuffer output family");
    }
    if (context.legacyAdapter || !context.identity.IsValid() ||
        context.identity.graph == nullptr ||
        !context.MatchesTargetGraph(*context.identity.graph) ||
        !context.IsFrameIdentityValid())
    {
        return fail(OpaqueRasterPipelinePreflightCode::InvalidRecordContext,
                    "Frozen opaque pipeline preflight requires a current typed record context");
    }

    RHIFormat colorTargetFormat = RHIFormat::RGBA8_UNORM_SRGB;
    if (outputFamily == OpaqueRasterOutputFamily::ForwardSceneHDR)
    {
        const RHITextureDesc* const colorTargetDesc =
            context.view.colorTarget.IsValid()
                ? context.identity.graph->GetTextureDesc(context.view.colorTarget)
                : nullptr;
        if (colorTargetDesc == nullptr)
        {
            return fail(OpaqueRasterPipelinePreflightCode::InvalidRecordContext,
                        "Frozen forward opaque pipeline preflight has no color target");
        }
        colorTargetFormat = colorTargetDesc->format;
    }
    if (context.executionPlan == nullptr ||
        context.executionPlan->frameSequence != context.identity.frameSequence ||
        context.executionPlan->viewOrdinal != context.identity.viewOrdinal ||
        !ValidateRenderFrameExecutionPlan(*context.executionPlan))
    {
        return fail(OpaqueRasterPipelinePreflightCode::InvalidExecutionPlan,
                    "Frozen opaque pipeline preflight received an invalid execution plan");
    }

    const RenderPassExecutionPlan* const opaquePlan = FindPassExecutionPlan(
        context.executionPlan, RenderPassKind::Opaque);
    if (opaquePlan == nullptr)
    {
        return fail(OpaqueRasterPipelinePreflightCode::InvalidExecutionPlan,
                    "Frozen execution plan has no opaque pass partition");
    }

    std::vector<OpaqueRasterPipelineRequest> requests;
    try
    {
        requests.reserve(static_cast<size_t>(opaquePlan->directPackets.count) +
                         opaquePlan->partition.drawGroupCount);
    }
    catch (...)
    {
        return fail(OpaqueRasterPipelinePreflightCode::PipelineUnavailable,
                    "Frozen opaque pipeline preflight could not retain request state");
    }

    const auto preflightPacketRange = [&context,
                                       &result,
                                       &preflightRequest,
                                       &requests,
                                       outputFamily,
                                       colorTargetFormat,
                                       surfaceRoute](DrawPacketRange range,
                                                     OpaqueRasterSubmissionRoute route)
    {
        for (uint32 offset = 0; offset < range.count; ++offset)
        {
            const uint64 referenceIndex = static_cast<uint64>(range.first) + offset;
            if (referenceIndex >= context.executionPlan->packetReferences.size())
            {
                result.code = OpaqueRasterPipelinePreflightCode::InvalidPacketPartition;
                result.diagnostic =
                    "Frozen opaque pipeline range escapes the execution-plan packet table";
                return false;
            }

            const RenderDrawPacketReference& reference =
                context.executionPlan->packetReferences[
                    static_cast<size_t>(referenceIndex)];
            const RenderDrawPacket& packet = reference.sourceSignature.packet;
            if (reference.pass != RenderPassKind::Opaque ||
                IsPacketInvalidForOpaqueRoute(packet))
            {
                result.code = OpaqueRasterPipelinePreflightCode::InvalidPacketPartition;
                result.diagnostic =
                    "Frozen opaque pipeline partition contains an invalid opaque packet";
                return false;
            }
            if (!IsPacketSelectedForSurfaceRoute(packet, surfaceRoute))
            {
                continue;
            }

            if (route == OpaqueRasterSubmissionRoute::Direct)
            {
                ++result.selectedDirectPacketCount;
            }
            else
            {
                ++result.selectedGPUDrivenPacketCount;
            }
            if (!preflightRequest(MakeOpaquePipelineRequest(
                                      outputFamily,
                                      surfaceRoute,
                                      route,
                                      packet.pipelineKey.materialVariant,
                                      colorTargetFormat,
                                      packet.pipelineKey.cullMode,
                                      packet.pipelineKey.reverseWinding,
                                      route == OpaqueRasterSubmissionRoute::Direct &&
                                          IsSkinnedPacket(packet),
                                      false),
                                  requests))
            {
                return false;
            }
        }
        return true;
    };

    // Deferred geometry intentionally does not infer CPU direct-instancing
    // from the view's batch plan. The published Direct lane is preflighted as
    // Direct; only a sealed GPU lane may select a GPU submission route.
    if (!preflightPacketRange(opaquePlan->directPackets,
                              OpaqueRasterSubmissionRoute::Direct))
    {
        return result;
    }

    // Skipped packets never require a pipeline. Their immutable identities do
    // still need to uphold the opaque policy, otherwise this route's frozen
    // partition is malformed even though it has no work to submit.
    for (uint32 offset = 0; offset < opaquePlan->skippedPackets.count; ++offset)
    {
        const uint64 referenceIndex =
            static_cast<uint64>(opaquePlan->skippedPackets.first) + offset;
        if (referenceIndex >= context.executionPlan->packetReferences.size())
        {
            return fail(OpaqueRasterPipelinePreflightCode::InvalidPacketPartition,
                        "Frozen skipped opaque range escapes the execution-plan packet table");
        }
        const RenderDrawPacketReference& reference =
            context.executionPlan->packetReferences[
                static_cast<size_t>(referenceIndex)];
        if (reference.pass != RenderPassKind::Opaque ||
            IsPacketInvalidForOpaqueRoute(reference.sourceSignature.packet))
        {
            return fail(OpaqueRasterPipelinePreflightCode::InvalidPacketPartition,
                        "Frozen skipped opaque partition contains an invalid opaque packet");
        }
    }

    if (opaquePlan->gpuEligiblePackets.count != 0)
    {
        // Count the selected frozen packet set before asking for a GPU
        // recording. A route with only ForwardSpecial packets does not make
        // DeferredGeometry depend on their later GPU submission.
        uint32 selectedGPUPlanPackets = 0;
        for (uint32 offset = 0; offset < opaquePlan->gpuEligiblePackets.count;
             ++offset)
        {
            const uint64 referenceIndex =
                static_cast<uint64>(opaquePlan->gpuEligiblePackets.first) + offset;
            if (referenceIndex >= context.executionPlan->packetReferences.size())
            {
                return fail(OpaqueRasterPipelinePreflightCode::InvalidPacketPartition,
                            "Frozen GPU opaque range escapes the execution-plan packet table");
            }
            const RenderDrawPacketReference& reference =
                context.executionPlan->packetReferences[
                    static_cast<size_t>(referenceIndex)];
            const RenderDrawPacket& packet = reference.sourceSignature.packet;
            if (reference.pass != RenderPassKind::Opaque ||
                IsPacketInvalidForOpaqueRoute(packet))
            {
                return fail(OpaqueRasterPipelinePreflightCode::InvalidPacketPartition,
                            "Frozen GPU opaque partition contains an invalid opaque packet");
            }
            if (IsPacketSelectedForSurfaceRoute(packet, surfaceRoute))
            {
                ++selectedGPUPlanPackets;
            }
        }

        if (selectedGPUPlanPackets != 0)
        {
            const RenderPassGPUDrivenInputs& inputs = context.opaqueGPUDriven;
            if (inputs.recordedState == nullptr || !inputs.HasCompleteHandles())
            {
                return fail(OpaqueRasterPipelinePreflightCode::MissingGPURecording,
                            "Frozen deferred opaque GPU lane has no complete sealed recording");
            }
            if (inputs.identity != context.identity ||
                !inputs.recordedState->Matches(GPUCullingRecordingIdentity{
                    context.identity.graphIdentity,
                    context.identity.graphRecordingGeneration,
                    context.identity.frameSequence,
                    context.identity.viewOrdinal,
                    context.identity.recordEpoch}))
            {
                return fail(OpaqueRasterPipelinePreflightCode::StaleGPURecording,
                            "Frozen deferred opaque GPU recording identity is stale");
            }
            if (!inputs.IsCompatibleWith(context.identity))
            {
                return fail(OpaqueRasterPipelinePreflightCode::StaleGPURecording,
                            "Frozen deferred opaque GPU recording is not valid for this graph");
            }
            if (inputs.gpuSceneRasterEnabled && inputs.gpuSceneRecordingFailure &&
                inputs.gpuSceneRecordingFailure->load())
            {
                return fail(OpaqueRasterPipelinePreflightCode::StaleGPURecording,
                            "Frozen deferred opaque GPU-scene recording is already failed");
            }
            if (context.meshPassPreparation == nullptr ||
                !ValidatePlannedGPUDrivenPacketRange(
                    *context.executionPlan,
                    RenderPassKind::Opaque,
                    context.meshPassPreparation->opaque))
            {
                return fail(OpaqueRasterPipelinePreflightCode::InvalidPacketPartition,
                            "Frozen deferred opaque GPU lane does not match its prepared packet stream");
            }

            const GPUCulling& culling = inputs.recordedState->GetCulling();
            const std::vector<GPUCullingDrawGroup>& groups = culling.GetDrawGroups();
            for (const GPUCullingDrawGroup& group : groups)
            {
                const PipelineKey& pipeline = group.batchKey.pipeline;
                // GPU groups retain the routing and pipeline key required for
                // submission, while material-policy validation belongs to the
                // frozen packet signatures checked above. Do not invent a
                // second group-local material-policy bit here: older sealed
                // GPU snapshots do not carry it.
                if (pipeline.materialVariant == MaterialPipelineVariant::Transparent ||
                    !IsOpaqueSurfaceEligibility(pipeline.surfacePathEligibility) ||
                    group.pipelineVariant != pipeline.materialVariant)
                {
                    return fail(OpaqueRasterPipelinePreflightCode::InvalidGPUGroup,
                                "Sealed deferred opaque GPU recording has an invalid draw group");
                }
                if (!IsSurfaceEligibilitySelectedForRoute(
                        pipeline.surfacePathEligibility, surfaceRoute))
                {
                    continue;
                }

                ++result.matchedGPUGroupCount;
                const OpaqueRasterSubmissionRoute route =
                    inputs.gpuSceneRasterEnabled
                        ? OpaqueRasterSubmissionRoute::GPUScene
                        : OpaqueRasterSubmissionRoute::GPUDriven;
                if (!preflightRequest(MakeOpaquePipelineRequest(
                                          outputFamily,
                                          surfaceRoute,
                                          route,
                                          group.pipelineVariant,
                                          colorTargetFormat,
                                          pipeline.cullMode,
                                          pipeline.reverseWinding,
                                          false,
                                          group.batchKey.usesMaterialParameterTable),
                                      requests))
                {
                    return result;
                }
            }
            if (result.matchedGPUGroupCount == 0)
            {
                return fail(OpaqueRasterPipelinePreflightCode::InvalidGPUGroup,
                            "Frozen deferred opaque GPU lane has no group for its selected route");
            }
            result.selectedGPUDrivenPacketCount = selectedGPUPlanPackets;
        }
    }

    result.code = requests.empty()
        ? OpaqueRasterPipelinePreflightCode::ReadyNoWork
        : OpaqueRasterPipelinePreflightCode::Ready;
    return result;
}

void OpaquePass::OnAdd(IRHIDevice* device)
{
    (void)device;
    RVX_CORE_DEBUG("OpaquePass added");
}

void OpaquePass::OnRemove()
{
    RVX_CORE_DEBUG("OpaquePass removed");
    m_pipelineCache = nullptr;
    m_materialSystem = nullptr;
    m_lightManager = nullptr;
    m_clusteredLighting = nullptr;
    m_renderScene = nullptr;
    m_gpuCulling = nullptr;
    m_gpuCullingRecordedState.reset();
    m_opaqueDrawItems = nullptr;
    m_maskedDrawItems = nullptr;
    m_directInstancePlan = {};
    m_directInstanceStream = {};
    m_directInstanceHandle = {};
    m_directInstanceIndexHandle = {};
    m_directMaterialParameterHandle = {};
    m_directMaterialParameterTable.Reset();
    m_directRasterMaterialSemanticKeysBySlot.clear();
    m_gpuMaterialParameterTable.Reset();
    m_gpuRasterMaterialSemanticKeysBySlot.clear();
    m_directInstancingPreflightFailed = false;
    m_deformationResources.reset();
}

void OpaquePass::SetResources(PipelineCache* pipelines,
                              MaterialSystem* materialSystem,
                              LightManager* lightManager,
                              ClusteredLighting* clusteredLighting)
{
    m_pipelineCache = pipelines;
    m_materialSystem = materialSystem;
    m_lightManager = lightManager;
    m_clusteredLighting = clusteredLighting;
}

void OpaquePass::InitializeGraphRecorder(
    const RenderScene* scene,
    const std::vector<RenderDrawItem>* opaqueDrawItems,
    const std::vector<RenderDrawItem>* maskedDrawItems,
    const GPUCulling* gpuCulling,
    const RenderPassGPUDrivenInputs& gpuInputs,
    bool gpuDrivenPlanned,
    std::shared_ptr<const LightingFramePlan> lightingPlan,
    std::shared_ptr<const RenderLightingFrameResourceSnapshot> lightingResources,
    std::shared_ptr<const RenderDeformationFrameResources> deformationResources,
    const DirectionalShadowRecordOutput& directionalShadow,
    const RayTracedShadowRecordOutput& rayTracedShadow,
    const AmbientOcclusionRecordInput& ambientOcclusion,
    std::shared_ptr<RasterInstanceStreamCache> directInstanceStreamCache,
    DirectRasterReadbackQualification* directReadbackQualification,
    const RenderPassRecordIdentity& recordIdentity,
    uint32 sourceFrameSlot,
    OpaqueRasterOutputFamily outputFamily,
    OpaqueRasterSurfaceRoute surfaceRoute,
    OpaqueDeferredGBufferTargets deferredTargets)
{
    m_renderScene = scene;
    m_opaqueDrawItems = opaqueDrawItems;
    m_maskedDrawItems = maskedDrawItems;
    m_gpuCulling = gpuCulling;
    m_gpuCullingRecordedState = gpuInputs.recordedState;
    m_gpuDrivenInstanceHandle = gpuInputs.instances;
    m_gpuSceneCandidateHandle = gpuInputs.gpuSceneCandidates;
    m_gpuScenePrimitiveHandle = gpuInputs.gpuScenePrimitives;
    m_gpuSceneTransformHandle = gpuInputs.gpuSceneTransforms;
    m_gpuDrivenInstanceIndexHandle = gpuInputs.instanceIndices;
    m_gpuDrivenIndirectHandle = gpuInputs.indirectDraws;
    m_gpuDrivenDrawCountHandle = gpuInputs.drawCount;
    m_gpuSceneRasterBinding = gpuInputs.gpuSceneRasterBinding;
    m_gpuSceneRecordingFailure = gpuInputs.gpuSceneRecordingFailure;
    m_gpuDrivenOpaqueIndirectEnabled = gpuDrivenPlanned;
    m_gpuSceneRasterEnabled = gpuInputs.gpuSceneRasterEnabled;
    m_lightingPlan = std::move(lightingPlan);
    m_lightingResources = std::move(lightingResources);
    m_deformationResources = std::move(deformationResources);
    m_lightingBindingSnapshot.reset();
    m_directionalShadowInputs = directionalShadow;
    m_rayTracedShadowInputs = rayTracedShadow;
    m_ambientOcclusionInput = ambientOcclusion;
    m_directInstanceStreamCache = std::move(directInstanceStreamCache);
    m_directReadbackQualification = directReadbackQualification;
    m_recordIdentity = recordIdentity;
    m_directReadbackSourceFrameSlot = sourceFrameSlot;
    m_outputFamily = outputFamily;
    m_surfaceRoute = surfaceRoute;
    m_deferredTargets = std::move(deferredTargets);
}

void OpaquePass::AddToGraph(
    RenderGraph& graph,
    const RenderPassRecordContext& context)
{
    AddToGraphInternal(
        graph,
        context,
        OpaqueRasterOutputFamily::ForwardSceneHDR,
        OpaqueRasterSurfaceRoute::AllOpaque,
        {});
}

void OpaquePass::AddDeferredToGraph(
    RenderGraph& graph,
    const RenderPassRecordContext& context,
    const OpaqueDeferredGBufferTargets& targets)
{
    AddToGraphInternal(
        graph,
        context,
        OpaqueRasterOutputFamily::DeferredGBuffer,
        OpaqueRasterSurfaceRoute::DeferredOpaque,
        targets);
}

void OpaquePass::AddForwardSpecialToGraph(
    RenderGraph& graph,
    const RenderPassRecordContext& context)
{
    AddToGraphInternal(
        graph,
        context,
        OpaqueRasterOutputFamily::ForwardSceneHDR,
        OpaqueRasterSurfaceRoute::ForwardSpecialOpaque,
        {});
}

void OpaquePass::AddToGraphInternal(
    RenderGraph& graph,
    const RenderPassRecordContext& context,
    OpaqueRasterOutputFamily outputFamily,
    OpaqueRasterSurfaceRoute surfaceRoute,
    const OpaqueDeferredGBufferTargets& deferredTargets)
{
    struct GraphPassData
    {
        RenderPassExecutionData execution{};
        RenderPassGPUDrivenInputs gpuInputs{};
        std::shared_ptr<RasterInstanceStreamCache> directInstanceStreamCache;
        DirectRasterReadbackQualification* directReadbackQualification = nullptr;
        uint32 directReadbackSourceFrameSlot = RVX_INVALID_INDEX;
        std::unique_ptr<OpaquePass> recorder;
        bool contextValid = false;
    };

    const auto hasCurrentGraphAttachments = [
                                                &graph,
                                                outputFamily,
                                                deferredTargets](
                                                const ViewData& view,
                                                const RenderPassRecordIdentity& identity)
    {
        if (outputFamily == OpaqueRasterOutputFamily::DeferredGBuffer)
        {
            if (!deferredTargets.IsValid() ||
                !HasCurrentGraphProvenance(deferredTargets.depth, identity) ||
                graph.GetTextureDesc(deferredTargets.depth) == nullptr)
            {
                return false;
            }
            return std::all_of(
                deferredTargets.color.begin(), deferredTargets.color.end(),
                [&graph, &identity](const RGTextureHandle& target)
                {
                    return HasCurrentGraphProvenance(target, identity) &&
                           graph.GetTextureDesc(target) != nullptr;
                });
        }

        const bool colorValid = view.colorTarget.IsValid() &&
            HasCurrentGraphProvenance(view.colorTarget, identity) &&
            graph.GetTextureDesc(view.colorTarget) != nullptr;
        const bool depthValid = !view.depthTarget.IsValid() ||
            (HasCurrentGraphProvenance(view.depthTarget, identity) &&
             graph.GetTextureDesc(view.depthTarget) != nullptr);
        return colorValid && depthValid;
    };

    // Do not call the generic execution helper until the supplied context is
    // proven to be this exact graph's fully paired recording. The helper can
    // initialize results when a snapshot is absent, which would otherwise
    // mutate a foreign or stale recording before this pass rejects it.
    const bool sourcePlanValid = context.executionPlan != nullptr &&
        context.executionPlan->frameSequence == context.identity.frameSequence &&
        context.executionPlan->viewOrdinal == context.identity.viewOrdinal;
    const bool suppliedResultsValid = context.results != nullptr &&
        context.results->identity == context.identity &&
        (context.results->executionReport.frameSequence == 0 ||
         context.results->executionReport.frameSequence ==
             context.identity.frameSequence);
    const bool suppliedSnapshotValid = suppliedResultsValid &&
        context.frameSnapshot != nullptr &&
        context.frameSnapshot->identity == context.identity &&
        context.frameSnapshot->executionPlan.frameSequence ==
            context.identity.frameSequence &&
        context.frameSnapshot->executionPlan.viewOrdinal ==
            context.identity.viewOrdinal &&
        context.frameSnapshot->view.renderFrameExecutionPlan ==
            &context.frameSnapshot->executionPlan &&
        context.frameSnapshot->view.meshPassPreparation ==
            &context.frameSnapshot->meshPassPreparation &&
        context.frameSnapshot->view.renderVisibility ==
            &context.frameSnapshot->visibility &&
        context.frameSnapshot->view.renderFrameExecutionReport ==
            &context.results->executionReport &&
        hasCurrentGraphAttachments(context.frameSnapshot->view, context.identity);
    const bool sourceContextValid = !context.legacyAdapter &&
        context.MatchesTargetGraph(graph) && context.IsFrameIdentityValid() &&
        sourcePlanValid && suppliedResultsValid && suppliedSnapshotValid &&
        hasCurrentGraphAttachments(context.view, context.identity);

    RenderPassExecutionData execution;
    if (sourceContextValid)
    {
        execution = MakeRenderPassExecutionData(context);
    }
    else
    {
        // Keep invalid source state completely out of this graph's callback.
        execution.view = context.view;
        execution.identity = context.identity;
    }
    const RenderFrameExecutionPlan* executionPlan =
        execution.GetExecutionPlan();
    const bool hasPlan = executionPlan != nullptr;
    const bool gpuPlanned = IsGPUDrivenPassPlanned(
        executionPlan, RenderPassKind::Opaque);
    // Invalid sources must remain true no-ops: do not capture their recorded
    // GPU state into this graph callback, even if it will later reject them.
    const RenderPassGPUDrivenInputs gpuInputs = sourceContextValid
        ? context.opaqueGPUDriven : RenderPassGPUDrivenInputs{};
    const GPUCullingRecordingIdentity gpuRecordingIdentity{
        execution.identity.graphIdentity,
        execution.identity.graphRecordingGeneration,
        execution.identity.frameSequence,
        execution.identity.viewOrdinal,
        execution.identity.recordEpoch};
    const bool executionAttachmentsValid =
        hasCurrentGraphAttachments(execution.view, execution.identity);
    const bool lightingInputsValid = execution.lightingPlan != nullptr &&
        execution.lightingPlan->IsValid() &&
        execution.lightingPlan->GetIdentityHash() != 0 &&
        execution.lightingResources != nullptr &&
        execution.lightingResources->IsComplete() &&
        execution.lightingResources->lightingPlanIdentity ==
            execution.lightingPlan->GetIdentityHash() &&
        execution.frameSnapshot != nullptr &&
        execution.frameSnapshot->lightingPlan == execution.lightingPlan &&
        execution.frameSnapshot->lightingResources == execution.lightingResources &&
        execution.view.textureIBLEnabled ==
            static_cast<uint32>(execution.lightingResources->textureIBLEnabled) &&
        execution.lightingPlan->GetIBLRequest().inputsReady ==
            execution.lightingResources->textureIBLEnabled &&
        execution.lightingPlan->GetCSMRequest().inputsReady ==
            execution.directionalShadow.enabled;
    // The Forward opaque route is the only legacy raster consumer of the
    // renderer-owned GTAO visibility field. Deferred GBuffer production must
    // not borrow it (the subsequent compute-lighting feature owns that edge),
    // and ForwardSpecial is deliberately composed after opaque lighting.
    const bool requiresAmbientOcclusion =
        outputFamily == OpaqueRasterOutputFamily::ForwardSceneHDR &&
        surfaceRoute == OpaqueRasterSurfaceRoute::AllOpaque;
    const bool ambientOcclusionInputsValid = !requiresAmbientOcclusion ||
        execution.ambientOcclusion.IsCompatibleWith(execution.identity);
    const bool contextValid = sourceContextValid && hasPlan &&
        execution.MatchesTargetGraph(graph) &&
        execution.IsFrameIdentityValid() &&
        execution.frameSnapshot != nullptr && execution.results != nullptr &&
        executionAttachmentsValid &&
        lightingInputsValid &&
        ambientOcclusionInputsValid &&
        execution.directionalShadow.IsCompatibleWith(
            execution.identity,
            execution.lightingPlan->GetIdentityHash()) &&
        execution.rayTracedShadow.IsCompatibleWith(execution.identity) &&
        (!gpuPlanned || (gpuInputs.IsCompatibleWith(execution.identity) &&
                         gpuInputs.recordedState->Matches(gpuRecordingIdentity)));
    if (sourceContextValid && gpuPlanned &&
        gpuInputs.gpuSceneRasterEnabled && !contextValid &&
        gpuInputs.gpuSceneRecordingFailure)
    {
        gpuInputs.gpuSceneRecordingFailure->store(true);
    }
    const bool resultOwnershipValid = sourceContextValid &&
        execution.identity.Matches(graph) && execution.results != nullptr &&
        execution.results->identity == execution.identity &&
        execution.frameSnapshot != nullptr &&
        execution.frameSnapshot->identity == execution.identity;

    const RenderResourceRegistry* const resourceRegistry = m_resourceRegistry;
    PipelineCache* const pipelineCache = m_pipelineCache;
    MaterialSystem* const materialSystem = m_materialSystem;
    const RenderScene* const renderScene = execution.frameSnapshot
        ? &execution.frameSnapshot->scene : nullptr;
    const GPUCulling* const gpuCulling = gpuInputs.recordedState != nullptr
        ? &gpuInputs.recordedState->GetCulling() : nullptr;
    const std::vector<RenderDrawItem>* const opaqueDrawItems =
        execution.frameSnapshot ? &execution.frameSnapshot->opaqueDrawItems : nullptr;
    const std::vector<RenderDrawItem>* const maskedDrawItems =
        execution.frameSnapshot ? &execution.frameSnapshot->maskedDrawItems : nullptr;
    const DirectionalShadowRecordOutput directionalShadow =
        execution.directionalShadow;
    const RayTracedShadowRecordOutput rayTracedShadow =
        execution.rayTracedShadow;
    const std::shared_ptr<const LightingFramePlan> lightingPlan =
        execution.lightingPlan;
    const std::shared_ptr<const RenderLightingFrameResourceSnapshot>
        lightingResources = execution.lightingResources;
    const std::shared_ptr<RasterInstanceStreamCache> directInstanceStreamCache =
        m_directInstanceStreamCache;
    DirectRasterReadbackQualification* const directReadbackQualification =
        sourceContextValid ? context.directOpaqueRasterReadbackQualification
                           : nullptr;
    const uint32 directReadbackSourceFrameSlot = sourceContextValid
        ? context.directOpaqueRasterReadbackSourceFrameSlot
        : RVX_INVALID_INDEX;
    const std::shared_ptr<RenderPassRecordResults> results =
        resultOwnershipValid ? execution.results : nullptr;

    graph.AddPass<GraphPassData>(
        outputFamily == OpaqueRasterOutputFamily::DeferredGBuffer
            ? "GBufferPass"
            : surfaceRoute == OpaqueRasterSurfaceRoute::ForwardSpecialOpaque
                  ? "ForwardSpecialOpaquePass"
                  : GetName(),
        GetPassType(),
        [execution,
         gpuInputs,
         contextValid,
         resourceRegistry,
         pipelineCache,
         materialSystem,
         renderScene,
         lightingPlan,
         lightingResources,
         directionalShadow,
         rayTracedShadow,
         gpuCulling,
         opaqueDrawItems,
         maskedDrawItems,
         gpuPlanned,
         directInstanceStreamCache,
         directReadbackQualification,
         directReadbackSourceFrameSlot,
         outputFamily,
         surfaceRoute,
         deferredTargets,
         results](RenderGraphBuilder& builder, GraphPassData& data)
        {
            data.execution = execution;
            data.gpuInputs = gpuInputs;
            data.directInstanceStreamCache = directInstanceStreamCache;
            data.directReadbackQualification = directReadbackQualification;
            data.directReadbackSourceFrameSlot = directReadbackSourceFrameSlot;
            data.contextValid = contextValid;
            if (!data.contextValid)
            {
                if (results != nullptr)
                {
                    OpaquePassDrawStats failedStats;
                    PublishOpaqueContextFailure(data.execution, failedStats);
                    PublishOpaqueRouteResults(
                        *results,
                        surfaceRoute,
                        failedStats,
                        {},
                        RenderExecutionStatus::Failed);
                }
                return;
            }

            data.recorder = std::make_unique<OpaquePass>();
            data.recorder->SetResources(
                pipelineCache,
                materialSystem,
                nullptr,
                nullptr);
            data.recorder->SetResourceRegistry(resourceRegistry);
            data.recorder->InitializeGraphRecorder(
                renderScene, opaqueDrawItems, maskedDrawItems, gpuCulling,
                data.gpuInputs, gpuPlanned, lightingPlan, lightingResources,
                data.execution.deformationResources,
                directionalShadow, rayTracedShadow,
                data.execution.ambientOcclusion,
                data.directInstanceStreamCache,
                data.directReadbackQualification,
                data.execution.identity,
                data.directReadbackSourceFrameSlot,
                outputFamily,
                surfaceRoute,
                deferredTargets);
            const auto declareDrawTextures = [&](
                const std::vector<RenderDrawItem>* drawItems)
            {
                if (drawItems == nullptr)
                {
                    return true;
                }
                for (const RenderDrawItem& item : *drawItems)
                {
                    if (!IsDrawItemSelectedForSurfaceRoute(
                            item, surfaceRoute))
                    {
                        continue;
                    }
                    if (!DeclareMaterialTextureGraphReads(
                            builder,
                            resourceRegistry,
                            item.material,
                            *results))
                    {
                        return false;
                    }
                }
                return true;
            };
            bool materialReadsDeclared =
                declareDrawTextures(opaqueDrawItems) &&
                declareDrawTextures(maskedDrawItems);
            if (materialReadsDeclared && gpuCulling != nullptr)
            {
                for (const GPUCullingDrawGroup& group :
                     gpuCulling->GetDrawGroups())
                {
                    if (surfaceRoute != OpaqueRasterSurfaceRoute::AllOpaque &&
                        !IsSurfaceEligibilitySelectedForRoute(
                            group.batchKey.pipeline.surfacePathEligibility,
                            surfaceRoute))
                    {
                        continue;
                    }
                    if (!DeclareMaterialTextureGraphReads(
                            builder,
                            resourceRegistry,
                            group.material,
                            *results))
                    {
                        materialReadsDeclared = false;
                        break;
                    }
                }
            }
            if (!materialReadsDeclared)
            {
                data.contextValid = false;
                OpaquePassDrawStats failedStats;
                PublishOpaqueContextFailure(data.execution, failedStats);
                PublishOpaqueRouteResults(
                    *results,
                    surfaceRoute,
                    failedStats,
                    {},
                    RenderExecutionStatus::Failed);
                return;
            }
            data.recorder->Setup(builder, data.execution.view);
            // Setup owns the graph-declaration diagnostics for this exact
            // recording. Publish them immediately so callers observing the
            // compiled graph do not have to wait for Execute to discover
            // whether shadow inputs were requested and declared.
            if (surfaceRoute == OpaqueRasterSurfaceRoute::AllOpaque)
            {
                results->opaqueShadowStats = data.recorder->GetShadowStats();
            }
        },
        [results, surfaceRoute](const GraphPassData& data,
                  RenderGraphPassContext& context)
        {
            if (!data.contextValid || !data.recorder)
            {
                if (results != nullptr)
                {
                    OpaquePassDrawStats failedStats;
                    PublishOpaqueContextFailure(data.execution, failedStats);
                    PublishOpaqueRouteResults(
                        *results,
                        surfaceRoute,
                        failedStats,
                        {},
                        RenderExecutionStatus::Failed);
                }
                context.MarkExecutionFailed(
                    "Opaque raster graph inputs failed validation");
                return;
            }
            if (results == nullptr)
            {
                context.MarkExecutionFailed(
                    "Opaque raster execution has no recording results owner");
                return;
            }
            data.recorder->Execute(
                context, data.execution.view, results.get());
            const OpaquePassDrawStats& drawStats = data.recorder->GetDrawStats();
            PublishOpaqueRouteResults(
                *results,
                surfaceRoute,
                drawStats,
                data.recorder->GetShadowStats(),
                drawStats.failureReason == RenderPolicyReason::None
                    ? RenderExecutionStatus::Completed
                    : RenderExecutionStatus::Failed);
            if (surfaceRoute ==
                OpaqueRasterSurfaceRoute::ForwardSpecialOpaque)
            {
                PublishDeferredOpaqueAggregateExecutionReport(
                    data.execution.view, *results);
            }
        });
}

void OpaquePass::Setup(RenderGraphBuilder& builder, const ViewData& view)
{
    m_directionalShadowReadHandle = {};
    m_rayTracedShadowMaskReadHandle = {};
    m_ambientOcclusionReadHandle = {};
    m_lightConstantsReadHandle = {};
    m_pointLightsReadHandle = {};
    m_spotLightsReadHandle = {};
    m_clusterConstantsReadHandle = {};
    m_clusterDataReadHandle = {};
    m_clusterLightIndicesReadHandle = {};
    m_exposureStateReadHandle = {};
    m_currentPaletteReadHandle = {};
    m_previousPaletteReadHandle = {};
    m_colorTargetViewHandle = {};
    m_depthTargetViewHandle = {};
    m_directionalShadowViewHandle = {};
    m_rayTracedShadowMaskViewHandle = {};
    m_ambientOcclusionViewHandle = {};
    m_environmentIBLIrradianceViewHandle = {};
    m_environmentIBLPrefilteredViewHandle = {};
    m_environmentIBLBRDFLUTViewHandle = {};
    m_gBufferTargetHandles = {};
    m_gBufferTargetViewHandles = {};
    m_colorTargetFormat = RHIFormat::Unknown;
    m_shadowStats = {};
    m_lightingBindingSnapshot.reset();
    m_directInstancePlan = {};
    m_directInstanceStream.instanceUploadBytes = 0;
    m_directInstanceStream.indexUploadBytes = 0;
    m_directInstanceStream.instancePatchedRowCount = 0;
    m_directInstanceStream.indexPatchedRowCount = 0;
    m_directInstanceStream.activeInstanceCount = 0;
    m_directInstanceStream.activeInstanceCapacity = 0;
    m_directInstanceStream.instanceFullMaterialization = false;
    m_directInstanceStream.indexFullMaterialization = false;
    m_directInstanceHandle = {};
    m_directInstanceIndexHandle = {};
    m_directMaterialParameterHandle = {};
    m_directMaterialParameterTable.Reset();
    m_directRasterMaterialSemanticKeysBySlot.clear();
    m_gpuMaterialParameterHandle = {};
    m_gpuMaterialParameterTable.Reset();
    m_gpuRasterMaterialSemanticKeysBySlot.clear();
    m_directInstancingPreflightFailed = false;

    // Every opaque route is bound to the same immutable lighting selection.
    // Deferred GBuffer does not consume the descriptor, but it still rejects a
    // stale or unpaired plan before declaring any graph-visible writes.
    if (m_lightingPlan == nullptr || !m_lightingPlan->IsValid() ||
        m_lightingPlan->GetIdentityHash() == 0 ||
        m_lightingResources == nullptr || !m_lightingResources->IsComplete() ||
        !m_lightingResources->HasCurrentGraphResources(m_recordIdentity) ||
        m_lightingResources->lightingPlanIdentity !=
            m_lightingPlan->GetIdentityHash() ||
        m_lightingPlan->GetCSMRequest().inputsReady !=
            m_directionalShadowInputs.enabled ||
        m_lightingPlan->GetIBLRequest().inputsReady !=
            m_lightingResources->textureIBLEnabled ||
        view.textureIBLEnabled !=
            static_cast<uint32>(m_lightingResources->textureIBLEnabled) ||
        !m_directionalShadowInputs.IsCompatibleWith(
            m_recordIdentity,
            m_lightingPlan->GetIdentityHash()) ||
        !m_rayTracedShadowInputs.IsCompatibleWith(m_recordIdentity))
    {
        return;
    }

    const bool sceneHasSkinning = m_renderScene != nullptr &&
        std::any_of(m_renderScene->GetObjects().begin(),
                    m_renderScene->GetObjects().end(),
                    [](const RenderObject& object)
                    {
                        return object.HasSkinningData();
                    });
    if (sceneHasSkinning)
    {
        const RenderDeformationFrameIdentity deformationIdentity =
            MakeRenderDeformationFrameIdentity(m_recordIdentity);
        if (m_deformationResources == nullptr ||
            !m_deformationResources->MatchesIdentity(deformationIdentity) ||
            !m_deformationResources->MatchesGraphProvenance(*m_recordIdentity.graph) ||
            m_deformationResources->GetMatrixCount() == 0 ||
            builder.GetBufferDesc(
                m_deformationResources->GetCurrentPaletteHandle()) == nullptr ||
            builder.GetBufferDesc(
                m_deformationResources->GetPreviousPaletteHandle()) == nullptr)
        {
            return;
        }
        m_currentPaletteReadHandle = builder.Read(
            m_deformationResources->GetCurrentPaletteHandle(),
            RHIResourceState::ShaderResource,
            RHIShaderStage::Vertex);
        m_previousPaletteReadHandle = builder.Read(
            m_deformationResources->GetPreviousPaletteHandle(),
            RHIResourceState::ShaderResource,
            RHIShaderStage::Vertex);
        if (!m_currentPaletteReadHandle.IsValid() ||
            !m_previousPaletteReadHandle.IsValid())
        {
            return;
        }
    }

    // Forward surface paths bind these six exact frozen allocations through
    // their descriptor snapshot. Declare the same allocations to RenderGraph
    // so state/lifetime validation cannot be bypassed by raw descriptor use.
    // Deferred GBuffer writes no lighting and leaves these reads to the
    // DeferredLighting feature.
    if (m_outputFamily != OpaqueRasterOutputFamily::DeferredGBuffer)
    {
        const bool descriptionsReady =
            builder.GetBufferDesc(m_lightingResources->lightConstants) != nullptr &&
            builder.GetBufferDesc(m_lightingResources->pointLights) != nullptr &&
            builder.GetBufferDesc(m_lightingResources->spotLights) != nullptr &&
            builder.GetBufferDesc(m_lightingResources->clusterConstants) != nullptr &&
            builder.GetBufferDesc(m_lightingResources->clusterData) != nullptr &&
            builder.GetBufferDesc(
                m_lightingResources->clusterLightIndices) != nullptr &&
            view.exposureState.IsValid() &&
            builder.GetBufferDesc(view.exposureState) != nullptr;
        if (!descriptionsReady)
        {
            return;
        }

        m_lightConstantsReadHandle = builder.Read(
            m_lightingResources->lightConstants,
            RHIResourceState::ConstantBuffer,
            RHIShaderStage::Pixel);
        m_pointLightsReadHandle = builder.Read(
            m_lightingResources->pointLights,
            RHIResourceState::ShaderResource,
            RHIShaderStage::Pixel);
        m_spotLightsReadHandle = builder.Read(
            m_lightingResources->spotLights,
            RHIResourceState::ShaderResource,
            RHIShaderStage::Pixel);
        m_clusterConstantsReadHandle = builder.Read(
            m_lightingResources->clusterConstants,
            RHIResourceState::ConstantBuffer,
            RHIShaderStage::Pixel);
        m_clusterDataReadHandle = builder.Read(
            m_lightingResources->clusterData,
            RHIResourceState::ShaderResource,
            RHIShaderStage::Pixel);
        m_clusterLightIndicesReadHandle = builder.Read(
            m_lightingResources->clusterLightIndices,
            RHIResourceState::ShaderResource,
            RHIShaderStage::Pixel);
        m_exposureStateReadHandle = builder.Read(
            view.exposureState,
            RHIResourceState::ShaderResource,
            RHIShaderStage::Pixel);
        if (!m_lightConstantsReadHandle.IsValid() ||
            !m_pointLightsReadHandle.IsValid() ||
            !m_spotLightsReadHandle.IsValid() ||
            !m_clusterConstantsReadHandle.IsValid() ||
            !m_clusterDataReadHandle.IsValid() ||
            !m_clusterLightIndicesReadHandle.IsValid() ||
            !m_exposureStateReadHandle.IsValid())
        {
            return;
        }

        if (m_surfaceRoute == OpaqueRasterSurfaceRoute::AllOpaque)
        {
            const RHITextureDesc* const colorDesc =
                builder.GetTextureDesc(view.colorTarget);
            const RHITextureDesc* const ambientOcclusionDesc =
                builder.GetTextureDesc(m_ambientOcclusionInput.filteredOcclusion);
            const RenderOutputExtent expectedExtent{
                colorDesc != nullptr ? colorDesc->width : 0,
                colorDesc != nullptr ? colorDesc->height : 0};
            if (!m_ambientOcclusionInput.IsCompatibleWith(m_recordIdentity) ||
                colorDesc == nullptr || ambientOcclusionDesc == nullptr ||
                !expectedExtent.IsValid() ||
                m_ambientOcclusionInput.extent != expectedExtent ||
                ambientOcclusionDesc->format != RHIFormat::R8_UNORM ||
                ambientOcclusionDesc->sampleCount != RHISampleCount::Count1 ||
                ambientOcclusionDesc->dimension != RHITextureDimension::Texture2D ||
                ambientOcclusionDesc->depth != 1 ||
                ambientOcclusionDesc->mipLevels != 1 ||
                ambientOcclusionDesc->arraySize != 1 ||
                ambientOcclusionDesc->width != expectedExtent.width ||
                ambientOcclusionDesc->height != expectedExtent.height ||
                !HasFlag(ambientOcclusionDesc->usage,
                         RHITextureUsage::ShaderResource))
            {
                return;
            }

            m_ambientOcclusionReadHandle = builder.Read(
                m_ambientOcclusionInput.filteredOcclusion,
                MakeRGAccessDesc(
                    RHIResourceState::ShaderResource,
                    RHIShaderStage::Pixel));
            RHITextureViewDesc ambientOcclusionViewDesc;
            ambientOcclusionViewDesc.format = RHIFormat::R8_UNORM;
            ambientOcclusionViewDesc.dimension =
                RHITextureDimension::Texture2D;
            ambientOcclusionViewDesc.subresourceRange =
                RHISubresourceRange::All();
            ambientOcclusionViewDesc.type = RHITextureViewType::ShaderResource;
            ambientOcclusionViewDesc.debugName = "ForwardOpaqueAmbientOcclusionSRV";
            m_ambientOcclusionViewHandle = builder.CreateTextureView(
                m_ambientOcclusionReadHandle, ambientOcclusionViewDesc);
            m_ambientOcclusionViewHandle = builder.Read(
                m_ambientOcclusionViewHandle,
                MakeRGAccessDesc(
                    RHIResourceState::ShaderResource,
                    RHIShaderStage::Pixel));
            if (!m_ambientOcclusionReadHandle.IsValid() ||
                !m_ambientOcclusionViewHandle.IsValid())
            {
                return;
            }
        }
    }

    m_gpuMaterialTablePreflightFailed = false;

    const auto accumulateShadowReceivers = [this](const std::vector<RenderDrawItem>* drawItems)
    {
        if (!drawItems || !m_renderScene)
        {
            return;
        }

        for (const RenderDrawItem& item : *drawItems)
        {
            if (!IsDrawItemSelectedForSurfaceRoute(item, m_surfaceRoute) ||
                item.objectIndex >= m_renderScene->GetObjectCount())
            {
                continue;
            }

            m_shadowStats.receiverCandidateDrawItemCount++;
            if (HasDrawFlag(
                    item.packet.flags, RenderDrawFlags::ReceivesShadow))
            {
                m_shadowStats.shadowReceivingDrawItemCount++;
            }
            else
            {
                m_shadowStats.shadowReceiverOptOutDrawItemCount++;
            }
        }
    };
    accumulateShadowReceivers(m_opaqueDrawItems);
    accumulateShadowReceivers(m_maskedDrawItems);

    const RGTextureHandle selectedDepth =
        m_outputFamily == OpaqueRasterOutputFamily::DeferredGBuffer
            ? m_deferredTargets.depth
            : view.depthTarget;

    // Deferred validation is deliberately two-phase. No graph write is
    // declared until all four colors and the depth target satisfy the entire
    // canonical contract; a later invalid attachment must not leave a partial
    // producer edge behind for lighting to consume.
    if (m_outputFamily == OpaqueRasterOutputFamily::DeferredGBuffer)
    {
        constexpr std::array<RHIFormat, 4> expectedFormats = {
            RHIFormat::RGBA8_UNORM_SRGB,
            RHIFormat::RGBA8_UNORM,
            RHIFormat::RGBA16_FLOAT,
            RHIFormat::R32_UINT};
        constexpr std::array<const char*, 4> viewNames = {
            "GBuffer0RTV", "GBuffer1RTV", "GBuffer2RTV", "GBuffer3RTV"};
        std::array<const RHITextureDesc*, 4> colorDescs{};
        bool validTargets = m_deferredTargets.IsValid();
        uint32 deferredWidth = 0;
        uint32 deferredHeight = 0;
        for (uint32 index = 0; index < expectedFormats.size() && validTargets;
             ++index)
        {
            const RGTextureHandle target = m_deferredTargets.color[index];
            const RHITextureDesc* desc = builder.GetTextureDesc(target);
            if (desc == nullptr || desc->format != expectedFormats[index] ||
                desc->sampleCount != RHISampleCount::Count1 ||
                desc->dimension != RHITextureDimension::Texture2D ||
                desc->depth != 1 || desc->mipLevels != 1 ||
                desc->arraySize != 1 ||
                !HasFlag(desc->usage, RHITextureUsage::RenderTarget) ||
                desc->width == 0 || desc->height == 0 ||
                (index != 0 &&
                 (desc->width != deferredWidth ||
                  desc->height != deferredHeight)))
            {
                validTargets = false;
                break;
            }
            if (index == 0)
            {
                deferredWidth = desc->width;
                deferredHeight = desc->height;
            }
            colorDescs[index] = desc;
        }
        const RHITextureDesc* depthDesc = validTargets
            ? builder.GetTextureDesc(selectedDepth)
            : nullptr;
        validTargets = validTargets && depthDesc != nullptr &&
            depthDesc->format == RHIFormat::D32_FLOAT &&
            depthDesc->sampleCount == RHISampleCount::Count1 &&
            depthDesc->dimension == RHITextureDimension::Texture2D &&
            depthDesc->depth == 1 && depthDesc->mipLevels == 1 &&
            depthDesc->arraySize == 1 &&
            HasFlag(depthDesc->usage, RHITextureUsage::DepthStencil) &&
            depthDesc->width == deferredWidth &&
            depthDesc->height == deferredHeight;
        if (!validTargets)
        {
            return;
        }

        for (uint32 index = 0; index < expectedFormats.size(); ++index)
        {
            const RGTextureHandle target = m_deferredTargets.color[index];
            const RHITextureDesc& desc = *colorDescs[index];
            m_gBufferTargetHandles[index] = target;
            RHITextureViewDesc viewDesc;
            viewDesc.format = desc.format;
            viewDesc.dimension = desc.dimension;
            viewDesc.subresourceRange = RHISubresourceRange::All();
            viewDesc.type = RHITextureViewType::RenderTarget;
            viewDesc.debugName = viewNames[index];
            m_gBufferTargetViewHandles[index] = builder.CreateTextureView(
                target, viewDesc);
        }

        m_depthTargetHandle = selectedDepth;
        RHITextureViewDesc depthViewDesc;
        depthViewDesc.format = depthDesc->format;
        depthViewDesc.dimension = depthDesc->dimension;
        depthViewDesc.subresourceRange = RHISubresourceRange::All();
        depthViewDesc.subresourceRange.aspect = RHITextureAspect::Depth;
        depthViewDesc.type = RHITextureViewType::DepthStencil;
        depthViewDesc.debugName = "OpaqueDepthDSV";
        m_depthTargetViewHandle = builder.CreateTextureView(
            m_depthTargetHandle, depthViewDesc);

        if (!m_depthTargetViewHandle.IsValid() ||
            !std::all_of(
                m_gBufferTargetViewHandles.begin(),
                m_gBufferTargetViewHandles.end(),
                [](const RGTextureViewHandle& handle)
                {
                    return handle.IsValid();
                }))
        {
            m_gBufferTargetHandles = {};
            m_gBufferTargetViewHandles = {};
            m_depthTargetHandle = {};
            m_depthTargetViewHandle = {};
            return;
        }

        for (uint32 index = 0; index < expectedFormats.size(); ++index)
        {
            m_gBufferTargetViewHandles[index] = builder.Write(
                m_gBufferTargetViewHandles[index],
                MakeRGAccessDesc(
                    RHIResourceState::RenderTarget,
                    RHIShaderStage::None));
        }
        m_depthTargetViewHandle = builder.Write(
            m_depthTargetViewHandle,
            MakeRGAccessDesc(
                RHIResourceState::DepthWrite,
                RHIShaderStage::None));
        m_colorTargetHandle = m_gBufferTargetHandles[0];
        m_colorTargetViewHandle = m_gBufferTargetViewHandles[0];
        m_colorTargetFormat = expectedFormats[0];
    }
    else if (view.colorTarget.IsValid())
    {
        m_colorTargetHandle = view.colorTarget;
        if (const RHITextureDesc* desc =
                builder.GetTextureDesc(m_colorTargetHandle))
        {
            m_colorTargetFormat = desc->format;
            RHITextureViewDesc viewDesc;
            viewDesc.format = desc->format;
            viewDesc.dimension = desc->dimension;
            viewDesc.subresourceRange = RHISubresourceRange::All();
            viewDesc.type = RHITextureViewType::RenderTarget;
            viewDesc.debugName = "OpaqueColorRTV";
            m_colorTargetViewHandle = builder.CreateTextureView(
                m_colorTargetHandle, viewDesc);
            m_colorTargetViewHandle = builder.Write(
                m_colorTargetViewHandle,
                MakeRGAccessDesc(
                    RHIResourceState::RenderTarget,
                    RHIShaderStage::None));
        }
    }

    // Forward retains its optional depth declaration. Deferred already
    // committed depth atomically with the four GBuffer attachments above.
    if (m_outputFamily != OpaqueRasterOutputFamily::DeferredGBuffer &&
        selectedDepth.IsValid())
    {
        m_depthTargetHandle = selectedDepth;
        const RHITextureDesc* desc =
            builder.GetTextureDesc(m_depthTargetHandle);
        if (desc != nullptr)
        {
            RHITextureViewDesc viewDesc;
            viewDesc.format = desc->format;
            viewDesc.dimension = desc->dimension;
            viewDesc.subresourceRange = RHISubresourceRange::All();
            viewDesc.subresourceRange.aspect = RHITextureAspect::Depth;
            viewDesc.type = RHITextureViewType::DepthStencil;
            viewDesc.debugName = "OpaqueDepthDSV";
            m_depthTargetViewHandle = builder.CreateTextureView(
                m_depthTargetHandle, viewDesc);
            m_depthTargetViewHandle = builder.Write(
                m_depthTargetViewHandle,
                MakeRGAccessDesc(
                    RHIResourceState::DepthWrite,
                    RHIShaderStage::None));
        }
    }

    if (m_outputFamily != OpaqueRasterOutputFamily::DeferredGBuffer &&
        m_directionalShadowInputs.enabled)
    {
        RGTextureHandle shadowMap = m_directionalShadowInputs.shadowMap;
        if (shadowMap.IsValid())
        {
            shadowMap.hasSubresourceRange = true;
            shadowMap.subresourceRange = RHISubresourceRange{0, RVX_ALL_MIPS, 0, RVX_ALL_LAYERS, RHITextureAspect::Depth};
            m_directionalShadowReadHandle = shadowMap;
            if (const RHITextureDesc* desc =
                    builder.GetTextureDesc(shadowMap))
            {
                RHITextureViewDesc viewDesc;
                viewDesc.format = desc->format;
                viewDesc.dimension = desc->dimension;
                viewDesc.subresourceRange = shadowMap.subresourceRange;
                viewDesc.type = RHITextureViewType::ShaderResource;
                viewDesc.debugName = "DirectionalShadowSRV";
                m_directionalShadowViewHandle =
                    builder.CreateTextureView(shadowMap, viewDesc);
                m_directionalShadowViewHandle = builder.Read(
                    m_directionalShadowViewHandle,
                    MakeRGAccessDesc(
                        RHIResourceState::ShaderResource,
                        RHIShaderStage::Pixel));
            }
            m_shadowStats.requested = true;
            m_shadowStats.renderGraphReadDeclared =
                m_directionalShadowViewHandle.IsValid();
        }
    }

    if (m_outputFamily != OpaqueRasterOutputFamily::DeferredGBuffer &&
        m_rayTracedShadowInputs.enabled)
    {
        RGTextureHandle shadowMask = m_rayTracedShadowInputs.shadowMask;
        if (shadowMask.IsValid())
        {
            m_rayTracedShadowMaskReadHandle = shadowMask;
            if (const RHITextureDesc* desc =
                    builder.GetTextureDesc(shadowMask))
            {
                RHITextureViewDesc viewDesc;
                viewDesc.format = desc->format;
                viewDesc.dimension = desc->dimension;
                viewDesc.subresourceRange = RHISubresourceRange::All();
                viewDesc.type = RHITextureViewType::ShaderResource;
                viewDesc.debugName = "RayTracedShadowMaskSRV";
                m_rayTracedShadowMaskViewHandle =
                    builder.CreateTextureView(shadowMask, viewDesc);
                m_rayTracedShadowMaskViewHandle = builder.Read(
                    m_rayTracedShadowMaskViewHandle,
                    MakeRGAccessDesc(
                        RHIResourceState::ShaderResource,
                        RHIShaderStage::Pixel));
            }
            m_shadowStats.rayTracedRequested = true;
            m_shadowStats.rayTracedRenderGraphReadDeclared =
                m_rayTracedShadowMaskViewHandle.IsValid();
        }
    }

    if (m_outputFamily != OpaqueRasterOutputFamily::DeferredGBuffer &&
        view.textureIBLEnabled != 0)
    {
        if (!m_lightingResources ||
            !m_lightingResources->textureIBLEnabled)
        {
            return;
        }
        m_environmentIBLIrradianceViewHandle = DeclareExactIBLRead(
            builder,
            m_lightingResources->environmentIBLIrradiance,
            m_lightingResources->environmentIBLIrradianceView,
            "OpaqueEnvironmentIrradianceSRV");
        m_environmentIBLPrefilteredViewHandle = DeclareExactIBLRead(
            builder,
            m_lightingResources->environmentIBLPrefiltered,
            m_lightingResources->environmentIBLPrefilteredView,
            "OpaqueEnvironmentPrefilteredSRV");
        m_environmentIBLBRDFLUTViewHandle = DeclareExactIBLRead(
            builder,
            m_lightingResources->environmentIBLBRDFLUT,
            m_lightingResources->environmentIBLBRDFLUTView,
            "OpaqueEnvironmentBRDFLUTSRV");
        if (!m_environmentIBLIrradianceViewHandle.IsValid() ||
            !m_environmentIBLPrefilteredViewHandle.IsValid() ||
            !m_environmentIBLBRDFLUTViewHandle.IsValid())
        {
            return;
        }
    }

    if (m_gpuDrivenOpaqueIndirectEnabled && m_gpuCulling)
    {
        if (m_gpuSceneRasterEnabled)
        {
            if (m_gpuSceneCandidateHandle.IsValid())
            {
                builder.Read(m_gpuSceneCandidateHandle, RHIShaderStage::Vertex);
            }
            if (m_gpuScenePrimitiveHandle.IsValid())
            {
                builder.Read(m_gpuScenePrimitiveHandle, RHIShaderStage::Vertex);
            }
            if (m_gpuSceneTransformHandle.IsValid())
            {
                builder.Read(m_gpuSceneTransformHandle, RHIShaderStage::Vertex);
            }
        }
        else if (m_gpuDrivenInstanceHandle.IsValid())
        {
            builder.Read(m_gpuDrivenInstanceHandle, RHIShaderStage::Vertex);
        }
        if (m_gpuDrivenInstanceIndexHandle.IsValid())
        {
            builder.Read(m_gpuDrivenInstanceIndexHandle,
                         RHIResourceState::VertexBuffer,
                         RHIShaderStage::Vertex);
        }
        if (m_gpuDrivenIndirectHandle.IsValid())
        {
            builder.Read(m_gpuDrivenIndirectHandle, RHIResourceState::IndirectArgument);
        }
        if (m_gpuDrivenDrawCountHandle.IsValid())
        {
            builder.Read(m_gpuDrivenDrawCountHandle, RHIResourceState::IndirectArgument);
        }

        const bool needsMaterialParameterTable = std::any_of(
            m_gpuCulling->GetDrawGroups().begin(),
            m_gpuCulling->GetDrawGroups().end(),
            [](const GPUCullingDrawGroup& group)
            {
                return group.batchKey.usesMaterialParameterTable;
            });
        if (needsMaterialParameterTable)
        {
            std::vector<MaterialParameterTableEntryRequest> requests;
            if (view.meshPassPreparation != nullptr)
            {
                for (const MeshPassProcessorResult& result :
                     view.meshPassPreparation->opaque.sortedGPUCandidates)
                {
                    if (!result.groupKey.usesMaterialParameterTable)
                    {
                        continue;
                    }
                    const MeshGPUBuffers buffers = ResolveRenderMeshBuffers(
                        m_resourceRegistry, result.packet.geometryKey.mesh);
                    if (!buffers.IsValid())
                    {
                        requests.clear();
                        break;
                    }
                    requests.push_back({
                        result.packet.materialKey.material,
                        buffers.HasNormalMapTangentBasis()});
                }
            }

            MaterialParameterTableSnapshot table;
            if (requests.empty() || m_materialSystem == nullptr ||
                !m_materialSystem->CreateMaterialParameterTableSnapshot(
                    requests, nullptr, table) ||
                !builder.RetainSubmissionResource(
                    Ref<RefCounted>(table.buffer)))
            {
                m_gpuMaterialTablePreflightFailed = true;
            }
            else
            {
                m_gpuRasterMaterialSemanticKeysBySlot =
                    std::move(table.rasterMaterialSemanticKeysBySlot);
                m_gpuMaterialParameterTable = std::move(table.buffer);
                m_gpuMaterialParameterHandle = builder.ImportBuffer(
                    m_gpuMaterialParameterTable,
                    RHIResourceState::ShaderResource);
                if (!m_gpuMaterialParameterHandle.IsValid())
                {
                    m_gpuMaterialTablePreflightFailed = true;
                    m_gpuMaterialParameterTable.Reset();
                    m_gpuRasterMaterialSemanticKeysBySlot.clear();
                }
                else
                {
                    builder.Read(m_gpuMaterialParameterHandle,
                                 RHIShaderStage::Pixel);
                }
            }
        }
    }

    if (!PrepareDirectInstanceStream(builder, view))
    {
        m_directInstancingPreflightFailed = true;
    }
}

bool OpaquePass::PrepareDirectInstanceStream(RenderGraphBuilder& builder,
                                             const ViewData& view)
{
    if (m_renderScene == nullptr ||
        view.instancingMode == RenderInstancingMode::Disabled ||
        // The existing instance stream is keyed to the complete Opaque draw
        // plan.  A deferred frame records two strict subsets of that plan, so
        // reusing its slot map here would make either route claim another
        // route's rows.  Submit the split routes Direct until a route-aware
        // instance-plan artifact exists.
        m_surfaceRoute != OpaqueRasterSurfaceRoute::AllOpaque)
    {
        return true;
    }
    if (view.instanceBatchPlans == nullptr ||
        !view.instanceBatchPlans->opaqueValid ||
        view.renderFrameExecutionPlan == nullptr ||
        view.meshPassPreparation == nullptr ||
        view.renderVisibility == nullptr || m_pipelineCache == nullptr)
    {
        return false;
    }

    m_directInstancePlan = view.instanceBatchPlans->opaque;
    if (m_directInstancePlan.instancedBatchCount == 0)
    {
        return true;
    }
    const DirectDrawPacketBatchBuildResult direct = BuildDirectDrawPacketBatch(
        *view.renderFrameExecutionPlan,
        RenderPassKind::Opaque,
        view.meshPassPreparation->opaque,
        view.renderVisibility);
    if (!direct.succeeded ||
        direct.batch.packets.size() !=
            m_directInstancePlan.executedPacketCount)
    {
        return false;
    }

    std::vector<MaterialParameterTableEntryRequest> materialRequests;
    for (const RenderInstanceBatch& batch : m_directInstancePlan.batches)
    {
        if (!batch.instanced || !batch.key.usesMaterialParameterTable)
        {
            continue;
        }
        for (const RenderInstanceBatchMember& member : batch.members)
        {
            if (member.directPacketIndex >= direct.batch.packets.size())
            {
                return false;
            }
            const RenderDrawPacket& packet =
                direct.batch.packets[member.directPacketIndex].packet;
            const RenderMeshResourceData* const mesh =
                m_resourceRegistry != nullptr
                    ? m_resourceRegistry->ResolveMesh(packet.geometryKey.mesh)
                    : nullptr;
            if (mesh == nullptr ||
                !HasMeshBufferSemantic(*mesh,
                                       RenderMeshBufferSemantic::Position) ||
                !HasMeshBufferSemantic(*mesh,
                                       RenderMeshBufferSemantic::Index))
            {
                return false;
            }
            materialRequests.push_back({
                packet.materialKey.material,
                HasMeshBufferSemantic(*mesh, RenderMeshBufferSemantic::Normal) &&
                    HasMeshBufferSemantic(*mesh, RenderMeshBufferSemantic::UV) &&
                    HasMeshBufferSemantic(*mesh, RenderMeshBufferSemantic::Tangent) &&
                    mesh->createInfo.hasTangentBasis});
        }
    }
    if (!materialRequests.empty())
    {
        MaterialParameterTableSnapshot table;
        if (m_materialSystem == nullptr ||
            !m_materialSystem->CreateMaterialParameterTableSnapshot(
                materialRequests, nullptr, table) ||
            !builder.RetainSubmissionResource(
                Ref<RefCounted>(table.buffer)))
        {
            return false;
        }
        m_directMaterialParameterTable = std::move(table.buffer);
        m_directRasterMaterialSemanticKeysBySlot =
            std::move(table.rasterMaterialSemanticKeysBySlot);
    }

    std::vector<GPUInstanceData> instances;
    std::vector<uint64> semanticIdentities;
    std::vector<RasterInstanceStreamKey> instanceKeys;
    std::vector<RasterInstanceStreamBatch> instanceBatches;
    if (!BuildRasterInstanceData(m_directInstancePlan,
                                 direct.batch,
                                 *m_renderScene,
                                 instances,
                                 semanticIdentities,
                                 instanceKeys,
                                 instanceBatches,
                                 nullptr))
    {
        return false;
    }

    IRHIDevice* device = m_pipelineCache->GetDevice();
    if (device == nullptr ||
        m_directInstanceStreamCache == nullptr ||
        !CreateRasterInstanceStream(*device,
                                    instances,
                                    instanceKeys,
                                    instanceBatches,
                                    "OpaqueDirectInstancing",
                                    *m_directInstanceStreamCache,
                                    m_directInstanceStream,
                                    semanticIdentities) ||
        !builder.RetainSubmissionResource(
            Ref<RefCounted>(m_directInstanceStream.instances)) ||
        !builder.RetainSubmissionResource(
            Ref<RefCounted>(m_directInstanceStream.instanceIndices)))
    {
        return false;
    }

    m_directInstanceHandle = builder.ImportBuffer(
        m_directInstanceStream.instances,
        RHIResourceState::ShaderResource);
    m_directInstanceIndexHandle = builder.ImportBuffer(
        m_directInstanceStream.instanceIndices,
        RHIResourceState::VertexBuffer);
    if (!m_directInstanceHandle.IsValid() ||
        !m_directInstanceIndexHandle.IsValid())
    {
        return false;
    }
    builder.Read(m_directInstanceHandle, RHIShaderStage::Vertex);
    builder.Read(m_directInstanceIndexHandle,
                 RHIResourceState::VertexBuffer,
                 RHIShaderStage::Vertex);
    if (m_directMaterialParameterTable)
    {
        m_directMaterialParameterHandle = builder.ImportBuffer(
            m_directMaterialParameterTable,
            RHIResourceState::ShaderResource);
        if (!m_directMaterialParameterHandle.IsValid())
        {
            m_directMaterialParameterTable.Reset();
            return false;
        }
        builder.Read(m_directMaterialParameterHandle,
                     RHIShaderStage::Pixel);
    }
    return true;
}

bool OpaquePass::FinalizeDirectRasterSemanticEvidence(
    std::span<const PlannedOpaqueDraw> plannedDraws)
{
    const bool hasInstancedDraw = std::any_of(
        plannedDraws.begin(), plannedDraws.end(),
        [](const PlannedOpaqueDraw& planned) { return planned.instanced; });
    if (!hasInstancedDraw)
    {
        return true;
    }
    if (m_resourceRegistry == nullptr || m_directInstanceStreamCache == nullptr ||
        !m_directInstanceStream.IsValid() ||
        m_directInstanceStream.activeFrameSlot >= RVX_MAX_FRAME_COUNT)
    {
        return false;
    }

    const RasterInstanceStreamFrameSlot& resident =
        m_directInstanceStreamCache->frameSlots[
            m_directInstanceStream.activeFrameSlot];
    if (!resident.IsValid() ||
        resident.instances.Get() != m_directInstanceStream.instances.Get() ||
        resident.instanceIndices.Get() !=
            m_directInstanceStream.instanceIndices.Get())
    {
        return false;
    }

    std::vector<uint64> semanticIdentities(resident.instanceCapacity, 0);
    std::vector<uint8> assigned(resident.instanceCapacity, 0);
    uint32 assignedCount = 0;
    for (const PlannedOpaqueDraw& planned : plannedDraws)
    {
        if (!planned.instanced)
        {
            continue;
        }
        const RenderDrawArguments& arguments = planned.packet.packet.arguments;
        if (arguments.firstInstance > resident.residentDrawOrder.size() ||
            arguments.instanceCount >
                resident.residentDrawOrder.size() - arguments.firstInstance)
        {
            return false;
        }
        const uint64 fixedRasterMaterialKey =
            planned.materialBinding.descriptorContentKey;
        for (uint32 instanceOffset = 0;
             instanceOffset < arguments.instanceCount;
             ++instanceOffset)
        {
            const uint32 drawOrderIndex = arguments.firstInstance + instanceOffset;
            const uint32 residentRow = resident.residentDrawOrder[drawOrderIndex];
            if (residentRow >= resident.residentInstances.size() ||
                residentRow >= semanticIdentities.size() ||
                assigned[residentRow] != 0)
            {
                return false;
            }
            const GPUInstanceData& instance = resident.residentInstances[residentRow];
            uint64 rasterMaterialKey = fixedRasterMaterialKey;
            if (planned.usesMaterialParameterTable)
            {
                if (instance.materialId >=
                    m_directRasterMaterialSemanticKeysBySlot.size())
                {
                    return false;
                }
                rasterMaterialKey =
                    m_directRasterMaterialSemanticKeysBySlot[instance.materialId];
            }
            const std::optional<uint64> semanticIdentity =
                m_resourceRegistry->CombineRasterMeshAndMaterialSemanticIdentity(
                    planned.packet.packet.geometryKey.mesh, rasterMaterialKey);
            if (!semanticIdentity)
            {
                return false;
            }
            semanticIdentities[residentRow] = *semanticIdentity;
            assigned[residentRow] = 1;
            ++assignedCount;
        }
    }
    if (assignedCount != resident.instanceCount)
    {
        return false;
    }
    return FinalizeRasterInstanceStreamSemanticSidecar(
        m_directInstanceStream,
        *m_directInstanceStreamCache,
        std::move(semanticIdentities));
}

bool OpaquePass::BuildCompleteDirectRasterTranscript(
    std::span<const PlannedOpaqueDraw> plannedDraws,
    RasterTranscriptDigest& outDigest) const
{
    outDigest = {};
    if (m_resourceRegistry == nullptr || m_renderScene == nullptr)
    {
        return false;
    }
    try
    {
        std::vector<DirectRasterTranscriptPlannedDraw> evidence;
        evidence.reserve(plannedDraws.size());
        for (const PlannedOpaqueDraw& planned : plannedDraws)
        {
            evidence.push_back({planned.packet,
                                planned.materialBinding.descriptorContentKey,
                                planned.usesMaterialParameterTable,
                                planned.representedPacketCount,
                                planned.instanced});
        }
        return RVX::BuildCompleteDirectRasterTranscript(
            evidence,
            *m_renderScene,
            *m_resourceRegistry,
            &m_directInstanceStream,
            m_directInstanceStreamCache.get(),
            m_directRasterMaterialSemanticKeysBySlot,
            outDigest);
    }
    catch (...)
    {
        outDigest = {};
        return false;
    }
}

bool OpaquePass::AreGPUDrivenOpaqueGroupsDrawable(
    uint32 expectedPacketCount,
    uint32 expectedGroupCount,
    uint32& outDrawItemCount) const
{
    outDrawItemCount = 0;
    if (!m_renderScene ||
        m_resourceRegistry == nullptr ||
        !m_gpuCulling)
    {
        return false;
    }

    const auto& groups = m_gpuCulling->GetDrawGroups();
    if (groups.empty())
    {
        return false;
    }
    uint32 sourceDrawItemCount = 0;
    const auto countDrawItems = [this, &sourceDrawItemCount](
                                    const std::vector<RenderDrawItem>* drawItems)
    {
        if (drawItems == nullptr)
        {
            return;
        }
        for (const RenderDrawItem& item : *drawItems)
        {
            if (IsDrawItemSelectedForSurfaceRoute(item, m_surfaceRoute))
            {
                ++sourceDrawItemCount;
            }
        }
    };
    countDrawItems(m_opaqueDrawItems);
    countDrawItems(m_maskedDrawItems);

    uint32 matchingGroupCount = 0;
    for (const GPUCullingDrawGroup& group : groups)
    {
        if (m_surfaceRoute != OpaqueRasterSurfaceRoute::AllOpaque &&
            !IsOpaqueSurfaceEligibility(
                group.batchKey.pipeline.surfacePathEligibility))
        {
            return false;
        }
        if (m_surfaceRoute != OpaqueRasterSurfaceRoute::AllOpaque &&
            !IsSurfaceEligibilitySelectedForRoute(
                group.batchKey.pipeline.surfacePathEligibility,
                m_surfaceRoute))
        {
            continue;
        }
        if (group.pipelineVariant != MaterialPipelineVariant::Opaque &&
            group.pipelineVariant != MaterialPipelineVariant::Masked)
        {
            return false;
        }

        ++matchingGroupCount;
        outDrawItemCount += group.maxDrawCount;
        MeshGPUBuffers buffers = ResolveRenderMeshBuffers(
            m_resourceRegistry, group.mesh);
        if (!buffers.IsValid() || group.maxDrawCount == 0)
        {
            return false;
        }

        if (buffers.HasSkinningVertexData())
        {
            return false;
        }
    }

    if (expectedGroupCount != 0 && matchingGroupCount != expectedGroupCount)
    {
        return false;
    }
    const uint32 requiredPacketCount =
        expectedPacketCount != 0 ? expectedPacketCount : sourceDrawItemCount;
    return outDrawItemCount > 0 &&
        outDrawItemCount == requiredPacketCount &&
        matchingGroupCount != 0;
}

bool OpaquePass::TryDrawGPUDrivenIndirect(RHICommandContext& ctx,
                                          RHIDescriptorSet* frameSet,
                                          const ObjectConstantBinding* tier1ObjectBinding,
                                          std::span<const PlannedGPUDrivenOpaqueDraw> plannedBatches,
                                          RenderPassRecordResults* results,
                                          uint32 expectedPacketCount,
                                          uint32 expectedGroupCount)
{
    const bool usesGPUSceneRaster = m_gpuSceneRasterEnabled;
    m_drawStats.gpuDrivenRequested = m_gpuDrivenOpaqueIndirectEnabled;
    m_drawStats.gpuDrivenFallbackReason = GPUDrivenDrawFallbackReason::Disabled;
    if (!m_gpuDrivenOpaqueIndirectEnabled)
    {
        return false;
    }
    if (!m_pipelineCache)
    {
        m_drawStats.gpuDrivenFallbackReason =
            GPUDrivenDrawFallbackReason::PipelineCacheUnavailable;
        return false;
    }
    if (!m_materialSystem)
    {
        m_drawStats.gpuDrivenFallbackReason =
            GPUDrivenDrawFallbackReason::MaterialSystemUnavailable;
        return false;
    }
    if (!m_gpuCulling)
    {
        m_drawStats.gpuDrivenFallbackReason =
            GPUDrivenDrawFallbackReason::CullingUnavailable;
        return false;
    }
    if ((!usesGPUSceneRaster && !m_gpuCulling->GetInstanceBuffer()) ||
        !m_gpuCulling->GetVisibleInstanceBuffer() ||
        (usesGPUSceneRaster &&
         (!m_gpuSceneRasterBinding ||
          !m_gpuSceneRasterBinding->IsReadyForBinding() ||
          m_gpuSceneRasterBinding->leaseVersion == 0)) ||
        (!m_gpuCulling->WasGpuExecutionUsedLastCull() &&
         !m_gpuCulling->WasCpuFallbackUsedLastCull()))
    {
        m_drawStats.gpuDrivenFallbackReason =
            GPUDrivenDrawFallbackReason::CullingOutputUnavailable;
        return false;
    }
    m_drawStats.gpuDrivenCullingReady = true;

    const auto& groups = m_gpuCulling->GetDrawGroups();
    uint32 matchingGroupCount = 0;
    uint32 matchingPacketCount = 0;
    for (const GPUCullingDrawGroup& group : groups)
    {
        if (m_surfaceRoute != OpaqueRasterSurfaceRoute::AllOpaque &&
            !IsOpaqueSurfaceEligibility(
                group.batchKey.pipeline.surfacePathEligibility))
        {
            m_drawStats.gpuDrivenFallbackReason =
                GPUDrivenDrawFallbackReason::DrawGroupsUnavailable;
            return false;
        }
        if (m_surfaceRoute != OpaqueRasterSurfaceRoute::AllOpaque &&
            !IsSurfaceEligibilitySelectedForRoute(
                group.batchKey.pipeline.surfacePathEligibility,
                m_surfaceRoute))
        {
            continue;
        }
        ++matchingGroupCount;
        matchingPacketCount += group.maxDrawCount;
    }
    if ((expectedGroupCount != 0 && matchingGroupCount != expectedGroupCount) ||
        plannedBatches.size() != matchingGroupCount ||
        (expectedPacketCount != 0 && matchingPacketCount != expectedPacketCount))
    {
        m_drawStats.gpuDrivenFallbackReason =
            GPUDrivenDrawFallbackReason::DrawGroupsUnavailable;
        return false;
    }
    m_drawStats.gpuDrivenPipelineReady = true;

    if (!frameSet)
    {
        m_drawStats.gpuDrivenFallbackReason =
            GPUDrivenDrawFallbackReason::FrameBindingsUnavailable;
        return false;
    }

    if (!usesGPUSceneRaster)
    {
        if (tier1ObjectBinding == nullptr || !tier1ObjectBinding->IsValid())
        {
            m_drawStats.gpuDrivenFallbackReason =
                GPUDrivenDrawFallbackReason::ObjectBindingUnavailable;
            return false;
        }
    }

    RHIDescriptorSet* objectSet = usesGPUSceneRaster
        ? m_gpuSceneRasterBinding->objectDescriptorSet.Get()
        : tier1ObjectBinding->descriptorSet.Get();
    if (!objectSet)
    {
        m_drawStats.gpuDrivenFallbackReason =
            GPUDrivenDrawFallbackReason::ObjectBindingUnavailable;
        return false;
    }
    static constexpr std::array<uint32, 1> gpuSceneObjectOffsets{0u};
    const auto objectDynamicOffsets = usesGPUSceneRaster
        ? gpuSceneObjectOffsets
        : tier1ObjectBinding->dynamicOffsets;
    m_drawStats.gpuDrivenEligible = true;
    m_drawStats.gpuDrivenIndirectExecutedDrawCountAvailable =
        m_gpuCulling->WasCpuFallbackUsedLastCull();

    bool submittedAny = false;
    for (const PlannedGPUDrivenOpaqueDraw& batch : plannedBatches)
    {
        if (!batch.rasterSubmission.IsReady())
        {
            m_drawStats.gpuDrivenFallbackReason =
                GPUDrivenDrawFallbackReason::PipelineCacheUnavailable;
            return false;
        }
        ctx.SetPipeline(batch.rasterSubmission.GetPipeline());
        if (frameSet)
        {
            ctx.SetDescriptorSet(0, frameSet);
        }
        ctx.SetDescriptorSet(1, objectSet, objectDynamicOffsets);

        ctx.SetVertexBuffer(0, batch.buffers.positionBuffer);
        ctx.SetVertexBuffer(6, m_gpuCulling->GetVisibleInstanceBuffer());
        if (batch.buffers.normalBuffer)
        {
            ctx.SetVertexBuffer(1, batch.buffers.normalBuffer);
        }
        if (batch.buffers.uvBuffer)
        {
            ctx.SetVertexBuffer(2, batch.buffers.uvBuffer);
            const GPUCullingDrawGroup& group =
                m_gpuCulling->GetDrawGroups()[batch.groupIndex];
            ctx.SetVertexBuffer(
                7,
                group.batchKey.pipeline.requiresUV1
                    ? batch.buffers.uv1Buffer
                    : batch.buffers.uvBuffer);
        }
        if (batch.buffers.tangentBuffer)
        {
            ctx.SetVertexBuffer(3, batch.buffers.tangentBuffer);
        }
        if (batch.buffers.boneIndicesBuffer)
        {
            ctx.SetVertexBuffer(4, batch.buffers.boneIndicesBuffer);
        }
        if (batch.buffers.boneWeightsBuffer)
        {
            ctx.SetVertexBuffer(5, batch.buffers.boneWeightsBuffer);
        }
        ctx.SetIndexBuffer(batch.buffers.indexBuffer, RHIFormat::R32_UINT);
        ctx.SetDescriptorSet(2, batch.materialBinding.descriptorSet, batch.materialBinding.dynamicOffsets);

        const GPUCullingIndexedIndirectSubmission cullingSubmission =
            m_gpuCulling->BuildIndexedIndirectGroupSubmission(batch.groupIndex);
        RenderSubmissionRequest request;
        request.kind = RenderSubmissionKind::IndexedIndirect;
        request.indexedIndirect = cullingSubmission.execution;
        request.capabilities = cullingSubmission.capabilities;
        static const IndexedIndirectRenderSubmissionStrategy strategy;
        const RenderSubmissionResult submission = strategy.Submit(ctx, request);
        if (submission.validationCode != RenderSubmissionValidationCode::Success)
        {
            m_drawStats.gpuDrivenFallbackReason =
                GPUDrivenDrawFallbackReason::CullingOutputUnavailable;
            return false;
        }
        if (submission.recorded)
        {
            if (results != nullptr)
            {
                static_cast<void>(results->RecordSuccessfulMaterialBinding(
                    batch.materialBinding));
            }
            submittedAny = true;
            ++m_drawStats.gpuDrivenIndirectBatchCount;
            m_drawStats.gpuDrivenIndirectSubmittedDrawUpperBound +=
                submission.submittedDrawUpperBound;
            if (submission.executedDrawCountAvailable)
            {
                m_drawStats.gpuDrivenIndirectDrawCount +=
                    submission.executedDrawCount;
            }
        }
    }

    if (submittedAny)
    {
        m_drawStats.gpuDrivenSubmitted = true;
        m_drawStats.gpuDrivenFallbackReason = GPUDrivenDrawFallbackReason::None;
        return true;
    }

    if (m_gpuCulling->WasCpuFallbackUsedLastCull() &&
        m_gpuCulling->GetDrawCount() == 0)
    {
        m_drawStats.gpuDrivenFallbackReason =
            GPUDrivenDrawFallbackReason::CulledAllDraws;
        return true;
    }

    m_drawStats.gpuDrivenFallbackReason =
        GPUDrivenDrawFallbackReason::NoIndirectSubmission;
    return false;
}

bool OpaquePass::BuildPlannedDirectBatch(
    RenderGraphPassContext& context,
    const ViewData& view,
    RHIFormat colorTargetFormat,
    RHIDescriptorSet* frameSet,
    std::vector<PlannedOpaqueDraw>& outPlannedDraws)
{
    outPlannedDraws.clear();
    m_drawStats.planRequested = true;
    m_drawStats.failureReason = RenderPolicyReason::UnexpectedRecordingFailure;
    if (view.renderFrameExecutionPlan == nullptr ||
        view.meshPassPreparation == nullptr ||
        m_renderScene == nullptr || m_resourceRegistry == nullptr ||
        m_pipelineCache == nullptr || m_materialSystem == nullptr ||
        frameSet == nullptr)
    {
        RVX_RENDER_ERROR("OpaquePass: Direct batch preflight is missing required frame dependencies");
        return false;
    }

    const DirectDrawPacketBatchBuildResult built =
        BuildDirectDrawPacketBatch(*view.renderFrameExecutionPlan,
                                   RenderPassKind::Opaque,
                                   view.meshPassPreparation->opaque,
                                   view.renderVisibility);
    if (!built.succeeded ||
        built.batch.packets.size() > std::numeric_limits<uint32>::max())
    {
        RVX_RENDER_ERROR(
            "OpaquePass: Direct packet batch construction failed (reason={}, packets={})",
            GetRenderPolicyReasonName(built.reason),
            built.batch.packets.size());
        return false;
    }

    std::vector<const DirectDrawPacket*> selectedPackets;
    selectedPackets.reserve(built.batch.packets.size());
    for (const DirectDrawPacket& candidate : built.batch.packets)
    {
        if (IsPacketInvalidForOpaqueRoute(candidate.packet))
        {
            RVX_RENDER_ERROR(
                "OpaquePass: Direct packet {} is invalid for the opaque surface routes",
                candidate.sourceOrdinal);
            return false;
        }
        if (IsPacketSelectedForSurfaceRoute(
                candidate.packet, m_surfaceRoute))
        {
            selectedPackets.push_back(&candidate);
        }
    }

    m_drawStats.planValidated = true;
    m_drawStats.plannedPacketCount =
        static_cast<uint32>(selectedPackets.size());
    m_drawStats.compiledPacketCount = m_drawStats.plannedPacketCount;

    if (!ValidateRetainedObjectIdentityIndex(*m_renderScene))
    {
        RVX_RENDER_ERROR(
            "OpaquePass: Direct packet batch failed retained object identity-index validation");
        return false;
    }

    outPlannedDraws.reserve(selectedPackets.size());
    for (const DirectDrawPacket* drawPtr : selectedPackets)
    {
        const DirectDrawPacket& draw = *drawPtr;
        const RenderDrawPacket& packet = draw.packet;
        if (packet.pass != RenderPassKind::Opaque ||
            !packet.pipelineKey.materialPolicyValid ||
            packet.primitiveData == RVX_INVALID_PRIMITIVE_DATA_INDEX ||
            !packet.geometryKey.mesh.IsValid() ||
            packet.geometryKey.indexType != MeshUploadIndexType::UInt32 ||
            packet.pipelineKey.topology != MeshUploadPrimitiveTopology::Triangles ||
            packet.arguments.indexCount == 0 ||
            packet.arguments.instanceCount != 1 ||
            packet.arguments.firstInstance != 0)
        {
            RVX_RENDER_ERROR(
                "OpaquePass: Direct packet {} failed draw/material-policy preflight",
                draw.sourceOrdinal);
            outPlannedDraws.clear();
            return false;
        }

        const RenderObject* const object = ResolveUniqueObject(
            *m_renderScene, packet.objectId, packet.primitiveData);
        if (object == nullptr ||
            object->mesh != packet.geometryKey.mesh ||
            (object->hasSkinningPaletteProvider &&
             !object->HasValidSkinningPalette()))
        {
            RVX_RENDER_ERROR(
                "OpaquePass: Direct packet {} failed unique render-object resolution",
                draw.sourceOrdinal);
            outPlannedDraws.clear();
            return false;
        }

        const bool masked = IsMaskedPacket(packet);
        const bool skinned = IsSkinnedPacket(packet);
        const bool missingMaterial =
            HasDrawFlag(packet.flags, RenderDrawFlags::MissingMaterial) ||
            !packet.materialKey.material.IsValid();
        const RenderSubmissionLayout expectedLayout =
            MakeExpectedDirectLayout(packet);
        if (draw.layout != expectedLayout ||
            draw.layout.primitiveDataBinding !=
                PrimitiveDataBinding::PerDrawConstants ||
            (static_cast<uint32>(draw.layout.vertexStreams) &
             static_cast<uint32>(MeshPassVertexStreams::InstanceIndex)) != 0 ||
            (masked && !HasDrawFlag(packet.flags, RenderDrawFlags::Masked)) ||
            (!masked && HasDrawFlag(packet.flags, RenderDrawFlags::Masked)) ||
            (packet.pipelineKey.materialVariant != MaterialPipelineVariant::Opaque &&
             packet.pipelineKey.materialVariant != MaterialPipelineVariant::Masked) ||
            packet.pipelineKey.materialVariant !=
                GetPipelineVariantForRenderMode(packet.materialKey.materialMode) ||
            packet.pipelineKey.skinned != skinned ||
            skinned != object->HasSkinningData() ||
            (masked && packet.materialKey.materialMode != MaterialRenderMode::Masked) ||
            (!masked && packet.materialKey.materialMode != MaterialRenderMode::Opaque) ||
            missingMaterial !=
                HasDrawFlag(packet.flags, RenderDrawFlags::MissingMaterial) ||
            HasDrawFlag(packet.flags, RenderDrawFlags::CastsShadow) !=
                (packet.pipelineKey.materialPolicyValid &&
                 object->castsShadow &&
                 MaterialShadowPolicyCasts(
                     packet.pipelineKey.shadowPolicy)) ||
            HasDrawFlag(packet.flags, RenderDrawFlags::ReceivesShadow) !=
                (packet.pipelineKey.materialPolicyValid &&
                 object->receivesShadow &&
                 MaterialShadowPolicyReceives(
                     packet.pipelineKey.shadowPolicy)))
        {
            RVX_RENDER_ERROR(
                "OpaquePass: Direct packet {} failed material/layout semantic preflight",
                draw.sourceOrdinal);
            outPlannedDraws.clear();
            return false;
        }

        const RenderMeshResourceData* const mesh =
            m_resourceRegistry->ResolveMesh(packet.geometryKey.mesh);
        const bool hasPosition = mesh != nullptr && HasMeshBufferSemantic(
            *mesh, RenderMeshBufferSemantic::Position);
        const bool hasNormal = mesh != nullptr && HasMeshBufferSemantic(
            *mesh, RenderMeshBufferSemantic::Normal);
        const bool hasUV = mesh != nullptr && HasMeshBufferSemantic(
            *mesh, RenderMeshBufferSemantic::UV);
        const bool hasUV1 = mesh != nullptr && HasMeshBufferSemantic(
            *mesh, RenderMeshBufferSemantic::UV1);
        const bool hasTangent = mesh != nullptr && HasMeshBufferSemantic(
            *mesh, RenderMeshBufferSemantic::Tangent);
        const bool hasIndex = mesh != nullptr && HasMeshBufferSemantic(
            *mesh, RenderMeshBufferSemantic::Index);
        MeshUploadSubmesh syntheticSubmesh;
        const MeshUploadSubmesh* const submesh = mesh != nullptr
            ? ResolveMeshSubmesh(*mesh, packet.geometryKey.submeshIndex,
                                 syntheticSubmesh)
            : nullptr;
        if (!hasPosition || !hasNormal || !hasUV || !hasTangent || !hasIndex ||
            (packet.pipelineKey.requiresUV1 && !hasUV1) ||
            submesh == nullptr)
        {
            RVX_RENDER_ERROR(
                "OpaquePass: Direct packet {} has incomplete uploaded mesh buffers "
                "(mesh={}:{}, submesh={}, valid={}, position={}, normal={}, "
                "uv={}, uv1={}, requiresUV1={}, tangent={}, index={}, submeshCount={})",
                draw.sourceOrdinal,
                packet.geometryKey.mesh.slot,
                packet.geometryKey.mesh.generation,
                packet.geometryKey.submeshIndex,
                hasPosition && hasIndex,
                hasPosition,
                hasNormal,
                hasUV,
                hasUV1,
                packet.pipelineKey.requiresUV1,
                hasTangent,
                hasIndex,
                mesh != nullptr
                    ? (mesh->submeshes.empty() && mesh->createInfo.indexCount != 0
                           ? 1u
                           : static_cast<uint32>(mesh->submeshes.size()))
                    : 0u);
            outPlannedDraws.clear();
            return false;
        }

        if (packet.submeshIndex != packet.geometryKey.submeshIndex ||
            packet.arguments.indexCount != submesh->indexCount ||
            packet.arguments.firstIndex != submesh->indexOffset ||
            packet.arguments.vertexOffset != submesh->baseVertex)
        {
            RVX_RENDER_ERROR(
                "OpaquePass: Direct packet {} does not match uploaded submesh arguments",
                draw.sourceOrdinal);
            outPlannedDraws.clear();
            return false;
        }

        const bool allowNormalMap = hasNormal && hasUV && hasTangent &&
            mesh->createInfo.hasTangentBasis;
        if (skinned &&
            (!object->HasSkinningData() ||
             !HasMeshBufferSemantic(*mesh,
                                    RenderMeshBufferSemantic::BoneIndices) ||
             !HasMeshBufferSemantic(*mesh,
                                    RenderMeshBufferSemantic::BoneWeights)))
        {
            RVX_RENDER_ERROR(
                "OpaquePass: Direct packet {} has incomplete skinning inputs",
                draw.sourceOrdinal);
            outPlannedDraws.clear();
            return false;
        }

        const bool usesDefaultMaterial =
            HasMeshPassBindingRequirement(draw.layout.bindings,
                                          MeshPassBindingRequirements::DefaultMaterial);
        const bool usesMaterial =
            HasMeshPassBindingRequirement(draw.layout.bindings,
                                          MeshPassBindingRequirements::Material);
        if ((missingMaterial &&
             (!usesDefaultMaterial || usesMaterial)) ||
            (!missingMaterial &&
             (usesDefaultMaterial || !usesMaterial)))
        {
            RVX_RENDER_ERROR(
                "OpaquePass: Direct packet {} has inconsistent material binding requirements",
                draw.sourceOrdinal);
            outPlannedDraws.clear();
            return false;
        }

        PlannedOpaqueDraw planned;
        planned.packet = draw;
        planned.frameSet = frameSet;
        planned.allowNormalMap = allowNormalMap;
        planned.receivesShadow = HasDrawFlag(
            packet.flags, RenderDrawFlags::ReceivesShadow);
        planned.skinned = skinned;
        if (skinned && object->HasValidSkinningPalette())
        {
            planned.skinningPalette = object->skinningPalette;
        }
        if (planned.frameSet == nullptr)
        {
            RVX_RENDER_ERROR(
                "OpaquePass: Direct packet {} produced an incomplete frame descriptor",
                draw.sourceOrdinal);
            outPlannedDraws.clear();
            return false;
        }
        outPlannedDraws.push_back(std::move(planned));
    }

    if (m_surfaceRoute == OpaqueRasterSurfaceRoute::AllOpaque)
    {
        ApplyDirectInstancePlan(
            context, view, colorTargetFormat, outPlannedDraws);
    }

    // Resolve the copying MeshGPUBuffers view only after the transactional
    // instance-plan compaction. A failed compaction leaves every source packet
    // in place, so the ordinary fallback still materializes each one.
    for (PlannedOpaqueDraw& planned : outPlannedDraws)
    {
        const RenderDrawPacket& packet = planned.packet.packet;
        const RenderObject* const object = ResolveUniqueObject(
            *m_renderScene, packet.objectId, packet.primitiveData);
        MeshGPUBuffers buffers = ResolveRenderMeshBuffers(
            m_resourceRegistry, packet.geometryKey.mesh);
        if (object == nullptr ||
            object->mesh != packet.geometryKey.mesh ||
            (object->hasSkinningPaletteProvider &&
             !object->HasValidSkinningPalette()) ||
            planned.skinned != IsSkinnedPacket(packet) ||
            planned.skinned != object->HasSkinningData() ||
            planned.receivesShadow !=
                (packet.pipelineKey.materialPolicyValid &&
                 object->receivesShadow &&
                 MaterialShadowPolicyReceives(
                     packet.pipelineKey.shadowPolicy)) ||
            !buffers.IsValid() || buffers.positionBuffer == nullptr ||
            buffers.normalBuffer == nullptr || !buffers.hasNormals ||
            buffers.uvBuffer == nullptr || !buffers.hasUVs ||
            (packet.pipelineKey.requiresUV1 &&
             !buffers.HasTextureCoordinateSet(1)) ||
            buffers.tangentBuffer == nullptr || buffers.indexBuffer == nullptr ||
            packet.geometryKey.submeshIndex >= buffers.submeshes.size())
        {
            RVX_RENDER_ERROR(
                "OpaquePass: Direct packet {} failed final geometry/object materialization",
                planned.packet.sourceOrdinal);
            outPlannedDraws.clear();
            return false;
        }

        const SubmeshGPUInfo submesh =
            buffers.submeshes[packet.geometryKey.submeshIndex];
        if (packet.submeshIndex != packet.geometryKey.submeshIndex ||
            packet.arguments.indexCount != submesh.indexCount ||
            packet.arguments.firstIndex != submesh.indexOffset ||
            packet.arguments.vertexOffset != submesh.baseVertex ||
            (planned.skinned && !buffers.HasSkinningVertexData()))
        {
            RVX_RENDER_ERROR(
                "OpaquePass: Direct packet {} changed after lexical geometry validation",
                planned.packet.sourceOrdinal);
            outPlannedDraws.clear();
            return false;
        }

        planned.allowNormalMap = buffers.HasNormalMapTangentBasis();
        planned.submesh = submesh;
        planned.buffers = std::move(buffers);
        if (planned.instanced)
        {
            if (!planned.rasterSubmission.IsReady())
            {
                RVX_RENDER_ERROR(
                    "OpaquePass: Direct instanced packet {} lost its graphics pipeline",
                    planned.packet.sourceOrdinal);
                outPlannedDraws.clear();
                return false;
            }
            continue;
        }

        const OpaqueRasterPipelineSelector pipelineSelector(*m_pipelineCache);
        planned.rasterSubmission = pipelineSelector.Prepare(
            MakeOpaquePipelineRequest(
                m_outputFamily,
                m_surfaceRoute,
                OpaqueRasterSubmissionRoute::Direct,
                packet.pipelineKey.materialVariant,
                colorTargetFormat,
                packet.pipelineKey.cullMode,
                packet.pipelineKey.reverseWinding,
                planned.skinned,
                false));
        if (!planned.rasterSubmission.IsReady() ||
            !context.RetainSubmissionResource(
                Ref<RefCounted>(planned.rasterSubmission.GetPipeline())))
        {
            RVX_RENDER_ERROR(
                "OpaquePass: Direct packet {} has no compatible graphics pipeline",
                planned.packet.sourceOrdinal);
            outPlannedDraws.clear();
            return false;
        }
    }

    for (PlannedOpaqueDraw& planned : outPlannedDraws)
    {
        const RenderDrawPacket& packet = planned.packet.packet;
        const bool missingMaterial =
            HasDrawFlag(packet.flags, RenderDrawFlags::MissingMaterial) ||
            !packet.materialKey.material.IsValid();
        const bool allowNormalMap = planned.allowNormalMap;
        MaterialBindingOptions materialOptions;
        materialOptions.allowNormalMap = allowNormalMap;
        materialOptions.materialParameterTable =
            planned.usesMaterialParameterTable
                ? m_directMaterialParameterTable.Get()
                : nullptr;
        if (planned.usesMaterialParameterTable &&
            materialOptions.materialParameterTable == nullptr)
        {
            RVX_RENDER_ERROR(
                "OpaquePass: Direct packet {} has no material parameter table for its instanced batch",
                planned.packet.sourceOrdinal);
            outPlannedDraws.clear();
            return false;
        }

        planned.materialBinding = m_materialSystem->PrepareMaterialBinding(
            packet.materialKey.material, nullptr, materialOptions);
        if (!planned.materialBinding.IsDrawable() ||
            (missingMaterial && !planned.materialBinding.usedFallback) ||
            !context.RetainSubmissionResource(
                Ref<RefCounted>(planned.materialBinding.constantBuffer)) ||
            !context.RetainSubmissionResource(
                Ref<RefCounted>(planned.materialBinding.descriptorSetRef)))
        {
            RVX_RENDER_ERROR(
                "OpaquePass: Direct packet {} material binding or submission ownership failed: {}",
                planned.packet.sourceOrdinal,
                planned.materialBinding.message);
            ++m_drawStats.skippedMaterialBindingCount;
            outPlannedDraws.clear();
            return false;
        }
        ++m_drawStats.materialBindingCount;
        if (planned.materialBinding.usedFallback)
        {
            ++m_drawStats.materialFallbackBindingCount;
        }
        m_drawStats.materialTextureFlags |= planned.materialBinding.textureFlags;
        m_drawStats.materialFallbackTextureFlags |=
            planned.materialBinding.fallbackTextureFlags;

        if (planned.instanced)
        {
            continue;
        }
        const RenderObject* const object = ResolveUniqueObject(
            *m_renderScene, packet.objectId, packet.primitiveData);
        if (object == nullptr)
        {
            RVX_RENDER_ERROR(
                "OpaquePass: Direct packet {} lost its render-object identity before binding",
                planned.packet.sourceOrdinal);
            outPlannedDraws.clear();
            return false;
        }
        SkinningPaletteBufferBinding skinningBuffers;
        SkinningPaletteSliceBinding skinningSlice;
        if (!ResolveSkinningPaletteBinding(
                *object, planned.buffers, m_deformationResources.get(),
                skinningBuffers, skinningSlice) ||
            !m_pipelineCache->CreateObjectConstantBinding(
                object->worldMatrix,
                object->normalMatrix,
                object->previousWorldMatrix,
                view.previousViewProjectionMatrix,
                object->previousWorldMatrixValid != 0 &&
                    view.previousViewProjectionValid != 0 &&
                    !view.resetTemporalHistory,
                planned.receivesShadow,
                skinningBuffers,
                skinningSlice,
                nullptr,
                planned.objectBinding) ||
            !context.RetainSubmissionResource(
                Ref<RefCounted>(planned.objectBinding.constantBuffer)) ||
            (planned.objectBinding.instanceBuffer &&
             !context.RetainSubmissionResource(
                 Ref<RefCounted>(planned.objectBinding.instanceBuffer))) ||
            !context.RetainSubmissionResource(
                Ref<RefCounted>(planned.objectBinding.descriptorSet)))
        {
            RVX_RENDER_ERROR(
                "OpaquePass: Direct packet {} object constants or submission ownership failed",
                planned.packet.sourceOrdinal);
            outPlannedDraws.clear();
            return false;
        }
        planned.skinningSlice = skinningSlice;
    }
    return true;
}

void OpaquePass::ApplyDirectInstancePlan(
    RenderGraphPassContext& context,
    const ViewData& view,
    RHIFormat colorTargetFormat,
    std::vector<PlannedOpaqueDraw>& plannedDraws)
{
    if (m_renderScene == nullptr ||
        view.instancingMode == RenderInstancingMode::Disabled ||
        m_directInstancePlan.instancedBatchCount == 0)
    {
        return;
    }
    const auto fallback = [this]()
    {
        m_drawStats.instancingFallbackBatchCount +=
            m_directInstancePlan.instancedBatchCount;
    };
    if (m_directInstancingPreflightFailed ||
        !m_directInstanceStream.IsValid() ||
        m_directInstancePlan.pass != RenderPassKind::Opaque ||
        m_directInstancePlan.executedPacketCount != plannedDraws.size() ||
        m_directInstancePlan.submittedInstanceCount != plannedDraws.size() ||
        m_directInstancePlan.submittedDrawCount !=
            m_directInstancePlan.batches.size() ||
        m_directInstanceStream.batchBindings.size() !=
            m_directInstancePlan.instancedBatchCount)
    {
        fallback();
        return;
    }

    std::vector<bool> consumed(plannedDraws.size(), false);
    uint32 instancedBatchCount = 0;
    size_t batchBindingIndex = 0;
    for (const RenderInstanceBatch& batch : m_directInstancePlan.batches)
    {
        if (batch.members.empty() ||
            batch.members.size() > std::numeric_limits<uint32>::max() ||
            (batch.instanced &&
             (batch.members.size() < 2 ||
              batch.reason != RenderInstanceBatchReason::None)) ||
            (!batch.instanced && batch.members.size() != 1))
        {
            fallback();
            return;
        }

        for (const RenderInstanceBatchMember& member : batch.members)
        {
            if (member.directPacketIndex >= plannedDraws.size() ||
                consumed[member.directPacketIndex])
            {
                fallback();
                return;
            }

            const PlannedOpaqueDraw& planned =
                plannedDraws[member.directPacketIndex];
            const RenderDrawGroupKey memberKey = MakeRenderInstanceBatchKey(
                planned.packet.packet, planned.packet.layout);
            if (member.packetId != planned.packet.packetId ||
                memberKey != batch.key ||
                (batch.key.usesMaterialParameterTable &&
                 (!planned.packet.packet.materialInstanceKey.parameterTableCompatible ||
                  planned.packet.packet.materialInstanceKey.textureBindingHash !=
                      batch.key.instanceMaterial.textureBindingHash)) ||
                (!batch.key.usesMaterialParameterTable &&
                 planned.packet.packet.materialKey.material !=
                     batch.key.material.material))
            {
                fallback();
                return;
            }
            consumed[member.directPacketIndex] = true;
        }

        if (!batch.instanced)
        {
            continue;
        }
        if (batch.key.usesMaterialParameterTable &&
            !m_directMaterialParameterTable)
        {
            fallback();
            return;
        }
        if (batchBindingIndex >= m_directInstanceStream.batchBindings.size())
        {
            fallback();
            return;
        }
        const RasterInstanceStreamBatchBinding& streamBinding =
            m_directInstanceStream.batchBindings[batchBindingIndex++];
        if (streamBinding.key != batch.key ||
            streamBinding.firstInstance != batch.firstInstance ||
            streamBinding.instanceCount != batch.members.size())
        {
            fallback();
            return;
        }
        ++instancedBatchCount;
    }
    if (!std::all_of(consumed.begin(), consumed.end(),
                     [](bool value) { return value; }) ||
        instancedBatchCount != m_directInstancePlan.instancedBatchCount ||
        batchBindingIndex != m_directInstanceStream.batchBindings.size())
    {
        fallback();
        return;
    }

    struct InstancedBinding
    {
        uint32 leaderIndex = 0;
        OpaqueRasterPreparedSubmission rasterSubmission;
        ObjectConstantBinding objectBinding;
    };
    std::vector<InstancedBinding> bindings;
    bindings.reserve(instancedBatchCount);
    for (const RenderInstanceBatch& batch : m_directInstancePlan.batches)
    {
        if (!batch.instanced)
        {
            continue;
        }

        const uint32 leaderIndex = batch.members.front().directPacketIndex;
        const PlannedOpaqueDraw& leader = plannedDraws[leaderIndex];
        const OpaqueRasterPipelineSelector pipelineSelector(*m_pipelineCache);
        OpaqueRasterPreparedSubmission rasterSubmission = pipelineSelector.Prepare(
            MakeOpaquePipelineRequest(
                m_outputFamily,
                m_surfaceRoute,
                OpaqueRasterSubmissionRoute::Instanced,
                leader.packet.packet.pipelineKey.materialVariant,
                colorTargetFormat,
                leader.packet.packet.pipelineKey.cullMode,
                leader.packet.packet.pipelineKey.reverseWinding,
                false,
                batch.key.usesMaterialParameterTable));
        ObjectConstantBinding objectBinding;
        if (!rasterSubmission.IsReady() || leader.skinned ||
            !context.RetainSubmissionResource(
                Ref<RefCounted>(rasterSubmission.GetPipeline())) ||
            !m_pipelineCache->CreateObjectConstantBinding(
                Mat4Identity(),
                Mat4Identity(),
                Mat4Identity(),
                view.previousViewProjectionMatrix,
                false,
                leader.receivesShadow,
                {},
                {},
                m_directInstanceStream.instances.Get(),
                objectBinding) ||
            !context.RetainSubmissionResource(
                Ref<RefCounted>(objectBinding.constantBuffer)) ||
            !context.RetainSubmissionResource(
                Ref<RefCounted>(objectBinding.instanceBuffer)) ||
            !context.RetainSubmissionResource(
                Ref<RefCounted>(objectBinding.descriptorSet)))
        {
            fallback();
            return;
        }
        bindings.push_back(
            {leaderIndex, std::move(rasterSubmission), std::move(objectBinding)});
    }

    std::vector<PlannedOpaqueDraw> batched;
    batched.reserve(m_directInstancePlan.batches.size());
    size_t bindingIndex = 0;
    size_t committedBatchBindingIndex = 0;
    for (const RenderInstanceBatch& batch : m_directInstancePlan.batches)
    {
        const uint32 leaderIndex = batch.members.front().directPacketIndex;
        if (!batch.instanced)
        {
            batched.push_back(std::move(plannedDraws[leaderIndex]));
            continue;
        }

        PlannedOpaqueDraw leader = std::move(plannedDraws[leaderIndex]);
        const RasterInstanceStreamBatchBinding& streamBinding =
            m_directInstanceStream.batchBindings[committedBatchBindingIndex++];
        leader.rasterSubmission =
            std::move(bindings[bindingIndex].rasterSubmission);
        leader.objectBinding = std::move(bindings[bindingIndex].objectBinding);
        leader.instanceIndexBuffer = m_directInstanceStream.instanceIndices;
        leader.packet.packet.arguments.instanceCount =
            static_cast<uint32>(batch.members.size());
        leader.packet.packet.arguments.firstInstance = streamBinding.firstInstance;
        leader.representedPacketCount =
            static_cast<uint32>(batch.members.size());
        leader.instanced = true;
        leader.usesMaterialParameterTable =
            batch.key.usesMaterialParameterTable;
        batched.push_back(std::move(leader));
        ++bindingIndex;
    }
    plannedDraws = std::move(batched);
}

bool OpaquePass::TryDrawPlannedDirect(
    RHICommandContext& ctx,
    std::span<const PlannedOpaqueDraw> plannedDraws,
    RenderPassRecordResults* results)
{
    static const DirectRenderSubmissionStrategy strategy;
    for (const PlannedOpaqueDraw& planned : plannedDraws)
    {
        if (!planned.rasterSubmission.IsReady())
        {
            return false;
        }
        ctx.SetPipeline(planned.rasterSubmission.GetPipeline());
        ctx.SetDescriptorSet(0, planned.frameSet);
        ctx.SetDescriptorSet(1,
                             planned.objectBinding.descriptorSet.Get(),
                             planned.objectBinding.dynamicOffsets);
        ctx.SetDescriptorSet(2,
                             planned.materialBinding.descriptorSet,
                             planned.materialBinding.dynamicOffsets);
        ctx.SetVertexBuffer(0, planned.buffers.positionBuffer);
        ctx.SetVertexBuffer(1, planned.buffers.normalBuffer);
        ctx.SetVertexBuffer(2, planned.buffers.uvBuffer);
        ctx.SetVertexBuffer(3, planned.buffers.tangentBuffer);
        ctx.SetVertexBuffer(
            7,
            planned.packet.packet.pipelineKey.requiresUV1
                ? planned.buffers.uv1Buffer
                : planned.buffers.uvBuffer);
        if (planned.skinned)
        {
            ctx.SetVertexBuffer(4, planned.buffers.boneIndicesBuffer);
            ctx.SetVertexBuffer(5, planned.buffers.boneWeightsBuffer);
        }
        if (planned.instanced)
        {
            ctx.SetVertexBuffer(6, planned.instanceIndexBuffer.Get());
        }
        ctx.SetIndexBuffer(planned.buffers.indexBuffer, RHIFormat::R32_UINT);

        const RenderDrawArguments& args = planned.packet.packet.arguments;
        RenderSubmissionRequest request;
        request.kind = RenderSubmissionKind::DirectIndexed;
        request.directIndexed = {
            args.indexCount,
            args.instanceCount,
            args.firstIndex,
            args.vertexOffset,
            args.firstInstance};
        const RenderSubmissionResult submission = strategy.Submit(ctx, request);
        if (submission.validationCode != RenderSubmissionValidationCode::Success)
        {
            return false;
        }
        if (submission.recorded)
        {
            if (results != nullptr)
            {
                static_cast<void>(results->RecordSuccessfulMaterialBinding(
                    planned.materialBinding));
                if (planned.skinned && planned.skinningPalette.paletteCount != 0)
                {
                    static_cast<void>(results->RecordSuccessfulSkinningPalette(
                        planned.skinningPalette,
                        RenderSkinningPaletteExecutionLane::Direct));
                }
            }
            ++m_drawStats.directDrawCount;
            ++m_drawStats.submittedDrawCount;
            m_drawStats.submittedInstanceCount += args.instanceCount;
            m_drawStats.executedPacketCount += planned.representedPacketCount;
            if (planned.instanced)
            {
                ++m_drawStats.instancedBatchCount;
            }
            if (planned.skinned)
            {
                if (m_deformationResources == nullptr ||
                    (m_drawStats.deformationContentHash != 0 &&
                     m_drawStats.deformationContentHash !=
                         m_deformationResources->GetContentHash()))
                {
                    return false;
                }
                m_drawStats.deformationContentHash =
                    m_deformationResources->GetContentHash();
                ++m_drawStats.skinnedDrawCount;
                ++m_drawStats.currentPaletteBoundDrawCount;
            }
        }
    }
    m_drawStats.directPacketPathUsed = true;
    return true;
}

void OpaquePass::Execute(RHICommandContext& ctx, const ViewData& view)
{
    (void)ctx;
    (void)view;
    // Typed AddToGraph owns graph resource realization and execution.
}

void OpaquePass::Execute(
    RenderGraphPassContext& context,
    const ViewData& view,
    RenderPassRecordResults* results)
{
    RHICommandContext& ctx = context.Commands();
    RequiredOpaqueExecutionGuard executionGuard(context);
    m_drawStats = {};
    const bool sceneHasSkinning = m_renderScene != nullptr &&
        std::any_of(m_renderScene->GetObjects().begin(),
                    m_renderScene->GetObjects().end(),
                    [](const RenderObject& object)
                    {
                        return object.HasSkinningData();
                    });
    if (sceneHasSkinning && !HasExactDeformationBuffers(
            context, m_deformationResources.get(), m_currentPaletteReadHandle,
            m_previousPaletteReadHandle))
    {
        m_drawStats.failureReason = RenderPolicyReason::ResourcesUnavailable;
        return;
    }
    m_drawStats.directInstanceUploadBytes =
        m_directInstanceStream.instanceUploadBytes;
    m_drawStats.directInstanceIndexUploadBytes =
        m_directInstanceStream.indexUploadBytes;
    m_drawStats.directInstancePatchedRowCount =
        m_directInstanceStream.instancePatchedRowCount;
    m_drawStats.directInstanceIndexPatchedRowCount =
        m_directInstanceStream.indexPatchedRowCount;
    m_drawStats.directInstanceActiveCount =
        m_directInstanceStream.activeInstanceCount;
    m_drawStats.directInstanceActiveCapacity =
        m_directInstanceStream.activeInstanceCapacity;
    m_drawStats.directInstanceFullMaterialization =
        m_directInstanceStream.instanceFullMaterialization;
    m_drawStats.directInstanceIndexFullMaterialization =
        m_directInstanceStream.indexFullMaterialization;
    m_drawStats.directInstanceUploadWork =
        m_directInstanceStream.instanceUploadWork;
    m_drawStats.directInstanceIndexUploadWork =
        m_directInstanceStream.indexUploadWork;
    if (m_directInstanceStreamCache)
    {
        m_drawStats.directMutationTotals =
            m_directInstanceStreamCache->mutationTotals;
        m_drawStats.directMutationTotalsSaturated =
            m_directInstanceStreamCache->mutationTotalsSaturated;
    }
    m_drawStats.instancingMode = view.instancingMode;
    m_drawStats.instancingPlanAvailable =
        m_drawStats.instancingMode == RenderInstancingMode::Auto &&
        m_directInstancePlan.IsComplete();
    m_drawStats.instancingPreflightSucceeded =
        m_drawStats.instancingPlanAvailable &&
        !m_directInstancingPreflightFailed;
    if (m_drawStats.instancingPlanAvailable)
    {
        m_drawStats.plannedInstancingPacketCount =
            m_directInstancePlan.executedPacketCount;
        m_drawStats.plannedInstancingDrawCount =
            m_directInstancePlan.submittedDrawCount;
        m_drawStats.plannedInstancingInstanceCount =
            m_directInstancePlan.submittedInstanceCount;
        m_drawStats.plannedInstancingBatchCount =
            m_directInstancePlan.instancedBatchCount;
    }
    uint64 gpuLaneSubmissionCpuNanoseconds = 0;
    uint64 directLaneSubmissionCpuNanoseconds = 0;
    bool gpuLaneSubmissionTimingAvailable = false;
    bool directLaneSubmissionTimingAvailable = false;

    const bool hasPublishedPlan = view.renderFrameExecutionPlan != nullptr;
    const RenderPassExecutionPlan* opaquePlan = [&]()
    {
        if (view.renderFrameExecutionPlan == nullptr)
        {
            return static_cast<const RenderPassExecutionPlan*>(nullptr);
        }
        for (const RenderPassExecutionPlan& candidate :
             view.renderFrameExecutionPlan->passes)
        {
            if (candidate.pass == RenderPassKind::Opaque)
            {
                return &candidate;
            }
        }
        return static_cast<const RenderPassExecutionPlan*>(nullptr);
    }();
    m_drawStats.planRequested = hasPublishedPlan;

    const auto updatePlanReport = [&](RenderExecutionStatus status,
                                      RenderPolicyReason reason,
                                      uint32 executedPackets,
                                      bool gpuLane,
                                      RenderVisibilityMode visibility)
    {
        if (status == RenderExecutionStatus::Failed &&
            m_gpuSceneRasterEnabled && m_gpuSceneRecordingFailure)
        {
            m_gpuSceneRecordingFailure->store(true);
        }
        if (view.renderFrameExecutionReport == nullptr)
        {
            return;
        }
        // Deferred routes publish their own evidence to RenderPassRecordResults
        // and are aggregated only after both have recorded.  Updating this
        // legacy one-pass report here would overwrite the first route with
        // the second and make the result look complete when it is not.
        if (m_surfaceRoute != OpaqueRasterSurfaceRoute::AllOpaque)
        {
            return;
        }
        for (RenderPassExecutionReport& report :
             view.renderFrameExecutionReport->passes)
        {
            if (report.pass != RenderPassKind::Opaque)
            {
                continue;
            }
            report.status = status;
            report.executedVisibility = visibility;
            report.reason = reason;
            report.materialBindingsAvailable = true;
            report.materialBindingCount = m_drawStats.materialBindingCount;
            report.materialFallbackBindingCount =
                m_drawStats.materialFallbackBindingCount;
            report.materialTextureFlags = m_drawStats.materialTextureFlags;
            report.materialFallbackTextureFlags =
                m_drawStats.materialFallbackTextureFlags;
            RenderPassLaneExecutionReport& lane =
                gpuLane ? report.gpuDrivenLane : report.directLane;
            lane.status = status;
            lane.reason = reason;
            lane.executedCountsAvailable = !gpuLane ||
                m_drawStats.gpuDrivenIndirectExecutedDrawCountAvailable;
            lane.executedPacketCount = lane.executedCountsAvailable
                ? executedPackets
                : 0;
            lane.executedDrawCount = lane.executedCountsAvailable
                ? (gpuLane ? m_drawStats.gpuDrivenIndirectDrawCount
                           : m_drawStats.directDrawCount)
                : 0;
            if (status == RenderExecutionStatus::Failed)
            {
                view.renderFrameExecutionReport->status = status;
            }
            break;
        }
        view.renderFrameExecutionReport->frameSequence =
            view.renderFrameExecutionPlan
                ? view.renderFrameExecutionPlan->frameSequence
                : 0;
    };

    const auto reportPlannedFailure = [&](bool gpuLane)
    {
        if (!hasPublishedPlan)
        {
            return;
        }
        m_drawStats.failureReason = RenderPolicyReason::UnexpectedRecordingFailure;
        updatePlanReport(RenderExecutionStatus::Failed,
                         m_drawStats.failureReason,
                         0,
                         gpuLane,
                         opaquePlan ? opaquePlan->visibility
                                    : RenderVisibilityMode::Cpu);
    };

    OpaqueRoutePlanCounts routePlan;
    if (opaquePlan != nullptr &&
        !CountOpaqueRoutePackets(
            *view.renderFrameExecutionPlan,
            *opaquePlan,
            m_surfaceRoute,
            routePlan))
    {
        m_drawStats.failureReason = RenderPolicyReason::InconsistentFacts;
        updatePlanReport(
            RenderExecutionStatus::Failed,
            m_drawStats.failureReason,
            0,
            false,
            opaquePlan->visibility);
        return;
    }

    if (hasPublishedPlan &&
        (opaquePlan == nullptr || view.meshPassPreparation == nullptr))
    {
        m_drawStats.failureReason = RenderPolicyReason::InconsistentFacts;
        updatePlanReport(RenderExecutionStatus::Failed,
                         m_drawStats.failureReason,
                         0,
                         false,
                         opaquePlan ? opaquePlan->visibility
                                    : RenderVisibilityMode::Cpu);
        return;
    }

    // Validate dependencies
    if (!m_pipelineCache || !m_pipelineCache->IsInitialized())
    {
        RVX_CORE_WARN("OpaquePass: PipelineCache not available (initialized: {})",
                      m_pipelineCache ? m_pipelineCache->IsInitialized() : false);
        reportPlannedFailure(routePlan.gpuPacketCount != 0);
        return;
    }

    if (!m_materialSystem || !m_materialSystem->IsInitialized())
    {
        RVX_CORE_WARN("OpaquePass: MaterialSystem not available");
        reportPlannedFailure(routePlan.gpuPacketCount != 0);
        return;
    }

    std::array<RHITextureView*, 4> gBufferTargetViews{};
    if (m_outputFamily == OpaqueRasterOutputFamily::DeferredGBuffer)
    {
        for (uint32 index = 0; index < gBufferTargetViews.size(); ++index)
        {
            if (!m_gBufferTargetViewHandles[index].IsValid())
            {
                RVX_CORE_WARN(
                    "OpaquePass: deferred GBuffer target {} was not declared",
                    index);
                reportPlannedFailure(routePlan.gpuPacketCount != 0);
                return;
            }
            gBufferTargetViews[index] =
                context.GetTextureView(m_gBufferTargetViewHandles[index]);
            if (gBufferTargetViews[index] == nullptr)
            {
                RVX_CORE_WARN(
                    "OpaquePass: failed to resolve deferred GBuffer target {}",
                    index);
                reportPlannedFailure(routePlan.gpuPacketCount != 0);
                return;
            }
        }
    }

    RHITextureView* colorTargetView =
        m_outputFamily == OpaqueRasterOutputFamily::DeferredGBuffer
            ? gBufferTargetViews[0]
            : context.GetTextureView(m_colorTargetViewHandle);
    const RHIFormat colorTargetFormat = m_colorTargetFormat;
    RHITextureView* depthTargetView = m_depthTargetViewHandle.IsValid()
        ? context.GetTextureView(m_depthTargetViewHandle)
        : nullptr;

    if (!colorTargetView)
    {
        RVX_CORE_WARN("OpaquePass: No color target view set");
        reportPlannedFailure(routePlan.gpuPacketCount != 0);
        return;
    }

    if (m_depthTargetHandle.IsValid() && !depthTargetView)
    {
        RVX_CORE_WARN("OpaquePass: failed to resolve graph-owned depth target view");
        reportPlannedFailure(routePlan.gpuPacketCount != 0);
        return;
    }

    ViewData drawView = view;
    const bool consumesSurfaceLighting =
        m_outputFamily != OpaqueRasterOutputFamily::DeferredGBuffer;
    DirectionalShadowRecordOutput resolvedDirectionalShadow =
        m_directionalShadowInputs;
    if (!consumesSurfaceLighting)
    {
        // GBuffer geometry is identity-bound to the frame lighting plan, but
        // it must not consume CSM/RT/IBL. Those dependencies belong to the
        // DeferredLighting feature after GBuffer production; declaring them
        // here would invert the graph and can form a cycle with future RT
        // producers that consume depth.
        resolvedDirectionalShadow.enabled = false;
        drawView.textureIBLEnabled = 0;
        drawView.rayTracedShadowEnabled = 0;
        drawView.rayTracedShadowMode = RayTracedShadowMode::ComplementRaster;
    }
    if (m_lightingPlan == nullptr || !m_lightingPlan->IsValid() ||
        m_lightingResources == nullptr || !m_lightingResources->IsComplete() ||
        m_lightingResources->lightingPlanIdentity !=
            m_lightingPlan->GetIdentityHash() ||
        !ResolveDirectionalShadowViewData(
            drawView,
            resolvedDirectionalShadow,
            m_recordIdentity,
            m_lightingPlan->GetIdentityHash()))
    {
        RVX_RENDER_WARN("OpaquePass: lighting plan or directional-shadow output is invalid");
        reportPlannedFailure(routePlan.gpuPacketCount != 0);
        return;
    }

    DirectionalShadowFrameResources shadowResources;
    if (consumesSurfaceLighting && m_directionalShadowInputs.enabled)
    {
        if (!m_directionalShadowReadHandle.IsValid() ||
            !m_directionalShadowViewHandle.IsValid())
        {
            RVX_RENDER_WARN("OpaquePass: directional shadow was requested without a graph read");
            reportPlannedFailure(routePlan.gpuPacketCount != 0);
            return;
        }
        shadowResources.shadowMapView =
            context.GetTextureView(m_directionalShadowViewHandle);
        if (shadowResources.shadowMapView == nullptr)
        {
            RVX_RENDER_WARN("OpaquePass: failed to resolve graph-owned directional shadow view");
            reportPlannedFailure(routePlan.gpuPacketCount != 0);
            return;
        }
        shadowResources.enabled = true;
    }
    m_shadowStats.frameShadowReady = shadowResources.enabled;

    RayTracedShadowFrameResources rayTracedShadowResources;
    RHITextureView* rayTracedShadowMaskView = nullptr;
    const bool rayTracedExecutionStateMatches =
        m_rayTracedShadowInputs.executionState != nullptr &&
        m_rayTracedShadowInputs.executionState->identity == m_rayTracedShadowInputs.identity;
    const bool rayTracedExecutionFailed = rayTracedExecutionStateMatches &&
        m_rayTracedShadowInputs.executionState->executionFailed.load(std::memory_order_acquire);
    const bool rayTracedExecutionReady = rayTracedExecutionStateMatches &&
        !rayTracedExecutionFailed &&
        m_rayTracedShadowInputs.executionState->dispatchReady.load(std::memory_order_acquire);
    m_shadowStats.rayTracedExecutionReady = rayTracedExecutionReady;
    m_shadowStats.rayTracedExecutionFailed = rayTracedExecutionFailed;
    if (consumesSurfaceLighting && rayTracedExecutionReady &&
        m_rayTracedShadowMaskReadHandle.IsValid() &&
        m_rayTracedShadowMaskViewHandle.IsValid())
    {
        rayTracedShadowMaskView = context.GetTextureView(
            m_rayTracedShadowMaskViewHandle);
        rayTracedShadowResources.shadowMaskView = rayTracedShadowMaskView;
        rayTracedShadowResources.enabled = rayTracedShadowMaskView != nullptr;
    }
    if (rayTracedExecutionFailed)
    {
        RVX_RENDER_WARN("OpaquePass: Ray-traced shadow producer failed for the current recording; using raster-only shadowing");
    }
    drawView.rayTracedShadowEnabled = rayTracedShadowResources.enabled ? 1 : 0;
    if (consumesSurfaceLighting && rayTracedShadowResources.enabled &&
        m_rayTracedShadowInputs.enabled)
    {
        drawView.rayTracedShadowFilterRadiusPixels =
            std::max(0.0f, m_rayTracedShadowInputs.filterRadiusTexels);
        drawView.rayTracedShadowMode = m_rayTracedShadowInputs.mode;
        if (drawView.rayTracedShadowMode == RayTracedShadowMode::ReplaceRaster)
        {
            DirectionalShadowRecordOutput disabledDirectionalShadow =
                m_directionalShadowInputs;
            disabledDirectionalShadow.enabled = false;
            if (!ResolveDirectionalShadowViewData(
                    drawView,
                    disabledDirectionalShadow,
                    m_recordIdentity,
                    m_lightingPlan->GetIdentityHash()))
            {
                reportPlannedFailure(routePlan.gpuPacketCount != 0);
                return;
            }
            shadowResources = {};
        }
    }
    m_shadowStats.rayTracedFrameMaskReady = rayTracedShadowResources.enabled;

    if (consumesSurfaceLighting)
    {
        const FrozenFrameLightResources& frozen =
            m_lightingResources->frozenLightResources;
        const bool graphBuffersMatchDescriptors =
            context.GetBuffer(m_lightConstantsReadHandle) ==
                frozen.lightConstantsBuffer.Get() &&
            context.GetBuffer(m_pointLightsReadHandle) ==
                frozen.pointLightsBuffer.Get() &&
            context.GetBuffer(m_spotLightsReadHandle) ==
                frozen.spotLightsBuffer.Get() &&
            context.GetBuffer(m_clusterConstantsReadHandle) ==
                frozen.clusterConstantsBuffer.Get() &&
            context.GetBuffer(m_clusterDataReadHandle) ==
                frozen.clusterBuffer.Get() &&
            context.GetBuffer(m_clusterLightIndicesReadHandle) ==
                frozen.clusterLightIndexBuffer.Get();
        if (!graphBuffersMatchDescriptors)
        {
            RVX_RENDER_WARN(
                "OpaquePass: graph light reads do not match frozen descriptor allocations");
            reportPlannedFailure(routePlan.gpuPacketCount != 0);
            return;
        }
        if (!m_exposureStateReadHandle.IsValid() ||
            context.GetBuffer(m_exposureStateReadHandle) == nullptr)
        {
            RVX_RENDER_WARN(
                "OpaquePass: Forward HDR rendering requires an exact committed exposure-state graph read");
            reportPlannedFailure(routePlan.gpuPacketCount != 0);
            return;
        }
    }

    const bool consumesAmbientOcclusion = consumesSurfaceLighting &&
        m_surfaceRoute == OpaqueRasterSurfaceRoute::AllOpaque;
    RHITextureView* ambientOcclusionView = nullptr;
    if (consumesAmbientOcclusion)
    {
        if (!m_ambientOcclusionInput.IsCompatibleWith(m_recordIdentity) ||
            !m_ambientOcclusionReadHandle.IsValid() ||
            !m_ambientOcclusionViewHandle.IsValid())
        {
            RVX_RENDER_WARN(
                "OpaquePass: Forward opaque lighting requires an exact current GTAO visibility field");
            reportPlannedFailure(routePlan.gpuPacketCount != 0);
            return;
        }

        RHITexture* const ambientOcclusionTexture =
            context.GetTexture(m_ambientOcclusionReadHandle);
        ambientOcclusionView = context.GetTextureView(
            m_ambientOcclusionViewHandle);
        if (ambientOcclusionTexture == nullptr || ambientOcclusionView == nullptr ||
            ambientOcclusionView->GetTexture() != ambientOcclusionTexture ||
            ambientOcclusionView->GetFormat() != RHIFormat::R8_UNORM ||
            ambientOcclusionTexture->GetWidth() !=
                m_ambientOcclusionInput.extent.width ||
            ambientOcclusionTexture->GetHeight() !=
                m_ambientOcclusionInput.extent.height)
        {
            RVX_RENDER_WARN(
                "OpaquePass: Forward opaque GTAO graph view does not match its exact R8 field contract");
            reportPlannedFailure(routePlan.gpuPacketCount != 0);
            return;
        }
    }

    EnvironmentIBLFrameResources environmentIBLResources;
    bool exactIBLViews = !consumesSurfaceLighting ||
        !m_lightingResources->textureIBLEnabled;
    if (consumesSurfaceLighting &&
        m_lightingResources->textureIBLEnabled)
    {
        exactIBLViews = m_lightingResources->ResolveEnvironmentResources(
            context.GetTextureView(m_environmentIBLIrradianceViewHandle),
            context.GetTextureView(m_environmentIBLPrefilteredViewHandle),
            context.GetTextureView(m_environmentIBLBRDFLUTViewHandle),
            environmentIBLResources);
        if (!exactIBLViews)
        {
            RVX_RENDER_WARN(
                "OpaquePass: graph IBL reads do not match the retained LightingFramePlan allocations");
            reportPlannedFailure(routePlan.gpuPacketCount != 0);
            return;
        }
    }

    LightingBindingSnapshotRequest lightingRequest;
    lightingRequest.lightingPlanIdentity = m_lightingPlan->GetIdentityHash();
    lightingRequest.frozenLightResources =
        m_lightingResources->frozenLightResources;
    lightingRequest.directionalShadow = shadowResources;
    lightingRequest.rayTracedShadow = rayTracedShadowResources;
    lightingRequest.environmentIBL = consumesSurfaceLighting
        ? environmentIBLResources
        : EnvironmentIBLFrameResources{};
    if (consumesSurfaceLighting)
    {
        lightingRequest.exposureStateBuffer = RHIBufferRef(
            context.GetBuffer(m_exposureStateReadHandle));
    }
    if (consumesAmbientOcclusion)
    {
        lightingRequest.ambientOcclusionView =
            RHITextureViewRef(ambientOcclusionView);
        lightingRequest.ambientOcclusionFieldHash =
            m_ambientOcclusionInput.fieldContentHash;
    }
    m_lightingBindingSnapshot = std::make_shared<LightingBindingSnapshot>();
    if (!m_pipelineCache->CreateLightingBindingSnapshot(
            drawView, lightingRequest, *m_lightingBindingSnapshot) ||
        !m_lightingBindingSnapshot->MatchesLightingPlan(
            m_lightingPlan->GetIdentityHash()) ||
        !RetainLightingBindingSnapshot(context, *m_lightingBindingSnapshot))
    {
        RVX_RENDER_WARN("OpaquePass: could not create record-owned lighting bindings");
        m_lightingBindingSnapshot.reset();
        reportPlannedFailure(routePlan.gpuPacketCount != 0);
        return;
    }
    const RHIDescriptorSetRef frameDescriptorSet =
        m_lightingBindingSnapshot->frameDescriptorSet;

    // A sealed plan owns lane selection. Preflight the full Direct lane before
    // recording anything, and never replay Direct after a GPU-lane failure.
    {
        const uint32 plannedGPUCount = routePlan.gpuPacketCount;
        const uint32 plannedDirectCount = routePlan.directPacketCount;
        const uint32 plannedInputCount = plannedGPUCount + plannedDirectCount +
            routePlan.skippedPacketCount;
        const bool plannedGPU = plannedGPUCount != 0;
        const bool plannedDirect = plannedDirectCount != 0;
        if (plannedGPU && !ValidatePlannedGPUDrivenPacketRange(
                              *view.renderFrameExecutionPlan,
                              RenderPassKind::Opaque,
                              view.meshPassPreparation->opaque))
        {
            m_drawStats.failureReason = RenderPolicyReason::InconsistentFacts;
            updatePlanReport(RenderExecutionStatus::Failed,
                             m_drawStats.failureReason,
                             0,
                             true,
                             opaquePlan->visibility);
            return;
        }

        const bool validateDirectPlan = plannedDirect || !plannedGPU;
        const bool executeDirectLane = plannedDirect ||
            (!plannedGPU && plannedInputCount == 0);
        std::vector<PlannedOpaqueDraw> plannedDraws;
        if (validateDirectPlan &&
            !BuildPlannedDirectBatch(context,
                                     view,
                                     colorTargetFormat,
                                     frameDescriptorSet.Get(),
                                     plannedDraws))
        {
            updatePlanReport(RenderExecutionStatus::Failed,
                             m_drawStats.failureReason,
                             0,
                             false,
                             opaquePlan->visibility);
            return;
        }
        // The stream was deliberately materialized with placeholder semantic
        // rows. Now that this same preflight has produced the exact actual
        // descriptor/table outcomes, finalize only its CPU sidecar. Failure
        // leaves transcript evidence unavailable and never changes drawing.
        const bool directRasterSemanticEvidenceFinalized = executeDirectLane &&
            FinalizeDirectRasterSemanticEvidence(plannedDraws);

        if (m_directReadbackQualification != nullptr &&
            m_directReadbackQualification->IsArmed())
        {
            const DirectRasterReadbackRecordingIdentity qualificationIdentity{
                m_recordIdentity.graphIdentity,
                m_recordIdentity.graphRecordingGeneration,
                m_recordIdentity.frameSequence,
                m_recordIdentity.recordEpoch,
                m_directReadbackSourceFrameSlot};
            if (!executeDirectLane || plannedDraws.empty())
            {
                m_directReadbackQualification->RejectNoDirectLane(
                    qualificationIdentity);
            }
            else
            {
                // A failed semantic sidecar is a hard qualification gate.  The
                // default unavailable digest consumes this one-shot request in
                // Prepare without creating readback buffers or recording copies.
                RasterTranscriptDigest reference;
                if (directRasterSemanticEvidenceFinalized)
                {
                    static_cast<void>(BuildCompleteDirectRasterTranscript(
                        plannedDraws, reference));
                }
                std::vector<DirectRasterReadbackDraw> qualificationDraws;
                try
                {
                    qualificationDraws.reserve(plannedDraws.size());
                    for (const PlannedOpaqueDraw& planned : plannedDraws)
                    {
                        qualificationDraws.push_back({
                            MakeRenderInstanceBatchKey(
                                planned.packet.packet, planned.packet.layout),
                            planned.packet.packet.arguments,
                            planned.representedPacketCount,
                            planned.instanced});
                    }
                }
                catch (...)
                {
                    qualificationDraws.clear();
                }
                if (IRHIDevice* device = m_pipelineCache->GetDevice())
                {
                    static_cast<void>(m_directReadbackQualification->Prepare(
                        *device,
                        qualificationIdentity,
                        m_directInstanceStream,
                        *m_directInstanceStreamCache,
                        qualificationDraws,
                        reference));
                }
            }
        }

        // Tier1 and Tier2 group resources are fully materialized before an
        // attachment is mutated.  The recording function below only emits
        // commands from these immutable bindings.
        ObjectConstantBinding tier1ObjectBinding;
        std::vector<PlannedGPUDrivenOpaqueDraw> plannedGPUBatches;
        bool gpuPreflightReady = plannedGPU;
        if (plannedGPU)
        {
            const bool usesGPUSceneRaster = m_gpuSceneRasterEnabled;
            uint32 gpuDrawItemCount = 0;
            if (!m_gpuCulling || !m_materialSystem || !m_pipelineCache ||
                m_gpuMaterialTablePreflightFailed ||
                (!usesGPUSceneRaster && !m_gpuCulling->GetInstanceBuffer()) ||
                !m_gpuCulling->GetVisibleInstanceBuffer() ||
                !AreGPUDrivenOpaqueGroupsDrawable(
                    plannedGPUCount,
                     0,
                     gpuDrawItemCount))
            {
                gpuPreflightReady = false;
            }
            if (gpuPreflightReady && !usesGPUSceneRaster)
            {
                if (!m_pipelineCache->CreateObjectConstantBinding(
                        Mat4Identity(),
                        Mat4Identity(),
                        Mat4Identity(),
                        drawView.previousViewProjectionMatrix,
                        false,
                        true,
                        {},
                        {},
                        m_gpuCulling->GetInstanceBuffer(),
                        tier1ObjectBinding) ||
                    !context.RetainSubmissionResource(
                        Ref<RefCounted>(tier1ObjectBinding.constantBuffer)) ||
                    (tier1ObjectBinding.instanceBuffer &&
                     !context.RetainSubmissionResource(
                         Ref<RefCounted>(tier1ObjectBinding.instanceBuffer))) ||
                    !context.RetainSubmissionResource(
                        Ref<RefCounted>(tier1ObjectBinding.descriptorSet)))
                {
                    gpuPreflightReady = false;
                }
            }
            if (gpuPreflightReady)
            {
                const auto& groups = m_gpuCulling->GetDrawGroups();
                const OpaqueRasterPipelineSelector pipelineSelector(*m_pipelineCache);
                plannedGPUBatches.reserve(groups.size());
                for (uint32 groupIndex = 0;
                     groupIndex < static_cast<uint32>(groups.size());
                     ++groupIndex)
                {
                    const GPUCullingDrawGroup& group = groups[groupIndex];
                    if (m_surfaceRoute != OpaqueRasterSurfaceRoute::AllOpaque &&
                        !IsOpaqueSurfaceEligibility(
                            group.batchKey.pipeline.surfacePathEligibility))
                    {
                        gpuPreflightReady = false;
                        break;
                    }
                    if (m_surfaceRoute != OpaqueRasterSurfaceRoute::AllOpaque &&
                        !IsSurfaceEligibilitySelectedForRoute(
                            group.batchKey.pipeline.surfacePathEligibility,
                            m_surfaceRoute))
                    {
                        continue;
                    }
                    const MeshGPUBuffers buffers = ResolveRenderMeshBuffers(
                        m_resourceRegistry, group.mesh);
                    const OpaqueRasterSubmissionRoute route = usesGPUSceneRaster
                        ? OpaqueRasterSubmissionRoute::GPUScene
                        : OpaqueRasterSubmissionRoute::GPUDriven;
                    OpaqueRasterPreparedSubmission rasterSubmission =
                        pipelineSelector.Prepare(MakeOpaquePipelineRequest(
                            m_outputFamily,
                            m_surfaceRoute,
                            route,
                            group.pipelineVariant,
                            colorTargetFormat,
                            group.batchKey.pipeline.cullMode,
                            group.batchKey.pipeline.reverseWinding,
                            false,
                            group.batchKey.usesMaterialParameterTable));
                    MaterialBindingOptions options;
                    options.allowNormalMap = buffers.HasNormalMapTangentBasis();
                    options.materialParameterTable =
                        m_gpuMaterialParameterTable.Get();
                    MaterialBindingResult binding = m_materialSystem->PrepareMaterialBinding(
                        group.material, nullptr, options);
                    if (!buffers.IsValid() || !buffers.uvBuffer ||
                        (group.batchKey.pipeline.requiresUV1 &&
                         !buffers.HasTextureCoordinateSet(1)) ||
                        !rasterSubmission.IsReady() || !binding.IsDrawable() ||
                        !context.RetainSubmissionResource(
                            Ref<RefCounted>(rasterSubmission.GetPipeline())) ||
                        !context.RetainSubmissionResource(
                            Ref<RefCounted>(binding.constantBuffer)) ||
                        !context.RetainSubmissionResource(
                            Ref<RefCounted>(binding.descriptorSetRef)))
                    {
                        gpuPreflightReady = false;
                        break;
                    }
                    ++m_drawStats.materialBindingCount;
                    if (binding.usedFallback)
                    {
                        ++m_drawStats.materialFallbackBindingCount;
                    }
                    m_drawStats.materialTextureFlags |= binding.textureFlags;
                    m_drawStats.materialFallbackTextureFlags |=
                        binding.fallbackTextureFlags;
                    PlannedGPUDrivenOpaqueDraw planned;
                    planned.groupIndex = groupIndex;
                    planned.buffers = buffers;
                    planned.rasterSubmission = std::move(rasterSubmission);
                    planned.materialBinding = std::move(binding);
                    plannedGPUBatches.emplace_back(std::move(planned));
                }
            }
            if (gpuPreflightReady &&
                m_surfaceRoute == OpaqueRasterSurfaceRoute::AllOpaque &&
                !usesGPUSceneRaster &&
                m_gpuCullingRecordedState != nullptr &&
                m_resourceRegistry != nullptr)
            {
                std::vector<uint64> fixedRasterMaterialKeysByGroup;
                try
                {
                    fixedRasterMaterialKeysByGroup.assign(
                        m_gpuCulling->GetDrawGroups().size(), 0);
                    for (const PlannedGPUDrivenOpaqueDraw& planned :
                         plannedGPUBatches)
                    {
                        if (planned.groupIndex >=
                            fixedRasterMaterialKeysByGroup.size())
                        {
                            fixedRasterMaterialKeysByGroup.clear();
                            break;
                        }
                        const GPUCullingDrawGroup& group =
                            m_gpuCulling->GetDrawGroups()[planned.groupIndex];
                        if (!group.batchKey.usesMaterialParameterTable)
                        {
                            fixedRasterMaterialKeysByGroup[planned.groupIndex] =
                                planned.materialBinding.descriptorContentKey;
                        }
                    }
                    if (fixedRasterMaterialKeysByGroup.size() ==
                        m_gpuCulling->GetDrawGroups().size())
                    {
                        static_cast<void>(
                            m_gpuCullingRecordedState
                                ->FinalizeTierOneRasterSemanticEvidence(
                                    fixedRasterMaterialKeysByGroup,
                                    m_gpuRasterMaterialSemanticKeysBySlot,
                                    *m_resourceRegistry));
                    }
                }
                catch (...)
                {
                    // Qualification evidence remains fail-closed. The
                    // already valid Tier1 raster submission must continue.
                }
            }
        }

        if (plannedGPU && !gpuPreflightReady && !executeDirectLane)
        {
            m_drawStats.failureReason = RenderPolicyReason::UnexpectedRecordingFailure;
            updatePlanReport(RenderExecutionStatus::Failed,
                             m_drawStats.failureReason,
                             0,
                             true,
                             opaquePlan->visibility);
            return;
        }

        m_drawStats.planValidated = true;
        m_drawStats.plannedPacketCount =
            plannedGPUCount + plannedDirectCount;
        m_drawStats.compiledPacketCount = plannedGPUCount +
            m_drawStats.compiledPacketCount;

        const RenderViewClearActions clearActions =
            ResolveRenderViewClearActions(drawView.clearPolicy,
                                          drawView.clearColor);
        RHIRenderPassDesc rpDesc;
        if (m_outputFamily == OpaqueRasterOutputFamily::DeferredGBuffer)
        {
            constexpr std::array<RHIClearColor, 4> clearValues = {{
                {0.0f, 0.0f, 0.0f, 1.0f},
                {0.5f, 0.5f, 1.0f, 0.0f},
                {0.0f, 0.0f, 0.0f, 0.0f},
                {0.0f, 0.0f, 0.0f, 0.0f}}};
            for (uint32 index = 0; index < gBufferTargetViews.size(); ++index)
            {
                rpDesc.AddColorAttachment(
                    gBufferTargetViews[index],
                    RHILoadOp::Clear,
                    RHIStoreOp::Store,
                    clearValues[index]);
            }
        }
        else
        {
            // Deferred ForwardSpecialOpaque is a continuation of the frame's
            // already-lit HDR/depth surfaces.  Reusing the camera clear
            // policy here would erase DeferredLighting and the opaque depth
            // before the special surfaces are recorded.  The regular
            // ForwardStandard pass remains the owner of camera clear policy.
            const bool preserveDeferredSurfaces =
                m_surfaceRoute ==
                OpaqueRasterSurfaceRoute::ForwardSpecialOpaque;
            rpDesc.AddColorAttachment(colorTargetView,
                                      preserveDeferredSurfaces
                                          ? RHILoadOp::Load
                                          : clearActions.colorLoadOp,
                                      RHIStoreOp::Store,
                                      clearActions.colorClearValue);
        }
        if (depthTargetView)
        {
            const bool preserveDeferredDepth =
                m_surfaceRoute ==
                OpaqueRasterSurfaceRoute::ForwardSpecialOpaque;
            rpDesc.SetDepthStencil(depthTargetView,
                                   preserveDeferredDepth
                                       ? RHILoadOp::Load
                                       : clearActions.depthLoadOp,
                                   RHIStoreOp::Store,
                                   m_pipelineCache->GetDepthClearValue(),
                                   0);
        }
        ctx.BeginRenderPass(rpDesc);
        ctx.SetViewport(drawView.GetRHIViewport());
        ctx.SetScissor(drawView.GetRHIScissor());

        bool gpuRecorded = true;
        bool directRecorded = true;
        if (plannedGPU)
        {
            const auto laneStart = std::chrono::steady_clock::now();
            gpuRecorded = gpuPreflightReady && TryDrawGPUDrivenIndirect(
                ctx,
                frameDescriptorSet.Get(),
                m_gpuSceneRasterEnabled ? nullptr : &tier1ObjectBinding,
                plannedGPUBatches,
                results,
                plannedGPUCount,
                0);
            if (!gpuRecorded && m_gpuSceneRasterEnabled &&
                m_gpuSceneRecordingFailure)
            {
                m_gpuSceneRecordingFailure->store(true);
            }
            gpuLaneSubmissionCpuNanoseconds = static_cast<uint64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - laneStart).count());
            gpuLaneSubmissionTimingAvailable = true;
        }
        if (executeDirectLane)
        {
            const auto laneStart = std::chrono::steady_clock::now();
            directRecorded = TryDrawPlannedDirect(
                ctx, plannedDraws, results);
            directLaneSubmissionCpuNanoseconds = static_cast<uint64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - laneStart).count());
            directLaneSubmissionTimingAvailable = true;
            // The Direct transcript follows exactly the same compressed
            // planned-draw sequence submitted above: regular DrawIndexed
            // entries stay in place and instanced draws expand through their
            // history-preserving slot-6 resident index stream.
            if (directRecorded)
            {
                static_cast<void>(BuildCompleteDirectRasterTranscript(
                    plannedDraws, m_drawStats.directRasterTranscript));
            }
        }
        ctx.EndRenderPass();

        if (directRecorded && m_directReadbackQualification != nullptr)
        {
            const DirectRasterReadbackRecordingIdentity qualificationIdentity{
                m_recordIdentity.graphIdentity,
                m_recordIdentity.graphRecordingGeneration,
                m_recordIdentity.frameSequence,
                m_recordIdentity.recordEpoch,
                m_directReadbackSourceFrameSlot};
            static_cast<void>(
                m_directReadbackQualification->RecordPostRenderCopy(
                    ctx, qualificationIdentity));
        }

        const bool passRecorded = gpuRecorded && directRecorded;
        if (!passRecorded && m_gpuSceneRasterEnabled &&
            m_gpuSceneRecordingFailure)
        {
            m_gpuSceneRecordingFailure->store(true);
        }
        m_drawStats.failureReason = passRecorded
            ? RenderPolicyReason::None
            : RenderPolicyReason::UnexpectedRecordingFailure;

        if (passRecorded && consumesSurfaceLighting && results != nullptr)
        {
            const uint64 lightingWorkCount =
                static_cast<uint64>(m_drawStats.executedPacketCount) +
                static_cast<uint64>(
                    m_drawStats.gpuDrivenIndirectDrawCount);
            if (lightingWorkCount != 0)
            {
                RenderLightingSurfaceExecutionEvidence evidence;
                evidence.lightingPlanIdentity =
                    m_lightingPlan->GetIdentityHash();
                evidence.route = m_surfaceRoute ==
                        OpaqueRasterSurfaceRoute::ForwardSpecialOpaque
                    ? RenderLightingSurfaceRoute::ForwardSpecialOpaque
                    : RenderLightingSurfaceRoute::ForwardOpaque;
                evidence.frozenBufferMask = 0x3fu;
                evidence.directionalShadowBound = shadowResources.enabled;
                evidence.textureIBLBound =
                    m_lightingResources->textureIBLEnabled;
                evidence.exactIBLViews = exactIBLViews;
                // Only the initial Forward opaque route receives the exact
                // GTAO field. ForwardSpecial and translucency use a neutral
                // descriptor fallback and must never claim consumption.
                evidence.ambientOcclusionFieldContentHash =
                    consumesAmbientOcclusion
                    ? m_ambientOcclusionInput.fieldContentHash
                    : 0;
                evidence.workCount = lightingWorkCount;
                evidence.executed = true;
                if (!results->RecordLightingSurfaceExecution(evidence))
                {
                    m_drawStats.failureReason =
                        RenderPolicyReason::UnexpectedRecordingFailure;
                }
            }
        }

        if (view.renderFrameExecutionReport != nullptr &&
            m_surfaceRoute == OpaqueRasterSurfaceRoute::AllOpaque)
        {
            for (RenderPassExecutionReport& report :
                 view.renderFrameExecutionReport->passes)
            {
                if (report.pass == RenderPassKind::Opaque)
                {
                    report.status = passRecorded
                        ? RenderExecutionStatus::Completed
                        : RenderExecutionStatus::Failed;
                    report.executedVisibility = opaquePlan->visibility;
                    report.reason = passRecorded
                        ? opaquePlan->reason
                        : m_drawStats.failureReason;
                    report.skippedPacketCount =
                        routePlan.skippedPacketCount;
                    report.materialBindingsAvailable = true;
                    report.materialBindingCount =
                        m_drawStats.materialBindingCount;
                    report.materialFallbackBindingCount =
                        m_drawStats.materialFallbackBindingCount;
                    report.materialTextureFlags =
                        m_drawStats.materialTextureFlags;
                    report.materialFallbackTextureFlags =
                        m_drawStats.materialFallbackTextureFlags;
                    report.gpuDrivenLane.status = plannedGPU
                        ? (gpuRecorded ? RenderExecutionStatus::Completed
                                       : RenderExecutionStatus::Failed)
                        : RenderExecutionStatus::NotAttempted;
                    report.gpuDrivenLane.reason = plannedGPU && !gpuRecorded
                        ? RenderPolicyReason::UnexpectedRecordingFailure
                        : RenderPolicyReason::None;
                    report.gpuDrivenLane.executedCountsAvailable = plannedGPU &&
                        m_drawStats.gpuDrivenIndirectExecutedDrawCountAvailable;
                    // Preserve honest partial-recording telemetry on a late
                    // group failure. The failed lane is not replayed through
                    // Direct in this frame.
                    report.gpuDrivenLane.executedPacketCount =
                        report.gpuDrivenLane.executedCountsAvailable
                        ? m_drawStats.gpuDrivenIndirectDrawCount
                        : 0;
                    report.gpuDrivenLane.executedDrawCount =
                        report.gpuDrivenLane.executedCountsAvailable
                        ? m_drawStats.gpuDrivenIndirectDrawCount
                        : 0;
                    report.gpuDrivenLane.submissionCpuNanoseconds =
                        gpuLaneSubmissionCpuNanoseconds;
                    report.gpuDrivenLane.submissionCpuTimingAvailable =
                        gpuLaneSubmissionTimingAvailable;
                    report.directLane.status = executeDirectLane
                        ? (directRecorded ? RenderExecutionStatus::Completed
                                          : RenderExecutionStatus::Failed)
                        : RenderExecutionStatus::NotAttempted;
                    report.directLane.reason = executeDirectLane && !directRecorded
                        ? RenderPolicyReason::UnexpectedRecordingFailure
                        : RenderPolicyReason::None;
                    report.directLane.executedCountsAvailable =
                        executeDirectLane;
                    report.directLane.executedPacketCount =
                        m_drawStats.executedPacketCount;
                    report.directLane.executedDrawCount =
                        m_drawStats.directDrawCount;
                    report.directLane.submissionCpuNanoseconds =
                        directLaneSubmissionCpuNanoseconds;
                    report.directLane.submissionCpuTimingAvailable =
                        directLaneSubmissionTimingAvailable;
                    if (!passRecorded)
                    {
                        view.renderFrameExecutionReport->status =
                            RenderExecutionStatus::Failed;
                    }
                    break;
                }
            }
            view.renderFrameExecutionReport->frameSequence =
                view.renderFrameExecutionPlan->frameSequence;
        }
        if (passRecorded)
        {
            executionGuard.MarkSucceeded();
        }
        return;
    }

}

} // namespace RVX
