/**
 * @file TransparentPass.cpp
 * @brief Recording-isolated transparent geometry pass implementation.
 */

#include "Render/Passes/TransparentPass.h"

#include "Core/Log.h"
#include "Render/Graph/ResourceViewCache.h"
#include "Render/Lighting/ClusteredLighting.h"
#include "Render/Lighting/LightManager.h"
#include "Render/Material/MaterialSystem.h"
#include "Render/PipelineCache.h"
#include "Render/Renderer/RenderScene.h"
#include "Render/Renderer/ViewData.h"
#include "Render/Resources/RenderResourceTypes.h"
#include "Resources/RenderResourceResolver.h"
#include "RHI/RHIRenderPass.h"

#include <array>
#include <memory>
#include <vector>

namespace RVX
{
    namespace
    {
        std::span<const Mat4> ResolveSkinningMatrices(
            const RenderObject& object,
            const MeshGPUBuffers& buffers)
        {
            if (!object.HasSkinningData() || !buffers.HasSkinningVertexData())
            {
                return {};
            }
            return std::span<const Mat4>(object.skinningMatrices.data(),
                                         object.skinningMatrices.size());
        }

        struct DrawRecord
        {
            MeshGPUBuffers buffers{};
            SubmeshGPUInfo submesh{};
            std::array<uint32, 1> objectDynamicOffsets{0};
            MaterialBindingSnapshot material{};
        };

        struct GraphPassData
        {
            RenderPassExecutionData execution{};
            RenderPassRecordIdentity identity{};
            std::shared_ptr<const RenderPassFrameSnapshot> frameSnapshot;
            std::shared_ptr<RenderPassRecordResults> results;
            const RenderResourceRegistry* resourceRegistry = nullptr;
            PipelineCache* pipelineCache = nullptr;
            MaterialSystem* materialSystem = nullptr;
            FrameLightResources lightResources{};
            RHIPipelineRef pipeline;
            RasterDrawBindingSnapshot bindings{};
            std::vector<DrawRecord> draws;
            RGTextureHandle colorHandle{};
            RGTextureHandle depthHandle{};
            RGTextureViewHandle colorViewHandle{};
            RGTextureViewHandle depthViewHandle{};
            RHIFormat colorFormat = RHIFormat::Unknown;
            bool depthAvailable = false;
            bool requestedEnabled = false;
            bool contextValid = false;
        };

        [[nodiscard]] bool RetainResource(RenderGraphBuilder& builder,
                                          RefCounted* resource)
        {
            return !resource ||
                   builder.RetainSubmissionResource(Ref<RefCounted>(resource));
        }

        [[nodiscard]] bool RetainDrawResources(
            RenderGraphBuilder& builder,
            GraphPassData& data)
        {
            for (const Ref<RefCounted>& resource : data.bindings.retainedResources)
            {
                if (!builder.RetainSubmissionResource(resource))
                {
                    return false;
                }
            }
            if (!RetainResource(builder, data.pipeline.Get()))
            {
                return false;
            }

            for (const DrawRecord& draw : data.draws)
            {
                if (!RetainResource(builder, draw.buffers.positionBuffer) ||
                    !RetainResource(builder, draw.buffers.normalBuffer) ||
                    !RetainResource(builder, draw.buffers.uvBuffer) ||
                    !RetainResource(builder, draw.buffers.tangentBuffer) ||
                    !RetainResource(builder, draw.buffers.boneIndicesBuffer) ||
                    !RetainResource(builder, draw.buffers.boneWeightsBuffer) ||
                    !RetainResource(builder, draw.buffers.indexBuffer) ||
                    !RetainResource(builder, draw.material.constantBuffer.Get()) ||
                    !RetainResource(builder, draw.material.descriptorSet.Get()) ||
                    !RetainResource(builder, draw.material.layout.Get()))
                {
                    return false;
                }
                for (const RHISamplerRef& sampler : draw.material.samplers)
                {
                    if (!RetainResource(builder, sampler.Get()))
                    {
                        return false;
                    }
                }
                for (const RHITextureViewRef& view : draw.material.textureViews)
                {
                    if (!RetainResource(builder, view.Get()))
                    {
                        return false;
                    }
                }
                for (const RHITextureRef& texture : draw.material.textures)
                {
                    if (!RetainResource(builder, texture.Get()))
                    {
                        return false;
                    }
                }
            }
            return true;
        }

