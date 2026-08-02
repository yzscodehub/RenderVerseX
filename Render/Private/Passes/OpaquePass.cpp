/**
 * @file OpaquePass.cpp
 * @brief Opaque geometry render pass implementation
 */

#include "Render/Passes/OpaquePass.h"
#include "Render/Passes/DirectDrawPacketBatch.h"
#include "Render/Passes/RenderPassClearValues.h"
#include "Core/Log.h"
#include "Render/GPUDriven/GPUCulling.h"
#include "Resources/RenderResourceResolver.h"
#include "Render/Graph/ResourceViewCache.h"
#include "Render/Lighting/ClusteredLighting.h"
#include "Render/Lighting/LightManager.h"
#include "Render/Material/MaterialSystem.h"
#include "Render/PipelineCache.h"
#include "Render/Passes/RayTracedShadowPass.h"
#include "Render/Passes/ShadowPass.h"
#include "Render/Renderer/RenderScene.h"
#include "Render/Renderer/ViewData.h"
#include "RHI/RHIRenderPass.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

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

    float SanitizeUnitRatio(float value)
    {
        return std::isfinite(value) ? clamp(value, 0.0f, 1.0f) : 0.0f;
    }

    void ClearDirectionalShadowViewData(ViewData& drawView)
    {
        drawView.directionalShadowEnabled = 0;
        drawView.directionalShadowCascadeCount = 0;
        drawView.directionalShadowCascadeSplits = Vec4(0.0f, 0.0f, 0.0f, 0.0f);
        drawView.directionalShadowCascadeFadeDistances = Vec4(0.0f, 0.0f, 0.0f, 0.0f);
        drawView.directionalShadowViewProjection = Mat4Identity();
        for (Mat4& viewProjection : drawView.directionalShadowViewProjections)
        {
            viewProjection = Mat4Identity();
        }
    }

    void TransitionVisibleMaterialTextures(const std::vector<RenderDrawItem>& drawItems,
                                           MaterialSystem& materialSystem,
                                           RHICommandContext& ctx)
    {
        for (const RenderDrawItem& item : drawItems)
        {
            if (!item.material.IsValid())
                continue;
            materialSystem.TransitionMaterialTextures(item.material, ctx);
        }
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
        layout.vertexStreams = MeshPassVertexStreams::Position |
            MeshPassVertexStreams::Normal |
            MeshPassVertexStreams::TexCoord |
            MeshPassVertexStreams::Tangent;
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

struct OpaquePass::PlannedOpaqueDraw
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
    bool allowNormalMap = false;
    bool previousWorldViewProjectionValid = false;
    bool skinned = false;
};

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
    m_shadowPass = nullptr;
    m_rayTracedShadowPass = nullptr;
    m_gpuCulling = nullptr;
    m_opaqueDrawItems = nullptr;
    m_maskedDrawItems = nullptr;
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

void OpaquePass::SetRenderScene(const RenderScene* scene,
                                const std::vector<RenderDrawItem>* opaqueDrawItems,
                                const std::vector<RenderDrawItem>* maskedDrawItems)
{
    m_renderScene = scene;
    m_opaqueDrawItems = opaqueDrawItems;
    m_maskedDrawItems = maskedDrawItems;
}

void OpaquePass::SetDirectionalShadowSource(const ShadowPass* shadowPass)
{
    m_shadowPass = shadowPass;
}

void OpaquePass::SetRayTracedShadowSource(const RayTracedShadowPass* shadowPass)
{
    m_rayTracedShadowPass = shadowPass;
}

void OpaquePass::SetGPUDrivenCullingSource(const GPUCulling* gpuCulling)
{
    m_gpuCulling = gpuCulling;
}

void OpaquePass::SetGPUDrivenRenderGraphResources(RGBufferHandle instanceBuffer,
                                                  RGBufferHandle instanceIndexBuffer,
                                                  RGBufferHandle indirectDrawBuffer,
                                                  RGBufferHandle drawCountBuffer)
{
    m_gpuDrivenInstanceHandle = instanceBuffer;
    m_gpuDrivenInstanceIndexHandle = instanceIndexBuffer;
    m_gpuDrivenIndirectHandle = indirectDrawBuffer;
    m_gpuDrivenDrawCountHandle = drawCountBuffer;
}

void OpaquePass::SetRenderTargets(RHITextureView* colorTargetView, RHITextureView* depthTargetView)
{
    m_colorTargetView = colorTargetView;
    m_depthTargetView = depthTargetView;
}

