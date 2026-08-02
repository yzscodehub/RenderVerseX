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
        const RenderDrawPacket& packet,
        const RenderObject& object) noexcept
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
        (void)object;
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

    struct LegacyDepthTraceEntry
    {
        RenderDrawPacket packet;
        RenderSubmissionLayout layout;

        bool operator==(const LegacyDepthTraceEntry&) const noexcept = default;
    };

    bool BuildLegacyDepthPacket(const RenderScene& scene,
                                const RenderResourceRegistry& registry,
                                const RenderDrawItem& item,
                                RenderDrawPacket& outPacket,
                                RenderSubmissionLayout& outLayout) noexcept
    {
        if (item.objectIndex >= scene.GetObjectCount() ||
            !item.mesh.IsValid())
        {
            return false;
        }

        const RenderObject& object = scene.GetObject(item.objectIndex);
        if (object.mesh != item.mesh || object.entityId == 0)
        {
            return false;
        }
        const MeshGPUBuffers buffers = ResolveRenderMeshBuffers(&registry, item.mesh);
        if (!buffers.IsValid() || item.submeshIndex >= buffers.submeshes.size())
        {
            return false;
        }

        const SubmeshGPUInfo& submesh = buffers.submeshes[item.submeshIndex];
        RenderDrawPacket packet = item.packet;
        packet.objectId = object.entityId;
        packet.primitiveData = item.objectIndex;
        packet.submeshIndex = item.submeshIndex;
        packet.pass = RenderPassKind::Depth;
        packet.pipelineKey.materialVariant =
            GetPipelineVariantForRenderMode(item.renderMode);
        packet.pipelineKey.topology = MeshUploadPrimitiveTopology::Triangles;
        packet.pipelineKey.skinned = object.HasSkinningData();
        packet.geometryKey.mesh = item.mesh;
        packet.geometryKey.submeshIndex = item.submeshIndex;
        packet.geometryKey.indexType = MeshUploadIndexType::UInt32;
        packet.materialKey.material = item.material;
        packet.materialKey.materialMode = item.renderMode;
        packet.arguments.indexCount = submesh.indexCount;
        packet.arguments.instanceCount = 1;
        packet.arguments.firstIndex = submesh.indexOffset;
        packet.arguments.vertexOffset = submesh.baseVertex;
        packet.arguments.firstInstance = 0;
        packet.flags = RenderDrawFlags::None;
        if (packet.pipelineKey.skinned)
        {
            packet.flags = packet.flags | RenderDrawFlags::Skinned;
        }
        if (item.renderMode == MaterialRenderMode::Masked)
        {
            packet.flags = packet.flags | RenderDrawFlags::Masked;
        }
        if (object.castsShadow)
        {
            packet.flags = packet.flags | RenderDrawFlags::CastsShadow;
        }
        if (object.receivesShadow)
        {
            packet.flags = packet.flags | RenderDrawFlags::ReceivesShadow;
        }
        if (!item.material.IsValid())
        {
            packet.flags = packet.flags | RenderDrawFlags::MissingMaterial;
        }

        outPacket = packet;
        outLayout = MakeExpectedDirectLayout(packet, object);
        return true;
    }

    bool BuildLegacyDepthTrace(const RenderScene& scene,
                               const RenderResourceRegistry& registry,
                               const std::vector<RenderDrawItem>* opaque,
                               const std::vector<RenderDrawItem>* masked,
                               std::vector<LegacyDepthTraceEntry>& outTrace) noexcept
    {
        outTrace.clear();
        const auto append = [&](const std::vector<RenderDrawItem>* items)
        {
            if (items == nullptr)
            {
                return true;
            }
            for (const RenderDrawItem& item : *items)
            {
                LegacyDepthTraceEntry entry;
                if (!BuildLegacyDepthPacket(scene,
                                            registry,
                                            item,
                                            entry.packet,
                                            entry.layout))
                {
                    return false;
                }
                outTrace.push_back(std::move(entry));
            }
            return true;
        };
        return append(opaque) && append(masked);
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

bool DepthPrepass::AreGPUDrivenDepthGroupsDrawable(uint32& outDrawItemCount) const
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

    return outDrawItemCount > 0 &&
        outDrawItemCount == sourceDrawItemCount &&
        m_gpuCulling->GetInstanceCount() == outDrawItemCount;
}

bool DepthPrepass::TryDrawGPUDrivenIndirect(RHICommandContext& ctx, const ViewData& view)
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
    if (!AreGPUDrivenDepthGroupsDrawable(drawItemCount))
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
    m_drawStats.dualBuildCompared = true;
    m_drawStats.dualBuildMatched = false;

    std::vector<LegacyDepthTraceEntry> legacyTrace;
    if (!BuildLegacyDepthTrace(*m_renderScene,
                               *m_resourceRegistry,
                               m_opaqueDrawItems,
                               m_maskedDrawItems,
                               legacyTrace) ||
        legacyTrace.size() != built.batch.packets.size())
    {
        m_drawStats.failureReason = RenderPolicyReason::UnexpectedRecordingFailure;
        return false;
    }

    outPlannedDraws.reserve(built.batch.packets.size());
    for (size_t index = 0; index < built.batch.packets.size(); ++index)
    {
        const DirectDrawPacket& draw = built.batch.packets[index];
        const LegacyDepthTraceEntry& trace = legacyTrace[index];
        if (draw.packet != trace.packet || draw.layout != trace.layout)
        {
            m_drawStats.failureReason = RenderPolicyReason::UnexpectedRecordingFailure;
            outPlannedDraws.clear();
            return false;
        }

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
            MakeExpectedDirectLayout(packet, object);
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

    m_drawStats.dualBuildMatched = true;
    return true;
}