        [[nodiscard]] bool PrepareDrawRecord(GraphPassData& data,
                                             const RenderDrawItem& item)
        {
            const RenderScene& scene = data.frameSnapshot->scene;
            if (item.objectIndex >= scene.GetObjectCount())
            {
                return false;
            }

            const RenderObject& object = scene.GetObject(item.objectIndex);
            MeshGPUBuffers buffers = ResolveRenderMeshBuffers(
                data.resourceRegistry, object.mesh);
            if (!buffers.IsValid() || item.submeshIndex >= buffers.submeshes.size())
            {
                return false;
            }

            DrawRecord record;
            record.buffers = buffers;
            record.submesh = buffers.submeshes[item.submeshIndex];
            MaterialBindingOptions materialOptions;
            materialOptions.allowNormalMap = buffers.HasNormalMapTangentBasis();
            if (!data.materialSystem->CreateMaterialBindingSnapshot(
                    item.material, nullptr, materialOptions, record.material) ||
                !record.material.IsDrawable())
            {
                return false;
            }

            const ViewData& view = data.execution.view;
            const bool previousWorldViewProjectionValid =
                object.previousWorldMatrixValid != 0 &&
                view.previousViewProjectionValid != 0 &&
                !view.resetTemporalHistory;
            const uint32 objectSlot = data.bindings.objectCursor;
            if (!data.pipelineCache->UpdateRasterDrawBindingSnapshotObject(
                    data.bindings,
                    object.worldMatrix,
                    object.normalMatrix,
                    object.previousWorldMatrix,
                    view.previousViewProjectionMatrix,
                    previousWorldViewProjectionValid,
                    object.receivesShadow,
                    ResolveSkinningMatrices(object, buffers)))
            {
                return false;
            }
            record.objectDynamicOffsets = PipelineCache::BuildSingleDynamicOffset(
                static_cast<uint64>(objectSlot) * data.bindings.objectConstantStride);
            data.draws.push_back(std::move(record));
            return true;
        }