void OpaquePass::Setup(RenderGraphBuilder& builder, const ViewData& view)
{
    m_directionalShadowReadHandle = {};
    m_rayTracedShadowMaskReadHandle = {};
    m_shadowStats = {};

    const auto accumulateShadowReceivers = [this](const std::vector<RenderDrawItem>* drawItems)
    {
        if (!drawItems || !m_renderScene)
        {
            return;
        }

        for (const RenderDrawItem& item : *drawItems)
        {
            if (item.objectIndex >= m_renderScene->GetObjectCount())
            {
                continue;
            }

            m_shadowStats.receiverCandidateDrawItemCount++;
            const RenderObject& object = m_renderScene->GetObject(item.objectIndex);
            if (object.receivesShadow)
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

    // Declare that we write to the color target
    if (view.colorTarget.IsValid())
    {
        m_colorTargetHandle = builder.Write(view.colorTarget, RHIResourceState::RenderTarget);
    }

    // Declare that we write to the depth target
    if (view.depthTarget.IsValid())
    {
        builder.SetDepthStencil(view.depthTarget, true, false);
        m_depthTargetHandle = view.depthTarget;
    }

    if (m_shadowPass && m_shadowPass->IsEnabled())
    {
        RGTextureHandle shadowMap = m_shadowPass->GetShadowMapTextureHandle();
        if (shadowMap.IsValid())
        {
            shadowMap.hasSubresourceRange = true;
            shadowMap.subresourceRange = RHISubresourceRange{0, RVX_ALL_MIPS, 0, RVX_ALL_LAYERS, RHITextureAspect::Depth};
            m_directionalShadowReadHandle = builder.Read(shadowMap, RHIShaderStage::Pixel);
            m_shadowStats.requested = true;
            m_shadowStats.renderGraphReadDeclared = true;
        }
    }

    if (m_rayTracedShadowPass && m_rayTracedShadowPass->IsEnabled())
    {
        RGTextureHandle shadowMask = m_rayTracedShadowPass->GetShadowMaskHandle();
        if (shadowMask.IsValid())
        {
            m_rayTracedShadowMaskReadHandle = builder.Read(shadowMask, RHIShaderStage::Pixel);
            m_shadowStats.rayTracedRequested = true;
            m_shadowStats.rayTracedRenderGraphReadDeclared = true;
        }
    }

    if (m_gpuDrivenOpaqueIndirectEnabled && m_gpuCulling)
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

bool OpaquePass::AreGPUDrivenOpaqueGroupsDrawable(uint32& outDrawItemCount) const
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
        if (group.pipelineVariant != MaterialPipelineVariant::Opaque &&
            group.pipelineVariant != MaterialPipelineVariant::Masked)
        {
            return false;
        }

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

bool OpaquePass::TryDrawGPUDrivenIndirect(RHICommandContext& ctx,
                                          const ViewData& view,
                                          RHIFormat colorTargetFormat,
                                          RHIDescriptorSet* frameSet,
                                          bool requireObjectConstantUpload)
{
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
    if (!m_gpuCulling->GetInstanceBuffer() ||
        !m_gpuCulling->GetInstanceIndexBuffer() ||
        (!m_gpuCulling->WasGpuExecutionUsedLastCull() &&
         !m_gpuCulling->WasCpuFallbackUsedLastCull()))
    {
        m_drawStats.gpuDrivenFallbackReason =
            GPUDrivenDrawFallbackReason::CullingOutputUnavailable;
        return false;
    }
    m_drawStats.gpuDrivenCullingReady = true;

    uint32 drawItemCount = 0;
    if (!AreGPUDrivenOpaqueGroupsDrawable(drawItemCount))
    {
        m_drawStats.gpuDrivenFallbackReason =
            GPUDrivenDrawFallbackReason::DrawGroupsUnavailable;
        return false;
    }

    struct GPUDrivenOpaqueBatch
    {
        uint32 groupIndex = 0;
        MeshGPUBuffers buffers;
        RHIPipeline* pipeline = nullptr;
        MaterialBindingResult materialBinding;
    };

    const auto& groups = m_gpuCulling->GetDrawGroups();
    std::vector<GPUDrivenOpaqueBatch> batches;
    batches.reserve(groups.size());

    for (uint32 groupIndex = 0; groupIndex < static_cast<uint32>(groups.size()); ++groupIndex)
    {
        const GPUCullingDrawGroup& group = groups[groupIndex];
        MeshGPUBuffers buffers = ResolveRenderMeshBuffers(
            m_resourceRegistry, group.mesh);
        if (!buffers.IsValid())
        {
            m_drawStats.gpuDrivenFallbackReason =
                GPUDrivenDrawFallbackReason::MeshResourcesUnavailable;
            return false;
        }

        RHIPipeline* pipeline =
            m_pipelineCache->GetGPUDrivenPipelineForVariant(group.pipelineVariant, colorTargetFormat);
        if (!pipeline)
        {
            m_drawStats.gpuDrivenFallbackReason =
                GPUDrivenDrawFallbackReason::PipelineUnavailable;
            return false;
        }

        MaterialBindingOptions materialOptions;
        materialOptions.allowNormalMap = buffers.HasNormalMapTangentBasis();
        MaterialBindingResult materialBinding =
            m_materialSystem->PrepareMaterialBinding(
                group.material, view.viewCache, materialOptions);
        if (!materialBinding.IsDrawable())
        {
            m_drawStats.gpuDrivenFallbackReason =
                GPUDrivenDrawFallbackReason::MaterialBindingUnavailable;
            return false;
        }

        GPUDrivenOpaqueBatch batch;
        batch.groupIndex = groupIndex;
        batch.buffers = std::move(buffers);
        batch.pipeline = pipeline;
        batch.materialBinding = std::move(materialBinding);
        batches.push_back(std::move(batch));
    }
    m_drawStats.gpuDrivenPipelineReady = true;

    if (!frameSet)
    {
        m_drawStats.gpuDrivenFallbackReason =
            GPUDrivenDrawFallbackReason::FrameBindingsUnavailable;
        return false;
    }

    const bool objectConstantsUpdated =
        m_pipelineCache->UpdateObjectConstants(
            Mat4Identity(),
            Mat4Identity(),
            Mat4Identity(),
            view.previousViewProjectionMatrix,
            false);
    if (requireObjectConstantUpload && !objectConstantsUpdated)
    {
        m_drawStats.gpuDrivenFallbackReason =
            GPUDrivenDrawFallbackReason::ObjectBindingUnavailable;
        return false;
    }
    if (!m_pipelineCache->UpdateObjectInstanceBuffer(m_gpuCulling->GetInstanceBuffer()))
    {
        m_drawStats.gpuDrivenFallbackReason =
            GPUDrivenDrawFallbackReason::ObjectBindingUnavailable;
        return false;
    }

    RHIDescriptorSet* objectSet = m_pipelineCache->GetObjectDescriptorSet();
    if (!objectSet)
    {
        m_drawStats.gpuDrivenFallbackReason =
            GPUDrivenDrawFallbackReason::ObjectBindingUnavailable;
        return false;
    }
    const auto objectDynamicOffsets = m_pipelineCache->GetCurrentObjectDynamicOffset();
    m_drawStats.gpuDrivenEligible = true;

    bool submittedAny = false;
    for (const GPUDrivenOpaqueBatch& batch : batches)
    {
        ctx.SetPipeline(batch.pipeline);
        if (frameSet)
        {
            ctx.SetDescriptorSet(0, frameSet);
        }
        if (objectSet)
        {
            ctx.SetDescriptorSet(1, objectSet, objectDynamicOffsets);
        }

        ctx.SetVertexBuffer(0, batch.buffers.positionBuffer);
        ctx.SetVertexBuffer(6, m_gpuCulling->GetInstanceIndexBuffer());
        if (batch.buffers.normalBuffer)
        {
            ctx.SetVertexBuffer(1, batch.buffers.normalBuffer);
        }
        if (batch.buffers.uvBuffer)
        {
            ctx.SetVertexBuffer(2, batch.buffers.uvBuffer);
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

        const uint32 submittedDraws = m_gpuCulling->DrawIndexedIndirectGroup(ctx, batch.groupIndex);
        if (submittedDraws > 0)
        {
            submittedAny = true;
            ++m_drawStats.gpuDrivenIndirectBatchCount;
            m_drawStats.gpuDrivenIndirectDrawCount += submittedDraws;
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
    const ViewData& view,
    RHIFormat colorTargetFormat,
    std::vector<PlannedOpaqueDraw>& outPlannedDraws)
{
    outPlannedDraws.clear();
    m_drawStats.planRequested = true;
    m_drawStats.failureReason = RenderPolicyReason::UnexpectedRecordingFailure;
    if (view.renderFrameExecutionPlan == nullptr ||
        view.meshPassPreparation == nullptr ||
        m_renderScene == nullptr || m_resourceRegistry == nullptr ||
        m_pipelineCache == nullptr || m_materialSystem == nullptr)
    {
        return false;
    }

    const DirectDrawPacketBatchBuildResult built =
        BuildDirectDrawPacketBatch(*view.renderFrameExecutionPlan,
                                   RenderPassKind::Opaque,
                                   view.meshPassPreparation->opaque);
    if (!built.succeeded ||
        built.batch.packets.size() > std::numeric_limits<uint32>::max())
    {
        return false;
    }

    m_drawStats.planValidated = true;
    m_drawStats.plannedPacketCount =
        static_cast<uint32>(built.batch.packets.size());

    outPlannedDraws.reserve(built.batch.packets.size());
    for (const DirectDrawPacket& draw : built.batch.packets)
    {
        const RenderDrawPacket& packet = draw.packet;
        if (packet.pass != RenderPassKind::Opaque ||
            packet.primitiveData == RVX_INVALID_PRIMITIVE_DATA_INDEX ||
            !packet.geometryKey.mesh.IsValid() ||
            packet.geometryKey.indexType != MeshUploadIndexType::UInt32 ||
            packet.pipelineKey.topology != MeshUploadPrimitiveTopology::Triangles ||
            packet.arguments.indexCount == 0 ||
            packet.arguments.instanceCount != 1 ||
            packet.arguments.firstInstance != 0)
        {
            outPlannedDraws.clear();
            return false;
        }

        RenderObject object;
        if (!ResolveUniqueObject(*m_renderScene,
                                 packet.objectId,
                                 packet.primitiveData,
                                 object) ||
            object.mesh != packet.geometryKey.mesh ||
            object.skinningMatrices.size() > std::numeric_limits<uint32>::max())
        {
            outPlannedDraws.clear();
            return false;
        }

        const bool masked = IsMaskedPacket(packet);
        const bool skinned = IsSkinnedPacket(packet);
        const bool missingMaterial =
            HasDrawFlag(packet.flags, RenderDrawFlags::MissingMaterial) ||
            !packet.materialKey.material.IsValid();
        const bool previousWorldViewProjectionValid =
            object.previousWorldMatrixValid != 0 &&
            view.previousViewProjectionValid != 0 &&
            !view.resetTemporalHistory;
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
            skinned != object.HasSkinningData() ||
            (masked && packet.materialKey.materialMode != MaterialRenderMode::Masked) ||
            (!masked && packet.materialKey.materialMode != MaterialRenderMode::Opaque) ||
            missingMaterial !=
                HasDrawFlag(packet.flags, RenderDrawFlags::MissingMaterial) ||
            HasDrawFlag(packet.flags, RenderDrawFlags::CastsShadow) !=
                object.castsShadow ||
            HasDrawFlag(packet.flags, RenderDrawFlags::ReceivesShadow) !=
                object.receivesShadow)
        {
            outPlannedDraws.clear();
            return false;
        }

        const MeshGPUBuffers buffers = ResolveRenderMeshBuffers(
            m_resourceRegistry, packet.geometryKey.mesh);
        if (!buffers.IsValid() ||
            buffers.positionBuffer == nullptr ||
            buffers.normalBuffer == nullptr || !buffers.hasNormals ||
            buffers.uvBuffer == nullptr || !buffers.hasUVs ||
            buffers.tangentBuffer == nullptr ||
            buffers.indexBuffer == nullptr ||
            packet.geometryKey.submeshIndex >= buffers.submeshes.size())
        {
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
            outPlannedDraws.clear();
            return false;
        }

        const bool allowNormalMap = buffers.HasNormalMapTangentBasis();
        if (skinned &&
            (!object.HasSkinningData() || !buffers.HasSkinningVertexData()))
        {
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
            outPlannedDraws.clear();
            return false;
        }

        RHIPipeline* pipeline = m_pipelineCache->GetPipelineForVariant(
            packet.pipelineKey.materialVariant,
            colorTargetFormat,
            skinned ? DefaultLitDirectVertexInputMode::Skinned
                    : DefaultLitDirectVertexInputMode::Rigid);
        if (pipeline == nullptr)
        {
            outPlannedDraws.clear();
            return false;
        }

        MaterialBindingOptions materialOptions;
        materialOptions.allowNormalMap = allowNormalMap;
        MaterialBindingResult materialBinding =
            m_materialSystem->PrepareMaterialBinding(
                packet.materialKey.material, view.viewCache, materialOptions);
        if (!materialBinding.IsDrawable() ||
            (missingMaterial && !materialBinding.usedFallback))
        {
            ++m_drawStats.skippedMaterialBindingCount;
            outPlannedDraws.clear();
            return false;
        }

        PlannedOpaqueDraw planned;
        planned.packet = draw;
        planned.object = std::move(object);
        planned.buffers = buffers;
        planned.submesh = submesh;
        planned.pipeline = pipeline;
        planned.frameSet = m_pipelineCache->GetFrameDescriptorSet();
        planned.objectSet = m_pipelineCache->GetObjectDescriptorSet();
        planned.materialBinding = std::move(materialBinding);
        planned.allowNormalMap = allowNormalMap;
        planned.previousWorldViewProjectionValid =
            previousWorldViewProjectionValid;
        planned.skinned = skinned;
        if (planned.frameSet == nullptr || planned.objectSet == nullptr ||
            planned.materialBinding.descriptorSet == nullptr)
        {
            outPlannedDraws.clear();
            return false;
        }

        if (!m_pipelineCache->UpdateObjectConstants(
                planned.object.worldMatrix,
                planned.object.normalMatrix,
                planned.object.previousWorldMatrix,
                view.previousViewProjectionMatrix,
                planned.previousWorldViewProjectionValid,
                planned.object.receivesShadow,
                ResolveSkinningMatrices(planned.object, planned.buffers)))
        {
            outPlannedDraws.clear();
            return false;
        }
        planned.objectDynamicOffsets =
            m_pipelineCache->GetCurrentObjectDynamicOffset();
        outPlannedDraws.push_back(std::move(planned));
    }

    return true;
}

bool OpaquePass::TryDrawPlannedDirect(
    RHICommandContext& ctx,
    const ViewData& view,
    RHITextureView* colorTargetView,
    std::span<const PlannedOpaqueDraw> plannedDraws)
{
    RHIRenderPassDesc rpDesc;
    rpDesc.AddColorAttachment(colorTargetView,
                              RHILoadOp::Clear,
                              RHIStoreOp::Store,
                              RVX_SCENE_COLOR_CLEAR_VALUE);
    if (m_depthTargetView)
    {
        rpDesc.SetDepthStencil(m_depthTargetView,
                               RHILoadOp::Clear,
                               RHIStoreOp::Store,
                               m_pipelineCache->GetDepthClearValue(),
                               0);
    }

    ctx.BeginRenderPass(rpDesc);
    ctx.SetViewport(view.GetRHIViewport());
    ctx.SetScissor(view.GetRHIScissor());
    for (const PlannedOpaqueDraw& planned : plannedDraws)
    {
        ctx.SetPipeline(planned.pipeline);
        ctx.SetDescriptorSet(0, planned.frameSet);
        ctx.SetDescriptorSet(1, planned.objectSet, planned.objectDynamicOffsets);
        ctx.SetDescriptorSet(2,
                             planned.materialBinding.descriptorSet,
                             planned.materialBinding.dynamicOffsets);
        ctx.SetVertexBuffer(0, planned.buffers.positionBuffer);
        ctx.SetVertexBuffer(1, planned.buffers.normalBuffer);
        ctx.SetVertexBuffer(2, planned.buffers.uvBuffer);
        ctx.SetVertexBuffer(3, planned.buffers.tangentBuffer);
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

void OpaquePass::Execute(RHICommandContext& ctx, const ViewData& view)
{
    m_drawStats = {};

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
        if (view.renderFrameExecutionReport == nullptr)
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
        reportPlannedFailure(opaquePlan != nullptr &&
                             opaquePlan->partition.gpuDrivenPacketCount != 0);
        return;
    }

    if (!m_materialSystem || !m_materialSystem->IsInitialized())
    {
        RVX_CORE_WARN("OpaquePass: MaterialSystem not available");
        reportPlannedFailure(opaquePlan != nullptr &&
                             opaquePlan->partition.gpuDrivenPacketCount != 0);
        return;
    }

    RHITextureView* colorTargetView = m_colorTargetView;
    RHIFormat colorTargetFormat = RHIFormat::Unknown;
    if (view.renderGraph && view.viewCache && m_colorTargetHandle.IsValid())
    {
        if (const RHITextureDesc* colorTargetDesc = view.renderGraph->GetTextureDesc(m_colorTargetHandle))
        {
            colorTargetFormat = colorTargetDesc->format;
        }
        if (RHITexture* colorTarget = view.renderGraph->GetTexture(m_colorTargetHandle))
        {
            colorTargetView = view.viewCache->GetDefaultRTV(colorTarget);
        }
    }

    if (!colorTargetView)
    {
        RVX_CORE_WARN("OpaquePass: No color target view set");
        reportPlannedFailure(opaquePlan != nullptr &&
                             opaquePlan->partition.gpuDrivenPacketCount != 0);
        return;
    }

    if (!hasPublishedPlan && !m_gpuDrivenOpaqueIndirectEnabled)
    {
        return;
    }

    if (!hasPublishedPlan && m_gpuDrivenOpaqueIndirectEnabled &&
        m_materialSystem && m_renderScene)
    {
        if (m_opaqueDrawItems)
            TransitionVisibleMaterialTextures(*m_opaqueDrawItems,
                                              *m_materialSystem,
                                              ctx);
        if (m_maskedDrawItems)
            TransitionVisibleMaterialTextures(*m_maskedDrawItems,
                                              *m_materialSystem,
                                              ctx);
    }

    ViewData drawView = view;
    drawView.rayTracedShadowEnabled = 0;
    drawView.rayTracedShadowMode = RayTracedShadowMode::ComplementRaster;
    DirectionalShadowFrameResources shadowResources;
    if (m_shadowPass && m_directionalShadowReadHandle.IsValid() &&
        view.renderGraph && view.viewCache &&
        !m_shadowPass->GetCascades().empty())
    {
        RHITexture* shadowTexture = view.renderGraph->GetTexture(m_directionalShadowReadHandle);
        RHITextureViewDesc shadowViewDesc;
        if (shadowTexture)
        {
            shadowViewDesc.format = shadowTexture->GetFormat();
            shadowViewDesc.dimension = shadowTexture->GetDimension();
            shadowViewDesc.subresourceRange = RHISubresourceRange::All();
            shadowViewDesc.type = RHITextureViewType::ShaderResource;
            if (IsDepthFormat(shadowViewDesc.format))
            {
                shadowViewDesc.subresourceRange.aspect = RHITextureAspect::Depth;
            }
            shadowViewDesc.debugName = "DirectionalShadowSRV";
        }

        RHITextureView* shadowView = shadowTexture ? view.viewCache->GetTextureView(shadowTexture, shadowViewDesc)
                                                   : nullptr;
        const ShadowPassConfig& shadowConfig = m_shadowPass->GetConfig();
        const auto& cascades = m_shadowPass->GetCascades();
        const uint32 cascadeCount = std::min(static_cast<uint32>(cascades.size()),
                                             RVX_MAX_DIRECTIONAL_SHADOW_CASCADES);
        const float nearClip = std::max(0.001f, view.nearPlane);
        const float farClip = std::max(nearClip + 1.0f, view.farPlane);
        const float clipRange = farClip - nearClip;
        drawView.directionalShadowEnabled = shadowView ? 1 : 0;
        drawView.directionalShadowCascadeCount = shadowView ? cascadeCount : 0;
        drawView.directionalShadowCascadeSplits = Vec4(0.0f, 0.0f, 0.0f, 0.0f);
        drawView.directionalShadowCascadeFadeDistances = Vec4(0.0f, 0.0f, 0.0f, 0.0f);
        for (uint32 i = 0; i < cascadeCount; ++i)
        {
            drawView.directionalShadowViewProjections[i] = cascades[i].viewProjection;
            drawView.directionalShadowCascadeSplits[i] = nearClip + cascades[i].splitDepth * clipRange;
        }
        const float blendRatio = SanitizeUnitRatio(shadowConfig.cascadeBlendRatio);
        for (uint32 i = 0; i + 1 < cascadeCount; ++i)
        {
            const float splitDistance = drawView.directionalShadowCascadeSplits[i];
            const float previousSplit = i == 0 ? nearClip : drawView.directionalShadowCascadeSplits[i - 1];
            const float cascadeSpan = std::max(0.0f, splitDistance - previousSplit);
            drawView.directionalShadowCascadeFadeDistances[i] = std::min(cascadeSpan, cascadeSpan * blendRatio);
        }
        drawView.directionalShadowViewProjection = cascadeCount > 0 ? cascades[0].viewProjection : Mat4Identity();
        drawView.directionalShadowDepthBias = shadowConfig.shadowBias;
        drawView.directionalShadowStrength = 1.0f;
        drawView.directionalShadowInvMapSize = shadowConfig.shadowMapSize > 0
                                                   ? 1.0f / static_cast<float>(shadowConfig.shadowMapSize)
                                                   : 0.0f;
        drawView.directionalShadowFilterRadiusTexels = shadowConfig.filterRadiusTexels;
        drawView.directionalShadowNormalBias = shadowConfig.normalBias;
        shadowResources.enabled = true;
        shadowResources.shadowMapView = shadowView;
    }
    else
    {
        ClearDirectionalShadowViewData(drawView);
    }

    FrameLightResources lightResources;
    if (m_lightManager)
    {
        lightResources.lightConstantsBuffer = m_lightManager->GetLightConstantsBuffer();
        lightResources.pointLightsBuffer = m_lightManager->GetPointLightsBuffer();
        lightResources.spotLightsBuffer = m_lightManager->GetSpotLightsBuffer();
    }
    if (m_clusteredLighting && m_clusteredLighting->IsInitialized())
    {
        lightResources.clusterConstantsBuffer = m_clusteredLighting->GetClusterConstantsBuffer();
        lightResources.clusterBuffer = m_clusteredLighting->GetClusterBuffer();
        lightResources.clusterLightIndexBuffer = m_clusteredLighting->GetLightIndexBuffer();
    }
    m_pipelineCache->UpdateFrameLightResources(lightResources);

    const DirectionalShadowFrameBindingResult shadowBinding =
        m_pipelineCache->UpdateDirectionalShadowFrameResources(shadowResources);
    if (shadowBinding.shadowSamplingEnabled)
    {
        drawView.directionalShadowEnabled = 1;
    }
    else
    {
        ClearDirectionalShadowViewData(drawView);
    }
    m_shadowStats.frameShadowReady = shadowBinding.shadowSamplingEnabled;

    RayTracedShadowFrameResources rayTracedShadowResources;
    if (m_rayTracedShadowMaskReadHandle.IsValid() && view.renderGraph && view.viewCache)
    {
        RHITexture* shadowMask = view.renderGraph->GetTexture(m_rayTracedShadowMaskReadHandle);
        if (shadowMask)
        {
            RHITextureViewDesc viewDesc;
            viewDesc.format = shadowMask->GetFormat();
            viewDesc.dimension = shadowMask->GetDimension();
            viewDesc.subresourceRange = RHISubresourceRange::All();
            viewDesc.type = RHITextureViewType::ShaderResource;
            viewDesc.debugName = "RayTracedShadowMaskSRV";

            rayTracedShadowResources.shadowMaskView = view.viewCache->GetTextureView(shadowMask, viewDesc);
            rayTracedShadowResources.enabled = rayTracedShadowResources.shadowMaskView != nullptr;
        }
    }
    const RayTracedShadowFrameBindingResult rayTracedShadowBinding =
        m_pipelineCache->UpdateRayTracedShadowFrameResources(rayTracedShadowResources);
    drawView.rayTracedShadowEnabled = rayTracedShadowBinding.shadowMaskSamplingEnabled ? 1 : 0;
    if (rayTracedShadowBinding.shadowMaskSamplingEnabled && m_rayTracedShadowPass)
    {
        const ShadowPassConfig& rayTracedShadowConfig = m_rayTracedShadowPass->GetConfig();
        drawView.rayTracedShadowFilterRadiusPixels =
            std::max(0.0f, rayTracedShadowConfig.filterRadiusTexels);
        drawView.rayTracedShadowMode = rayTracedShadowConfig.rayTracedShadowMode;
        if (drawView.rayTracedShadowMode == RayTracedShadowMode::ReplaceRaster)
        {
            ClearDirectionalShadowViewData(drawView);
        }
    }
    m_shadowStats.rayTracedFrameMaskReady = rayTracedShadowBinding.shadowMaskSamplingEnabled;

    m_pipelineCache->UpdateViewConstants(drawView);

    // A published plan owns lane selection. Preflight the full Direct lane
    // before recording anything, and never replay Direct after a GPU-lane
    // recording failure.
    if (hasPublishedPlan)
    {
        const bool plannedGPU =
            opaquePlan->partition.gpuDrivenPacketCount != 0;
        if (plannedGPU && !ValidateWholePassGPUDrivenPacketRange(
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
        if (!plannedGPU)
        {
            std::vector<PlannedOpaqueDraw> plannedDraws;
            if (!BuildPlannedDirectBatch(
                    view, colorTargetFormat, plannedDraws))
            {
                updatePlanReport(RenderExecutionStatus::Failed,
                                 m_drawStats.failureReason,
                                 0,
                                 false,
                                 opaquePlan->visibility);
                return;
            }

            // Only the preflighted packet values determine material texture
            // transitions for the planned Direct lane.
            for (const PlannedOpaqueDraw& planned : plannedDraws)
            {
                MaterialBindingOptions materialOptions;
                materialOptions.allowNormalMap = planned.allowNormalMap;
                m_materialSystem->TransitionMaterialTextures(
                    planned.packet.packet.materialKey.material,
                    ctx,
                    materialOptions);
            }

            TryDrawPlannedDirect(ctx,
                                 drawView,
                                 colorTargetView,
                                 plannedDraws);
            m_drawStats.failureReason = RenderPolicyReason::None;
            updatePlanReport(RenderExecutionStatus::Completed,
                             RenderPolicyReason::None,
                             m_drawStats.executedPacketCount,
                             false,
                             opaquePlan->visibility);
            return;
        }

        // The GPU lane retains its established indirect recording path. It
        // deliberately has no Direct fallback once the plan selected it.
        if (m_renderScene)
        {
            if (m_opaqueDrawItems)
            {
                TransitionVisibleMaterialTextures(*m_opaqueDrawItems,
                                                  *m_materialSystem,
                                                  ctx);
            }
            if (m_maskedDrawItems)
            {
                TransitionVisibleMaterialTextures(*m_maskedDrawItems,
                                                  *m_materialSystem,
                                                  ctx);
            }
        }

        RHIRenderPassDesc rpDesc;
        rpDesc.AddColorAttachment(colorTargetView,
                                  RHILoadOp::Clear,
                                  RHIStoreOp::Store,
                                  RVX_SCENE_COLOR_CLEAR_VALUE);
        if (m_depthTargetView)
        {
            rpDesc.SetDepthStencil(m_depthTargetView,
                                   RHILoadOp::Clear,
                                   RHIStoreOp::Store,
                                   m_pipelineCache->GetDepthClearValue(),
                                   0);
        }
        ctx.BeginRenderPass(rpDesc);
        ctx.SetViewport(drawView.GetRHIViewport());
        ctx.SetScissor(drawView.GetRHIScissor());

        const bool recorded = TryDrawGPUDrivenIndirect(
            ctx,
            drawView,
            colorTargetFormat,
            m_pipelineCache->GetFrameDescriptorSet(),
            true);
        ctx.EndRenderPass();

        if (!recorded)
        {
            m_drawStats.failureReason =
                RenderPolicyReason::UnexpectedRecordingFailure;
            updatePlanReport(RenderExecutionStatus::Failed,
                             m_drawStats.failureReason,
                             0,
                             true,
                             opaquePlan->visibility);
        }
        else
        {
            m_drawStats.failureReason = RenderPolicyReason::None;
            updatePlanReport(RenderExecutionStatus::Completed,
                             RenderPolicyReason::None,
                             m_drawStats.gpuDrivenIndirectDrawCount,
                             true,
                             opaquePlan->visibility);
        }

        if (view.renderFrameExecutionReport != nullptr)
        {
            for (RenderPassExecutionReport& report :
                 view.renderFrameExecutionReport->passes)
            {
                if (report.pass == RenderPassKind::Opaque)
                {
                    report.directLane.status = RenderExecutionStatus::NotAttempted;
                    report.directLane.reason = RenderPolicyReason::None;
                    break;
                }
            }
        }
        return;
    }

    // Standalone compatibility path: no frame plan is available. The
    // GPU-driven path is explicit opt-in and never replays Direct work.
    RHIRenderPassDesc rpDesc;
    rpDesc.AddColorAttachment(colorTargetView,
                              RHILoadOp::Clear,
                              RHIStoreOp::Store,
                              RVX_SCENE_COLOR_CLEAR_VALUE);
    if (m_depthTargetView)
    {
        rpDesc.SetDepthStencil(m_depthTargetView,
                               RHILoadOp::Clear,
                               RHIStoreOp::Store,
                               m_pipelineCache->GetDepthClearValue(),
                               0);
    }

    ctx.BeginRenderPass(rpDesc);
    ctx.SetViewport(drawView.GetRHIViewport());
    ctx.SetScissor(drawView.GetRHIScissor());
    TryDrawGPUDrivenIndirect(ctx,
                             drawView,
                             colorTargetFormat,
                             m_pipelineCache->GetFrameDescriptorSet(),
                             false);
    ctx.EndRenderPass();
}

} // namespace RVX
