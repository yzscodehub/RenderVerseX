/**
 * @file DepthPrepass.cpp
 * @brief DepthPrepass implementation
 */

#include "Render/Passes/DepthPrepass.h"
#include "Render/Renderer/ViewData.h"
#include "Render/Renderer/RenderScene.h"
#include "Render/Renderer/RenderDrawItem.h"
#include "Render/GPUResourceManager.h"
#include "Render/PipelineCache.h"
#include "RHI/RHIRenderPass.h"
#include "Core/Log.h"

namespace RVX
{

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

void DepthPrepass::SetDepthTarget(RHITextureView* depthView)
{
    m_depthTargetView = depthView;
}

void DepthPrepass::Setup(RenderGraphBuilder& builder, const ViewData& view)
{
    // Declare depth buffer write (no color output)
    if (view.depthTarget.IsValid())
    {
        builder.SetDepthStencil(view.depthTarget, true, false);
    }
}

void DepthPrepass::Execute(RHICommandContext& ctx, const ViewData& view)
{
    if (!m_pipelineCache || !m_renderScene || (!m_opaqueDrawItems && !m_maskedDrawItems) || !m_depthTargetView)
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
    rpDesc.SetDepthStencil(m_depthTargetView, RHILoadOp::Clear, RHIStoreOp::Store, 1.0f, 0);

    ctx.BeginRenderPass(rpDesc);

    // Set viewport and scissor
    ctx.SetViewport(view.GetRHIViewport());
    ctx.SetScissor(view.GetRHIScissor());

    // Bind depth-only pipeline
    ctx.SetPipeline(depthPipeline);

    // Draw all visible opaque objects with depth-only shader
    if (m_gpuResources)
    {
        const auto drawItem = [&](const RenderDrawItem& item)
        {
            if (item.objectIndex >= m_renderScene->GetObjectCount())
                return;

            const RenderObject& obj = m_renderScene->GetObject(item.objectIndex);

            // Get GPU buffers for this mesh
            MeshGPUBuffers buffers = m_gpuResources->GetMeshBuffers(obj.meshId);
            if (!buffers.IsValid())
            {
                return;  // Mesh not uploaded yet
            }

            // Update per-object constants (world matrix)
            m_pipelineCache->UpdateObjectConstants(obj.worldMatrix);

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

            // Bind index buffer
            ctx.SetIndexBuffer(buffers.indexBuffer, RHIFormat::R32_UINT);

            if (item.submeshIndex >= buffers.submeshes.size())
            {
                return;
            }

            const SubmeshGPUInfo& submesh = buffers.submeshes[item.submeshIndex];
            ctx.DrawIndexed(submesh.indexCount, 1,
                            submesh.indexOffset, submesh.baseVertex, 0);
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