        [[nodiscard]] bool PrepareRecording(GraphPassData& data,
                                            RenderGraphBuilder& builder)
        {
            if (!data.contextValid || !data.results || !data.frameSnapshot ||
                !data.requestedEnabled || !data.pipelineCache ||
                !data.materialSystem || !data.resourceRegistry)
            {
                return false;
            }

            const ViewData& view = data.execution.view;
            if (!view.colorTarget.IsValid() ||
                data.frameSnapshot->transparentDrawItems.empty())
            {
                return false;
            }
            const RHITextureDesc* colorDesc =
                builder.GetTextureDesc(view.colorTarget);
            if (colorDesc == nullptr)
            {
                return false;
            }
            data.colorFormat = colorDesc->format;
            data.depthAvailable = view.depthTarget.IsValid();

            const uint32 candidateCount = static_cast<uint32>(
                data.frameSnapshot->transparentDrawItems.size());
            if (!data.pipelineCache->CreateTransparentRasterDrawBindingSnapshot(
                    view, candidateCount, data.lightResources, data.bindings))
            {
                return false;
            }
            data.pipeline = RHIPipelineRef(data.pipelineCache->GetPipelineForVariant(
                MaterialPipelineVariant::Transparent, data.colorFormat));
            if (!data.pipeline)
            {
                return false;
            }

            data.draws.reserve(candidateCount);
            // The frame snapshot is already sorted back-to-front by scene
            // extraction. Do not regroup or sort here: valid entries retain
            // their relative blend order while invalid entries are skipped.
            for (const RenderDrawItem& item : data.frameSnapshot->transparentDrawItems)
            {
                (void)PrepareDrawRecord(data, item);
            }
            if (data.draws.empty())
            {
                return false;
            }

            // RenderGraph access declarations are irreversible. Establish all
            // submission ownership first, so a sealed/rejected batch produces
            // a genuine no-op instead of a graph-visible write without work.
            if (!RetainDrawResources(builder, data))
            {
                return false;
            }

            if (view.textureIBLEnabled != 0)
            {
                const bool iblReadsDeclared =
                    view.environmentIrradianceTexture.IsValid() &&
                    builder.Read(
                        view.environmentIrradianceTexture,
                        RHIShaderStage::Pixel).IsValid() &&
                    view.environmentPrefilteredTexture.IsValid() &&
                    builder.Read(
                        view.environmentPrefilteredTexture,
                        RHIShaderStage::Pixel).IsValid() &&
                    view.environmentBRDFLUTTexture.IsValid() &&
                    builder.Read(
                        view.environmentBRDFLUTTexture,
                        RHIShaderStage::Pixel).IsValid();
                if (!iblReadsDeclared)
                {
                    data.draws.clear();
                    return false;
                }
            }

            data.colorHandle = view.colorTarget;
            RHITextureViewDesc colorViewDesc;
            colorViewDesc.format = colorDesc->format;
            colorViewDesc.dimension = colorDesc->dimension;
            colorViewDesc.subresourceRange = RHISubresourceRange::All();
            colorViewDesc.type = RHITextureViewType::RenderTarget;
            colorViewDesc.debugName = "TransparentColorRTV";
            data.colorViewHandle = builder.CreateTextureView(
                data.colorHandle, colorViewDesc);
            data.colorViewHandle = builder.ReadWrite(
                data.colorViewHandle,
                MakeRGAccessDesc(
                    RHIResourceState::RenderTarget,
                    RHIShaderStage::Pixel));
            if (!data.colorViewHandle.IsValid())
            {
                data.draws.clear();
                return false;
            }
            if (data.depthAvailable)
            {
                data.depthHandle = view.depthTarget;
                const RHITextureDesc* depthDesc =
                    builder.GetTextureDesc(data.depthHandle);
                if (!depthDesc)
                {
                    data.draws.clear();
                    data.colorHandle = {};
                    return false;
                }
                RHITextureViewDesc depthViewDesc;
                depthViewDesc.format = depthDesc->format;
                depthViewDesc.dimension = depthDesc->dimension;
                depthViewDesc.subresourceRange = RHISubresourceRange::All();
                depthViewDesc.subresourceRange.aspect = RHITextureAspect::Depth;
                depthViewDesc.type = RHITextureViewType::DepthStencil;
                depthViewDesc.debugName = "TransparentDepthDSV";
                data.depthViewHandle = builder.CreateTextureView(
                    data.depthHandle, depthViewDesc);
                data.depthViewHandle = builder.Read(
                    data.depthViewHandle,
                    MakeRGAccessDesc(
                        RHIResourceState::DepthRead,
                        RHIShaderStage::Vertex | RHIShaderStage::Pixel));
                if (!data.depthViewHandle.IsValid())
                    return false;
            }
            return true;
        }

