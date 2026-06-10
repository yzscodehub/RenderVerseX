/**
 * @file OpaquePass.cpp
 * @brief Opaque geometry render pass implementation
 */

#include "Render/Passes/OpaquePass.h"
#include "Core/Log.h"
#include "Render/GPUResourceManager.h"
#include "Render/Graph/ResourceViewCache.h"
#include "Render/Material/MaterialSystem.h"
#include "Render/PipelineCache.h"
#include "Render/Passes/ShadowPass.h"
#include "Render/Renderer/RenderScene.h"
#include "Render/Renderer/ViewData.h"
#include "Resource/Types/MaterialResource.h"
#include "Resource/Types/TextureResource.h"
#include "RHI/RHIRenderPass.h"

#include <algorithm>
#include <cmath>

namespace RVX
{

namespace
{
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
    m_shadowPass = nullptr;
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

void OpaquePass::SetDirectionalShadowSource(const ShadowPass* shadowPass)
{
    m_shadowPass = shadowPass;
}

void OpaquePass::SetRenderTargets(RHITextureView* colorTargetView, RHITextureView* depthTargetView)
{
    m_colorTargetView = colorTargetView;
    m_depthTargetView = depthTargetView;
}

void OpaquePass::Setup(RenderGraphBuilder& builder, const ViewData& view)
{
    m_directionalShadowReadHandle = {};
    m_shadowStats = {};

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
        return;
    }

    if (m_gpuResources)
    {
        if (m_opaqueDrawItems)
            TransitionVisibleMaterialTextures(*m_opaqueDrawItems, *m_gpuResources, ctx);
        if (m_maskedDrawItems)
            TransitionVisibleMaterialTextures(*m_maskedDrawItems, *m_gpuResources, ctx);
    }

    ViewData drawView = view;
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
    m_pipelineCache->UpdateViewConstants(drawView);

    // 1. Begin render pass using builder pattern
    RHIRenderPassDesc rpDesc;
    rpDesc.AddColorAttachment(colorTargetView, RHILoadOp::Clear, RHIStoreOp::Store,
                              {0.1f, 0.1f, 0.15f, 1.0f});

    if (m_depthTargetView)
    {
        const float clearDepth = m_pipelineCache ? m_pipelineCache->GetDepthClearValue()
                                                 : PipelineCache::GetDepthClearValue(false);
        rpDesc.SetDepthStencil(m_depthTargetView, RHILoadOp::Clear, RHIStoreOp::Store,
                               clearDepth, 0);
    }

    ctx.BeginRenderPass(rpDesc);

    // 2. Set viewport and scissor
    ctx.SetViewport(view.GetRHIViewport());
    ctx.SetScissor(view.GetRHIScissor());

    // 3. Cache frame constants descriptor set. DX12 needs descriptor sets bound after the pipeline root signature.
    RHIDescriptorSet* frameSet = m_pipelineCache->GetFrameDescriptorSet();

    // 4. Draw each visible object group with its material variant pipeline.
    if (m_renderScene && m_gpuResources)
    {
        const auto drawGroup = [&](const std::vector<RenderDrawItem>* drawItems,
                                   MaterialPipelineVariant variant,
                                   const char* groupName)
        {
            if (!drawItems || drawItems->empty())
                return;

            RHIPipeline* pipeline = m_pipelineCache->GetPipelineForVariant(variant, colorTargetFormat);
            if (!pipeline)
            {
                RVX_CORE_WARN("OpaquePass: Missing {} pipeline; skipping {} draw items",
                              groupName, drawItems->size());
                return;
            }

            ctx.SetPipeline(pipeline);
            if (frameSet)
            {
                ctx.SetDescriptorSet(0, frameSet);
            }

            for (const RenderDrawItem& item : *drawItems)
            {
                if (item.objectIndex >= m_renderScene->GetObjectCount())
                    continue;

                const RenderObject& obj = m_renderScene->GetObject(item.objectIndex);

                // Get GPU buffers for this mesh
                MeshGPUBuffers buffers = m_gpuResources->GetMeshBuffers(obj.meshId);
                if (!buffers.IsValid())
                {
                    continue;  // Mesh not uploaded yet
                }

                // Update per-object constants
                m_pipelineCache->UpdateObjectConstants(obj.worldMatrix, obj.normalMatrix);
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
                    continue;
                }

                const SubmeshGPUInfo& submesh = buffers.submeshes[item.submeshIndex];
                const Resource::MaterialResource* materialResource =
                    item.materialResource ? item.materialResource : ResolveMaterialResource(obj, item.submeshIndex);
                MaterialBindingOptions materialOptions;
                materialOptions.allowNormalMap = buffers.HasNormalMapTangentBasis();
                const MaterialBindingResult materialBinding =
                    m_materialSystem->PrepareMaterialBinding(materialResource, view.viewCache, materialOptions);
                if (!materialBinding.IsDrawable())
                {
                    RVX_CORE_WARN("OpaquePass: Skipping {} draw item because material binding failed: {}",
                                  groupName,
                                  materialBinding.message);
                    continue;
                }

                ctx.SetDescriptorSet(2, materialBinding.descriptorSet, materialBinding.dynamicOffsets);

                ctx.DrawIndexed(submesh.indexCount, 1,
                                submesh.indexOffset, submesh.baseVertex, 0);
            }
        };

        drawGroup(m_opaqueDrawItems, MaterialPipelineVariant::Opaque, "opaque");
        drawGroup(m_maskedDrawItems, MaterialPipelineVariant::Masked, "masked");
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
