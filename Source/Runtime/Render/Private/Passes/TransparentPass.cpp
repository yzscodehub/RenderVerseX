/**
 * @file TransparentPass.cpp
 * @brief Recording-isolated transparent geometry pass implementation.
 */

#include "Render/Passes/TransparentPass.h"
#include "Render/Animation/RenderDeformationFrameResources.h"

#include "Passes/MaterialTextureGraphBindings.h"

#include "Core/Log.h"
#include "Render/Graph/ResourceViewCache.h"
#include "Render/Lighting/ClusteredLighting.h"
#include "Render/Lighting/LightingFramePlan.h"
#include "Render/Lighting/LightManager.h"
#include "Render/Material/MaterialSystem.h"
#include "Render/PipelineCache.h"
#include "Render/Renderer/RenderScene.h"
#include "Render/Renderer/ViewData.h"
#include "Render/Resources/RenderResourceTypes.h"
#include "Resources/RenderResourceResolver.h"
#include "RHI/RHIRenderPass.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <memory>
#include <vector>

namespace RVX
{
    namespace
    {
        [[nodiscard]] bool SamePaletteMetadata(
            const RenderSkinningPaletteMetadata& left,
            const RenderSkinningPaletteMetadata& right) noexcept
        {
            return left.providerComponentId == right.providerComponentId &&
                   left.sourceModelResourceId == right.sourceModelResourceId &&
                   left.poseSequence == right.poseSequence &&
                   left.paletteHash == right.paletteHash &&
                   left.paletteCount == right.paletteCount;
        }

        [[nodiscard]] bool ResolveSkinningPaletteBinding(
            const RenderObject& object,
            const MeshGPUBuffers& buffers,
            const RenderDeformationFrameResources* resources,
            SkinningPaletteBufferBinding& outBuffers,
            SkinningPaletteSliceBinding& outSlice) noexcept
        {
            outBuffers = {};
            outSlice = {};
            if (!object.HasSkinningData())
                return true;
            if (!buffers.HasSkinningVertexData() || resources == nullptr ||
                !resources->IsComplete() || object.entityId == 0 ||
                !object.HasValidSkinningPalette())
            {
                return false;
            }
            const RenderDeformationPaletteSlice* const slice =
                resources->FindSlice(object.entityId);
            if (slice == nullptr || !slice->IsStructurallyValid() ||
                slice->matrixCount != object.skinningMatrices.size() ||
                !SamePaletteMetadata(
                    slice->currentMetadata, object.skinningPalette) ||
                slice->currentOffset >
                    resources->GetMatrixCount() - slice->matrixCount ||
                slice->previousOffset >
                    resources->GetMatrixCount() - slice->matrixCount ||
                (slice->previousValid &&
                 (!object.HasValidPreviousSkinningPalette() ||
                  !SamePaletteMetadata(
                      slice->previousMetadata,
                      object.previousSkinningPalette))) ||
                (!slice->previousValid &&
                 (object.HasValidPreviousSkinningPalette() ||
                  slice->previousMetadata.paletteCount != 0)))
            {
                return false;
            }
            outBuffers.currentPaletteBuffer = resources->GetCurrentPaletteBuffer();
            outBuffers.previousPaletteBuffer = resources->GetPreviousPaletteBuffer();
            outSlice.currentOffset = slice->currentOffset;
            outSlice.previousOffset = slice->previousOffset;
            outSlice.matrixCount = slice->matrixCount;
            outSlice.previousValid = slice->previousValid;
            return outBuffers.HasPalettes() && outSlice.IsWellFormed();
        }

        [[nodiscard]] bool HasExactDeformationBuffers(
            RenderGraphPassContext& context,
            const RenderDeformationFrameResources* resources,
            RGBufferHandle currentHandle,
            RGBufferHandle previousHandle) noexcept
        {
            return resources != nullptr && resources->IsComplete() &&
                   resources->GetMatrixCount() != 0 &&
                   currentHandle.IsValid() && previousHandle.IsValid() &&
                   context.GetBuffer(currentHandle) ==
                       resources->GetCurrentPaletteBuffer().Get() &&
                   context.GetBuffer(previousHandle) ==
                       resources->GetPreviousPaletteBuffer().Get();
        }

        [[nodiscard]] bool HasDrawFlag(RenderDrawFlags flags,
                                       RenderDrawFlags flag) noexcept
        {
            return (static_cast<uint32>(flags) &
                    static_cast<uint32>(flag)) != 0;
        }

        struct DrawRecord
        {
            MeshGPUBuffers buffers{};
            SubmeshGPUInfo submesh{};
            std::array<uint32, 1> objectDynamicOffsets{0};
            MaterialBindingSnapshot material{};
            RHIPipelineRef pipeline;
            MaterialBlendMode blendMode = MaterialBlendMode::Opaque;
            MaterialShadingModel shadingModel =
                MaterialShadingModel::StandardLit;
            bool skinned = false;
            bool requiresUV1 = false;
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
            RasterObjectBindingSnapshot bindings{};
            std::vector<DrawRecord> draws;
            RGTextureHandle colorHandle{};
            RGTextureHandle depthHandle{};
            RGTextureHandle opaqueSceneSnapshotReadHandle{};
            RGTextureHandle reactiveMaskHandle{};
            RGTextureHandle directionalShadowReadHandle{};
            RGTextureHandle rayTracedShadowMaskReadHandle{};
            RGBufferHandle lightConstantsReadHandle{};
            RGBufferHandle pointLightsReadHandle{};
            RGBufferHandle spotLightsReadHandle{};
            RGBufferHandle clusterConstantsReadHandle{};
            RGBufferHandle clusterDataReadHandle{};
            RGBufferHandle clusterLightIndicesReadHandle{};
            RGBufferHandle exposureStateReadHandle{};
            RGBufferHandle currentPaletteReadHandle{};
            RGBufferHandle previousPaletteReadHandle{};
            RGTextureViewHandle colorViewHandle{};
            RGTextureViewHandle depthViewHandle{};
            RGTextureViewHandle opaqueSceneSnapshotViewHandle{};
            RGTextureViewHandle reactiveMaskViewHandle{};
            RGTextureViewHandle directionalShadowViewHandle{};
            RGTextureViewHandle rayTracedShadowMaskViewHandle{};
            RGTextureViewHandle environmentIBLIrradianceViewHandle{};
            RGTextureViewHandle environmentIBLPrefilteredViewHandle{};
            RGTextureViewHandle environmentIBLBRDFLUTViewHandle{};
            RHIFormat colorFormat = RHIFormat::Unknown;
            bool depthAvailable = false;
            bool requestedEnabled = false;
            bool contextValid = false;
            bool recordingPrepared = false;
        };

        enum class DrawPreparationResult : uint8
        {
            Prepared = 0,
            SkippedResource,
            SkippedMaterial,
        };

        [[nodiscard]] const RenderPassExecutionPlan* FindTransparentPlan(
            const RenderFrameExecutionPlan* executionPlan) noexcept
        {
            if (executionPlan == nullptr)
            {
                return nullptr;
            }
            for (const RenderPassExecutionPlan& candidate : executionPlan->passes)
            {
                if (candidate.pass == RenderPassKind::Transparent)
                {
                    return &candidate;
                }
            }
            return nullptr;
        }

