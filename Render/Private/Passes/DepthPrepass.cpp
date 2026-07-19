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
#include "Render/GPUResourceManager.h"
#include "Resources/RenderResourceResolver.h"
#include "Render/PipelineCache.h"
#include "RHI/RHIRenderPass.h"
#include "Core/Log.h"

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
} // namespace

DepthPrepass::DepthPrepass()
{
    // Depth prepass is disabled by default - enable when depth-only pipeline is ready
    SetEnabled(false);
}

void DepthPrepass::SetResources(GPUResourceManager* gpuResources, PipelineCache* pipelineCache)
{
    m_gpuResources = gpuResources;
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
                                                    RGBufferHandle indirectDrawBuffer,
                                                    RGBufferHandle drawCountBuffer)
{
    m_gpuDrivenInstanceHandle = instanceBuffer;
    m_gpuDrivenIndirectHandle = indirectDrawBuffer;
    m_gpuDrivenDrawCountHandle = drawCountBuffer;
}

void DepthPrepass::SetDepthTarget(RHITextureView* depthView)
{
    m_depthTargetView = depthView;
}

bool DepthPrepass::IsSupported() const
{
    return m_pipelineCache && m_pipelineCache->IsInitialized() && m_pipelineCache->GetDepthOnlyPipeline();
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
        (m_resourceRegistry == nullptr && !m_gpuResources) ||
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
            m_resourceRegistry, m_gpuResources, group.mesh, group.meshId);
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

    m_pipelineCache->UpdateObjectConstants(Mat4Identity(),
                                           Mat4Identity(),
                                           Mat4Identity(),
                                           view.previousViewProjectionMatrix,
                                           false);
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
            m_resourceRegistry, m_gpuResources, group.mesh, group.meshId);
        if (!buffers.IsValid())
        {
            return false;
        }

        ctx.SetVertexBuffer(0, buffers.positionBuffer);
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

    // Get depth-only pipeline
    RHIPipeline* depthPipeline = m_pipelineCache->GetDepthOnlyPipeline();
    if (!depthPipeline)
    {
        // Depth-only pipeline not available - skip this pass
        // This is expected until the depth-only shader/pipeline is created
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

    // Bind depth-only pipeline
    ctx.SetPipeline(depthPipeline);

    // Draw all visible opaque objects with depth-only shader
    if (m_resourceRegistry != nullptr || m_gpuResources != nullptr)
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
                m_gpuResources,
                obj.mesh,
                obj.meshId);
            if (!buffers.IsValid())
            {
                ++m_drawStats.skippedMissingMeshCount;
                return;  // Mesh not uploaded yet
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

            RHIDescriptorSet* frameSet = m_pipelineCache->GetFrameDescriptorSet();
            if (frameSet)
            {
                ctx.SetDescriptorSet(0, frameSet);
            }

            RHIDescriptorSet* objectSet = m_pipelineCache->GetObjectDescriptorSet();
            if (objectSet)
            {
                const auto objectDynamicOffsets = m_pipelineCache->GetCurrentObjectDynamicOffset();
                ctx.SetDescriptorSet(1, objectSet, objectDynamicOffsets);
            }

            // Bind vertex buffers - only position is needed for depth
            ctx.SetVertexBuffer(0, buffers.positionBuffer);  // Slot 0: Position
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
