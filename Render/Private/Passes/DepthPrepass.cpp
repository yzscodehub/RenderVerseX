/**
 * @file DepthPrepass.cpp
 * @brief DepthPrepass implementation
 */

#include "Render/Passes/DepthPrepass.h"
#include "Core/Log.h"
#include "Render/GPUDriven/GPUCulling.h"
#include "Render/Graph/ResourceViewCache.h"
#include "Render/Material/MaterialSystem.h"
#include "Render/PipelineCache.h"
#include "Render/Renderer/RenderDrawItem.h"
#include "Render/Renderer/RenderScene.h"
#include "Render/Renderer/ViewData.h"
#include "Render/Submission/RenderSubmissionStrategy.h"
#include "Resources/RenderResourceResolver.h"
#include "Resources/RenderSubmissionResourceBatch.h"
#include "RHI/RHIRenderPass.h"

#include <array>
#include <chrono>
#include <limits>
#include <memory>

namespace RVX
{

namespace
{
    std::span<const Mat4> ResolveSkinningMatrices(const RenderObject& object, const MeshGPUBuffers& buffers)
    {
        if (!object.HasSkinningData() || !buffers.HasSkinningVertexData())
        {
            return {};
        }

        return std::span<const Mat4>(object.skinningMatrices.data(), object.skinningMatrices.size());
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

    RenderSubmissionLayout MakeExpectedDirectLayout(
        const RenderDrawPacket& packet) noexcept
    {
        RenderSubmissionLayout layout;
        layout.vertexStreams = MeshPassVertexStreams::Position;
        layout.bindings = MeshPassBindingRequirements::Frame |
            MeshPassBindingRequirements::Object |
            MeshPassBindingRequirements::Geometry;
        if (IsSkinnedPacket(packet))
        {
            layout.vertexStreams |= MeshPassVertexStreams::BoneIndices;
            layout.vertexStreams |= MeshPassVertexStreams::BoneWeights;
            layout.bindings |= MeshPassBindingRequirements::Skinning;
        }
        if (IsMaskedPacket(packet))
        {
            layout.vertexStreams |= MeshPassVertexStreams::TexCoord;
            const bool missingMaterial =
                HasDrawFlag(packet.flags, RenderDrawFlags::MissingMaterial) ||
                !packet.materialKey.material.IsValid();
            layout.bindings |= missingMaterial
                ? MeshPassBindingRequirements::DefaultMaterial
                : MeshPassBindingRequirements::Material;
        }
        layout.primitiveDataBinding = PrimitiveDataBinding::PerDrawConstants;
        return layout;
    }

    bool ResolveUniqueObject(const RenderScene& scene,
                             RenderObjectId objectId,
                             uint32 primitiveData,
                             RenderObject& outObject) noexcept
    {
        if (objectId == 0 || primitiveData >= scene.GetObjectCount())
        {
            return false;
        }

        const RenderObject& indexedObject = scene.GetObject(primitiveData);
        if (indexedObject.entityId != objectId)
        {
            return false;
        }

        const RenderObject* uniqueObject = nullptr;
        for (uint32 index = 0;
             index < static_cast<uint32>(scene.GetObjectCount());
             ++index)
        {
            const RenderObject& candidate = scene.GetObject(index);
            if (candidate.entityId != objectId)
            {
                continue;
            }
            if (uniqueObject != nullptr)
            {
                return false;
            }
            uniqueObject = &candidate;
        }
        if (uniqueObject == nullptr || uniqueObject != &indexedObject)
        {
            return false;
        }
        outObject = *uniqueObject;
        return true;
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

    void PublishDepthContextFailure(const RenderPassExecutionData& execution,
                                    DepthPrepassDrawStats& stats)
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
            executionPlan, RenderPassKind::Depth);
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
            if (report.pass != RenderPassKind::Depth)
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

} // namespace

struct DepthPrepass::PlannedDepthDraw
{
    DirectDrawPacket packet;
    RenderObject object;
    MeshGPUBuffers buffers;
    SubmeshGPUInfo submesh;
    RHIPipeline* pipeline = nullptr;
    RHIDescriptorSet* frameSet = nullptr;
    ObjectConstantBinding objectBinding;
    MaterialBindingResult materialBinding;
    bool masked = false;
    bool skinned = false;
};

struct DepthPrepass::PlannedGPUDrivenDepthDraw
{
    uint32 groupIndex = 0;
    MeshGPUBuffers buffers;
};

DepthPrepass::DepthPrepass()
{
    // Depth prepass is disabled by default - enable when depth-only pipeline is ready
    SetEnabled(false);
}

void DepthPrepass::SetResources(PipelineCache* pipelineCache)
{
    m_pipelineCache = pipelineCache;
}

void DepthPrepass::InitializeGraphRecorder(
    const RenderScene* scene,
    const std::vector<RenderDrawItem>* opaqueDrawItems,
    const std::vector<RenderDrawItem>* maskedDrawItems,
    const GPUCulling* gpuCulling,
    const RenderPassGPUDrivenInputs& gpuInputs,
    bool gpuDrivenPlanned)
{
    m_renderScene = scene;
    m_opaqueDrawItems = opaqueDrawItems;
    m_maskedDrawItems = maskedDrawItems;
    m_gpuCulling = gpuCulling;
    m_gpuDrivenInstanceHandle = gpuInputs.instances;
    m_gpuSceneCandidateHandle = gpuInputs.gpuSceneCandidates;
    m_gpuScenePrimitiveHandle = gpuInputs.gpuScenePrimitives;
    m_gpuSceneTransformHandle = gpuInputs.gpuSceneTransforms;
    m_gpuDrivenInstanceIndexHandle = gpuInputs.instanceIndices;
    m_gpuDrivenIndirectHandle = gpuInputs.indirectDraws;
    m_gpuDrivenDrawCountHandle = gpuInputs.drawCount;
    m_gpuSceneRasterBinding = gpuInputs.gpuSceneRasterBinding;
    m_gpuSceneRecordingFailure = gpuInputs.gpuSceneRecordingFailure;
    m_gpuDrivenDepthIndirectEnabled = gpuDrivenPlanned;
    m_gpuSceneRasterEnabled = gpuInputs.gpuSceneRasterEnabled;
}

void DepthPrepass::AddToGraph(
    RenderGraph& graph,
    const RenderPassRecordContext& context)
{
    struct GraphPassData
    {
        RenderPassExecutionData execution{};
        RenderPassGPUDrivenInputs gpuInputs{};
        std::unique_ptr<DepthPrepass> recorder;
        bool contextValid = false;
    };

    const auto hasCurrentGraphDepthAttachment = [&graph](
                                                   const ViewData& view,
                                                   const RenderPassRecordIdentity& identity)
    {
        return view.renderGraph == &graph && view.depthTarget.IsValid() &&
               HasCurrentGraphProvenance(view.depthTarget, identity) &&
               graph.GetTextureDesc(view.depthTarget) != nullptr;
    };
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
        context.frameSnapshot->view.renderGraph == &graph &&
        context.frameSnapshot->view.renderFrameExecutionPlan ==
            &context.frameSnapshot->executionPlan &&
        context.frameSnapshot->view.meshPassPreparation ==
            &context.frameSnapshot->meshPassPreparation &&
        context.frameSnapshot->view.renderVisibility ==
            &context.frameSnapshot->visibility &&
        context.frameSnapshot->view.renderFrameExecutionReport ==
            &context.results->executionReport &&
        hasCurrentGraphDepthAttachment(
            context.frameSnapshot->view, context.identity);
    const bool sourceContextValid = !context.legacyAdapter &&
        context.MatchesTargetGraph(graph) && context.IsFrameIdentityValid() &&
        sourcePlanValid && suppliedResultsValid && suppliedSnapshotValid &&
        context.view.renderFrameExecutionPlan == context.executionPlan &&
        context.view.meshPassPreparation == context.meshPassPreparation &&
        context.view.renderFrameExecutionReport == context.executionReport &&
        hasCurrentGraphDepthAttachment(context.view, context.identity);

    RenderPassExecutionData execution;
    if (sourceContextValid)
    {
        execution = MakeRenderPassExecutionData(context);
    }
    else
    {
        execution.view = context.view;
        execution.identity = context.identity;
    }
    const RenderFrameExecutionPlan* executionPlan =
        execution.GetExecutionPlan();
    const bool hasPlan = executionPlan != nullptr;
    const bool gpuPlanned = IsGPUDrivenPassPlanned(
        executionPlan, RenderPassKind::Depth);
    // Invalid sources are true no-ops: do not extend the lifetime of a
    // foreign recording's GPU state merely by registering a rejected pass.
    const RenderPassGPUDrivenInputs gpuInputs = sourceContextValid
        ? context.depthGPUDriven : RenderPassGPUDrivenInputs{};
    const GPUCullingRecordingIdentity gpuRecordingIdentity{
        execution.identity.graphIdentity,
        execution.identity.graphRecordingGeneration,
        execution.identity.frameSequence,
        execution.identity.viewOrdinal,
        execution.identity.recordEpoch};
    const bool contextValid = sourceContextValid && hasPlan &&
        execution.MatchesTargetGraph(graph) &&
        execution.IsFrameIdentityValid() &&
        execution.frameSnapshot != nullptr && execution.results != nullptr &&
        hasCurrentGraphDepthAttachment(execution.view, execution.identity) &&
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

    // Copy every service and frame reference into the graph registration
    // closure.  The callbacks below only read GraphPassData; the originating
    // pass is retained solely as a write-only diagnostics sink.
    PipelineCache* const pipelineCache = m_pipelineCache;
    MaterialSystem* const materialSystem = m_materialSystem;
    const RenderResourceRegistry* const resourceRegistry = m_resourceRegistry;
    const RenderScene* const renderScene = execution.frameSnapshot
        ? &execution.frameSnapshot->scene : nullptr;
    const std::vector<RenderDrawItem>* const opaqueDrawItems =
        execution.frameSnapshot ? &execution.frameSnapshot->opaqueDrawItems : nullptr;
    const std::vector<RenderDrawItem>* const maskedDrawItems =
        execution.frameSnapshot ? &execution.frameSnapshot->maskedDrawItems : nullptr;
    const GPUCulling* const gpuCulling = gpuInputs.recordedState != nullptr
        ? &gpuInputs.recordedState->GetCulling() : nullptr;
    const bool enabled = m_enabled;
    const std::shared_ptr<RenderPassRecordResults> results =
        resultOwnershipValid ? execution.results : nullptr;

    graph.AddPass<GraphPassData>(
        GetName(),
        GetPassType(),
        [execution,
         gpuInputs,
         contextValid,
         pipelineCache,
         materialSystem,
         resourceRegistry,
         renderScene,
         opaqueDrawItems,
         maskedDrawItems,
         gpuCulling,
         gpuPlanned,
         enabled,
         results](RenderGraphBuilder& builder, GraphPassData& data)
        {
            data.execution = execution;
            data.gpuInputs = gpuInputs;
            data.contextValid = contextValid;
            if (!data.contextValid || !results)
            {
                if (results)
                {
                    PublishDepthContextFailure(data.execution, results->depthStats);
                }
                return;
            }

            // The recorder is graph-owned. All frame input is copied from the
            // sealed typed context; persistent pass state is configuration only.
            data.recorder = std::make_unique<DepthPrepass>();
            data.recorder->SetResources(pipelineCache);
            data.recorder->SetMaterialSystem(materialSystem);
            data.recorder->SetResourceRegistry(resourceRegistry);
            data.recorder->SetEnabled(enabled);
            data.recorder->InitializeGraphRecorder(
                renderScene, opaqueDrawItems, maskedDrawItems, gpuCulling,
                data.gpuInputs, gpuPlanned);
            data.recorder->Setup(builder, data.execution.view);
        },
        [results](const GraphPassData& data, RHICommandContext& ctx)
        {
            if (!data.contextValid || !data.recorder || !results)
            {
                if (results)
                {
                    PublishDepthContextFailure(data.execution, results->depthStats);
                }
                return;
            }
            data.recorder->Execute(ctx, data.execution.view);
            results->depthStats = data.recorder->GetDrawStats();
        });
}

bool DepthPrepass::IsSupported() const
{
    return m_pipelineCache && m_pipelineCache->IsInitialized() &&
        m_pipelineCache->GetDepthOnlyPipeline() &&
        m_pipelineCache->GetMaskedDepthOnlyPipeline();
}

void DepthPrepass::Setup(RenderGraphBuilder& builder, const ViewData& view)
{
    m_depthTargetHandle = {};

    if (!IsEnabled())
        return;

    // Declare depth buffer write (no color output)
    if (view.depthTarget.IsValid())
    {
        builder.SetDepthStencil(view.depthTarget, true, false);
        m_depthTargetHandle = view.depthTarget;
    }

    if (m_gpuDrivenDepthIndirectEnabled && m_gpuCulling)
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
    }
}

bool DepthPrepass::AreGPUDrivenDepthGroupsDrawable(
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
    if (expectedGroupCount != 0 && groups.size() != expectedGroupCount)
    {
        return false;
    }

    uint32 sourceDrawItemCount = 0;
    const auto countDrawItems = [&sourceDrawItemCount](const std::vector<RenderDrawItem>* drawItems)
    {
        sourceDrawItemCount += drawItems ? static_cast<uint32>(drawItems->size()) : 0;
    };
    countDrawItems(m_opaqueDrawItems);
    countDrawItems(m_maskedDrawItems);

    for (const GPUCullingDrawGroup& group : groups)
    {
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

    const uint32 requiredPacketCount =
        expectedPacketCount != 0 ? expectedPacketCount : sourceDrawItemCount;
    return outDrawItemCount > 0 &&
        outDrawItemCount == requiredPacketCount &&
        m_gpuCulling->GetInstanceCount() == outDrawItemCount;
}

bool DepthPrepass::TryDrawGPUDrivenIndirect(RHICommandContext& ctx,
                                            RHIDescriptorSet* frameSet,
                                            const ObjectConstantBinding* tier1ObjectBinding,
                                            RHIPipeline* pipeline,
                                            std::span<const PlannedGPUDrivenDepthDraw> plannedBatches,
                                            uint32 expectedPacketCount,
                                            uint32 expectedGroupCount)
{
    const bool usesGPUSceneRaster = m_gpuSceneRasterEnabled;
    m_drawStats.gpuDrivenRequested = m_gpuDrivenDepthIndirectEnabled && m_gpuCulling != nullptr;
    if (!m_gpuDrivenDepthIndirectEnabled ||
        !m_pipelineCache ||
        !m_gpuCulling ||
        !m_gpuCulling->GetInstanceIndexBuffer() ||
        (!usesGPUSceneRaster && !m_gpuCulling->GetInstanceBuffer()) ||
        (usesGPUSceneRaster &&
         (!m_gpuSceneRasterBinding ||
          !m_gpuSceneRasterBinding->IsReadyForBinding() ||
          m_gpuSceneRasterBinding->leaseVersion == 0)) ||
        (!m_gpuCulling->WasGpuExecutionUsedLastCull() && !m_gpuCulling->WasCpuFallbackUsedLastCull()))
    {
        return false;
    }

    const auto& groups = m_gpuCulling->GetDrawGroups();
    if ((expectedGroupCount != 0 && groups.size() != expectedGroupCount) ||
        plannedBatches.size() != groups.size() ||
        (expectedPacketCount != 0 &&
         m_gpuCulling->GetInstanceCount() != expectedPacketCount))
    {
        return false;
    }
    m_drawStats.gpuDrivenEligible = true;

    if (!pipeline)
    {
        return false;
    }

    if (!usesGPUSceneRaster &&
        (tier1ObjectBinding == nullptr || !tier1ObjectBinding->IsValid()))
    {
        return false;
    }

    ctx.SetPipeline(pipeline);

    if (usesGPUSceneRaster && !frameSet)
    {
        return false;
    }
    if (frameSet)
    {
        ctx.SetDescriptorSet(0, frameSet);
    }

    if (usesGPUSceneRaster)
    {
        static constexpr std::array<uint32, 1> gpuSceneObjectOffsets{0u};
        ctx.SetDescriptorSet(1,
                             m_gpuSceneRasterBinding->objectDescriptorSet.Get(),
                             gpuSceneObjectOffsets);
    }
    else if (tier1ObjectBinding != nullptr)
    {
        ctx.SetDescriptorSet(1,
                             tier1ObjectBinding->descriptorSet.Get(),
                             tier1ObjectBinding->dynamicOffsets);
    }

    m_drawStats.gpuDrivenIndirectExecutedDrawCountAvailable =
        m_gpuCulling->WasCpuFallbackUsedLastCull();
    bool submittedAnyGroup = false;
    for (const PlannedGPUDrivenDepthDraw& batch : plannedBatches)
    {
        ctx.SetVertexBuffer(0, batch.buffers.positionBuffer);
        ctx.SetVertexBuffer(6, m_gpuCulling->GetInstanceIndexBuffer());
        if (batch.buffers.boneIndicesBuffer)
        {
            ctx.SetVertexBuffer(4, batch.buffers.boneIndicesBuffer);
        }
        if (batch.buffers.boneWeightsBuffer)
        {
            ctx.SetVertexBuffer(5, batch.buffers.boneWeightsBuffer);
        }
        ctx.SetIndexBuffer(batch.buffers.indexBuffer, RHIFormat::R32_UINT);

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
            return false;
        }
        if (submission.recorded)
        {
            submittedAnyGroup = true;
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

    // A CPU fallback may correctly compact every planned candidate away. The
    // GPU lane was recorded successfully even though it submitted no draws.
    return submittedAnyGroup ||
        (m_gpuCulling->WasCpuFallbackUsedLastCull() &&
         m_gpuCulling->GetDrawCount() == 0);
}

bool DepthPrepass::BuildPlannedDirectBatch(
    const ViewData& view,
    std::vector<PlannedDepthDraw>& outPlannedDraws)
{
    outPlannedDraws.clear();
    m_drawStats.planRequested = true;
    m_drawStats.failureReason = RenderPolicyReason::InconsistentFacts;
    if (view.renderFrameExecutionPlan == nullptr ||
        view.meshPassPreparation == nullptr ||
        m_renderScene == nullptr || m_resourceRegistry == nullptr ||
        m_pipelineCache == nullptr || m_materialSystem == nullptr)
    {
        m_drawStats.failureReason = RenderPolicyReason::UnexpectedRecordingFailure;
        return false;
    }

    const DirectDrawPacketBatchBuildResult built =
        BuildDirectDrawPacketBatch(*view.renderFrameExecutionPlan,
                                   RenderPassKind::Depth,
                                   view.meshPassPreparation->depth,
                                   view.renderVisibility);
    if (!built.succeeded)
    {
        m_drawStats.failureReason = RenderPolicyReason::InconsistentFacts;
        return false;
    }

    m_drawStats.planValidated = true;
    uint32 plannedPacketCount = 0;
    for (const RenderPassExecutionPlan& passPlan :
         view.renderFrameExecutionPlan->passes)
    {
        if (passPlan.pass == RenderPassKind::Depth)
        {
            plannedPacketCount = passPlan.directPackets.count;
            break;
        }
    }
    m_drawStats.plannedPacketCount = plannedPacketCount;
    m_drawStats.compiledPacketCount =
        built.batch.packets.size() > std::numeric_limits<uint32>::max()
            ? 0
            : static_cast<uint32>(built.batch.packets.size());
    outPlannedDraws.reserve(built.batch.packets.size());
    for (const DirectDrawPacket& draw : built.batch.packets)
    {
        const RenderDrawPacket& packet = draw.packet;
        if (packet.pass != RenderPassKind::Depth ||
            packet.primitiveData == RVX_INVALID_PRIMITIVE_DATA_INDEX ||
            !packet.geometryKey.mesh.IsValid() ||
            packet.geometryKey.indexType != MeshUploadIndexType::UInt32 ||
            packet.pipelineKey.topology != MeshUploadPrimitiveTopology::Triangles ||
            packet.arguments.indexCount == 0 ||
            packet.arguments.instanceCount != 1 ||
            packet.arguments.firstInstance != 0)
        {
            m_drawStats.failureReason = RenderPolicyReason::InconsistentFacts;
            outPlannedDraws.clear();
            return false;
        }

        RenderObject object;
        if (!ResolveUniqueObject(*m_renderScene,
                                 packet.objectId,
                                 packet.primitiveData,
                                 object) ||
            object.mesh != packet.geometryKey.mesh)
        {
            m_drawStats.failureReason = RenderPolicyReason::InconsistentFacts;
            outPlannedDraws.clear();
            return false;
        }

        const RenderSubmissionLayout expectedLayout =
            MakeExpectedDirectLayout(packet);
        if (draw.layout != expectedLayout ||
            draw.layout.primitiveDataBinding != PrimitiveDataBinding::PerDrawConstants ||
            (static_cast<uint32>(draw.layout.vertexStreams) &
             static_cast<uint32>(MeshPassVertexStreams::InstanceIndex)) != 0)
        {
            m_drawStats.failureReason = RenderPolicyReason::InconsistentFacts;
            outPlannedDraws.clear();
            return false;
        }

        const bool masked = IsMaskedPacket(packet);
        const bool skinned = IsSkinnedPacket(packet);
        const bool missingMaterial =
            HasDrawFlag(packet.flags, RenderDrawFlags::MissingMaterial) ||
            !packet.materialKey.material.IsValid();
        if ((masked && !HasDrawFlag(packet.flags, RenderDrawFlags::Masked)) ||
            (!masked && HasDrawFlag(packet.flags, RenderDrawFlags::Masked)) ||
            packet.pipelineKey.skinned != skinned ||
            skinned != object.HasSkinningData() ||
            (masked && packet.materialKey.materialMode != MaterialRenderMode::Masked) ||
            (!masked && packet.materialKey.materialMode != MaterialRenderMode::Opaque) ||
            missingMaterial != HasDrawFlag(packet.flags, RenderDrawFlags::MissingMaterial))
        {
            m_drawStats.failureReason = RenderPolicyReason::InconsistentFacts;
            outPlannedDraws.clear();
            return false;
        }

        const MeshGPUBuffers buffers = ResolveRenderMeshBuffers(
            m_resourceRegistry, packet.geometryKey.mesh);
        if (!buffers.IsValid() ||
            buffers.positionBuffer == nullptr || buffers.indexBuffer == nullptr ||
            packet.geometryKey.submeshIndex >= buffers.submeshes.size())
        {
            m_drawStats.failureReason = RenderPolicyReason::UnexpectedRecordingFailure;
            outPlannedDraws.clear();
            return false;
        }
        const SubmeshGPUInfo submesh =
            buffers.submeshes[packet.geometryKey.submeshIndex];
        if (packet.submeshIndex != packet.geometryKey.submeshIndex ||
            packet.arguments.indexCount != submesh.indexCount ||
            packet.arguments.firstIndex != submesh.indexOffset ||
            packet.arguments.vertexOffset != submesh.baseVertex)
        {
            m_drawStats.failureReason = RenderPolicyReason::InconsistentFacts;
            outPlannedDraws.clear();
            return false;
        }
        if (skinned &&
            (!object.HasSkinningData() || !buffers.HasSkinningVertexData()))
        {
            m_drawStats.failureReason = RenderPolicyReason::UnexpectedRecordingFailure;
            outPlannedDraws.clear();
            return false;
        }
        if (masked && (!buffers.uvBuffer || !buffers.hasUVs))
        {
            m_drawStats.failureReason = RenderPolicyReason::UnexpectedRecordingFailure;
            ++m_drawStats.skippedMissingUVCount;
            outPlannedDraws.clear();
            return false;
        }

        const bool usesDefaultMaterial =
            HasMeshPassBindingRequirement(draw.layout.bindings,
                                          MeshPassBindingRequirements::DefaultMaterial);
        const bool usesMaterial =
            HasMeshPassBindingRequirement(draw.layout.bindings,
                                          MeshPassBindingRequirements::Material);
        if (masked && (usesDefaultMaterial == usesMaterial ||
                       usesDefaultMaterial != missingMaterial))
        {
            m_drawStats.failureReason = RenderPolicyReason::InconsistentFacts;
            outPlannedDraws.clear();
            return false;
        }

        RHIPipeline* pipeline = masked
            ? m_pipelineCache->GetMaskedDepthOnlyPipeline()
            : m_pipelineCache->GetDepthOnlyPipeline();
        if (pipeline == nullptr)
        {
            m_drawStats.failureReason = RenderPolicyReason::UnexpectedRecordingFailure;
            outPlannedDraws.clear();
            return false;
        }

        MaterialBindingResult materialBinding;
        if (masked)
        {
            MaterialBindingOptions options;
            options.allowNormalMap = false;
            materialBinding = m_materialSystem->PrepareMaterialBinding(
                packet.materialKey.material, view.viewCache, options);
            if (!materialBinding.IsDrawable())
            {
                m_drawStats.failureReason = RenderPolicyReason::UnexpectedRecordingFailure;
                ++m_drawStats.skippedMaterialBindingCount;
                outPlannedDraws.clear();
                return false;
            }
            if (!RetainRenderSubmissionResource(
                    view.submissionResourceBatch,
                    Ref<RefCounted>(materialBinding.constantBuffer)) ||
                !RetainRenderSubmissionResource(
                    view.submissionResourceBatch,
                    Ref<RefCounted>(materialBinding.descriptorSetRef)))
            {
                m_drawStats.failureReason = RenderPolicyReason::UnexpectedRecordingFailure;
                outPlannedDraws.clear();
                return false;
            }
        }

        PlannedDepthDraw planned;
        planned.packet = draw;
        planned.object = std::move(object);
        planned.buffers = buffers;
        planned.submesh = submesh;
        planned.pipeline = pipeline;
        planned.frameSet = m_pipelineCache->GetFrameDescriptorSet();
        planned.materialBinding = std::move(materialBinding);
        planned.masked = masked;
        planned.skinned = skinned;
        if (planned.frameSet == nullptr)
        {
            m_drawStats.failureReason = RenderPolicyReason::UnexpectedRecordingFailure;
            outPlannedDraws.clear();
            return false;
        }
        if (!m_pipelineCache->CreateObjectConstantBinding(
                planned.object.worldMatrix,
                planned.object.normalMatrix,
                planned.object.previousWorldMatrix,
                view.previousViewProjectionMatrix,
                planned.object.previousWorldMatrixValid != 0 &&
                    view.previousViewProjectionValid != 0 &&
                    !view.resetTemporalHistory,
                true,
                ResolveSkinningMatrices(planned.object, planned.buffers),
                nullptr,
                planned.objectBinding) ||
            !RetainRenderSubmissionResource(
                view.submissionResourceBatch,
                Ref<RefCounted>(planned.objectBinding.constantBuffer)) ||
            (planned.objectBinding.instanceBuffer &&
             !RetainRenderSubmissionResource(
                 view.submissionResourceBatch,
                 Ref<RefCounted>(planned.objectBinding.instanceBuffer))) ||
            !RetainRenderSubmissionResource(
                view.submissionResourceBatch,
                Ref<RefCounted>(planned.objectBinding.descriptorSet)))
        {
            m_drawStats.failureReason = RenderPolicyReason::UnexpectedRecordingFailure;
            outPlannedDraws.clear();
            return false;
        }
        outPlannedDraws.push_back(std::move(planned));
    }

    return true;
}

bool DepthPrepass::TryDrawPlannedDirect(
    RHICommandContext& ctx,
    std::span<const PlannedDepthDraw> plannedDraws)
{
    static const DirectRenderSubmissionStrategy strategy;
    for (const PlannedDepthDraw& planned : plannedDraws)
    {
        ctx.SetPipeline(planned.pipeline);
        ctx.SetDescriptorSet(0, planned.frameSet);
        ctx.SetDescriptorSet(1,
                             planned.objectBinding.descriptorSet.Get(),
                             planned.objectBinding.dynamicOffsets);
        ctx.SetVertexBuffer(0, planned.buffers.positionBuffer);
        if (planned.masked)
        {
            ctx.SetVertexBuffer(2, planned.buffers.uvBuffer);
            ctx.SetDescriptorSet(2,
                                 planned.materialBinding.descriptorSet,
                                 planned.materialBinding.dynamicOffsets);
        }
        if (planned.skinned)
        {
            ctx.SetVertexBuffer(4, planned.buffers.boneIndicesBuffer);
            ctx.SetVertexBuffer(5, planned.buffers.boneWeightsBuffer);
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
            ++m_drawStats.directDrawCount;
            m_drawStats.executedPacketCount += submission.executedDrawCount;
        }
    }
    m_drawStats.directPacketPathUsed = true;
    return true;
}

void DepthPrepass::Execute(RHICommandContext& ctx, const ViewData& view)
{
    m_drawStats = {};
    uint64 gpuLaneSubmissionCpuNanoseconds = 0;
    uint64 directLaneSubmissionCpuNanoseconds = 0;
    bool gpuLaneSubmissionTimingAvailable = false;
    bool directLaneSubmissionTimingAvailable = false;
    const auto reportGPUSceneRecordingFailure = [&]()
    {
        if (m_gpuSceneRasterEnabled && m_gpuSceneRecordingFailure)
        {
            m_gpuSceneRecordingFailure->store(true);
        }
    };

    RHITextureViewRef depthTargetViewOwner;
    RHITextureView* depthTargetView = nullptr;
    const bool depthHandleBelongsToCurrentGraph =
        view.renderGraph != nullptr &&
        m_depthTargetHandle.IsValid() &&
        m_depthTargetHandle.graphIdentity == view.renderGraph->GetGraphIdentity() &&
        m_depthTargetHandle.recordingGeneration ==
            view.renderGraph->GetRecordingGeneration();
    if (depthHandleBelongsToCurrentGraph && view.viewCache)
    {
        if (RHITexture* depthTarget = view.renderGraph->GetTexture(m_depthTargetHandle))
        {
            depthTargetViewOwner = RHITextureViewRef(
                view.viewCache->GetDefaultDSV(depthTarget));
            depthTargetView = depthTargetViewOwner.Get();
        }
    }

    if (!m_pipelineCache || !m_renderScene || !depthTargetView)
    {
        reportGPUSceneRecordingFailure();
        return;
    }

    if (depthTargetView->GetTexture() == nullptr ||
        !RetainRenderSubmissionResource(
            view.submissionResourceBatch, Ref<RefCounted>(depthTargetView)) ||
        !RetainRenderSubmissionResource(
            view.submissionResourceBatch,
            Ref<RefCounted>(depthTargetView->GetTexture())))
    {
        RVX_RENDER_WARN("DepthPrepass: submission ownership rejected graph-owned depth attachment");
        reportGPUSceneRecordingFailure();
        return;
    }

    const auto updatePlanReport = [&](RenderExecutionStatus status,
                                      RenderPolicyReason reason,
                                      uint32 executedPackets,
                                      bool gpuLane,
                                      RenderVisibilityMode visibility)
    {
        if (status == RenderExecutionStatus::Failed)
        {
            reportGPUSceneRecordingFailure();
        }
        if (view.renderFrameExecutionReport == nullptr)
        {
            return;
        }
        for (RenderPassExecutionReport& report :
             view.renderFrameExecutionReport->passes)
        {
            if (report.pass != RenderPassKind::Depth)
            {
                continue;
            }
            report.status = status;
            report.executedVisibility = visibility;
            report.reason = reason;
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

    if (view.renderFrameExecutionPlan != nullptr &&
        view.meshPassPreparation == nullptr)
    {
        m_drawStats.planRequested = true;
        m_drawStats.failureReason = RenderPolicyReason::InconsistentFacts;
        updatePlanReport(RenderExecutionStatus::Failed,
                         m_drawStats.failureReason,
                         0,
                         false,
                         RenderVisibilityMode::Cpu);
        return;
    }

    // A published plan is authoritative.  Build and preflight the entire
    // Direct lane before beginning the render pass, and never replay Direct
    // work when a planned GPU lane fails at recording time.
    if (view.renderFrameExecutionPlan != nullptr &&
        view.meshPassPreparation != nullptr)
    {
        m_drawStats.planRequested = true;
        const RenderPassExecutionPlan* depthPlan = nullptr;
        for (const RenderPassExecutionPlan& candidate :
             view.renderFrameExecutionPlan->passes)
        {
            if (candidate.pass == RenderPassKind::Depth)
            {
                depthPlan = &candidate;
                break;
            }
        }
        if (depthPlan == nullptr)
        {
            m_drawStats.failureReason = RenderPolicyReason::InconsistentFacts;
            updatePlanReport(RenderExecutionStatus::Failed,
                             m_drawStats.failureReason,
                             0,
                             false,
                             RenderVisibilityMode::Cpu);
            return;
        }

        const uint32 plannedGPUCount =
            depthPlan->partition.gpuDrivenPacketCount;
        const uint32 plannedDirectCount =
            depthPlan->partition.directPacketCount;
        const bool plannedGPU = plannedGPUCount != 0;
        const bool plannedDirect = plannedDirectCount != 0;
        if (plannedGPU && !ValidatePlannedGPUDrivenPacketRange(
                              *view.renderFrameExecutionPlan,
                              RenderPassKind::Depth,
                              view.meshPassPreparation->depth))
        {
            m_drawStats.failureReason = RenderPolicyReason::InconsistentFacts;
            updatePlanReport(RenderExecutionStatus::Failed,
                             m_drawStats.failureReason,
                             0,
                             true,
                             depthPlan->visibility);
            return;
        }

        // Preflight the Direct lane before attachment mutation. A Direct-only
        // or all-Skip plan still records an empty pass so the planned clear is
        // deterministic. Hybrid plans preflight both lanes before recording.
        const bool validateDirectPlan = plannedDirect || !plannedGPU;
        const bool executeDirectLane = plannedDirect ||
            (!plannedGPU && depthPlan->partition.inputPacketCount == 0);
        std::vector<PlannedDepthDraw> plannedDraws;
        if (validateDirectPlan &&
            !BuildPlannedDirectBatch(view, plannedDraws))
        {
            updatePlanReport(RenderExecutionStatus::Failed,
                             m_drawStats.failureReason,
                             0,
                             false,
                             depthPlan->visibility);
            return;
        }

        ObjectConstantBinding tier1ObjectBinding;
        RHIPipeline* gpuPipeline = nullptr;
        RHIDescriptorSetRef gpuFrameDescriptorSet;
        std::vector<PlannedGPUDrivenDepthDraw> plannedGPUBatches;
        bool gpuPreflightReady = plannedGPU;
        if (plannedGPU)
        {
            const bool usesGPUSceneRaster = m_gpuSceneRasterEnabled;
            uint32 gpuDrawItemCount = 0;
            if (!m_gpuCulling || !m_pipelineCache ||
                (!usesGPUSceneRaster && !m_gpuCulling->GetInstanceBuffer()) ||
                !m_gpuCulling->GetInstanceIndexBuffer() ||
                !AreGPUDrivenDepthGroupsDrawable(
                    plannedGPUCount,
                    depthPlan->partition.drawGroupCount,
                    gpuDrawItemCount) ||
                (!usesGPUSceneRaster &&
                 m_pipelineCache->GetGPUDrivenDepthOnlyPipeline() == nullptr) ||
                 (usesGPUSceneRaster &&
                  (!m_gpuSceneRasterBinding ||
                   !m_gpuSceneRasterBinding->IsReadyForBinding() ||
                   !m_gpuSceneRasterBinding->depthPipeline)))
            {
                gpuPreflightReady = false;
            }
            if (gpuPreflightReady)
            {
                gpuPipeline = usesGPUSceneRaster
                    ? m_gpuSceneRasterBinding->depthPipeline.Get()
                    : m_pipelineCache->GetGPUDrivenDepthOnlyPipeline();
                gpuFrameDescriptorSet = m_pipelineCache->GetFrameDescriptorSetSnapshot();
                const auto& groups = m_gpuCulling->GetDrawGroups();
                plannedGPUBatches.reserve(groups.size());
                for (uint32 groupIndex = 0;
                     groupIndex < static_cast<uint32>(groups.size());
                     ++groupIndex)
                {
                    const GPUCullingDrawGroup& group = groups[groupIndex];
                    const MeshGPUBuffers buffers = ResolveRenderMeshBuffers(
                        m_resourceRegistry, group.mesh);
                    if (!buffers.IsValid() ||
                        (usesGPUSceneRaster &&
                         group.pipelineVariant != MaterialPipelineVariant::Opaque))
                    {
                        gpuPreflightReady = false;
                        break;
                    }
                    PlannedGPUDrivenDepthDraw planned;
                    planned.groupIndex = groupIndex;
                    planned.buffers = buffers;
                    plannedGPUBatches.emplace_back(std::move(planned));
                }
            }
            if (gpuPreflightReady &&
                (gpuPipeline == nullptr || !gpuFrameDescriptorSet ||
                !RetainRenderSubmissionResource(
                    view.submissionResourceBatch,
                    Ref<RefCounted>(gpuFrameDescriptorSet))))
            {
                gpuPreflightReady = false;
            }
            if (gpuPreflightReady && !usesGPUSceneRaster &&
                (!m_pipelineCache->CreateObjectConstantBinding(
                     Mat4Identity(),
                     Mat4Identity(),
                     Mat4Identity(),
                     view.previousViewProjectionMatrix,
                     false,
                     true,
                     {},
                     m_gpuCulling->GetInstanceBuffer(),
                     tier1ObjectBinding) ||
                 !RetainRenderSubmissionResource(
                     view.submissionResourceBatch,
                     Ref<RefCounted>(tier1ObjectBinding.constantBuffer)) ||
                 (tier1ObjectBinding.instanceBuffer &&
                  !RetainRenderSubmissionResource(
                      view.submissionResourceBatch,
                      Ref<RefCounted>(tier1ObjectBinding.instanceBuffer))) ||
                 !RetainRenderSubmissionResource(
                     view.submissionResourceBatch,
                     Ref<RefCounted>(tier1ObjectBinding.descriptorSet))))
            {
                gpuPreflightReady = false;
            }
        }

        if (plannedGPU && !gpuPreflightReady && !executeDirectLane)
        {
            m_drawStats.failureReason = RenderPolicyReason::UnexpectedRecordingFailure;
            updatePlanReport(RenderExecutionStatus::Failed,
                             m_drawStats.failureReason,
                             0,
                             true,
                             depthPlan->visibility);
            return;
        }

        m_drawStats.planValidated = true;
        m_drawStats.plannedPacketCount =
            plannedGPUCount + plannedDirectCount;
        m_drawStats.compiledPacketCount = plannedGPUCount +
            static_cast<uint32>(plannedDraws.size());

        RHIRenderPassDesc rpDesc;
        rpDesc.SetDepthStencil(depthTargetView,
                               RHILoadOp::Clear,
                               RHIStoreOp::Store,
                               m_pipelineCache->GetDepthClearValue(),
                               0);
        ctx.BeginRenderPass(rpDesc);
        ctx.SetViewport(view.GetRHIViewport());
        ctx.SetScissor(view.GetRHIScissor());

        bool gpuRecorded = true;
        bool directRecorded = true;
        if (plannedGPU)
        {
            const auto laneStart = std::chrono::steady_clock::now();
            gpuRecorded = gpuPreflightReady && TryDrawGPUDrivenIndirect(
                ctx,
                gpuFrameDescriptorSet.Get(),
                m_gpuSceneRasterEnabled ? nullptr : &tier1ObjectBinding,
                gpuPipeline,
                plannedGPUBatches,
                plannedGPUCount,
                depthPlan->partition.drawGroupCount);
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
            directRecorded = TryDrawPlannedDirect(ctx, plannedDraws);
            directLaneSubmissionCpuNanoseconds = static_cast<uint64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - laneStart).count());
            directLaneSubmissionTimingAvailable = true;
        }
        ctx.EndRenderPass();

        const bool passRecorded = gpuRecorded && directRecorded;
        if (!passRecorded)
        {
            reportGPUSceneRecordingFailure();
        }
        m_drawStats.failureReason = passRecorded
            ? RenderPolicyReason::None
            : RenderPolicyReason::UnexpectedRecordingFailure;
        if (view.renderFrameExecutionReport != nullptr)
        {
            for (RenderPassExecutionReport& report :
                 view.renderFrameExecutionReport->passes)
            {
                if (report.pass != RenderPassKind::Depth)
                {
                    continue;
                }
                report.status = passRecorded
                    ? RenderExecutionStatus::Completed
                    : RenderExecutionStatus::Failed;
                report.executedVisibility = depthPlan->visibility;
                report.reason = passRecorded
                    ? depthPlan->reason
                    : m_drawStats.failureReason;
                report.skippedPacketCount =
                    depthPlan->partition.skippedPacketCount;
                report.gpuDrivenLane.status = plannedGPU
                    ? (gpuRecorded ? RenderExecutionStatus::Completed
                                   : RenderExecutionStatus::Failed)
                    : RenderExecutionStatus::NotAttempted;
                report.gpuDrivenLane.reason = plannedGPU && !gpuRecorded
                    ? RenderPolicyReason::UnexpectedRecordingFailure
                    : RenderPolicyReason::None;
                report.gpuDrivenLane.executedCountsAvailable = plannedGPU &&
                    m_drawStats.gpuDrivenIndirectExecutedDrawCountAvailable;
                // A late group failure does not erase commands already
                // recorded by earlier groups. Report the actual submitted
                // prefix while marking the lane failed; never claim zero or
                // replay that prefix through Direct.
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
                report.directLane.executedCountsAvailable = executeDirectLane;
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
            view.renderFrameExecutionReport->frameSequence =
                view.renderFrameExecutionPlan->frameSequence;
        }
        return;
    }

    m_drawStats.planRequested = true;
    m_drawStats.failureReason = RenderPolicyReason::InconsistentFacts;
}

} // namespace RVX
