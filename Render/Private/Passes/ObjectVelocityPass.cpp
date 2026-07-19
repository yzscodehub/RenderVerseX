#include "Render/Passes/ObjectVelocityPass.h"

#include "Core/Log.h"
#include "Render/GPUResourceManager.h"
#include "Resources/RenderResourceResolver.h"
#include "Render/Graph/ResourceViewCache.h"
#include "Render/Material/MaterialSystem.h"
#include "Render/PipelineCache.h"
#include "Render/Renderer/RenderScene.h"
#include "Render/Renderer/ViewData.h"
#include "RHI/RHIRenderPass.h"

namespace RVX
{
    namespace
    {
        const IRenderMaterialSource* ResolveMaterialResource(const RenderObject& object, uint32 submeshIndex)
        {
            if (submeshIndex >= object.materialResources.size())
            {
                return nullptr;
            }

            return object.materialResources[submeshIndex];
        }

        std::span<const Mat4> ResolveSkinningMatrices(const RenderObject& object, const MeshGPUBuffers& buffers)
        {
            if (!object.HasSkinningData() || !buffers.HasSkinningVertexData())
            {
                return {};
            }

            return std::span<const Mat4>(object.skinningMatrices.data(), object.skinningMatrices.size());
        }
    } // namespace

    void ObjectVelocityPass::OnAdd(IRHIDevice* device)
    {
        m_device = device;
    }

    void ObjectVelocityPass::OnRemove()
    {
        m_device = nullptr;
        m_gpuResources = nullptr;
        m_pipelineCache = nullptr;
        m_viewCache = nullptr;
        m_materialSystem = nullptr;
        m_renderScene = nullptr;
        m_opaqueDrawItems = nullptr;
        m_maskedDrawItems = nullptr;
        m_velocityWriteHandle = {};
        m_depthReadHandle = {};
        m_stats = {};
        m_enabled = false;
    }

    void ObjectVelocityPass::SetResources(GPUResourceManager* gpuResources,
                                          PipelineCache* pipelineCache,
                                          ResourceViewCache* viewCache,
                                          MaterialSystem* materialSystem)
    {
        m_gpuResources = gpuResources;
        m_pipelineCache = pipelineCache;
        m_viewCache = viewCache;
        m_materialSystem = materialSystem;
        m_device = pipelineCache ? pipelineCache->GetDevice() : m_device;
    }

    void ObjectVelocityPass::SetRenderScene(const RenderScene* scene,
                                            const std::vector<RenderDrawItem>* opaqueDrawItems,
                                            const std::vector<RenderDrawItem>* maskedDrawItems)
    {
        m_renderScene = scene;
        m_opaqueDrawItems = opaqueDrawItems;
        m_maskedDrawItems = maskedDrawItems;
    }

    bool ObjectVelocityPass::IsSupported() const
    {
        if (!m_enabled)
        {
            m_unsupportedReason = "Object velocity pass has not been requested";
            return false;
        }

        if (m_resourceRegistry == nullptr && !m_gpuResources)
        {
            m_unsupportedReason = "Object velocity pass requires a GPUResourceManager";
            return false;
        }

        if (!m_pipelineCache || !m_pipelineCache->IsInitialized())
        {
            m_unsupportedReason = "Object velocity pass requires an initialized PipelineCache";
            return false;
        }

        if (!m_viewCache || !m_viewCache->IsInitialized())
        {
            m_unsupportedReason = "Object velocity pass requires an initialized ResourceViewCache";
            return false;
        }

        if (!m_materialSystem || !m_materialSystem->IsInitialized())
        {
            m_unsupportedReason = "Object velocity pass requires an initialized MaterialSystem";
            return false;
        }

        if (!m_pipelineCache->GetFrameDescriptorSet() ||
            !m_pipelineCache->GetObjectDescriptorSet() ||
            !m_pipelineCache->GetObjectVelocityPipeline(RHIFormat::RG16_FLOAT) ||
            !m_pipelineCache->GetMaskedObjectVelocityPipeline(RHIFormat::RG16_FLOAT))
        {
            m_unsupportedReason = "Object velocity pipeline resources are not available";
            return false;
        }

        m_unsupportedReason.clear();
        return true;
    }

