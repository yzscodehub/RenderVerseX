/**
 * @file DepthPrepass.cpp
 * @brief DepthPrepass implementation
 */

#include "Render/Passes/DepthPrepass.h"
#include "Render/Renderer/ViewData.h"
#include "Render/Renderer/RenderScene.h"
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

void DepthPrepass::SetRenderScene(const RenderScene* scene, const std::vector<uint32_t>* visibleIndices)
{
    m_renderScene = scene;
    m_visibleIndices = visibleIndices;
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
    if (!m_pipelineCache || !m_renderScene || !m_visibleIndices || !m_depthTargetView)
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

    // Bind view constants descriptor set
    RHIDescriptorSet* viewSet = m_pipelineCache->GetViewDescriptorSet();
    if (viewSet)
    {
        ctx.SetDescriptorSet(0, viewSet);
    }

    // Draw all visible opaque objects with depth-only shader
    if (m_gpuResources)
    {
        for (uint32_t idx : *m_visibleIndices)
        {
            if (idx >= m_renderScene->GetObjectCount())
                continue;

            const RenderObject& obj = m_renderScene->GetObject(idx);

            // Get GPU buffers for this mesh
            MeshGPUBuffers buffers = m_gpuResources->GetMeshBuffers(obj.meshId);
            if (!buffers.IsValid())
            {
                continue;  // Mesh not uploaded yet
            }

            // Update per-object constants (world matrix)
            m_pipelineCache->UpdateObjectConstants(obj.worldMatrix);

            // Bind vertex buffers - only position is needed for depth
            ctx.SetVertexBuffer(0, buffers.positionBuffer);  // Slot 0: Position

            // Bind index buffer
            ctx.SetIndexBuffer(buffers.indexBuffer, RHIFormat::R32_UINT);

            // Draw each submesh
            for (const SubmeshGPUInfo& submesh : buffers.submeshes)
            {
                ctx.DrawIndexed(submesh.indexCount, 1, 
                               submesh.indexOffset, submesh.baseVertex, 0);
            }
        }
    }

    ctx.EndRenderPass();
}

} // namespace RVX
