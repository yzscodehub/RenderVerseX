/**
 * @file OpaquePass.cpp
 * @brief Opaque geometry render pass implementation
 */

#include "Render/Passes/OpaquePass.h"
#include "Render/Renderer/ViewData.h"
#include "Render/Renderer/RenderScene.h"
#include "Render/GPUResourceManager.h"
#include "Render/PipelineCache.h"
#include "RHI/RHIRenderPass.h"
#include "Core/Log.h"

namespace RVX
{

void OpaquePass::OnAdd(IRHIDevice* device)
{
    m_device = device;
    RVX_CORE_DEBUG("OpaquePass added");
}

void OpaquePass::OnRemove()
{
    RVX_CORE_DEBUG("OpaquePass removed");
    m_device = nullptr;
    m_gpuResources = nullptr;
    m_pipelineCache = nullptr;
    m_renderScene = nullptr;
    m_visibleIndices = nullptr;
}

void OpaquePass::SetResources(GPUResourceManager* gpuMgr, PipelineCache* pipelines)
{
    m_gpuResources = gpuMgr;
    m_pipelineCache = pipelines;
}

void OpaquePass::SetRenderScene(const RenderScene* scene, const std::vector<uint32_t>* visibleIndices)
{
    m_renderScene = scene;
    m_visibleIndices = visibleIndices;
}

void OpaquePass::SetRenderTargets(RHITextureView* colorTargetView, RHITextureView* depthTargetView)
{
    m_colorTargetView = colorTargetView;
    m_depthTargetView = depthTargetView;
}

void OpaquePass::Setup(RenderGraphBuilder& builder, const ViewData& view)
{
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
}

void OpaquePass::Execute(RHICommandContext& ctx, const ViewData& view)
{
    static bool firstFrame = true;
    if (firstFrame)
    {
        RVX_CORE_INFO("OpaquePass::Execute - First frame diagnostics:");
        RVX_CORE_INFO("  m_pipelineCache: {}", m_pipelineCache ? "valid" : "null");
        RVX_CORE_INFO("  m_gpuResources: {}", m_gpuResources ? "valid" : "null");
        RVX_CORE_INFO("  m_renderScene: {}", m_renderScene ? "valid" : "null");
        RVX_CORE_INFO("  m_visibleIndices: {}", m_visibleIndices ? "valid" : "null");
        RVX_CORE_INFO("  m_colorTargetView: {}", m_colorTargetView ? "valid" : "null");
        RVX_CORE_INFO("  m_depthTargetView: {}", m_depthTargetView ? "valid" : "null");
        
        if (m_pipelineCache)
        {
            RVX_CORE_INFO("  PipelineCache initialized: {}", m_pipelineCache->IsInitialized());
            RVX_CORE_INFO("  OpaquePipeline: {}", m_pipelineCache->GetOpaquePipeline() ? "valid" : "null");
        }
        
        if (m_renderScene)
        {
            RVX_CORE_INFO("  RenderScene object count: {}", m_renderScene->GetObjectCount());
        }
        
        if (m_visibleIndices)
        {
            RVX_CORE_INFO("  Visible indices count: {}", m_visibleIndices->size());
        }
        
        RVX_CORE_INFO("  Viewport: {}x{}", view.viewportWidth, view.viewportHeight);
        firstFrame = false;
    }

    // Validate dependencies
    if (!m_pipelineCache || !m_pipelineCache->IsInitialized())
    {
        RVX_CORE_WARN("OpaquePass: PipelineCache not available (initialized: {})", 
                      m_pipelineCache ? m_pipelineCache->IsInitialized() : false);
        return;
    }

    if (!m_colorTargetView)
    {
        RVX_CORE_WARN("OpaquePass: No color target view set");
        return;
    }

    // 1. Begin render pass using builder pattern
    RHIRenderPassDesc rpDesc;
    rpDesc.AddColorAttachment(m_colorTargetView, RHILoadOp::Clear, RHIStoreOp::Store,
                              {0.1f, 0.1f, 0.15f, 1.0f});

    if (m_depthTargetView)
    {
        rpDesc.SetDepthStencil(m_depthTargetView, RHILoadOp::Clear, RHIStoreOp::Store,
                               1.0f, 0);
    }

    ctx.BeginRenderPass(rpDesc);

    // 2. Set viewport and scissor
    ctx.SetViewport(view.GetRHIViewport());
    ctx.SetScissor(view.GetRHIScissor());

    // 3. Bind pipeline
    RHIPipeline* pipeline = m_pipelineCache->GetOpaquePipeline();
    if (!pipeline)
    {
        RVX_CORE_WARN("OpaquePass: No opaque pipeline available");
        ctx.EndRenderPass();
        return;
    }
    ctx.SetPipeline(pipeline);

    // 4. Bind view/object constants descriptor set
    RHIDescriptorSet* viewSet = m_pipelineCache->GetViewDescriptorSet();
    if (viewSet)
    {
        ctx.SetDescriptorSet(0, viewSet);
    }

    // 5. Draw each visible object
    if (m_renderScene && m_visibleIndices && m_gpuResources)
    {
        uint32_t drawCalls = 0;
        uint32_t skippedNotResident = 0;
        
        for (uint32_t idx : *m_visibleIndices)
        {
            const RenderObject& obj = m_renderScene->GetObject(idx);
            
            // Get GPU buffers for this mesh
            MeshGPUBuffers buffers = m_gpuResources->GetMeshBuffers(obj.meshId);
            if (!buffers.IsValid())
            {
                skippedNotResident++;
                continue;  // Mesh not uploaded yet
            }

            // Update per-object constants (world matrix)
            m_pipelineCache->UpdateObjectConstants(obj.worldMatrix);

            // Bind SEPARATE vertex buffers to different slots
            ctx.SetVertexBuffer(0, buffers.positionBuffer);  // Slot 0: Position
            
            if (buffers.normalBuffer)
            {
                ctx.SetVertexBuffer(1, buffers.normalBuffer);  // Slot 1: Normal
            }
            
            if (buffers.uvBuffer)
            {
                ctx.SetVertexBuffer(2, buffers.uvBuffer);  // Slot 2: UV
            }

            // Bind index buffer
            ctx.SetIndexBuffer(buffers.indexBuffer, RHIFormat::R32_UINT);

            // Draw each submesh
            for (const SubmeshGPUInfo& submesh : buffers.submeshes)
            {
                ctx.DrawIndexed(submesh.indexCount, 1, 
                               submesh.indexOffset, submesh.baseVertex, 0);
                drawCalls++;
            }
        }

        // Per-frame stats (disabled to reduce log spam)
        // RVX_CORE_DEBUG("OpaquePass: {} draw calls, {} skipped (not resident)", 
        //               drawCalls, skippedNotResident);
        (void)drawCalls;
        (void)skippedNotResident;
    }
    else
    {
        // Log why we're not drawing
        if (!m_renderScene) RVX_CORE_DEBUG("OpaquePass: No render scene");
        if (!m_visibleIndices) RVX_CORE_DEBUG("OpaquePass: No visible indices");
        if (!m_gpuResources) RVX_CORE_DEBUG("OpaquePass: No GPU resources");
    }

    // 6. End render pass
    ctx.EndRenderPass();
}

} // namespace RVX
