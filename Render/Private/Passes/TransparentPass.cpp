/**
 * @file TransparentPass.cpp
 * @brief TransparentPass implementation
 */

#include "Render/Passes/TransparentPass.h"
#include "Render/Renderer/ViewData.h"
#include "Render/Renderer/RenderScene.h"
#include "Render/GPUResourceManager.h"
#include "Render/PipelineCache.h"
#include "RHI/RHIRenderPass.h"
#include "Core/Log.h"

namespace RVX
{

TransparentPass::TransparentPass()
{
    // Transparent pass is enabled by default but requires transparent objects to render
}

void TransparentPass::SetResources(GPUResourceManager* gpuResources, PipelineCache* pipelineCache)
{
    m_gpuResources = gpuResources;
    m_pipelineCache = pipelineCache;
}

void TransparentPass::SetRenderScene(const RenderScene* scene, const std::vector<uint32_t>* transparentIndices)
{
    m_renderScene = scene;
    m_transparentIndices = transparentIndices;
}

void TransparentPass::SetRenderTargets(RHITextureView* colorTargetView, RHITextureView* depthTargetView)
{
    m_colorTargetView = colorTargetView;
    m_depthTargetView = depthTargetView;
}

void TransparentPass::Setup(RenderGraphBuilder& builder, const ViewData& view)
{
    // Read-write color target (we blend onto it)
    if (view.colorTarget.IsValid())
    {
        m_colorTargetHandle = builder.Write(view.colorTarget, RHIResourceState::RenderTarget);
    }

    // Read-only depth (we test against but don't write)
    if (view.depthTarget.IsValid())
    {
        builder.SetDepthStencil(view.depthTarget, false, false);  // Read-only depth
        m_depthTargetHandle = view.depthTarget;
    }
}

void TransparentPass::Execute(RHICommandContext& ctx, const ViewData& view)
{
    // Skip if no transparent objects
    if (!m_transparentIndices || m_transparentIndices->empty())
    {
        return;
    }

    if (!m_pipelineCache || !m_pipelineCache->IsInitialized())
    {
        return;
    }

    if (!m_colorTargetView)
    {
        return;
    }

    // Begin render pass with alpha blending
    RHIRenderPassDesc rpDesc;
    rpDesc.AddColorAttachment(m_colorTargetView, RHILoadOp::Load, RHIStoreOp::Store,
                              {0.0f, 0.0f, 0.0f, 0.0f});  // Load existing content

    if (m_depthTargetView)
    {
        // Depth read-only: Load existing depth, don't store (or store if other passes need it)
        rpDesc.SetDepthStencil(m_depthTargetView, RHILoadOp::Load, RHIStoreOp::Store, 1.0f, 0);
    }

    ctx.BeginRenderPass(rpDesc);

    // Set viewport and scissor
    ctx.SetViewport(view.GetRHIViewport());
    ctx.SetScissor(view.GetRHIScissor());

    // Bind transparent pipeline (should have alpha blending enabled)
    // For now, use opaque pipeline - transparent pipeline would have:
    // - BlendState: SrcAlpha, InvSrcAlpha
    // - DepthWrite: false
    RHIPipeline* pipeline = m_pipelineCache->GetOpaquePipeline();
    if (!pipeline)
    {
        ctx.EndRenderPass();
        return;
    }
    ctx.SetPipeline(pipeline);

    // Bind view constants descriptor set
    RHIDescriptorSet* viewSet = m_pipelineCache->GetViewDescriptorSet();
    if (viewSet)
    {
        ctx.SetDescriptorSet(0, viewSet);
    }

    // Draw transparent objects in back-to-front order
    // (Caller is responsible for sorting m_transparentIndices)
    if (m_renderScene && m_gpuResources)
    {
        for (uint32_t idx : *m_transparentIndices)
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

            // Bind vertex buffers
            ctx.SetVertexBuffer(0, buffers.positionBuffer);
            
            if (buffers.normalBuffer)
            {
                ctx.SetVertexBuffer(1, buffers.normalBuffer);
            }
            
            if (buffers.uvBuffer)
            {
                ctx.SetVertexBuffer(2, buffers.uvBuffer);
            }

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
