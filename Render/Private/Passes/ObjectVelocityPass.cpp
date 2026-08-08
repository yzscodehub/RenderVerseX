#include "Render/Passes/ObjectVelocityPass.h"

#include "Core/Log.h"
#include "Render/Graph/ResourceViewCache.h"
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
            bool masked = false;
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
            RHIPipelineRef opaquePipeline;
            RHIPipelineRef maskedPipeline;
            RasterDrawBindingSnapshot bindings{};
            std::vector<DrawRecord> draws;
            RGTextureHandle velocityHandle{};
            RGTextureHandle depthHandle{};
            bool requestedEnabled = false;
            bool supported = false;
            bool contextValid = false;
        };

        [[nodiscard]] bool IsNewerOrSameRecording(
            const RenderPassRecordIdentity& candidate,
            const RenderPassRecordIdentity& previous)
        {
            if (!candidate.IsValid())
            {
                return false;
            }
            if (!previous.IsValid())
            {
                return true;
            }
            if (candidate == previous)
            {
                return true;
            }
            if (candidate.frameSequence != previous.frameSequence)
            {
                return candidate.frameSequence > previous.frameSequence;
            }
            if (candidate.viewOrdinal != previous.viewOrdinal)
            {
                return candidate.viewOrdinal > previous.viewOrdinal;
            }
            return candidate.recordEpoch > previous.recordEpoch;
        }

        void ResetStatsForIdentity(GraphPassData& data)
        {
            if (!data.results || data.results->identity != data.identity)
            {
                return;
            }
            ObjectVelocityPassStats& stats = data.results->objectVelocityStats;
            stats = {};
            stats.requested = data.requestedEnabled;
            stats.supported = data.supported;
            stats.outputFormat = RHIFormat::RG16_FLOAT;
            if (!data.frameSnapshot)
            {
                return;
            }
            const ViewData& view = data.execution.view;
            stats.velocityTargetAvailable = view.velocityTarget.IsValid();
            stats.depthAvailable = view.depthTarget.IsValid();
            stats.previousViewProjectionAvailable =
                view.previousViewProjectionValid != 0 && !view.resetTemporalHistory;
            stats.width = view.viewportWidth;
            stats.height = view.viewportHeight;
            stats.opaqueDrawItemCount = static_cast<uint32>(
                data.frameSnapshot->opaqueDrawItems.size());
            stats.maskedDrawItemCount = static_cast<uint32>(
                data.frameSnapshot->maskedDrawItems.size());
            stats.drawItemCount = stats.opaqueDrawItemCount + stats.maskedDrawItemCount;
            stats.drawItemsAvailable = stats.drawItemCount > 0;
            if (data.identity.graph != nullptr && view.velocityTarget.IsValid())
            {
                if (const RHITextureDesc* desc = data.identity.graph->GetTextureDesc(
                        view.velocityTarget))
                {
                    stats.outputFormat = desc->format;
                    if (stats.width == 0)
                        stats.width = desc->width;
                    if (stats.height == 0)
                        stats.height = desc->height;
                }
            }
        }

        bool RetainResource(RenderSubmissionResourceBatch* batch,
                            RefCounted* resource)
        {
            return !resource ||
                   RetainRenderSubmissionResource(batch, Ref<RefCounted>(resource));
        }

        bool RetainDrawResources(GraphPassData& data)
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
            if (!RetainResource(batch, data.opaquePipeline.Get()) ||
                !RetainResource(batch, data.maskedPipeline.Get()))
            {
                return false;
            }
            for (const DrawRecord& draw : data.draws)
            {
                if (!RetainResource(batch, draw.buffers.positionBuffer) ||
                    !RetainResource(batch, draw.buffers.indexBuffer) ||
                    !RetainResource(batch, draw.buffers.uvBuffer) ||
                    !RetainResource(batch, draw.buffers.boneIndicesBuffer) ||
                    !RetainResource(batch, draw.buffers.boneWeightsBuffer) ||
                    !RetainResource(batch, draw.material.constantBuffer.Get()) ||
                    !RetainResource(batch, draw.material.descriptorSet.Get()) ||
                    !RetainResource(batch, draw.material.layout.Get()))
                {
                    return false;
                }
                for (const RHISamplerRef& sampler : draw.material.samplers)
                {
                    if (!RetainResource(batch, sampler.Get()))
                    {
                        return false;
                    }
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

        bool PrepareDrawRecord(GraphPassData& data,
                               const RenderDrawItem& item,
                               bool masked)
        {
            ObjectVelocityPassStats& stats = data.results->objectVelocityStats;
            const RenderScene& scene = data.frameSnapshot->scene;
            if (item.objectIndex >= scene.GetObjectCount())
            {
                ++stats.skippedMissingResourceCount;
                return false;
            }
            const RenderObject& object = scene.GetObject(item.objectIndex);
            if (object.previousWorldMatrixValid == 0 ||
                !stats.previousViewProjectionAvailable)
            {
                ++stats.skippedNoHistoryCount;
                return false;
            }

            MeshGPUBuffers buffers = ResolveRenderMeshBuffers(
                data.resourceRegistry, object.mesh);
            if (!buffers.IsValid() || item.submeshIndex >= buffers.submeshes.size())
            {
                ++stats.skippedMissingResourceCount;
                return false;
            }
            if (masked && !buffers.uvBuffer)
            {
                ++stats.skippedMissingUVCount;
                return false;
            }

            DrawRecord record;
            record.buffers = buffers;
            record.submesh = buffers.submeshes[item.submeshIndex];
            record.masked = masked;
            if (masked)
            {
                MaterialBindingOptions materialOptions;
                materialOptions.allowNormalMap = false;
                if (!data.materialSystem->CreateMaterialBindingSnapshot(
                        item.material,
                        data.viewCache,
                        materialOptions,
                        record.material) ||
                    !record.material.IsDrawable())
                {
                    ++stats.skippedMaterialBindingCount;
                    return false;
                }
            }

            const uint32 objectSlot = data.bindings.objectCursor;
            if (!data.pipelineCache->UpdateRasterDrawBindingSnapshotObject(
                    data.bindings,
                    object.worldMatrix,
                    object.normalMatrix,
                    object.previousWorldMatrix,
                    data.execution.view.previousViewProjectionMatrix,
                    true,
                    object.receivesShadow,
                    ResolveSkinningMatrices(object, buffers)))
            {
                ++stats.skippedMissingResourceCount;
                return false;
            }
            record.objectDynamicOffsets =
                PipelineCache::BuildSingleDynamicOffset(
                    static_cast<uint64>(objectSlot) *
                    data.bindings.objectConstantStride);
            data.draws.push_back(std::move(record));
            return true;
        }

        bool PrepareRecording(GraphPassData& data,
                              RenderGraphBuilder& builder)
        {
            ResetStatsForIdentity(data);
            if (!data.contextValid || !data.results || !data.frameSnapshot ||
                !data.requestedEnabled || !data.supported)
            {
                return false;
            }
            const ViewData& view = data.execution.view;
            ObjectVelocityPassStats& stats = data.results->objectVelocityStats;
            if (!view.renderGraph || !view.velocityTarget.IsValid() ||
                !view.depthTarget.IsValid() || !stats.previousViewProjectionAvailable ||
                !stats.drawItemsAvailable || view.viewportWidth == 0 ||
                view.viewportHeight == 0)
            {
                return false;
            }

            const uint32 candidateCount = std::max(1u, stats.drawItemCount);
            if (!data.pipelineCache->CreateRasterDrawBindingSnapshot(
                    view, candidateCount, data.bindings))
            {
                return false;
            }
            const RHIFormat outputFormat = stats.outputFormat;
            data.opaquePipeline = RHIPipelineRef(
                data.pipelineCache->GetObjectVelocityPipeline(outputFormat));
            data.maskedPipeline = RHIPipelineRef(
                data.pipelineCache->GetMaskedObjectVelocityPipeline(outputFormat));
            if (!data.opaquePipeline || !data.maskedPipeline)
            {
                return false;
            }

            data.draws.reserve(stats.drawItemCount);
            for (const RenderDrawItem& item : data.frameSnapshot->opaqueDrawItems)
            {
                PrepareDrawRecord(data, item, false);
            }
            for (const RenderDrawItem& item : data.frameSnapshot->maskedDrawItems)
            {
                PrepareDrawRecord(data, item, true);
            }
            if (data.draws.empty())
            {
                return false;
            }

            // Resource declarations cannot be rolled back.  Establish the
            // submission lifetime before publishing ReadWrite/Read access so a
            // sealed/rejected batch fails as a true no-op rather than leaving
            // a graph-visible velocity/depth usage with no executable draw.
            if (!RetainDrawResources(data))
            {
                return false;
            }

            data.velocityHandle = builder.ReadWrite(
                view.velocityTarget,
                MakeRHIAccessSnapshot(RHIResourceState::RenderTarget,
                                       RHIShaderStage::Pixel));
            data.depthHandle = builder.Read(
                view.depthTarget,
                RHIResourceState::DepthRead,
                RHIShaderStage::Vertex | RHIShaderStage::Pixel);
            if (!data.velocityHandle.IsValid() || !data.depthHandle.IsValid())
            {
                data.draws.clear();
                return false;
            }
            stats.outputDeclared = true;
            return true;
        }

        void ExecuteRecording(const GraphPassData& data,
                              RHICommandContext& ctx)
        {
            if (!data.results || data.results->identity != data.identity)
            {
                return;
            }
            ObjectVelocityPassStats stats = data.results->objectVelocityStats;
            stats.velocityRecorded = false;
            if (!data.contextValid || !stats.outputDeclared || data.draws.empty() ||
                !data.identity.IsValid() || data.identity.graph == nullptr ||
                !data.identity.Matches(*data.identity.graph) ||
                !data.velocityHandle.IsValid() || !data.depthHandle.IsValid() ||
                data.velocityHandle.graphIdentity != data.identity.graphIdentity ||
                data.velocityHandle.recordingGeneration != data.identity.graphRecordingGeneration ||
                data.depthHandle.graphIdentity != data.identity.graphIdentity ||
                data.depthHandle.recordingGeneration != data.identity.graphRecordingGeneration)
            {
                data.results->objectVelocityStats = stats;
                return;
            }

            RenderGraph* graph = data.identity.graph;
            RHITexture* velocityTexture = graph->GetTexture(data.velocityHandle);
            RHITexture* depthTexture = graph->GetTexture(data.depthHandle);
            if (!velocityTexture || !depthTexture || !data.viewCache ||
                !data.bindings.IsValid())
            {
                data.results->objectVelocityStats = stats;
                return;
            }
            RHITextureViewRef velocityViewOwner(
                data.viewCache->GetDefaultRTV(velocityTexture));
            RHITextureViewRef depthViewOwner(
                data.viewCache->GetDefaultDSV(depthTexture));
            if (!velocityViewOwner || !depthViewOwner)
            {
                RVX_CORE_WARN("ObjectVelocityPass: failed to resolve velocity RTV or depth DSV");
                data.results->objectVelocityStats = stats;
                return;
            }
            if (RenderSubmissionResourceBatch* batch =
                    data.execution.view.submissionResourceBatch)
            {
                if (!RetainRenderSubmissionResource(
                        batch, Ref<RefCounted>(velocityViewOwner.Get())) ||
                    !RetainRenderSubmissionResource(
                        batch, Ref<RefCounted>(velocityViewOwner->GetTexture())) ||
                    !RetainRenderSubmissionResource(
                        batch, Ref<RefCounted>(depthViewOwner.Get())) ||
                    !RetainRenderSubmissionResource(
                        batch, Ref<RefCounted>(depthViewOwner->GetTexture())))
                {
                    data.results->objectVelocityStats = stats;
                    return;
                }
            }
            RHITextureView* velocityView = velocityViewOwner.Get();
            RHITextureView* depthView = depthViewOwner.Get();

            uint32 drawableCount = 0;
            for (const DrawRecord& draw : data.draws)
            {
                if (draw.buffers.IsValid() && draw.buffers.positionBuffer &&
                    draw.buffers.indexBuffer &&
                    (!draw.masked || (draw.buffers.uvBuffer && draw.material.IsDrawable())))
                {
                    ++drawableCount;
                }
            }
            if (drawableCount == 0)
            {
                data.results->objectVelocityStats = stats;
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

            RHIDescriptorSet* frameSet = data.bindings.frameDescriptorSet.Get();
            RHIDescriptorSet* objectSet = data.bindings.objectDescriptorSet.Get();
            RHIPipeline* currentPipeline = nullptr;
            for (const DrawRecord& draw : data.draws)
            {
                if (!draw.buffers.IsValid() || !draw.buffers.positionBuffer ||
                    !draw.buffers.indexBuffer ||
                    (draw.masked && (!draw.buffers.uvBuffer || !draw.material.IsDrawable())))
                {
                    ++stats.skippedMissingResourceCount;
                    continue;
                }
                RHIPipeline* pipeline = draw.masked
                    ? data.maskedPipeline.Get() : data.opaquePipeline.Get();
                if (pipeline != currentPipeline)
                {
                    ctx.SetPipeline(pipeline);
                    ctx.SetDescriptorSet(0, frameSet);
                    currentPipeline = pipeline;
                }
                ctx.SetDescriptorSet(1, objectSet, draw.objectDynamicOffsets);
                if (draw.masked)
                {
                    ctx.SetDescriptorSet(2,
                                         draw.material.descriptorSet.Get(),
                                         draw.material.binding.dynamicOffsets);
                }
                ctx.SetVertexBuffer(0, draw.buffers.positionBuffer);
                if (draw.masked)
                    ctx.SetVertexBuffer(2, draw.buffers.uvBuffer);
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
                ++stats.objectsWithHistory;
                ++stats.drawCount;
                if (draw.masked)
                    ++stats.maskedDrawCount;
            }
            ctx.EndRenderPass();
            stats.velocityRecorded = stats.drawCount > 0;
            data.results->objectVelocityStats = stats;
        }
    } // namespace

    void ObjectVelocityPass::OnAdd(IRHIDevice* device)
    {
        m_device = device;
    }

    void ObjectVelocityPass::OnRemove()
    {
        m_device = nullptr;
        m_resourceRegistry = nullptr;
        m_pipelineCache = nullptr;
        m_viewCache = nullptr;
        m_materialSystem = nullptr;
        m_enabled = false;
        m_lastPublishedIdentity = {};
        m_lastPublishedStats = {};
    }

    void ObjectVelocityPass::SetResources(PipelineCache* pipelineCache,
                                          ResourceViewCache* viewCache,
                                          MaterialSystem* materialSystem)
    {
        m_pipelineCache = pipelineCache;
        m_viewCache = viewCache;
        m_materialSystem = materialSystem;
        m_device = pipelineCache ? pipelineCache->GetDevice() : m_device;
    }

    void ObjectVelocityPass::PublishRecordResults(
        const std::shared_ptr<RenderPassRecordResults>& results,
        const RenderPassRecordIdentity& expectedIdentity)
    {
        if (results == nullptr || results->identity != expectedIdentity ||
            !IsNewerOrSameRecording(expectedIdentity, m_lastPublishedIdentity))
        {
            return;
        }

        m_lastPublishedIdentity = expectedIdentity;
        m_lastPublishedStats = results->objectVelocityStats;
    }

    bool ObjectVelocityPass::IsSupported() const
    {
        if (!m_enabled)
        {
            m_unsupportedReason = "Object velocity pass has not been requested";
            return false;
        }
        if (m_resourceRegistry == nullptr)
        {
            m_unsupportedReason = "Object velocity pass requires a RenderResourceRegistry";
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
        (void)builder;
        (void)view;
        // Typed AddToGraph owns all per-recording setup.  The legacy hook is
        // deliberately inert so it cannot reintroduce persistent frame mailboxes.
    }

    void ObjectVelocityPass::Execute(RHICommandContext& ctx, const ViewData& view)
    {
        (void)ctx;
        (void)view;
        // Typed AddToGraph owns execution; no persistent pass state is consumed.
    }

    void ObjectVelocityPass::AddToGraph(RenderGraph& graph, const ViewData& view)
    {
        AddToGraph(graph, MakeRenderPassRecordContext(graph, view));
    }

    void ObjectVelocityPass::AddToGraph(
        RenderGraph& graph,
        const RenderPassRecordContext& context)
    {
        // This migrated typed path requires an already paired graph-owned
        // snapshot and results object. Never let the generic helper synthesize
        // either one before proving the supplied source belongs to this exact
        // target graph: synthesis initializes results and could otherwise
        // mutate another graph's recording before rejection.
        const bool sourceContextValid = !context.legacyAdapter &&
            context.MatchesTargetGraph(graph) &&
            context.IsFrameIdentityValid() &&
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
            // Deliberately construct a no-op payload rather than invoking the
            // normal snapshot helper.  This preserves the foreign result and
            // snapshot byte-for-byte for the graph that owns them.
            execution.view = context.view;
            execution.identity = context.identity;
        }
        const bool contextValid = !context.legacyAdapter &&
            context.MatchesTargetGraph(graph) &&
            context.IsFrameIdentityValid() &&
            execution.MatchesTargetGraph(graph) &&
            execution.IsFrameIdentityValid() &&
            execution.frameSnapshot != nullptr && execution.results != nullptr &&
            execution.view.renderGraph == &graph &&
            execution.view.velocityTarget.IsValid() &&
            execution.view.depthTarget.IsValid() &&
            HasCurrentGraphProvenance(execution.view.velocityTarget, execution.identity) &&
            HasCurrentGraphProvenance(execution.view.depthTarget, execution.identity) &&
            graph.GetTextureDesc(execution.view.velocityTarget) != nullptr &&
            graph.GetTextureDesc(execution.view.depthTarget) != nullptr;
        // A malformed context can carry a results object owned by another
        // graph.  Never capture that object into this graph's callback: even
        // fail-closed diagnostics must not mutate foreign recording results.
        const bool resultOwnershipValid = execution.identity.Matches(graph) &&
            execution.results != nullptr &&
            execution.results->identity == execution.identity &&
            execution.frameSnapshot != nullptr &&
            execution.frameSnapshot->identity == execution.identity;
        const bool requestedEnabled = m_enabled;
        const bool supported = IsSupported();
        PipelineCache* const pipelineCache = m_pipelineCache;
        ResourceViewCache* const viewCache = m_viewCache;
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
             supported,
             pipelineCache,
             viewCache,
             materialSystem,
             resourceRegistry,
             frameSnapshot,
             results,
             resultOwnershipValid](RenderGraphBuilder& builder, GraphPassData& data)
            {
                data.execution = execution;
                data.identity = execution.identity;
                data.contextValid = contextValid;
                data.requestedEnabled = requestedEnabled;
                data.supported = supported;
                data.pipelineCache = pipelineCache;
                data.viewCache = viewCache;
                data.materialSystem = materialSystem;
                data.resourceRegistry = resourceRegistry;
                data.frameSnapshot = frameSnapshot;
                data.results = resultOwnershipValid ? results : nullptr;
                ResetStatsForIdentity(data);
                if (!data.pipelineCache || !data.viewCache ||
                    !data.materialSystem || !data.resourceRegistry)
                {
                    data.contextValid = false;
                    return;
                }
                if (!PrepareRecording(data, builder))
                {
                    data.draws.clear();
                    data.velocityHandle = {};
                    data.depthHandle = {};
                    if (data.results && data.results->identity == data.identity)
                    {
                        data.results->objectVelocityStats.outputDeclared = false;
                        data.results->objectVelocityStats.velocityRecorded = false;
                    }
                }
            },
            [](const GraphPassData& data, RHICommandContext& ctx)
            {
                ExecuteRecording(data, ctx);
            });
    }

} // namespace RVX
