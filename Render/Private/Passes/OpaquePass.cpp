/**
 * @file OpaquePass.cpp
 * @brief Opaque geometry render pass implementation
 */

#include "Render/Passes/OpaquePass.h"
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

} // namespace

void OpaquePass::OnAdd(IRHIDevice* device)
{
    m_device = device;
    RVX_CORE_DEBUG("OpaquePass added");
}

void OpaquePass::OnRemove()
{
    RVX_CORE_DEBUG("OpaquePass removed");
    m_indirectDrawBuffer.Reset();
    m_indirectDrawBufferCapacity = 0;
    m_indirectDrawCommands.clear();
    m_device = nullptr;
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

uint32 OpaquePass::FindIndirectBatchLength(const std::vector<RenderDrawItem>& drawItems,
                                           size_t startIndex) const
{
    if (startIndex >= drawItems.size())
    {
        return 0;
    }

    const RenderDrawItem& first = drawItems[startIndex];
    uint32 count = 1;
    for (size_t i = startIndex + 1; i < drawItems.size(); ++i)
    {
        const RenderDrawItem& item = drawItems[i];
        if (item.objectIndex != first.objectIndex ||
            item.mesh != first.mesh ||
            item.material != first.material)
        {
            break;
        }

        ++count;
    }

    return count;
}

bool OpaquePass::EnsureIndirectDrawCapacity(uint32 commandCount)
{
    if (commandCount == 0)
    {
        return true;
    }

    if (m_indirectDrawBuffer && m_indirectDrawBufferCapacity >= commandCount)
    {
        return true;
    }

    if (!m_device)
    {
        return false;
    }

    RHIBufferDesc desc;
    desc.size = static_cast<uint64>(commandCount) * sizeof(IndirectDrawIndexedCommand);
    desc.usage = RHIBufferUsage::IndirectArgs | RHIBufferUsage::CopyDst;
    desc.memoryType = RHIMemoryType::Upload;
    desc.stride = sizeof(IndirectDrawIndexedCommand);
    desc.debugName = "OpaquePass.IndirectDrawBuffer";

    m_indirectDrawBuffer = m_device->CreateBuffer(desc);
    m_indirectDrawBufferCapacity = m_indirectDrawBuffer ? commandCount : 0;
    return m_indirectDrawBuffer != nullptr;
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
                                          RHIDescriptorSet* frameSet)
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

    m_pipelineCache->UpdateObjectConstants(Mat4Identity(),
                                           Mat4Identity(),
                                           Mat4Identity(),
                                           view.previousViewProjectionMatrix,
                                           false);
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

void OpaquePass::Execute(RHICommandContext& ctx, const ViewData& view)
{
    m_drawStats = {};

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

    if (m_materialSystem && m_renderScene)
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

    // 1. Begin render pass using builder pattern
    RHIRenderPassDesc rpDesc;
    rpDesc.AddColorAttachment(colorTargetView,
                              RHILoadOp::Clear,
                              RHIStoreOp::Store,
                              RVX_SCENE_COLOR_CLEAR_VALUE);

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
    if (m_renderScene && m_resourceRegistry != nullptr)
    {
        if (TryDrawGPUDrivenIndirect(ctx, view, colorTargetFormat, frameSet))
        {
            ctx.EndRenderPass();
            return;
        }

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

            const auto drawDirectItem = [&](const RenderDrawItem& item)
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
                    obj.mesh);
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
                                                       obj.receivesShadow,
                                                       ResolveSkinningMatrices(obj, buffers));
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
                if (buffers.boneIndicesBuffer)
                {
                    ctx.SetVertexBuffer(4, buffers.boneIndicesBuffer);  // Slot 4: Bone indices
                }
                if (buffers.boneWeightsBuffer)
                {
                    ctx.SetVertexBuffer(5, buffers.boneWeightsBuffer);  // Slot 5: Bone weights
                }

                // Bind index buffer
                ctx.SetIndexBuffer(buffers.indexBuffer, RHIFormat::R32_UINT);

                if (item.submeshIndex >= buffers.submeshes.size())
                {
                    ++m_drawStats.skippedInvalidSubmeshCount;
                    return;
                }

                const SubmeshGPUInfo& submesh = buffers.submeshes[item.submeshIndex];
                MaterialBindingOptions materialOptions;
                materialOptions.allowNormalMap = buffers.HasNormalMapTangentBasis();
                const MaterialBindingResult materialBinding =
                    m_materialSystem->PrepareMaterialBinding(
                        item.material, view.viewCache, materialOptions);
                if (!materialBinding.IsDrawable())
                {
                    ++m_drawStats.skippedMaterialBindingCount;
                    RVX_CORE_WARN("OpaquePass: Skipping {} draw item because material binding failed: {}",
                                  groupName,
                                  materialBinding.message);
                    return;
                }

                ctx.SetDescriptorSet(2, materialBinding.descriptorSet, materialBinding.dynamicOffsets);

                ctx.DrawIndexed(submesh.indexCount, 1,
                                submesh.indexOffset, submesh.baseVertex, 0);
                ++m_drawStats.directDrawCount;
            };

            const auto tryDrawIndirectBatch = [&](size_t startIndex, uint32 batchLength) -> bool
            {
                if (batchLength < 2 || !EnsureIndirectDrawCapacity(batchLength))
                {
                    return false;
                }

                const RenderDrawItem& firstItem = (*drawItems)[startIndex];
                if (firstItem.objectIndex >= m_renderScene->GetObjectCount())
                {
                    return false;
                }

                const RenderObject& obj = m_renderScene->GetObject(firstItem.objectIndex);
                MeshGPUBuffers buffers = ResolveRenderMeshBuffers(
                    m_resourceRegistry,
                    obj.mesh);
                if (!buffers.IsValid())
                {
                    return false;
                }

                m_indirectDrawCommands.clear();
                m_indirectDrawCommands.reserve(batchLength);
                RenderResourceHandle batchMaterial;
                for (uint32 i = 0; i < batchLength; ++i)
                {
                    const RenderDrawItem& item = (*drawItems)[startIndex + i];
                    if (item.objectIndex != firstItem.objectIndex ||
                        item.submeshIndex >= buffers.submeshes.size())
                    {
                        return false;
                    }

                    if (i == 0)
                    {
                        batchMaterial = item.material;
                    }
                    else if (item.material != batchMaterial)
                    {
                        return false;
                    }

                    const SubmeshGPUInfo& submesh = buffers.submeshes[item.submeshIndex];
                    IndirectDrawIndexedCommand command = {};
                    command.indexCount = submesh.indexCount;
                    command.instanceCount = 1;
                    command.firstIndex = submesh.indexOffset;
                    command.vertexOffset = submesh.baseVertex;
                    command.firstInstance = 0;
                    m_indirectDrawCommands.push_back(command);
                }

                MaterialBindingOptions materialOptions;
                materialOptions.allowNormalMap = buffers.HasNormalMapTangentBasis();
                const MaterialBindingResult materialBinding =
                    m_materialSystem->PrepareMaterialBinding(
                        batchMaterial, view.viewCache, materialOptions);
                if (!materialBinding.IsDrawable())
                {
                    return false;
                }

                m_pipelineCache->UpdateObjectConstants(obj.worldMatrix, obj.normalMatrix,
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
                ctx.SetIndexBuffer(buffers.indexBuffer, RHIFormat::R32_UINT);
                ctx.SetDescriptorSet(2, materialBinding.descriptorSet, materialBinding.dynamicOffsets);

                m_indirectDrawBuffer->Upload(m_indirectDrawCommands.data(), m_indirectDrawCommands.size());
                ctx.DrawIndexedIndirect(m_indirectDrawBuffer.Get(),
                                        0,
                                        static_cast<uint32>(m_indirectDrawCommands.size()),
                                        sizeof(IndirectDrawIndexedCommand));
                ++m_drawStats.indirectBatchCount;
                m_drawStats.indirectDrawCount += static_cast<uint32>(m_indirectDrawCommands.size());
                return true;
            };

            for (size_t drawIndex = 0; drawIndex < drawItems->size();)
            {
                const uint32 batchLength = m_indirectBatchingEnabled
                    ? FindIndirectBatchLength(*drawItems, drawIndex)
                    : 1;
                if (tryDrawIndirectBatch(drawIndex, batchLength))
                {
                    drawIndex += batchLength;
                    continue;
                }

                drawDirectItem((*drawItems)[drawIndex]);
                ++drawIndex;
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
        if (m_resourceRegistry == nullptr)
            RVX_CORE_DEBUG("OpaquePass: No GPU resources");
    }

    // 6. End render pass
    ctx.EndRenderPass();
}

} // namespace RVX