        [[nodiscard]] RenderPolicyReason GetTransparentSuccessReason(
            const RenderPassExecutionData& execution) noexcept
        {
            const RenderPassExecutionPlan* const passPlan =
                FindTransparentPlan(execution.GetExecutionPlan());
            return passPlan != nullptr ? passPlan->reason : RenderPolicyReason::None;
        }

        void PublishTransparentExecutionReport(
            const RenderPassExecutionData& execution,
            const TransparentPassDrawStats& stats,
            RenderExecutionStatus status,
            RenderPolicyReason reason)
        {
            RenderFrameExecutionReport* const frameReport =
                execution.GetExecutionReport();
            if (frameReport == nullptr)
            {
                return;
            }

            const RenderPassExecutionPlan* const passPlan =
                FindTransparentPlan(execution.GetExecutionPlan());
            for (RenderPassExecutionReport& report : frameReport->passes)
            {
                if (report.pass != RenderPassKind::Transparent)
                {
                    continue;
                }

                report.status = status;
                report.executedVisibility = passPlan != nullptr
                    ? passPlan->visibility : RenderVisibilityMode::Cpu;
                report.reason = reason;
                report.skippedPacketCount = passPlan != nullptr
                    ? passPlan->partition.skippedPacketCount : 0;
                report.materialBindingsAvailable = true;
                report.materialBindingCount = stats.materialBindingCount;
                report.materialFallbackBindingCount =
                    stats.materialFallbackBindingCount;
                report.materialTextureFlags = stats.materialTextureFlags;
                report.materialFallbackTextureFlags =
                    stats.materialFallbackTextureFlags;

                // Transparent rendering is deliberately a canonical Direct
                // lane. PacketRequiresDirect is an input policy fact, not a
                // runtime fallback from a GPU-driven lane.
                report.gpuDrivenLane = {};
                report.directLane.submission = RenderSubmissionMode::Direct;
                const bool directLanePlanned = passPlan != nullptr &&
                    passPlan->partition.directPacketCount != 0;
                report.directLane.status = directLanePlanned
                    ? status : RenderExecutionStatus::NotAttempted;
                report.directLane.reason = directLanePlanned
                    ? reason : RenderPolicyReason::None;
                report.directLane.executedCountsAvailable = directLanePlanned;
                report.directLane.executedPacketCount = directLanePlanned
                    ? stats.executedPacketCount : 0;
                report.directLane.executedDrawCount = directLanePlanned
                    ? stats.executedDrawCount : 0;
                break;
            }
            frameReport->frameSequence = execution.identity.frameSequence;
            if (status == RenderExecutionStatus::Failed)
            {
                frameReport->status = RenderExecutionStatus::Failed;
            }
        }

        void MarkTransparentPreflightFailure(GraphPassData& data,
                                             RenderPolicyReason reason)
        {
            if (data.results == nullptr)
            {
                return;
            }
            TransparentPassDrawStats& stats = data.results->transparentStats;
            stats.preflightFailed = true;
            stats.failureReason = reason;
            PublishTransparentExecutionReport(
                data.execution, stats, RenderExecutionStatus::Failed, reason);
        }

        [[nodiscard]] bool RetainResource(RenderGraphBuilder& builder,
                                          RefCounted* resource)
        {
            return !resource ||
                   builder.RetainSubmissionResource(Ref<RefCounted>(resource));
        }

        [[nodiscard]] bool RetainLightingBindingSnapshot(
            RenderGraphPassContext& context,
            const LightingBindingSnapshot& bindings)
        {
            return bindings.IsValid() &&
                context.RetainSubmissionResource(
                    Ref<RefCounted>(bindings.viewConstantBuffer.Get())) &&
                context.RetainSubmissionResource(
                    Ref<RefCounted>(bindings.directionalShadowView.Get())) &&
                context.RetainSubmissionResource(
                    Ref<RefCounted>(bindings.directionalShadowSampler.Get())) &&
                context.RetainSubmissionResource(
                    Ref<RefCounted>(bindings.lightConstantsBuffer.Get())) &&
                context.RetainSubmissionResource(
                    Ref<RefCounted>(bindings.pointLightsBuffer.Get())) &&
                context.RetainSubmissionResource(
                    Ref<RefCounted>(bindings.spotLightsBuffer.Get())) &&
                context.RetainSubmissionResource(
                    Ref<RefCounted>(bindings.rayTracedShadowMaskView.Get())) &&
                context.RetainSubmissionResource(
                    Ref<RefCounted>(bindings.clusterConstantsBuffer.Get())) &&
                context.RetainSubmissionResource(
                    Ref<RefCounted>(bindings.clusterBuffer.Get())) &&
                context.RetainSubmissionResource(
                    Ref<RefCounted>(bindings.clusterLightIndexBuffer.Get())) &&
                context.RetainSubmissionResource(
                    Ref<RefCounted>(bindings.environmentIBLIrradianceView.Get())) &&
                context.RetainSubmissionResource(
                    Ref<RefCounted>(bindings.environmentIBLPrefilteredView.Get())) &&
                context.RetainSubmissionResource(
                    Ref<RefCounted>(bindings.environmentIBLBRDFLUTView.Get())) &&
                context.RetainSubmissionResource(
                    Ref<RefCounted>(bindings.environmentIBLSampler.Get())) &&
                context.RetainSubmissionResource(
                    Ref<RefCounted>(bindings.exposureStateBuffer.Get())) &&
                context.RetainSubmissionResource(
                    Ref<RefCounted>(bindings.ambientOcclusionView.Get())) &&
                context.RetainSubmissionResource(
                    Ref<RefCounted>(bindings.frameSetLayout.Get())) &&
                context.RetainSubmissionResource(
                    Ref<RefCounted>(bindings.frameDescriptorSet.Get()));
        }

