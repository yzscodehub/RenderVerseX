/**
 * @file OpaquePass.cpp
 * @brief Opaque geometry render pass implementation
 */

#include "Render/Passes/OpaquePass.h"
#include "Core/Log.h"
#include "Render/GPUResourceManager.h"
#include "Render/Graph/ResourceViewCache.h"
#include "Render/PipelineCache.h"
#include "Render/Renderer/RenderScene.h"
#include "Render/Renderer/ViewData.h"
#include "Resource/Types/MaterialResource.h"
#include "Resource/Types/TextureResource.h"
#include "RHI/RHIRenderPass.h"

namespace RVX
{

namespace
{
    RHITextureView* ResolveBaseColorTextureView(const RenderObject& obj, size_t submeshIndex,
                                                GPUResourceManager& gpuResources, ResourceViewCache* viewCache)
    {
        if (!viewCache || submeshIndex >= obj.materialResources.size())
            return nullptr;

        const Resource::MaterialResource* material = obj.materialResources[submeshIndex];
        if (!material)
            return nullptr;

        Resource::ResourceHandle<Resource::TextureResource> albedoTexture = material->GetAlbedoTexture();
        Resource::TextureResource* textureResource = albedoTexture.Get();
        if (!textureResource || !gpuResources.IsGPUReady(textureResource->GetId()))
            return nullptr;

        RHITexture* texture = gpuResources.GetTexture(textureResource->GetId());
        if (!texture)
            return nullptr;

        return viewCache->GetDefaultSRV(texture);
    }

    Vec4 ResolveBaseColorFactor(const RenderObject& obj, size_t submeshIndex)
    {
        if (submeshIndex >= obj.materialResources.size())
            return Vec4(1.0f, 1.0f, 1.0f, 1.0f);

        const Resource::MaterialResource* materialResource = obj.materialResources[submeshIndex];
        if (!materialResource || !materialResource->GetMaterial())
            return Vec4(1.0f, 1.0f, 1.0f, 1.0f);

        return materialResource->GetMaterial()->GetBaseColor();
    }

    void TransitionVisibleMaterialTextures(const RenderScene& renderScene, const std::vector<uint32_t>& visibleIndices,
                                           GPUResourceManager& gpuResources, RHICommandContext& ctx)
    {
        for (uint32_t idx : visibleIndices)
        {
            const RenderObject& obj = renderScene.GetObject(idx);
            for (const Resource::MaterialResource* material : obj.materialResources)
            {
                if (!material)
                    continue;

                Resource::ResourceHandle<Resource::TextureResource> albedoTexture = material->GetAlbedoTexture();
                Resource::TextureResource* textureResource = albedoTexture.Get();
                if (!textureResource || !gpuResources.IsGPUReady(textureResource->GetId()))
                    continue;

                gpuResources.TransitionTexture(textureResource->GetId(), ctx, RHIResourceState::ShaderResource);
            }
        }
    }
} // namespace

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

    if (m_renderScene && m_visibleIndices && m_gpuResources)
    {
        TransitionVisibleMaterialTextures(*m_renderScene, *m_visibleIndices, *m_gpuResources, ctx);
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
        const auto dynamicOffsets = m_pipelineCache->GetCurrentConstantDynamicOffsets();
        ctx.SetDescriptorSet(0, viewSet, dynamicOffsets);
    }

    // 5. Draw each visible object
    if (m_renderScene && m_visibleIndices && m_gpuResources)
    {
        for (uint32_t idx : *m_visibleIndices)
        {
            const RenderObject& obj = m_renderScene->GetObject(idx);
            
            // Get GPU buffers for this mesh
            MeshGPUBuffers buffers = m_gpuResources->GetMeshBuffers(obj.meshId);
            if (!buffers.IsValid())
            {
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
            for (size_t submeshIndex = 0; submeshIndex < buffers.submeshes.size(); ++submeshIndex)
            {
                const SubmeshGPUInfo& submesh = buffers.submeshes[submeshIndex];
                m_pipelineCache->UpdateMaterialConstants(ResolveBaseColorFactor(obj, submeshIndex));

                RHITextureView* baseColorView = ResolveBaseColorTextureView(obj, submeshIndex, *m_gpuResources, view.viewCache);
                RHIDescriptorSet* materialSet = view.viewCache
                    ? m_pipelineCache->GetMaterialDescriptorSet(baseColorView, view.viewCache->GetGeneration())
                    : m_pipelineCache->GetMaterialDescriptorSet(baseColorView);
                if (materialSet)
                {
                    const auto dynamicOffsets = m_pipelineCache->GetCurrentConstantDynamicOffsets();
                    ctx.SetDescriptorSet(0, materialSet, dynamicOffsets);
                }

                ctx.DrawIndexed(submesh.indexCount, 1, 
                               submesh.indexOffset, submesh.baseVertex, 0);
            }
        }
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
