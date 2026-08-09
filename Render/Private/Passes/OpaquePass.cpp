/**
 * @file OpaquePass.cpp
 * @brief Opaque geometry render pass implementation
 */

#include "Render/Passes/OpaquePass.h"

#include "Passes/MaterialTextureGraphBindings.h"
#include "Core/Log.h"
#include "Render/GPUDriven/GPUCulling.h"
#include "Render/Graph/ResourceViewCache.h"
#include "Render/Lighting/ClusteredLighting.h"
#include "Render/Lighting/LightManager.h"
#include "Render/Material/MaterialSystem.h"
#include "Render/PipelineCache.h"
#include "Render/Passes/DirectDrawPacketBatch.h"
#include "Render/Passes/RenderPassClearValues.h"
#include "Render/Renderer/RenderScene.h"
#include "Render/Renderer/ViewData.h"
#include "Render/Submission/RenderSubmissionStrategy.h"
#include "Resources/RenderResourceResolver.h"
#include "Resources/RenderSubmissionResourceBatch.h"
#include "RHI/RHIRenderPass.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
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

    bool HasDrawFlag(RenderDrawFlags flags, RenderDrawFlags flag) noexcept
    {
        return (static_cast<uint32>(flags) & static_cast<uint32>(flag)) != 0;
    }

    bool IsMaskedPacket(const RenderDrawPacket& packet) noexcept
    {
        return packet.pipelineKey.materialVariant == MaterialPipelineVariant::Masked;
    }

    bool IsSkinnedPacket(const RenderDrawPacket& packet) noexcept
    {
        return HasDrawFlag(packet.flags, RenderDrawFlags::Skinned) ||
            packet.pipelineKey.skinned;
    }

    RenderSubmissionLayout MakeExpectedDirectLayout(
        const RenderDrawPacket& packet) noexcept
    {
        RenderSubmissionLayout layout;
        layout.vertexStreams = MeshPassVertexStreams::Position |
            MeshPassVertexStreams::Normal |
            MeshPassVertexStreams::TexCoord |
            MeshPassVertexStreams::Tangent;
        layout.bindings = MeshPassBindingRequirements::Frame |
            MeshPassBindingRequirements::Object |
            MeshPassBindingRequirements::Geometry;
        layout.bindings |= HasDrawFlag(
            packet.flags, RenderDrawFlags::MissingMaterial)
            ? MeshPassBindingRequirements::DefaultMaterial
            : MeshPassBindingRequirements::Material;
        if (IsSkinnedPacket(packet))
        {
            layout.vertexStreams |= MeshPassVertexStreams::BoneIndices;
            layout.vertexStreams |= MeshPassVertexStreams::BoneWeights;
            layout.bindings |= MeshPassBindingRequirements::Skinning;
        }
        layout.primitiveDataBinding = PrimitiveDataBinding::PerDrawConstants;
        return layout;
    }

    bool ResolveUniqueObject(const RenderScene& scene,
                             RenderObjectId objectId,
                             uint32 primitiveData,
                             RenderObject& outObject) noexcept
    {
        if (objectId == 0 || primitiveData >= scene.GetObjectCount())
        {
            return false;
        }

        const RenderObject& indexedObject = scene.GetObject(primitiveData);
        if (indexedObject.entityId != objectId)
        {
            return false;
        }

        const RenderObject* uniqueObject = nullptr;
        for (uint32 index = 0;
             index < static_cast<uint32>(scene.GetObjectCount());
             ++index)
        {
            const RenderObject& candidate = scene.GetObject(index);
            if (candidate.entityId != objectId)
            {
                continue;
            }
            if (uniqueObject != nullptr)
            {
                return false;
            }
            uniqueObject = &candidate;
        }
        if (uniqueObject == nullptr || uniqueObject != &indexedObject)
        {
            return false;
        }
        outObject = *uniqueObject;
        return true;
    }

    const RenderPassExecutionPlan* FindPassExecutionPlan(
        const RenderFrameExecutionPlan* plan,
        RenderPassKind pass)
    {
        if (plan == nullptr)
        {
            return nullptr;
        }
        for (const RenderPassExecutionPlan& candidate : plan->passes)
        {
            if (candidate.pass == pass)
            {
                return &candidate;
            }
        }
        return nullptr;
    }

    bool IsGPUDrivenPassPlanned(const RenderFrameExecutionPlan* plan,
                                RenderPassKind pass)
    {
        const RenderPassExecutionPlan* passPlan =
            FindPassExecutionPlan(plan, pass);
        return passPlan != nullptr &&
               passPlan->partition.gpuDrivenPacketCount != 0;
    }

    void PublishOpaqueContextFailure(const RenderPassExecutionData& execution,
                                     OpaquePassDrawStats& stats)
    {
        stats = {};
        const RenderFrameExecutionPlan* executionPlan =
            execution.GetExecutionPlan();
        stats.planRequested = executionPlan != nullptr;
        stats.failureReason = RenderPolicyReason::InconsistentFacts;
        RenderFrameExecutionReport* executionReport =
            execution.GetExecutionReport();
        if (executionReport == nullptr)
        {
            return;
        }
        RenderFrameExecutionReport& frameReport = *executionReport;
        frameReport.status = RenderExecutionStatus::Failed;
        if (frameReport.frameSequence == 0)
        {
            frameReport.frameSequence = execution.identity.frameSequence;
        }

        const RenderPassExecutionPlan* passPlan = FindPassExecutionPlan(
            executionPlan, RenderPassKind::Opaque);
        const bool gpuLanePlanned = passPlan != nullptr &&
            passPlan->partition.gpuDrivenPacketCount != 0;
        const bool directLanePlanned = passPlan != nullptr &&
            passPlan->partition.directPacketCount != 0;
        const auto publishLaneFailure = [reason = stats.failureReason](
                                            RenderPassLaneExecutionReport& lane,
                                            bool planned)
        {
            lane.status = planned
                ? RenderExecutionStatus::Failed
                : RenderExecutionStatus::NotAttempted;
            lane.reason = planned ? reason : RenderPolicyReason::None;
            lane.executedCountsAvailable = planned;
            lane.executedPacketCount = 0;
            lane.executedDrawCount = 0;
        };
        for (RenderPassExecutionReport& report : frameReport.passes)
        {
            if (report.pass != RenderPassKind::Opaque)
            {
                continue;
            }
            report.status = RenderExecutionStatus::Failed;
            report.reason = stats.failureReason;
            publishLaneFailure(report.gpuDrivenLane, gpuLanePlanned);
            publishLaneFailure(report.directLane, directLanePlanned);
            break;
        }
    }

} // namespace

struct OpaquePass::PlannedOpaqueDraw
{
    DirectDrawPacket packet;
    RenderObject object;
    MeshGPUBuffers buffers;
    SubmeshGPUInfo submesh;
    RHIPipeline* pipeline = nullptr;
    RHIDescriptorSet* frameSet = nullptr;
    ObjectConstantBinding objectBinding;
    MaterialBindingResult materialBinding;
    bool allowNormalMap = false;
    bool previousWorldViewProjectionValid = false;
    bool skinned = false;
    RHIBufferRef instanceIndexBuffer;
    uint32 representedPacketCount = 1;
    bool instanced = false;
};

struct OpaquePass::PlannedGPUDrivenOpaqueDraw
{
    uint32 groupIndex = 0;
    MeshGPUBuffers buffers;
    RHIPipeline* pipeline = nullptr;
    MaterialBindingResult materialBinding;
};

void OpaquePass::OnAdd(IRHIDevice* device)
{
    (void)device;
    RVX_CORE_DEBUG("OpaquePass added");
}