        [[nodiscard]] RGTextureViewHandle DeclareExactIBLRead(
            RenderGraphBuilder& builder,
            RGTextureHandle texture,
            const RHITextureViewRef& retainedView,
            const char* debugName)
        {
            const RHITextureDesc* const textureDesc =
                builder.GetTextureDesc(texture);
            if (!texture.IsValid() || textureDesc == nullptr || !retainedView ||
                retainedView->GetTexture() == nullptr)
            {
                return {};
            }

            RHITextureViewDesc viewDesc;
            viewDesc.format = retainedView->GetFormat();
            viewDesc.dimension = textureDesc->dimension;
            viewDesc.subresourceRange = retainedView->GetSubresourceRange();
            viewDesc.type = RHITextureViewType::ShaderResource;
            viewDesc.debugName = debugName;
            RGTextureViewHandle view =
                builder.CreateTextureView(texture, viewDesc);
            return builder.Read(
                view,
                MakeRGAccessDesc(
                    RHIResourceState::ShaderResource,
                    RHIShaderStage::Pixel));
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
            for (const DrawRecord& draw : data.draws)
            {
                if (!RetainResource(builder, draw.buffers.positionBuffer) ||
                    !RetainResource(builder, draw.buffers.normalBuffer) ||
                    !RetainResource(builder, draw.buffers.uvBuffer) ||
                    !RetainResource(builder, draw.buffers.uv1Buffer) ||
                    !RetainResource(builder, draw.buffers.tangentBuffer) ||
                    !RetainResource(builder, draw.buffers.boneIndicesBuffer) ||
                    !RetainResource(builder, draw.buffers.boneWeightsBuffer) ||
                    !RetainResource(builder, draw.buffers.indexBuffer) ||
                    !RetainResource(builder, draw.pipeline.Get()) ||
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

        [[nodiscard]] DrawPreparationResult PrepareDrawRecord(
            GraphPassData& data,
            const RenderDrawItem& item)
        {
            const RenderScene& scene = data.frameSnapshot->scene;
            if (item.objectIndex >= scene.GetObjectCount())
            {
                return DrawPreparationResult::SkippedResource;
            }

            const RenderObject& object = scene.GetObject(item.objectIndex);
            MeshGPUBuffers buffers = ResolveRenderMeshBuffers(
                data.resourceRegistry, object.mesh);
            if (!buffers.IsValid() || !buffers.normalBuffer ||
                !buffers.uvBuffer || !buffers.tangentBuffer ||
                !item.packet.pipelineKey.materialPolicyValid ||
                (item.packet.pipelineKey.requiresUV1 &&
                 !buffers.HasTextureCoordinateSet(1)) ||
                item.submeshIndex >= buffers.submeshes.size())
            {
                return DrawPreparationResult::SkippedResource;
            }
            const bool skinned = object.HasSkinningData();
            if (skinned && !buffers.HasSkinningVertexData())
            {
                return DrawPreparationResult::SkippedResource;
            }

            DrawRecord record;
            record.buffers = buffers;
            record.submesh = buffers.submeshes[item.submeshIndex];
            record.skinned = skinned;
            record.requiresUV1 = item.packet.pipelineKey.requiresUV1;
            record.blendMode = item.packet.pipelineKey.blendMode;
            record.shadingModel = item.packet.pipelineKey.shadingModel;
            record.pipeline = RHIPipelineRef(
                data.pipelineCache->GetTransparentMrtPipeline(
                    data.colorFormat,
                    skinned ? DefaultLitDirectVertexInputMode::Skinned
                            : DefaultLitDirectVertexInputMode::Rigid,
                    item.packet.pipelineKey.cullMode,
                    item.packet.pipelineKey.reverseWinding,
                    item.packet.pipelineKey.blendMode));
            if (!record.pipeline)
            {
                return DrawPreparationResult::SkippedResource;
            }
            MaterialBindingOptions materialOptions;
            materialOptions.allowNormalMap = buffers.HasNormalMapTangentBasis();
            if (!data.materialSystem->CreateMaterialBindingSnapshot(
                    item.material, nullptr, materialOptions, record.material) ||
                !record.material.IsDrawable())
            {
                return DrawPreparationResult::SkippedMaterial;
            }
            TransparentPassDrawStats& stats = data.results->transparentStats;
            ++stats.materialBindingCount;
            if (record.material.binding.usedFallback)
            {
                ++stats.materialFallbackBindingCount;
                ++stats.translucencyFallbackCount;
            }
            stats.materialTextureFlags |= record.material.binding.textureFlags;
            stats.materialFallbackTextureFlags |=
                record.material.binding.fallbackTextureFlags;

            const ViewData& view = data.execution.view;
            const bool previousWorldViewProjectionValid =
                object.previousWorldMatrixValid != 0 &&
                view.previousViewProjectionValid != 0 &&
                !view.resetTemporalHistory;
            const uint32 objectSlot = data.bindings.objectCursor;
            SkinningPaletteBufferBinding skinningBuffers;
            SkinningPaletteSliceBinding skinningSlice;
            if (!ResolveSkinningPaletteBinding(
                    object, buffers,
                    data.execution.deformationResources.get(),
                    skinningBuffers, skinningSlice) ||
                !data.pipelineCache->UpdateRasterObjectBindingSnapshotObject(
                    data.bindings,
                    object.worldMatrix,
                    object.normalMatrix,
                    object.previousWorldMatrix,
                    view.previousViewProjectionMatrix,
                    previousWorldViewProjectionValid,
                    HasDrawFlag(
                        item.packet.flags,
                        RenderDrawFlags::ReceivesShadow),
                    skinningSlice))
            {
                return DrawPreparationResult::SkippedResource;
            }
            record.objectDynamicOffsets = PipelineCache::BuildSingleDynamicOffset(
                static_cast<uint64>(objectSlot) * data.bindings.objectConstantStride);
            data.draws.push_back(std::move(record));
            return DrawPreparationResult::Prepared;
        }

        [[nodiscard]] bool PrepareRecording(GraphPassData& data,
                                            RenderGraphBuilder& builder)
        {
            if (data.results != nullptr)
            {
                data.results->transparentStats = {};
                data.results->transparentStats.requested = data.requestedEnabled;
                data.results->transparentStats.candidateDrawItemCount =
                    data.frameSnapshot != nullptr
                    ? static_cast<uint32>(
                          data.frameSnapshot->transparentDrawItems.size())
                    : 0;
                data.results->transparentStats.transparentOrderHash =
                    data.frameSnapshot != nullptr
                    ? BuildTransparentDrawOrderHash(
                          data.frameSnapshot->transparentDrawItems)
                    : 0;
                data.results->transparentStats.transparentOrderValid =
                    data.frameSnapshot != nullptr &&
                    IsTransparentDrawListStrictlyOrdered(
                        data.frameSnapshot->transparentDrawItems);
            }
            if (!data.contextValid || !data.results || !data.frameSnapshot ||
                !data.requestedEnabled || !data.pipelineCache ||
                !data.materialSystem || !data.resourceRegistry)
            {
                RVX_CORE_WARN(
                    "TransparentPass: base preflight facts are inconsistent "
                    "(contextValid={}, results={}, frameSnapshot={}, requestedEnabled={}, "
                    "pipelineCache={}, materialSystem={}, resourceRegistry={})",
                    data.contextValid,
                    data.results != nullptr,
                    data.frameSnapshot != nullptr,
                    data.requestedEnabled,
                    data.pipelineCache != nullptr,
                    data.materialSystem != nullptr,
                    data.resourceRegistry != nullptr);
                MarkTransparentPreflightFailure(
                    data, RenderPolicyReason::InconsistentFacts);
                return false;
            }

            const ViewData& view = data.execution.view;
            const OpaqueSceneSnapshotRecordInput& opaqueSnapshot =
                data.execution.opaqueSceneSnapshot;
            if (data.frameSnapshot->transparentDrawItems.empty())
            {
                TransparentPassDrawStats& stats = data.results->transparentStats;
                stats.noWork = true;
                stats.failureReason = RenderPolicyReason::None;
                PublishTransparentExecutionReport(
                    data.execution,
                    stats,
                    RenderExecutionStatus::Completed,
                    GetTransparentSuccessReason(data.execution));
                return false;
            }
            if (!IsTransparentDrawListStrictlyOrdered(
                    data.frameSnapshot->transparentDrawItems) ||
                !view.colorTarget.IsValid() ||
                !opaqueSnapshot.IsCompatibleWith(data.identity) ||
                !view.reactiveMaskTarget.IsValid() ||
                opaqueSnapshot.IsEmpty())
            {
                RVX_CORE_WARN(
                    "TransparentPass: draw-order or scene-input facts are inconsistent "
                    "(orderValid={}, colorTarget={}, opaqueSnapshotEmpty={}, "
                    "opaqueSnapshotCompatible={}, opaqueSnapshotTexture={}, "
                    "opaqueSnapshotReceipt={}, reactiveMaskTarget={})",
                    IsTransparentDrawListStrictlyOrdered(
                        data.frameSnapshot->transparentDrawItems),
                    view.colorTarget.IsValid(),
                    opaqueSnapshot.IsEmpty(),
                    opaqueSnapshot.IsCompatibleWith(data.identity),
                    opaqueSnapshot.texture.IsValid(),
                    opaqueSnapshot.receipt != nullptr,
                    view.reactiveMaskTarget.IsValid());
                MarkTransparentPreflightFailure(
                    data, RenderPolicyReason::InconsistentFacts);
                return false;
            }
            const RHITextureDesc* colorDesc =
                builder.GetTextureDesc(view.colorTarget);
            if (colorDesc == nullptr)
            {
                MarkTransparentPreflightFailure(
                    data, RenderPolicyReason::ResourcesUnavailable);
                return false;
            }
            data.colorFormat = colorDesc->format;
            const RHITextureDesc* const opaqueSnapshotDesc =
                builder.GetTextureDesc(opaqueSnapshot.texture);
            const RHITextureDesc* const reactiveMaskDesc =
                builder.GetTextureDesc(view.reactiveMaskTarget);
            if (data.colorFormat != RHIFormat::RGBA16_FLOAT ||
                !HasFlag(colorDesc->usage, RHITextureUsage::CopySrc) ||
                opaqueSnapshot.texture.index == view.colorTarget.index ||
                opaqueSnapshotDesc == nullptr ||
                opaqueSnapshotDesc->format != RHIFormat::RGBA16_FLOAT ||
                opaqueSnapshotDesc->width != colorDesc->width ||
                opaqueSnapshotDesc->height != colorDesc->height ||
                opaqueSnapshotDesc->sampleCount != colorDesc->sampleCount ||
                !HasFlag(opaqueSnapshotDesc->usage,
                         RHITextureUsage::ShaderResource) ||
                !HasFlag(opaqueSnapshotDesc->usage, RHITextureUsage::CopyDst) ||
                reactiveMaskDesc == nullptr ||
                reactiveMaskDesc->format != RHIFormat::R8_UNORM ||
                reactiveMaskDesc->width != colorDesc->width ||
                reactiveMaskDesc->height != colorDesc->height ||
                reactiveMaskDesc->sampleCount != colorDesc->sampleCount ||
                !HasFlag(reactiveMaskDesc->usage,
                         RHITextureUsage::RenderTarget))
            {
                MarkTransparentPreflightFailure(
                    data, RenderPolicyReason::ResourcesUnavailable);
                return false;
            }
            int32 lastPriority = std::numeric_limits<int32>::min();
            for (const RenderDrawItem& item :
                 data.frameSnapshot->transparentDrawItems)
            {
                if (item.translucencySortPriority != lastPriority)
                {
                    ++data.results->transparentStats.translucencyPriorityBucketCount;
                    lastPriority = item.translucencySortPriority;
                }
            }
            data.results->transparentStats.opaqueSceneSnapshotReady = true;
            data.depthAvailable = view.depthTarget.IsValid();

            const uint64 lightingPlanIdentity =
                data.execution.lightingPlan->GetIdentityHash();
            const RenderLightingFrameResourceSnapshot& lightingResources =
                *data.execution.lightingResources;
            const bool lightingGraphInputsReady =
                lightingResources.HasCurrentGraphResources(data.identity) &&
                builder.GetBufferDesc(lightingResources.lightConstants) != nullptr &&
                builder.GetBufferDesc(lightingResources.pointLights) != nullptr &&
                builder.GetBufferDesc(lightingResources.spotLights) != nullptr &&
                builder.GetBufferDesc(lightingResources.clusterConstants) != nullptr &&
                builder.GetBufferDesc(lightingResources.clusterData) != nullptr &&
                builder.GetBufferDesc(
                    lightingResources.clusterLightIndices) != nullptr &&
                view.exposureState.IsValid() &&
                builder.GetBufferDesc(view.exposureState) != nullptr;
            if (!lightingGraphInputsReady)
            {
                MarkTransparentPreflightFailure(
                    data, RenderPolicyReason::ResourcesUnavailable);
                return false;
            }
            data.lightConstantsReadHandle = builder.Read(
                lightingResources.lightConstants,
                RHIResourceState::ConstantBuffer,
                RHIShaderStage::Pixel);
            data.pointLightsReadHandle = builder.Read(
                lightingResources.pointLights,
                RHIResourceState::ShaderResource,
                RHIShaderStage::Pixel);
            data.spotLightsReadHandle = builder.Read(
                lightingResources.spotLights,
                RHIResourceState::ShaderResource,
                RHIShaderStage::Pixel);
            data.clusterConstantsReadHandle = builder.Read(
                lightingResources.clusterConstants,
                RHIResourceState::ConstantBuffer,
                RHIShaderStage::Pixel);
            data.clusterDataReadHandle = builder.Read(
                lightingResources.clusterData,
                RHIResourceState::ShaderResource,
                RHIShaderStage::Pixel);
            data.clusterLightIndicesReadHandle = builder.Read(
                lightingResources.clusterLightIndices,
                RHIResourceState::ShaderResource,
                RHIShaderStage::Pixel);
            data.exposureStateReadHandle = builder.Read(
                view.exposureState,
                RHIResourceState::ShaderResource,
                RHIShaderStage::Pixel);
            if (!data.lightConstantsReadHandle.IsValid() ||
                !data.pointLightsReadHandle.IsValid() ||
                !data.spotLightsReadHandle.IsValid() ||
                !data.clusterConstantsReadHandle.IsValid() ||
                !data.clusterDataReadHandle.IsValid() ||
                !data.clusterLightIndicesReadHandle.IsValid() ||
                !data.exposureStateReadHandle.IsValid())
            {
                MarkTransparentPreflightFailure(
                    data, RenderPolicyReason::ResourcesUnavailable);
                return false;
            }
            const DirectionalShadowRecordOutput& directionalShadow =
                data.execution.directionalShadow;
            if (!directionalShadow.IsCompatibleWith(
                    data.identity, lightingPlanIdentity))
            {
                MarkTransparentPreflightFailure(
                    data, RenderPolicyReason::InconsistentFacts);
                return false;
            }
            if (directionalShadow.enabled)
            {
                RGTextureHandle shadowMap = directionalShadow.shadowMap;
                shadowMap.hasSubresourceRange = true;
                shadowMap.subresourceRange = RHISubresourceRange{
                    0, RVX_ALL_MIPS, 0, RVX_ALL_LAYERS, RHITextureAspect::Depth};
                const RHITextureDesc* shadowDesc = builder.GetTextureDesc(shadowMap);
                if (shadowDesc == nullptr)
                {
                    MarkTransparentPreflightFailure(
                        data, RenderPolicyReason::ResourcesUnavailable);
                    return false;
                }
                RHITextureViewDesc shadowViewDesc;
                shadowViewDesc.format = shadowDesc->format;
                shadowViewDesc.dimension = shadowDesc->dimension;
                shadowViewDesc.subresourceRange = shadowMap.subresourceRange;
                shadowViewDesc.type = RHITextureViewType::ShaderResource;
                shadowViewDesc.debugName = "TransparentDirectionalShadowSRV";
                data.directionalShadowReadHandle = shadowMap;
                data.directionalShadowViewHandle = builder.CreateTextureView(
                    shadowMap, shadowViewDesc);
                data.directionalShadowViewHandle = builder.Read(
                    data.directionalShadowViewHandle,
                    MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                     RHIShaderStage::Pixel));
                if (!data.directionalShadowViewHandle.IsValid())
                {
                    MarkTransparentPreflightFailure(
                        data, RenderPolicyReason::ResourcesUnavailable);
                    return false;
                }
            }

            const RayTracedShadowRecordOutput& rayTracedShadow =
                data.execution.rayTracedShadow;
            if (!rayTracedShadow.IsCompatibleWith(data.identity))
            {
                MarkTransparentPreflightFailure(
                    data, RenderPolicyReason::InconsistentFacts);
                return false;
            }
            if (rayTracedShadow.enabled)
            {
                const RHITextureDesc* rayTracedShadowDesc =
                    builder.GetTextureDesc(rayTracedShadow.shadowMask);
                if (rayTracedShadowDesc == nullptr)
                {
                    MarkTransparentPreflightFailure(
                        data, RenderPolicyReason::ResourcesUnavailable);
                    return false;
                }
                RHITextureViewDesc rayTracedShadowViewDesc;
                rayTracedShadowViewDesc.format = rayTracedShadowDesc->format;
                rayTracedShadowViewDesc.dimension = rayTracedShadowDesc->dimension;
                rayTracedShadowViewDesc.subresourceRange = RHISubresourceRange::All();
                rayTracedShadowViewDesc.type = RHITextureViewType::ShaderResource;
                rayTracedShadowViewDesc.debugName = "TransparentRayTracedShadowMaskSRV";
                data.rayTracedShadowMaskReadHandle = rayTracedShadow.shadowMask;
                data.rayTracedShadowMaskViewHandle = builder.CreateTextureView(
                    data.rayTracedShadowMaskReadHandle, rayTracedShadowViewDesc);
                data.rayTracedShadowMaskViewHandle = builder.Read(
                    data.rayTracedShadowMaskViewHandle,
                    MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                     RHIShaderStage::Pixel));
                if (!data.rayTracedShadowMaskViewHandle.IsValid())
                {
                    MarkTransparentPreflightFailure(
                        data, RenderPolicyReason::ResourcesUnavailable);
                    return false;
                }
            }

            const uint32 candidateCount = static_cast<uint32>(
                data.frameSnapshot->transparentDrawItems.size());
            SkinningPaletteBufferBinding skinningPalettes;
            const std::shared_ptr<const RenderDeformationFrameResources>&
                deformationResources = data.execution.deformationResources;
            if (deformationResources != nullptr &&
                deformationResources->GetMatrixCount() != 0)
            {
                if (!deformationResources->MatchesIdentity(
                        MakeRenderDeformationFrameIdentity(data.identity)) ||
                    !deformationResources->MatchesGraphProvenance(
                        *data.identity.graph) ||
                    builder.GetBufferDesc(
                        deformationResources->GetCurrentPaletteHandle()) == nullptr ||
                    builder.GetBufferDesc(
                        deformationResources->GetPreviousPaletteHandle()) == nullptr)
                {
                    MarkTransparentPreflightFailure(
                        data, RenderPolicyReason::ResourcesUnavailable);
                    return false;
                }
                data.currentPaletteReadHandle = builder.Read(
                    deformationResources->GetCurrentPaletteHandle(),
                    RHIResourceState::ShaderResource,
                    RHIShaderStage::Vertex);
                data.previousPaletteReadHandle = builder.Read(
                    deformationResources->GetPreviousPaletteHandle(),
                    RHIResourceState::ShaderResource,
                    RHIShaderStage::Vertex);
                if (!data.currentPaletteReadHandle.IsValid() ||
                    !data.previousPaletteReadHandle.IsValid())
                {
                    MarkTransparentPreflightFailure(
                        data, RenderPolicyReason::ResourcesUnavailable);
                    return false;
                }
                skinningPalettes.currentPaletteBuffer =
                    deformationResources->GetCurrentPaletteBuffer();
                skinningPalettes.previousPaletteBuffer =
                    deformationResources->GetPreviousPaletteBuffer();
            }
            if (!data.pipelineCache->CreateRasterObjectBindingSnapshot(
                    candidateCount, skinningPalettes, data.bindings))
            {
                MarkTransparentPreflightFailure(
                    data, RenderPolicyReason::ResourcesUnavailable);
                return false;
            }
            for (const RenderDrawItem& item :
                 data.frameSnapshot->transparentDrawItems)
            {
                if (!DeclareMaterialTextureGraphReads(
                        builder,
                        data.resourceRegistry,
                        item.material,
                        *data.results))
                {
                    MarkTransparentPreflightFailure(
                        data, RenderPolicyReason::ResourcesUnavailable);
                    return false;
                }
            }

            data.draws.reserve(candidateCount);
            // The frame snapshot is already sorted back-to-front by scene
            // extraction. Do not regroup or sort here: valid entries retain
            // their relative blend order while invalid entries are skipped.
            for (const RenderDrawItem& item : data.frameSnapshot->transparentDrawItems)
            {
                const DrawPreparationResult result = PrepareDrawRecord(data, item);
                if (result == DrawPreparationResult::SkippedMaterial)
                {
                    ++data.results->transparentStats.skippedMaterialBindingCount;
                }
                else if (result == DrawPreparationResult::SkippedResource)
                {
                    ++data.results->transparentStats.skippedResourceCount;
                }
            }
            data.results->transparentStats.preparedDrawItemCount =
                static_cast<uint32>(data.draws.size());
            if (data.draws.empty())
            {
                MarkTransparentPreflightFailure(
                    data, RenderPolicyReason::ResourcesUnavailable);
                return false;
            }

            // RenderGraph access declarations are irreversible. Establish all
            // submission ownership first, so a sealed/rejected batch produces
            // a genuine no-op instead of a graph-visible write without work.
            if (!RetainDrawResources(builder, data))
            {
                MarkTransparentPreflightFailure(
                    data, RenderPolicyReason::ResourcesUnavailable);
                return false;
            }

            if (view.textureIBLEnabled != 0)
            {
                const RenderLightingFrameResourceSnapshot& lightingResources =
                    *data.execution.lightingResources;
                data.environmentIBLIrradianceViewHandle = DeclareExactIBLRead(
                    builder,
                    lightingResources.environmentIBLIrradiance,
                    lightingResources.environmentIBLIrradianceView,
                    "TransparentEnvironmentIrradianceSRV");
                data.environmentIBLPrefilteredViewHandle = DeclareExactIBLRead(
                    builder,
                    lightingResources.environmentIBLPrefiltered,
                    lightingResources.environmentIBLPrefilteredView,
                    "TransparentEnvironmentPrefilteredSRV");
                data.environmentIBLBRDFLUTViewHandle = DeclareExactIBLRead(
                    builder,
                    lightingResources.environmentIBLBRDFLUT,
                    lightingResources.environmentIBLBRDFLUTView,
                    "TransparentEnvironmentBRDFLUTSRV");
                const bool iblReadsDeclared =
                    data.environmentIBLIrradianceViewHandle.IsValid() &&
                    data.environmentIBLPrefilteredViewHandle.IsValid() &&
                    data.environmentIBLBRDFLUTViewHandle.IsValid();
                if (!iblReadsDeclared)
                {
                    data.draws.clear();
                    MarkTransparentPreflightFailure(
                        data, RenderPolicyReason::ResourcesUnavailable);
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
                MarkTransparentPreflightFailure(
                    data, RenderPolicyReason::ResourcesUnavailable);
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
                    MarkTransparentPreflightFailure(
                        data, RenderPolicyReason::ResourcesUnavailable);
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
                {
                    MarkTransparentPreflightFailure(
                        data, RenderPolicyReason::ResourcesUnavailable);
                    return false;
                }
            }
            data.opaqueSceneSnapshotReadHandle = builder.Read(
                opaqueSnapshot.texture,
                RHIResourceState::ShaderResource,
                RHIShaderStage::Pixel);
            RHITextureViewDesc snapshotViewDesc;
            snapshotViewDesc.format = RHIFormat::RGBA16_FLOAT;
            snapshotViewDesc.dimension = RHITextureDimension::Texture2D;
            snapshotViewDesc.type = RHITextureViewType::ShaderResource;
            snapshotViewDesc.debugName = "TransparentOpaqueSceneSnapshotSRV";
            data.opaqueSceneSnapshotViewHandle = builder.CreateTextureView(
                data.opaqueSceneSnapshotReadHandle, snapshotViewDesc);
            data.opaqueSceneSnapshotViewHandle = builder.Read(
                data.opaqueSceneSnapshotViewHandle,
                MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                 RHIShaderStage::Pixel));
            data.reactiveMaskHandle = view.reactiveMaskTarget;
            RHITextureViewDesc reactiveViewDesc;
            reactiveViewDesc.format = RHIFormat::R8_UNORM;
            reactiveViewDesc.dimension = RHITextureDimension::Texture2D;
            reactiveViewDesc.type = RHITextureViewType::RenderTarget;
            reactiveViewDesc.debugName = "TransparentReactiveMaskRTV";
            data.reactiveMaskViewHandle = builder.CreateTextureView(
                data.reactiveMaskHandle, reactiveViewDesc);
            data.reactiveMaskViewHandle = builder.ReadWrite(
                data.reactiveMaskViewHandle,
                MakeRGAccessDesc(RHIResourceState::RenderTarget,
                                 RHIShaderStage::Pixel));
            if (!data.opaqueSceneSnapshotReadHandle.IsValid() ||
                !data.opaqueSceneSnapshotViewHandle.IsValid() ||
                !data.reactiveMaskViewHandle.IsValid())
            {
                MarkTransparentPreflightFailure(
                    data, RenderPolicyReason::ResourcesUnavailable);
                return false;
            }
            data.results->transparentStats.reactiveMaskAttachmentBound = true;
            data.recordingPrepared = true;
            return true;
        }

        [[nodiscard]] bool ExecuteRecording(const GraphPassData& data,
                                            RenderGraphPassContext& context)
        {
            RHICommandContext& ctx = context.Commands();
            if (!data.contextValid || !data.results ||
                data.results->identity != data.identity || !data.recordingPrepared ||
                data.draws.empty() ||
                !data.bindings.IsValid() ||
                data.execution.lightingPlan == nullptr ||
                !data.execution.lightingPlan->IsValid() ||
                data.execution.lightingPlan->GetIdentityHash() == 0 ||
                data.execution.lightingResources == nullptr ||
                !data.execution.lightingResources->IsComplete() ||
                data.execution.lightingResources->lightingPlanIdentity !=
                    data.execution.lightingPlan->GetIdentityHash() ||
                data.execution.frameSnapshot == nullptr ||
                data.execution.frameSnapshot->lightingPlan !=
                    data.execution.lightingPlan ||
                data.execution.frameSnapshot->lightingResources !=
                    data.execution.lightingResources ||
                data.execution.view.textureIBLEnabled != static_cast<uint32>(
                    data.execution.lightingResources->textureIBLEnabled) ||
                data.execution.lightingPlan->GetCSMRequest().inputsReady !=
                    data.execution.directionalShadow.enabled ||
                !data.execution.directionalShadow.IsCompatibleWith(
                    data.identity,
                    data.execution.lightingPlan->GetIdentityHash()) ||
                !data.execution.rayTracedShadow.IsCompatibleWith(data.identity) ||
                !data.identity.IsValid() || data.identity.graph == nullptr ||
                !data.identity.Matches(*data.identity.graph) ||
                !data.colorHandle.IsValid() ||
                data.colorHandle.graphIdentity != data.identity.graphIdentity ||
                data.colorHandle.recordingGeneration !=
                    data.identity.graphRecordingGeneration ||
                !data.opaqueSceneSnapshotReadHandle.IsValid() ||
                !data.opaqueSceneSnapshotViewHandle.IsValid() ||
                !data.reactiveMaskHandle.IsValid() ||
                !data.reactiveMaskViewHandle.IsValid() ||
                data.opaqueSceneSnapshotReadHandle.graphIdentity !=
                    data.identity.graphIdentity ||
                data.opaqueSceneSnapshotReadHandle.recordingGeneration !=
                    data.identity.graphRecordingGeneration ||
                data.reactiveMaskHandle.graphIdentity != data.identity.graphIdentity ||
                data.reactiveMaskHandle.recordingGeneration !=
                    data.identity.graphRecordingGeneration ||
                (data.depthAvailable &&
                 (!data.depthHandle.IsValid() ||
                  data.depthHandle.graphIdentity != data.identity.graphIdentity ||
                  data.depthHandle.recordingGeneration !=
                      data.identity.graphRecordingGeneration)))
            {
                return false;
            }
            const bool hasSkinnedDraw = std::any_of(
                data.draws.begin(), data.draws.end(),
                [](const DrawRecord& draw) { return draw.skinned; });
            if (hasSkinnedDraw && !HasExactDeformationBuffers(
                    context, data.execution.deformationResources.get(),
                    data.currentPaletteReadHandle,
                    data.previousPaletteReadHandle))
            {
                RVX_RENDER_WARN(
                    "TransparentPass deformation graph resources do not match the recording snapshot");
                return false;
            }

            RHITexture* colorTexture = context.GetTexture(data.colorHandle);
            RHITexture* depthTexture = data.depthAvailable
                ? context.GetTexture(data.depthHandle) : nullptr;
            if (!colorTexture || (data.depthAvailable && !depthTexture))
            {
                return false;
            }
            RHITextureView* colorView =
                context.GetTextureView(data.colorViewHandle);
            RHITextureView* depthView = data.depthAvailable
                ? context.GetTextureView(data.depthViewHandle) : nullptr;
            if (!colorView || (data.depthAvailable && !depthView))
            {
                RVX_CORE_WARN("TransparentPass: failed to resolve graph-owned attachment views");
                return false;
            }

            ViewData drawView = data.execution.view;
            const uint64 lightingPlanIdentity =
                data.execution.lightingPlan->GetIdentityHash();
            if (!ResolveDirectionalShadowViewData(
                    drawView,
                    data.execution.directionalShadow,
                    data.identity,
                    lightingPlanIdentity))
            {
                RVX_CORE_WARN("TransparentPass: invalid directional-shadow output");
                return false;
            }

            DirectionalShadowFrameResources directionalShadowResources;
            if (data.execution.directionalShadow.enabled)
            {
                if (!data.directionalShadowReadHandle.IsValid() ||
                    !data.directionalShadowViewHandle.IsValid())
                {
                    RVX_CORE_WARN("TransparentPass: directional shadow has no graph read");
                    return false;
                }
                directionalShadowResources.shadowMapView =
                    context.GetTextureView(data.directionalShadowViewHandle);
                if (directionalShadowResources.shadowMapView == nullptr)
                {
                    RVX_CORE_WARN("TransparentPass: failed to resolve directional shadow view");
                    return false;
                }
                directionalShadowResources.enabled = true;
            }

            RayTracedShadowFrameResources rayTracedShadowResources;
            const RayTracedShadowRecordOutput& rayTracedShadow =
                data.execution.rayTracedShadow;
            const bool rayTracedStateMatches =
                rayTracedShadow.executionState != nullptr &&
                rayTracedShadow.executionState->identity == rayTracedShadow.identity;
            const bool rayTracedExecutionFailed = rayTracedStateMatches &&
                rayTracedShadow.executionState->executionFailed.load(
                    std::memory_order_acquire);
            const bool rayTracedExecutionReady = rayTracedStateMatches &&
                !rayTracedExecutionFailed &&
                rayTracedShadow.executionState->dispatchReady.load(
                    std::memory_order_acquire);
            if (rayTracedExecutionReady &&
                data.rayTracedShadowMaskReadHandle.IsValid() &&
                data.rayTracedShadowMaskViewHandle.IsValid())
            {
                rayTracedShadowResources.shadowMaskView = context.GetTextureView(
                    data.rayTracedShadowMaskViewHandle);
                rayTracedShadowResources.enabled =
                    rayTracedShadowResources.shadowMaskView != nullptr;
            }
            if (rayTracedExecutionFailed)
            {
                RVX_RENDER_WARN(
                    "TransparentPass: ray-traced shadow producer failed; using raster shadowing");
            }
            drawView.rayTracedShadowEnabled =
                rayTracedShadowResources.enabled ? 1 : 0;
            drawView.rayTracedShadowMode = RayTracedShadowMode::ComplementRaster;
            if (rayTracedShadowResources.enabled && rayTracedShadow.enabled)
            {
                drawView.rayTracedShadowFilterRadiusPixels = std::max(
                    0.0f, rayTracedShadow.filterRadiusTexels);
                drawView.rayTracedShadowMode = rayTracedShadow.mode;
                if (drawView.rayTracedShadowMode ==
                    RayTracedShadowMode::ReplaceRaster)
                {
                    DirectionalShadowRecordOutput disabledDirectionalShadow =
                        data.execution.directionalShadow;
                    disabledDirectionalShadow.enabled = false;
                    if (!ResolveDirectionalShadowViewData(
                            drawView,
                            disabledDirectionalShadow,
                            data.identity,
                            lightingPlanIdentity))
                    {
                        return false;
                    }
                    directionalShadowResources = {};
                }
            }

            const FrozenFrameLightResources& frozen =
                data.execution.lightingResources->frozenLightResources;
            const bool graphBuffersMatchDescriptors =
                context.GetBuffer(data.lightConstantsReadHandle) ==
                    frozen.lightConstantsBuffer.Get() &&
                context.GetBuffer(data.pointLightsReadHandle) ==
                    frozen.pointLightsBuffer.Get() &&
                context.GetBuffer(data.spotLightsReadHandle) ==
                    frozen.spotLightsBuffer.Get() &&
                context.GetBuffer(data.clusterConstantsReadHandle) ==
                    frozen.clusterConstantsBuffer.Get() &&
                context.GetBuffer(data.clusterDataReadHandle) ==
                    frozen.clusterBuffer.Get() &&
                context.GetBuffer(data.clusterLightIndicesReadHandle) ==
                    frozen.clusterLightIndexBuffer.Get();
            if (!graphBuffersMatchDescriptors)
            {
                RVX_RENDER_WARN(
                    "TransparentPass: graph light reads do not match frozen descriptor allocations");
                return false;
            }
            RHIBuffer* const exposureState = context.GetBuffer(
                data.exposureStateReadHandle);
            if (exposureState == nullptr)
            {
                RVX_CORE_WARN(
                    "TransparentPass: Forward HDR rendering requires an exact committed exposure-state graph read");
                return false;
            }

            LightingBindingSnapshotRequest lightingRequest;
            lightingRequest.lightingPlanIdentity = lightingPlanIdentity;
            lightingRequest.frozenLightResources =
                data.execution.lightingResources->frozenLightResources;
            lightingRequest.directionalShadow = directionalShadowResources;
            lightingRequest.rayTracedShadow = rayTracedShadowResources;
            EnvironmentIBLFrameResources environmentIBLResources;
            const bool exactIBLViews =
                data.execution.lightingResources->ResolveEnvironmentResources(
                    data.execution.lightingResources->textureIBLEnabled
                        ? context.GetTextureView(
                              data.environmentIBLIrradianceViewHandle)
                        : nullptr,
                    data.execution.lightingResources->textureIBLEnabled
                        ? context.GetTextureView(
                              data.environmentIBLPrefilteredViewHandle)
                        : nullptr,
                    data.execution.lightingResources->textureIBLEnabled
                        ? context.GetTextureView(
                              data.environmentIBLBRDFLUTViewHandle)
                        : nullptr,
                    environmentIBLResources);
            if (!exactIBLViews)
            {
                RVX_CORE_WARN(
                    "TransparentPass: graph IBL reads do not match the retained LightingFramePlan allocations");
                return false;
            }
            lightingRequest.environmentIBL = environmentIBLResources;
            lightingRequest.exposureStateBuffer = RHIBufferRef(exposureState);
            RHITextureView* const opaqueSceneSnapshotView = context.GetTextureView(
                data.opaqueSceneSnapshotViewHandle);
            RHITextureView* const reactiveMaskView = context.GetTextureView(
                data.reactiveMaskViewHandle);
            if (opaqueSceneSnapshotView == nullptr || reactiveMaskView == nullptr ||
                opaqueSceneSnapshotView == colorView ||
                !data.execution.opaqueSceneSnapshot.IsCompatibleWith(data.identity) ||
                !data.execution.opaqueSceneSnapshot.receipt->copied.load(
                    std::memory_order_acquire))
            {
                RVX_CORE_WARN(
                    "TransparentPass: opaque HDR typed-input receipt/reactive MRT binding is invalid "
                    "(empty={}, compatible={}, texture={}, receipt={})",
                    data.execution.opaqueSceneSnapshot.IsEmpty(),
                    data.execution.opaqueSceneSnapshot.IsCompatibleWith(data.identity),
                    data.execution.opaqueSceneSnapshot.texture.IsValid(),
                    data.execution.opaqueSceneSnapshot.receipt != nullptr);
                return false;
            }
            data.results->transparentStats.opaqueSceneSnapshotCopied = true;
            lightingRequest.opaqueSceneSnapshotView =
                RHITextureViewRef(opaqueSceneSnapshotView);
            lightingRequest.opaqueSceneSnapshotSampler =
                data.pipelineCache->GetEnvironmentIBLSamplerRef();
            LightingBindingSnapshot lightingBindings;
            if (!data.pipelineCache->CreateLightingBindingSnapshot(
                    drawView, lightingRequest, lightingBindings) ||
                !lightingBindings.MatchesLightingPlan(lightingPlanIdentity) ||
                !RetainLightingBindingSnapshot(context, lightingBindings))
            {
                RVX_CORE_WARN("TransparentPass: could not create record-owned lighting bindings");
                return false;
            }

            RHIRenderPassDesc renderPassDesc;
            renderPassDesc.AddColorAttachment(
                colorView, RHILoadOp::Load, RHIStoreOp::Store);
            renderPassDesc.AddColorAttachment(
                reactiveMaskView, RHILoadOp::Load, RHIStoreOp::Store);
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

            RHIPipeline* currentPipeline = nullptr;
            TransparentPassDrawStats& stats = data.results->transparentStats;
            bool executionFailed = false;
            for (const DrawRecord& draw : data.draws)
            {
                if (!draw.buffers.IsValid() || !draw.buffers.positionBuffer ||
                    !draw.buffers.indexBuffer || !draw.material.IsDrawable())
                {
                    ++stats.skippedExecutionDrawCount;
                    executionFailed = true;
                    continue;
                }

                RHIPipeline* pipeline = draw.pipeline.Get();
                if (pipeline != currentPipeline)
                {
                    ctx.SetPipeline(pipeline);
                    ctx.SetDescriptorSet(
                        0, lightingBindings.frameDescriptorSet.Get());
                    currentPipeline = pipeline;
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
                {
                    ctx.SetVertexBuffer(2, draw.buffers.uvBuffer);
                    ctx.SetVertexBuffer(
                        7,
                        draw.requiresUV1
                            ? draw.buffers.uv1Buffer
                            : draw.buffers.uvBuffer);
                }
                if (draw.buffers.tangentBuffer)
                    ctx.SetVertexBuffer(3, draw.buffers.tangentBuffer);
                if (draw.skinned)
                {
                    ctx.SetVertexBuffer(4, draw.buffers.boneIndicesBuffer);
                    ctx.SetVertexBuffer(5, draw.buffers.boneWeightsBuffer);
                }
                ctx.SetIndexBuffer(draw.buffers.indexBuffer, RHIFormat::R32_UINT);
                ctx.DrawIndexed(draw.submesh.indexCount,
                                1,
                                draw.submesh.indexOffset,
                                draw.submesh.baseVertex,
                                0);
                static_cast<void>(data.results->RecordSuccessfulMaterialBinding(
                    draw.material.binding));
                ++stats.executedPacketCount;
                ++stats.executedDrawCount;
                ++stats.reactiveMaskWriteDrawCount;
                if (draw.shadingModel == MaterialShadingModel::ThinTransmission)
                {
                    ++stats.thinTransmissionDrawCount;
                }
                else if (draw.blendMode == MaterialBlendMode::Premultiplied)
                {
                    ++stats.premultipliedDrawCount;
                }
                else if (draw.blendMode == MaterialBlendMode::Additive)
                {
                    ++stats.additiveDrawCount;
                }
                else
                {
                    ++stats.alphaBlendDrawCount;
                }
            }
            ctx.EndRenderPass();
            const bool succeeded = !executionFailed &&
                stats.executedDrawCount == stats.preparedDrawItemCount;
            if (succeeded && stats.executedDrawCount != 0)
            {
                RenderLightingSurfaceExecutionEvidence evidence;
                evidence.lightingPlanIdentity = lightingPlanIdentity;
                evidence.route = RenderLightingSurfaceRoute::Transparent;
                evidence.frozenBufferMask = 0x3fu;
                evidence.directionalShadowBound =
                    directionalShadowResources.enabled;
                evidence.textureIBLBound =
                    data.execution.lightingResources->textureIBLEnabled;
                evidence.exactIBLViews = exactIBLViews;
                evidence.workCount = stats.executedDrawCount;
                evidence.executed = true;
                return data.results->RecordLightingSurfaceExecution(evidence);
            }
            return succeeded;
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
        const bool lightingInputsValid = execution.lightingPlan != nullptr &&
            execution.lightingPlan->IsValid() &&
            execution.lightingPlan->GetIdentityHash() != 0 &&
            execution.lightingResources != nullptr &&
            execution.lightingResources->IsComplete() &&
            execution.lightingResources->lightingPlanIdentity ==
                execution.lightingPlan->GetIdentityHash() &&
            execution.frameSnapshot != nullptr &&
            execution.frameSnapshot->lightingPlan == execution.lightingPlan &&
            execution.frameSnapshot->lightingResources == execution.lightingResources &&
            execution.view.textureIBLEnabled ==
                static_cast<uint32>(execution.lightingResources->textureIBLEnabled) &&
            execution.lightingPlan->GetIBLRequest().inputsReady ==
                execution.lightingResources->textureIBLEnabled &&
            execution.lightingPlan->GetCSMRequest().inputsReady ==
                execution.directionalShadow.enabled &&
            execution.directionalShadow.IsCompatibleWith(
                execution.identity,
                execution.lightingPlan->GetIdentityHash()) &&
            execution.rayTracedShadow.IsCompatibleWith(execution.identity);
        const bool contextValid = !context.legacyAdapter &&
            context.MatchesTargetGraph(graph) && context.IsFrameIdentityValid() &&
            execution.MatchesTargetGraph(graph) && execution.IsFrameIdentityValid() &&
            execution.frameSnapshot != nullptr && execution.results != nullptr &&
            colorValid && depthValid && lightingInputsValid;
        const bool resultOwnershipValid = execution.identity.Matches(graph) &&
            execution.results != nullptr &&
            execution.results->identity == execution.identity &&
            execution.frameSnapshot != nullptr &&
            execution.frameSnapshot->identity == execution.identity;

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
                data.frameSnapshot = frameSnapshot;
                data.results = resultOwnershipValid ? results : nullptr;
                if (!PrepareRecording(data, builder))
                {
                    data.draws.clear();
                    data.bindings = {};
                    data.colorHandle = {};
                    data.depthHandle = {};
                    data.depthAvailable = false;
                }
            },
            [](const GraphPassData& data,
               RenderGraphPassContext& context)
            {
                if (data.results == nullptr)
                {
                    context.MarkExecutionFailed(
                        "TransparentPass has no recording-results owner");
                    return;
                }
                if (data.results->transparentStats.noWork)
                {
                    return;
                }
                if (!ExecuteRecording(data, context))
                {
                    TransparentPassDrawStats& stats =
                        data.results->transparentStats;
                    stats.executionFailed = true;
                    stats.failureReason =
                        RenderPolicyReason::UnexpectedRecordingFailure;
                    PublishTransparentExecutionReport(
                        data.execution,
                        stats,
                        RenderExecutionStatus::Failed,
                        stats.failureReason);
                    context.MarkExecutionFailed(
                        "TransparentPass failed after graph registration");
                    return;
                }
                TransparentPassDrawStats& stats = data.results->transparentStats;
                stats.failureReason = RenderPolicyReason::None;
                PublishTransparentExecutionReport(
                    data.execution,
                    stats,
                    RenderExecutionStatus::Completed,
                    GetTransparentSuccessReason(data.execution));
            });
    }

} // namespace RVX
