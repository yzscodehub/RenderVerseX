/**
 * @file DepthPrepass.cpp
 * @brief DepthPrepass implementation
 */

#include "Render/Passes/DepthPrepass.h"
#include "Render/GPUDriven/GPUCulling.h"
#include "Render/Graph/ResourceViewCache.h"
#include "Render/Renderer/ViewData.h"
#include "Render/Renderer/RenderScene.h"
#include "Render/Renderer/RenderDrawItem.h"
#include "Render/Material/MaterialSystem.h"
#include "Resources/RenderResourceResolver.h"
#include "Render/PipelineCache.h"
#include "RHI/RHIRenderPass.h"
#include "Core/Log.h"

#include <limits>

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

} // namespace

struct DepthPrepass::PlannedDepthDraw
{
    DirectDrawPacket packet;
    RenderObject object;
    MeshGPUBuffers buffers;
    SubmeshGPUInfo submesh;
    RHIPipeline* pipeline = nullptr;
    RHIDescriptorSet* frameSet = nullptr;
    RHIDescriptorSet* objectSet = nullptr;
    std::array<uint32, 1> objectDynamicOffsets{};
    MaterialBindingResult materialBinding;
    bool masked = false;
    bool skinned = false;
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

void DepthPrepass::SetRenderScene(const RenderScene* scene,
                                  const std::vector<RenderDrawItem>* opaqueDrawItems,
                                  const std::vector<RenderDrawItem>* maskedDrawItems)
{
    m_renderScene = scene;
    m_opaqueDrawItems = opaqueDrawItems;
    m_maskedDrawItems = maskedDrawItems;
}

void DepthPrepass::SetGPUDrivenCullingSource(const GPUCulling* gpuCulling)
{
    m_gpuCulling = gpuCulling;
}

void DepthPrepass::SetGPUDrivenRenderGraphResources(RGBufferHandle instanceBuffer,
                                                    RGBufferHandle instanceIndexBuffer,
                                                    RGBufferHandle indirectDrawBuffer,
                                                    RGBufferHandle drawCountBuffer)
{
    m_gpuDrivenInstanceHandle = instanceBuffer;
    m_gpuDrivenInstanceIndexHandle = instanceIndexBuffer;
    m_gpuDrivenIndirectHandle = indirectDrawBuffer;
    m_gpuDrivenDrawCountHandle = drawCountBuffer;
}

void DepthPrepass::SetDepthTarget(RHITextureView* depthView)
{
    m_depthTargetView = depthView;
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
        if (m_gpuDrivenInstanceHandle.IsValid())
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
                                            const ViewData& view,
                                            uint32 expectedPacketCount,
                                            uint32 expectedGroupCount)
{
    m_drawStats.gpuDrivenRequested = m_gpuDrivenDepthIndirectEnabled && m_gpuCulling != nullptr;
    if (!m_gpuDrivenDepthIndirectEnabled ||
        !m_pipelineCache ||
        !m_gpuCulling ||
        !m_gpuCulling->GetInstanceBuffer() ||
        !m_gpuCulling->GetInstanceIndexBuffer() ||
        (!m_gpuCulling->WasGpuExecutionUsedLastCull() && !m_gpuCulling->WasCpuFallbackUsedLastCull()))
    {
        return false;
    }

    uint32 drawItemCount = 0;
    if (!AreGPUDrivenDepthGroupsDrawable(expectedPacketCount,
                                         expectedGroupCount,
                                         drawItemCount))
    {
        return false;
    }
    m_drawStats.gpuDrivenEligible = true;

    RHIPipeline* pipeline = m_pipelineCache->GetGPUDrivenDepthOnlyPipeline();
    if (!pipeline)
    {
        return false;
    }

    if (!m_pipelineCache->UpdateObjectConstants(Mat4Identity(),
                                                Mat4Identity(),
                                                Mat4Identity(),
                                                view.previousViewProjectionMatrix,
                                                false))
    {
        return false;
    }
    if (!m_pipelineCache->UpdateObjectInstanceBuffer(m_gpuCulling->GetInstanceBuffer()))
    {
        return false;
    }

    ctx.SetPipeline(pipeline);

    if (RHIDescriptorSet* frameSet = m_pipelineCache->GetFrameDescriptorSet())
    {
        ctx.SetDescriptorSet(0, frameSet);
    }

    if (RHIDescriptorSet* objectSet = m_pipelineCache->GetObjectDescriptorSet())
    {
        const auto objectDynamicOffsets = m_pipelineCache->GetCurrentObjectDynamicOffset();
        ctx.SetDescriptorSet(1, objectSet, objectDynamicOffsets);
    }

    const auto& groups = m_gpuCulling->GetDrawGroups();
    bool submittedAnyGroup = false;
    for (uint32 groupIndex = 0; groupIndex < static_cast<uint32>(groups.size()); ++groupIndex)
    {
        const GPUCullingDrawGroup& group = groups[groupIndex];
        MeshGPUBuffers buffers = ResolveRenderMeshBuffers(
            m_resourceRegistry, group.mesh);
        if (!buffers.IsValid())
        {
            return false;
        }

        ctx.SetVertexBuffer(0, buffers.positionBuffer);
        ctx.SetVertexBuffer(6, m_gpuCulling->GetInstanceIndexBuffer());
        if (buffers.boneIndicesBuffer)
        {
            ctx.SetVertexBuffer(4, buffers.boneIndicesBuffer);
        }
        if (buffers.boneWeightsBuffer)
        {
            ctx.SetVertexBuffer(5, buffers.boneWeightsBuffer);
        }
        ctx.SetIndexBuffer(buffers.indexBuffer, RHIFormat::R32_UINT);

        const uint32 submittedDraws = m_gpuCulling->DrawIndexedIndirectGroup(ctx, groupIndex);
        if (submittedDraws > 0)
        {
            submittedAnyGroup = true;
            ++m_drawStats.gpuDrivenIndirectBatchCount;
            m_drawStats.gpuDrivenIndirectDrawCount += submittedDraws;
        }
    }

    return submittedAnyGroup;
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
                                   view.meshPassPreparation->depth);
    if (!built.succeeded)
    {
        m_drawStats.failureReason = RenderPolicyReason::InconsistentFacts;
        return false;
    }