        void ExecuteRecording(const GraphPassData& data,
                              RenderGraphPassContext& context)
        {
            RHICommandContext& ctx = context.Commands();
            if (!data.contextValid || !data.results ||
                data.results->identity != data.identity || data.draws.empty() ||
                !data.pipeline || !data.bindings.IsValid() ||
                !data.identity.IsValid() || data.identity.graph == nullptr ||
                !data.identity.Matches(*data.identity.graph) ||
                !data.colorHandle.IsValid() ||
                data.colorHandle.graphIdentity != data.identity.graphIdentity ||
                data.colorHandle.recordingGeneration !=
                    data.identity.graphRecordingGeneration ||
                (data.depthAvailable &&
                 (!data.depthHandle.IsValid() ||
                  data.depthHandle.graphIdentity != data.identity.graphIdentity ||
                  data.depthHandle.recordingGeneration !=
                      data.identity.graphRecordingGeneration)))
            {
                return;
            }

            RHITexture* colorTexture = context.GetTexture(data.colorHandle);
            RHITexture* depthTexture = data.depthAvailable
                ? context.GetTexture(data.depthHandle) : nullptr;
            if (!colorTexture || (data.depthAvailable && !depthTexture))
            {
                return;
            }
            RHITextureView* colorView =
                context.GetTextureView(data.colorViewHandle);
            RHITextureView* depthView = data.depthAvailable
                ? context.GetTextureView(data.depthViewHandle) : nullptr;
            if (!colorView || (data.depthAvailable && !depthView))
            {
                RVX_CORE_WARN("TransparentPass: failed to resolve graph-owned attachment views");
                return;
            }

            RHIRenderPassDesc renderPassDesc;
            renderPassDesc.AddColorAttachment(
                colorView, RHILoadOp::Load, RHIStoreOp::Store);
            if (depthView)
            {
                renderPassDesc.SetDepthStencil(
                    depthView, RHILoadOp::Load, RHIStoreOp::Store, 1.0f, 0);
                renderPassDesc.depthStencilAttachment.readOnly = true;
            }
            renderPassDesc.SetRenderArea(
                0, 0, colorTexture->GetWidth(), colorTexture->GetHeight());

            ctx.BeginRenderPass(renderPassDesc);
            RHIViewport viewport;
            viewport.width = static_cast<float>(colorTexture->GetWidth());
            viewport.height = static_cast<float>(colorTexture->GetHeight());
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;
            ctx.SetViewport(viewport);
            RHIRect scissor;
            scissor.width = colorTexture->GetWidth();
            scissor.height = colorTexture->GetHeight();
            ctx.SetScissor(scissor);

            ctx.SetPipeline(data.pipeline.Get());
            ctx.SetDescriptorSet(0, data.bindings.frameDescriptorSet.Get());
            for (const DrawRecord& draw : data.draws)
            {
                if (!draw.buffers.IsValid() || !draw.buffers.positionBuffer ||
                    !draw.buffers.indexBuffer || !draw.material.IsDrawable())
                {
                    continue;
                }

                ctx.SetDescriptorSet(
                    1, data.bindings.objectDescriptorSet.Get(), draw.objectDynamicOffsets);
                ctx.SetDescriptorSet(2,
                                     draw.material.descriptorSet.Get(),
                                     draw.material.binding.dynamicOffsets);
                ctx.SetVertexBuffer(0, draw.buffers.positionBuffer);
                if (draw.buffers.normalBuffer)
                    ctx.SetVertexBuffer(1, draw.buffers.normalBuffer);
                if (draw.buffers.uvBuffer)
                    ctx.SetVertexBuffer(2, draw.buffers.uvBuffer);
                if (draw.buffers.tangentBuffer)
                    ctx.SetVertexBuffer(3, draw.buffers.tangentBuffer);
                if (draw.buffers.boneIndicesBuffer)
                    ctx.SetVertexBuffer(4, draw.buffers.boneIndicesBuffer);
                if (draw.buffers.boneWeightsBuffer)
                    ctx.SetVertexBuffer(5, draw.buffers.boneWeightsBuffer);
                ctx.SetIndexBuffer(draw.buffers.indexBuffer, RHIFormat::R32_UINT);
                ctx.DrawIndexed(draw.submesh.indexCount,
                                1,
                                draw.submesh.indexOffset,
                                draw.submesh.baseVertex,
                                0);
            }
            ctx.EndRenderPass();
        }
    } // namespace

    TransparentPass::TransparentPass() = default;

    void TransparentPass::SetResources(PipelineCache* pipelineCache,
                                       MaterialSystem* materialSystem,
                                       LightManager* lightManager,
                                       ClusteredLighting* clusteredLighting)
    {
        m_pipelineCache = pipelineCache;
        m_materialSystem = materialSystem;
        m_lightManager = lightManager;
        m_clusteredLighting = clusteredLighting;
    }

    void TransparentPass::Setup(RenderGraphBuilder& builder, const ViewData& view)
    {
        (void)builder;
        (void)view;
        // Typed AddToGraph owns all production setup.
    }

    void TransparentPass::Execute(RHICommandContext& ctx, const ViewData& view)
    {
        (void)ctx;
        (void)view;
        // Typed AddToGraph owns all production execution.
    }

    void TransparentPass::AddToGraph(RenderGraph& graph, const ViewData& view)
    {
        AddToGraph(graph, MakeRenderPassRecordContext(graph, view));
    }