    void ObjectVelocityPass::Setup(RenderGraphBuilder& builder, const ViewData& view)
    {
        m_stats = {};
        m_stats.requested = m_enabled;
        m_stats.supported = IsSupported();
        m_stats.velocityTargetAvailable = view.velocityTarget.IsValid();
        m_stats.depthAvailable = view.depthTarget.IsValid();
        m_stats.previousViewProjectionAvailable = view.previousViewProjectionValid != 0 && !view.resetTemporalHistory;
        m_stats.opaqueDrawItemCount = m_opaqueDrawItems ? static_cast<uint32>(m_opaqueDrawItems->size()) : 0;
        m_stats.maskedDrawItemCount = m_maskedDrawItems ? static_cast<uint32>(m_maskedDrawItems->size()) : 0;
        m_stats.drawItemCount = m_stats.opaqueDrawItemCount + m_stats.maskedDrawItemCount;
        m_stats.drawItemsAvailable = m_stats.drawItemCount > 0;
        m_stats.width = view.viewportWidth;
        m_stats.height = view.viewportHeight;
        m_stats.outputFormat = RHIFormat::RG16_FLOAT;
        m_velocityWriteHandle = {};
        m_depthReadHandle = {};

        if (!m_stats.supported ||
            !m_stats.velocityTargetAvailable ||
            !m_stats.depthAvailable ||
            !m_stats.drawItemsAvailable ||
            view.viewportWidth == 0 ||
            view.viewportHeight == 0 ||
            !view.renderGraph)
        {
            return;
        }

        m_velocityWriteHandle = builder.Write(view.velocityTarget, RHIResourceState::RenderTarget);
        builder.SetDepthStencil(view.depthTarget, false, false);
        m_depthReadHandle = view.depthTarget;
        m_stats.outputDeclared = true;
    }