bool DepthPrepass::TryDrawPlannedDirect(
    RHICommandContext& ctx,
    const ViewData& view,
    RHITextureView* depthTargetView,
    std::span<const PlannedDepthDraw> plannedDraws)
{
    RHIRenderPassDesc rpDesc;
    rpDesc.SetDepthStencil(depthTargetView,
                           RHILoadOp::Clear,
                           RHIStoreOp::Store,
                           m_pipelineCache->GetDepthClearValue(),
                           0);
    ctx.BeginRenderPass(rpDesc);
    ctx.SetViewport(view.GetRHIViewport());
    ctx.SetScissor(view.GetRHIScissor());

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
    ctx.EndRenderPass();
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

    if (!m_pipelineCache || !m_renderScene || (!m_opaqueDrawItems && !m_maskedDrawItems) || !depthTargetView)
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

        const bool plannedGPU =
            depthPlan->partition.gpuDrivenPacketCount != 0;
        if (!plannedGPU)
        {
            std::vector<PlannedDepthDraw> plannedDraws;
            if (!BuildPlannedDirectBatch(view, plannedDraws))
            {
                updatePlanReport(RenderExecutionStatus::Failed,
                                 m_drawStats.failureReason,
                                 0,
                                 false,
                                 RenderVisibilityMode::Cpu);
                return;
            }

            TryDrawPlannedDirect(ctx,
                                 view,
                                 depthTargetView,
                                 plannedDraws);
            m_drawStats.failureReason = RenderPolicyReason::None;
            updatePlanReport(RenderExecutionStatus::Completed,
                             RenderPolicyReason::None,
                             m_drawStats.executedPacketCount,
                             false,
                             RenderVisibilityMode::Cpu);
            return;
        }

        RHIRenderPassDesc rpDesc;
        rpDesc.SetDepthStencil(depthTargetView,
                               RHILoadOp::Clear,
                               RHIStoreOp::Store,
                               m_pipelineCache->GetDepthClearValue(),
                               0);
        ctx.BeginRenderPass(rpDesc);
        ctx.SetViewport(view.GetRHIViewport());
        ctx.SetScissor(view.GetRHIScissor());
        const bool recorded = TryDrawGPUDrivenIndirect(ctx, view);
        if (!recorded)
        {
            m_drawStats.failureReason =
                RenderPolicyReason::UnexpectedRecordingFailure;
            updatePlanReport(RenderExecutionStatus::Failed,
                             m_drawStats.failureReason,
                             0,
                             true,
                             depthPlan->visibility);
            if (view.renderFrameExecutionReport != nullptr)
            {
                for (RenderPassExecutionReport& report :
                     view.renderFrameExecutionReport->passes)
                {
                    if (report.pass == RenderPassKind::Depth)
                    {
                        report.executedVisibility = depthPlan->visibility;
                        report.gpuDrivenLane.status =
                            RenderExecutionStatus::Failed;
                        report.gpuDrivenLane.reason = m_drawStats.failureReason;
                        report.directLane.status =
                            RenderExecutionStatus::NotAttempted;
                        break;
                    }
                }
            }
        }
        else
        {
            updatePlanReport(RenderExecutionStatus::Completed,
                             RenderPolicyReason::None,
                             m_drawStats.gpuDrivenIndirectDrawCount,
                             true,
                             depthPlan->visibility);
            if (view.renderFrameExecutionReport != nullptr)
            {
                for (RenderPassExecutionReport& report :
                     view.renderFrameExecutionReport->passes)
                {
                    if (report.pass == RenderPassKind::Depth)
                    {
                        report.executedVisibility =
                            depthPlan->visibility;
                        report.gpuDrivenLane.status =
                            RenderExecutionStatus::Completed;
                        report.gpuDrivenLane.reason = RenderPolicyReason::None;
                        report.gpuDrivenLane.executedPacketCount =
                            m_drawStats.gpuDrivenIndirectDrawCount;
                        report.gpuDrivenLane.executedDrawCount =
                            m_drawStats.gpuDrivenIndirectDrawCount;
                        break;
                    }
                }
            }
        }
        ctx.EndRenderPass();
        return;
    }

    // Standalone compatibility path: no frame plan is available, so retain
    // the historical opaque/masked draw-item execution for isolated tests.
    RHIPipeline* depthPipeline = m_pipelineCache->GetDepthOnlyPipeline();
    if (!depthPipeline)
    {
        return;
    }

    // Begin render pass with depth-only attachment (no color)
    RHIRenderPassDesc rpDesc;
    rpDesc.SetDepthStencil(depthTargetView,
                           RHILoadOp::Clear,
                           RHIStoreOp::Store,
                           m_pipelineCache->GetDepthClearValue(),
                           0);

    ctx.BeginRenderPass(rpDesc);

    // Set viewport and scissor
    ctx.SetViewport(view.GetRHIViewport());
    ctx.SetScissor(view.GetRHIScissor());

    if (TryDrawGPUDrivenIndirect(ctx, view))
    {
        ctx.EndRenderPass();
        return;
    }

    // Draw all visible opaque/masked objects with their exact depth pipeline.
    if (m_resourceRegistry != nullptr)
    {
        const auto drawItem = [&](const RenderDrawItem& item)
        {
            if (item.objectIndex >= m_renderScene->GetObjectCount())
            {
                ++m_drawStats.skippedInvalidObjectCount;
                return;
            }

            const RenderObject& obj = m_renderScene->GetObject(item.objectIndex);

            // Get GPU buffers for this mesh
            MeshGPUBuffers buffers = ResolveRenderMeshBuffers(
                m_resourceRegistry,
                obj.mesh);
            if (!buffers.IsValid())
            {
                ++m_drawStats.skippedMissingMeshCount;
                return;  // Mesh not uploaded yet
            }

            const bool masked = item.renderMode == MaterialRenderMode::Masked;
            RHIPipeline* pipeline = depthPipeline;
            MaterialBindingResult materialBinding;
            if (masked)
            {
                if (m_materialSystem == nullptr ||
                    buffers.uvBuffer == nullptr || !buffers.hasUVs)
                {
                    ++m_drawStats.skippedMissingUVCount;
                    return;
                }
                pipeline = m_pipelineCache->GetMaskedDepthOnlyPipeline();
                if (pipeline == nullptr)
                {
                    ++m_drawStats.skippedMaterialBindingCount;
                    return;
                }
                MaterialBindingOptions options;
                options.allowNormalMap = false;
                materialBinding = m_materialSystem->PrepareMaterialBinding(
                    item.material, view.viewCache, options);
                if (!materialBinding.IsDrawable())
                {
                    ++m_drawStats.skippedMaterialBindingCount;
                    return;
                }
            }

            // Update per-object constants
            m_pipelineCache->UpdateObjectConstants(obj.worldMatrix,
                                                   obj.normalMatrix,
                                                   obj.previousWorldMatrix,
                                                   view.previousViewProjectionMatrix,
                                                   obj.previousWorldMatrixValid != 0 &&
                                                       view.previousViewProjectionValid != 0 &&
                                                       !view.resetTemporalHistory,
                                                   ResolveSkinningMatrices(obj, buffers));

            ctx.SetPipeline(pipeline);
            if (RHIDescriptorSet* frameSet = m_pipelineCache->GetFrameDescriptorSet())
            {
                ctx.SetDescriptorSet(0, frameSet);
            }

            if (RHIDescriptorSet* objectSet = m_pipelineCache->GetObjectDescriptorSet())
            {
                const auto objectDynamicOffsets =
                    m_pipelineCache->GetCurrentObjectDynamicOffset();
                ctx.SetDescriptorSet(1, objectSet, objectDynamicOffsets);
            }

            // Bind vertex buffers - only position is needed for depth
            ctx.SetVertexBuffer(0, buffers.positionBuffer);  // Slot 0: Position
            if (masked)
            {
                ctx.SetVertexBuffer(2, buffers.uvBuffer);
                ctx.SetDescriptorSet(2,
                                     materialBinding.descriptorSet,
                                     materialBinding.dynamicOffsets);
            }
            if (buffers.boneIndicesBuffer)
            {
                ctx.SetVertexBuffer(4, buffers.boneIndicesBuffer);
            }
            if (buffers.boneWeightsBuffer)
            {
                ctx.SetVertexBuffer(5, buffers.boneWeightsBuffer);
            }

            // Bind index buffer
            ctx.SetIndexBuffer(buffers.indexBuffer, RHIFormat::R32_UINT);

            if (item.submeshIndex >= buffers.submeshes.size())
            {
                ++m_drawStats.skippedInvalidSubmeshCount;
                return;
            }

            const SubmeshGPUInfo& submesh = buffers.submeshes[item.submeshIndex];
            ctx.DrawIndexed(submesh.indexCount, 1,
                            submesh.indexOffset, submesh.baseVertex, 0);
            ++m_drawStats.directDrawCount;
        };

        if (m_opaqueDrawItems)
        {
            for (const RenderDrawItem& item : *m_opaqueDrawItems)
            {
                drawItem(item);
            }
        }

        if (m_maskedDrawItems)
        {
            for (const RenderDrawItem& item : *m_maskedDrawItems)
            {
                drawItem(item);
            }
        }
    }

    ctx.EndRenderPass();
}

} // namespace RVX
