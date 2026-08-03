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
#include "Resources/RenderSubmissionResourceBatch.h"
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
            ResourceViewCache* viewCache = nullptr;
            MaterialSystem* materialSystem = nullptr;
            FrameLightResources lightResources{};
            RHIPipelineRef pipeline;
            RasterDrawBindingSnapshot bindings{};
            std::vector<DrawRecord> draws;
            RGTextureHandle colorHandle{};
            RGTextureHandle depthHandle{};
            RHIFormat colorFormat = RHIFormat::Unknown;
            bool depthAvailable = false;
            bool requestedEnabled = false;
            bool contextValid = false;
        };

        [[nodiscard]] bool RetainResource(RenderSubmissionResourceBatch* batch,
                                          RefCounted* resource)
        {
            return !resource ||
                   RetainRenderSubmissionResource(batch, Ref<RefCounted>(resource));
        }

        [[nodiscard]] bool RetainDrawResources(GraphPassData& data)
        {
            RenderSubmissionResourceBatch* batch =
                data.execution.view.submissionResourceBatch;
            if (batch == nullptr)
            {
                return true;
            }

            for (const Ref<RefCounted>& resource : data.bindings.retainedResources)
            {
                if (!RetainRenderSubmissionResource(batch, resource))
                {
                    return false;
                }
            }
            if (!RetainResource(batch, data.pipeline.Get()))
            {
                return false;
            }

            for (const DrawRecord& draw : data.draws)
            {
                if (!RetainResource(batch, draw.buffers.positionBuffer) ||
                    !RetainResource(batch, draw.buffers.normalBuffer) ||
                    !RetainResource(batch, draw.buffers.uvBuffer) ||
                    !RetainResource(batch, draw.buffers.tangentBuffer) ||
                    !RetainResource(batch, draw.buffers.boneIndicesBuffer) ||
                    !RetainResource(batch, draw.buffers.boneWeightsBuffer) ||
                    !RetainResource(batch, draw.buffers.indexBuffer) ||
                    !RetainResource(batch, draw.material.constantBuffer.Get()) ||
                    !RetainResource(batch, draw.material.descriptorSet.Get()) ||
                    !RetainResource(batch, draw.material.layout.Get()) ||
                    !RetainResource(batch, draw.material.sampler.Get()))
                {
                    return false;
                }
                for (const RHITextureViewRef& view : draw.material.textureViews)
                {
                    if (!RetainResource(batch, view.Get()))
                    {
                        return false;
                    }
                }
                for (const RHITextureRef& texture : draw.material.textures)
                {
                    if (!RetainResource(batch, texture.Get()))
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
                    item.material, data.viewCache, materialOptions, record.material) ||
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
                !data.requestedEnabled || !data.pipelineCache || !data.viewCache ||
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
                data.identity.graph->GetTextureDesc(view.colorTarget);
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
            if (!RetainDrawResources(data))
            {
                return false;
            }

            data.colorHandle = builder.ReadWrite(
                view.colorTarget,
                MakeRHIAccessSnapshot(RHIResourceState::RenderTarget,
                                       RHIShaderStage::Pixel));
            if (!data.colorHandle.IsValid())
            {
                data.draws.clear();
                return false;
            }
            if (data.depthAvailable)
            {
                data.depthHandle = builder.Read(
                    view.depthTarget,
                    RHIResourceState::DepthRead,
                    RHIShaderStage::Vertex | RHIShaderStage::Pixel);
                if (!data.depthHandle.IsValid())
                {
                    data.draws.clear();
                    data.colorHandle = {};
                    return false;
                }
            }
            return true;
        }

        void ExecuteRecording(const GraphPassData& data,
                              RHICommandContext& ctx)
        {
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

            RenderGraph* graph = data.identity.graph;
            RHITexture* colorTexture = graph->GetTexture(data.colorHandle);
            RHITexture* depthTexture = data.depthAvailable
                ? graph->GetTexture(data.depthHandle) : nullptr;
            if (!colorTexture || (data.depthAvailable && !depthTexture))
            {
                return;
            }
            RHITextureViewRef colorViewOwner(
                data.viewCache->GetDefaultRTV(colorTexture));
            RHITextureViewRef depthViewOwner(data.depthAvailable
                ? data.viewCache->GetDefaultDSV(depthTexture) : nullptr);
            if (!colorViewOwner || (data.depthAvailable && !depthViewOwner))
            {
                RVX_CORE_WARN("TransparentPass: failed to resolve graph-owned attachment views");
                return;
            }

            if (RenderSubmissionResourceBatch* batch =
                    data.execution.view.submissionResourceBatch)
            {
                if (!RetainRenderSubmissionResource(
                        batch, Ref<RefCounted>(colorViewOwner.Get())) ||
                    !RetainRenderSubmissionResource(
                        batch, Ref<RefCounted>(colorViewOwner->GetTexture())) ||
                    (depthViewOwner &&
                     (!RetainRenderSubmissionResource(
                          batch, Ref<RefCounted>(depthViewOwner.Get())) ||
                      !RetainRenderSubmissionResource(
                          batch, Ref<RefCounted>(depthViewOwner->GetTexture())))))
                {
                    return;
                }
            }

            RHIRenderPassDesc renderPassDesc;
            renderPassDesc.AddColorAttachment(
                colorViewOwner.Get(), RHILoadOp::Load, RHIStoreOp::Store);
            if (depthViewOwner)
            {
                renderPassDesc.SetDepthStencil(
                    depthViewOwner.Get(), RHILoadOp::Load, RHIStoreOp::Store, 1.0f, 0);
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

    void TransparentPass::SetRenderScene(
        const RenderScene* scene,
        const std::vector<RenderDrawItem>* transparentDrawItems)
    {
        (void)scene;
        (void)transparentDrawItems;
        // Task 9B-6 removes this source-compatibility adapter. It must not
        // retain a caller-owned pointer or revive a cross-graph mailbox.
    }

    void TransparentPass::SetRenderTargets(RHITextureView* colorTargetView,
                                           RHITextureView* depthTargetView)
    {
        (void)colorTargetView;
        (void)depthTargetView;
        // RenderGraph handles are resolved into owned views during execution.
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
            execution.view.renderGraph == &graph && colorValid && depthValid;
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
        ResourceViewCache* const viewCache = execution.view.viewCache;
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
             viewCache,
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
                data.viewCache = viewCache;
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
            [](const GraphPassData& data, RHICommandContext& ctx)
            {
                ExecuteRecording(data, ctx);
            });
    }

} // namespace RVX