void OpaquePass::OnRemove()
{
    RVX_CORE_DEBUG("OpaquePass removed");
    m_pipelineCache = nullptr;
    m_materialSystem = nullptr;
    m_lightManager = nullptr;
    m_clusteredLighting = nullptr;
    m_renderScene = nullptr;
    m_gpuCulling = nullptr;
    m_opaqueDrawItems = nullptr;
    m_maskedDrawItems = nullptr;
    m_directInstancePlan = {};
    m_directInstanceStream = {};
    m_directInstanceHandle = {};
    m_directInstanceIndexHandle = {};
    m_directMaterialParameterHandle = {};
    m_directMaterialParameterTable.Reset();
    m_gpuMaterialParameterTable.Reset();
    m_directInstancingPreflightFailed = false;
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

void OpaquePass::InitializeGraphRecorder(
    const RenderScene* scene,
    const std::vector<RenderDrawItem>* opaqueDrawItems,
    const std::vector<RenderDrawItem>* maskedDrawItems,
    const GPUCulling* gpuCulling,
    const RenderPassGPUDrivenInputs& gpuInputs,
    bool gpuDrivenPlanned,
    const DirectionalShadowRecordOutput& directionalShadow,
    const RayTracedShadowRecordOutput& rayTracedShadow)
{
    m_renderScene = scene;
    m_opaqueDrawItems = opaqueDrawItems;
    m_maskedDrawItems = maskedDrawItems;
    m_gpuCulling = gpuCulling;
    m_gpuDrivenInstanceHandle = gpuInputs.instances;
    m_gpuSceneCandidateHandle = gpuInputs.gpuSceneCandidates;
    m_gpuScenePrimitiveHandle = gpuInputs.gpuScenePrimitives;
    m_gpuSceneTransformHandle = gpuInputs.gpuSceneTransforms;
    m_gpuDrivenInstanceIndexHandle = gpuInputs.instanceIndices;
    m_gpuDrivenIndirectHandle = gpuInputs.indirectDraws;
    m_gpuDrivenDrawCountHandle = gpuInputs.drawCount;
    m_gpuSceneRasterBinding = gpuInputs.gpuSceneRasterBinding;
    m_gpuSceneRecordingFailure = gpuInputs.gpuSceneRecordingFailure;
    m_gpuDrivenOpaqueIndirectEnabled = gpuDrivenPlanned;
    m_gpuSceneRasterEnabled = gpuInputs.gpuSceneRasterEnabled;
    m_directionalShadowInputs = directionalShadow;
    m_rayTracedShadowInputs = rayTracedShadow;
}

void OpaquePass::AddToGraph(
    RenderGraph& graph,
    const RenderPassRecordContext& context)
{
    struct GraphPassData
    {
        RenderPassExecutionData execution{};
        RenderPassGPUDrivenInputs gpuInputs{};
        std::unique_ptr<OpaquePass> recorder;
        bool contextValid = false;
    };

    const auto hasCurrentGraphAttachments = [&graph](
                                                const ViewData& view,
                                                const RenderPassRecordIdentity& identity)
    {
        const bool colorValid = view.colorTarget.IsValid() &&
            HasCurrentGraphProvenance(view.colorTarget, identity) &&
            graph.GetTextureDesc(view.colorTarget) != nullptr;
        const bool depthValid = !view.depthTarget.IsValid() ||
            (HasCurrentGraphProvenance(view.depthTarget, identity) &&
             graph.GetTextureDesc(view.depthTarget) != nullptr);
        return colorValid && depthValid;
    };

    // Do not call the generic execution helper until the supplied context is
    // proven to be this exact graph's fully paired recording. The helper can
    // initialize results when a snapshot is absent, which would otherwise
    // mutate a foreign or stale recording before this pass rejects it.
    const bool sourcePlanValid = context.executionPlan != nullptr &&
        context.executionPlan->frameSequence == context.identity.frameSequence &&
        context.executionPlan->viewOrdinal == context.identity.viewOrdinal;
    const bool suppliedResultsValid = context.results != nullptr &&
        context.results->identity == context.identity &&
        (context.results->executionReport.frameSequence == 0 ||
         context.results->executionReport.frameSequence ==
             context.identity.frameSequence);
    const bool suppliedSnapshotValid = suppliedResultsValid &&
        context.frameSnapshot != nullptr &&
        context.frameSnapshot->identity == context.identity &&
        context.frameSnapshot->executionPlan.frameSequence ==
            context.identity.frameSequence &&
        context.frameSnapshot->executionPlan.viewOrdinal ==
            context.identity.viewOrdinal &&
        context.frameSnapshot->view.renderFrameExecutionPlan ==
            &context.frameSnapshot->executionPlan &&
        context.frameSnapshot->view.meshPassPreparation ==
            &context.frameSnapshot->meshPassPreparation &&
        context.frameSnapshot->view.renderVisibility ==
            &context.frameSnapshot->visibility &&
        context.frameSnapshot->view.renderFrameExecutionReport ==
            &context.results->executionReport &&
        hasCurrentGraphAttachments(context.frameSnapshot->view, context.identity);
    const bool sourceContextValid = !context.legacyAdapter &&
        context.MatchesTargetGraph(graph) && context.IsFrameIdentityValid() &&
        sourcePlanValid && suppliedResultsValid && suppliedSnapshotValid &&
        hasCurrentGraphAttachments(context.view, context.identity);

    RenderPassExecutionData execution;
    if (sourceContextValid)
    {
        execution = MakeRenderPassExecutionData(context);
    }
    else
    {
        // Keep invalid source state completely out of this graph's callback.
        execution.view = context.view;
        execution.identity = context.identity;
    }
    const RenderFrameExecutionPlan* executionPlan =
        execution.GetExecutionPlan();
    const bool hasPlan = executionPlan != nullptr;
    const bool gpuPlanned = IsGPUDrivenPassPlanned(
        executionPlan, RenderPassKind::Opaque);
    // Invalid sources must remain true no-ops: do not capture their recorded
    // GPU state into this graph callback, even if it will later reject them.
    const RenderPassGPUDrivenInputs gpuInputs = sourceContextValid
        ? context.opaqueGPUDriven : RenderPassGPUDrivenInputs{};
    const GPUCullingRecordingIdentity gpuRecordingIdentity{
        execution.identity.graphIdentity,
        execution.identity.graphRecordingGeneration,
        execution.identity.frameSequence,
        execution.identity.viewOrdinal,
        execution.identity.recordEpoch};
    const bool executionAttachmentsValid =
        hasCurrentGraphAttachments(execution.view, execution.identity);
    const bool contextValid = sourceContextValid && hasPlan &&
        execution.MatchesTargetGraph(graph) &&
        execution.IsFrameIdentityValid() &&
        execution.frameSnapshot != nullptr && execution.results != nullptr &&
        executionAttachmentsValid &&
        execution.directionalShadow.IsCompatibleWith(execution.identity) &&
        execution.rayTracedShadow.IsCompatibleWith(execution.identity) &&
        (!gpuPlanned || (gpuInputs.IsCompatibleWith(execution.identity) &&
                         gpuInputs.recordedState->Matches(gpuRecordingIdentity)));
    if (sourceContextValid && gpuPlanned &&
        gpuInputs.gpuSceneRasterEnabled && !contextValid &&
        gpuInputs.gpuSceneRecordingFailure)
    {
        gpuInputs.gpuSceneRecordingFailure->store(true);
    }
    const bool resultOwnershipValid = sourceContextValid &&
        execution.identity.Matches(graph) && execution.results != nullptr &&
        execution.results->identity == execution.identity &&
        execution.frameSnapshot != nullptr &&
        execution.frameSnapshot->identity == execution.identity;

    const RenderResourceRegistry* const resourceRegistry = m_resourceRegistry;
    PipelineCache* const pipelineCache = m_pipelineCache;
    MaterialSystem* const materialSystem = m_materialSystem;
    LightManager* const lightManager = m_lightManager;
    ClusteredLighting* const clusteredLighting = m_clusteredLighting;
    const RenderScene* const renderScene = execution.frameSnapshot
        ? &execution.frameSnapshot->scene : nullptr;
    const GPUCulling* const gpuCulling = gpuInputs.recordedState != nullptr
        ? &gpuInputs.recordedState->GetCulling() : nullptr;
    const std::vector<RenderDrawItem>* const opaqueDrawItems =
        execution.frameSnapshot ? &execution.frameSnapshot->opaqueDrawItems : nullptr;
    const std::vector<RenderDrawItem>* const maskedDrawItems =
        execution.frameSnapshot ? &execution.frameSnapshot->maskedDrawItems : nullptr;
    const DirectionalShadowRecordOutput directionalShadow =
        execution.directionalShadow;
    const RayTracedShadowRecordOutput rayTracedShadow =
        execution.rayTracedShadow;
    const std::shared_ptr<RenderPassRecordResults> results =
        resultOwnershipValid ? execution.results : nullptr;

    graph.AddPass<GraphPassData>(
        GetName(),
        GetPassType(),
        [execution,
         gpuInputs,
         contextValid,
         resourceRegistry,
         pipelineCache,
         materialSystem,
         lightManager,
         clusteredLighting,
         renderScene,
         directionalShadow,
         rayTracedShadow,
         gpuCulling,
         opaqueDrawItems,
         maskedDrawItems,
         gpuPlanned,
         results](RenderGraphBuilder& builder, GraphPassData& data)
        {
            data.execution = execution;
            data.gpuInputs = gpuInputs;
            data.contextValid = contextValid;
            if (!data.contextValid)
            {
                if (results != nullptr)
                {
                    PublishOpaqueContextFailure(data.execution, results->opaqueStats);
                    results->opaqueShadowStats = {};
                }
                return;
            }

            data.recorder = std::make_unique<OpaquePass>();
            data.recorder->SetResources(
                pipelineCache,
                materialSystem,
                lightManager,
                clusteredLighting);
            data.recorder->SetResourceRegistry(resourceRegistry);
            data.recorder->InitializeGraphRecorder(
                renderScene, opaqueDrawItems, maskedDrawItems, gpuCulling,
                data.gpuInputs, gpuPlanned, directionalShadow, rayTracedShadow);
            const auto declareDrawTextures = [&](
                const std::vector<RenderDrawItem>* drawItems)
            {
                if (drawItems == nullptr)
                {
                    return true;
                }
                for (const RenderDrawItem& item : *drawItems)
                {
                    if (!DeclareMaterialTextureGraphReads(
                            builder,
                            resourceRegistry,
                            item.material,
                            *results))
                    {
                        return false;
                    }
                }
                return true;
            };
            bool materialReadsDeclared =
                declareDrawTextures(opaqueDrawItems) &&
                declareDrawTextures(maskedDrawItems);
            if (materialReadsDeclared && gpuCulling != nullptr)
            {
                for (const GPUCullingDrawGroup& group :
                     gpuCulling->GetDrawGroups())
                {
                    if (!DeclareMaterialTextureGraphReads(
                            builder,
                            resourceRegistry,
                            group.material,
                            *results))
                    {
                        materialReadsDeclared = false;
                        break;
                    }
                }
            }
            if (!materialReadsDeclared)
            {
                data.contextValid = false;
                PublishOpaqueContextFailure(
                    data.execution, results->opaqueStats);
                return;
            }
            data.recorder->Setup(builder, data.execution.view);
            // Setup owns the graph-declaration diagnostics for this exact
            // recording. Publish them immediately so callers observing the
            // compiled graph do not have to wait for Execute to discover
            // whether shadow inputs were requested and declared.
            results->opaqueShadowStats = data.recorder->GetShadowStats();
        },
        [results](const GraphPassData& data,
                  RenderGraphPassContext& context)
        {
            if (!data.contextValid || !data.recorder)
            {
                if (results != nullptr)
                {
                    PublishOpaqueContextFailure(data.execution, results->opaqueStats);
                    results->opaqueShadowStats = {};
                }
                return;
            }
            if (results == nullptr)
            {
                return;
            }
            data.recorder->Execute(context, data.execution.view);
            results->opaqueStats = data.recorder->GetDrawStats();
            results->opaqueShadowStats = data.recorder->GetShadowStats();
        });
}

void OpaquePass::Setup(RenderGraphBuilder& builder, const ViewData& view)
{
    m_directionalShadowReadHandle = {};
    m_rayTracedShadowMaskReadHandle = {};
    m_colorTargetViewHandle = {};
    m_depthTargetViewHandle = {};
    m_directionalShadowViewHandle = {};
    m_rayTracedShadowMaskViewHandle = {};
    m_colorTargetFormat = RHIFormat::Unknown;
    m_shadowStats = {};
    m_directInstancePlan = {};
    m_directInstanceStream = {};
    m_directInstanceHandle = {};
    m_directInstanceIndexHandle = {};
    m_directMaterialParameterHandle = {};
    m_directMaterialParameterTable.Reset();
    m_gpuMaterialParameterHandle = {};
    m_gpuMaterialParameterTable.Reset();
    m_directInstancingPreflightFailed = false;
    m_gpuMaterialTablePreflightFailed = false;

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
        m_colorTargetHandle = view.colorTarget;
        if (const RHITextureDesc* desc =
                builder.GetTextureDesc(m_colorTargetHandle))
        {
            m_colorTargetFormat = desc->format;
            RHITextureViewDesc viewDesc;
            viewDesc.format = desc->format;
            viewDesc.dimension = desc->dimension;
            viewDesc.subresourceRange = RHISubresourceRange::All();
            viewDesc.type = RHITextureViewType::RenderTarget;
            viewDesc.debugName = "OpaqueColorRTV";
            m_colorTargetViewHandle = builder.CreateTextureView(
                m_colorTargetHandle, viewDesc);
            m_colorTargetViewHandle = builder.Write(
                m_colorTargetViewHandle,
                MakeRGAccessDesc(
                    RHIResourceState::RenderTarget,
                    RHIShaderStage::None));
        }
    }

    // Declare that we write to the depth target
    if (view.depthTarget.IsValid())
    {
        m_depthTargetHandle = view.depthTarget;
        if (const RHITextureDesc* desc =
                builder.GetTextureDesc(m_depthTargetHandle))
        {
            RHITextureViewDesc viewDesc;
            viewDesc.format = desc->format;
            viewDesc.dimension = desc->dimension;
            viewDesc.subresourceRange = RHISubresourceRange::All();
            viewDesc.subresourceRange.aspect = RHITextureAspect::Depth;
            viewDesc.type = RHITextureViewType::DepthStencil;
            viewDesc.debugName = "OpaqueDepthDSV";
            m_depthTargetViewHandle = builder.CreateTextureView(
                m_depthTargetHandle, viewDesc);
            m_depthTargetViewHandle = builder.Write(
                m_depthTargetViewHandle,
                MakeRGAccessDesc(
                    RHIResourceState::DepthWrite,
                    RHIShaderStage::None));
        }
    }

    if (m_directionalShadowInputs.enabled)
    {
        RGTextureHandle shadowMap = m_directionalShadowInputs.shadowMap;
        if (shadowMap.IsValid())
        {
            shadowMap.hasSubresourceRange = true;
            shadowMap.subresourceRange = RHISubresourceRange{0, RVX_ALL_MIPS, 0, RVX_ALL_LAYERS, RHITextureAspect::Depth};
            m_directionalShadowReadHandle = shadowMap;
            if (const RHITextureDesc* desc =
                    builder.GetTextureDesc(shadowMap))
            {
                RHITextureViewDesc viewDesc;
                viewDesc.format = desc->format;
                viewDesc.dimension = desc->dimension;
                viewDesc.subresourceRange = shadowMap.subresourceRange;
                viewDesc.type = RHITextureViewType::ShaderResource;
                viewDesc.debugName = "DirectionalShadowSRV";
                m_directionalShadowViewHandle =
                    builder.CreateTextureView(shadowMap, viewDesc);
                m_directionalShadowViewHandle = builder.Read(
                    m_directionalShadowViewHandle,
                    MakeRGAccessDesc(
                        RHIResourceState::ShaderResource,
                        RHIShaderStage::Pixel));
            }
            m_shadowStats.requested = true;
            m_shadowStats.renderGraphReadDeclared =
                m_directionalShadowViewHandle.IsValid();
        }
    }

    if (m_rayTracedShadowInputs.enabled)
    {
        RGTextureHandle shadowMask = m_rayTracedShadowInputs.shadowMask;
        if (shadowMask.IsValid())
        {
            m_rayTracedShadowMaskReadHandle = shadowMask;
            if (const RHITextureDesc* desc =
                    builder.GetTextureDesc(shadowMask))
            {
                RHITextureViewDesc viewDesc;
                viewDesc.format = desc->format;
                viewDesc.dimension = desc->dimension;
                viewDesc.subresourceRange = RHISubresourceRange::All();
                viewDesc.type = RHITextureViewType::ShaderResource;
                viewDesc.debugName = "RayTracedShadowMaskSRV";
                m_rayTracedShadowMaskViewHandle =
                    builder.CreateTextureView(shadowMask, viewDesc);
                m_rayTracedShadowMaskViewHandle = builder.Read(
                    m_rayTracedShadowMaskViewHandle,
                    MakeRGAccessDesc(
                        RHIResourceState::ShaderResource,
                        RHIShaderStage::Pixel));
            }
            m_shadowStats.rayTracedRequested = true;
            m_shadowStats.rayTracedRenderGraphReadDeclared =
                m_rayTracedShadowMaskViewHandle.IsValid();
        }
    }

    if (view.textureIBLEnabled != 0)
    {
        if (view.environmentIrradianceTexture.IsValid())
        {
            static_cast<void>(builder.Read(
                view.environmentIrradianceTexture,
                RHIShaderStage::Pixel));
        }
        if (view.environmentPrefilteredTexture.IsValid())
        {
            static_cast<void>(builder.Read(
                view.environmentPrefilteredTexture,
                RHIShaderStage::Pixel));
        }
        if (view.environmentBRDFLUTTexture.IsValid())
        {
            static_cast<void>(builder.Read(
                view.environmentBRDFLUTTexture,
                RHIShaderStage::Pixel));
        }
    }

    if (m_gpuDrivenOpaqueIndirectEnabled && m_gpuCulling)
    {
        if (m_gpuSceneRasterEnabled)
        {
            if (m_gpuSceneCandidateHandle.IsValid())
            {
                builder.Read(m_gpuSceneCandidateHandle, RHIShaderStage::Vertex);
            }
            if (m_gpuScenePrimitiveHandle.IsValid())
            {
                builder.Read(m_gpuScenePrimitiveHandle, RHIShaderStage::Vertex);
            }
            if (m_gpuSceneTransformHandle.IsValid())
            {
                builder.Read(m_gpuSceneTransformHandle, RHIShaderStage::Vertex);
            }
        }
        else if (m_gpuDrivenInstanceHandle.IsValid())
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

        const bool needsMaterialParameterTable = std::any_of(
            m_gpuCulling->GetDrawGroups().begin(),
            m_gpuCulling->GetDrawGroups().end(),
            [](const GPUCullingDrawGroup& group)
            {
                return group.batchKey.usesMaterialParameterTable;
            });
        if (needsMaterialParameterTable)
        {
            std::vector<MaterialParameterTableEntryRequest> requests;
            if (view.meshPassPreparation != nullptr)
            {
                for (const MeshPassProcessorResult& result :
                     view.meshPassPreparation->opaque.sortedGPUCandidates)
                {
                    if (!result.groupKey.usesMaterialParameterTable)
                    {
                        continue;
                    }
                    const MeshGPUBuffers buffers = ResolveRenderMeshBuffers(
                        m_resourceRegistry, result.packet.geometryKey.mesh);
                    if (!buffers.IsValid())
                    {
                        requests.clear();
                        break;
                    }
                    requests.push_back({
                        result.packet.materialKey.material,
                        buffers.HasNormalMapTangentBasis()});
                }
            }

            MaterialParameterTableSnapshot table;
            if (requests.empty() || m_materialSystem == nullptr ||
                !m_materialSystem->CreateMaterialParameterTableSnapshot(
                    requests, nullptr, table) ||
                !builder.RetainSubmissionResource(
                    Ref<RefCounted>(table.buffer)))
            {
                m_gpuMaterialTablePreflightFailed = true;
            }
            else
            {
                m_gpuMaterialParameterTable = std::move(table.buffer);
                m_gpuMaterialParameterHandle = builder.ImportBuffer(
                    m_gpuMaterialParameterTable,
                    RHIResourceState::ShaderResource);
                if (!m_gpuMaterialParameterHandle.IsValid())
                {
                    m_gpuMaterialTablePreflightFailed = true;
                    m_gpuMaterialParameterTable.Reset();
                }
                else
                {
                    builder.Read(m_gpuMaterialParameterHandle,
                                 RHIShaderStage::Pixel);
                }
            }
        }
    }

    if (!PrepareDirectInstanceStream(builder, view))
    {
        m_directInstancingPreflightFailed = true;
    }
}

