/**
 * @file TransparentPass.cpp
 * @brief TransparentPass implementation
 */

#include "Render/Passes/TransparentPass.h"
#include "Core/Log.h"
#include "Render/GPUResourceManager.h"
#include "Resources/RenderResourceResolver.h"
#include "Render/Graph/ResourceViewCache.h"
#include "Render/Lighting/ClusteredLighting.h"
#include "Render/Lighting/LightManager.h"
#include "Render/Material/MaterialSystem.h"
#include "Render/PipelineCache.h"
#include "Render/Renderer/RenderDrawItem.h"
#include "Render/Renderer/RenderScene.h"
#include "Render/Renderer/ViewData.h"
#include "RHI/RHIRenderPass.h"

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

TransparentPass::TransparentPass()
{
    // Transparent pass is enabled by default but requires transparent objects to render
}

namespace
{
    const IRenderMaterialSource* ResolveMaterialResource(const RenderObject& obj, size_t submeshIndex)
    {
        if (submeshIndex >= obj.materialResources.size())
            return nullptr;

        return obj.materialResources[submeshIndex];
    }
} // namespace

void TransparentPass::SetResources(GPUResourceManager* gpuResources,
                                   PipelineCache* pipelineCache,
                                   MaterialSystem* materialSystem,
                                   LightManager* lightManager,
                                   ClusteredLighting* clusteredLighting)
{
    m_gpuResources = gpuResources;
    m_pipelineCache = pipelineCache;
    m_materialSystem = materialSystem;
    m_lightManager = lightManager;
    m_clusteredLighting = clusteredLighting;
}
void TransparentPass::SetRenderScene(const RenderScene* scene, const std::vector<RenderDrawItem>* transparentDrawItems)
{
    m_renderScene = scene;
    m_transparentDrawItems = transparentDrawItems;
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
    if (!m_transparentDrawItems || m_transparentDrawItems->empty())
    {
        return;
    }

    if (!m_pipelineCache || !m_pipelineCache->IsInitialized() || !m_materialSystem || !m_materialSystem->IsInitialized())
    {
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
        return;
    }

    RHITextureView* depthTargetView = m_depthTargetView;
    if (view.renderGraph && view.viewCache && m_depthTargetHandle.IsValid())
    {
        if (RHITexture* depthTarget = view.renderGraph->GetTexture(m_depthTargetHandle))
        {
            depthTargetView = view.viewCache->GetDefaultDSV(depthTarget);
        }
    }

    // RQ3a keeps directional shadow sampling scoped to OpaquePass. Transparent
    // shading reuses DefaultLit, so reset the frame shadow state explicitly
    // before binding the frame descriptor for transparent draws.
    ViewData transparentView = view;
    transparentView.directionalShadowEnabled = 0;
    transparentView.directionalShadowCascadeCount = 0;
    transparentView.directionalShadowCascadeSplits = Vec4(0.0f, 0.0f, 0.0f, 0.0f);
    transparentView.directionalShadowCascadeFadeDistances = Vec4(0.0f, 0.0f, 0.0f, 0.0f);
    transparentView.directionalShadowNormalBias = 0.0f;
    m_pipelineCache->UpdateDirectionalShadowFrameResources({});
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
    m_pipelineCache->UpdateViewConstants(transparentView);

    // Begin render pass with alpha blending
    RHIRenderPassDesc rpDesc;
    rpDesc.AddColorAttachment(colorTargetView, RHILoadOp::Load, RHIStoreOp::Store,
                              {0.0f, 0.0f, 0.0f, 0.0f});  // Load existing content

    if (depthTargetView)
    {
        // Depth read-only: Load existing depth, don't store (or store if other passes need it)
        rpDesc.SetDepthStencil(depthTargetView, RHILoadOp::Load, RHIStoreOp::Store, 1.0f, 0);
    }

    ctx.BeginRenderPass(rpDesc);

    // Set viewport and scissor
    ctx.SetViewport(view.GetRHIViewport());
    ctx.SetScissor(view.GetRHIScissor());

    RHIPipeline* pipeline =
        m_pipelineCache->GetPipelineForVariant(MaterialPipelineVariant::Transparent, colorTargetFormat);
    if (!pipeline)
    {
        RVX_CORE_WARN("TransparentPass: Missing transparent pipeline; skipping {} draw items",
                      m_transparentDrawItems->size());
        ctx.EndRenderPass();
        return;
    }
    ctx.SetPipeline(pipeline);

    RHIDescriptorSet* frameSet = m_pipelineCache->GetFrameDescriptorSet();
    if (frameSet)
    {
        ctx.SetDescriptorSet(0, frameSet);
    }

    // Draw transparent submeshes in back-to-front order.
    if (m_renderScene &&
        (m_resourceRegistry != nullptr || m_gpuResources != nullptr))
    {
        for (const RenderDrawItem& item : *m_transparentDrawItems)
        {
            if (item.objectIndex >= m_renderScene->GetObjectCount())
                continue;

            const RenderObject& obj = m_renderScene->GetObject(item.objectIndex);

            // Get GPU buffers for this mesh
            MeshGPUBuffers buffers = ResolveRenderMeshBuffers(
                m_resourceRegistry,
                m_gpuResources,
                obj.mesh,
                obj.meshId);
            if (!buffers.IsValid())
            {
                continue;  // Mesh not uploaded yet
            }

            // Update per-object constants
            m_pipelineCache->UpdateObjectConstants(obj.worldMatrix,
                                                   obj.normalMatrix,
                                                   obj.previousWorldMatrix,
                                                   view.previousViewProjectionMatrix,
                                                   obj.previousWorldMatrixValid != 0 &&
                                                       view.previousViewProjectionValid != 0 &&
                                                       !view.resetTemporalHistory,
                                                   obj.receivesShadow,
                                                   ResolveSkinningMatrices(obj, buffers));
            RHIDescriptorSet* objectSet = m_pipelineCache->GetObjectDescriptorSet();
            if (objectSet)
            {
                const auto objectDynamicOffsets = m_pipelineCache->GetCurrentObjectDynamicOffset();
                ctx.SetDescriptorSet(1, objectSet, objectDynamicOffsets);
            }

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

            if (buffers.tangentBuffer)
            {
                ctx.SetVertexBuffer(3, buffers.tangentBuffer);
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
                continue;
            }

            const SubmeshGPUInfo& submesh = buffers.submeshes[item.submeshIndex];
            const IRenderMaterialSource* materialResource =
                item.materialResource ? item.materialResource : ResolveMaterialResource(obj, item.submeshIndex);
            MaterialBindingOptions materialOptions;
            materialOptions.allowNormalMap = buffers.HasNormalMapTangentBasis();
            const MaterialBindingResult materialBinding =
                m_resourceRegistry
                    ? m_materialSystem->PrepareMaterialBinding(
                          item.material, view.viewCache, materialOptions)
                    : m_materialSystem->PrepareMaterialBinding(
                          materialResource, view.viewCache, materialOptions);
            if (!materialBinding.IsDrawable())
            {
                RVX_CORE_WARN("TransparentPass: Skipping draw item because material binding failed: {}",
                              materialBinding.message);
                continue;
            }

            ctx.SetDescriptorSet(2, materialBinding.descriptorSet, materialBinding.dynamicOffsets);

            ctx.DrawIndexed(submesh.indexCount, 1,
                            submesh.indexOffset, submesh.baseVertex, 0);
        }
    }

    ctx.EndRenderPass();
}

} // namespace RVX