    m_drawStats.planValidated = true;
    m_drawStats.plannedPacketCount =
        built.batch.packets.size() > std::numeric_limits<uint32>::max()
            ? 0
            : static_cast<uint32>(built.batch.packets.size());
    m_drawStats.compiledPacketCount = m_drawStats.plannedPacketCount;
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
        }

        PlannedDepthDraw planned;
        planned.packet = draw;
        planned.object = std::move(object);
        planned.buffers = buffers;
        planned.submesh = submesh;
        planned.pipeline = pipeline;
        planned.frameSet = m_pipelineCache->GetFrameDescriptorSet();
        planned.objectSet = m_pipelineCache->GetObjectDescriptorSet();
        planned.materialBinding = std::move(materialBinding);
        planned.masked = masked;
        planned.skinned = skinned;
        if (planned.frameSet == nullptr || planned.objectSet == nullptr)
        {
            m_drawStats.failureReason = RenderPolicyReason::UnexpectedRecordingFailure;
            outPlannedDraws.clear();
            return false;
        }
        if (!m_pipelineCache->UpdateObjectConstants(
                planned.object.worldMatrix,
                planned.object.normalMatrix,
                planned.object.previousWorldMatrix,
                view.previousViewProjectionMatrix,
                planned.object.previousWorldMatrixValid != 0 &&
                    view.previousViewProjectionValid != 0 &&
                    !view.resetTemporalHistory,
                ResolveSkinningMatrices(planned.object, planned.buffers)))
        {
            m_drawStats.failureReason = RenderPolicyReason::UnexpectedRecordingFailure;
            outPlannedDraws.clear();
            return false;
        }
        planned.objectDynamicOffsets =
            m_pipelineCache->GetCurrentObjectDynamicOffset();
        outPlannedDraws.push_back(std::move(planned));
    }

    return true;
}

bool DepthPrepass::TryDrawPlannedDirect(
    RHICommandContext& ctx,
    std::span<const PlannedDepthDraw> plannedDraws)
{
    for (const PlannedDepthDraw& planned : plannedDraws)
    {
        ctx.SetPipeline(planned.pipeline);
        ctx.SetDescriptorSet(0, planned.frameSet);
        ctx.SetDescriptorSet(1, planned.objectSet, planned.objectDynamicOffsets);
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
        ctx.DrawIndexed(args.indexCount,
                        args.instanceCount,
                        args.firstIndex,
                        args.vertexOffset,
                        args.firstInstance);
        ++m_drawStats.directDrawCount;
        ++m_drawStats.executedPacketCount;
    }
    m_drawStats.directPacketPathUsed = true;
    return true;
}

