/**
 * @file OpaquePass.cpp
 * @brief Opaque geometry render pass implementation
 */

#include "Render/Passes/OpaquePass.h"
#include "Core/Log.h"
#include "Render/GPUResourceManager.h"
#include "Render/Material/MaterialSystem.h"
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
    const Resource::MaterialResource* ResolveMaterialResource(const RenderObject& obj, size_t submeshIndex)
    {
        if (submeshIndex >= obj.materialResources.size())
            return nullptr;

        return obj.materialResources[submeshIndex];
    }

    void TransitionVisibleMaterialTextures(const std::vector<RenderDrawItem>& drawItems,
                                           GPUResourceManager& gpuResources, RHICommandContext& ctx)
    {
        const auto transitionTexture = [&gpuResources, &ctx](
                                           Resource::ResourceHandle<Resource::TextureResource> textureHandle)
        {
            Resource::TextureResource* textureResource = textureHandle.Get();
            if (!textureResource || !gpuResources.IsGPUReady(textureResource->GetId()))
                return;

            gpuResources.TransitionTexture(textureResource->GetId(), ctx, RHIResourceState::ShaderResource);
        };

        for (const RenderDrawItem& item : drawItems)
        {
            const Resource::MaterialResource* material = item.materialResource;
            if (!material)
                continue;

            transitionTexture(material->GetAlbedoTexture());
            transitionTexture(material->GetNormalTexture());
            transitionTexture(material->GetMetallicRoughnessTexture());
            transitionTexture(material->GetAOTexture());
            transitionTexture(material->GetEmissiveTexture());
        }
    }

    template <typename DrawFunc>
    void ForEachOpaqueDrawItem(const std::vector<RenderDrawItem>* opaqueDrawItems,
                               const std::vector<RenderDrawItem>* maskedDrawItems,
                               DrawFunc&& drawFunc)
    {
        if (opaqueDrawItems)
        {
            for (const RenderDrawItem& item : *opaqueDrawItems)
            {
                drawFunc(item);
            }
        }

        if (maskedDrawItems)
        {
            for (const RenderDrawItem& item : *maskedDrawItems)
            {
                drawFunc(item);
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
    m_materialSystem = nullptr;
    m_renderScene = nullptr;
    m_opaqueDrawItems = nullptr;
    m_maskedDrawItems = nullptr;
}

void OpaquePass::SetResources(GPUResourceManager* gpuMgr, PipelineCache* pipelines, MaterialSystem* materialSystem)
{
    m_gpuResources = gpuMgr;
    m_pipelineCache = pipelines;
    m_materialSystem = materialSystem;
}

void OpaquePass::SetRenderScene(const RenderScene* scene,
                                const std::vector<RenderDrawItem>* opaqueDrawItems,
                                const std::vector<RenderDrawItem>* maskedDrawItems)
{
    m_renderScene = scene;
    m_opaqueDrawItems = opaqueDrawItems;
    m_maskedDrawItems = maskedDrawItems;
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

    if (!m_materialSystem || !m_materialSystem->IsInitialized())
    {
        RVX_CORE_WARN("OpaquePass: MaterialSystem not available");
        return;
    }

    if (!m_colorTargetView)
    {
        RVX_CORE_WARN("OpaquePass: No color target view set");
        return;
    }

    if (m_gpuResources)
    {
        if (m_opaqueDrawItems)
            TransitionVisibleMaterialTextures(*m_opaqueDrawItems, *m_gpuResources, ctx);
        if (m_maskedDrawItems)
            TransitionVisibleMaterialTextures(*m_maskedDrawItems, *m_gpuResources, ctx);
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

    // 4. Bind frame constants descriptor set
    RHIDescriptorSet* frameSet = m_pipelineCache->GetFrameDescriptorSet();
    if (frameSet)
    {
        ctx.SetDescriptorSet(0, frameSet);
    }

    // 5. Draw each visible object
    if (m_renderScene && m_gpuResources)
    {
        ForEachOpaqueDrawItem(
            m_opaqueDrawItems, m_maskedDrawItems,
            [&](const RenderDrawItem& item)
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
            RHIDescriptorSet* objectSet = m_pipelineCache->GetObjectDescriptorSet();
            if (objectSet)
            {
                const auto objectDynamicOffsets = m_pipelineCache->GetCurrentObjectDynamicOffset();
                ctx.SetDescriptorSet(1, objectSet, objectDynamicOffsets);
            }

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

            if (buffers.tangentBuffer)
            {
                ctx.SetVertexBuffer(3, buffers.tangentBuffer);  // Slot 3: Tangent
            }

            // Bind index buffer
            ctx.SetIndexBuffer(buffers.indexBuffer, RHIFormat::R32_UINT);

            if (item.submeshIndex >= buffers.submeshes.size())
            {
                return;
            }

            const SubmeshGPUInfo& submesh = buffers.submeshes[item.submeshIndex];
            const Resource::MaterialResource* materialResource =
                item.materialResource ? item.materialResource : ResolveMaterialResource(obj, item.submeshIndex);
            m_materialSystem->UpdateMaterialConstants(materialResource, view.viewCache);

            RHIDescriptorSet* materialSet = m_materialSystem->GetOrCreateMaterialSet(materialResource, view.viewCache);
            if (materialSet)
            {
                const auto materialDynamicOffsets = m_materialSystem->GetCurrentMaterialDynamicOffset();
                ctx.SetDescriptorSet(2, materialSet, materialDynamicOffsets);
            }

            ctx.DrawIndexed(submesh.indexCount, 1,
                            submesh.indexOffset, submesh.baseVertex, 0);
        });
    }
    else
    {
        // Log why we're not drawing
        if (!m_renderScene) RVX_CORE_DEBUG("OpaquePass: No render scene");
        if (!m_opaqueDrawItems && !m_maskedDrawItems) RVX_CORE_DEBUG("OpaquePass: No draw items");
        if (!m_gpuResources) RVX_CORE_DEBUG("OpaquePass: No GPU resources");
    }

    // 6. End render pass
    ctx.EndRenderPass();
}

} // namespace RVX
