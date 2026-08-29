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
#include "Render/Lighting/LocalShadowAtlas.h"
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
#include <mutex>
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
            RenderResourceHandle materialHandle{};
            MaterialBlendMode blendMode = MaterialBlendMode::Opaque;
            MaterialShadingModel shadingModel =
                MaterialShadingModel::StandardLit;
            MaterialCullMode cullMode = MaterialCullMode::Back;
            TranslucencyRoute translucencyRoute =
                TranslucencyRoute::Unsupported;
            uint32 sourceOrdinal = RVX_INVALID_INDEX;
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
            RGTextureHandle froxelVolumetricLightingReadHandle{};
            RGTextureHandle reactiveMaskHandle{};
            RGTextureHandle directionalShadowReadHandle{};
            RGTextureHandle rayTracedShadowMaskReadHandle{};
            RGTextureHandle localShadowAtlasReadHandle{};
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
            RGTextureViewHandle froxelVolumetricLightingViewHandle{};
            RGTextureViewHandle reactiveMaskViewHandle{};
            RGTextureViewHandle directionalShadowViewHandle{};
            RGTextureViewHandle rayTracedShadowMaskViewHandle{};
            RGTextureViewHandle localShadowAtlasViewHandle{};
            RGTextureViewHandle environmentIBLIrradianceViewHandle{};
            RGTextureViewHandle environmentIBLPrefilteredViewHandle{};
            RGTextureViewHandle environmentIBLBRDFLUTViewHandle{};
            RHIFormat colorFormat = RHIFormat::Unknown;
            bool depthAvailable = false;
            bool requestedEnabled = false;
            bool contextValid = false;
            bool recordingPrepared = false;
            bool weightedBlendedStage = false;
            bool stagedRecording = false;
            bool publishLightingEvidence = true;
            uint64 translucencyPlanIdentityHash = 0;
            // The staged state owns this prepared payload. Keep the reverse
            // execution lookup non-owning so the prepared recording cannot
            // form a self-cycle that retains per-frame material descriptors.
            std::weak_ptr<TransparentStagedRecordingState> stagedState;
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
                    Ref<RefCounted>(bindings.localShadowAtlasView.Get())) &&
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
                    Ref<RefCounted>(bindings.froxelVolumetricLightingView.Get())) &&
                context.RetainSubmissionResource(
                    Ref<RefCounted>(bindings.frameSetLayout.Get())) &&
                context.RetainSubmissionResource(
                    Ref<RefCounted>(bindings.frameDescriptorSet.Get()));
        }

        [[nodiscard]] bool ResolveStagedLightingBindings(
            const GraphPassData& data,
            RenderGraphPassContext& context,
            const ViewData& view,
            const LightingBindingSnapshotRequest& request,
            uint64 lightingPlanIdentity,
            LightingBindingSnapshot& outBindings);

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

        [[nodiscard]] bool IsExactFroxelVolumetricTexture(
            const RHITextureDesc* textureDesc,
            const VolumetricLightingRecordInput& volumetricLighting) noexcept
        {
            const FroxelGridMetadata& grid = volumetricLighting.froxelGrid;
            return textureDesc != nullptr && !volumetricLighting.identityRoute &&
                grid.tileSizePixels != 0 && grid.froxelWidth != 0 &&
                grid.froxelHeight != 0 && grid.depthSliceCount != 0 &&
                grid.froxelCount == static_cast<uint64>(grid.froxelWidth) *
                    grid.froxelHeight * grid.depthSliceCount &&
                textureDesc->dimension == RHITextureDimension::Texture3D &&
                textureDesc->format == RHIFormat::RGBA16_FLOAT &&
                textureDesc->width == grid.froxelWidth &&
                textureDesc->height == grid.froxelHeight &&
                textureDesc->depth == grid.depthSliceCount &&
                textureDesc->mipLevels == 1 && textureDesc->arraySize == 1 &&
                textureDesc->sampleCount == RHISampleCount::Count1 &&
                HasFlag(textureDesc->usage, RHITextureUsage::ShaderResource);
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
            const RenderDrawItem& item,
            TranslucencyRoute translucencyRoute =
                TranslucencyRoute::Unsupported,
            uint32 sourceOrdinal = RVX_INVALID_INDEX)
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
            record.cullMode = item.packet.pipelineKey.cullMode;
            record.translucencyRoute = translucencyRoute;
            record.sourceOrdinal = sourceOrdinal;
            record.materialHandle = item.material;
            if (translucencyRoute == TranslucencyRoute::WeightedBlended)
            {
                record.pipeline = RHIPipelineRef(
                    data.pipelineCache->GetWeightedBlendedOITAccumulationPipeline(
                        skinned ? DefaultLitDirectVertexInputMode::Skinned
                                : DefaultLitDirectVertexInputMode::Rigid,
                        item.packet.pipelineKey.cullMode,
                        item.packet.pipelineKey.reverseWinding));
            }
            else
            {
                record.pipeline = RHIPipelineRef(
                    data.pipelineCache->GetTransparentMrtPipeline(
                        data.colorFormat,
                        skinned ? DefaultLitDirectVertexInputMode::Skinned
                                : DefaultLitDirectVertexInputMode::Rigid,
                        item.packet.pipelineKey.cullMode,
                        item.packet.pipelineKey.reverseWinding,
                        item.packet.pipelineKey.blendMode));
            }
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
            if (!data.stagedRecording && data.results != nullptr)
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
            const VolumetricLightingRecordInput& volumetricLighting =
                data.execution.volumetricLighting;
            if (!data.stagedRecording &&
                data.frameSnapshot->transparentDrawItems.empty())
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
            const bool directStage = !data.weightedBlendedStage;
            if (!IsTransparentDrawListStrictlyOrdered(
                    data.frameSnapshot->transparentDrawItems) ||
                !view.colorTarget.IsValid() ||
                !opaqueSnapshot.IsCompatibleWith(data.identity) ||
                !volumetricLighting.IsCompatibleWith(data.identity) ||
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
            const RHITextureDesc* const volumetricLightingDesc =
                volumetricLighting.identityRoute ? nullptr :
                builder.GetTextureDesc(
                    volumetricLighting.integratedScatteringTransmittance);
            const RHITextureDesc* const reactiveMaskDesc =
                builder.GetTextureDesc(view.reactiveMaskTarget);
            const bool directTargetsValid = directStage &&
                data.colorFormat == RHIFormat::RGBA16_FLOAT &&
                HasFlag(colorDesc->usage, RHITextureUsage::CopySrc) &&
                opaqueSnapshot.texture.index != view.colorTarget.index &&
                opaqueSnapshotDesc != nullptr &&
                opaqueSnapshotDesc->format == RHIFormat::RGBA16_FLOAT &&
                opaqueSnapshotDesc->width == colorDesc->width &&
                opaqueSnapshotDesc->height == colorDesc->height &&
                opaqueSnapshotDesc->sampleCount == colorDesc->sampleCount &&
                HasFlag(opaqueSnapshotDesc->usage,
                        RHITextureUsage::ShaderResource) &&
                HasFlag(opaqueSnapshotDesc->usage, RHITextureUsage::CopyDst) &&
                reactiveMaskDesc != nullptr &&
                reactiveMaskDesc->format == RHIFormat::R8_UNORM &&
                reactiveMaskDesc->width == colorDesc->width &&
                reactiveMaskDesc->height == colorDesc->height &&
                reactiveMaskDesc->sampleCount == colorDesc->sampleCount &&
                HasFlag(reactiveMaskDesc->usage,
                        RHITextureUsage::RenderTarget);
            const bool weightedTargetsValid = !directStage &&
                data.colorFormat ==
                    PipelineCache::GetWeightedBlendedOITAccumulationFormat() &&
                reactiveMaskDesc != nullptr &&
                reactiveMaskDesc->format ==
                    PipelineCache::GetWeightedBlendedOITRevealageFormat() &&
                reactiveMaskDesc->width == colorDesc->width &&
                reactiveMaskDesc->height == colorDesc->height &&
                reactiveMaskDesc->sampleCount == colorDesc->sampleCount &&
                HasFlag(colorDesc->usage, RHITextureUsage::RenderTarget) &&
                HasFlag(reactiveMaskDesc->usage,
                        RHITextureUsage::RenderTarget);
            const bool opaqueInputsValid = opaqueSnapshot.texture.index !=
                    view.colorTarget.index &&
                opaqueSnapshotDesc != nullptr &&
                opaqueSnapshotDesc->format == RHIFormat::RGBA16_FLOAT &&
                opaqueSnapshotDesc->width == colorDesc->width &&
                opaqueSnapshotDesc->height == colorDesc->height &&
                opaqueSnapshotDesc->sampleCount == colorDesc->sampleCount &&
                HasFlag(opaqueSnapshotDesc->usage,
                        RHITextureUsage::ShaderResource) &&
                HasFlag(opaqueSnapshotDesc->usage, RHITextureUsage::CopyDst);
            if ((!directTargetsValid && !weightedTargetsValid) ||
                !opaqueInputsValid ||
                (!volumetricLighting.identityRoute &&
                 !IsExactFroxelVolumetricTexture(
                     volumetricLightingDesc, volumetricLighting)))
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

            const LocalShadowRecordOutput& localShadow =
                data.execution.localShadowOutput;
            if (!localShadow.IsCompatibleWith(
                    data.identity,
                    data.execution.lightingPlan->GetFrameIdentity(),
                    data.execution.lightingPlan->GetViewIdentity(),
                    lightingPlanIdentity))
            {
                MarkTransparentPreflightFailure(
                    data, RenderPolicyReason::InconsistentFacts);
                return false;
            }
            if (data.execution.localShadowAtlasPlan != nullptr &&
                localShadow.enabled !=
                    !data.execution.localShadowAtlasPlan->GetPages().empty())
            {
                MarkTransparentPreflightFailure(
                    data, RenderPolicyReason::InconsistentFacts);
                return false;
            }
            if (localShadow.enabled)
            {
                const std::shared_ptr<const LocalShadowAtlasPlan>& atlasPlan =
                    data.execution.localShadowAtlasPlan;
                if (atlasPlan == nullptr || !atlasPlan->IsValid() ||
                    localShadow.localShadowPlanIdentity !=
                        atlasPlan->GetLocalShadowPlanIdentity() ||
                    localShadow.atlasPlanIdentity != atlasPlan->GetIdentityHash() ||
                    localShadow.atlasGeneration != atlasPlan->GetAtlasGeneration() ||
                    localShadow.pageCount != atlasPlan->GetPages().size() ||
                    localShadow.pageResolution !=
                        atlasPlan->GetTextureContract().pageResolution)
                {
                    MarkTransparentPreflightFailure(
                        data, RenderPolicyReason::InconsistentFacts);
                    return false;
                }

                RGTextureHandle depthAtlas = localShadow.depthAtlas;
                depthAtlas.hasSubresourceRange = true;
                depthAtlas.subresourceRange = RHISubresourceRange{
                    0, RVX_ALL_MIPS, 0, RVX_ALL_LAYERS,
                    RHITextureAspect::Depth};
                const RHITextureDesc* const atlasDesc =
                    builder.GetTextureDesc(depthAtlas);
                const LocalShadowAtlasTextureContract& atlasContract =
                    atlasPlan->GetTextureContract();
                const std::vector<LocalShadowAtlasPage>& pages =
                    atlasPlan->GetPages();
                const bool pageRangesMatch =
                    localShadow.pageDepthLayers.size() == pages.size() &&
                    std::equal(
                        localShadow.pageDepthLayers.begin(),
                        localShadow.pageDepthLayers.end(),
                        pages.begin(),
                        [](const RGTextureHandle& layer,
                           const LocalShadowAtlasPage& page)
                        {
                            return layer.IsValid() && layer.hasSubresourceRange &&
                                layer.subresourceRange.baseMipLevel == 0 &&
                                layer.subresourceRange.mipLevelCount == 1 &&
                                layer.subresourceRange.baseArrayLayer ==
                                    page.pageIndex &&
                                layer.subresourceRange.arrayLayerCount == 1 &&
                                layer.subresourceRange.aspect ==
                                    RHITextureAspect::Depth;
                        });
                if (atlasDesc == nullptr ||
                    atlasDesc->format != RHIFormat::D32_FLOAT ||
                    atlasDesc->dimension != RHITextureDimension::Texture2D ||
                    atlasDesc->width != localShadow.pageResolution ||
                    atlasDesc->height != localShadow.pageResolution ||
                    atlasDesc->arraySize != atlasContract.pageCapacity ||
                    !HasFlag(atlasDesc->usage, RHITextureUsage::DepthStencil) ||
                    !HasFlag(atlasDesc->usage, RHITextureUsage::ShaderResource) ||
                    !pageRangesMatch)
                {
                    MarkTransparentPreflightFailure(
                        data, RenderPolicyReason::ResourcesUnavailable);
                    return false;
                }

                data.localShadowAtlasReadHandle = builder.Read(
                    depthAtlas,
                    MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                     RHIShaderStage::Pixel));
                RHITextureViewDesc localShadowViewDesc;
                localShadowViewDesc.format = RHIFormat::D32_FLOAT;
                localShadowViewDesc.dimension = RHITextureDimension::Texture2D;
                localShadowViewDesc.subresourceRange =
                    depthAtlas.subresourceRange;
                localShadowViewDesc.type = RHITextureViewType::ShaderResource;
                localShadowViewDesc.debugName =
                    "TransparentLocalShadowAtlasSRV";
                data.localShadowAtlasViewHandle = builder.CreateTextureView(
                    data.localShadowAtlasReadHandle, localShadowViewDesc);
                data.localShadowAtlasViewHandle = builder.Read(
                    data.localShadowAtlasViewHandle,
                    MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                     RHIShaderStage::Pixel));
                if (!data.localShadowAtlasReadHandle.IsValid() ||
                    !data.localShadowAtlasViewHandle.IsValid())
                {
                    MarkTransparentPreflightFailure(
                        data, RenderPolicyReason::ResourcesUnavailable);
                    return false;
                }
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
            if (!data.stagedRecording)
            {
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
                for (const RenderDrawItem& item :
                     data.frameSnapshot->transparentDrawItems)
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
            }
            else
            {
                for (const DrawRecord& draw : data.draws)
                {
                    if (!DeclareMaterialTextureGraphReads(
                            builder,
                            data.resourceRegistry,
                            draw.materialHandle,
                            *data.results))
                    {
                        MarkTransparentPreflightFailure(
                            data, RenderPolicyReason::ResourcesUnavailable);
                        return false;
                    }
                }
            }
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
                const RenderLightingFrameResourceSnapshot& iblLightingResources =
                    *data.execution.lightingResources;
                data.environmentIBLIrradianceViewHandle = DeclareExactIBLRead(
                    builder,
                    iblLightingResources.environmentIBLIrradiance,
                    iblLightingResources.environmentIBLIrradianceView,
                    "TransparentEnvironmentIrradianceSRV");
                data.environmentIBLPrefilteredViewHandle = DeclareExactIBLRead(
                    builder,
                    iblLightingResources.environmentIBLPrefiltered,
                    iblLightingResources.environmentIBLPrefilteredView,
                    "TransparentEnvironmentPrefilteredSRV");
                data.environmentIBLBRDFLUTViewHandle = DeclareExactIBLRead(
                    builder,
                    iblLightingResources.environmentIBLBRDFLUT,
                    iblLightingResources.environmentIBLBRDFLUTView,
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
            data.reactiveMaskHandle = view.reactiveMaskTarget;
            RHITextureViewDesc reactiveViewDesc;
            reactiveViewDesc.format = data.weightedBlendedStage
                ? PipelineCache::GetWeightedBlendedOITRevealageFormat()
                : RHIFormat::R8_UNORM;
            reactiveViewDesc.dimension = colorDesc->dimension;
            reactiveViewDesc.type = RHITextureViewType::RenderTarget;
            reactiveViewDesc.debugName = data.weightedBlendedStage
                ? "WeightedBlendedOITRevealageRTV"
                : "TransparentReactiveMaskRTV";
            data.reactiveMaskViewHandle = builder.CreateTextureView(
                data.reactiveMaskHandle, reactiveViewDesc);
            data.reactiveMaskViewHandle = builder.ReadWrite(
                data.reactiveMaskViewHandle,
                MakeRGAccessDesc(RHIResourceState::RenderTarget,
                                 RHIShaderStage::Pixel));
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
            if (!volumetricLighting.identityRoute)
            {
                data.froxelVolumetricLightingReadHandle = builder.Read(
                    volumetricLighting.integratedScatteringTransmittance,
                    MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                     RHIShaderStage::Pixel));
                RHITextureViewDesc froxelViewDesc;
                froxelViewDesc.format = RHIFormat::RGBA16_FLOAT;
                froxelViewDesc.dimension = RHITextureDimension::Texture3D;
                froxelViewDesc.type = RHITextureViewType::ShaderResource;
                froxelViewDesc.debugName =
                    "TransparentFroxelVolumetricLightingSRV";
                data.froxelVolumetricLightingViewHandle =
                    builder.CreateTextureView(
                        data.froxelVolumetricLightingReadHandle,
                        froxelViewDesc);
                data.froxelVolumetricLightingViewHandle = builder.Read(
                    data.froxelVolumetricLightingViewHandle,
                    MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                     RHIShaderStage::Pixel));
            }
            if (!data.opaqueSceneSnapshotReadHandle.IsValid() ||
                !data.opaqueSceneSnapshotViewHandle.IsValid() ||
                !data.reactiveMaskViewHandle.IsValid() ||
                (!volumetricLighting.identityRoute &&
                 (!data.froxelVolumetricLightingReadHandle.IsValid() ||
                  !data.froxelVolumetricLightingViewHandle.IsValid())))
            {
                MarkTransparentPreflightFailure(
                    data, RenderPolicyReason::ResourcesUnavailable);
                return false;
            }
            if (directStage)
            {
                data.results->transparentStats.reactiveMaskAttachmentBound = true;
            }
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
                !data.execution.localShadowOutput.IsCompatibleWith(
                    data.identity,
                    data.execution.lightingPlan->GetFrameIdentity(),
                    data.execution.lightingPlan->GetViewIdentity(),
                    data.execution.lightingPlan->GetIdentityHash()) ||
                !data.execution.volumetricLighting.IsCompatibleWith(
                    data.identity) ||
                !data.identity.IsValid() || data.identity.graph == nullptr ||
                !data.identity.Matches(*data.identity.graph) ||
                !data.colorHandle.IsValid() ||
                data.colorHandle.graphIdentity != data.identity.graphIdentity ||
                data.colorHandle.recordingGeneration !=
                    data.identity.graphRecordingGeneration ||
                !data.reactiveMaskHandle.IsValid() ||
                !data.reactiveMaskViewHandle.IsValid() ||
                data.reactiveMaskHandle.graphIdentity != data.identity.graphIdentity ||
                data.reactiveMaskHandle.recordingGeneration !=
                    data.identity.graphRecordingGeneration ||
                (!data.weightedBlendedStage &&
                 (!data.opaqueSceneSnapshotReadHandle.IsValid() ||
                  !data.opaqueSceneSnapshotViewHandle.IsValid() ||
                  data.opaqueSceneSnapshotReadHandle.graphIdentity !=
                      data.identity.graphIdentity ||
                  data.opaqueSceneSnapshotReadHandle.recordingGeneration !=
                      data.identity.graphRecordingGeneration)) ||
                (!data.execution.volumetricLighting.identityRoute &&
                 (!data.froxelVolumetricLightingReadHandle.IsValid() ||
                  !data.froxelVolumetricLightingViewHandle.IsValid() ||
                  data.froxelVolumetricLightingReadHandle.graphIdentity !=
                      data.identity.graphIdentity ||
                  data.froxelVolumetricLightingReadHandle.recordingGeneration !=
                      data.identity.graphRecordingGeneration)) ||
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

            LocalShadowFrameResources localShadowResources;
            const LocalShadowRecordOutput& localShadow =
                data.execution.localShadowOutput;
            if (localShadow.enabled)
            {
                const std::shared_ptr<const LocalShadowAtlasPlan>& atlasPlan =
                    data.execution.localShadowAtlasPlan;
                if (atlasPlan == nullptr || !atlasPlan->IsValid() ||
                    localShadow.localShadowPlanIdentity !=
                        atlasPlan->GetLocalShadowPlanIdentity() ||
                    localShadow.atlasPlanIdentity != atlasPlan->GetIdentityHash() ||
                    localShadow.atlasGeneration != atlasPlan->GetAtlasGeneration() ||
                    !data.localShadowAtlasReadHandle.IsValid() ||
                    !data.localShadowAtlasViewHandle.IsValid())
                {
                    RVX_CORE_WARN(
                        "TransparentPass: local-shadow output has no exact graph atlas read");
                    return false;
                }

                RHITexture* const atlasTexture =
                    context.GetTexture(data.localShadowAtlasReadHandle);
                RHITextureView* const atlasView =
                    context.GetTextureView(data.localShadowAtlasViewHandle);
                const LocalShadowAtlasTextureContract& atlasContract =
                    atlasPlan->GetTextureContract();
                if (atlasTexture == nullptr || atlasView == nullptr ||
                    atlasView->GetTexture() != atlasTexture ||
                    atlasView->GetFormat() != RHIFormat::D32_FLOAT ||
                    atlasTexture->GetFormat() != RHIFormat::D32_FLOAT ||
                    atlasTexture->GetWidth() != localShadow.pageResolution ||
                    atlasTexture->GetHeight() != localShadow.pageResolution ||
                    atlasTexture->GetArraySize() != atlasContract.pageCapacity ||
                    atlasView->GetSubresourceRange().arrayLayerCount !=
                        RVX_ALL_LAYERS ||
                    atlasView->GetSubresourceRange().aspect !=
                        RHITextureAspect::Depth)
                {
                    RVX_CORE_WARN(
                        "TransparentPass: local-shadow atlas does not match its sealed D32 array contract");
                    return false;
                }
                localShadowResources.depthAtlasView = atlasView;
                localShadowResources.enabled = true;
                localShadowResources.localShadowPlanIdentity =
                    localShadow.localShadowPlanIdentity;
                localShadowResources.atlasPlanIdentity =
                    localShadow.atlasPlanIdentity;
                localShadowResources.atlasGeneration =
                    localShadow.atlasGeneration;
                localShadowResources.pageCount = localShadow.pageCount;
                localShadowResources.pageResolution =
                    localShadow.pageResolution;
                localShadowResources.pageCapacity = atlasContract.pageCapacity;
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
            lightingRequest.localShadow = localShadowResources;
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
            const VolumetricLightingRecordInput& volumetricLighting =
                data.execution.volumetricLighting;
            if (!volumetricLighting.identityRoute)
            {
                RHITexture* const froxelTexture = context.GetTexture(
                    data.froxelVolumetricLightingReadHandle);
                RHITextureView* const froxelView = context.GetTextureView(
                    data.froxelVolumetricLightingViewHandle);
                const FroxelGridMetadata& grid =
                    volumetricLighting.froxelGrid;
                if (froxelTexture == nullptr || froxelView == nullptr ||
                    froxelView->GetTexture() != froxelTexture ||
                    froxelView->GetFormat() != RHIFormat::RGBA16_FLOAT ||
                    froxelTexture->GetDimension() !=
                        RHITextureDimension::Texture3D ||
                    froxelTexture->GetFormat() != RHIFormat::RGBA16_FLOAT ||
                    froxelTexture->GetWidth() != grid.froxelWidth ||
                    froxelTexture->GetHeight() != grid.froxelHeight ||
                    froxelTexture->GetDepth() != grid.depthSliceCount ||
                    froxelTexture->GetMipLevels() != 1 ||
                    froxelTexture->GetArraySize() != 1 ||
                    froxelTexture->GetSampleCount() != RHISampleCount::Count1)
                {
                    RVX_CORE_WARN(
                        "TransparentPass: froxel volumetric graph view does not match the sealed Texture3D contract");
                    return false;
                }
                lightingRequest.froxelVolumetricLightingView =
                    RHITextureViewRef(froxelView);
                lightingRequest.froxelVolumetricLightingPlanIdentity =
                    volumetricLighting.planIdentity;
            }
            LightingBindingSnapshot lightingBindings;
            const bool lightingBindingsReady = data.stagedRecording
                ? ResolveStagedLightingBindings(
                      data,
                      context,
                      drawView,
                      lightingRequest,
                      lightingPlanIdentity,
                      lightingBindings)
                : (data.pipelineCache->CreateLightingBindingSnapshot(
                       drawView, lightingRequest, lightingBindings) &&
                   lightingBindings.MatchesLightingPlan(lightingPlanIdentity) &&
                   RetainLightingBindingSnapshot(context, lightingBindings));
            if (!lightingBindingsReady)
            {
                RVX_CORE_WARN("TransparentPass: could not create record-owned lighting bindings");
                return false;
            }

            RHIRenderPassDesc renderPassDesc;
            if (data.weightedBlendedStage)
            {
                // The composition owns the sole shared Accum/Reveal clear.
                // Mesh and particle contributors both load these targets so
                // either contributor may be absent without erasing the other.
                renderPassDesc.AddColorAttachment(
                    colorView, RHILoadOp::Load, RHIStoreOp::Store);
                renderPassDesc.AddColorAttachment(
                    reactiveMaskView, RHILoadOp::Load, RHIStoreOp::Store);
            }
            else
            {
                renderPassDesc.AddColorAttachment(
                    colorView, RHILoadOp::Load, RHIStoreOp::Store);
                renderPassDesc.AddColorAttachment(
                    reactiveMaskView, RHILoadOp::Load, RHIStoreOp::Store);
            }
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
            uint32 executedStageDrawCount = 0;
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
                ++executedStageDrawCount;
                if (!data.weightedBlendedStage)
                {
                    ++stats.reactiveMaskWriteDrawCount;
                }
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
                (data.stagedRecording
                     ? executedStageDrawCount == data.draws.size()
                     : stats.executedDrawCount == stats.preparedDrawItemCount);
            if (succeeded && executedStageDrawCount != 0 &&
                data.publishLightingEvidence)
            {
                RenderLightingSurfaceExecutionEvidence evidence;
                evidence.lightingPlanIdentity = lightingPlanIdentity;
                evidence.route = RenderLightingSurfaceRoute::Transparent;
                evidence.frozenBufferMask = 0x3fu;
                evidence.directionalShadowBound =
                    directionalShadowResources.enabled;
                evidence.localShadowBound = localShadowResources.enabled;
                if (localShadowResources.enabled)
                {
                    evidence.localShadowPlanIdentity =
                        localShadow.localShadowPlanIdentity;
                    evidence.localShadowAtlasPlanIdentity =
                        localShadow.atlasPlanIdentity;
                    evidence.localShadowAtlasGeneration =
                        localShadow.atlasGeneration;
                    evidence.localShadowPageCount = localShadow.pageCount;
                }
                evidence.textureIBLBound =
                    data.execution.lightingResources->textureIBLEnabled;
                evidence.exactIBLViews = exactIBLViews;
                evidence.workCount = executedStageDrawCount;
                evidence.executed = true;
                return data.results->RecordLightingSurfaceExecution(evidence);
            }
            return succeeded;
        }
    } // namespace

    struct TransparentStagedRecordingState
    {
        mutable std::mutex mutex;
        GraphPassData prepared{};
        TransparentStagedRecordingStats stats{};
        LightingBindingSnapshot lightingBindings{};
        bool lightingBindingsPrepared = false;
        bool weightedBlendedEnabled = false;
        bool weightedBlendedRecorded = false;
        bool directRecorded = false;
    };

    TransparentPass::PreparedTranslucencyRecording::
        PreparedTranslucencyRecording(
            std::shared_ptr<TransparentStagedRecordingState> state) noexcept
        : m_state(std::move(state))
    {
    }

    bool TransparentPass::PreparedTranslucencyRecording::IsPrepared() const noexcept
    {
        if (m_state == nullptr)
        {
            return false;
        }
        std::lock_guard lock(m_state->mutex);
        return m_state->stats.prepared && !m_state->stats.preparationFailed;
    }

    uint64 TransparentPass::PreparedTranslucencyRecording::GetPlanIdentityHash() const noexcept
    {
        if (m_state == nullptr)
        {
            return 0;
        }
        std::lock_guard lock(m_state->mutex);
        return m_state->stats.planIdentityHash;
    }

    TransparentStagedRecordingStats
    TransparentPass::PreparedTranslucencyRecording::GetStats() const
    {
        if (m_state == nullptr)
        {
            return {};
        }
        std::lock_guard lock(m_state->mutex);
        return m_state->stats;
    }

    namespace
    {
        bool ResolveStagedLightingBindings(
            const GraphPassData& data,
            RenderGraphPassContext& context,
            const ViewData& view,
            const LightingBindingSnapshotRequest& request,
            uint64 lightingPlanIdentity,
            LightingBindingSnapshot& outBindings)
        {
            const std::shared_ptr<TransparentStagedRecordingState> state =
                data.stagedState.lock();
            if (state == nullptr)
            {
                return false;
            }

            std::lock_guard lock(state->mutex);
            if (!state->lightingBindingsPrepared)
            {
                if (!data.pipelineCache->CreateLightingBindingSnapshot(
                        view,
                        request,
                        state->lightingBindings) ||
                    !state->lightingBindings.MatchesLightingPlan(
                        lightingPlanIdentity))
                {
                    return false;
                }
                state->lightingBindingsPrepared = true;
            }
            if (!state->lightingBindings.MatchesLightingPlan(
                    lightingPlanIdentity) ||
                !RetainLightingBindingSnapshot(context, state->lightingBindings))
            {
                return false;
            }
            outBindings = state->lightingBindings;
            return true;
        }

        void FailStagedPreparation(
            const std::shared_ptr<TransparentStagedRecordingState>& state) noexcept
        {
            if (state == nullptr)
            {
                return;
            }
            std::lock_guard lock(state->mutex);
            state->stats.prepared = false;
            state->stats.preparationFailed = true;
        }

        [[nodiscard]] bool HasExactStagedPlanInventory(
            const TranslucencyFramePlan& plan,
            const RenderPassRecordContext& context,
            const std::vector<RenderDrawItem>& drawItems) noexcept
        {
            if (!plan.IsExecutable() || plan.HasRequiredCapabilityFailure() ||
                plan.GetIdentityHash() == 0 ||
                plan.GetFrameIdentity() != context.featureFrameIdentity ||
                plan.GetViewIdentity() != context.featureViewIdentity ||
                context.identity.frameSequence !=
                    plan.GetFrameIdentity().frameSequence ||
                !IsTransparentDrawListStrictlyOrdered(drawItems) ||
                plan.GetMeshSourceIdentityHash() == 0 ||
                plan.GetMeshSourceIdentityHash() !=
                    BuildTransparentDrawSourceIdentityHash(drawItems) ||
                plan.GetSummary().meshCandidateCount != drawItems.size())
            {
                return false;
            }

            for (uint32 sourceOrdinal = 0;
                 sourceOrdinal < static_cast<uint32>(drawItems.size());
                 ++sourceOrdinal)
            {
                const TranslucencyPlanEntry* const entry = plan.FindEntry(
                    TranslucencySourceKind::Mesh, sourceOrdinal);
                if (entry == nullptr || entry->sourceOrdinal != sourceOrdinal ||
                    entry->sourceIdentityHash == 0 ||
                    entry->sourceIdentityHash !=
                        BuildTransparentDrawItemIdentityHash(
                            drawItems[sourceOrdinal]) ||
                    entry->firstItem != 0 || entry->itemCount != 1 ||
                    entry->route == TranslucencyRoute::Unsupported ||
                    (entry->route != TranslucencyRoute::WeightedBlended &&
                     entry->route != TranslucencyRoute::SortedRefraction &&
                     entry->route != TranslucencyRoute::DirectAdditive))
                {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] bool PrepareStagedSkinningPalettes(
            const RenderPassExecutionData& execution,
            SkinningPaletteBufferBinding& outPalettes) noexcept
        {
            outPalettes = {};
            const std::shared_ptr<const RenderDeformationFrameResources>& resources =
                execution.deformationResources;
            if (resources == nullptr || resources->GetMatrixCount() == 0)
            {
                return true;
            }
            if (!resources->IsComplete() ||
                !resources->MatchesIdentity(
                    MakeRenderDeformationFrameIdentity(execution.identity)))
            {
                return false;
            }
            outPalettes.currentPaletteBuffer = resources->GetCurrentPaletteBuffer();
            outPalettes.previousPaletteBuffer = resources->GetPreviousPaletteBuffer();
            return outPalettes.HasPalettes();
        }

        void MarkStagedExecution(
            const std::shared_ptr<TransparentStagedRecordingState>& state,
            bool weightedBlended,
            bool succeeded,
            uint32 drawCount) noexcept
        {
            if (state == nullptr)
            {
                return;
            }
            std::lock_guard lock(state->mutex);
            if (!succeeded)
            {
                state->stats.executionFailed = true;
                return;
            }
            if (weightedBlended)
            {
                state->stats.weightedBlendedExecuted = true;
                state->stats.weightedBlendedExecutedDrawCount = drawCount;
            }
            else
            {
                state->stats.directExecuted = true;
                for (const DrawRecord& draw : state->prepared.draws)
                {
                    if (draw.translucencyRoute ==
                        TranslucencyRoute::SortedRefraction)
                    {
                        ++state->stats.sortedRefractionExecutedDrawCount;
                    }
                    else if (draw.translucencyRoute ==
                             TranslucencyRoute::DirectAdditive)
                    {
                        ++state->stats.directAdditiveExecutedDrawCount;
                    }
                }
            }
        }

        [[nodiscard]] std::vector<DrawRecord> SelectStagedDraws(
            const GraphPassData& prepared,
            bool weightedBlended)
        {
            std::vector<DrawRecord> selected;
            selected.reserve(prepared.draws.size());
            for (const DrawRecord& draw : prepared.draws)
            {
                const bool select = weightedBlended
                    ? draw.translucencyRoute ==
                        TranslucencyRoute::WeightedBlended
                    : (draw.translucencyRoute ==
                           TranslucencyRoute::SortedRefraction ||
                       draw.translucencyRoute ==
                           TranslucencyRoute::DirectAdditive);
                if (select)
                {
                    selected.push_back(draw);
                }
            }
            return selected;
        }
    } // namespace

    TransparentPass::PreparedTranslucencyRecording
    TransparentPass::PrepareTranslucencyRecording(
        const TranslucencyFramePlan& plan,
        const RenderPassRecordContext& context)
    {
        auto state = std::make_shared<TransparentStagedRecordingState>();
        state->stats.planIdentityHash = plan.GetIdentityHash();
        state->stats.preparationCount = 1;

        const bool sourceContextValid = !context.legacyAdapter &&
            context.identity.IsValid() && context.identity.graph != nullptr &&
            context.MatchesTargetGraph(*context.identity.graph) &&
            context.IsFrameIdentityValid() && context.frameSnapshot != nullptr &&
            context.results != nullptr;
        const bool suppliedResultsCompatible = context.results == nullptr ||
            context.results->identity == RenderPassRecordIdentity{} ||
            context.results->identity == context.identity;
        const bool suppliedSnapshotCompatible = context.frameSnapshot == nullptr ||
            context.frameSnapshot->identity == context.identity;
        if (!sourceContextValid || !suppliedResultsCompatible ||
            !suppliedSnapshotCompatible || !m_enabled ||
            m_pipelineCache == nullptr || m_materialSystem == nullptr ||
            m_resourceRegistry == nullptr ||
            !HasExactStagedPlanInventory(
                plan, context, context.frameSnapshot->transparentDrawItems))
        {
            FailStagedPreparation(state);
            return PreparedTranslucencyRecording(std::move(state));
        }

        RenderPassExecutionData execution = MakeRenderPassExecutionData(context);
        if (!execution.MatchesTargetGraph(*context.identity.graph) ||
            !execution.IsFrameIdentityValid() ||
            execution.frameSnapshot == nullptr || execution.results == nullptr ||
            execution.lightingPlan == nullptr ||
            !execution.lightingPlan->IsValid() ||
            execution.lightingResources == nullptr ||
            !execution.lightingResources->IsComplete())
        {
            FailStagedPreparation(state);
            return PreparedTranslucencyRecording(std::move(state));
        }

        GraphPassData& prepared = state->prepared;
        prepared.execution = execution;
        prepared.identity = execution.identity;
        prepared.frameSnapshot = execution.frameSnapshot;
        prepared.results = execution.results;
        prepared.resourceRegistry = m_resourceRegistry;
        prepared.pipelineCache = m_pipelineCache;
        prepared.materialSystem = m_materialSystem;
        prepared.colorFormat = RHIFormat::RGBA16_FLOAT;
        prepared.requestedEnabled = true;
        prepared.contextValid = true;
        prepared.stagedRecording = true;
        prepared.translucencyPlanIdentityHash = plan.GetIdentityHash();
        prepared.stagedState = state;
        state->weightedBlendedEnabled =
            plan.GetWeightedBlendedResolution() ==
            WeightedBlendedOITCapabilityResolution::Enabled;

        if (context.frameSnapshot->transparentDrawItems.empty())
        {
            state->stats.noWork = true;
            state->stats.prepared = true;
            return PreparedTranslucencyRecording(std::move(state));
        }

        SkinningPaletteBufferBinding skinningPalettes;
        if (!PrepareStagedSkinningPalettes(execution, skinningPalettes) ||
            !m_pipelineCache->CreateRasterObjectBindingSnapshot(
                static_cast<uint32>(
                    context.frameSnapshot->transparentDrawItems.size()),
                skinningPalettes,
                prepared.bindings))
        {
            FailStagedPreparation(state);
            return PreparedTranslucencyRecording(std::move(state));
        }

        prepared.draws.reserve(
            context.frameSnapshot->transparentDrawItems.size());
        for (uint32 sourceOrdinal = 0;
             sourceOrdinal < static_cast<uint32>(
                 context.frameSnapshot->transparentDrawItems.size());
             ++sourceOrdinal)
        {
            const RenderDrawItem& item =
                context.frameSnapshot->transparentDrawItems[sourceOrdinal];
            const TranslucencyPlanEntry* const entry = plan.FindEntry(
                TranslucencySourceKind::Mesh, sourceOrdinal);
            if (entry == nullptr ||
                PrepareDrawRecord(
                    prepared, item, entry->route, sourceOrdinal) !=
                    DrawPreparationResult::Prepared)
            {
                FailStagedPreparation(state);
                return PreparedTranslucencyRecording(std::move(state));
            }

            ++state->stats.plannedMeshDrawCount;
            switch (entry->route)
            {
                case TranslucencyRoute::WeightedBlended:
                    ++state->stats.weightedBlendedDrawCount;
                    break;
                case TranslucencyRoute::SortedRefraction:
                    ++state->stats.sortedRefractionDrawCount;
                    break;
                case TranslucencyRoute::DirectAdditive:
                    ++state->stats.directAdditiveDrawCount;
                    break;
                default:
                    FailStagedPreparation(state);
                    return PreparedTranslucencyRecording(std::move(state));
            }
        }

        state->stats.noWork = prepared.draws.empty();
        state->stats.prepared = true;
        return PreparedTranslucencyRecording(std::move(state));
    }

    bool TransparentPass::RecordWeightedBlended(
        RenderGraph& graph,
        PreparedTranslucencyRecording& prepared,
        RGTextureHandle accumulation,
        RGTextureHandle revealage)
    {
        const std::shared_ptr<TransparentStagedRecordingState>& state =
            prepared.m_state;
        if (state == nullptr || !accumulation.IsValid() || !revealage.IsValid() ||
            accumulation.index == revealage.index)
        {
            return false;
        }

        GraphPassData passData;
        uint32 drawCount = 0;
        {
            std::lock_guard lock(state->mutex);
            if (!state->stats.prepared || state->stats.preparationFailed ||
                state->weightedBlendedRecorded ||
                !state->prepared.identity.Matches(graph))
            {
                return false;
            }
            state->weightedBlendedRecorded = true;
            state->stats.weightedBlendedPassRecorded = true;
            passData = state->prepared;
            passData.draws = SelectStagedDraws(state->prepared, true);
            passData.publishLightingEvidence =
                state->stats.sortedRefractionDrawCount == 0 &&
                state->stats.directAdditiveDrawCount == 0;
            drawCount = static_cast<uint32>(passData.draws.size());
        }
        if (drawCount == 0)
        {
            return true;
        }

        graph.AddPass<GraphPassData>(
            "TransparentWeightedBlendedOIT",
            RenderGraphPassType::Graphics,
            [passData, accumulation, revealage](RenderGraphBuilder& builder,
                                                 GraphPassData& data) mutable
            {
                data = passData;
                data.weightedBlendedStage = true;
                data.execution.view.colorTarget = accumulation;
                data.execution.view.reactiveMaskTarget = revealage;
                if (!PrepareRecording(data, builder))
                {
                    data.draws.clear();
                }
            },
            [state, drawCount](const GraphPassData& data,
                               RenderGraphPassContext& context)
            {
                const bool succeeded = data.recordingPrepared &&
                    ExecuteRecording(data, context);
                MarkStagedExecution(state, true, succeeded, drawCount);
                if (!succeeded)
                {
                    context.MarkExecutionFailed(
                        "Transparent weighted-blended OIT recording failed");
                }
            });
        return true;
    }

    bool TransparentPass::RecordDirect(
        RenderGraph& graph,
        PreparedTranslucencyRecording& prepared,
        RGTextureHandle sceneHDR,
        RGTextureHandle reactiveMask)
    {
        const std::shared_ptr<TransparentStagedRecordingState>& state =
            prepared.m_state;
        if (state == nullptr || !sceneHDR.IsValid() || !reactiveMask.IsValid() ||
            sceneHDR.index == reactiveMask.index)
        {
            return false;
        }

        GraphPassData passData;
        uint32 drawCount = 0;
        {
            std::lock_guard lock(state->mutex);
            if (!state->stats.prepared || state->stats.preparationFailed ||
                state->directRecorded || !state->prepared.identity.Matches(graph) ||
                (state->stats.weightedBlendedDrawCount != 0 &&
                 !state->weightedBlendedRecorded) ||
                (state->weightedBlendedEnabled &&
                 sceneHDR.index ==
                     state->prepared.execution.view.colorTarget.index))
            {
                return false;
            }
            state->directRecorded = true;
            state->stats.directPassRecorded = true;
            passData = state->prepared;
            passData.draws = SelectStagedDraws(state->prepared, false);
            passData.publishLightingEvidence = true;
            drawCount = static_cast<uint32>(passData.draws.size());
        }
        if (drawCount == 0)
        {
            return true;
        }

        graph.AddPass<GraphPassData>(
            "TransparentSortedAndAdditive",
            RenderGraphPassType::Graphics,
            [passData, sceneHDR, reactiveMask](RenderGraphBuilder& builder,
                                                GraphPassData& data) mutable
            {
                data = passData;
                data.weightedBlendedStage = false;
                data.execution.view.colorTarget = sceneHDR;
                data.execution.view.reactiveMaskTarget = reactiveMask;
                if (!PrepareRecording(data, builder))
                {
                    data.draws.clear();
                }
            },
            [state, drawCount](const GraphPassData& data,
                               RenderGraphPassContext& context)
            {
                const bool succeeded = data.recordingPrepared &&
                    ExecuteRecording(data, context);
                MarkStagedExecution(state, false, succeeded, drawCount);
                if (!succeeded)
                {
                    context.MarkExecutionFailed(
                        "Transparent sorted/additive recording failed");
                }
            });
        return true;
    }

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
            execution.rayTracedShadow.IsCompatibleWith(execution.identity) &&
            execution.localShadowOutput.IsCompatibleWith(
                execution.identity,
                execution.lightingPlan->GetFrameIdentity(),
                execution.lightingPlan->GetViewIdentity(),
                execution.lightingPlan->GetIdentityHash()) &&
            execution.volumetricLighting.IsCompatibleWith(
                execution.identity);
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