    void TransparentPass::AddToGraph(
        RenderGraph& graph,
        const RenderPassRecordContext& context)
    {
        // Do not invoke the generic execution helper until the source context
        // is proven to be this exact graph's paired recording. The helper may
        // initialize results, which would otherwise mutate foreign state.
        const bool sourceContextValid = !context.legacyAdapter &&
            context.MatchesTargetGraph(graph) && context.IsFrameIdentityValid() &&
            context.frameSnapshot != nullptr && context.results != nullptr;
        const bool suppliedResultsCompatible =
            context.results == nullptr ||
            context.results->identity == RenderPassRecordIdentity{} ||
            context.results->identity == context.identity;
        const bool suppliedSnapshotCompatible =
            context.frameSnapshot == nullptr ||
            context.frameSnapshot->identity == context.identity;
        RenderPassExecutionData execution;
        if (sourceContextValid && suppliedResultsCompatible &&
            suppliedSnapshotCompatible)
        {
            execution = MakeRenderPassExecutionData(context);
        }
        else
        {
            execution.view = context.view;
            execution.identity = context.identity;
        }

        const bool colorValid = execution.view.colorTarget.IsValid() &&
            HasCurrentGraphProvenance(execution.view.colorTarget, execution.identity) &&
            graph.GetTextureDesc(execution.view.colorTarget) != nullptr;
        const bool depthValid = !execution.view.depthTarget.IsValid() ||
            (HasCurrentGraphProvenance(execution.view.depthTarget, execution.identity) &&
             graph.GetTextureDesc(execution.view.depthTarget) != nullptr);
        const bool contextValid = !context.legacyAdapter &&
            context.MatchesTargetGraph(graph) && context.IsFrameIdentityValid() &&
            execution.MatchesTargetGraph(graph) && execution.IsFrameIdentityValid() &&
            execution.frameSnapshot != nullptr && execution.results != nullptr &&
            colorValid && depthValid;
        const bool resultOwnershipValid = execution.identity.Matches(graph) &&
            execution.results != nullptr &&
            execution.results->identity == execution.identity &&
            execution.frameSnapshot != nullptr &&
            execution.frameSnapshot->identity == execution.identity;

        FrameLightResources lightResources;
        if (m_lightManager != nullptr)
        {
            lightResources.lightConstantsBuffer = m_lightManager->GetLightConstantsBuffer();
            lightResources.pointLightsBuffer = m_lightManager->GetPointLightsBuffer();
            lightResources.spotLightsBuffer = m_lightManager->GetSpotLightsBuffer();
        }
        if (m_clusteredLighting != nullptr && m_clusteredLighting->IsInitialized())
        {
            lightResources.clusterConstantsBuffer =
                m_clusteredLighting->GetClusterConstantsBuffer();
            lightResources.clusterBuffer = m_clusteredLighting->GetClusterBuffer();
            lightResources.clusterLightIndexBuffer =
                m_clusteredLighting->GetLightIndexBuffer();
        }

        const bool requestedEnabled = m_enabled;
        PipelineCache* const pipelineCache = m_pipelineCache;
        MaterialSystem* const materialSystem = m_materialSystem;
        const RenderResourceRegistry* const resourceRegistry = m_resourceRegistry;
        const std::shared_ptr<const RenderPassFrameSnapshot> frameSnapshot =
            resultOwnershipValid ? execution.frameSnapshot : nullptr;
        const std::shared_ptr<RenderPassRecordResults> results = execution.results;

        graph.AddPass<GraphPassData>(
            GetName(),
            GetPassType(),
            [execution,
             contextValid,
             requestedEnabled,
             pipelineCache,
             materialSystem,
             resourceRegistry,
             lightResources,
             frameSnapshot,
             results,
             resultOwnershipValid](RenderGraphBuilder& builder, GraphPassData& data)
            {
                data.execution = execution;
                data.identity = execution.identity;
                data.contextValid = contextValid;
                data.requestedEnabled = requestedEnabled;
                data.pipelineCache = pipelineCache;
                data.materialSystem = materialSystem;
                data.resourceRegistry = resourceRegistry;
                data.lightResources = lightResources;
                data.frameSnapshot = frameSnapshot;
                data.results = resultOwnershipValid ? results : nullptr;
                if (!PrepareRecording(data, builder))
                {
                    data.draws.clear();
                    data.pipeline.Reset();
                    data.bindings = {};
                    data.colorHandle = {};
                    data.depthHandle = {};
                    data.depthAvailable = false;
                }
            },
            [](const GraphPassData& data,
               RenderGraphPassContext& context)
            {
                ExecuteRecording(data, context);
            });
    }

} // namespace RVX