bool OpaquePass::PrepareDirectInstanceStream(RenderGraphBuilder& builder,
                                             const ViewData& view)
{
    if (m_renderScene == nullptr ||
        view.instancingMode == RenderInstancingMode::Disabled)
    {
        return true;
    }
    if (view.instanceBatchPlans == nullptr ||
        !view.instanceBatchPlans->opaqueValid ||
        view.renderFrameExecutionPlan == nullptr ||
        view.meshPassPreparation == nullptr ||
        view.renderVisibility == nullptr || m_pipelineCache == nullptr)
    {
        return false;
    }

    m_directInstancePlan = view.instanceBatchPlans->opaque;
    if (m_directInstancePlan.instancedBatchCount == 0)
    {
        return true;
    }
    const DirectDrawPacketBatchBuildResult direct = BuildDirectDrawPacketBatch(
        *view.renderFrameExecutionPlan,
        RenderPassKind::Opaque,
        view.meshPassPreparation->opaque,
        view.renderVisibility);
    if (!direct.succeeded ||
        direct.batch.packets.size() !=
            m_directInstancePlan.executedPacketCount)
    {
        return false;
    }

    std::vector<MaterialParameterTableEntryRequest> materialRequests;
    for (const RenderInstanceBatch& batch : m_directInstancePlan.batches)
    {
        if (!batch.instanced || !batch.key.usesMaterialParameterTable)
        {
            continue;
        }
        for (const RenderInstanceBatchMember& member : batch.members)
        {
            if (member.directPacketIndex >= direct.batch.packets.size())
            {
                return false;
            }
            const RenderDrawPacket& packet =
                direct.batch.packets[member.directPacketIndex].packet;
            const MeshGPUBuffers buffers = ResolveRenderMeshBuffers(
                m_resourceRegistry, packet.geometryKey.mesh);
            if (!buffers.IsValid())
            {
                return false;
            }
            materialRequests.push_back({
                packet.materialKey.material,
                buffers.HasNormalMapTangentBasis()});
        }
    }
    if (!materialRequests.empty())
    {
        MaterialParameterTableSnapshot table;
        if (m_materialSystem == nullptr ||
            !m_materialSystem->CreateMaterialParameterTableSnapshot(
                materialRequests, nullptr, table) ||
            !builder.RetainSubmissionResource(
                Ref<RefCounted>(table.buffer)))
        {
            return false;
        }
        m_directMaterialParameterTable = std::move(table.buffer);
    }

    std::vector<GPUInstanceData> instances;
    if (!BuildRasterInstanceData(m_directInstancePlan,
                                 direct.batch,
                                 *m_renderScene,
                                 instances))
    {
        return false;
    }

    IRHIDevice* device = m_pipelineCache->GetDevice();
    if (device == nullptr ||
        !CreateRasterInstanceStream(*device,
                                    instances,
                                    "OpaqueDirectInstancing",
                                    m_directInstanceStream) ||
        !builder.RetainSubmissionResource(
            Ref<RefCounted>(m_directInstanceStream.instances)) ||
        !builder.RetainSubmissionResource(
            Ref<RefCounted>(m_directInstanceStream.instanceIndices)))
    {
        m_directInstanceStream = {};
        return false;
    }

    m_directInstanceHandle = builder.ImportBuffer(
        m_directInstanceStream.instances,
        RHIResourceState::ShaderResource);
    m_directInstanceIndexHandle = builder.ImportBuffer(
        m_directInstanceStream.instanceIndices,
        RHIResourceState::VertexBuffer);
    if (!m_directInstanceHandle.IsValid() ||
        !m_directInstanceIndexHandle.IsValid())
    {
        m_directInstanceStream = {};
        return false;
    }
    builder.Read(m_directInstanceHandle, RHIShaderStage::Vertex);
    builder.Read(m_directInstanceIndexHandle,
                 RHIResourceState::VertexBuffer,
                 RHIShaderStage::Vertex);
    if (m_directMaterialParameterTable)
    {
        m_directMaterialParameterHandle = builder.ImportBuffer(
            m_directMaterialParameterTable,
            RHIResourceState::ShaderResource);
        if (!m_directMaterialParameterHandle.IsValid())
        {
            m_directMaterialParameterTable.Reset();
            return false;
        }
        builder.Read(m_directMaterialParameterHandle,
                     RHIShaderStage::Pixel);
    }
    return true;
}