void DepthPrepass::Execute(RHICommandContext& ctx, const ViewData& view)
{
    m_drawStats = {};

    RHITextureView* depthTargetView = m_depthTargetView;
    if (view.renderGraph && view.viewCache && m_depthTargetHandle.IsValid())
    {
        if (RHITexture* depthTarget = view.renderGraph->GetTexture(m_depthTargetHandle))
        {
            depthTargetView = view.viewCache->GetDefaultDSV(depthTarget);
        }
    }

    if (!m_pipelineCache || !m_renderScene || !depthTargetView)
    {
        return;
    }

    const auto updatePlanReport = [&](RenderExecutionStatus status,
                                      RenderPolicyReason reason,
                                      uint32 executedPackets,
                                      bool gpuLane,
                                      RenderVisibilityMode visibility)
    {
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
            lane.executedPacketCount = executedPackets;
            lane.executedDrawCount = gpuLane
                ? m_drawStats.gpuDrivenIndirectDrawCount
                : m_drawStats.directDrawCount;
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

        m_drawStats.planValidated = true;
        m_drawStats.plannedPacketCount =
            plannedGPUCount + plannedDirectCount;
        m_drawStats.compiledPacketCount = m_drawStats.plannedPacketCount;

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
        if (plannedGPU)
        {
            gpuRecorded = TryDrawGPUDrivenIndirect(
                ctx,
                view,
                plannedGPUCount,
                depthPlan->partition.drawGroupCount);
        }
        if (executeDirectLane)
        {
            TryDrawPlannedDirect(ctx, plannedDraws);
        }
        ctx.EndRenderPass();

        m_drawStats.failureReason = gpuRecorded
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
                report.status = gpuRecorded
                    ? RenderExecutionStatus::Completed
                    : RenderExecutionStatus::Failed;
                report.executedVisibility = depthPlan->visibility;
                report.reason = gpuRecorded
                    ? depthPlan->reason
                    : m_drawStats.failureReason;
                report.skippedPacketCount =
                    depthPlan->partition.skippedPacketCount;
                report.gpuDrivenLane.status = plannedGPU
                    ? (gpuRecorded ? RenderExecutionStatus::Completed
                                   : RenderExecutionStatus::Failed)
                    : RenderExecutionStatus::NotAttempted;
                report.gpuDrivenLane.reason = plannedGPU
                    ? m_drawStats.failureReason
                    : RenderPolicyReason::None;
                // A late group failure does not erase commands already
                // recorded by earlier groups. Report the actual submitted
                // prefix while marking the lane failed; never claim zero or
                // replay that prefix through Direct.
                report.gpuDrivenLane.executedPacketCount = plannedGPU
                    ? m_drawStats.gpuDrivenIndirectDrawCount
                    : 0;
                report.gpuDrivenLane.executedDrawCount = plannedGPU
                    ? m_drawStats.gpuDrivenIndirectDrawCount
                    : 0;
                report.directLane.status = executeDirectLane
                    ? RenderExecutionStatus::Completed
                    : RenderExecutionStatus::NotAttempted;
                report.directLane.reason = RenderPolicyReason::None;
                report.directLane.executedPacketCount =
                    m_drawStats.executedPacketCount;
                report.directLane.executedDrawCount =
                    m_drawStats.directDrawCount;
                if (!gpuRecorded)
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

    if (!m_gpuDrivenDepthIndirectEnabled)
    {
        return;
    }

    // Standalone compatibility path: no frame plan is available.  The
    // GPU-driven path is explicit opt-in and has no Direct replay fallback.
    RHIRenderPassDesc rpDesc;
    rpDesc.SetDepthStencil(depthTargetView,
                           RHILoadOp::Clear,
                           RHIStoreOp::Store,
                           m_pipelineCache->GetDepthClearValue(),
                           0);

    ctx.BeginRenderPass(rpDesc);

    ctx.SetViewport(view.GetRHIViewport());
    ctx.SetScissor(view.GetRHIScissor());
    TryDrawGPUDrivenIndirect(ctx, view);
    ctx.EndRenderPass();
}

} // namespace RVX