    void ObjectVelocityPass::Execute(RHICommandContext& ctx, const ViewData& view)
    {
        if (!m_stats.outputDeclared ||
            !view.renderGraph ||
            !m_pipelineCache ||
            (m_resourceRegistry == nullptr && !m_gpuResources) ||
            !m_viewCache ||
            !m_materialSystem ||
            !m_renderScene ||
            !m_velocityWriteHandle.IsValid() ||
            !m_depthReadHandle.IsValid())
        {
            m_stats.velocityRecorded = false;
            return;
        }

        RHITexture* velocityTexture = view.renderGraph->GetTexture(m_velocityWriteHandle);
        RHITexture* depthTexture = view.renderGraph->GetTexture(m_depthReadHandle);
        if (!velocityTexture || !depthTexture)
        {
            m_stats.velocityRecorded = false;
            return;
        }

        RHITextureView* velocityView = m_viewCache->GetDefaultRTV(velocityTexture);
        RHITextureView* depthView = m_viewCache->GetDefaultDSV(depthTexture);
        if (!velocityView || !depthView)
        {
            RVX_CORE_WARN("ObjectVelocityPass: failed to resolve velocity RTV or depth DSV");
            m_stats.velocityRecorded = false;
            return;
        }

        RHIFormat outputFormat = RHIFormat::RG16_FLOAT;
        if (const RHITextureDesc* outputDesc = view.renderGraph->GetTextureDesc(m_velocityWriteHandle))
        {
            outputFormat = outputDesc->format;
        }

        RHIPipeline* opaquePipeline = m_pipelineCache->GetObjectVelocityPipeline(outputFormat);
        RHIPipeline* maskedPipeline = m_pipelineCache->GetMaskedObjectVelocityPipeline(outputFormat);
        RHIDescriptorSet* frameSet = m_pipelineCache->GetFrameDescriptorSet();
        RHIDescriptorSet* objectSet = m_pipelineCache->GetObjectDescriptorSet();
        if (!opaquePipeline || !maskedPipeline || !frameSet || !objectSet)
        {
            RVX_CORE_WARN("ObjectVelocityPass: pipeline or descriptor resources are unavailable");
            m_stats.velocityRecorded = false;
            return;
        }

        RHIRenderPassDesc renderPassDesc;
        renderPassDesc.AddColorAttachment(velocityView, RHILoadOp::Load, RHIStoreOp::Store);
        renderPassDesc.SetDepthStencil(depthView, RHILoadOp::Load, RHIStoreOp::Store, 1.0f, 0);
        renderPassDesc.depthStencilAttachment.readOnly = true;
        renderPassDesc.SetRenderArea(0, 0, velocityTexture->GetWidth(), velocityTexture->GetHeight());

        ctx.BeginRenderPass(renderPassDesc);

        RHIViewport viewport;
        viewport.width = static_cast<float>(velocityTexture->GetWidth());
        viewport.height = static_cast<float>(velocityTexture->GetHeight());
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        ctx.SetViewport(viewport);

        RHIRect scissor;
        scissor.width = velocityTexture->GetWidth();
        scissor.height = velocityTexture->GetHeight();
        ctx.SetScissor(scissor);

        const auto historyValid = [&view](const RenderObject& object) -> bool
        {
            return object.previousWorldMatrixValid != 0 &&
                   view.previousViewProjectionValid != 0 &&
                   !view.resetTemporalHistory;
        };

        const auto uploadObjectConstants = [this, &view, objectSet, &ctx](
                                               const RenderObject& object,
                                               const MeshGPUBuffers& buffers)
        {
            m_pipelineCache->UpdateObjectConstants(object.worldMatrix,
                                                   object.normalMatrix,
                                                   object.previousWorldMatrix,
                                                   view.previousViewProjectionMatrix,
                                                   true,
                                                   ResolveSkinningMatrices(object, buffers));
            const auto objectDynamicOffsets = m_pipelineCache->GetCurrentObjectDynamicOffset();
            ctx.SetDescriptorSet(1, objectSet, objectDynamicOffsets);
        };

        if (m_opaqueDrawItems && !m_opaqueDrawItems->empty())
        {
            ctx.SetPipeline(opaquePipeline);
            ctx.SetDescriptorSet(0, frameSet);

            for (const RenderDrawItem& item : *m_opaqueDrawItems)
            {
                if (item.objectIndex >= m_renderScene->GetObjectCount())
                {
                    ++m_stats.skippedMissingResourceCount;
                    continue;
                }

                const RenderObject& object = m_renderScene->GetObject(item.objectIndex);
                if (!historyValid(object))
                {
                    ++m_stats.skippedNoHistoryCount;
                    continue;
                }

                MeshGPUBuffers buffers = ResolveRenderMeshBuffers(
                    m_resourceRegistry,
                    m_gpuResources,
                    object.mesh,
                    object.meshId);
                if (!buffers.IsValid() || item.submeshIndex >= buffers.submeshes.size())
                {
                    ++m_stats.skippedMissingResourceCount;
                    continue;
                }

                uploadObjectConstants(object, buffers);
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

                const SubmeshGPUInfo& submesh = buffers.submeshes[item.submeshIndex];
                ctx.DrawIndexed(submesh.indexCount, 1, submesh.indexOffset, submesh.baseVertex, 0);
                ++m_stats.objectsWithHistory;
                ++m_stats.drawCount;
            }
        }

        if (m_maskedDrawItems && !m_maskedDrawItems->empty())
        {
            ctx.SetPipeline(maskedPipeline);
            ctx.SetDescriptorSet(0, frameSet);

            for (const RenderDrawItem& item : *m_maskedDrawItems)
            {
                if (item.objectIndex >= m_renderScene->GetObjectCount())
                {
                    ++m_stats.skippedMissingResourceCount;
                    continue;
                }

                const RenderObject& object = m_renderScene->GetObject(item.objectIndex);
                if (!historyValid(object))
                {
                    ++m_stats.skippedNoHistoryCount;
                    continue;
                }

                MeshGPUBuffers buffers = ResolveRenderMeshBuffers(
                    m_resourceRegistry,
                    m_gpuResources,
                    object.mesh,
                    object.meshId);
                if (!buffers.IsValid() || item.submeshIndex >= buffers.submeshes.size())
                {
                    ++m_stats.skippedMissingResourceCount;
                    continue;
                }

                if (!buffers.uvBuffer)
                {
                    ++m_stats.skippedMissingUVCount;
                    continue;
                }

                const IRenderMaterialSource* materialResource =
                    item.materialResource ? item.materialResource : ResolveMaterialResource(object, item.submeshIndex);
                MaterialBindingOptions materialOptions;
                materialOptions.allowNormalMap = false;
                const MaterialBindingResult materialBinding =
                    m_resourceRegistry
                        ? m_materialSystem->PrepareMaterialBinding(
                              item.material, view.viewCache, materialOptions)
                        : m_materialSystem->PrepareMaterialBinding(
                              materialResource, view.viewCache, materialOptions);
                if (!materialBinding.IsDrawable())
                {
                    ++m_stats.skippedMaterialBindingCount;
                    continue;
                }

                uploadObjectConstants(object, buffers);
                ctx.SetDescriptorSet(2, materialBinding.descriptorSet, materialBinding.dynamicOffsets);
                ctx.SetVertexBuffer(0, buffers.positionBuffer);
                ctx.SetVertexBuffer(2, buffers.uvBuffer);
                if (buffers.boneIndicesBuffer)
                {
                    ctx.SetVertexBuffer(4, buffers.boneIndicesBuffer);
                }
                if (buffers.boneWeightsBuffer)
                {
                    ctx.SetVertexBuffer(5, buffers.boneWeightsBuffer);
                }
                ctx.SetIndexBuffer(buffers.indexBuffer, RHIFormat::R32_UINT);

                const SubmeshGPUInfo& submesh = buffers.submeshes[item.submeshIndex];
                ctx.DrawIndexed(submesh.indexCount, 1, submesh.indexOffset, submesh.baseVertex, 0);
                ++m_stats.objectsWithHistory;
                ++m_stats.drawCount;
                ++m_stats.maskedDrawCount;
            }
        }

        ctx.EndRenderPass();
        m_stats.velocityRecorded = m_stats.drawCount > 0;
    }

} // namespace RVX