bool OpaquePass::AreGPUDrivenOpaqueGroupsDrawable(
    uint32 expectedPacketCount,
    uint32 expectedGroupCount,
    uint32& outDrawItemCount) const
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
    if (expectedGroupCount != 0 && groups.size() != expectedGroupCount)
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

    const uint32 requiredPacketCount =
        expectedPacketCount != 0 ? expectedPacketCount : sourceDrawItemCount;
    return outDrawItemCount > 0 &&
        outDrawItemCount == requiredPacketCount &&
        m_gpuCulling->GetInstanceCount() == outDrawItemCount;
}

bool OpaquePass::TryDrawGPUDrivenIndirect(RHICommandContext& ctx,
                                          RHIDescriptorSet* frameSet,
                                          const ObjectConstantBinding* tier1ObjectBinding,
                                          std::span<const PlannedGPUDrivenOpaqueDraw> plannedBatches,
                                          uint32 expectedPacketCount,
                                          uint32 expectedGroupCount)
{
    const bool usesGPUSceneRaster = m_gpuSceneRasterEnabled;
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
    if ((!usesGPUSceneRaster && !m_gpuCulling->GetInstanceBuffer()) ||
        !m_gpuCulling->GetVisibleInstanceBuffer() ||
        (usesGPUSceneRaster &&
         (!m_gpuSceneRasterBinding ||
          !m_gpuSceneRasterBinding->IsReadyForBinding() ||
          m_gpuSceneRasterBinding->leaseVersion == 0)) ||
        (!m_gpuCulling->WasGpuExecutionUsedLastCull() &&
         !m_gpuCulling->WasCpuFallbackUsedLastCull()))
    {
        m_drawStats.gpuDrivenFallbackReason =
            GPUDrivenDrawFallbackReason::CullingOutputUnavailable;
        return false;
    }
    m_drawStats.gpuDrivenCullingReady = true;

    const auto& groups = m_gpuCulling->GetDrawGroups();
    if ((expectedGroupCount != 0 && groups.size() != expectedGroupCount) ||
        plannedBatches.size() != groups.size() ||
        (expectedPacketCount != 0 &&
         m_gpuCulling->GetInstanceCount() != expectedPacketCount))
    {
        m_drawStats.gpuDrivenFallbackReason =
            GPUDrivenDrawFallbackReason::DrawGroupsUnavailable;
        return false;
    }
    m_drawStats.gpuDrivenPipelineReady = true;

    if (!frameSet)
    {
        m_drawStats.gpuDrivenFallbackReason =
            GPUDrivenDrawFallbackReason::FrameBindingsUnavailable;
        return false;
    }

    if (!usesGPUSceneRaster)
    {
        if (tier1ObjectBinding == nullptr || !tier1ObjectBinding->IsValid())
        {
            m_drawStats.gpuDrivenFallbackReason =
                GPUDrivenDrawFallbackReason::ObjectBindingUnavailable;
            return false;
        }
    }

    RHIDescriptorSet* objectSet = usesGPUSceneRaster
        ? m_gpuSceneRasterBinding->objectDescriptorSet.Get()
        : tier1ObjectBinding->descriptorSet.Get();
    if (!objectSet)
    {
        m_drawStats.gpuDrivenFallbackReason =
            GPUDrivenDrawFallbackReason::ObjectBindingUnavailable;
        return false;
    }
    static constexpr std::array<uint32, 1> gpuSceneObjectOffsets{0u};
    const auto objectDynamicOffsets = usesGPUSceneRaster
        ? gpuSceneObjectOffsets
        : tier1ObjectBinding->dynamicOffsets;
    m_drawStats.gpuDrivenEligible = true;
    m_drawStats.gpuDrivenIndirectExecutedDrawCountAvailable =
        m_gpuCulling->WasCpuFallbackUsedLastCull();

    bool submittedAny = false;
    for (const PlannedGPUDrivenOpaqueDraw& batch : plannedBatches)
    {
        ctx.SetPipeline(batch.pipeline);
        if (frameSet)
        {
            ctx.SetDescriptorSet(0, frameSet);
        }
        ctx.SetDescriptorSet(1, objectSet, objectDynamicOffsets);

        ctx.SetVertexBuffer(0, batch.buffers.positionBuffer);
        ctx.SetVertexBuffer(6, m_gpuCulling->GetVisibleInstanceBuffer());
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

        const GPUCullingIndexedIndirectSubmission cullingSubmission =
            m_gpuCulling->BuildIndexedIndirectGroupSubmission(batch.groupIndex);
        RenderSubmissionRequest request;
        request.kind = RenderSubmissionKind::IndexedIndirect;
        request.indexedIndirect = cullingSubmission.execution;
        request.capabilities = cullingSubmission.capabilities;
        static const IndexedIndirectRenderSubmissionStrategy strategy;
        const RenderSubmissionResult submission = strategy.Submit(ctx, request);
        if (submission.validationCode != RenderSubmissionValidationCode::Success)
        {
            m_drawStats.gpuDrivenFallbackReason =
                GPUDrivenDrawFallbackReason::CullingOutputUnavailable;
            return false;
        }
        if (submission.recorded)
        {
            submittedAny = true;
            ++m_drawStats.gpuDrivenIndirectBatchCount;
            m_drawStats.gpuDrivenIndirectSubmittedDrawUpperBound +=
                submission.submittedDrawUpperBound;
            if (submission.executedDrawCountAvailable)
            {
                m_drawStats.gpuDrivenIndirectDrawCount +=
                    submission.executedDrawCount;
            }
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

bool OpaquePass::BuildPlannedDirectBatch(
    RenderGraphPassContext& context,
    const ViewData& view,
    RHIFormat colorTargetFormat,
    RHIDescriptorSet* frameSet,
    std::vector<PlannedOpaqueDraw>& outPlannedDraws)
{
    outPlannedDraws.clear();
    m_drawStats.planRequested = true;
    m_drawStats.failureReason = RenderPolicyReason::UnexpectedRecordingFailure;
    if (view.renderFrameExecutionPlan == nullptr ||
        view.meshPassPreparation == nullptr ||
        m_renderScene == nullptr || m_resourceRegistry == nullptr ||
        m_pipelineCache == nullptr || m_materialSystem == nullptr ||
        frameSet == nullptr)
    {
        RVX_RENDER_ERROR("OpaquePass: Direct batch preflight is missing required frame dependencies");
        return false;
    }

    const DirectDrawPacketBatchBuildResult built =
        BuildDirectDrawPacketBatch(*view.renderFrameExecutionPlan,
                                   RenderPassKind::Opaque,
                                   view.meshPassPreparation->opaque,
                                   view.renderVisibility);
    if (!built.succeeded ||
        built.batch.packets.size() > std::numeric_limits<uint32>::max())
    {
        RVX_RENDER_ERROR(
            "OpaquePass: Direct packet batch construction failed (reason={}, packets={})",
            GetRenderPolicyReasonName(built.reason),
            built.batch.packets.size());
        return false;
    }

    m_drawStats.planValidated = true;
    uint32 plannedPacketCount = 0;
    for (const RenderPassExecutionPlan& passPlan :
         view.renderFrameExecutionPlan->passes)
    {
        if (passPlan.pass == RenderPassKind::Opaque)
        {
            plannedPacketCount = passPlan.directPackets.count;
            break;
        }
    }
    m_drawStats.plannedPacketCount = plannedPacketCount;
    m_drawStats.compiledPacketCount =
        static_cast<uint32>(built.batch.packets.size());

    outPlannedDraws.reserve(built.batch.packets.size());
    for (const DirectDrawPacket& draw : built.batch.packets)
    {
        const RenderDrawPacket& packet = draw.packet;
        if (packet.pass != RenderPassKind::Opaque ||
            packet.primitiveData == RVX_INVALID_PRIMITIVE_DATA_INDEX ||
            !packet.geometryKey.mesh.IsValid() ||
            packet.geometryKey.indexType != MeshUploadIndexType::UInt32 ||
            packet.pipelineKey.topology != MeshUploadPrimitiveTopology::Triangles ||
            packet.arguments.indexCount == 0 ||
            packet.arguments.instanceCount != 1 ||
            packet.arguments.firstInstance != 0)
        {
            RVX_RENDER_ERROR(
                "OpaquePass: Direct packet {} failed draw-argument preflight",
                draw.sourceOrdinal);
            outPlannedDraws.clear();
            return false;
        }

        RenderObject object;
        if (!ResolveUniqueObject(*m_renderScene,
                                 packet.objectId,
                                 packet.primitiveData,
                                 object) ||
            object.mesh != packet.geometryKey.mesh ||
            object.skinningMatrices.size() > std::numeric_limits<uint32>::max())
        {
            RVX_RENDER_ERROR(
                "OpaquePass: Direct packet {} failed unique render-object resolution",
                draw.sourceOrdinal);
            outPlannedDraws.clear();
            return false;
        }

        const bool masked = IsMaskedPacket(packet);
        const bool skinned = IsSkinnedPacket(packet);
        const bool missingMaterial =
            HasDrawFlag(packet.flags, RenderDrawFlags::MissingMaterial) ||
            !packet.materialKey.material.IsValid();
        const bool previousWorldViewProjectionValid =
            object.previousWorldMatrixValid != 0 &&
            view.previousViewProjectionValid != 0 &&
            !view.resetTemporalHistory;
        const RenderSubmissionLayout expectedLayout =
            MakeExpectedDirectLayout(packet);
        if (draw.layout != expectedLayout ||
            draw.layout.primitiveDataBinding !=
                PrimitiveDataBinding::PerDrawConstants ||
            (static_cast<uint32>(draw.layout.vertexStreams) &
             static_cast<uint32>(MeshPassVertexStreams::InstanceIndex)) != 0 ||
            (masked && !HasDrawFlag(packet.flags, RenderDrawFlags::Masked)) ||
            (!masked && HasDrawFlag(packet.flags, RenderDrawFlags::Masked)) ||
            (packet.pipelineKey.materialVariant != MaterialPipelineVariant::Opaque &&
             packet.pipelineKey.materialVariant != MaterialPipelineVariant::Masked) ||
            packet.pipelineKey.materialVariant !=
                GetPipelineVariantForRenderMode(packet.materialKey.materialMode) ||
            packet.pipelineKey.skinned != skinned ||
            skinned != object.HasSkinningData() ||
            (masked && packet.materialKey.materialMode != MaterialRenderMode::Masked) ||
            (!masked && packet.materialKey.materialMode != MaterialRenderMode::Opaque) ||
            missingMaterial !=
                HasDrawFlag(packet.flags, RenderDrawFlags::MissingMaterial) ||
            HasDrawFlag(packet.flags, RenderDrawFlags::CastsShadow) !=
                object.castsShadow ||
            HasDrawFlag(packet.flags, RenderDrawFlags::ReceivesShadow) !=
                object.receivesShadow)
        {
            RVX_RENDER_ERROR(
                "OpaquePass: Direct packet {} failed material/layout semantic preflight",
                draw.sourceOrdinal);
            outPlannedDraws.clear();
            return false;
        }

        const MeshGPUBuffers buffers = ResolveRenderMeshBuffers(
            m_resourceRegistry, packet.geometryKey.mesh);
        if (!buffers.IsValid() ||
            buffers.positionBuffer == nullptr ||
            buffers.normalBuffer == nullptr || !buffers.hasNormals ||
            buffers.uvBuffer == nullptr || !buffers.hasUVs ||
            buffers.tangentBuffer == nullptr ||
            buffers.indexBuffer == nullptr ||
            packet.geometryKey.submeshIndex >= buffers.submeshes.size())
        {
            RVX_RENDER_ERROR(
                "OpaquePass: Direct packet {} has incomplete uploaded mesh buffers",
                draw.sourceOrdinal);
            outPlannedDraws.clear();
            return false;
        }

        const SubmeshGPUInfo submesh =
            buffers.submeshes[packet.geometryKey.submeshIndex];
        if (packet.submeshIndex != packet.geometryKey.submeshIndex ||
            packet.arguments.indexCount != submesh.indexCount ||
            packet.arguments.firstIndex != submesh.indexOffset ||
            packet.arguments.vertexOffset != submesh.baseVertex)
        {
            RVX_RENDER_ERROR(
                "OpaquePass: Direct packet {} does not match uploaded submesh arguments",
                draw.sourceOrdinal);
            outPlannedDraws.clear();
            return false;
        }

        const bool allowNormalMap = buffers.HasNormalMapTangentBasis();
        if (skinned &&
            (!object.HasSkinningData() || !buffers.HasSkinningVertexData()))
        {
            RVX_RENDER_ERROR(
                "OpaquePass: Direct packet {} has incomplete skinning inputs",
                draw.sourceOrdinal);
            outPlannedDraws.clear();
            return false;
        }

        const bool usesDefaultMaterial =
            HasMeshPassBindingRequirement(draw.layout.bindings,
                                          MeshPassBindingRequirements::DefaultMaterial);
        const bool usesMaterial =
            HasMeshPassBindingRequirement(draw.layout.bindings,
                                          MeshPassBindingRequirements::Material);
        if ((missingMaterial &&
             (!usesDefaultMaterial || usesMaterial)) ||
            (!missingMaterial &&
             (usesDefaultMaterial || !usesMaterial)))
        {
            RVX_RENDER_ERROR(
                "OpaquePass: Direct packet {} has inconsistent material binding requirements",
                draw.sourceOrdinal);
            outPlannedDraws.clear();
            return false;
        }

        RHIPipeline* pipeline = m_pipelineCache->GetPipelineForVariant(
            packet.pipelineKey.materialVariant,
            colorTargetFormat,
            skinned ? DefaultLitDirectVertexInputMode::Skinned
                    : DefaultLitDirectVertexInputMode::Rigid);
        if (pipeline == nullptr)
        {
            RVX_RENDER_ERROR(
                "OpaquePass: Direct packet {} has no compatible graphics pipeline",
                draw.sourceOrdinal);
            outPlannedDraws.clear();
            return false;
        }

        MaterialBindingOptions materialOptions;
        materialOptions.allowNormalMap = allowNormalMap;
        materialOptions.materialParameterTable =
            m_directMaterialParameterTable.Get();
        MaterialBindingResult materialBinding =
            m_materialSystem->PrepareMaterialBinding(
                packet.materialKey.material, nullptr, materialOptions);
        if (!materialBinding.IsDrawable() ||
            (missingMaterial && !materialBinding.usedFallback))
        {
            RVX_RENDER_ERROR(
                "OpaquePass: Direct packet {} material binding failed: {}",
                draw.sourceOrdinal,
                materialBinding.message);
            ++m_drawStats.skippedMaterialBindingCount;
            outPlannedDraws.clear();
            return false;
        }
        if (!context.RetainSubmissionResource(
                Ref<RefCounted>(materialBinding.constantBuffer)) ||
            !context.RetainSubmissionResource(
                Ref<RefCounted>(materialBinding.descriptorSetRef)))
        {
            RVX_RENDER_ERROR(
                "OpaquePass: Direct packet {} material resources were rejected by submission ownership",
                draw.sourceOrdinal);
            outPlannedDraws.clear();
            return false;
        }
        ++m_drawStats.materialBindingCount;
        if (materialBinding.usedFallback)
        {
            ++m_drawStats.materialFallbackBindingCount;
        }
        m_drawStats.materialTextureFlags |= materialBinding.textureFlags;
        m_drawStats.materialFallbackTextureFlags |=
            materialBinding.fallbackTextureFlags;

        PlannedOpaqueDraw planned;
        planned.packet = draw;
        planned.object = std::move(object);
        planned.buffers = buffers;
        planned.submesh = submesh;
        planned.pipeline = pipeline;
        planned.frameSet = frameSet;
        planned.materialBinding = std::move(materialBinding);
        planned.allowNormalMap = allowNormalMap;
        planned.previousWorldViewProjectionValid =
            previousWorldViewProjectionValid;
        planned.skinned = skinned;
        if (planned.frameSet == nullptr ||
            planned.materialBinding.descriptorSet == nullptr)
        {
            RVX_RENDER_ERROR(
                "OpaquePass: Direct packet {} produced incomplete frame/material descriptors",
                draw.sourceOrdinal);
            outPlannedDraws.clear();
            return false;
        }

        if (!m_pipelineCache->CreateObjectConstantBinding(
                planned.object.worldMatrix,
                planned.object.normalMatrix,
                planned.object.previousWorldMatrix,
                view.previousViewProjectionMatrix,
                planned.previousWorldViewProjectionValid,
                planned.object.receivesShadow,
                ResolveSkinningMatrices(planned.object, planned.buffers),
                nullptr,
                planned.objectBinding) ||
            !context.RetainSubmissionResource(
                Ref<RefCounted>(planned.objectBinding.constantBuffer)) ||
            (planned.objectBinding.instanceBuffer &&
             !context.RetainSubmissionResource(
                 Ref<RefCounted>(planned.objectBinding.instanceBuffer))) ||
            !context.RetainSubmissionResource(
                Ref<RefCounted>(planned.objectBinding.descriptorSet)))
        {
            RVX_RENDER_ERROR(
                "OpaquePass: Direct packet {} object constants or submission ownership failed",
                draw.sourceOrdinal);
            outPlannedDraws.clear();
            return false;
        }
        outPlannedDraws.push_back(std::move(planned));
    }

    ApplyDirectInstancePlan(
        context, view, colorTargetFormat, outPlannedDraws);
    return true;
}

void OpaquePass::ApplyDirectInstancePlan(
    RenderGraphPassContext& context,
    const ViewData& view,
    RHIFormat colorTargetFormat,
    std::vector<PlannedOpaqueDraw>& plannedDraws)
{
    if (m_renderScene == nullptr ||
        view.instancingMode == RenderInstancingMode::Disabled ||
        m_directInstancePlan.instancedBatchCount == 0)
    {
        return;
    }
    if (m_directInstancingPreflightFailed ||
        !m_directInstanceStream.IsValid() ||
        m_directInstancePlan.executedPacketCount != plannedDraws.size())
    {
        m_drawStats.instancingFallbackBatchCount +=
            m_directInstancePlan.instancedBatchCount;
        return;
    }

    struct InstancedBinding
    {
        uint32 leaderIndex = 0;
        RHIPipeline* pipeline = nullptr;
        ObjectConstantBinding objectBinding;
    };
    std::vector<InstancedBinding> bindings;
    bindings.reserve(m_directInstancePlan.instancedBatchCount);
    std::vector<bool> consumed(plannedDraws.size(), false);

    for (const RenderInstanceBatch& batch : m_directInstancePlan.batches)
    {
        if (batch.members.empty())
        {
            m_drawStats.instancingFallbackBatchCount +=
                m_directInstancePlan.instancedBatchCount;
            return;
        }
        for (const RenderInstanceBatchMember& member : batch.members)
        {
            if (member.directPacketIndex >= plannedDraws.size() ||
                consumed[member.directPacketIndex])
            {
                m_drawStats.instancingFallbackBatchCount +=
                    m_directInstancePlan.instancedBatchCount;
                return;
            }
            consumed[member.directPacketIndex] = true;
        }
        if (!batch.instanced)
        {
            continue;
        }

        const uint32 leaderIndex = batch.members.front().directPacketIndex;
        const PlannedOpaqueDraw& leader = plannedDraws[leaderIndex];
        RHIPipeline* pipeline = batch.key.usesMaterialParameterTable
            ? m_pipelineCache->GetInstancedMaterialPipelineForVariant(
                  leader.packet.packet.pipelineKey.materialVariant,
                  colorTargetFormat)
            : m_pipelineCache->GetGPUDrivenPipelineForVariant(
                  leader.packet.packet.pipelineKey.materialVariant,
                  colorTargetFormat);
        ObjectConstantBinding objectBinding;
        if (pipeline == nullptr || leader.skinned ||
            !m_pipelineCache->CreateObjectConstantBinding(
                Mat4Identity(),
                Mat4Identity(),
                Mat4Identity(),
                view.previousViewProjectionMatrix,
                false,
                leader.object.receivesShadow,
                {},
                m_directInstanceStream.instances.Get(),
                objectBinding) ||
            !context.RetainSubmissionResource(
                Ref<RefCounted>(objectBinding.constantBuffer)) ||
            !context.RetainSubmissionResource(
                Ref<RefCounted>(objectBinding.instanceBuffer)) ||
            !context.RetainSubmissionResource(
                Ref<RefCounted>(objectBinding.descriptorSet)))
        {
            m_drawStats.instancingFallbackBatchCount +=
                m_directInstancePlan.instancedBatchCount;
            return;
        }
        bindings.push_back({leaderIndex, pipeline, std::move(objectBinding)});
    }
    if (!std::all_of(consumed.begin(), consumed.end(),
                     [](bool value) { return value; }))
    {
        m_drawStats.instancingFallbackBatchCount +=
            m_directInstancePlan.instancedBatchCount;
        return;
    }

    std::vector<PlannedOpaqueDraw> batched;
    batched.reserve(m_directInstancePlan.batches.size());
    size_t bindingIndex = 0;
    for (const RenderInstanceBatch& batch : m_directInstancePlan.batches)
    {
        const uint32 leaderIndex = batch.members.front().directPacketIndex;
        if (!batch.instanced)
        {
            batched.push_back(std::move(plannedDraws[leaderIndex]));
            continue;
        }
        if (bindingIndex >= bindings.size() ||
            bindings[bindingIndex].leaderIndex != leaderIndex)
        {
            m_drawStats.instancingFallbackBatchCount +=
                m_directInstancePlan.instancedBatchCount;
            return;
        }
        PlannedOpaqueDraw leader = std::move(plannedDraws[leaderIndex]);
        leader.pipeline = bindings[bindingIndex].pipeline;
        leader.objectBinding = std::move(bindings[bindingIndex].objectBinding);
        leader.instanceIndexBuffer = m_directInstanceStream.instanceIndices;
        leader.packet.packet.arguments.instanceCount =
            static_cast<uint32>(batch.members.size());
        leader.packet.packet.arguments.firstInstance = batch.firstInstance;
        leader.representedPacketCount =
            static_cast<uint32>(batch.members.size());
        leader.instanced = true;
        batched.push_back(std::move(leader));
        ++bindingIndex;
    }
    plannedDraws = std::move(batched);
}

bool OpaquePass::TryDrawPlannedDirect(
    RHICommandContext& ctx,
    std::span<const PlannedOpaqueDraw> plannedDraws)
{
    static const DirectRenderSubmissionStrategy strategy;
    for (const PlannedOpaqueDraw& planned : plannedDraws)
    {
        ctx.SetPipeline(planned.pipeline);
        ctx.SetDescriptorSet(0, planned.frameSet);
        ctx.SetDescriptorSet(1,
                             planned.objectBinding.descriptorSet.Get(),
                             planned.objectBinding.dynamicOffsets);
        ctx.SetDescriptorSet(2,
                             planned.materialBinding.descriptorSet,
                             planned.materialBinding.dynamicOffsets);
        ctx.SetVertexBuffer(0, planned.buffers.positionBuffer);
        ctx.SetVertexBuffer(1, planned.buffers.normalBuffer);
        ctx.SetVertexBuffer(2, planned.buffers.uvBuffer);
        ctx.SetVertexBuffer(3, planned.buffers.tangentBuffer);
        if (planned.skinned)
        {
            ctx.SetVertexBuffer(4, planned.buffers.boneIndicesBuffer);
            ctx.SetVertexBuffer(5, planned.buffers.boneWeightsBuffer);
        }
        if (planned.instanced)
        {
            ctx.SetVertexBuffer(6, planned.instanceIndexBuffer.Get());
        }
        ctx.SetIndexBuffer(planned.buffers.indexBuffer, RHIFormat::R32_UINT);

        const RenderDrawArguments& args = planned.packet.packet.arguments;
        RenderSubmissionRequest request;
        request.kind = RenderSubmissionKind::DirectIndexed;
        request.directIndexed = {
            args.indexCount,
            args.instanceCount,
            args.firstIndex,
            args.vertexOffset,
            args.firstInstance};
        const RenderSubmissionResult submission = strategy.Submit(ctx, request);
        if (submission.validationCode != RenderSubmissionValidationCode::Success)
        {
            return false;
        }
        if (submission.recorded)
        {
            ++m_drawStats.directDrawCount;
            ++m_drawStats.submittedDrawCount;
            m_drawStats.submittedInstanceCount += args.instanceCount;
            m_drawStats.executedPacketCount += planned.representedPacketCount;
            if (planned.instanced)
            {
                ++m_drawStats.instancedBatchCount;
            }
        }
    }
    m_drawStats.directPacketPathUsed = true;
    return true;
}

void OpaquePass::Execute(RHICommandContext& ctx, const ViewData& view)
{
    (void)ctx;
    (void)view;
    // Typed AddToGraph owns graph resource realization and execution.
}

void OpaquePass::Execute(
    RenderGraphPassContext& context,
    const ViewData& view)
{
    RHICommandContext& ctx = context.Commands();
    m_drawStats = {};
    m_drawStats.instancingMode = view.instancingMode;
    m_drawStats.instancingPlanAvailable =
        m_drawStats.instancingMode == RenderInstancingMode::Auto &&
        m_directInstancePlan.IsComplete();
    m_drawStats.instancingPreflightSucceeded =
        m_drawStats.instancingPlanAvailable &&
        !m_directInstancingPreflightFailed;
    if (m_drawStats.instancingPlanAvailable)
    {
        m_drawStats.plannedInstancingPacketCount =
            m_directInstancePlan.executedPacketCount;
        m_drawStats.plannedInstancingDrawCount =
            m_directInstancePlan.submittedDrawCount;
        m_drawStats.plannedInstancingInstanceCount =
            m_directInstancePlan.submittedInstanceCount;
        m_drawStats.plannedInstancingBatchCount =
            m_directInstancePlan.instancedBatchCount;
    }
    uint64 gpuLaneSubmissionCpuNanoseconds = 0;
    uint64 directLaneSubmissionCpuNanoseconds = 0;
    bool gpuLaneSubmissionTimingAvailable = false;
    bool directLaneSubmissionTimingAvailable = false;

    const bool hasPublishedPlan = view.renderFrameExecutionPlan != nullptr;
    const RenderPassExecutionPlan* opaquePlan = [&]()
    {
        if (view.renderFrameExecutionPlan == nullptr)
        {
            return static_cast<const RenderPassExecutionPlan*>(nullptr);
        }
        for (const RenderPassExecutionPlan& candidate :
             view.renderFrameExecutionPlan->passes)
        {
            if (candidate.pass == RenderPassKind::Opaque)
            {
                return &candidate;
            }
        }
        return static_cast<const RenderPassExecutionPlan*>(nullptr);
    }();
    m_drawStats.planRequested = hasPublishedPlan;

    const auto updatePlanReport = [&](RenderExecutionStatus status,
                                      RenderPolicyReason reason,
                                      uint32 executedPackets,
                                      bool gpuLane,
                                      RenderVisibilityMode visibility)
    {
        if (status == RenderExecutionStatus::Failed &&
            m_gpuSceneRasterEnabled && m_gpuSceneRecordingFailure)
        {
            m_gpuSceneRecordingFailure->store(true);
        }
        if (view.renderFrameExecutionReport == nullptr)
        {
            return;
        }
        for (RenderPassExecutionReport& report :
             view.renderFrameExecutionReport->passes)
        {
            if (report.pass != RenderPassKind::Opaque)
            {
                continue;
            }
            report.status = status;
            report.executedVisibility = visibility;
            report.reason = reason;
            report.materialBindingsAvailable = true;
            report.materialBindingCount = m_drawStats.materialBindingCount;
            report.materialFallbackBindingCount =
                m_drawStats.materialFallbackBindingCount;
            report.materialTextureFlags = m_drawStats.materialTextureFlags;
            report.materialFallbackTextureFlags =
                m_drawStats.materialFallbackTextureFlags;
            RenderPassLaneExecutionReport& lane =
                gpuLane ? report.gpuDrivenLane : report.directLane;
            lane.status = status;
            lane.reason = reason;
            lane.executedCountsAvailable = !gpuLane ||
                m_drawStats.gpuDrivenIndirectExecutedDrawCountAvailable;
            lane.executedPacketCount = lane.executedCountsAvailable
                ? executedPackets
                : 0;
            lane.executedDrawCount = lane.executedCountsAvailable
                ? (gpuLane ? m_drawStats.gpuDrivenIndirectDrawCount
                           : m_drawStats.directDrawCount)
                : 0;
            if (status == RenderExecutionStatus::Failed)
            {
                view.renderFrameExecutionReport->status = status;
            }
            break;
        }
        view.renderFrameExecutionReport->frameSequence =
            view.renderFrameExecutionPlan
                ? view.renderFrameExecutionPlan->frameSequence
                : 0;
    };

    const auto reportPlannedFailure = [&](bool gpuLane)
    {
        if (!hasPublishedPlan)
        {
            return;
        }
        m_drawStats.failureReason = RenderPolicyReason::UnexpectedRecordingFailure;
        updatePlanReport(RenderExecutionStatus::Failed,
                         m_drawStats.failureReason,
                         0,
                         gpuLane,
                         opaquePlan ? opaquePlan->visibility
                                    : RenderVisibilityMode::Cpu);
    };

    if (hasPublishedPlan &&
        (opaquePlan == nullptr || view.meshPassPreparation == nullptr))
    {
        m_drawStats.failureReason = RenderPolicyReason::InconsistentFacts;
        updatePlanReport(RenderExecutionStatus::Failed,
                         m_drawStats.failureReason,
                         0,
                         false,
                         opaquePlan ? opaquePlan->visibility
                                    : RenderVisibilityMode::Cpu);
        return;
    }

    // Validate dependencies
    if (!m_pipelineCache || !m_pipelineCache->IsInitialized())
    {
        RVX_CORE_WARN("OpaquePass: PipelineCache not available (initialized: {})",
                      m_pipelineCache ? m_pipelineCache->IsInitialized() : false);
        reportPlannedFailure(opaquePlan != nullptr &&
                             opaquePlan->partition.gpuDrivenPacketCount != 0);
        return;
    }

    if (!m_materialSystem || !m_materialSystem->IsInitialized())
    {
        RVX_CORE_WARN("OpaquePass: MaterialSystem not available");
        reportPlannedFailure(opaquePlan != nullptr &&
                             opaquePlan->partition.gpuDrivenPacketCount != 0);
        return;
    }

    RHITextureView* colorTargetView =
        context.GetTextureView(m_colorTargetViewHandle);
    const RHIFormat colorTargetFormat = m_colorTargetFormat;
    RHITextureView* depthTargetView = m_depthTargetViewHandle.IsValid()
        ? context.GetTextureView(m_depthTargetViewHandle)
        : nullptr;

    if (!colorTargetView)
    {
        RVX_CORE_WARN("OpaquePass: No color target view set");
        reportPlannedFailure(opaquePlan != nullptr &&
                             opaquePlan->partition.gpuDrivenPacketCount != 0);
        return;
    }

    if (m_depthTargetHandle.IsValid() && !depthTargetView)
    {
        RVX_CORE_WARN("OpaquePass: failed to resolve graph-owned depth target view");
        reportPlannedFailure(opaquePlan != nullptr &&
                             opaquePlan->partition.gpuDrivenPacketCount != 0);
        return;
    }

    ViewData drawView = view;
    drawView.rayTracedShadowEnabled = 0;
    drawView.rayTracedShadowMode = RayTracedShadowMode::ComplementRaster;
    DirectionalShadowFrameResources shadowResources;
    if (m_directionalShadowInputs.enabled &&
        m_directionalShadowReadHandle.IsValid() &&
        m_directionalShadowViewHandle.IsValid() &&
        !m_directionalShadowInputs.cascadeViewProjections.empty())
    {
        RHITextureView* shadowView =
            context.GetTextureView(m_directionalShadowViewHandle);
        const uint32 cascadeCount = std::min(
            static_cast<uint32>(m_directionalShadowInputs.cascadeViewProjections.size()),
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
            drawView.directionalShadowViewProjections[i] =
                m_directionalShadowInputs.cascadeViewProjections[i];
            const float splitDepth = i < m_directionalShadowInputs.cascadeSplitDepths.size()
                ? m_directionalShadowInputs.cascadeSplitDepths[i] : 0.0f;
            drawView.directionalShadowCascadeSplits[i] =
                nearClip + splitDepth * clipRange;
        }
        const float blendRatio = SanitizeUnitRatio(
            m_directionalShadowInputs.cascadeBlendRatio);
        for (uint32 i = 0; i + 1 < cascadeCount; ++i)
        {
            const float splitDistance = drawView.directionalShadowCascadeSplits[i];
            const float previousSplit = i == 0 ? nearClip : drawView.directionalShadowCascadeSplits[i - 1];
            const float cascadeSpan = std::max(0.0f, splitDistance - previousSplit);
            drawView.directionalShadowCascadeFadeDistances[i] = std::min(cascadeSpan, cascadeSpan * blendRatio);
        }
        drawView.directionalShadowViewProjection = cascadeCount > 0
            ? m_directionalShadowInputs.cascadeViewProjections[0]
            : Mat4Identity();
        drawView.directionalShadowDepthBias = m_directionalShadowInputs.shadowBias;
        drawView.directionalShadowStrength = 1.0f;
        drawView.directionalShadowInvMapSize = m_directionalShadowInputs.shadowMapSize > 0
                                                   ? 1.0f / static_cast<float>(m_directionalShadowInputs.shadowMapSize)
                                                   : 0.0f;
        drawView.directionalShadowFilterRadiusTexels =
            m_directionalShadowInputs.filterRadiusTexels;
        drawView.directionalShadowNormalBias = m_directionalShadowInputs.normalBias;
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
    RHITextureView* rayTracedShadowMaskView = nullptr;
    const bool rayTracedExecutionStateMatches =
        m_rayTracedShadowInputs.executionState != nullptr &&
        m_rayTracedShadowInputs.executionState->identity == m_rayTracedShadowInputs.identity;
    const bool rayTracedExecutionFailed = rayTracedExecutionStateMatches &&
        m_rayTracedShadowInputs.executionState->executionFailed.load(std::memory_order_acquire);
    const bool rayTracedExecutionReady = rayTracedExecutionStateMatches &&
        !rayTracedExecutionFailed &&
        m_rayTracedShadowInputs.executionState->dispatchReady.load(std::memory_order_acquire);
    m_shadowStats.rayTracedExecutionReady = rayTracedExecutionReady;
    m_shadowStats.rayTracedExecutionFailed = rayTracedExecutionFailed;
    if (rayTracedExecutionReady && m_rayTracedShadowMaskReadHandle.IsValid() &&
        m_rayTracedShadowMaskViewHandle.IsValid())
    {
        rayTracedShadowMaskView = context.GetTextureView(
            m_rayTracedShadowMaskViewHandle);
        rayTracedShadowResources.shadowMaskView = rayTracedShadowMaskView;
        rayTracedShadowResources.enabled = rayTracedShadowMaskView != nullptr;
    }
    if (rayTracedExecutionFailed)
    {
        RVX_RENDER_WARN("OpaquePass: Ray-traced shadow producer failed for the current recording; using raster-only shadowing");
    }
    const RayTracedShadowFrameBindingResult rayTracedShadowBinding =
        m_pipelineCache->UpdateRayTracedShadowFrameResources(rayTracedShadowResources);
    drawView.rayTracedShadowEnabled = rayTracedShadowBinding.shadowMaskSamplingEnabled ? 1 : 0;
    if (rayTracedShadowBinding.shadowMaskSamplingEnabled &&
        m_rayTracedShadowInputs.enabled)
    {
        drawView.rayTracedShadowFilterRadiusPixels =
            std::max(0.0f, m_rayTracedShadowInputs.filterRadiusTexels);
        drawView.rayTracedShadowMode = m_rayTracedShadowInputs.mode;
        if (drawView.rayTracedShadowMode == RayTracedShadowMode::ReplaceRaster)
        {
            ClearDirectionalShadowViewData(drawView);
        }
    }
    m_shadowStats.rayTracedFrameMaskReady = rayTracedShadowBinding.shadowMaskSamplingEnabled;

    m_pipelineCache->UpdateViewConstants(drawView);
    const RHIDescriptorSetRef frameDescriptorSet =
        m_pipelineCache->GetFrameDescriptorSetSnapshot();
    const bool retainedFrameDescriptorSet =
        context.RetainSubmissionResource(
            Ref<RefCounted>(frameDescriptorSet));
    if (!retainedFrameDescriptorSet)
    {
        RVX_RENDER_WARN("OpaquePass: submission ownership rejected frame bindings");
        drawView.rayTracedShadowEnabled = 0;
        m_shadowStats.rayTracedFrameMaskReady = false;
        reportPlannedFailure(opaquePlan != nullptr &&
                             opaquePlan->partition.gpuDrivenPacketCount != 0);
        return;
    }

    // A sealed plan owns lane selection. Preflight the full Direct lane before
    // recording anything, and never replay Direct after a GPU-lane failure.
    {
        const uint32 plannedGPUCount =
            opaquePlan->partition.gpuDrivenPacketCount;
        const uint32 plannedDirectCount =
            opaquePlan->partition.directPacketCount;
        const bool plannedGPU = plannedGPUCount != 0;
        const bool plannedDirect = plannedDirectCount != 0;
        if (plannedGPU && !ValidatePlannedGPUDrivenPacketRange(
                              *view.renderFrameExecutionPlan,
                              RenderPassKind::Opaque,
                              view.meshPassPreparation->opaque))
        {
            m_drawStats.failureReason = RenderPolicyReason::InconsistentFacts;
            updatePlanReport(RenderExecutionStatus::Failed,
                             m_drawStats.failureReason,
                             0,
                             true,
                             opaquePlan->visibility);
            return;
        }

        const bool validateDirectPlan = plannedDirect || !plannedGPU;
        const bool executeDirectLane = plannedDirect ||
            (!plannedGPU && opaquePlan->partition.inputPacketCount == 0);
        std::vector<PlannedOpaqueDraw> plannedDraws;
        if (validateDirectPlan &&
            !BuildPlannedDirectBatch(context,
                                     view,
                                     colorTargetFormat,
                                     frameDescriptorSet.Get(),
                                     plannedDraws))
        {
            updatePlanReport(RenderExecutionStatus::Failed,
                             m_drawStats.failureReason,
                             0,
                             false,
                             opaquePlan->visibility);
            return;
        }

        // Tier1 and Tier2 group resources are fully materialized before an
        // attachment is mutated.  The recording function below only emits
        // commands from these immutable bindings.
        ObjectConstantBinding tier1ObjectBinding;
        std::vector<PlannedGPUDrivenOpaqueDraw> plannedGPUBatches;
        bool gpuPreflightReady = plannedGPU;
        if (plannedGPU)
        {
            const bool usesGPUSceneRaster = m_gpuSceneRasterEnabled;
            uint32 gpuDrawItemCount = 0;
            if (!m_gpuCulling || !m_materialSystem || !m_pipelineCache ||
                m_gpuMaterialTablePreflightFailed ||
                (!usesGPUSceneRaster && !m_gpuCulling->GetInstanceBuffer()) ||
                !m_gpuCulling->GetVisibleInstanceBuffer() ||
                !AreGPUDrivenOpaqueGroupsDrawable(
                    plannedGPUCount,
                     opaquePlan->partition.drawGroupCount,
                     gpuDrawItemCount))
            {
                gpuPreflightReady = false;
            }
            if (gpuPreflightReady && !usesGPUSceneRaster)
            {
                if (!m_pipelineCache->CreateObjectConstantBinding(
                        Mat4Identity(),
                        Mat4Identity(),
                        Mat4Identity(),
                        drawView.previousViewProjectionMatrix,
                        false,
                        true,
                        {},
                        m_gpuCulling->GetInstanceBuffer(),
                        tier1ObjectBinding) ||
                    !context.RetainSubmissionResource(
                        Ref<RefCounted>(tier1ObjectBinding.constantBuffer)) ||
                    (tier1ObjectBinding.instanceBuffer &&
                     !context.RetainSubmissionResource(
                         Ref<RefCounted>(tier1ObjectBinding.instanceBuffer))) ||
                    !context.RetainSubmissionResource(
                        Ref<RefCounted>(tier1ObjectBinding.descriptorSet)))
                {
                    gpuPreflightReady = false;
                }
            }
            if (gpuPreflightReady)
            {
                const auto& groups = m_gpuCulling->GetDrawGroups();
                plannedGPUBatches.reserve(groups.size());
                for (uint32 groupIndex = 0;
                     groupIndex < static_cast<uint32>(groups.size());
                     ++groupIndex)
                {
                    const GPUCullingDrawGroup& group = groups[groupIndex];
                    const MeshGPUBuffers buffers = ResolveRenderMeshBuffers(
                        m_resourceRegistry, group.mesh);
                    RHIPipeline* pipeline = nullptr;
                    if (usesGPUSceneRaster)
                    {
                        pipeline = group.batchKey.usesMaterialParameterTable
                            ? (m_gpuSceneRasterBinding
                                ? m_gpuSceneRasterBinding
                                      ->instancedMaterialOpaquePipeline.Get()
                                : nullptr)
                            : (group.pipelineVariant ==
                                       MaterialPipelineVariant::Masked
                                ? (m_gpuSceneRasterBinding
                                    ? m_gpuSceneRasterBinding
                                          ->maskedPipeline.Get()
                                    : nullptr)
                                : (m_gpuSceneRasterBinding
                                    ? m_gpuSceneRasterBinding
                                          ->opaquePipeline.Get()
                                    : nullptr));
                    }
                    else
                    {
                        pipeline = group.batchKey.usesMaterialParameterTable
                            ? m_pipelineCache
                                  ->GetInstancedMaterialPipelineForVariant(
                                      group.pipelineVariant,
                                      colorTargetFormat)
                            : m_pipelineCache->GetGPUDrivenPipelineForVariant(
                                  group.pipelineVariant,
                                  colorTargetFormat);
                    }
                    MaterialBindingOptions options;
                    options.allowNormalMap = buffers.HasNormalMapTangentBasis();
                    options.materialParameterTable =
                        m_gpuMaterialParameterTable.Get();
                    MaterialBindingResult binding = m_materialSystem->PrepareMaterialBinding(
                        group.material, nullptr, options);
                    if (!buffers.IsValid() || pipeline == nullptr || !binding.IsDrawable() ||
                        !context.RetainSubmissionResource(
                            Ref<RefCounted>(binding.constantBuffer)) ||
                        !context.RetainSubmissionResource(
                            Ref<RefCounted>(binding.descriptorSetRef)))
                    {
                        gpuPreflightReady = false;
                        break;
                    }
                    ++m_drawStats.materialBindingCount;
                    if (binding.usedFallback)
                    {
                        ++m_drawStats.materialFallbackBindingCount;
                    }
                    m_drawStats.materialTextureFlags |= binding.textureFlags;
                    m_drawStats.materialFallbackTextureFlags |=
                        binding.fallbackTextureFlags;
                    PlannedGPUDrivenOpaqueDraw planned;
                    planned.groupIndex = groupIndex;
                    planned.buffers = buffers;
                    planned.pipeline = pipeline;
                    planned.materialBinding = std::move(binding);
                    plannedGPUBatches.emplace_back(std::move(planned));
                }
            }
        }

        if (plannedGPU && !gpuPreflightReady && !executeDirectLane)
        {
            m_drawStats.failureReason = RenderPolicyReason::UnexpectedRecordingFailure;
            updatePlanReport(RenderExecutionStatus::Failed,
                             m_drawStats.failureReason,
                             0,
                             true,
                             opaquePlan->visibility);
            return;
        }

        m_drawStats.planValidated = true;
        m_drawStats.plannedPacketCount =
            plannedGPUCount + plannedDirectCount;
        m_drawStats.compiledPacketCount = plannedGPUCount +
            m_drawStats.compiledPacketCount;

        RHIRenderPassDesc rpDesc;
        rpDesc.AddColorAttachment(colorTargetView,
                                  RHILoadOp::Clear,
                                  RHIStoreOp::Store,
                                  RVX_SCENE_COLOR_CLEAR_VALUE);
        if (depthTargetView)
        {
            rpDesc.SetDepthStencil(depthTargetView,
                                   RHILoadOp::Clear,
                                   RHIStoreOp::Store,
                                   m_pipelineCache->GetDepthClearValue(),
                                   0);
        }
        ctx.BeginRenderPass(rpDesc);
        ctx.SetViewport(drawView.GetRHIViewport());
        ctx.SetScissor(drawView.GetRHIScissor());

        bool gpuRecorded = true;
        bool directRecorded = true;
        if (plannedGPU)
        {
            const auto laneStart = std::chrono::steady_clock::now();
            gpuRecorded = gpuPreflightReady && TryDrawGPUDrivenIndirect(
                ctx,
                frameDescriptorSet.Get(),
                m_gpuSceneRasterEnabled ? nullptr : &tier1ObjectBinding,
                plannedGPUBatches,
                plannedGPUCount,
                opaquePlan->partition.drawGroupCount);
            if (!gpuRecorded && m_gpuSceneRasterEnabled &&
                m_gpuSceneRecordingFailure)
            {
                m_gpuSceneRecordingFailure->store(true);
            }
            gpuLaneSubmissionCpuNanoseconds = static_cast<uint64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - laneStart).count());
            gpuLaneSubmissionTimingAvailable = true;
        }
        if (executeDirectLane)
        {
            const auto laneStart = std::chrono::steady_clock::now();
            directRecorded = TryDrawPlannedDirect(ctx, plannedDraws);
            directLaneSubmissionCpuNanoseconds = static_cast<uint64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - laneStart).count());
            directLaneSubmissionTimingAvailable = true;
        }
        ctx.EndRenderPass();

        const bool passRecorded = gpuRecorded && directRecorded;
        if (!passRecorded && m_gpuSceneRasterEnabled &&
            m_gpuSceneRecordingFailure)
        {
            m_gpuSceneRecordingFailure->store(true);
        }
        m_drawStats.failureReason = passRecorded
            ? RenderPolicyReason::None
            : RenderPolicyReason::UnexpectedRecordingFailure;

        if (view.renderFrameExecutionReport != nullptr)
        {
            for (RenderPassExecutionReport& report :
                 view.renderFrameExecutionReport->passes)
            {
                if (report.pass == RenderPassKind::Opaque)
                {
                    report.status = passRecorded
                        ? RenderExecutionStatus::Completed
                        : RenderExecutionStatus::Failed;
                    report.executedVisibility = opaquePlan->visibility;
                    report.reason = passRecorded
                        ? opaquePlan->reason
                        : m_drawStats.failureReason;
                    report.skippedPacketCount =
                        opaquePlan->partition.skippedPacketCount;
                    report.materialBindingsAvailable = true;
                    report.materialBindingCount =
                        m_drawStats.materialBindingCount;
                    report.materialFallbackBindingCount =
                        m_drawStats.materialFallbackBindingCount;
                    report.materialTextureFlags =
                        m_drawStats.materialTextureFlags;
                    report.materialFallbackTextureFlags =
                        m_drawStats.materialFallbackTextureFlags;
                    report.gpuDrivenLane.status = plannedGPU
                        ? (gpuRecorded ? RenderExecutionStatus::Completed
                                       : RenderExecutionStatus::Failed)
                        : RenderExecutionStatus::NotAttempted;
                    report.gpuDrivenLane.reason = plannedGPU && !gpuRecorded
                        ? RenderPolicyReason::UnexpectedRecordingFailure
                        : RenderPolicyReason::None;
                    report.gpuDrivenLane.executedCountsAvailable = plannedGPU &&
                        m_drawStats.gpuDrivenIndirectExecutedDrawCountAvailable;
                    // Preserve honest partial-recording telemetry on a late
                    // group failure. The failed lane is not replayed through
                    // Direct in this frame.
                    report.gpuDrivenLane.executedPacketCount =
                        report.gpuDrivenLane.executedCountsAvailable
                        ? m_drawStats.gpuDrivenIndirectDrawCount
                        : 0;
                    report.gpuDrivenLane.executedDrawCount =
                        report.gpuDrivenLane.executedCountsAvailable
                        ? m_drawStats.gpuDrivenIndirectDrawCount
                        : 0;
                    report.gpuDrivenLane.submissionCpuNanoseconds =
                        gpuLaneSubmissionCpuNanoseconds;
                    report.gpuDrivenLane.submissionCpuTimingAvailable =
                        gpuLaneSubmissionTimingAvailable;
                    report.directLane.status = executeDirectLane
                        ? (directRecorded ? RenderExecutionStatus::Completed
                                          : RenderExecutionStatus::Failed)
                        : RenderExecutionStatus::NotAttempted;
                    report.directLane.reason = executeDirectLane && !directRecorded
                        ? RenderPolicyReason::UnexpectedRecordingFailure
                        : RenderPolicyReason::None;
                    report.directLane.executedCountsAvailable =
                        executeDirectLane;
                    report.directLane.executedPacketCount =
                        m_drawStats.executedPacketCount;
                    report.directLane.executedDrawCount =
                        m_drawStats.directDrawCount;
                    report.directLane.submissionCpuNanoseconds =
                        directLaneSubmissionCpuNanoseconds;
                    report.directLane.submissionCpuTimingAvailable =
                        directLaneSubmissionTimingAvailable;
                    if (!passRecorded)
                    {
                        view.renderFrameExecutionReport->status =
                            RenderExecutionStatus::Failed;
                    }
                    break;
                }
            }
            view.renderFrameExecutionReport->frameSequence =
                view.renderFrameExecutionPlan->frameSequence;
        }
        return;
    }

}

} // namespace RVX
