/**
 * @file SceneRenderer.cpp
 * @brief SceneRenderer implementation
 */

#include "Render/Renderer/SceneRenderer.h"
#include "ShaderContracts/ShaderProgramProvider.h"
#include "Context/RenderContextInternal.h"
#include "GPUScene/GPUSceneUpdate.h"
#include "GPUScene/GPUSceneUploader.h"
#include "Features/DefaultRenderComposition.h"
#include "Features/AmbientOcclusionFeatureAdapter.h"
#include "Features/DeferredRecipeFeatureAdapter.h"
#include "Features/LegacyForwardFeatureAdapter.h"
#include "Features/PostProcessPipelineFeatureAdapter.h"
#include "Features/TranslucencyFeatureAdapter.h"
#include "Features/ReflectionFeatureAdapter.h"
#include "Features/VolumetricLightingFeatureAdapter.h"
#include "Render/Features/RenderFeatureGraphBuilder.h"
#include "Render/Features/RenderFeatureRegistry.h"
#include "Render/Features/RenderGraphBlackboard.h"
#include "Render/Animation/RenderDeformationFrameResources.h"
#include "Render/Lighting/ClusteredLighting.h"
#include "Render/Lighting/LightManager.h"
#include "Render/Lighting/LocalShadowAtlas.h"
#include "Render/Lighting/LocalShadowFramePlan.h"
#include "Render/Graph/RenderGraphCompiler.h"
#include "Render/Graph/RenderGraphExecutor.h"
#include "Core/Assert.h"
#include "Core/Hash/SHA256.h"
#include "Core/Log.h"
#include "Core/PathUtils.h"
#include "Render/Passes/CameraVelocityPass.h"
#include "Render/Passes/DepthPrepass.h"
#include "Render/Passes/DeferredLightingPass.h"
#include "Render/Passes/DirectDrawPacketBatch.h"
#include "Render/Passes/ObjectVelocityPass.h"
#include "Render/Passes/IRenderPass.h"
#include "Render/Passes/LocalShadowPass.h"
#include "Render/Passes/OpaquePass.h"
#include "Render/Passes/RenderPassRecordContext.h"
#include "Render/Passes/RenderPassClearValues.h"
#include "Render/Passes/RayTracedReflectionCompositePass.h"
#include "Render/Passes/RayTracedReflectionDenoisePass.h"
#include "Render/Passes/RayTracedReflectionPass.h"
#include "Render/Passes/RayTracedShadowPass.h"
#include "Render/Passes/ShadowPass.h"
#include "Render/Passes/SkyboxPass.h"
#include "Render/Passes/TransparentPass.h"
#include "Render/PostProcess/Bloom.h"
#include "Render/PostProcess/ChromaticAberration.h"
#include "Render/PostProcess/ColorGrading.h"
#include "Render/PostProcess/FilmGrain.h"
#include "Render/PostProcess/FXAA.h"
#include "Render/PostProcess/ScreenSpaceOutline.h"
#include "Render/PostProcess/SSR.h"
#include "Render/PostProcess/TAA.h"
#include "Render/PostProcess/ToneMapping.h"
#include "Render/PostProcess/Vignette.h"
#include "Render/RayTracing/RayTracingScene.h"
#include "Render/Reflection/ReflectionFramePlan.h"
#include "Renderer/RenderPassRegistry.h"
#include "Render/Renderer/RenderSceneDatabase.h"
#include "Resources/RenderResourceRegistry.h"
#include "Resources/RenderResourceResolver.h"
#include "Resources/RenderRetirementQueue.h"
#include "Resources/RenderSubmissionResourceBatch.h"
#include "Resources/RenderSubmissionTracker.h"
#include "RHI/RHICapabilities.h"
#include "RHI/RHIRenderPass.h"
#include "Runtime/RenderViewHistoryCoordinator.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <new>
#include <optional>
#include <utility>
#include <vector>

namespace RVX
{
template <typename T>
T* SceneRenderer::FindCompositionPass() const noexcept
{
    return m_defaultRenderComposition != nullptr
        ? m_defaultRenderComposition->FindPass<T>()
        : nullptr;
}

template <typename T>
T* SceneRenderer::FindCompositionFeature() const noexcept
{
    return m_defaultRenderComposition != nullptr
        ? m_defaultRenderComposition->FindFeature<T>()
        : nullptr;
}

PostProcessStack* SceneRenderer::GetPostProcessStack()
{
    return m_defaultRenderComposition != nullptr
        ? m_defaultRenderComposition->GetPostProcessStack()
        : nullptr;
}

const PostProcessStack* SceneRenderer::GetPostProcessStack() const
{
    return m_defaultRenderComposition != nullptr
        ? m_defaultRenderComposition->GetPostProcessStack()
        : nullptr;
}

template <typename T>
T* SceneRenderer::FindPostProcessEffect() const noexcept
{
    PostProcessStack* const stack = m_defaultRenderComposition != nullptr
        ? m_defaultRenderComposition->GetPostProcessStack()
        : nullptr;
    return stack != nullptr ? stack->GetEffect<T>() : nullptr;
}

RenderFeatureRegistry*
SceneRenderer::GetRenderFeatureRegistry() const noexcept
{
    return m_defaultRenderComposition != nullptr
        ? m_defaultRenderComposition->GetFeatureRegistry()
        : nullptr;
}

RenderPassRegistry* SceneRenderer::GetRenderPassRegistry() const noexcept
{
    return m_defaultRenderComposition != nullptr
        ? &m_defaultRenderComposition->GetPassBundle().GetRegistry()
        : nullptr;
}

namespace
{
    [[nodiscard]] bool TryAccumulatePerformanceBytes(
        uint64& total,
        uint64 value) noexcept
    {
        if (value > std::numeric_limits<uint64>::max() - total)
            return false;
        total += value;
        return true;
    }

    [[nodiscard]] const char* GetPerformanceQualificationBackendName(
        RHIBackendType backend) noexcept
    {
        switch (backend)
        {
            case RHIBackendType::DX11: return "dx11";
            case RHIBackendType::DX12: return "dx12";
            case RHIBackendType::Vulkan: return "vulkan";
            case RHIBackendType::Metal: return "metal";
            case RHIBackendType::OpenGL: return "opengl";
            case RHIBackendType::None: break;
        }
        return "invalid";
    }

    [[nodiscard]] bool TryDerivePerspectiveCaptureDepthConvention(
        const RenderViewSnapshot& view,
        bool& outReverseZ) noexcept
    {
        outReverseZ = false;
        if (!std::isfinite(view.nearPlane) || !std::isfinite(view.farPlane) ||
            view.nearPlane <= 0.0f || view.farPlane <= view.nearPlane)
        {
            return false;
        }

        // The renderer uses a right-handed camera space (forward is -Z).
        // Derive, rather than assume, the frozen device convention from the
        // accepted projection. Orthographic matrices retain W and therefore
        // fail this perspective-only capture contract.
        const Vec4 nearClip = view.projectionMatrix *
            Vec4(0.0f, 0.0f, -view.nearPlane, 1.0f);
        const Vec4 farClip = view.projectionMatrix *
            Vec4(0.0f, 0.0f, -view.farPlane, 1.0f);
        if (!std::isfinite(nearClip.x) || !std::isfinite(nearClip.y) ||
            !std::isfinite(nearClip.z) || !std::isfinite(nearClip.w) ||
            !std::isfinite(farClip.x) || !std::isfinite(farClip.y) ||
            !std::isfinite(farClip.z) || !std::isfinite(farClip.w) ||
            std::abs(nearClip.w) <= 1.0e-6f ||
            std::abs(farClip.w) <= 1.0e-6f ||
            std::abs(nearClip.w - farClip.w) <= 1.0e-6f)
        {
            return false;
        }

        const float32 nearDepth = nearClip.z / nearClip.w;
        const float32 farDepth = farClip.z / farClip.w;
        if (!std::isfinite(nearDepth) || !std::isfinite(farDepth) ||
            nearDepth < -1.0e-4f || nearDepth > 1.0001f ||
            farDepth < -1.0e-4f || farDepth > 1.0001f ||
            std::abs(nearDepth - farDepth) <= 1.0e-6f)
        {
            return false;
        }
        outReverseZ = nearDepth > farDepth;
        return true;
    }

    [[nodiscard]] RenderFeatureCapabilityMask
    BuildRenderFeatureCapabilityMask(const RHICapabilities& capabilities) noexcept
    {
        RenderFeatureCapabilityMask mask =
            ToMask(RenderFeatureCapability::Graphics);
        if (capabilities.supportsComputePipeline)
        {
            mask |= ToMask(RenderFeatureCapability::Compute);
        }
        if (capabilities.supportsDescriptorSets)
        {
            mask |= ToMask(RenderFeatureCapability::DescriptorBindings);
        }
        if (capabilities.supportsRaytracing &&
            capabilities.supportsRaytracingPipeline)
        {
            mask |= ToMask(RenderFeatureCapability::RayTracing);
        }
        if (capabilities.supportsAccelerationStructureUpdate)
        {
            mask |= ToMask(
                RenderFeatureCapability::AccelerationStructureUpdate);
        }
        const bool independentFloatBlendReady =
            capabilities.supportsIndependentColorAttachmentBlend &&
            capabilities.maxColorAttachments >= 2 &&
            capabilities.SupportsColorAttachment(
                RHIFormat::RGBA16_FLOAT) &&
            capabilities.SupportsColorAttachmentBlend(
                RHIFormat::RGBA16_FLOAT) &&
            capabilities.SupportsColorAttachment(
                RHIFormat::R16_FLOAT) &&
            capabilities.SupportsColorAttachmentBlend(
                RHIFormat::R16_FLOAT);
        if (independentFloatBlendReady)
        {
            mask |= ToMask(RenderFeatureCapability::IndependentFloatBlend);
        }
        if (capabilities.maxColorAttachments >= 4)
        {
            mask |= ToMask(RenderFeatureCapability::MultipleRenderTargets);
        }
        // Exact RGBA16F UAV support remains an implementation/pipeline
        // readiness fact. This hardware bit only states that the backend has
        // the compute and descriptor primitives needed to attempt it.
        if (capabilities.supportsComputePipeline &&
            capabilities.supportsDescriptorSets)
        {
            mask |= ToMask(RenderFeatureCapability::StorageTexture);
        }
        return mask;
    }

    [[nodiscard]] RenderDeferredOpaqueExecutionStatus
    ToDeferredOpaqueExecutionStatus(RenderExecutionStatus status) noexcept
    {
        switch (status)
        {
            case RenderExecutionStatus::Completed:
                return RenderDeferredOpaqueExecutionStatus::Completed;
            case RenderExecutionStatus::Failed:
                return RenderDeferredOpaqueExecutionStatus::Failed;
            case RenderExecutionStatus::NotAttempted:
            default:
                return RenderDeferredOpaqueExecutionStatus::NotAttempted;
        }
    }

    [[nodiscard]] RenderHybridRayTracingShadowRoute
    ToPublicHybridShadowRoute(HybridRayTracingShadowRoute route) noexcept
    {
        switch (route)
        {
            case HybridRayTracingShadowRoute::RayTraced:
                return RenderHybridRayTracingShadowRoute::RayTraced;
            case HybridRayTracingShadowRoute::RasterCSM:
                return RenderHybridRayTracingShadowRoute::RasterCSM;
            case HybridRayTracingShadowRoute::Failed:
                return RenderHybridRayTracingShadowRoute::Failed;
            case HybridRayTracingShadowRoute::Disabled:
            default:
                return RenderHybridRayTracingShadowRoute::None;
        }
    }

    [[nodiscard]] RenderHybridRayTracingReflectionRoute
    ToPublicHybridReflectionRoute(
        HybridRayTracingReflectionRoute route) noexcept
    {
        switch (route)
        {
            case HybridRayTracingReflectionRoute::RayTraced:
                return RenderHybridRayTracingReflectionRoute::RayTraced;
            case HybridRayTracingReflectionRoute::ScreenSpaceReflection:
                return RenderHybridRayTracingReflectionRoute::ScreenSpaceReflection;
            case HybridRayTracingReflectionRoute::ReflectionProbe:
                return RenderHybridRayTracingReflectionRoute::ReflectionProbe;
            case HybridRayTracingReflectionRoute::ImageBasedLighting:
                return RenderHybridRayTracingReflectionRoute::ImageBasedLighting;
            case HybridRayTracingReflectionRoute::Failed:
                return RenderHybridRayTracingReflectionRoute::Failed;
            case HybridRayTracingReflectionRoute::Disabled:
            default:
                return RenderHybridRayTracingReflectionRoute::None;
        }
    }

    [[nodiscard]] const std::shared_ptr<const RenderFeatureChannelRegistry>&
    GetBuiltinRenderFeatureChannelRegistry()
    {
        static const auto registry =
            std::make_shared<const RenderFeatureChannelRegistry>(
                CreateBuiltinRenderFeatureChannelRegistry());
        return registry;
    }

    [[nodiscard]] uint64 MixGPUCullingQualificationIdentity(
        uint64 seed, uint64 value) noexcept
    {
        value += 0x9E3779B97F4A7C15ull;
        value = (value ^ (value >> 30u)) * 0xBF58476D1CE4E5B9ull;
        value = (value ^ (value >> 27u)) * 0x94D049BB133111EBull;
        value ^= value >> 31u;
        seed ^= value + 0x9E3779B97F4A7C15ull + (seed << 6u) +
            (seed >> 2u);
        return seed;
    }

    struct SSRPipelineIdentityResolution
    {
        uint64 identity = 0;
        bool ready = false;
    };

    /**
     * @brief Freeze the exact four-pipeline SSR family used by both
     * reflection planning and graph recording.
     */
    [[nodiscard]] SSRPipelineIdentityResolution
    ResolveSSRPipelineIdentity(const PipelineCache* pipelineCache) noexcept
    {
        if (pipelineCache == nullptr)
        {
            return {};
        }

        const PipelineCacheStats& stats = pipelineCache->GetStats();
        const bool ready = stats.ssrHiZBasePipelineHash != 0 &&
            stats.ssrHiZReducePipelineHash != 0 &&
            stats.ssrTracePipelineHash != 0 &&
            stats.ssrResolvePipelineHash != 0 &&
            pipelineCache->GetSSRHiZBasePipeline() != nullptr &&
            pipelineCache->GetSSRHiZReducePipeline() != nullptr &&
            pipelineCache->GetSSRTracePipeline() != nullptr &&
            pipelineCache->GetSSRResolvePipeline() != nullptr;
        if (!ready)
        {
            return {};
        }

        const uint64 identity = ComputeSSRPipelineIdentity(
            stats.ssrHiZBasePipelineHash,
            stats.ssrHiZReducePipelineHash,
            stats.ssrTracePipelineHash,
            stats.ssrResolvePipelineHash);
        return {identity, identity != 0};
    }

    [[nodiscard]] LocalShadowAtlasConfig ResolveLocalShadowAtlasConfig(
        RenderQualityTier quality) noexcept
    {
        LocalShadowAtlasConfig config;
        config.overflowPolicy =
            LocalShadowOverflowPolicy::StableUnshadowedFallback;
        switch (quality)
        {
            case RenderQualityTier::Low:
                config.pageResolution = 512;
                config.pageCapacity = 8;
                config.pointLightCapacity = 1;
                config.spotLightCapacity = 2;
                break;
            case RenderQualityTier::Medium:
                config.pageResolution = 1024;
                config.pageCapacity = 16;
                config.pointLightCapacity = 2;
                config.spotLightCapacity = 4;
                break;
            case RenderQualityTier::High:
            case RenderQualityTier::Ultra:
                config.pageResolution = 1024;
                config.pageCapacity = 32;
                config.pointLightCapacity = 4;
                config.spotLightCapacity = 8;
                break;
            case RenderQualityTier::Count:
            default:
                break;
        }
        return config;
    }

    [[nodiscard]] uint64 HashLocalShadowAtlasConfig(
        const LocalShadowAtlasConfig& config) noexcept
    {
        uint64 hash = 0xCBF29CE484222325ull;
        hash = MixGPUCullingQualificationIdentity(hash, config.pageResolution);
        hash = MixGPUCullingQualificationIdentity(hash, config.pageCapacity);
        hash = MixGPUCullingQualificationIdentity(
            hash, config.pointLightCapacity);
        hash = MixGPUCullingQualificationIdentity(
            hash, config.spotLightCapacity);
        return MixGPUCullingQualificationIdentity(
            hash, static_cast<uint64>(config.overflowPolicy));
    }

    [[nodiscard]] uint64 HashGPUCullingQualificationPacketReference(
        const RenderDrawPacketReference& reference) noexcept
    {
        const RenderDrawPacketId& id = reference.packetId;
        uint64 hash = 0xCBF29CE484222325ull;
        hash = MixGPUCullingQualificationIdentity(hash, id.frameSequence);
        hash = MixGPUCullingQualificationIdentity(hash, id.viewOrdinal);
        hash = MixGPUCullingQualificationIdentity(
            hash, static_cast<uint32>(id.pass));
        hash = MixGPUCullingQualificationIdentity(hash, id.objectId);
        hash = MixGPUCullingQualificationIdentity(hash, id.primitiveData);
        hash = MixGPUCullingQualificationIdentity(hash, id.mesh.slot);
        hash = MixGPUCullingQualificationIdentity(hash, id.mesh.generation);
        hash = MixGPUCullingQualificationIdentity(
            hash, id.logicalSubmeshIndex);
        hash = MixGPUCullingQualificationIdentity(
            hash, id.geometrySubmeshIndex);
        hash = MixGPUCullingQualificationIdentity(hash, id.sourcePacketIndex);
        return MixGPUCullingQualificationIdentity(hash, id.sourceOrdinal);
    }

    void SaturatingAdd(uint64& total, uint64 value, bool& saturated) noexcept
    {
        const uint64 maximum = std::numeric_limits<uint64>::max();
        if (saturated || value > maximum - total)
        {
            total = maximum;
            saturated = true;
            return;
        }
        total += value;
    }

    void AccumulateDirectMutationTotals(
        DirectRasterMutationTotals& target,
        const DirectRasterMutationTotals& source,
        bool& saturated) noexcept
    {
        SaturatingAdd(target.instancePatchedRowCount,
                      source.instancePatchedRowCount,
                      saturated);
        SaturatingAdd(target.indexPatchedRowCount,
                      source.indexPatchedRowCount,
                      saturated);
        SaturatingAdd(target.instanceUploadBytes,
                      source.instanceUploadBytes,
                      saturated);
        SaturatingAdd(target.indexUploadBytes,
                      source.indexUploadBytes,
                      saturated);
        SaturatingAdd(target.instanceFullMaterializationCount,
                      source.instanceFullMaterializationCount,
                      saturated);
        SaturatingAdd(target.indexFullMaterializationCount,
                      source.indexFullMaterializationCount,
                      saturated);
    }

    void AccumulateGPUCullingMutationTotals(
        GPUCullingCanonicalMutationTotals& target,
        const GPUCullingCanonicalMutationTotals& source,
        bool& saturated) noexcept
    {
        SaturatingAdd(target.instancePatchedRowCount, source.instancePatchedRowCount, saturated);
        SaturatingAdd(target.candidatePatchedRowCount, source.candidatePatchedRowCount, saturated);
        SaturatingAdd(target.activeRowPatchedRowCount, source.activeRowPatchedRowCount, saturated);
        SaturatingAdd(target.instanceUploadedRowCount, source.instanceUploadedRowCount, saturated);
        SaturatingAdd(target.candidateUploadedRowCount, source.candidateUploadedRowCount, saturated);
        SaturatingAdd(target.activeRowUploadedRowCount, source.activeRowUploadedRowCount, saturated);
        SaturatingAdd(target.instanceUploadBytes, source.instanceUploadBytes, saturated);
        SaturatingAdd(target.candidateUploadBytes, source.candidateUploadBytes, saturated);
        SaturatingAdd(target.activeRowUploadBytes, source.activeRowUploadBytes, saturated);
        SaturatingAdd(target.instanceFullMaterializationCount, source.instanceFullMaterializationCount, saturated);
        SaturatingAdd(target.candidateFullMaterializationCount, source.candidateFullMaterializationCount, saturated);
        SaturatingAdd(target.activeRowFullMaterializationCount, source.activeRowFullMaterializationCount, saturated);
        SaturatingAdd(target.continuityFullMaterializationCount, source.continuityFullMaterializationCount, saturated);
        SaturatingAdd(target.capacityFullMaterializationCount, source.capacityFullMaterializationCount, saturated);
    }

    bool MutationTotalsDecreased(
        const RenderSceneMutationTotals& current,
        const RenderSceneMutationTotals& previous) noexcept
    {
        return current.fullRebuildCount < previous.fullRebuildCount ||
               current.incrementalCommitCount < previous.incrementalCommitCount ||
               current.rebuiltObjectCount < previous.rebuiltObjectCount ||
               current.removedObjectCount < previous.removedObjectCount;
    }

    bool MutationTotalsDecreased(
        const GPUScenePublicationMutationTotals& current,
        const GPUScenePublicationMutationTotals& previous) noexcept
    {
        return current.fullPublicationCount < previous.fullPublicationCount ||
               current.incrementalPublicationCount < previous.incrementalPublicationCount ||
               current.identityOnlyPublicationCount < previous.identityOnlyPublicationCount ||
               current.materializedObjectCount < previous.materializedObjectCount ||
               current.addCount < previous.addCount ||
               current.updateCount < previous.updateCount ||
               current.removeCount < previous.removeCount ||
               current.noOpCount < previous.noOpCount;
    }

    bool MutationTotalsDecreased(
        const GPUSceneUploadMutationTotals& current,
        const GPUSceneUploadMutationTotals& previous) noexcept
    {
        if (current.submittedUploadCount < previous.submittedUploadCount ||
               current.uploadBytes < previous.uploadBytes ||
               current.uploadRangeCount < previous.uploadRangeCount ||
               current.fullUploadCount < previous.fullUploadCount)
        {
            return true;
        }
        for (uint32 tableIndex = 0;
             tableIndex < GPU_SCENE_DIAGNOSTICS_TABLE_COUNT;
             ++tableIndex)
        {
            if (current.submittedUploadedRowCount[tableIndex] <
                previous.submittedUploadedRowCount[tableIndex])
            {
                return true;
            }
        }
        return false;
    }

    bool MutationTotalsDecreased(
        const GPUCullingCanonicalMutationTotals& current,
        const GPUCullingCanonicalMutationTotals& previous) noexcept
    {
        return current.instancePatchedRowCount < previous.instancePatchedRowCount ||
               current.candidatePatchedRowCount < previous.candidatePatchedRowCount ||
               current.activeRowPatchedRowCount < previous.activeRowPatchedRowCount ||
               current.instanceUploadedRowCount < previous.instanceUploadedRowCount ||
               current.candidateUploadedRowCount < previous.candidateUploadedRowCount ||
               current.activeRowUploadedRowCount < previous.activeRowUploadedRowCount ||
               current.instanceUploadBytes < previous.instanceUploadBytes ||
               current.candidateUploadBytes < previous.candidateUploadBytes ||
               current.activeRowUploadBytes < previous.activeRowUploadBytes ||
               current.instanceFullMaterializationCount < previous.instanceFullMaterializationCount ||
               current.candidateFullMaterializationCount < previous.candidateFullMaterializationCount ||
               current.activeRowFullMaterializationCount < previous.activeRowFullMaterializationCount ||
               current.continuityFullMaterializationCount < previous.continuityFullMaterializationCount ||
               current.capacityFullMaterializationCount < previous.capacityFullMaterializationCount;
    }

    bool MutationTotalsDecreased(
        const DirectRasterMutationTotals& current,
        const DirectRasterMutationTotals& previous) noexcept
    {
        return current.instancePatchedRowCount < previous.instancePatchedRowCount ||
               current.indexPatchedRowCount < previous.indexPatchedRowCount ||
               current.instanceUploadBytes < previous.instanceUploadBytes ||
               current.indexUploadBytes < previous.indexUploadBytes ||
               current.instanceFullMaterializationCount < previous.instanceFullMaterializationCount ||
               current.indexFullMaterializationCount < previous.indexFullMaterializationCount;
    }

    bool MutationEvidenceDecreased(
        const RenderMutationEvidenceDiagnostics& current,
        const RenderMutationEvidenceDiagnostics& previous) noexcept
    {
        return current.gpuCullingOwnerCount < previous.gpuCullingOwnerCount ||
               current.directRasterOwnerCount < previous.directRasterOwnerCount ||
               MutationTotalsDecreased(current.scene, previous.scene) ||
               MutationTotalsDecreased(current.gpuScenePublication,
                                       previous.gpuScenePublication) ||
               MutationTotalsDecreased(current.gpuSceneUpload,
                                       previous.gpuSceneUpload) ||
               MutationTotalsDecreased(current.gpuCulling, previous.gpuCulling) ||
               MutationTotalsDecreased(current.directRaster, previous.directRaster);
    }
    void PopulatePlanMeasurement(RenderPolicyDiagnostics& diagnostics,
                                 const RenderViewDrawPlan* drawPlan)
    {
        RenderPolicyMeasurement& measurement = diagnostics.measurement;
        measurement.candidatePacketCount = 0;
        measurement.drawGroupCount = 0;
        measurement.averageGroupOccupancy = 0.0;
        measurement.averageGroupOccupancyAvailable = false;
        measurement.semanticPacketCount = 0;
        measurement.visiblePacketCount = 0;
        measurement.semanticPrimitiveCount = 0;
        measurement.visiblePrimitiveCount = 0;
        measurement.materialBindingCount = 0;
        measurement.uniqueMaterialCount = 0;
        measurement.semanticDrawGroupCount = 0;
        measurement.drawPlanHash = 0;

        if (!diagnostics.planAvailable)
        {
            return;
        }

        const RenderFrameExecutionPlan& plan = diagnostics.selectedPlan;
        measurement.frameSequence = plan.frameSequence;
        for (const RenderPassExecutionPlan& pass : plan.passes)
        {
            // These are pass-packet aggregates. Depth/Opaque can intentionally
            // represent the same source primitive in separate pass lanes.
            measurement.candidatePacketCount +=
                pass.partition.candidatePacketCount;
            measurement.drawGroupCount += pass.partition.drawGroupCount;
        }
        if (measurement.drawGroupCount != 0)
        {
            measurement.averageGroupOccupancy =
                static_cast<float64>(measurement.candidatePacketCount) /
                static_cast<float64>(measurement.drawGroupCount);
            measurement.averageGroupOccupancyAvailable = true;
        }
        if (drawPlan == nullptr || !drawPlan->IsValid())
        {
            return;
        }
        measurement.drawPlanHash = drawPlan->semanticHash;
        for (const RenderPassDrawPlan& pass : drawPlan->passes)
        {
            measurement.semanticPacketCount += pass.stats.semanticPacketCount;
            measurement.visiblePacketCount += pass.stats.visiblePacketCount;
            measurement.semanticPrimitiveCount +=
                pass.stats.candidatePrimitiveCount;
            measurement.visiblePrimitiveCount +=
                pass.stats.visiblePrimitiveCount;
            measurement.materialBindingCount +=
                pass.stats.materialBindingCount;
            measurement.uniqueMaterialCount += pass.stats.uniqueMaterialCount;
            measurement.semanticDrawGroupCount +=
                pass.stats.semanticDrawGroupCount;
        }
    }

    void PopulateSubmissionMeasurement(RenderPolicyDiagnostics& diagnostics)
    {
        RenderPolicyMeasurement& measurement = diagnostics.measurement;
        measurement.submissionCpuNanoseconds = 0;
        measurement.submissionCpuTimingAvailable = false;

        for (const RenderPassExecutionReport& pass :
             diagnostics.executionReport.passes)
        {
            if (pass.pass != RenderPassKind::Depth &&
                pass.pass != RenderPassKind::Opaque)
            {
                continue;
            }
            const std::array<const RenderPassLaneExecutionReport*, 2> lanes{
                &pass.gpuDrivenLane, &pass.directLane};
            for (const RenderPassLaneExecutionReport* lane : lanes)
            {
                if (!lane->submissionCpuTimingAvailable)
                {
                    continue;
                }
                measurement.submissionCpuNanoseconds +=
                    lane->submissionCpuNanoseconds;
                measurement.submissionCpuTimingAvailable = true;
            }
        }
    }

    void AccumulateUploadWork(RenderUploadWorkDiagnostics& target,
                              const RenderUploadWorkDiagnostics& source)
    {
        const bool targetHadCommittedRanges = target.committedRangeCount != 0;
        const bool sourceHasCommittedRanges = source.committedRangeCount != 0;
        target.cpuCopiedPayloadBytes += source.cpuCopiedPayloadBytes;
        target.committedPayloadBytes += source.committedPayloadBytes;
        target.mappedRangeCount += source.mappedRangeCount;
        target.committedRangeCount += source.committedRangeCount;
        target.gpuCopyBytes += source.gpuCopyBytes;
        target.gpuCopyRangeCount += source.gpuCopyRangeCount;
        for (uint32 scope = 0;
             scope < RVX_RENDER_HOST_WRITE_SYNCHRONIZATION_SCOPE_COUNT;
             ++scope)
        {
            target.hostVisibilitySynchronizationScopeRangeCounts[scope] +=
                source.hostVisibilitySynchronizationScopeRangeCounts[scope];
            target.hostVisibilitySynchronizationScopeBytes[scope] +=
                source.hostVisibilitySynchronizationScopeBytes[scope];
        }

        if (!sourceHasCommittedRanges)
        {
            return;
        }
        if (!source.hostVisibilitySynchronizedBytes.IsAvailable())
        {
            target.hostVisibilitySynchronizedBytes =
                DiagnosticValue<uint64>::Unavailable(
                    source.hostVisibilitySynchronizedBytes.GetReason());
            return;
        }
        if (!targetHadCommittedRanges)
        {
            target.hostVisibilitySynchronizedBytes =
                source.hostVisibilitySynchronizedBytes;
            return;
        }
        if (!target.hostVisibilitySynchronizedBytes.IsAvailable())
        {
            return;
        }
        target.hostVisibilitySynchronizedBytes =
            DiagnosticValue<uint64>::Available(
                target.hostVisibilitySynchronizedBytes.GetValue().value_or(0) +
                source.hostVisibilitySynchronizedBytes.GetValue().value_or(0));
    }

    bool IsExactPostRecordingGraphicsSubmission(
        const RenderSubmissionTracker& tracker,
        uint64 recordingGraphicsBaseline,
        const GPUCompletionToken& completion) noexcept
    {
        GPUCompletionToken normalized;
        if (!MergeGPUCompletionToken(normalized, completion) ||
            normalized.count == 0 ||
            tracker.Query(normalized) == GPUCompletionStatus::Lost)
        {
            return false;
        }

        uint64 graphicsValue = 0;
        for (uint8 pointIndex = 0; pointIndex < normalized.count; ++pointIndex)
        {
            const GPUCompletionPoint point = normalized.points[pointIndex];
            if (point.value != tracker.GetLastSubmittedValue(point.domain))
            {
                return false;
            }
            if (point.domain == GPUQueueDomain::Graphics)
            {
                graphicsValue = point.value;
            }
        }
        return graphicsValue != 0 &&
               graphicsValue > recordingGraphicsBaseline;
    }

    MeshPassResourceAvailability ResolvePassResourceAvailability(
        const RenderResourceRegistry* registry,
        RenderResourceHandle handle,
        bool gpuDataReady)
    {
        if (registry == nullptr || !handle.IsValid())
        {
            return MeshPassResourceAvailability::Unavailable;
        }

        const RenderResourceStatus status = registry->QueryStatus(handle);
        if (status.code != RenderResourceStatusCode::Current)
        {
            return MeshPassResourceAvailability::Unavailable;
        }

        switch (status.state)
        {
            case RenderResourcePublicState::Reserved:
            case RenderResourcePublicState::UploadQueued:
            case RenderResourcePublicState::Uploading:
                return MeshPassResourceAvailability::Pending;
            case RenderResourcePublicState::GPUReady:
            case RenderResourcePublicState::ReplacementQueued:
            case RenderResourcePublicState::Replacing:
                return gpuDataReady
                           ? MeshPassResourceAvailability::Ready
                           : MeshPassResourceAvailability::Unavailable;
            case RenderResourcePublicState::Released:
            case RenderResourcePublicState::Failed:
            case RenderResourcePublicState::Evicting:
            default:
                return MeshPassResourceAvailability::Unavailable;
        }
    }

    RenderDrawFlags SetRenderDrawFlag(RenderDrawFlags flags,
                                      RenderDrawFlags flag,
                                      bool enabled)
    {
        const uint32 flagBits = static_cast<uint32>(flag);
        const uint32 resultBits = enabled
                                      ? static_cast<uint32>(flags) | flagBits
                                      : static_cast<uint32>(flags) & ~flagBits;
        return static_cast<RenderDrawFlags>(resultBits);
    }

    [[nodiscard]] bool HasMirroredWinding(const Mat4& worldMatrix) noexcept
    {
        const float32 determinant = glm::determinant(Mat3(worldMatrix));
        return std::isfinite(determinant) && determinant < 0.0f;
    }

    void ResolveDrawPacketPreparationBindings(
        RenderDrawPacket& packet,
        const RenderResourceRegistry* registry,
        const MaterialSystem* materialSystem)
    {
        const MeshGPUBuffers buffers = ResolveRenderMeshBuffers(
            registry, packet.geometryKey.mesh);
        if (buffers.IsValid() &&
            packet.geometryKey.submeshIndex < buffers.submeshes.size())
        {
            const SubmeshGPUInfo& submesh =
                buffers.submeshes[packet.geometryKey.submeshIndex];
            packet.arguments.indexCount = submesh.indexCount;
            packet.arguments.firstIndex = submesh.indexOffset;
            packet.arguments.vertexOffset = submesh.baseVertex;
        }
        packet.materialInstanceKey = materialSystem
            ? materialSystem->ResolveInstanceBindingKey(
                  packet.materialKey.material)
            : MaterialInstanceBindingKey{};
        if (packet.materialKey.material.IsValid())
        {
            packet.pipelineKey.materialPolicyValid = materialSystem != nullptr &&
                materialSystem->ResolveSurfacePipelinePolicy(
                    packet.materialKey.material,
                    packet.pipelineKey.cullMode,
                    packet.pipelineKey.shadowPolicy,
                    packet.pipelineKey.requiresUV1,
                    packet.pipelineKey.surfacePathEligibility,
                    packet.pipelineKey.blendMode,
                    packet.pipelineKey.shadingModel,
                    packet.translucencySortPriority);
            packet.flags = ApplyMaterialShadowPolicy(
                packet.flags,
                packet.pipelineKey.materialPolicyValid,
                packet.pipelineKey.shadowPolicy);
            // Preserve object handedness even for no-cull surfaces. The
            // material shader consumes SV_IsFrontFace to orient the TBN for
            // double-sided lighting, so front-face selection remains part of
            // the observable pipeline contract when raster culling is off.
        }
        else
        {
            packet.pipelineKey.cullMode = MaterialCullMode::Back;
            packet.pipelineKey.shadowPolicy =
                MaterialShadowPolicy::CastAndReceive;
            packet.pipelineKey.materialPolicyValid = true;
            packet.pipelineKey.requiresUV1 = false;
            packet.pipelineKey.blendMode = packet.materialKey.materialMode ==
                    RenderMaterialMode::Transparent
                ? MaterialBlendMode::AlphaBlend
                : MaterialBlendMode::Opaque;
            packet.pipelineKey.shadingModel =
                MaterialShadingModel::StandardLit;
            packet.translucencySortPriority = 0;
            packet.pipelineKey.surfacePathEligibility =
                packet.materialKey.materialMode == RenderMaterialMode::Transparent
                    ? SurfacePathEligibility::ForwardTranslucency
                    : SurfacePathEligibility::ForwardSpecialOpaque;
        }
    }

    class RenderDrawClassificationBindingResolver final
        : public IRenderDrawClassificationBindingResolver
    {
    public:
        RenderDrawClassificationBindingResolver(
            const RenderResourceRegistry* registry,
            const MaterialSystem* materialSystem) noexcept
            : m_registry(registry),
              m_materialSystem(materialSystem)
        {
        }

        void Resolve(RenderDrawPacket& packet) const override
        {
            ResolveDrawPacketPreparationBindings(
                packet, m_registry, m_materialSystem);
        }

    private:
        const RenderResourceRegistry* m_registry = nullptr;
        const MaterialSystem* m_materialSystem = nullptr;
    };

    MeshPassProcessorInput MakeMeshPassProcessorInput(
        RenderDrawPacket packet,
        const RenderResourceRegistry* registry,
        const MaterialSystem* materialSystem,
        uint32 sourceOrdinal,
        float32 viewDepth,
        uint32 classificationIndex = RVX_INVALID_INDEX)
    {
        ResolveDrawPacketPreparationBindings(
            packet, registry, materialSystem);
        const MeshGPUBuffers buffers = ResolveRenderMeshBuffers(
            registry, packet.geometryKey.mesh);
        const bool geometryReady = buffers.IsValid() &&
            packet.geometryKey.submeshIndex < buffers.submeshes.size();

        MeshPassProcessorInput input;
        input.packet = packet;
        if (registry != nullptr)
        {
            input.geometryAssetId =
                registry->GetExactAssetId(packet.geometryKey.mesh);
            input.materialAssetId =
                registry->GetExactAssetId(packet.materialKey.material);
        }
        input.availability.pipeline = MeshPassResourceAvailability::Ready;
        input.availability.geometry = ResolvePassResourceAvailability(
            registry, packet.geometryKey.mesh, geometryReady);
        input.availability.material = ResolvePassResourceAvailability(
            registry,
            packet.materialKey.material,
            registry != nullptr &&
                registry->ResolveMaterial(packet.materialKey.material) != nullptr);
        input.sourceOrdinal = sourceOrdinal;
        input.viewDepth = viewDepth;
        input.classificationIndex = classificationIndex;
        return input;
    }

    [[nodiscard]] uint32 GetGPUSceneCullingCandidatePassMask(
        RenderPassKind pass) noexcept
    {
        switch (pass)
        {
            case RenderPassKind::Depth:
                return static_cast<uint32>(GPUScenePassMask::Depth);
            case RenderPassKind::Opaque:
                return static_cast<uint32>(GPUScenePassMask::Opaque);
            case RenderPassKind::None:
            case RenderPassKind::Shadow:
            case RenderPassKind::Transparent:
            default:
                return 0;
        }
    }

    RenderDrawPacket BuildShadowPacket(const RenderScene& scene,
                                       const RenderObject& object,
                                       const MeshBatch& batch)
    {
        RenderDrawPacket packet;
        if (scene.FindCachedDrawPacketTemplate(batch, packet))
        {
            packet.objectId = batch.objectId;
            packet.primitiveData = batch.primitiveData;
            packet.submeshIndex = batch.submeshIndex;
        }
        else
        {
            packet = BuildLegacyMaterialDrawPacket(batch);
        }
        packet.pipelineKey.reverseWinding =
            HasMirroredWinding(object.worldMatrix);
        packet.flags = SetRenderDrawFlag(
            packet.flags,
            RenderDrawFlags::CastsShadow,
            packet.pipelineKey.materialPolicyValid && object.castsShadow &&
                MaterialShadowPolicyCasts(
                    packet.pipelineKey.shadowPolicy));
        packet.flags = ApplyMaterialShadowPolicy(
            packet.flags,
            packet.pipelineKey.materialPolicyValid,
            packet.pipelineKey.shadowPolicy);
        const bool skinned = object.HasSkinningData();
        packet.flags = SetRenderDrawFlag(
            packet.flags, RenderDrawFlags::Skinned, skinned);
        // Shadow packets specialize the pass and shadow flag after reading a
        // retained template. Keep their pipeline identity synchronized with
        // the object-level skinning contract as well.
        packet.pipelineKey.skinned = skinned;
        return packet;
    }

    MeshBatch BuildLegacyShadowBatch(const RenderObject& object,
                                     uint32 objectIndex,
                                     uint32 submeshIndex,
                                     const SubmeshGPUInfo* submesh)
    {
        RenderBatchFlags flags = RenderBatchFlags::None;
        if (object.HasSkinningData())
            flags |= RenderBatchFlags::Skinned;
        if (object.castsShadow)
            flags |= RenderBatchFlags::CastsShadow;
        if (object.receivesShadow)
            flags |= RenderBatchFlags::ReceivesShadow;

        const RenderMaterialMode materialMode =
            submeshIndex < object.materialModes.size()
                ? object.materialModes[submeshIndex]
                : RenderMaterialMode::Opaque;
        if (materialMode == RenderMaterialMode::Masked)
            flags |= RenderBatchFlags::Masked;
        if (materialMode == RenderMaterialMode::Transparent)
            flags |= RenderBatchFlags::Transparent;
        if (!object.material.IsValid())
            flags |= RenderBatchFlags::MissingMaterial;

        MeshBatch batch;
        batch.objectId = object.entityId;
        batch.mesh = object.mesh;
        batch.material = object.material;
        batch.submeshIndex = submeshIndex;
        batch.primitiveData = objectIndex;
        batch.materialMode = materialMode;
        batch.flags = flags;
        if (submesh != nullptr)
        {
            batch.geometry.indexOffset = submesh->indexOffset;
            batch.geometry.indexCount = submesh->indexCount;
            batch.geometry.baseVertex = submesh->baseVertex;
        }
        return batch;
    }

    uint64 GetRenderObjectHistoryKey(const RenderObject& object)
    {
        return object.entityId;
    }

    float ClampFiniteRange(float value, float fallback, float minValue, float maxValue)
    {
        return std::isfinite(value) ? std::clamp(value, minValue, maxValue) : fallback;
    }

    uint32 ResolveRayTracingBudgetReflectionDimension(uint32 dimension, float scale)
    {
        if (dimension == 0)
            return 0;

        const float scaled = std::ceil(static_cast<float>(dimension) * scale);
        return std::max(1u, static_cast<uint32>(scaled));
    }

    uint64 EstimateRayTracingPixelCount(uint32 width, uint32 height)
    {
        return static_cast<uint64>(width) * static_cast<uint64>(height);
    }

    uint64 EstimateReflectionRayCount(uint32 width, uint32 height, float resolutionScale, uint32 samplesPerPixel)
    {
        const uint32 scaledWidth = ResolveRayTracingBudgetReflectionDimension(width, resolutionScale);
        const uint32 scaledHeight = ResolveRayTracingBudgetReflectionDimension(height, resolutionScale);
        return EstimateRayTracingPixelCount(scaledWidth, scaledHeight) * static_cast<uint64>(samplesPerPixel);
    }

    float ReduceReflectionResolutionScaleToFitRayBudget(uint32 width,
                                                        uint32 height,
                                                        float resolutionScale,
                                                        float minResolutionScale,
                                                        uint32 samplesPerPixel,
                                                        uint64 availableRayBudget)
    {
        if (width == 0 || height == 0 || samplesPerPixel == 0 || availableRayBudget == 0)
        {
            return minResolutionScale;
        }

        float scale = std::clamp(resolutionScale, minResolutionScale, 1.0f);
        uint64 estimatedRayCount = EstimateReflectionRayCount(width, height, scale, samplesPerPixel);
        while (estimatedRayCount > availableRayBudget && scale > minResolutionScale)
        {
            const uint32 scaledWidth = ResolveRayTracingBudgetReflectionDimension(width, scale);
            const uint32 scaledHeight = ResolveRayTracingBudgetReflectionDimension(height, scale);

            float nextScale = scale;
            if (scaledWidth > 1)
            {
                nextScale = std::min(
                    nextScale,
                    std::nextafter(static_cast<float>(scaledWidth - 1u) / static_cast<float>(width), 0.0f));
            }
            if (scaledHeight > 1)
            {
                nextScale = std::min(
                    nextScale,
                    std::nextafter(static_cast<float>(scaledHeight - 1u) / static_cast<float>(height), 0.0f));
            }

            nextScale = std::max(minResolutionScale, nextScale);
            if (nextScale >= scale)
            {
                break;
            }

            scale = nextScale;
            estimatedRayCount = EstimateReflectionRayCount(width, height, scale, samplesPerPixel);
        }

        return scale;
    }

    uint32 EstimateReflectionDenoiseKernelTapCount(uint32 radius)
    {
        if (radius == 0)
            return 0;

        const uint32 diameter = radius * 2u + 1u;
        return diameter * diameter;
    }

    uint64 EstimateReflectionDenoiseTapCount(uint32 width, uint32 height, float resolutionScale, uint32 radius)
    {
        const uint32 scaledWidth = ResolveRayTracingBudgetReflectionDimension(width, resolutionScale);
        const uint32 scaledHeight = ResolveRayTracingBudgetReflectionDimension(height, resolutionScale);
        return EstimateRayTracingPixelCount(scaledWidth, scaledHeight) *
               static_cast<uint64>(EstimateReflectionDenoiseKernelTapCount(radius));
    }

    uint32 ApplyRayTracingGpuBudgetScale(uint32 value, uint32 minValue, float qualityScale)
    {
        if (value == 0 || qualityScale >= 0.999f)
            return value;

        const uint32 scaledValue = std::max(minValue,
                                           static_cast<uint32>(std::floor(static_cast<float>(value) * qualityScale)));
        if (scaledValue == value && value > minValue)
            return value - 1u;

        return scaledValue;
    }

    PostProcessSettings MakeDefaultRuntimePostProcessSettings()
    {
        PostProcessSettings settings;
        settings.enableToneMapping = true;
        settings.exposureSettings = {};
        settings.gamma = 2.2f;
        settings.toneMappingOperator = ToneMappingOperator::ACES;
        settings.enableBloom = true;
        settings.bloomIntensity = 0.0f;
        settings.antiAliasingMode = RenderAntiAliasingMode::None;
        settings.enableDOF = false;
        settings.enableMotionBlur = false;
        settings.enableColorGrading = false;
        settings.enableVignette = false;
        settings.enableChromaticAberration = false;
        settings.enableFilmGrain = false;
        settings.enableSSR = false;
        settings.enableRayTracedReflections = false;
        settings.enableRayTracedReflectionDenoise = true;
        return settings;
    }

    [[nodiscard]] RenderAmbientOcclusionSettings
    ResolveAmbientOcclusionSettings(
        const RenderFrameSettings& frameSettings) noexcept
    {
        RenderAmbientOcclusionSettings settings =
            frameSettings.postProcess.ambientOcclusion;
        settings.enabled = frameSettings.postProcess.enabled &&
            settings.enabled;
        return settings;
    }

    RenderPolicyReadiness ResolveGPUShaderReadiness(
        const GPUCullingExecutionDecision& decision)
    {
        switch (decision.fallbackReason)
        {
            case GPUCullingFallbackReason::ShaderFileMissing:
            case GPUCullingFallbackReason::ShaderCompilationFailed:
                return RenderPolicyReadiness::Unavailable;
            default:
                return RenderPolicyReadiness::Ready;
        }
    }

    RenderPolicyReadiness ResolveGPUBindingReadiness(
        const GPUCullingExecutionDecision& decision)
    {
        switch (decision.fallbackReason)
        {
            case GPUCullingFallbackReason::DescriptorSetLayoutCreationFailed:
            case GPUCullingFallbackReason::PipelineLayoutCreationFailed:
            case GPUCullingFallbackReason::DescriptorSetCreationFailed:
                return RenderPolicyReadiness::Unavailable;
            default:
                return RenderPolicyReadiness::Ready;
        }
    }

    RenderPolicyReadiness ResolveGPUResourceReadiness(
        const GPUCullingExecutionDecision& decision)
    {
        return decision.fallbackReason ==
                       GPUCullingFallbackReason::PipelineResourcesUnavailable
            ? RenderPolicyReadiness::Unavailable
            : RenderPolicyReadiness::Ready;
    }

    bool IsGPUImplementationAvailable(
        const GPUCullingExecutionDecision& decision)
    {
        return decision.fallbackReason != GPUCullingFallbackReason::DeviceMissing &&
               decision.fallbackReason !=
                   GPUCullingFallbackReason::ShaderBackendUnsupported;
    }

    RenderPassPolicyFacts MakeRenderPassPolicyFacts(
        RenderPassKind pass,
        const IRenderPass* renderPass,
        const MeshPassPacketStream& stream,
        const GPUCullingExecutionDecision& gpuExecution,
        bool gpuPassPipelineReady)
    {
        RenderPassPolicyFacts facts;
        facts.pass = pass;
        facts.requested = renderPass && renderPass->IsRequestedEnabled();
        facts.supported = renderPass && renderPass->IsSupported();
        facts.directAllowed = facts.supported;
        facts.gpuDrivenAllowed = facts.supported &&
            (pass == RenderPassKind::Depth || pass == RenderPassKind::Opaque);
        facts.fixedCountIndirectAllowed = false;
        const RenderPolicyReadiness directReadiness = facts.supported
            ? RenderPolicyReadiness::Ready
            : RenderPolicyReadiness::Unavailable;
        facts.directShaderReadiness = directReadiness;
        facts.directPipelineReadiness = directReadiness;
        facts.directResourceReadiness = directReadiness;
        facts.gpuDrivenShaderReadiness =
            ResolveGPUShaderReadiness(gpuExecution);
        facts.gpuDrivenPipelineReadiness =
            gpuExecution.pipelineReady && gpuPassPipelineReady
            ? RenderPolicyReadiness::Ready
            : RenderPolicyReadiness::Unavailable;
        facts.gpuDrivenResourceReadiness =
            ResolveGPUResourceReadiness(gpuExecution);
        facts.inputPacketCount = stream.stats.inputPacketCount;
        facts.relevantPacketCount = stream.stats.relevantPacketCount;
        facts.candidatePacketCount = stream.stats.gpuCandidatePacketCount;
        facts.directPacketCount = stream.stats.directPacketCount;
        facts.skippedPacketCount = stream.stats.skippedPacketCount;
        facts.drawGroupCount = static_cast<uint32>(stream.groups.size());
        facts.workloadBeneficial = facts.candidatePacketCount != 0;
        return facts;
    }

    GPUDrivenPolicyReason ProjectLegacyPolicyReason(RenderPolicyReason reason)
    {
        switch (reason)
        {
            case RenderPolicyReason::None:
            case RenderPolicyReason::ForcedGPUDriven:
                return GPUDrivenPolicyReason::None;
            case RenderPolicyReason::InvalidRequest:
                return GPUDrivenPolicyReason::InvalidMode;
            case RenderPolicyReason::ForcedDirect:
                return GPUDrivenPolicyReason::ForcedDisabled;
            case RenderPolicyReason::BackendNotQualified:
                return GPUDrivenPolicyReason::BackendNotQualified;
            case RenderPolicyReason::QualificationInvalid:
                return GPUDrivenPolicyReason::QualificationInvalid;
            case RenderPolicyReason::PipelineUnavailable:
            case RenderPolicyReason::PipelinePending:
            case RenderPolicyReason::ShaderUnavailable:
            case RenderPolicyReason::ShaderPending:
            case RenderPolicyReason::BindingsUnavailable:
            case RenderPolicyReason::BindingsPending:
                return GPUDrivenPolicyReason::PipelineUnavailable;
            case RenderPolicyReason::CapabilityUnavailable:
                return GPUDrivenPolicyReason::IndirectDrawCountUnsupported;
            default:
                return GPUDrivenPolicyReason::PipelineUnavailable;
        }
    }

}

bool SceneRendererExternalTargetDesc::IsValid() const
{
    return colorTarget &&
           colorTarget->GetWidth() > 0 &&
           colorTarget->GetHeight() > 0;
}

SceneRenderer::SceneRenderer()
    : m_gpuSceneUpdate(std::make_unique<GPUSceneUpdate>()),
      m_viewHistoryCoordinator(std::make_unique<RenderViewHistoryCoordinator>())
{
}

SceneRenderer::SceneRenderer(
    std::unique_ptr<IShaderProgramProvider> shaderProgramProvider)
    : m_shaderProgramProvider(std::move(shaderProgramProvider)),
      m_gpuSceneUpdate(std::make_unique<GPUSceneUpdate>()),
      m_viewHistoryCoordinator(std::make_unique<RenderViewHistoryCoordinator>())
{
}

SceneRenderer::~SceneRenderer()
{
    Shutdown();
}

bool SceneRenderer::SupportsHDRSceneColor() const
{
    const IRHIDevice* device = m_renderContext
        ? m_renderContext->GetDevice()
        : m_featureReportDeviceForTesting;
    if (!device)
    {
        return false;
    }

    switch (device->GetBackendType())
    {
        case RHIBackendType::DX11:
        case RHIBackendType::DX12:
        case RHIBackendType::Vulkan:
        case RHIBackendType::Metal:
        case RHIBackendType::OpenGL:
            return true;
        case RHIBackendType::Auto:
        case RHIBackendType::None:
        default:
            return false;
    }
}

ToneMappingOutputColorSpace SceneRenderer::ResolveToneMappingOutputColorSpace(RHIFormat outputFormat) const
{
    if (IsSRGBFormat(outputFormat))
    {
        return ToneMappingOutputColorSpace::Linear;
    }

    switch (outputFormat)
    {
        case RHIFormat::R16_FLOAT:
        case RHIFormat::R32_FLOAT:
        case RHIFormat::RG16_FLOAT:
        case RHIFormat::RG32_FLOAT:
        case RHIFormat::RG11B10_FLOAT:
        case RHIFormat::RGBA16_FLOAT:
        case RHIFormat::RGBA32_FLOAT:
            return ToneMappingOutputColorSpace::Linear;
        case RHIFormat::RGBA8_UNORM:
        case RHIFormat::BGRA8_UNORM:
        case RHIFormat::RGB10A2_UNORM:
        default:
            return ToneMappingOutputColorSpace::SRGB;
    }
}

bool SceneRenderer::ValidateDeferredToneMappingPresentationContract(
    const PostProcessStackExecuteStats& stats,
    RHIFormat backBufferFormat,
    std::string& outDiagnostic)
{
    outDiagnostic.clear();
    if (backBufferFormat == RHIFormat::Unknown ||
        stats.finalOutputFormat != backBufferFormat)
    {
        outDiagnostic =
            "DeferredQuality requires ToneMapping validation against the real backbuffer format";
        return false;
    }
    if (!stats.toneMappingBoundaryValid)
    {
        outDiagnostic = stats.toneMappingBoundaryWarning.empty()
            ? "DeferredQuality ToneMapping boundary is invalid"
            : stats.toneMappingBoundaryWarning;
        return false;
    }
    if (stats.fallbackCopyApplied)
    {
        outDiagnostic =
            "DeferredQuality cannot present HDR scene color through a fallback copy";
        return false;
    }
    if (stats.requestedToneMappingEffectCount != 1 ||
        stats.supportedToneMappingEffectCount != 1 ||
        stats.enabledToneMappingEffectCount != 1)
    {
        outDiagnostic =
            "DeferredQuality requires exactly one requested, supported, and enabled ToneMapping transform";
        return false;
    }

    const auto toneMappingPlan = std::find_if(
        stats.effectPlans.begin(), stats.effectPlans.end(),
        [](const PostProcessEffectExecutionPlan& plan)
        {
            return plan.effectName == "ToneMapping";
        });
    if (toneMappingPlan == stats.effectPlans.end() ||
        !toneMappingPlan->requested || !toneMappingPlan->supported ||
        !toneMappingPlan->enabled || !toneMappingPlan->pipelineReady)
    {
        outDiagnostic =
            "DeferredQuality ToneMapping pipeline is unavailable for the presentation boundary";
        return false;
    }
    return true;
}

bool SceneRenderer::ValidateDeferredToneMappingSemanticEvidence(
    const RenderGraph& graph,
    const PostProcessStackExecuteStats& stats,
    RHIFormat backBufferFormat,
    std::string& outDiagnostic)
{
    if (!ValidateDeferredToneMappingPresentationContract(
            stats, backBufferFormat, outDiagnostic))
    {
        return false;
    }
    if (stats.scheduledToneMappingEffectCount != 1)
    {
        outDiagnostic =
            "DeferredQuality did not schedule exactly one ToneMapping transform";
        return false;
    }

    const auto scheduledPlan = std::find_if(
        stats.effectPlans.begin(), stats.effectPlans.end(),
        [backBufferFormat](const PostProcessEffectExecutionPlan& plan)
        {
            return plan.effectName == "ToneMapping" && plan.scheduled &&
                   plan.inputFormat == RHIFormat::RGBA16_FLOAT &&
                   plan.outputFormat == backBufferFormat;
        });
    if (scheduledPlan == stats.effectPlans.end())
    {
        outDiagnostic =
            "DeferredQuality ToneMapping did not schedule the RGBA16F-to-backbuffer transform";
        return false;
    }

    const RenderGraph::Diagnostics diagnostics = graph.GetDiagnostics();
    const auto toneMappingPass = std::find_if(
        diagnostics.passes.begin(), diagnostics.passes.end(),
        [](const RenderGraph::PassDiagnostic& pass)
        {
            return pass.name == "ToneMapping";
        });
    if (toneMappingPass == diagnostics.passes.end())
    {
        outDiagnostic =
            "DeferredQuality ToneMapping has no RenderGraph execution evidence";
        return false;
    }
    if (std::count_if(diagnostics.passes.begin(), diagnostics.passes.end(),
                      [](const RenderGraph::PassDiagnostic& pass)
                      {
                          return pass.name == "ToneMapping";
                      }) != 1)
    {
        outDiagnostic =
            "DeferredQuality RenderGraph contains multiple ToneMapping transforms";
        return false;
    }
    if (toneMappingPass->culled || !toneMappingPass->executedLastRun ||
        !toneMappingPass->semanticExecutionSucceededLastRun)
    {
        outDiagnostic = toneMappingPass->semanticExecutionDiagnostic.empty()
            ? "DeferredQuality ToneMapping did not record its required fullscreen draw"
            : toneMappingPass->semanticExecutionDiagnostic;
        return false;
    }
    return true;
}

SceneColorFormatPolicy SceneRenderer::ResolveSceneColorFormatPolicy(RHIFormat backBufferFormat,
                                                                    bool postProcessActive) const
{
    SceneColorFormatPolicy policy;
    policy.backBufferFormat = backBufferFormat;
    policy.toneMappingOutputFormat = backBufferFormat;
    policy.toneMappingOutputColorSpace = ResolveToneMappingOutputColorSpace(policy.toneMappingOutputFormat);
    policy.actualSceneColorFormat = backBufferFormat;

    if (backBufferFormat == RHIFormat::Unknown)
    {
        policy.hdrFallbackReason = "back buffer format is unavailable";
        return policy;
    }

    if (!postProcessActive)
    {
        policy.hdrFallbackReason = "post-process stack has no supported enabled effects";
        return policy;
    }

    if (!SupportsHDRSceneColor())
    {
        policy.hdrFallbackReason = "current RHI backend does not advertise HDR scene color support";
        return policy;
    }

    policy.actualSceneColorFormat = policy.requestedSceneColorFormat;
    policy.hdrSceneColorEnabled = true;
    policy.hdrFallbackReason.clear();
    return policy;
}

void SceneRenderer::Initialize(
    RenderContext* renderContext,
    RenderResourceRegistry* resourceRegistry,
    RenderRetirementQueue* retirementQueue)
{
    if (m_initialized)
    {
        RVX_CORE_WARN("SceneRenderer already initialized");
        return;
    }

    if (!renderContext || !resourceRegistry || !retirementQueue)
    {
        RVX_CORE_ERROR("SceneRenderer: Invalid render-owned dependencies");
        return;
    }
    if (!m_shaderProgramProvider)
    {
        RVX_CORE_ERROR(
            "SceneRenderer: Shader program provider was not injected");
        return;
    }

    m_renderContext = renderContext;
    m_renderResourceRegistry = resourceRegistry;
    m_retirementQueue = retirementQueue;
    if (m_defaultRenderComposition == nullptr)
    {
        m_defaultRenderComposition =
            std::make_unique<DefaultRenderComposition>();
    }
    m_rayTracingSceneManager = std::make_unique<RayTracingSceneManager>();
    m_rayTracingSceneManager->Initialize(m_renderContext->GetDevice());
    m_rayTracingSceneStats = m_rayTracingSceneManager->GetStats();

    // Create render graph
    m_renderGraph = std::make_unique<RenderGraph>();
    m_gpuSceneUploader = std::make_unique<GPUSceneUploader>();
    if (!m_gpuSceneUploader->Initialize(
            m_renderContext->GetDevice(),
            RenderContextInternalAccess::GetSubmissionTracker(*m_renderContext)))
    {
        // GPU-scene upload is diagnostics/future Tier 2 preparation only. A
        // failure here must never make the authoritative RenderScene fail.
        RVX_RENDER_WARN("SceneRenderer: GPUSceneUploader unavailable");
    }

    // Candidate extraction is shared, but Depth and Opaque own independent
    // instance/group/indirect/count streams. This keeps their policy and
    // failure domains independent until a later GPU-scene compaction stage.
    GPUCullingConfig gpuCullingConfig;
    gpuCullingConfig.enableOcclusionCulling = false;
    gpuCullingConfig.enableDistanceCulling = false;
    const FrameSynchronizer* frameSynchronizer =
        m_renderContext->GetFrameSynchronizer();
    const uint32 gpuCullingFrameSlotCount = frameSynchronizer != nullptr
        ? frameSynchronizer->GetFrameCount()
        : 1u;
    m_depthGPUCulling = std::make_unique<GPUCulling>(
        m_shaderProgramProvider.get());
    m_depthGPUCulling->Initialize(
        m_renderContext->GetDevice(), gpuCullingConfig,
        gpuCullingFrameSlotCount);
    m_opaqueGPUCulling = std::make_unique<GPUCulling>(
        m_shaderProgramProvider.get());
    m_opaqueGPUCulling->Initialize(
        m_renderContext->GetDevice(), gpuCullingConfig,
        gpuCullingFrameSlotCount);

    // Create pipeline cache with shader reflection
    m_pipelineCache = std::make_unique<PipelineCache>(
        *m_shaderProgramProvider);

    // Create material system after pipeline layouts are available.
    m_materialSystem = std::make_unique<MaterialSystem>();
    m_lightManager = std::make_unique<LightManager>();
    m_lightManager->Initialize(renderContext->GetDevice());
    m_clusteredLighting = std::make_unique<ClusteredLighting>();
    if (!m_clusteredLighting->Initialize(renderContext->GetDevice()))
    {
        m_clusteredLightingStats.fallbackReason = m_clusteredLighting->GetLastError();
        RVX_CORE_WARN("SceneRenderer: ClusteredLighting unavailable: {}",
                      m_clusteredLightingStats.fallbackReason);
    }

    // Create transient resource pool for RenderGraph
    m_transientResourcePool = std::make_unique<TransientResourcePool>();
    RenderSubmissionTracker* submissionTracker =
        RenderContextInternalAccess::GetSubmissionTracker(*m_renderContext);
    m_transientResourcePool->Initialize(m_renderContext->GetDevice(),
                                        submissionTracker,
                                        m_retirementQueue,
                                        gpuCullingFrameSlotCount);

    // Create resource view cache for automatic view management
    m_resourceViewCache = std::make_unique<ResourceViewCache>();
    m_resourceViewCache->Initialize(m_renderContext->GetDevice(),
                                    submissionTracker,
                                    m_retirementQueue);

    // Get render target format from swap chain
    RHIFormat rtFormat = RHIFormat::BGRA8_UNORM;
    if (m_renderContext->GetSwapChain())
    {
        rtFormat = m_renderContext->GetSwapChain()->GetFormat();
        RVX_CORE_INFO("SceneRenderer: Render target format from swap chain: {}", static_cast<int>(rtFormat));
    }
    else
    {
        RVX_CORE_WARN("SceneRenderer: Swap chain not ready, using default render target format: {}", static_cast<int>(rtFormat));
    }
    m_sceneColorFormatPolicy = ResolveSceneColorFormatPolicy(rtFormat, true);
    m_pipelineCache->SetRenderTargetFormats(m_sceneColorFormatPolicy.actualSceneColorFormat,
                                            m_sceneColorFormatPolicy.actualSceneColorFormat,
                                            m_sceneColorFormatPolicy.toneMappingOutputFormat);
    RVX_CORE_INFO("SceneRenderer: Pipeline formats scene={}, postProcessIntermediate={}, toneMappingOutput={}",
                  static_cast<int>(m_sceneColorFormatPolicy.actualSceneColorFormat),
                  static_cast<int>(m_sceneColorFormatPolicy.actualSceneColorFormat),
                  static_cast<int>(m_sceneColorFormatPolicy.toneMappingOutputFormat));
    if (!m_sceneColorFormatPolicy.hdrSceneColorEnabled)
    {
        RVX_CORE_WARN("SceneRenderer: HDR scene color fallback: {}",
                      m_sceneColorFormatPolicy.hdrFallbackReason);
    }

    // Initialize pipeline cache
    RVX_CORE_INFO("SceneRenderer: Initializing PipelineCache...");
    // Render owns logical shader asset IDs only.  Development and shipping
    // providers resolve those IDs to source or cooked artifacts respectively.
    if (m_pipelineCache->Initialize(m_renderContext->GetDevice(), "."))
    {
        m_pipelineCache->SetObjectConstantSubmissionTracker(
            RenderContextInternalAccess::GetSubmissionTracker(*m_renderContext));
        RVX_CORE_INFO("SceneRenderer: PipelineCache initialized successfully!");
        RVX_CORE_INFO("  OpaquePipeline: {}", m_pipelineCache->GetOpaquePipeline() ? "created" : "null");
    }
    else
    {
        RVX_CORE_ERROR("SceneRenderer: PipelineCache failed to initialize - rendering will be limited");
    }

    if (m_pipelineCache && m_pipelineCache->IsInitialized() && m_materialSystem)
    {
        if (!m_materialSystem->Initialize(
                m_renderContext->GetDevice(),
                m_pipelineCache->GetMaterialSetLayout(),
                m_renderResourceRegistry,
                m_resourceViewCache.get()))
        {
            RVX_CORE_ERROR("SceneRenderer: MaterialSystem failed to initialize");
        }
        else
        {
            m_materialSystem->SetMaterialConstantSubmissionTracker(
                RenderContextInternalAccess::GetSubmissionTracker(*m_renderContext));
        }
    }

    // Seal the compatibility ForwardStandard composition before any frame.
    if (!SetupDefaultRenderFeatures())
    {
        // A failed registration attempt must not leave an "installed" marker
        // paired with a cleared or partially composed pass set. Tear down the
        // entire private composition so a later Initialize retry starts from
        // a coherent ownership boundary.
        if (m_defaultRenderComposition != nullptr)
        {
            m_defaultRenderComposition->Shutdown();
            m_defaultRenderComposition.reset();
        }
        return;
    }
    SetupDefaultPostProcess();

    m_initialized = true;
    RVX_CORE_DEBUG("SceneRenderer initialized");
}

void SceneRenderer::Shutdown(bool deviceLost)
{
    InvalidateRenderFramePlan();
    // Do not release timing query/readback resources while their submission is
    // still completion-owned.  The timing owner is responsible for retaining
    // a terminal sample; a shutdown path must never turn an in-flight sample
    // into fabricated "unavailable" evidence.
    const RenderGraphPassTimingState timingState =
        m_qualificationPassTiming->GetDiagnostics().state;
    if (timingState == RenderGraphPassTimingState::Unavailable ||
        timingState == RenderGraphPassTimingState::Idle ||
        timingState == RenderGraphPassTimingState::Completed ||
        timingState == RenderGraphPassTimingState::Rejected)
    {
        m_qualificationPassTiming->Shutdown(false);
    }
    if (deviceLost || !m_qualityPerformanceSession.HasPendingCompletion())
        m_qualityPerformanceSession.Shutdown(deviceLost);
    AbandonAcceptedFrameHistory();
    if (GetPostProcessStack())
    {
        GetPostProcessStack()->GetExposureController()
            .CancelQualificationProbe();
    }
    m_exposureProbeCompletion.reset();
    m_exposureProbeHistoryIdentity.reset();
    m_exposureProbeSourceExecutionReceipt.reset();
    m_exposureProbePresented = false;
    if (FindCompositionFeature<AmbientOcclusionFeatureAdapter>() != nullptr)
    {
        FindCompositionFeature<AmbientOcclusionFeatureAdapter>()->AbandonQualificationProbe();
    }
    m_ambientOcclusionProbeHistoryIdentity.reset();
    m_ambientOcclusionProbeSourceExecutionReceipt.reset();
    m_ambientOcclusionProbeSubmissionAccepted = false;
    m_ambientOcclusionProbePresented = false;
    m_ambientOcclusionProbeAbandoned = false;
    if (FindPostProcessEffect<ScreenSpaceOutlinePass>() != nullptr)
    {
        FindPostProcessEffect<ScreenSpaceOutlinePass>()->AbandonQualificationProbe();
    }
    m_toonOutlineProbeSourceExecutionReceipt.reset();
    m_toonOutlineProbeSubmissionAccepted = false;
    m_toonOutlineProbePresented = false;
    m_toonOutlineProbeAbandoned = false;
    if (FindCompositionFeature<ReflectionFeatureAdapter>() != nullptr)
    {
        FindCompositionFeature<ReflectionFeatureAdapter>()->AbandonSSRQualificationProbe();
    }
    m_reflectionProbeSourceExecutionReceipt.reset();
    m_reflectionProbeSourceDiagnostics.reset();
    m_hiZReflectionProbeBinding = {};
    m_reflectionProbeSubmissionAccepted = false;
    m_reflectionProbePresented = false;
    m_reflectionProbeAbandoned = false;
    if (m_viewHistoryCoordinator)
    {
        m_viewHistoryCoordinator->Clear();
    }
    m_environmentIBLRequested = false;
    m_lightingFramePlan.reset();
    m_lightingFramePlanFailureReason.clear();
    if (m_gpuSceneUploader)
    {
        m_gpuSceneUploader->ReleaseUnsubmittedFrame();
        m_gpuSceneUploader->Shutdown();
        m_gpuSceneUploader.reset();
    }
    if (m_gpuSceneUpdate)
    {
        m_gpuSceneUpdate->Clear();
    }
    if (m_submissionBatch)
    {
        RVX_ASSERT_MSG(m_retirementQueue != nullptr,
                       "Pending submission ownership requires retirement queue");
        m_submissionBatch->ReleaseUnsubmitted(*m_retirementQueue);
        m_submissionBatch.reset();
    }

    // The composition owns both the registry adapter and post-process stack,
    // while the adapter still borrows the stack. Sever that internal binding
    // before the composition tears both sides down.
    if (FindCompositionFeature<PostProcessPipelineFeatureAdapter>() != nullptr)
    {
        FindCompositionFeature<PostProcessPipelineFeatureAdapter>()->SetStack(nullptr);
    }
    if (FindCompositionFeature<AmbientOcclusionFeatureAdapter>() != nullptr)
    {
        FindCompositionFeature<AmbientOcclusionFeatureAdapter>()->Shutdown();
    }
    if (FindCompositionFeature<VolumetricLightingFeatureAdapter>() != nullptr)
    {
        FindCompositionFeature<VolumetricLightingFeatureAdapter>()->Shutdown();
    }
    m_activeLocalShadowPass.reset();
    if (m_defaultRenderComposition != nullptr)
    {
        m_defaultRenderComposition->Shutdown();
        m_defaultRenderComposition.reset();
    }
    m_pendingRenderExecutionReceipt.reset();
    m_pendingReflectionDiagnostics.reset();
    m_lastAttemptedRenderExecutionReceipt = {};
    m_completedRenderExecutionReceipt = {};
    m_completedReflectionDiagnostics = {};
    m_completedHiZReflectionQualificationReceipt = {};
    m_completedHiZReflectionQualificationDiagnostics = {};
    m_nextHiZReflectionProbeGeneration = 0;
    m_presentedTemporalPresentationState.reset();
    m_activeRenderDeformationResources.reset();
    m_featureCompositionFailureReason.clear();
    if (m_materialSystem)
    {
        m_materialSystem->Shutdown();
        m_materialSystem.reset();
    }
    if (m_lightManager)
    {
        m_lightManager->Shutdown();
        m_lightManager.reset();
    }
    if (m_clusteredLighting)
    {
        m_clusteredLighting->Shutdown();
        m_clusteredLighting.reset();
    }

    if (m_pipelineCache)
    {
        m_pipelineCache->Shutdown();
        m_pipelineCache.reset();
    }

    if (m_transientResourcePool)
    {
        m_transientResourcePool->Shutdown();
        m_transientResourcePool.reset();
    }

    if (m_resourceViewCache)
    {
        m_resourceViewCache->Shutdown();
        m_resourceViewCache.reset();
    }

    if (m_rayTracingSceneManager)
    {
        if (m_rayTracingSceneRecordingToken.IsValid())
        {
            m_rayTracingSceneManager->ReleaseUnsubmitted(
                m_rayTracingSceneRecordingToken);
        }
        m_rayTracingSceneManager->Shutdown();
        m_rayTracingSceneManager.reset();
    }
    m_rayTracingSceneRecordingToken = {};
    m_hybridRayTracingFramePlan.reset();
    m_pendingHybridRayTracingObservation = {};
    m_hybridRayTracingExecutionReceipt.reset();

    if (m_depthGPUCulling)
    {
        m_depthGPUCulling->Shutdown();
        m_depthGPUCulling.reset();
    }
    if (m_opaqueGPUCulling)
    {
        m_opaqueGPUCulling->Shutdown();
        m_opaqueGPUCulling.reset();
    }

    m_renderGraph.reset();
    // The depth view owns the backend view allocation and the texture owns
    // the backing image allocation.  Release them in that order while the
    // RenderContext still retains its device; Vulkan's allocator must not see
    // either allocation survive device teardown.
    m_viewData.depthTarget = {};
    m_depthGraphHandle = {};
    m_depthAccessSnapshot = MakeRHITextureAccessSnapshot(
        RHIResourceState::Undefined,
        RHIShaderStage::None,
        GPUQueueDomain::Graphics,
        RHIContentValidity::Invalid);
    m_depthTextureView.Reset();
    m_depthTexture.Reset();
    m_depthWidth = 0;
    m_depthHeight = 0;
    m_rayTracingSceneStats = {};
    m_rayTracingFrameBudgetStats = {};
    m_completedPresentedMaterialBindingsAvailable = false;
    m_completedPresentedMaterialBindingsOverflow = false;
    m_completedPresentedMaterialBindings.clear();
    m_completedPresentedSkinningPalettesAvailable = false;
    m_completedPresentedSkinningPalettesOverflow = false;
    m_completedPresentedSkinningPalettes.clear();
    m_pendingTemporalHistoryReset = false;
    m_previousObjectWorldMatrices.clear();
    m_preGraphPrepareCallbacks.clear();
    m_renderResourceRegistry = nullptr;
    m_retirementQueue = nullptr;
    m_renderContext = nullptr;
    m_initialized = false;

    if (Log::GetCoreLogger())
    {
        RVX_CORE_DEBUG("SceneRenderer shutdown");
    }
}

void SceneRenderer::RequestTemporalHistoryReset()
{
    m_pendingTemporalHistoryReset = true;
    m_presentedTemporalPresentationState.reset();
    if (FindCompositionFeature<AmbientOcclusionFeatureAdapter>() != nullptr)
    {
        FindCompositionFeature<AmbientOcclusionFeatureAdapter>()->ResetHistory();
    }
    if (FindCompositionFeature<VolumetricLightingFeatureAdapter>() != nullptr)
    {
        FindCompositionFeature<VolumetricLightingFeatureAdapter>()->ResetHistory();
    }
    if (m_viewHistoryCoordinator)
    {
        m_viewHistoryCoordinator->RequestReset(
            RenderHistoryResetReason::ExplicitDiscontinuity);
    }
}

void SceneRenderer::EvaluateAcceptedFrameHistory(
    const RenderFramePacketV5& frame,
    RenderFrameApplyResult& result) noexcept
{
    // A newer accepted packet supersedes an unpresented candidate. The old
    // identity must never become reusable merely because a later candidate
    // happened to reach presentation.
    if (m_pendingRenderExecutionReceipt.has_value())
    {
        RenderExecutionReceipt superseded =
            *m_pendingRenderExecutionReceipt;
        superseded.presented = false;
        superseded.succeeded = false;
        superseded.diagnostic =
            "Render execution receipt was superseded before presentation";
        m_lastAttemptedRenderExecutionReceipt = std::move(superseded);
        m_pendingRenderExecutionReceipt.reset();
    }
    AbandonAcceptedFrameHistory();

    if (!m_viewHistoryCoordinator)
    {
        // Construction creates the coordinator, but keep this boundary
        // fail-closed if a future lifecycle path changes that invariant.
        result.temporalHistoryReset = true;
        return;
    }

    // The retained RenderScene advances this state only from
    // MarkAcceptedFramePresented(), after RenderSubsystem has presented the
    // submitted frame. It is therefore the sole source for the legacy camera
    // reprojection input. This deliberately says nothing about TAA, shadow,
    // reflection, or any other effect-owned temporal resource.
    const bool committedCameraProjectionAvailable =
        m_renderScene.GetLastRenderedFrameSequence() != 0;
    const RenderHistoryAvailability availability =
        m_viewHistoryCoordinator->Evaluate(
            frame, committedCameraProjectionAvailable);

    result.temporalHistoryReset = result.temporalHistoryReset ||
        m_pendingTemporalHistoryReset || !availability.available;
    if (availability.identity.IsValid())
    {
        m_activeViewHistoryCandidate = availability.identity;
        m_activeViewHistoryAvailability = availability;
    }
}

void SceneRenderer::AbandonAcceptedFrameHistory() noexcept
{
    if (FindCompositionFeature<VolumetricLightingFeatureAdapter>())
    {
        FindCompositionFeature<VolumetricLightingFeatureAdapter>()->AbandonHistoryCandidate();
    }
    if (FindCompositionFeature<AmbientOcclusionFeatureAdapter>())
    {
        if (m_ambientOcclusionProbeSubmissionAccepted &&
            !m_ambientOcclusionProbePresented)
        {
            FindCompositionFeature<AmbientOcclusionFeatureAdapter>()->AbandonQualificationProbe();
            m_ambientOcclusionProbeAbandoned = true;
        }
        FindCompositionFeature<AmbientOcclusionFeatureAdapter>()->AbandonHistoryCandidate();
    }
    if (FindPostProcessEffect<ScreenSpaceOutlinePass>() != nullptr)
    {
        if (m_toonOutlineProbeSubmissionAccepted &&
            !m_toonOutlineProbePresented)
        {
            FindPostProcessEffect<ScreenSpaceOutlinePass>()->AbandonQualificationProbe();
            m_toonOutlineProbeAbandoned = true;
        }
    }
    const bool reflectionProbeBoundToAcceptedFrame =
        FindCompositionFeature<ReflectionFeatureAdapter>() != nullptr &&
        FindCompositionFeature<ReflectionFeatureAdapter>()->GetPendingSSRQualificationProbeIdentity()
            .has_value();
    if (FindCompositionFeature<ReflectionFeatureAdapter>() != nullptr &&
        !m_reflectionProbePresented &&
        (m_reflectionProbeSubmissionAccepted ||
         reflectionProbeBoundToAcceptedFrame))
    {
        FindCompositionFeature<ReflectionFeatureAdapter>()->AbandonSSRQualificationProbe();
        m_reflectionProbeSourceExecutionReceipt.reset();
        m_reflectionProbeSourceDiagnostics.reset();
        m_hiZReflectionProbeBinding = {};
        m_reflectionProbeSubmissionAccepted = false;
        m_reflectionProbePresented = false;
        m_reflectionProbeAbandoned = true;
    }
    if (FindCompositionFeature<PostProcessPipelineFeatureAdapter>())
    {
        FindCompositionFeature<PostProcessPipelineFeatureAdapter>()->AbandonTemporalHistoryCandidate();
    }
    if (FindCompositionPass<RayTracedReflectionPass>() != nullptr &&
        m_activeViewHistoryCandidate.has_value())
    {
        FindCompositionPass<RayTracedReflectionPass>()->AbandonSubmittedHistory(
            *m_activeViewHistoryCandidate);
    }
    if (m_activeViewHistoryCandidate.has_value() &&
        m_viewHistoryCoordinator)
    {
        m_viewHistoryCoordinator->Abandon(*m_activeViewHistoryCandidate);
    }
    m_activeViewHistoryCandidate.reset();
    m_activeViewHistoryAvailability.reset();
    m_pendingTemporalPresentationState.reset();
}

void SceneRenderer::PrepareForSwapChainResize()
{
    InvalidateRenderFramePlan();
    if (!m_initialized)
        return;

    if (m_pipelineCache)
    {
        m_pipelineCache->ResetFrameResourceBindings();
    }

    if (m_renderGraph)
    {
        m_renderGraph.reset();
    }

    if (m_resourceViewCache)
    {
        m_resourceViewCache->Clear();
    }

    m_depthTextureView.Reset();
    m_depthTexture.Reset();
    m_depthWidth = 0;
    m_depthHeight = 0;
    m_depthAccessSnapshot = MakeRHITextureAccessSnapshot(
        RHIResourceState::Undefined,
        RHIShaderStage::None,
        GPUQueueDomain::Graphics,
        RHIContentValidity::Invalid);

    m_backBufferAccessSnapshots.clear();
    m_depthGraphHandle = {};
    m_backBufferGraphHandle = {};
    m_activeBackBufferIndex = RVX_INVALID_INDEX;
    m_lastSwapChainWidth = 0;
    m_lastSwapChainHeight = 0;
    m_viewData.colorTarget = {};
    m_viewData.depthTarget = {};
    m_viewData.velocityTarget = {};
    m_viewData.reactiveMaskTarget = {};

    RequestTemporalHistoryReset();
}

void SceneRenderer::SetExternalRenderTarget(const SceneRendererExternalTargetDesc& desc)
{
    const bool targetChanged =
        m_externalRenderTarget.colorTarget != desc.colorTarget ||
        m_externalRenderTarget.depthTarget != desc.depthTarget ||
        m_externalRenderTarget.colorInitialState != desc.colorInitialState ||
        m_externalRenderTarget.colorFinalState != desc.colorFinalState ||
        m_externalRenderTarget.depthInitialState != desc.depthInitialState ||
        m_externalRenderTarget.depthFinalState != desc.depthFinalState ||
        (m_externalRenderTargetStats.width != 0 &&
         desc.colorTarget &&
         (m_externalRenderTargetStats.width != desc.colorTarget->GetWidth() ||
          m_externalRenderTargetStats.height != desc.colorTarget->GetHeight() ||
          m_externalRenderTargetStats.colorFormat != desc.colorTarget->GetFormat())) ||
        (m_externalRenderTargetStats.depthFormat != RHIFormat::Unknown &&
         desc.depthTarget &&
         m_externalRenderTargetStats.depthFormat != desc.depthTarget->GetFormat());

    m_externalRenderTarget = desc;
    if (targetChanged)
    {
        InvalidateRenderFramePlan();
        RequestTemporalHistoryReset();
    }
}

void SceneRenderer::ClearExternalRenderTarget()
{
    InvalidateRenderFramePlan();
    m_externalRenderTarget = {};
    m_externalRenderTargetStats = {};
    RequestTemporalHistoryReset();
}

void SceneRenderer::SetGPUDrivenCullingMode(RenderGPUDrivenMode mode)
{
    m_gpuDrivenCullingMode = mode;
    InvalidateRenderFramePlan();
    m_gpuDrivenCullingEnabled = false;
}

void SceneRenderer::SetGPUDrivenCullingEnabled(bool enabled)
{
    SetGPUDrivenCullingMode(enabled
                                ? RenderGPUDrivenMode::ForceEnabled
                                : RenderGPUDrivenMode::ForceDisabled);
}

void SceneRenderer::RefreshFrameDiagnostics(bool renderAttempted,
                                            bool rendered,
                                            bool graphBuilt,
                                            bool graphCompiled,
                                            const char* skippedReason)
{
    SceneRendererFrameDiagnostics diagnostics;
    diagnostics.frameCount = ++m_frameDiagnosticsCounter;
    diagnostics.renderAttempted = renderAttempted;
    diagnostics.rendered = rendered;
    diagnostics.graphBuilt = graphBuilt;
    diagnostics.graphCompiled = graphCompiled;
    diagnostics.graphExecutionSkipped = renderAttempted && !rendered;
    diagnostics.skippedReason = skippedReason ? skippedReason : "";
    diagnostics.attemptedExecutionReceipt =
        m_lastAttemptedRenderExecutionReceipt;
    diagnostics.presentedExecutionReceipt =
        m_completedRenderExecutionReceipt;
    diagnostics.mutationEvidence = m_completedMutationEvidence;
    diagnostics.presentedMaterialBindingsAvailable =
        m_completedPresentedMaterialBindingsAvailable;
    diagnostics.presentedMaterialBindingsOverflow =
        m_completedPresentedMaterialBindingsOverflow;
    diagnostics.presentedMaterialBindings =
        m_completedPresentedMaterialBindings;
    diagnostics.presentedSkinningPalettesAvailable =
        m_completedPresentedSkinningPalettesAvailable;
    diagnostics.presentedSkinningPalettesOverflow =
        m_completedPresentedSkinningPalettesOverflow;
    diagnostics.presentedSkinningPalettes =
        m_completedPresentedSkinningPalettes;

    diagnostics.renderSceneObjectCount = m_renderScene.GetObjectCount();
    diagnostics.renderSceneLightCount = m_renderScene.GetLightCount();
    diagnostics.visibleObjectCount = m_visibleObjectIndices.size();
    diagnostics.opaqueDrawItemCount = m_opaqueDrawItems.size();
    diagnostics.maskedDrawItemCount = m_maskedDrawItems.size();
    diagnostics.transparentDrawItemCount = m_transparentDrawItems.size();
    diagnostics.transparentRejectedNonFiniteDepthCount =
        m_transparentDrawListDiagnostics.rejectedNonFiniteDepthCount;
    diagnostics.transparentOrderValid =
        m_transparentDrawListDiagnostics.orderValid;
    diagnostics.transparentOrderHash =
        m_transparentDrawListDiagnostics.orderHash;
    if (m_activeRenderPassResults != nullptr &&
        m_activeRenderPassResults->identity == m_activeRenderPassIdentity)
    {
        const TransparentPassDrawStats& transparentStats =
            m_activeRenderPassResults->transparentStats;
        diagnostics.transparentCandidateDrawItemCount =
            transparentStats.candidateDrawItemCount;
        diagnostics.transparentPreparedDrawItemCount =
            transparentStats.preparedDrawItemCount;
        diagnostics.transparentExecutedPacketCount =
            transparentStats.executedPacketCount;
        diagnostics.transparentExecutedDrawCount =
            transparentStats.executedDrawCount;
        diagnostics.transparentAlphaBlendDrawCount =
            transparentStats.alphaBlendDrawCount;
        diagnostics.transparentPremultipliedDrawCount =
            transparentStats.premultipliedDrawCount;
        diagnostics.transparentAdditiveDrawCount =
            transparentStats.additiveDrawCount;
        diagnostics.transparentThinTransmissionDrawCount =
            transparentStats.thinTransmissionDrawCount;
        diagnostics.transparentOpaqueSceneColorPyramidReady =
            transparentStats.opaqueSceneColorPyramidReady;
        diagnostics.transparentOpaqueSceneColorPyramidBuilt =
            transparentStats.opaqueSceneColorPyramidBuilt;
        diagnostics.transparentReactiveMaskAttachmentBound =
            transparentStats.reactiveMaskAttachmentBound;
        diagnostics.transparentReactiveMaskWriteDrawCount =
            transparentStats.reactiveMaskWriteDrawCount;
        diagnostics.transparentTranslucencyPriorityBucketCount =
            transparentStats.translucencyPriorityBucketCount;
        diagnostics.transparentTranslucencyFallbackCount =
            transparentStats.translucencyFallbackCount;
        diagnostics.transparentSkippedMaterialBindingCount =
            transparentStats.skippedMaterialBindingCount;
        diagnostics.transparentSkippedResourceCount =
            transparentStats.skippedResourceCount;
        diagnostics.transparentSkippedExecutionDrawCount =
            transparentStats.skippedExecutionDrawCount;
        diagnostics.transparentMaterialBindingCount =
            transparentStats.materialBindingCount;
        diagnostics.transparentMaterialFallbackBindingCount =
            transparentStats.materialFallbackBindingCount;
        diagnostics.transparentMaterialTextureFlags =
            transparentStats.materialTextureFlags;
        diagnostics.transparentMaterialFallbackTextureFlags =
            transparentStats.materialFallbackTextureFlags;
        diagnostics.transparentNoWork = transparentStats.noWork;
        diagnostics.transparentPreflightFailed = transparentStats.preflightFailed;
        diagnostics.transparentExecutionFailed = transparentStats.executionFailed;
    }
    diagnostics.featureExtractionStats = m_featureExtractionStats;
    diagnostics.pointLightCount = m_localLightingStats.pointLightCount;
    diagnostics.spotLightCount = m_localLightingStats.spotLightCount;
    diagnostics.lightConstantsBufferReady = m_localLightingStats.lightConstantsBufferReady;
    diagnostics.pointLightsBufferReady = m_localLightingStats.pointLightsBufferReady;
    diagnostics.spotLightsBufferReady = m_localLightingStats.spotLightsBufferReady;
    if (m_pipelineCache)
    {
        const FrameLightBindingResult& frameLightBinding =
            m_pipelineCache->GetLastFrameLightBindingResult();
        m_localLightingStats.frameLightResourcesBound = frameLightBinding.lightResourcesBound;
        m_localLightingStats.frameLightFallbackReason = frameLightBinding.fallbackReason;
    }
    diagnostics.frameLightResourcesBound = m_localLightingStats.frameLightResourcesBound;
    diagnostics.frameLightFallbackReason = m_localLightingStats.frameLightFallbackReason;
    diagnostics.pointShadowRequestCount = m_localLightingStats.pointShadowRequestCount;
    diagnostics.spotShadowRequestCount = m_localLightingStats.spotShadowRequestCount;
    diagnostics.localShadowRequestCount = m_localLightingStats.localShadowRequestCount;
    diagnostics.localShadowCasterDrawCount =
        m_localLightingStats.localShadowCasterDrawCount;
    diagnostics.localShadowAtlasReady = m_localLightingStats.localShadowAtlasReady;
    diagnostics.localShadowFallbackReason = m_localLightingStats.localShadowFallbackReason;
    diagnostics.localLightingAvailable = m_lightManager != nullptr;
    diagnostics.pointLightRequestedCount =
        m_localLightingStats.pointLightRequestedCount;
    diagnostics.pointLightAdmittedCount =
        m_localLightingStats.pointLightAdmittedCount;
    diagnostics.pointLightCapacity = m_localLightingStats.pointLightCapacity;
    diagnostics.pointLightOverflowCount =
        m_localLightingStats.pointLightOverflowCount;
    diagnostics.spotLightRequestedCount =
        m_localLightingStats.spotLightRequestedCount;
    diagnostics.spotLightAdmittedCount =
        m_localLightingStats.spotLightAdmittedCount;
    diagnostics.spotLightCapacity = m_localLightingStats.spotLightCapacity;
    diagnostics.spotLightOverflowCount =
        m_localLightingStats.spotLightOverflowCount;
    diagnostics.pointShadowSupported =
        m_localLightingStats.pointShadowSupported;
    diagnostics.pointShadowUnsupportedReason =
        m_localLightingStats.pointShadowUnsupportedReason;
    diagnostics.spotShadowSupported =
        m_localLightingStats.spotShadowSupported;
    diagnostics.spotShadowUnsupportedReason =
        m_localLightingStats.spotShadowUnsupportedReason;
    diagnostics.reflection = m_completedReflectionDiagnostics;
    diagnostics.hiZReflectionQualificationReceipt =
        m_completedHiZReflectionQualificationReceipt;
    diagnostics.hiZReflectionQualification =
        m_completedHiZReflectionQualificationDiagnostics;
    diagnostics.clusteredLightingInitialized = m_clusteredLightingStats.initialized;
    diagnostics.clusteredLightingFrameBegun = m_clusteredLightingStats.frameBegun;
    diagnostics.clusteredLightingLightsAssigned = m_clusteredLightingStats.lightsAssigned;
    diagnostics.clusteredLightingGpuBuffersUploaded = m_clusteredLightingStats.gpuBuffersUploaded;
    diagnostics.clusteredLightingClusterAABBBufferReady = m_clusteredLightingStats.clusterAABBBufferReady;
    diagnostics.clusteredLightingClusterBufferReady = m_clusteredLightingStats.clusterBufferReady;
    diagnostics.clusteredLightingLightIndexBufferReady = m_clusteredLightingStats.lightIndexBufferReady;
    diagnostics.clusteredLightingConstantsBufferReady = m_clusteredLightingStats.clusterConstantsBufferReady;
    diagnostics.clusteredLightingClusterCount = m_clusteredLightingStats.clusterCount;
    diagnostics.clusteredLightingLightIndexCount = m_clusteredLightingStats.lightIndexCount;
    diagnostics.clusteredLightingActiveClusters = m_clusteredLightingStats.activeClusters;
    diagnostics.clusteredLightingTotalLightAssignments = m_clusteredLightingStats.totalLightAssignments;
    diagnostics.clusteredLightingMaxLightsInCluster = m_clusteredLightingStats.maxLightsInCluster;
    diagnostics.clusteredLightingAvgLightsPerCluster = m_clusteredLightingStats.avgLightsPerCluster;
    diagnostics.clusteredLightingFallbackReason = m_clusteredLightingStats.fallbackReason;

    diagnostics.registeredPassCount = m_passChainStats.registeredPassCount;
    diagnostics.graphPassCount = m_passChainStats.graphPassCount;
    diagnostics.skippedDisabledPassCount = m_passChainStats.skippedDisabledPassCount;
    diagnostics.skippedUnsupportedPassCount = m_passChainStats.skippedUnsupportedPassCount;
    diagnostics.passStatuses = m_passChainStats.passStatuses;
    const RenderPassStatus* directionalShadowStatus = nullptr;
    for (const RenderPassStatus& status : diagnostics.passStatuses)
    {
        if (status.name == "ShadowPass")
        {
            directionalShadowStatus = &status;
            break;
        }
    }

    diagnostics.directionalShadowRequested =
        directionalShadowStatus != nullptr &&
        directionalShadowStatus->requestedEnabled;
    diagnostics.directionalShadowSupported =
        directionalShadowStatus != nullptr &&
        directionalShadowStatus->supported;

    const bool hasCurrentDirectionalShadowResults =
        m_activeRenderPassResults != nullptr &&
        m_activeRenderPassResults->identity == m_activeRenderPassIdentity;
    diagnostics.directionalShadowAvailable = hasCurrentDirectionalShadowResults;
    if (hasCurrentDirectionalShadowResults)
    {
        const DirectionalShadowRecordOutput& directionalShadowOutput =
            m_activeRenderPassResults->directionalShadowOutput;
        const ShadowPassStats& shadowStats =
            m_activeRenderPassResults->shadowStats;
        diagnostics.directionalShadowRequestedCascadeCount =
            diagnostics.directionalShadowRequested
                ? m_shadowPassConfig.numCascades
                : 0u;
        diagnostics.directionalShadowProducedCascadeCount =
            static_cast<uint32>(
                directionalShadowOutput.cascadeViewProjections.size());
        diagnostics.directionalShadowResolvedCascadeCount =
            shadowStats.resolvedCascadeViewCount;
        diagnostics.directionalShadowMapSize =
            directionalShadowOutput.shadowMapSize;
        diagnostics.directionalShadowCasterCount =
            shadowStats.shadowCasterCount;
        diagnostics.directionalShadowDrawCount = shadowStats.drawCount;
        diagnostics.directionalShadowOutputReady =
            directionalShadowOutput.enabled &&
            directionalShadowOutput.IsCompatibleWith(
                m_activeRenderPassIdentity);
        diagnostics.directionalShadowSamplingEnabled =
            diagnostics.directionalShadowOutputReady &&
            m_activeRenderPassResults->opaqueShadowStats.frameShadowReady;
    }

    if (diagnostics.directionalShadowRequested)
    {
        if (!diagnostics.directionalShadowSupported)
        {
            diagnostics.directionalShadowReason =
                directionalShadowStatus->unsupportedReason.empty()
                    ? "directional shadow pass is unsupported"
                    : directionalShadowStatus->unsupportedReason;
        }
        else if (!diagnostics.directionalShadowAvailable)
        {
            diagnostics.directionalShadowReason =
                "directional shadow record results are unavailable for the active graph";
        }
        else if (!m_primaryDirectionalLight.IsShadowEligible())
        {
            diagnostics.directionalShadowReason =
                "no shadow-eligible primary directional light is available";
        }
        else if (!std::isfinite(m_shadowPassConfig.maxDistance) ||
                 m_shadowPassConfig.maxDistance <=
                     std::max(0.001f, m_viewData.nearPlane))
        {
            diagnostics.directionalShadowReason =
                "directional shadow maximum distance does not cover the near clip plane";
        }
        else if (!diagnostics.directionalShadowOutputReady)
        {
            diagnostics.directionalShadowReason =
                "directional shadow output is not ready for the active graph";
        }
        else if (!diagnostics.directionalShadowSamplingEnabled)
        {
            diagnostics.directionalShadowReason =
                "directional shadow output was not sampled by the opaque pass";
        }
    }
    if (m_renderResourceRegistry)
    {
        diagnostics.gpuResourceStats = m_renderResourceRegistry->GetStats();
    }
    diagnostics.gpuDrivenCullingStats = m_gpuDrivenCullingStats;
    diagnostics.gpuDrivenCullingStats.gpuSceneDepthQualification =
        m_depthGPUCulling
            ? m_depthGPUCulling->GetGPUSceneQualificationDiagnostics()
            : GPUSceneCullingQualificationDiagnostics{};
    diagnostics.gpuDrivenCullingStats.gpuSceneOpaqueQualification =
        m_opaqueGPUCulling
            ? m_opaqueGPUCulling->GetGPUSceneQualificationDiagnostics()
            : GPUSceneCullingQualificationDiagnostics{};
    diagnostics.gpuDrivenCullingStats.directOpaqueRasterReadbackQualification =
        m_directOpaqueRasterReadbackQualification.GetDiagnostics();
    const auto accumulateCullingUploadBytes =
        [&diagnostics](const GPUCulling* culling)
    {
        if (!culling)
            return;
        diagnostics.gpuDrivenCullingStats.instanceUploadBytes +=
            culling->GetLastInstanceUploadBytes();
        diagnostics.gpuDrivenCullingStats.gpuSceneCandidateUploadBytes +=
            culling->GetLastGPUSceneCandidateUploadBytes();
        const GPUCullingIncrementalDiagnostics& incremental =
            culling->GetIncrementalDiagnostics();
        diagnostics.gpuDrivenCullingStats.activeRowUploadBytes +=
            culling->GetLastActiveRowUploadBytes();
        diagnostics.gpuDrivenCullingStats.activeRowCount +=
            incremental.activeRowCount;
        diagnostics.gpuDrivenCullingStats.activeRowHighWatermark = std::max(
            diagnostics.gpuDrivenCullingStats.activeRowHighWatermark,
            incremental.activeRowHighWatermark);
        diagnostics.gpuDrivenCullingStats.instancePatchedRowCount +=
            incremental.instancePatchedRowCount;
        diagnostics.gpuDrivenCullingStats.gpuSceneCandidatePatchedRowCount +=
            incremental.candidatePatchedRowCount;
        diagnostics.gpuDrivenCullingStats.activeRowPatchedRowCount +=
            incremental.activeRowPatchedRowCount;
        diagnostics.gpuDrivenCullingStats.instanceFullMaterializationCount +=
            incremental.instanceFullMaterializationCount;
        diagnostics.gpuDrivenCullingStats.gpuSceneCandidateFullMaterializationCount +=
            incremental.candidateFullMaterializationCount;
        diagnostics.gpuDrivenCullingStats.activeRowFullMaterializationCount +=
            incremental.activeRowFullMaterializationCount;
        diagnostics.gpuDrivenCullingStats.continuityFullMaterializationCount +=
            incremental.continuityFullMaterializationCount;
        diagnostics.gpuDrivenCullingStats.capacityFullMaterializationCount +=
            incremental.capacityFullMaterializationCount;
        AccumulateUploadWork(
            diagnostics.gpuDrivenCullingStats.canonicalInstanceUploadWork,
            incremental.instanceUploadWork);
        AccumulateUploadWork(
            diagnostics.gpuDrivenCullingStats.canonicalCandidateUploadWork,
            incremental.candidateUploadWork);
        AccumulateUploadWork(
            diagnostics.gpuDrivenCullingStats.canonicalActiveRowUploadWork,
            incremental.activeRowUploadWork);
    };
    // Depth and opaque have independent completion-safe input slots. Report
    // the real sum of both CPU copies rather than one logical object count.
    diagnostics.gpuDrivenCullingStats.instanceUploadBytes = 0;
    diagnostics.gpuDrivenCullingStats.gpuSceneCandidateUploadBytes = 0;
    diagnostics.gpuDrivenCullingStats.activeRowUploadBytes = 0;
    diagnostics.gpuDrivenCullingStats.activeRowCount = 0;
    diagnostics.gpuDrivenCullingStats.activeRowHighWatermark = 0;
    diagnostics.gpuDrivenCullingStats.instancePatchedRowCount = 0;
    diagnostics.gpuDrivenCullingStats.gpuSceneCandidatePatchedRowCount = 0;
    diagnostics.gpuDrivenCullingStats.activeRowPatchedRowCount = 0;
    diagnostics.gpuDrivenCullingStats.instanceFullMaterializationCount = 0;
    diagnostics.gpuDrivenCullingStats.gpuSceneCandidateFullMaterializationCount = 0;
    diagnostics.gpuDrivenCullingStats.activeRowFullMaterializationCount = 0;
    diagnostics.gpuDrivenCullingStats.continuityFullMaterializationCount = 0;
    diagnostics.gpuDrivenCullingStats.capacityFullMaterializationCount = 0;
    diagnostics.gpuDrivenCullingStats.canonicalInstanceUploadWork = {};
    diagnostics.gpuDrivenCullingStats.canonicalCandidateUploadWork = {};
    diagnostics.gpuDrivenCullingStats.canonicalActiveRowUploadWork = {};
    accumulateCullingUploadBytes(m_depthGPUCulling.get());
    accumulateCullingUploadBytes(m_opaqueGPUCulling.get());
    const auto accumulateDirectRasterDiagnostics =
        [&diagnostics](uint64 instanceBytes,
                       uint64 indexBytes,
                       uint32 instancePatchedRowCount,
                       uint32 indexPatchedRowCount,
                       uint32 activeInstanceCount,
                       uint32 activeInstanceCapacity,
                       bool instanceFullMaterialization,
                       bool indexFullMaterialization,
                       const RenderUploadWorkDiagnostics& instanceUploadWork,
                       const RenderUploadWorkDiagnostics& indexUploadWork)
    {
        diagnostics.gpuDrivenCullingStats.directRasterInstanceUploadBytes +=
            instanceBytes;
        diagnostics.gpuDrivenCullingStats
            .directRasterInstanceIndexUploadBytes += indexBytes;
        diagnostics.gpuDrivenCullingStats
            .directRasterInstancePatchedRowCount += instancePatchedRowCount;
        diagnostics.gpuDrivenCullingStats
            .directRasterIndexPatchedRowCount += indexPatchedRowCount;
        diagnostics.gpuDrivenCullingStats.directRasterActiveInstanceCount +=
            activeInstanceCount;
        diagnostics.gpuDrivenCullingStats.directRasterActiveInstanceCapacity +=
            activeInstanceCapacity;
        diagnostics.gpuDrivenCullingStats
            .directRasterInstanceFullMaterializationCount +=
            instanceFullMaterialization ? 1U : 0U;
        diagnostics.gpuDrivenCullingStats
            .directRasterIndexFullMaterializationCount +=
            indexFullMaterialization ? 1U : 0U;
        AccumulateUploadWork(
            diagnostics.gpuDrivenCullingStats.directRasterInstanceUploadWork,
            instanceUploadWork);
        AccumulateUploadWork(
            diagnostics.gpuDrivenCullingStats.directRasterIndexUploadWork,
            indexUploadWork);
    };
    diagnostics.gpuDrivenCullingStats.directRasterInstanceUploadBytes = 0;
    diagnostics.gpuDrivenCullingStats.directRasterInstanceIndexUploadBytes = 0;
    diagnostics.gpuDrivenCullingStats.directRasterInstancePatchedRowCount = 0;
    diagnostics.gpuDrivenCullingStats.directRasterIndexPatchedRowCount = 0;
    diagnostics.gpuDrivenCullingStats.directRasterActiveInstanceCount = 0;
    diagnostics.gpuDrivenCullingStats.directRasterActiveInstanceCapacity = 0;
    diagnostics.gpuDrivenCullingStats
        .directRasterInstanceFullMaterializationCount = 0;
    diagnostics.gpuDrivenCullingStats
        .directRasterIndexFullMaterializationCount = 0;
    diagnostics.gpuDrivenCullingStats.directRasterInstanceUploadWork = {};
    diagnostics.gpuDrivenCullingStats.directRasterIndexUploadWork = {};
    // Record results are graph-owned and identity-qualified.  Reading the
    // pass objects directly here could retain a prior completed frame when the
    // current graph fails before a pass publishes its new result.  Only a
    // completed current recording may publish Direct-stream work.
    const RenderPassRecordResults* const completedPassResults =
        rendered && m_submissionQualifiedFrameDiagnostics &&
            m_activeRenderPassResults != nullptr &&
            m_activeRenderPassResults->identity == m_activeRenderPassIdentity
        ? m_activeRenderPassResults.get()
        : nullptr;
    if (completedPassResults != nullptr)
    {
        const DepthPrepassDrawStats& depth = completedPassResults->depthStats;
        accumulateDirectRasterDiagnostics(
            depth.directInstanceUploadBytes,
            depth.directInstanceIndexUploadBytes,
            depth.directInstancePatchedRowCount,
            depth.directInstanceIndexPatchedRowCount,
            depth.directInstanceActiveCount,
            depth.directInstanceActiveCapacity,
            depth.directInstanceFullMaterialization,
            depth.directInstanceIndexFullMaterialization,
            depth.directInstanceUploadWork,
            depth.directInstanceIndexUploadWork);
        const OpaquePassDrawStats& opaque = completedPassResults->opaqueStats;
        accumulateDirectRasterDiagnostics(
            opaque.directInstanceUploadBytes,
            opaque.directInstanceIndexUploadBytes,
            opaque.directInstancePatchedRowCount,
            opaque.directInstanceIndexPatchedRowCount,
            opaque.directInstanceActiveCount,
            opaque.directInstanceActiveCapacity,
            opaque.directInstanceFullMaterialization,
            opaque.directInstanceIndexFullMaterialization,
            opaque.directInstanceUploadWork,
            opaque.directInstanceIndexUploadWork);
        diagnostics.gpuDrivenCullingStats.directOpaqueRasterTranscript =
            opaque.directRasterTranscript;
        const ShadowPassStats& shadow = completedPassResults->shadowStats;
        accumulateDirectRasterDiagnostics(
            shadow.directInstanceUploadBytes,
            shadow.directInstanceIndexUploadBytes,
            shadow.directInstancePatchedRowCount,
            shadow.directInstanceIndexPatchedRowCount,
            shadow.directInstanceActiveCount,
            shadow.directInstanceActiveCapacity,
            shadow.directInstanceFullMaterialization,
            shadow.directInstanceIndexFullMaterialization,
            shadow.directInstanceUploadWork,
            shadow.directInstanceIndexUploadWork);
        bool directTotalsSaturated = false;
        diagnostics.mutationEvidence.directRaster = {};
        AccumulateDirectMutationTotals(
            diagnostics.mutationEvidence.directRaster,
            depth.directMutationTotals,
            directTotalsSaturated);
        AccumulateDirectMutationTotals(
            diagnostics.mutationEvidence.directRaster,
            opaque.directMutationTotals,
            directTotalsSaturated);
        AccumulateDirectMutationTotals(
            diagnostics.mutationEvidence.directRaster,
            shadow.directMutationTotals,
            directTotalsSaturated);
        directTotalsSaturated = directTotalsSaturated ||
            depth.directMutationTotalsSaturated ||
            opaque.directMutationTotalsSaturated ||
            shadow.directMutationTotalsSaturated;
        diagnostics.mutationEvidence.directRasterOwnerCount = 3;
        diagnostics.mutationEvidence.completion.saturated =
            diagnostics.mutationEvidence.completion.saturated ||
            directTotalsSaturated;
    }
    diagnostics.hzbRequested =
        m_renderVisibility.diagnostics.occlusionRequested ||
        diagnostics.gpuDrivenCullingStats.occlusionRequestedButUnavailable;
    diagnostics.hzbSupported = false;
    diagnostics.hzbEnabled = false;
    diagnostics.hzbReason = diagnostics.hzbRequested
        ? "hierarchical depth buffer occlusion culling is not implemented"
        : "";
    diagnostics.instancing = m_instancingDiagnostics;
    diagnostics.policy = m_renderPolicyDiagnostics;
    diagnostics.rayTracingSceneStats = m_rayTracingSceneStats;

    diagnostics.requestedPostProcessEffectCount = m_postProcessStats.stackStats.requestedEffectCount;
    diagnostics.enabledPostProcessEffectCount = m_postProcessStats.stackStats.enabledEffectCount;
    diagnostics.unsupportedPostProcessSkippedCount = m_postProcessStats.stackStats.unsupportedSkippedCount;
    diagnostics.scheduledPostProcessEffectCount = m_postProcessStats.stackStats.scheduledEffectCount;
    diagnostics.postProcessGraphPassCount = m_postProcessStats.stackStats.graphPassCount;
    diagnostics.requestedVisualQualityPreset = m_postProcessStats.stackStats.requestedQualityPreset;
    diagnostics.appliedVisualQualityPreset = m_postProcessStats.stackStats.appliedQualityPreset;
    diagnostics.hdrSceneColorEnabled = m_postProcessStats.hdrSceneColorEnabled;
    diagnostics.toneMappingOutputColorSpace = m_postProcessStats.toneMappingOutputColorSpace;
    diagnostics.postProcessFinalOutputFormat = m_postProcessStats.stackStats.finalOutputFormat;
    diagnostics.hdrFallbackReason = m_postProcessStats.hdrFallbackReason;
    diagnostics.postProcessToneMappingBoundaryValid = m_postProcessStats.stackStats.toneMappingBoundaryValid;
    diagnostics.postProcessFallbackCopyApplied = m_postProcessStats.stackStats.fallbackCopyApplied;
    diagnostics.postProcessFallbackCopyPassCount = m_postProcessStats.stackStats.fallbackCopyPassCount;
    diagnostics.postProcessFallbackCopyReason = m_postProcessStats.stackStats.fallbackCopyReason;
    diagnostics.postProcessDepthInputAvailable = m_postProcessStats.frameInputDepthAvailable;
    diagnostics.postProcessVelocityInputAvailable = m_postProcessStats.frameInputVelocityAvailable;
    diagnostics.postProcessTemporalHistoryAvailable = m_postProcessStats.frameInputTemporalHistoryAvailable;
    diagnostics.postProcessToneMappingBoundaryWarning = m_postProcessStats.stackStats.toneMappingBoundaryWarning;
    diagnostics.postProcessEffectPlans = m_postProcessStats.stackStats.effectPlans;

    diagnostics.externalTargetRequested = m_externalRenderTargetStats.requested;
    diagnostics.externalTargetActive = m_externalRenderTargetStats.active;
    diagnostics.externalColorImported = m_externalRenderTargetStats.importedColor;
    diagnostics.externalDepthImported = m_externalRenderTargetStats.importedDepth;
    diagnostics.externalTargetFallbackReason = m_externalRenderTargetStats.fallbackReason;

    if (graphCompiled && m_renderGraph)
    {
        const RenderGraph::CompileStats& stats = m_renderGraph->GetCompileStats();
        diagnostics.graphCompileValid = stats.compileValid;
        diagnostics.renderGraphTotalPasses = stats.totalPasses;
        diagnostics.renderGraphCulledPasses = stats.culledPasses;
        diagnostics.renderGraphBarrierCount = stats.barrierCount;
        diagnostics.renderGraphTextureBarrierCount = stats.textureBarrierCount;
        diagnostics.renderGraphBufferBarrierCount = stats.bufferBarrierCount;
        diagnostics.renderGraphValidationWarningCount = stats.validationWarningCount;
        diagnostics.renderGraphValidationErrorCount = stats.validationErrorCount;
        diagnostics.renderGraphMemorySavingsPercent = stats.GetMemorySavingsPercent();
        diagnostics.graphDiagnostics = m_renderGraph->GetCompileDiagnostics();

        if (!diagnostics.graphCompileValid && diagnostics.skippedReason.empty())
        {
            diagnostics.skippedReason = "RenderGraph compile reported validation errors";
        }
    }

    diagnostics.featureReport = BuildRenderFeatureReport(diagnostics);

    if (!diagnostics.skippedReason.empty())
    {
        diagnostics.graphExecutionSkipped = true;
    }

    m_frameDiagnostics = std::move(diagnostics);

    m_toolDiagnosticsSnapshot = {};
    m_toolDiagnosticsSnapshot.frameDiagnosticsAvailable = true;
    m_toolDiagnosticsSnapshot.frame = m_frameDiagnostics;
    const IRHIDevice* device = m_renderContext
        ? m_renderContext->GetDevice()
        : m_featureReportDeviceForTesting;
    if (device)
    {
        m_toolDiagnosticsSnapshot.rhiCapabilityReportAvailable = true;
        m_toolDiagnosticsSnapshot.rhiCapabilityReport = device->GetCapabilityReport();
    }
    if (graphBuilt && m_renderGraph)
    {
        m_toolDiagnosticsSnapshot.renderGraphDiagnosticsAvailable = true;
        m_toolDiagnosticsSnapshot.renderGraph = m_renderGraph->GetDiagnostics();
    }
}

void SceneRenderer::RefreshSubmissionQualifiedFrameDiagnostics()
{
    const bool renderAttempted = m_frameDiagnostics.renderAttempted;
    const bool rendered = m_frameDiagnostics.rendered;
    const bool graphBuilt = m_frameDiagnostics.graphBuilt;
    const bool graphCompiled = m_frameDiagnostics.graphCompiled;
    const std::string skippedReason = m_frameDiagnostics.skippedReason;
    const uint64 frameCount = m_frameDiagnostics.frameCount;

    RefreshFrameDiagnostics(renderAttempted,
                            rendered,
                            graphBuilt,
                            graphCompiled,
                            skippedReason.empty() ? nullptr : skippedReason.c_str());

    // This is a refinement of the recorded frame, not a second render attempt.
    m_frameDiagnosticsCounter = frameCount;
    m_frameDiagnostics.frameCount = frameCount;
    if (m_toolDiagnosticsSnapshot.frameDiagnosticsAvailable)
    {
        m_toolDiagnosticsSnapshot.frame = m_frameDiagnostics;
    }
}

void SceneRenderer::UpdateLiveMutationEvidence()
{
    RenderMutationEvidenceDiagnostics current{};
    current.scene = m_renderScene.GetMutationTotals();
    bool saturated = m_renderScene.IsMutationTotalsSaturated();

    if (m_gpuSceneUpdate)
    {
        current.gpuScenePublication = m_gpuSceneUpdate->GetMutationTotals();
        saturated = saturated || m_gpuSceneUpdate->IsMutationTotalsSaturated();
    }
    if (m_gpuSceneUploader)
    {
        current.gpuSceneUpload = m_gpuSceneUploader->GetMutationTotals();
        saturated = saturated || m_gpuSceneUploader->IsMutationTotalsSaturated();
    }

    const auto accumulateCulling = [&current, &saturated](const GPUCulling* culling)
    {
        if (culling == nullptr)
        {
            return;
        }
        ++current.gpuCullingOwnerCount;
        AccumulateGPUCullingMutationTotals(current.gpuCulling,
                                           culling->GetMutationTotals(),
                                           saturated);
        saturated = saturated || culling->IsMutationTotalsSaturated();
    };
    accumulateCulling(m_depthGPUCulling.get());
    accumulateCulling(m_opaqueGPUCulling.get());

    // Direct stream caches are pass-owned. A graph-owned completed result
    // supplies their exact persistent totals for the current recording; a
    // later no-work frame intentionally retains the last observed owner view.
    const RenderMutationEvidenceDiagnostics& frameEvidence =
        m_frameDiagnostics.mutationEvidence;
    if (frameEvidence.directRasterOwnerCount != 0)
    {
        current.directRaster = frameEvidence.directRaster;
        current.directRasterOwnerCount = frameEvidence.directRasterOwnerCount;
        saturated = saturated || frameEvidence.completion.saturated;
    }
    else
    {
        current.directRaster = m_liveMutationEvidence.directRaster;
        current.directRasterOwnerCount =
            m_liveMutationEvidence.directRasterOwnerCount;
    }

    if (MutationEvidenceDecreased(current, m_lastObservedMutationEvidence))
    {
        ++m_mutationEvidenceEpoch;
        if (m_mutationEvidenceEpoch == 0)
        {
            ++m_mutationEvidenceEpoch;
        }
    }
    current.completion.saturated = saturated;
    m_lastObservedMutationEvidence = current;
    m_liveMutationEvidence = current;
}

void SceneRenderer::FreezeMutationEvidenceForPresentedFrame()
{
    UpdateLiveMutationEvidence();
    m_completedMutationEvidence = m_liveMutationEvidence;
    SaturatingAdd(m_completedPresentationCount,
                  1,
                  m_completedPresentationCountSaturated);
    RenderMutationCompletionWatermark& completion =
        m_completedMutationEvidence.completion;
    completion.available = true;
    completion.saturated = completion.saturated ||
                           m_completedPresentationCountSaturated;
    completion.evidenceEpoch = m_mutationEvidenceEpoch;
    completion.completedPresentationCount = m_completedPresentationCount;
    completion.completedFrameSequence =
        m_renderScene.GetLastRenderedFrameSequence();
    completion.requiredSceneRevision =
        m_renderScene.GetAcceptedHeader().requiredSceneRevision;
    completion.appliedSceneRevision =
        m_renderScene.GetRetainedStats().appliedSceneRevision;

    RenderPresentedSceneReceipt presentedScene =
        m_renderScene.GetPendingPresentedSceneReceipt();
    if (presentedScene.IsStructurallyValid() &&
        presentedScene.completedPresentedFrameSequence ==
            completion.completedFrameSequence &&
        presentedScene.requiredSceneRevision ==
            completion.requiredSceneRevision &&
        presentedScene.appliedSceneRevision == completion.appliedSceneRevision &&
        m_renderScene.CommitPendingPresentedSceneReceipt(
            completion.completedFrameSequence))
    {
        presentedScene.available = true;
        m_completedMutationEvidence.presentedScene = presentedScene;
    }
    else
    {
        // Never relabel stale or incomplete accepted-frame evidence as a
        // completed presentation receipt.
        m_completedMutationEvidence.presentedScene = {};
    }

    // The mutation watermark is the only action evidence that may cross the
    // latest-only diagnostics transport. Publishing it after Present prevents
    // unsubmitted, failed, or merely recorded work from replacing it.
    m_frameDiagnostics.mutationEvidence = m_completedMutationEvidence;
    if (m_toolDiagnosticsSnapshot.frameDiagnosticsAvailable)
    {
        m_toolDiagnosticsSnapshot.frame = m_frameDiagnostics;
    }
}

void SceneRenderer::FreezePresentedMaterialBindingsForPresentedFrame()
{
    m_completedPresentedMaterialBindingsAvailable = false;
    m_completedPresentedMaterialBindingsOverflow = false;
    m_completedPresentedMaterialBindings.clear();
    m_completedPresentedSkinningPalettesAvailable = false;
    m_completedPresentedSkinningPalettesOverflow = false;
    m_completedPresentedSkinningPalettes.clear();

    const uint64 presentationSequence =
        m_renderScene.GetLastRenderedFrameSequence();
    const RenderPassRecordResults* const results =
        m_submissionQualifiedFrameDiagnostics &&
            m_frameDiagnostics.rendered &&
            m_frameDiagnostics.graphCompiled &&
            m_activeRenderPassResults != nullptr &&
            m_activeRenderPassResults->identity == m_activeRenderPassIdentity &&
            m_activeRenderPassIdentity.frameSequence == presentationSequence
        ? m_activeRenderPassResults.get()
        : nullptr;
    if (results == nullptr || results->successfulMaterialBindingsOverflow)
    {
        m_completedPresentedMaterialBindingsOverflow =
            results != nullptr && results->successfulMaterialBindingsOverflow;
    }
    else
    {
        m_completedPresentedMaterialBindings.reserve(
            results->successfulMaterialBindings.size());
        bool valid = true;
        for (const RenderRecordedMaterialBindingReceipt& receipt :
             results->successfulMaterialBindings)
        {
            if (!receipt.IsValid())
            {
                m_completedPresentedMaterialBindings.clear();
                valid = false;
                break;
            }
            m_completedPresentedMaterialBindings.push_back(
                {receipt.material,
                 receipt.contentRevision,
                 receipt.descriptorContentKey,
                 receipt.descriptorRevision,
                 receipt.textureEntries,
                 receipt.fallbackUsed,
                 m_activeRenderPassIdentity.frameSequence,
                 presentationSequence,
                 receipt.parameterLayoutHash,
                 receipt.featureMask,
                 receipt.shadingModel,
                 receipt.blendMode,
                 receipt.surfacePathEligibility});
        }
        m_completedPresentedMaterialBindingsAvailable = valid;
    }

    if (results == nullptr || results->successfulSkinningPalettesOverflow)
    {
        m_completedPresentedSkinningPalettesOverflow =
            results != nullptr && results->successfulSkinningPalettesOverflow;
    }
    else
    {
        m_completedPresentedSkinningPalettes.reserve(
            results->successfulSkinningPalettes.size());
        bool valid = true;
        for (const RenderRecordedSkinningPaletteReceipt& receipt :
             results->successfulSkinningPalettes)
        {
            if (!receipt.IsValid())
            {
                m_completedPresentedSkinningPalettes.clear();
                valid = false;
                break;
            }
            m_completedPresentedSkinningPalettes.push_back(
                {receipt.metadata.providerComponentId,
                 receipt.metadata.sourceModelResourceId,
                 receipt.metadata.poseSequence,
                 receipt.metadata.paletteHash,
                 receipt.metadata.paletteCount,
                 receipt.lane,
                 m_activeRenderPassIdentity.frameSequence,
                 presentationSequence});
        }
        m_completedPresentedSkinningPalettesAvailable = valid;
    }

    // This is a post-Present refinement.  No recorded or merely submitted
    // material binding may replace the prior completion-qualified receipt.
    m_frameDiagnostics.presentedMaterialBindingsAvailable =
        m_completedPresentedMaterialBindingsAvailable;
    m_frameDiagnostics.presentedMaterialBindingsOverflow =
        m_completedPresentedMaterialBindingsOverflow;
    m_frameDiagnostics.presentedMaterialBindings =
        m_completedPresentedMaterialBindings;
    m_frameDiagnostics.presentedSkinningPalettesAvailable =
        m_completedPresentedSkinningPalettesAvailable;
    m_frameDiagnostics.presentedSkinningPalettesOverflow =
        m_completedPresentedSkinningPalettesOverflow;
    m_frameDiagnostics.presentedSkinningPalettes =
        m_completedPresentedSkinningPalettes;
    if (m_toolDiagnosticsSnapshot.frameDiagnosticsAvailable)
    {
        m_toolDiagnosticsSnapshot.frame = m_frameDiagnostics;
    }
}

void SceneRenderer::PrepareFrameApplyResources(
    RenderResourceRegistry& registry)
{
    InvalidateRenderFramePlan();
    // A rejected apply must not expose visibility or preparation telemetry
    // from the previously accepted frame as if it belonged to this attempt.
    ClearPreparedDrawState();
    m_renderResourceRegistry = &registry;
    if (FindCompositionPass<DepthPrepass>())
    {
        FindCompositionPass<DepthPrepass>()->SetResourceRegistry(&registry);
    }
    if (FindCompositionPass<ShadowPass>())
    {
        FindCompositionPass<ShadowPass>()->SetResourceRegistry(&registry);
    }
    if (FindCompositionPass<ObjectVelocityPass>())
    {
        FindCompositionPass<ObjectVelocityPass>()->SetResourceRegistry(&registry);
    }
    if (FindCompositionPass<OpaquePass>())
    {
        FindCompositionPass<OpaquePass>()->SetResourceRegistry(&registry);
    }
    if (FindCompositionPass<TransparentPass>())
    {
        FindCompositionPass<TransparentPass>()->SetResourceRegistry(&registry);
    }
    if (FindCompositionPass<ParticleFeaturePass>())
    {
        FindCompositionPass<ParticleFeaturePass>()->SetResourceRegistry(&registry);
    }
    if (FindCompositionPass<SkyboxPass>())
    {
        FindCompositionPass<SkyboxPass>()->SetResourceRegistry(&registry);
    }
    if (FindCompositionPass<RayTracedShadowPass>())
    {
        FindCompositionPass<RayTracedShadowPass>()->SetResourceRegistry(&registry);
    }
    if (FindCompositionPass<RayTracedReflectionPass>())
    {
        FindCompositionPass<RayTracedReflectionPass>()->SetResourceRegistry(&registry);
    }
}

RenderFrameApplyResult SceneRenderer::ApplyFrameV5(
    const RenderFramePacketV5& frame,
    const RenderSceneDatabase& scene,
    RenderResourceRegistry& registry)
{
    PrepareFrameApplyResources(registry);
    const std::optional<RenderEnvironmentSnapshot> environment =
        scene.GetEnvironment();
    const bool environmentIBLRequested = environment.has_value() &&
        (environment->irradianceTexture.IsValid() ||
         environment->prefilteredTexture.IsValid() ||
         environment->brdfLutTexture.IsValid());
    RenderFrameApplyResult result =
        m_renderScene.ApplyFrameV5(frame, scene, registry);
    if (result.IsApplied())
    {
        // RenderScene intentionally clears incomplete handles before they can
        // reach RHI binding. Preserve the accepted scene's request separately
        // so diagnostics can still distinguish it from no IBL request.
        m_environmentIBLRequested = environmentIBLRequested;
        m_reflectionProbeSnapshots.clear();
        m_reflectionProbeSnapshots.reserve(scene.GetProbes().size());
        for (const auto& [probeId, probe] : scene.GetProbes())
        {
            static_cast<void>(probeId);
            m_reflectionProbeSnapshots.push_back(probe);
        }
        std::sort(
            m_reflectionProbeSnapshots.begin(),
            m_reflectionProbeSnapshots.end(),
            [](const RenderProbeSnapshot& left,
               const RenderProbeSnapshot& right)
            {
                return left.probeId < right.probeId;
            });
        uint64 probeStateHash = 0xCBF29CE484222325ULL;
        probeStateHash = MixGPUCullingQualificationIdentity(
            probeStateHash, scene.GetRevision());
        probeStateHash = MixGPUCullingQualificationIdentity(
            probeStateHash, m_reflectionProbeSnapshots.size());
        for (const RenderProbeSnapshot& probe : m_reflectionProbeSnapshots)
        {
            probeStateHash = MixGPUCullingQualificationIdentity(
                probeStateHash, probe.probeId);
            probeStateHash = MixGPUCullingQualificationIdentity(
                probeStateHash,
                (static_cast<uint64>(probe.texture.slot) << 32U) |
                    probe.texture.generation);
        }
        m_reflectionProbeStateHash = probeStateHash == 0
            ? 1 : probeStateHash;
    }
    return FinalizeFrameApply(frame, std::move(result), registry);
}

RenderFrameApplyResult SceneRenderer::FinalizeFrameApply(
    const RenderFramePacketV5& frame,
    RenderFrameApplyResult result,
    RenderResourceRegistry& registry)
{
    if (!result.IsApplied())
    {
        return result;
    }

    EvaluateAcceptedFrameHistory(frame, result);

    // RenderScene is authoritative. A failed GPUScene publication records
    // diagnostics and leaves the previously committed resident generation intact.
    if (m_gpuSceneUpdate)
    {
        try
        {
            const uint64 previousCommittedVersion =
                m_gpuSceneUpdate->GetStats().committedVersion;
            bool publicationAttempted = false;
            GPUScenePublicationStats publication;
            if (m_forceFullGPUScenePublication ||
                m_renderScene.IsFullGPUSceneMutation())
            {
                publication = m_gpuSceneUpdate->Publish(
                    m_renderScene, registry);
                publicationAttempted = true;
            }
            else if (!m_renderScene.GetGPUSceneChangedObjectIds().empty() ||
                     !m_renderScene.GetGPUSceneRemovedObjectIds().empty())
            {
                publication = m_gpuSceneUpdate->PublishIncremental(
                    m_renderScene,
                    m_renderScene.GetGPUSceneChangedObjectIds(),
                    m_renderScene.GetGPUSceneRemovedObjectIds(),
                    registry);
                publicationAttempted = true;
            }
            else if (m_gpuSceneUpdate->GetStats().committedSourceSequence !=
                     m_renderScene.GetAcceptedHeader().sequence)
            {
                // Persistent scene content is unchanged, but execution policy
                // still needs an exact accepted-frame identity. The empty
                // incremental transaction advances only that identity: the
                // database version and GPU upload ranges remain unchanged.
                publication = m_gpuSceneUpdate->PublishIncremental(
                    m_renderScene, {}, {}, registry);
                publicationAttempted = true;
            }
            if (publicationAttempted)
            {
                if (publication.committedVersion != previousCommittedVersion)
                    SynchronizeGPUSceneUploader();
                m_forceFullGPUScenePublication =
                    publication.failureReason ==
                        GPUScenePublicationFailureReason::AllocationFailed ||
                    publication.failureReason ==
                        GPUScenePublicationFailureReason::DatabaseCommitFailed ||
                    publication.failureReason ==
                        GPUScenePublicationFailureReason::UnexpectedFailure;
            }
            m_lastGPUSceneResourceContentRevision =
                registry.GetContentRevision();
        }
        catch (const std::bad_alloc&)
        {
            m_forceFullGPUScenePublication = true;
            m_gpuSceneUpdate->RecordFailure(
                m_renderScene.GetAcceptedHeader().sequence,
                GPUScenePublicationFailureReason::AllocationFailed);
        }
        catch (...)
        {
            m_forceFullGPUScenePublication = true;
            m_gpuSceneUpdate->RecordUnexpectedFailure(
                m_renderScene.GetAcceptedHeader().sequence);
        }
    }

    const bool previousViewValid =
        m_renderScene.GetLastRenderedFrameSequence() != 0;
    const Mat4& previousViewProjection = previousViewValid
                                             ? m_renderScene.GetLastRenderedView()
                                                   .viewProjectionMatrix
                                             : m_renderScene.GetView()
                                                   .viewProjectionMatrix;
    m_viewData.SetupFromSnapshot(m_renderScene.GetView(),
                                 previousViewProjection,
                                 previousViewValid,
                                 result.temporalHistoryReset);
    // Keep shader-facing frame constants and post-process qualification
    // identities bound to the accepted packet, not to a process-local render
    // loop counter.  This is also the exact sequence carried by the temporal
    // history candidate used at submission and Present.
    m_viewData.frameNumber = m_renderScene.GetAcceptedHeader().sequence;

    const RenderFrameSettings& frameSettings = m_renderScene.GetSettings();
    const RenderAntiAliasingMode requestedAntiAliasingMode =
        frameSettings.postProcess.antiAliasingMode;
    const bool temporalAntiAliasingRequested =
        frameSettings.postProcess.enabled &&
        requestedAntiAliasingMode == RenderAntiAliasingMode::TAA;
    const RenderOutputExtent temporalExtent{
        m_viewData.viewportWidth, m_viewData.viewportHeight};
    const bool previousTemporalStateReusable =
        temporalAntiAliasingRequested && !result.temporalHistoryReset &&
        m_activeViewHistoryAvailability.has_value() &&
        m_activeViewHistoryAvailability->available &&
        m_presentedTemporalPresentationState.has_value() &&
        IsValidRenderTemporalJitter(
            m_presentedTemporalPresentationState->jitter);
    // The temporal sampling sequence belongs to presentation-qualified view
    // history, not to accepted scene-packet ordinals.  A packet that never
    // presents cannot advance the presentation-owned phase, while a reset
    // starts at phase zero regardless of how many startup packets preceded it.
    const uint32 currentTemporalPhase = previousTemporalStateReusable
        ? (m_presentedTemporalPresentationState->jitter.phaseIndex + 1U) %
              RVX_RENDER_TEMPORAL_JITTER_PHASE_COUNT
        : 0U;
    const RenderTemporalJitter currentTemporalJitter =
        temporalAntiAliasingRequested
            ? MakeRenderTemporalJitterForPhase(currentTemporalPhase,
                                               temporalExtent)
            : RenderTemporalJitter{};
    const RenderTemporalJitter previousTemporalJitter =
        previousTemporalStateReusable
            ? m_presentedTemporalPresentationState->jitter
            : RenderTemporalJitter{};
    const Mat4 previousJitteredViewProjection =
        previousTemporalStateReusable
            ? m_presentedTemporalPresentationState->jitteredViewProjection
            : m_viewData.previousViewProjectionMatrix;
    m_viewData.SetTemporalJitter(
        currentTemporalJitter,
        previousTemporalJitter,
        previousJitteredViewProjection,
        previousTemporalStateReusable);
    if (m_activeViewHistoryCandidate.has_value())
    {
        m_pendingTemporalPresentationState = PendingTemporalPresentationState{
            *m_activeViewHistoryCandidate,
            currentTemporalJitter,
            m_viewData.jitteredViewProjectionMatrix};
    }

    m_viewData.instancingMode = frameSettings.instancingMode;
    SetGPUDrivenCullingMode(frameSettings.gpuCulling.mode);
    const RenderAmbientOcclusionSettings resolvedAmbientOcclusion =
        ResolveAmbientOcclusionSettings(frameSettings);
    m_postProcessSettings.enableBloom =
        frameSettings.postProcess.enabled &&
        frameSettings.postProcess.enableBloom;
    if (FindCompositionPass<DepthPrepass>() != nullptr)
    {
        FindCompositionPass<DepthPrepass>()->SetEnabled(
            resolvedAmbientOcclusion.enabled &&
            frameSettings.pipelineRequest.recipe ==
                RenderPipelineRecipe::ForwardStandard);
    }
    m_postProcessSettings.enableSSR =
        frameSettings.postProcess.enabled &&
        frameSettings.postProcess.enableSSR;
    m_postProcessSettings.antiAliasingMode =
        frameSettings.postProcess.enabled
            ? requestedAntiAliasingMode
            : RenderAntiAliasingMode::None;
    m_postProcessSettings.enableVelocityOutput =
        frameSettings.postProcess.enabled &&
        (frameSettings.postProcess.enableVelocityOutput ||
         requestedAntiAliasingMode == RenderAntiAliasingMode::TAA ||
         resolvedAmbientOcclusion.enabled);
    m_postProcessSettings.enableColorGrading =
        frameSettings.postProcess.enabled &&
        frameSettings.postProcess.enableColorGrading;
    m_postProcessSettings.enableToonOutline =
        frameSettings.postProcess.enabled &&
        frameSettings.postProcess.enableToonOutline;
    m_postProcessSettings.toonOutlineDepthThreshold =
        frameSettings.postProcess.toonOutlineDepthThreshold;
    m_postProcessSettings.toonOutlineNormalThreshold =
        frameSettings.postProcess.toonOutlineNormalThreshold;
    m_postProcessSettings.enableRayTracedReflectionDenoise =
        frameSettings.postProcess.enableRayTracedReflectionDenoise;
    m_postProcessSettings.toneMappingOperator =
        static_cast<ToneMappingOperator>(
            frameSettings.postProcess.toneMappingOperator);
    m_postProcessSettings.exposureSettings =
        frameSettings.postProcess.exposureSettings;
    m_postProcessSettings.gamma = frameSettings.postProcess.gamma;
    m_postProcessSettings.bloomThreshold =
        frameSettings.postProcess.bloomThreshold;
    m_postProcessSettings.bloomIntensity =
        frameSettings.postProcess.bloomIntensity;
    m_postProcessSettings.bloomRadius =
        frameSettings.postProcess.bloomRadius;
    m_postProcessSettings.contrast =
        frameSettings.postProcess.colorGradingContrast;
    m_postProcessSettings.saturation =
        frameSettings.postProcess.colorGradingSaturation;
    m_postProcessSettings.brightness =
        frameSettings.postProcess.colorGradingBrightness;
    m_postProcessSettings.enableRayTracedReflections =
        frameSettings.rayTracing.enabled &&
        frameSettings.rayTracing.enableReflections;
    ApplyPostProcessSettings(m_postProcessSettings);

    m_shadowPassConfig.shadowMapSize =
        frameSettings.shadows.atlasResolution;
    m_shadowPassConfig.numCascades = frameSettings.shadows.cascadeCount;
    m_shadowPassConfig.maxDistance = frameSettings.shadows.maxDistance;
    m_shadowPassConfig.cascadeSplitLambda =
        frameSettings.shadows.cascadeSplitLambda;
    m_shadowPassConfig.filterRadiusTexels =
        frameSettings.shadows.filterRadiusTexels;
    m_shadowPassConfig.shadowBias = frameSettings.shadows.shadowBias;
    m_shadowPassConfig.normalBias = frameSettings.shadows.normalBias;
    m_shadowPassConfig.cascadeBlendRatio =
        frameSettings.shadows.cascadeBlendRatio;
    ApplyShadowPassConfig(m_shadowPassConfig);
    if (FindCompositionPass<ShadowPass>())
    {
        FindCompositionPass<ShadowPass>()->SetEnabled(frameSettings.shadows.enabled);
    }
    if (FindCompositionPass<RayTracedShadowPass>())
    {
        FindCompositionPass<RayTracedShadowPass>()->SetEnabled(
            frameSettings.rayTracing.enabled &&
            frameSettings.rayTracing.enableShadows);
    }

    GPUCullingConfig gpuCullingConfig = GetGPUDrivenCullingConfig();
    gpuCullingConfig.maxInstances = frameSettings.gpuCulling.maxVisibleObjects;
    gpuCullingConfig.enableOcclusionCulling =
        frameSettings.gpuCulling.enableOcclusionCulling;
    gpuCullingConfig.enableDistanceCulling =
        frameSettings.gpuCulling.enableDistanceCulling;
    gpuCullingConfig.maxDrawDistance =
        frameSettings.gpuCulling.maxDrawDistance;
    SetGPUDrivenCullingConfig(gpuCullingConfig);

    SceneRayTracingBudgetSettings rayTracingBudget =
        GetRayTracingBudgetSettings();
    rayTracingBudget.enabled = frameSettings.rayTracing.budgetEnabled;
    rayTracingBudget.maxRayCount = frameSettings.rayTracing.maxRayCount;
    rayTracingBudget.maxDenoiseTapCount =
        frameSettings.rayTracing.maxDenoiseTapCount;
    rayTracingBudget.maxTrackedResourceBytes =
        frameSettings.rayTracing.maxTrackedResourceBytes;
    rayTracingBudget.maxMeasuredGpuMs =
        frameSettings.rayTracing.maxMeasuredGpuMs;
    rayTracingBudget.maxShadowMeasuredGpuMs =
        frameSettings.rayTracing.maxShadowMeasuredGpuMs;
    rayTracingBudget.maxReflectionMeasuredGpuMs =
        frameSettings.rayTracing.maxReflectionMeasuredGpuMs;
    rayTracingBudget.gpuTimingAdjustmentFrameCount =
        frameSettings.rayTracing.gpuTimingAdjustmentFrameCount;
    ApplyRayTracingBudgetSettings(rayTracingBudget);

    m_featureSnapshot = std::make_shared<const RenderFeatureSnapshot>(
        m_renderScene.GetFeatures());
    const RenderFeatureSnapshotMetadata featureMetadata =
        m_featureSnapshot->GetMetadata();
    m_featureExtractionStats = {};
    m_featureExtractionStats.attempted = true;
    m_featureExtractionStats.usedProviderPath = true;
    m_featureExtractionStats.requiresLegacyFallback = false;
    m_featureExtractionStats.snapshotSchemaVersion =
        featureMetadata.schemaVersion;
    m_featureExtractionStats.snapshotSequence = featureMetadata.sequence;
    m_featureExtractionStats.snapshotComplete = featureMetadata.complete;
    m_featureExtractionStats.providerCount = featureMetadata.providerCount;
    m_featureExtractionStats.skippedProviderCount =
        featureMetadata.skippedProviderCount;
    m_featureExtractionStats.particleItemCount =
        featureMetadata.particleItemCount;
    m_featureExtractionStats.waterItemCount =
        featureMetadata.waterItemCount;
    m_featureExtractionStats.terrainItemCount =
        featureMetadata.terrainItemCount;
    for (const ParticleRenderSnapshotItem& item :
         m_featureSnapshot->particles.items)
    {
        if (item.payloadStatus ==
            ParticleRenderSnapshotPayloadStatus::MetadataOnly)
        {
            ++m_featureExtractionStats.particleMetadataOnlyCount;
        }
        if (item.IsCpuPacketReady())
        {
            ++m_featureExtractionStats.particleCpuPacketReadyCount;
        }
        if (item.sortingSupported)
        {
            ++m_featureExtractionStats.particleSortingSupportedCount;
        }
    }
      if (FindCompositionPass<ParticleFeaturePass>())
      {
          FindCompositionPass<ParticleFeaturePass>()->SetSnapshot(&m_featureSnapshot->particles);
      }
      if (FindCompositionPass<WaterFeaturePass>())
      {
          FindCompositionPass<WaterFeaturePass>()->SetSnapshot(&m_featureSnapshot->water);
      }

    const RenderEnvironmentSnapshot& environment =
        m_renderScene.GetEnvironment();
    const RenderTextureResourceData* irradianceResource =
        registry.ResolveTexture(environment.irradianceTexture);
    const RenderTextureResourceData* prefilteredResource =
        registry.ResolveTexture(environment.prefilteredTexture);
    const RenderTextureResourceData* brdfLUTResource =
        registry.ResolveTexture(environment.brdfLutTexture);
    const bool textureIBLReady =
        environment.irradianceTexture.IsValid() &&
        environment.prefilteredTexture.IsValid() &&
        environment.brdfLutTexture.IsValid() &&
        irradianceResource && irradianceResource->texture &&
        prefilteredResource && prefilteredResource->texture &&
        brdfLUTResource && brdfLUTResource->texture;
    ++m_environmentIBLStats.frameCount;
    m_environmentIBLStats.skyboxFound =
        m_renderScene.GetSky().skyTexture.IsValid();
    m_environmentIBLStats.uploadRequested = false;
    m_environmentIBLStats.prefilteredMipLevels = 1;
    m_environmentIBLStats.intensity = environment.intensity;
    UpdateEnvironmentIBLBindingDiagnostics(
        m_environmentIBLRequested,
        textureIBLReady,
        "PacketEnvironmentResourcesUnavailable");
    if (textureIBLReady)
    {
        m_environmentIBLStats.prefilteredMipLevels =
            std::max(1U, prefilteredResource->texture->GetMipLevels());
    }
    m_viewData.textureIBLEnabled =
        m_environmentIBLStats.textureIBLEnabled ? 1 : 0;
    m_viewData.textureIBLPrefilteredMipLevels =
        m_environmentIBLStats.prefilteredMipLevels;
    m_viewData.textureIBLIntensity = environment.intensity;
    m_viewData.ambientFloorIntensity =
        m_environmentIBLStats.textureIBLEnabled ? 0.0f : 0.08f;

    const auto visibilityAndDrawPlanningStart =
        std::chrono::steady_clock::now();
    BuildMaterialDrawLists();
    m_pendingVisibilityAndDrawPlanningNanoseconds =
        static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() -
            visibilityAndDrawPlanningStart)
                                .count());
    return result;
}

RenderFrameExecutionResult SceneRenderer::RenderAcceptedFrame()
{
    RenderFrameExecutionResult result;
    if (!m_renderScene.HasAcceptedFrame())
    {
        return result;
    }
    if (m_gpuSceneUpdate && m_renderResourceRegistry &&
        m_renderResourceRegistry->GetContentRevision() !=
            m_lastGPUSceneResourceContentRevision)
    {
        // Exact-generation revalidation prevents an evicted/reloaded resource
        // from ever making an old accepted resident row appear current.
        try
        {
            static_cast<void>(
                m_gpuSceneUpdate->Revalidate(*m_renderResourceRegistry));
            m_lastGPUSceneResourceContentRevision =
                m_renderResourceRegistry->GetContentRevision();
        }
        catch (const std::bad_alloc&)
        {
            m_gpuSceneUpdate->RecordFailure(
                m_renderScene.GetAcceptedHeader().sequence,
                GPUScenePublicationFailureReason::AllocationFailed);
        }
        catch (...)
        {
            m_gpuSceneUpdate->RecordUnexpectedFailure(
                m_renderScene.GetAcceptedHeader().sequence);
        }
    }
    ReclaimGPUSceneRetiredRows();
    RVX_ASSERT_MSG(!m_submissionBatch,
                   "Previous frame submission ownership was not resolved");
    m_submissionBatch = std::make_unique<RenderSubmissionResourceBatch>();
    m_submissionQualifiedFrameDiagnostics = false;
    m_hasRecordingSubmissionBoundary = false;
    m_recordingGraphicsSubmissionBaseline = 0;
    m_recordingSubmissionTracker = nullptr;
    m_currentCpuFrameTiming = {};
    m_currentCpuFrameTiming.visibilityAndDrawPlanning =
        m_pendingVisibilityAndDrawPlanningNanoseconds;
    result.frameSequence = m_renderScene.GetAcceptedHeader().sequence;
    result.referencedResources = m_renderScene.GetReferencedResources();
    if (FindPostProcessEffect<ToneMappingPass>())
    {
        const RenderFrameHeaderV5& header = m_renderScene.GetAcceptedHeader();
        FindPostProcessEffect<ToneMappingPass>()->SetPixelProbeRequest(
            m_renderScene.GetCaptureRequest(),
            header.sequence,
            header.requiredSceneRevision,
            m_renderContext ? m_renderContext->GetSurface().generation : 0);
    }
    Render();
    if (HasSubmissionFailure())
    {
        RVX_RENDER_ERROR(
            "SceneRenderer: rejected recorded frame {} (rendered={}, graphCompileValid={}, executionStatus={}, gpuDrivenFailureStage={})",
            result.frameSequence,
            m_frameDiagnostics.rendered,
            m_frameDiagnostics.graphCompileValid,
            GetRenderExecutionStatusName(
                m_renderPolicyDiagnostics.executionReport.status),
            GetGPUDrivenFrameFailureStageName(
                m_gpuDrivenFrameFailureStage));
        if (GetRenderFeatureRegistry() != nullptr &&
            !m_pendingRenderExecutionReceipt.has_value())
        {
            const RenderExecutionReceipt& attempted =
                m_lastAttemptedRenderExecutionReceipt;
            RVX_RENDER_ERROR(
                "SceneRenderer: execution receipt rejected (available={}, graphComposed={}, executionSucceeded={}, structurallyValid={}, diagnostic={})",
                attempted.available,
                attempted.graphComposed,
                attempted.executionSucceeded,
                attempted.IsStructurallyValid(),
                attempted.diagnostic.empty() ? "none" : attempted.diagnostic);
        }
        for (const RenderPassExecutionReport& pass :
             m_renderPolicyDiagnostics.executionReport.passes)
        {
            if (pass.status != RenderExecutionStatus::Failed)
            {
                continue;
            }
            RVX_RENDER_ERROR(
                "SceneRenderer: failed pass={} reason={} gpuLane={} directLane={} skippedPackets={}",
                GetRenderPassKindName(pass.pass),
                GetRenderPolicyReasonName(pass.reason),
                GetRenderExecutionStatusName(pass.gpuDrivenLane.status),
                GetRenderExecutionStatusName(pass.directLane.status),
                pass.skippedPacketCount);
        }
        result.code = RenderFrameExecutionCode::SubmissionFailed;
        return result;
    }
    if ((m_depthGPUCulling &&
         !m_depthGPUCulling->RetainSubmissionResources(*m_submissionBatch)) ||
        (m_opaqueGPUCulling &&
         !m_opaqueGPUCulling->RetainSubmissionResources(*m_submissionBatch)) ||
        (m_depthGPUCullingRecordedState &&
         !m_depthGPUCullingRecordedState->RetainSubmissionResources(*m_submissionBatch)) ||
        (m_opaqueGPUCullingRecordedState &&
         !m_opaqueGPUCullingRecordedState->RetainSubmissionResources(*m_submissionBatch)))
    {
        result.code = RenderFrameExecutionCode::SubmissionFailed;
        return result;
    }
    RenderSubmissionTracker* const tracker = m_renderContext
        ? RenderContextInternalAccess::GetSubmissionTracker(*m_renderContext)
        : nullptr;
    if (tracker == nullptr)
    {
        result.code = RenderFrameExecutionCode::SubmissionFailed;
        return result;
    }
    // Capture after graph realization and every submission-owned resource has
    // been retained.  The next accepted token must advance this exact floor.
    m_recordingGraphicsSubmissionBaseline =
        tracker->GetLastSubmittedValue(GPUQueueDomain::Graphics);
    m_recordingSubmissionTracker = tracker;
    m_hasRecordingSubmissionBoundary = true;
    result.code = RenderFrameExecutionCode::Rendered;
    return result;
}

bool SceneRenderer::CompleteToneMappingPixelProbe(
    uint64 requestId,
    uint64 frameSequence,
    RenderFramePixelProbeResult& outResult)
{
    const bool completed =
        FindPostProcessEffect<ToneMappingPass>() != nullptr &&
        FindPostProcessEffect<ToneMappingPass>()->CompletePixelProbe(
            requestId, frameSequence, outResult);
    return completed;
}

bool SceneRenderer::ArmGPUSceneCullingQualificationCapture()
{
    bool armed = false;
    if (m_depthGPUCulling)
    {
        armed = m_depthGPUCulling->ArmGPUSceneQualificationCapture() || armed;
    }
    if (m_opaqueGPUCulling)
    {
        armed = m_opaqueGPUCulling->ArmGPUSceneQualificationCapture() || armed;
    }
    return armed;
}

bool SceneRenderer::ArmDirectOpaqueRasterReadbackQualificationCapture()
{
    return m_directOpaqueRasterReadbackQualification.Arm();
}

bool SceneRenderer::ArmQualificationCaptureBundle()
{
    return m_qualificationCaptureBridge.Arm();
}

bool SceneRenderer::ArmRenderQualityPerformanceSession()
{
    RenderQualityPerformanceHardwareIdentity hardware;
    uint64 initialCumulativeTrackedUploadBytes = 0;
    if (!BuildRenderQualityPerformanceHardwareIdentity(hardware))
    {
        m_qualityPerformanceSession.RejectAdmission(
            RenderQualityPerformanceFailure::InvalidHardwareIdentity,
            "Render could not freeze a backend capability identity for performance evidence.");
        return false;
    }
    if (!CollectRenderQualityCumulativeTrackedUploadBytes(
            initialCumulativeTrackedUploadBytes))
    {
        m_qualityPerformanceSession.RejectAdmission(
            RenderQualityPerformanceFailure::TelemetryUnavailable,
            "Render could not freeze the tracked upload baseline for performance evidence.");
        return false;
    }
    return m_qualityPerformanceSession.Arm(
        hardware, initialCumulativeTrackedUploadBytes);
}

void SceneRenderer::RejectRenderQualityPerformanceSessionAdmission(
    const char* diagnostic) noexcept
{
    m_qualityPerformanceSession.RejectAdmission(
        RenderQualityPerformanceFailure::SubmissionRejected,
        diagnostic != nullptr
            ? diagnostic
            : "RenderThreadRuntime could not admit the performance session.");
}

bool SceneRenderer::RecordRenderQualityPerformanceCpuTiming(
    const RenderFrameIdentity& frameIdentity,
    const RenderQualityPerformanceCpuTiming& timing) noexcept
{
    return m_qualityPerformanceSession.RecordCpuTiming(frameIdentity, timing);
}

bool SceneRenderer::BuildRenderQualityPerformanceHardwareIdentity(
    RenderQualityPerformanceHardwareIdentity& outIdentity) const noexcept
{
    outIdentity = {};
    const IRHIDevice* const device = m_renderContext != nullptr
        ? m_renderContext->GetDevice()
        : nullptr;
    if (device == nullptr)
        return false;

    try
    {
        const RHICapabilities& capabilities = device->GetCapabilities();
        outIdentity.backend =
            GetPerformanceQualificationBackendName(capabilities.backendType);
        const std::string report = ExportRHICapabilityReportJson(
            BuildRHICapabilityReport(capabilities));
        outIdentity.capabilityReportSha256 = Hash::FormatSHA256Digest(
            Hash::ComputeSHA256(report));
    }
    catch (...)
    {
        outIdentity = {};
        return false;
    }
    return outIdentity.IsValid();
}

bool SceneRenderer::CollectRenderQualityCumulativeTrackedUploadBytes(
    uint64& outTrackedUploadBytes) const noexcept
{
    outTrackedUploadBytes = 0;
    const RenderMutationEvidenceDiagnostics& evidence =
        m_completedMutationEvidence;
    if (!evidence.completion.available || evidence.completion.saturated)
    {
        return false;
    }

    return TryAccumulatePerformanceBytes(
               outTrackedUploadBytes, evidence.gpuSceneUpload.uploadBytes) &&
           TryAccumulatePerformanceBytes(
               outTrackedUploadBytes, evidence.gpuCulling.instanceUploadBytes) &&
           TryAccumulatePerformanceBytes(
               outTrackedUploadBytes, evidence.gpuCulling.candidateUploadBytes) &&
           TryAccumulatePerformanceBytes(
               outTrackedUploadBytes, evidence.gpuCulling.activeRowUploadBytes) &&
           TryAccumulatePerformanceBytes(
               outTrackedUploadBytes, evidence.directRaster.instanceUploadBytes) &&
           TryAccumulatePerformanceBytes(
               outTrackedUploadBytes, evidence.directRaster.indexUploadBytes);
}

bool SceneRenderer::CollectRenderQualityPerformanceTelemetry(
    const RenderFrameIdentity& frameIdentity,
    RenderQualityPerformanceFrameTelemetry& outTelemetry) const noexcept
{
    outTelemetry = {};
    if (!frameIdentity.IsValid() || m_renderGraph == nullptr ||
        m_transientResourcePool == nullptr ||
        m_renderResourceRegistry == nullptr ||
        !m_completedMutationEvidence.completion.available ||
        m_completedMutationEvidence.completion.completedFrameSequence !=
            frameIdentity.frameSequence)
    {
        return false;
    }

    try
    {
        const TransientResourcePool::Stats poolStats =
            m_transientResourcePool->GetStats();
        const RenderResourceRegistryStats registryStats =
            m_renderResourceRegistry->GetStats();
        const RenderGraph::Diagnostics graphDiagnostics =
            m_renderGraph->GetDiagnostics();
        if (registryStats.residentMeshCount >
                std::numeric_limits<uint32>::max() ||
            registryStats.residentTextureCount >
                std::numeric_limits<uint32>::max() ||
            registryStats.residentMaterialCount >
                std::numeric_limits<uint32>::max())
        {
            return false;
        }

        uint64 importedHistoryBytes = 0;
        uint64 importedPersistentBytes = 0;
        uint32 stableImportedTextureCount = 0;
        uint32 stableImportedBufferCount = 0;
        for (const RenderGraph::ResourceDiagnostic& resource :
             graphDiagnostics.resources)
        {
            if (!resource.used || !resource.imported)
                continue;
            if (resource.importedResourceClass !=
                    RGImportedResourceClass::Persistent &&
                resource.importedResourceClass !=
                    RGImportedResourceClass::History)
            {
                continue;
            }
            // A saturated diagnostic cannot be a trustworthy performance
            // footprint. Reject it at the renderer projection boundary before
            // a later policy check could mistake it for usable telemetry.
            if (resource.estimatedMemoryBytes ==
                std::numeric_limits<uint64>::max())
            {
                return false;
            }
            uint32& importedCount = resource.type ==
                    RenderGraph::DiagnosticResourceType::Texture
                ? stableImportedTextureCount
                : stableImportedBufferCount;
            if (importedCount == std::numeric_limits<uint32>::max())
                return false;
            ++importedCount;

            // Lifetime class is explicit import metadata. RHI/debug names are
            // diagnostic-only and must never decide performance accounting.
            if (resource.importedResourceClass ==
                    RGImportedResourceClass::History &&
                !TryAccumulatePerformanceBytes(
                    importedHistoryBytes, resource.estimatedMemoryBytes))
            {
                return false;
            }
            if (resource.importedResourceClass ==
                    RGImportedResourceClass::Persistent &&
                !TryAccumulatePerformanceBytes(
                    importedPersistentBytes, resource.estimatedMemoryBytes))
            {
                return false;
            }
        }

        uint64 cumulativeUploadBytes = 0;
        if (!CollectRenderQualityCumulativeTrackedUploadBytes(
                cumulativeUploadBytes))
            return false;

        // The performance lab always builds particle and water feature passes.
        // Their per-frame uploads are not monotonic mutation totals, therefore
        // retain them separately and fold them into the exact-frame delta in
        // the session.  A stale stats record would otherwise attribute a later
        // frame's VFX upload to this presented receipt.
        const ParticleFeaturePassStats& particleStats =
            GetParticleFeaturePassStats();
        const WaterFeaturePassStats& waterStats = GetWaterFeaturePassStats();
        if (particleStats.recordFrameIdentity != frameIdentity ||
            waterStats.recordFrameIdentity != frameIdentity)
        {
            return false;
        }
        uint64 frameTrackedUploadBytes = 0;
        if (!TryAccumulatePerformanceBytes(
                frameTrackedUploadBytes, particleStats.uploadBytes) ||
            !TryAccumulatePerformanceBytes(
                frameTrackedUploadBytes, waterStats.uploadBytes))
        {
            return false;
        }

        outTelemetry.frameIdentity = frameIdentity;
        outTelemetry.trackedUploadCoverageMask =
            RVX_RENDER_QUALITY_PERFORMANCE_REQUIRED_UPLOAD_COVERAGE;
        outTelemetry.cumulativeTrackedUploadBytes = cumulativeUploadBytes;
        outTelemetry.frameTrackedUploadBytes = frameTrackedUploadBytes;
        outTelemetry.cumulativeTrackedUploadBytesAvailable = true;
        // These are deliberately tracked estimated footprint domains, not a
        // full GPU memory report. Imported temporal history is an explicit
        // RenderGraph lifetime class, never a name-derived heuristic.
        outTelemetry.trackedEstimatedFootprint.coverageMask =
            RVX_RENDER_QUALITY_PERFORMANCE_REQUIRED_FOOTPRINT_COVERAGE;
        outTelemetry.trackedEstimatedFootprint
            .transientPoolPhysicalEstimatedBytes = poolStats.totalPooledMemory;
        outTelemetry.trackedEstimatedFootprint
            .importedPersistentEstimatedBytes = importedPersistentBytes;
        outTelemetry.trackedEstimatedFootprint
            .importedHistoryEstimatedBytes = importedHistoryBytes;
        outTelemetry.trackedEstimatedFootprint
            .resourceRegistryResidentEstimatedBytes =
            static_cast<uint64>(registryStats.usedMemory);
        outTelemetry.trackedEstimatedFootprint.available = true;
        outTelemetry.resourceCounts.transientTextureCount =
            poolStats.texturePoolSize;
        outTelemetry.resourceCounts.transientBufferCount =
            poolStats.bufferPoolSize;
        outTelemetry.resourceCounts.stableImportedTextureCount =
            stableImportedTextureCount;
        outTelemetry.resourceCounts.stableImportedBufferCount =
            stableImportedBufferCount;
        outTelemetry.resourceCounts.residentMeshCount =
            static_cast<uint32>(registryStats.residentMeshCount);
        outTelemetry.resourceCounts.residentTextureCount =
            static_cast<uint32>(registryStats.residentTextureCount);
        outTelemetry.resourceCounts.residentMaterialCount =
            static_cast<uint32>(registryStats.residentMaterialCount);
        outTelemetry.resourceCounts.available = true;
    }
    catch (...)
    {
        outTelemetry = {};
        return false;
    }
    return outTelemetry.IsValid();
}

bool SceneRenderer::ArmExposureQualificationProbe()
{
    if (FindCompositionFeature<PostProcessPipelineFeatureAdapter>() == nullptr || m_exposureProbeCompletion ||
        m_exposureProbeHistoryIdentity.has_value() ||
        m_exposureProbeSourceExecutionReceipt.has_value() ||
        !FindCompositionFeature<PostProcessPipelineFeatureAdapter>()->RequestExposureQualificationProbe())
    {
        return false;
    }
    m_exposureProbeSourceExecutionReceipt.reset();
    return true;
}

bool SceneRenderer::ArmAmbientOcclusionQualificationProbe()
{
    if (FindCompositionFeature<AmbientOcclusionFeatureAdapter>() == nullptr ||
        m_ambientOcclusionProbeSubmissionAccepted ||
        m_ambientOcclusionProbeHistoryIdentity.has_value() ||
        m_ambientOcclusionProbeSourceExecutionReceipt.has_value() ||
        !FindCompositionFeature<AmbientOcclusionFeatureAdapter>()->RequestQualificationProbe())
    {
        return false;
    }
    m_ambientOcclusionProbePresented = false;
    m_ambientOcclusionProbeAbandoned = false;
    return true;
}

bool SceneRenderer::ArmToonOutlineQualificationProbe()
{
    if (FindPostProcessEffect<ScreenSpaceOutlinePass>() == nullptr ||
        m_toonOutlineProbeSubmissionAccepted ||
        m_toonOutlineProbeSourceExecutionReceipt.has_value() ||
        FindPostProcessEffect<ScreenSpaceOutlinePass>()->HasPendingQualificationProbe() ||
        !FindPostProcessEffect<ScreenSpaceOutlinePass>()->ArmQualificationProbe())
    {
        return false;
    }
    m_toonOutlineProbePresented = false;
    m_toonOutlineProbeAbandoned = false;
    return true;
}

void SceneRenderer::ClearHiZReflectionQualificationCompletion() noexcept
{
    // A completion-owned SSR result is deliberately independent of the
    // latest ordinary presentation diagnostics. Clear every projection as
    // one owner-thread operation before a new probe can bind a source frame.
    m_completedHiZReflectionQualificationReceipt = {};
    m_completedHiZReflectionQualificationDiagnostics = {};
    m_frameDiagnostics.hiZReflectionQualificationReceipt = {};
    m_frameDiagnostics.hiZReflectionQualification = {};
    if (m_toolDiagnosticsSnapshot.frameDiagnosticsAvailable)
    {
        m_toolDiagnosticsSnapshot.frame.hiZReflectionQualificationReceipt =
            {};
        m_toolDiagnosticsSnapshot.frame.hiZReflectionQualification = {};
    }
}

bool SceneRenderer::ArmHiZReflectionQualificationProbe()
{
    if (FindCompositionFeature<ReflectionFeatureAdapter>() == nullptr ||
        m_reflectionProbeSubmissionAccepted ||
        m_reflectionProbeSourceExecutionReceipt.has_value() ||
        m_reflectionProbeSourceDiagnostics.has_value() ||
        FindCompositionFeature<ReflectionFeatureAdapter>()->HasPendingSSRQualificationProbe() ||
        !FindCompositionFeature<ReflectionFeatureAdapter>()->ArmSSRQualificationProbe())
    {
        return false;
    }
    ++m_nextHiZReflectionProbeGeneration;
    if (m_nextHiZReflectionProbeGeneration == 0)
    {
        ++m_nextHiZReflectionProbeGeneration;
    }
    m_hiZReflectionProbeBinding = {};
    m_hiZReflectionProbeBinding.generation =
        m_nextHiZReflectionProbeGeneration;
    ClearHiZReflectionQualificationCompletion();
    m_reflectionProbePresented = false;
    m_reflectionProbeAbandoned = false;
    return true;
}

void SceneRenderer::PollGPUSceneCullingQualificationCompletion(
    const RenderSubmissionTracker& tracker)
{
    if (m_depthGPUCulling)
    {
        static_cast<void>(
            m_depthGPUCulling->PollGPUSceneQualificationCapture(tracker));
    }
    if (m_opaqueGPUCulling)
    {
        static_cast<void>(
            m_opaqueGPUCulling->PollGPUSceneQualificationCapture(tracker));
    }
    static_cast<void>(
        m_directOpaqueRasterReadbackQualification.PollCompletion(tracker));
    static_cast<void>(m_qualificationCaptureBridge.PollCompletion(tracker));
    if (m_qualificationPassTiming->IsEnabled() &&
        m_qualificationPassTiming->GetDiagnostics().state ==
            RenderGraphPassTimingState::PendingCompletion)
    {
        static_cast<void>(m_qualificationPassTiming->PollCompletion(tracker));
    }
    if (m_exposureProbeCompletion &&
        m_exposureProbeHistoryIdentity.has_value())
    {
        const GPUCompletionStatus status =
            tracker.Query(*m_exposureProbeCompletion);
        if ((status == GPUCompletionStatus::Completed ||
             status == GPUCompletionStatus::CompatibilityWaitIdle) &&
            m_exposureProbePresented && FindCompositionFeature<PostProcessPipelineFeatureAdapter>() != nullptr)
        {
            const RenderViewHistoryIdentity identity =
                *m_exposureProbeHistoryIdentity;
            RenderPostProcessExecutionReceipt postProcess;
            if (FindCompositionFeature<PostProcessPipelineFeatureAdapter>()->CompleteExposureQualificationProbe(
                    identity, postProcess))
            {
                if (m_exposureProbeSourceExecutionReceipt.has_value() &&
                    m_exposureProbeSourceExecutionReceipt->presented &&
                    m_exposureProbeSourceExecutionReceipt->frameIdentity ==
                        identity.previousFrame &&
                    m_exposureProbeSourceExecutionReceipt->viewIdentity ==
                        identity.view)
                {
                    RenderExecutionReceipt qualified =
                        *m_exposureProbeSourceExecutionReceipt;
                    qualified.postProcess = std::move(postProcess);
                    if (qualified.IsStructurallyValid())
                    {
                        // Qualification completion is an explicit result for
                        // its frozen source frame. Publish that source receipt
                        // even when ordinary rendering advanced meanwhile.
                        m_completedRenderExecutionReceipt = qualified;
                        m_lastAttemptedRenderExecutionReceipt =
                            std::move(qualified);
                    }
                    else
                    {
                        m_lastAttemptedRenderExecutionReceipt = qualified;
                        m_lastAttemptedRenderExecutionReceipt.succeeded = false;
                        m_lastAttemptedRenderExecutionReceipt.diagnostic =
                            "Completed exposure probe invalidated its source receipt";
                    }
                }
                else
                {
                    m_lastAttemptedRenderExecutionReceipt =
                        m_completedRenderExecutionReceipt;
                    m_lastAttemptedRenderExecutionReceipt.succeeded = false;
                    m_lastAttemptedRenderExecutionReceipt.diagnostic =
                        "Exposure qualification source receipt was lost or mismatched";
                }
            }
            else
            {
                m_lastAttemptedRenderExecutionReceipt.succeeded = false;
                m_lastAttemptedRenderExecutionReceipt.diagnostic =
                    "Exposure qualification probe failed after GPU completion";
            }
            m_exposureProbeCompletion.reset();
            m_exposureProbeHistoryIdentity.reset();
            m_exposureProbeSourceExecutionReceipt.reset();
            m_exposureProbePresented = false;
        }
        else if (status == GPUCompletionStatus::Lost)
        {
            if (GetPostProcessStack())
            {
                GetPostProcessStack()->GetExposureController()
                    .CancelQualificationProbe();
            }
            m_exposureProbeCompletion.reset();
            m_exposureProbeHistoryIdentity.reset();
            m_exposureProbeSourceExecutionReceipt.reset();
            m_exposureProbePresented = false;
        }
    }

    if (m_ambientOcclusionProbeSubmissionAccepted &&
        FindCompositionFeature<AmbientOcclusionFeatureAdapter>() != nullptr &&
        (m_ambientOcclusionProbePresented ||
         m_ambientOcclusionProbeAbandoned))
    {
        RenderExecutionReceipt qualified =
            m_ambientOcclusionProbeSourceExecutionReceipt.has_value()
                ? *m_ambientOcclusionProbeSourceExecutionReceipt
                : RenderExecutionReceipt{};
        const bool completed =
            FindCompositionFeature<AmbientOcclusionFeatureAdapter>()->CompleteQualificationProbe(
                tracker, qualified.ambientOcclusion);
        if (completed && m_ambientOcclusionProbePresented &&
            m_ambientOcclusionProbeHistoryIdentity.has_value() &&
            qualified.presented &&
            qualified.frameIdentity ==
                m_ambientOcclusionProbeHistoryIdentity->previousFrame &&
            qualified.viewIdentity ==
                m_ambientOcclusionProbeHistoryIdentity->view &&
            qualified.IsStructurallyValid())
        {
            m_completedRenderExecutionReceipt = qualified;
            m_lastAttemptedRenderExecutionReceipt = std::move(qualified);
            m_ambientOcclusionProbeSubmissionAccepted = false;
            m_ambientOcclusionProbeHistoryIdentity.reset();
            m_ambientOcclusionProbeSourceExecutionReceipt.reset();
            m_ambientOcclusionProbePresented = false;
            m_ambientOcclusionProbeAbandoned = false;
        }
        else if (!FindCompositionFeature<AmbientOcclusionFeatureAdapter>()->HasPendingQualificationProbe())
        {
            m_lastAttemptedRenderExecutionReceipt =
                m_ambientOcclusionProbeSourceExecutionReceipt.has_value()
                    ? *m_ambientOcclusionProbeSourceExecutionReceipt
                    : m_completedRenderExecutionReceipt;
            m_lastAttemptedRenderExecutionReceipt.succeeded = false;
            m_lastAttemptedRenderExecutionReceipt.diagnostic =
                "Ambient-occlusion qualification probe failed or lost its presented source";
            m_ambientOcclusionProbeSubmissionAccepted = false;
            m_ambientOcclusionProbeHistoryIdentity.reset();
            m_ambientOcclusionProbeSourceExecutionReceipt.reset();
            m_ambientOcclusionProbePresented = false;
            m_ambientOcclusionProbeAbandoned = false;
        }
    }

    if (m_toonOutlineProbeSubmissionAccepted &&
        FindPostProcessEffect<ScreenSpaceOutlinePass>() != nullptr &&
        (m_toonOutlineProbePresented || m_toonOutlineProbeAbandoned))
    {
        ScreenSpaceOutlineQualificationProbeStats probeStats;
        const bool completed =
            FindPostProcessEffect<ScreenSpaceOutlinePass>()->PollQualificationProbe(
                tracker, probeStats);
        RenderExecutionReceipt qualified =
            m_toonOutlineProbeSourceExecutionReceipt.has_value()
                ? *m_toonOutlineProbeSourceExecutionReceipt
                : RenderExecutionReceipt{};
        const bool sourceMatches =
            completed && probeStats.IsValid() &&
            m_toonOutlineProbePresented &&
            m_toonOutlineProbeSourceExecutionReceipt.has_value() &&
            qualified.presented &&
            qualified.frameIdentity ==
                probeStats.identity.historyIdentity.previousFrame &&
            qualified.viewIdentity ==
                probeStats.identity.historyIdentity.view &&
            qualified.toonLighting.IsExecutionReady() &&
            qualified.toonOutline.IsExecutionReady() &&
            qualified.toonOutline.qualificationRequested &&
            !qualified.toonOutline.qualificationReady &&
            qualified.toonOutline.settingsHash ==
                probeStats.identity.settingsHash &&
            qualified.toonOutline.materialTableContentHash ==
                probeStats.identity.materialTableContentHash &&
            qualified.toonLighting.materialTableContentHash ==
                probeStats.identity.materialTableContentHash;
        if (sourceMatches)
        {
            qualified.toonOutline.qualificationReady = true;
            qualified.toonOutline.outlinedPixelCount =
                probeStats.outlinedPixelCount;
            qualified.toonOutline.nonToonAffectedPixelCount =
                probeStats.nonToonAffectedPixelCount;
            qualified.toonOutline.resolutionAware =
                probeStats.resolutionAware;
            qualified.toonOutline.renderWidth =
                probeStats.identity.extent.width;
            qualified.toonOutline.renderHeight =
                probeStats.identity.extent.height;
            qualified.toonOutline.diagnostic.clear();
            if (qualified.IsStructurallyValid())
            {
                // A completed physical observation remains valid evidence even
                // when it exposes zero Toon coverage or non-Toon damage.  The
                // Sample qualification contract evaluates those semantic
                // counts without falsifying successful render/present state.
                m_completedRenderExecutionReceipt = qualified;
                m_lastAttemptedRenderExecutionReceipt =
                    std::move(qualified);
            }
            else
            {
                const std::string failure =
                    "Completed Toon-outline probe invalidated its source receipt";
                m_lastAttemptedRenderExecutionReceipt =
                    m_toonOutlineProbeSourceExecutionReceipt.has_value()
                        ? *m_toonOutlineProbeSourceExecutionReceipt
                        : m_completedRenderExecutionReceipt;
                m_lastAttemptedRenderExecutionReceipt.toonOutline.diagnostic =
                    failure;
                m_lastAttemptedRenderExecutionReceipt.diagnostic = failure;
            }
            m_toonOutlineProbeSubmissionAccepted = false;
            m_toonOutlineProbeSourceExecutionReceipt.reset();
            m_toonOutlineProbePresented = false;
            m_toonOutlineProbeAbandoned = false;
        }
        else if (!FindPostProcessEffect<ScreenSpaceOutlinePass>()
                      ->HasPendingQualificationProbe())
        {
            const std::string failure = probeStats.failure.empty()
                ? "Toon-outline qualification probe failed or lost its presented source"
                : probeStats.failure;
            m_lastAttemptedRenderExecutionReceipt =
                m_toonOutlineProbeSourceExecutionReceipt.has_value()
                    ? *m_toonOutlineProbeSourceExecutionReceipt
                    : m_completedRenderExecutionReceipt;
            m_lastAttemptedRenderExecutionReceipt.toonOutline.diagnostic =
                failure;
            m_lastAttemptedRenderExecutionReceipt.diagnostic = failure;
            m_toonOutlineProbeSubmissionAccepted = false;
            m_toonOutlineProbeSourceExecutionReceipt.reset();
            m_toonOutlineProbePresented = false;
            m_toonOutlineProbeAbandoned = false;
        }
    }

    if (m_reflectionProbeSubmissionAccepted &&
        FindCompositionFeature<ReflectionFeatureAdapter>() != nullptr &&
        (m_reflectionProbePresented || m_reflectionProbeAbandoned))
    {
        SSRQualificationProbeStats probeStats;
        const bool completed = FindCompositionFeature<ReflectionFeatureAdapter>()->PollSSRQualificationProbe(
            tracker, probeStats);
        const bool sourceMatches = completed && probeStats.IsValid() &&
            m_reflectionProbePresented &&
            m_hiZReflectionProbeBinding.IsBound() &&
            m_reflectionProbeSourceExecutionReceipt.has_value() &&
            m_reflectionProbeSourceDiagnostics.has_value() &&
            m_reflectionProbeSourceExecutionReceipt->presented &&
            m_reflectionProbeSourceExecutionReceipt->IsStructurallyValid() &&
            m_reflectionProbeSourceExecutionReceipt->frameIdentity ==
                probeStats.identity.frameIdentity &&
            m_reflectionProbeSourceExecutionReceipt->viewIdentity ==
                probeStats.identity.viewIdentity &&
            m_hiZReflectionProbeBinding.frameIdentity ==
                probeStats.identity.frameIdentity &&
            m_hiZReflectionProbeBinding.viewIdentity ==
                probeStats.identity.viewIdentity &&
            m_hiZReflectionProbeBinding.reflectionPlanIdentity ==
                probeStats.identity.reflectionPlanIdentity &&
            m_hiZReflectionProbeBinding.lightingPlanIdentity ==
                probeStats.identity.lightingPlanIdentity &&
            m_hiZReflectionProbeBinding.configurationIdentity ==
                probeStats.identity.configurationIdentity &&
            m_hiZReflectionProbeBinding.pipelineIdentity ==
                probeStats.identity.pipelineIdentity &&
            m_hiZReflectionProbeBinding.depthPyramidIdentity ==
                probeStats.identity.depthPyramidIdentity &&
            m_hiZReflectionProbeBinding.reflectionSurfaceIdentity ==
                probeStats.identity.reflectionSurfaceIdentity &&
            m_hiZReflectionProbeBinding.exposureStateContentHash ==
                probeStats.identity.exposureStateContentHash &&
            m_reflectionProbeSourceDiagnostics->frameSequence ==
                probeStats.identity.frameIdentity.frameSequence &&
            m_reflectionProbeSourceDiagnostics->reflectionPlanIdentity ==
                probeStats.identity.reflectionPlanIdentity &&
            m_reflectionProbeSourceDiagnostics->lightingPlanIdentity ==
                probeStats.identity.lightingPlanIdentity &&
            m_reflectionProbeSourceDiagnostics->configurationIdentity ==
                probeStats.identity.configurationIdentity &&
            m_reflectionProbeSourceDiagnostics->pipelineIdentity ==
                probeStats.identity.pipelineIdentity &&
            m_reflectionProbeSourceDiagnostics->depthPyramidIdentity ==
                probeStats.identity.depthPyramidIdentity &&
            m_reflectionProbeSourceDiagnostics->reflectionSurfaceIdentity ==
                probeStats.identity.reflectionSurfaceIdentity &&
            m_reflectionProbeSourceDiagnostics->exposureStateContentHash ==
                probeStats.identity.exposureStateContentHash &&
            m_reflectionProbeSourceDiagnostics->requested &&
            m_reflectionProbeSourceDiagnostics->supported &&
            m_reflectionProbeSourceDiagnostics->scheduled &&
            m_reflectionProbeSourceDiagnostics->executed &&
            m_reflectionProbeSourceDiagnostics->hiZBuilt &&
            m_reflectionProbeSourceDiagnostics->globalIBLReady &&
            m_reflectionProbeSourceDiagnostics->fallbackChainResolved &&
            probeStats.routeMaskRadianceSchemaVersion == 1 &&
            probeStats.localProbeRadianceQualified &&
            probeStats.globalIBLRadianceQualified &&
            probeStats.sampleCount != 0 &&
            probeStats.screenSpaceHitPixelCount != 0 &&
            probeStats.localProbePixelCount != 0 &&
            probeStats.globalIBLPixelCount != 0 &&
            probeStats.receiverSampleCount ==
                probeStats.screenSpaceHitPixelCount +
                    probeStats.localProbePixelCount +
                    probeStats.globalIBLPixelCount &&
            probeStats.backgroundPixelCount +
                    probeStats.nonReflectivePixelCount +
                    probeStats.receiverSampleCount ==
                probeStats.sampleCount &&
            probeStats.screenSpaceHitPixelCount <=
                probeStats.receiverSampleCount;
        if (sourceMatches)
        {
            RenderReflectionDiagnostics qualified =
                *m_reflectionProbeSourceDiagnostics;
            qualified.available = true;
            qualified.screenSpaceApplied = true;
            qualified.probesApplied = probeStats.localProbePixelCount != 0;
            qualified.reflectionRouteEvidenceReady = true;
            qualified.routeMaskRadianceSchemaVersion =
                probeStats.routeMaskRadianceSchemaVersion;
            qualified.localProbeRadianceQualified =
                probeStats.localProbeRadianceQualified;
            qualified.globalIBLRadianceQualified =
                probeStats.globalIBLRadianceQualified;
            qualified.reflectionRouteSampleCount =
                probeStats.receiverSampleCount;
            qualified.reflectionReadbackSampleCount = probeStats.sampleCount;
            qualified.backgroundPixelCount = probeStats.backgroundPixelCount;
            qualified.nonReflectivePixelCount =
                probeStats.nonReflectivePixelCount;
            qualified.screenSpaceHitPixelCount =
                probeStats.screenSpaceHitPixelCount;
            qualified.localProbePixelCount = probeStats.localProbePixelCount;
            qualified.globalIBLPixelCount = probeStats.globalIBLPixelCount;
            qualified.fallbackReason.clear();

            // Publish an independent source-frame pair. The newest presented
            // receipt/diagnostics remain untouched while this delayed N-frame
            // readback completes after N+1/N+2 have already presented.
            m_completedHiZReflectionQualificationDiagnostics =
                std::move(qualified);
            m_completedHiZReflectionQualificationReceipt =
                *m_reflectionProbeSourceExecutionReceipt;
            m_reflectionProbeSubmissionAccepted = false;
            m_reflectionProbeSourceExecutionReceipt.reset();
            m_reflectionProbeSourceDiagnostics.reset();
            m_hiZReflectionProbeBinding = {};
            m_reflectionProbePresented = false;
            m_reflectionProbeAbandoned = false;
        }
        else if (!FindCompositionFeature<ReflectionFeatureAdapter>()->HasPendingSSRQualificationProbe())
        {
            const std::string failure = probeStats.failure.empty()
                ? "Hi-Z reflection qualification probe failed or lost its presented source"
                : probeStats.failure;
            m_lastAttemptedRenderExecutionReceipt =
                m_reflectionProbeSourceExecutionReceipt.has_value()
                    ? *m_reflectionProbeSourceExecutionReceipt
                    : m_completedRenderExecutionReceipt;
            m_lastAttemptedRenderExecutionReceipt.diagnostic = failure;
            m_reflectionProbeSubmissionAccepted = false;
            m_reflectionProbeSourceExecutionReceipt.reset();
            m_reflectionProbeSourceDiagnostics.reset();
            m_hiZReflectionProbeBinding = {};
            m_reflectionProbePresented = false;
            m_reflectionProbeAbandoned = false;
        }
    }

    // Completion can arrive while the runtime is deliberately progressing no
    // replacement frame.  Project only the post-fence evidence into the
    // already-published source-frame diagnostics; rebuilding the complete
    // snapshot here would incorrectly restamp it with mutable current-frame
    // state.
    m_frameDiagnostics.gpuDrivenCullingStats.gpuSceneDepthQualification =
        m_depthGPUCulling
        ? m_depthGPUCulling->GetGPUSceneQualificationDiagnostics()
        : GPUSceneCullingQualificationDiagnostics{};
    m_frameDiagnostics.gpuDrivenCullingStats.gpuSceneOpaqueQualification =
        m_opaqueGPUCulling
        ? m_opaqueGPUCulling->GetGPUSceneQualificationDiagnostics()
        : GPUSceneCullingQualificationDiagnostics{};
    m_frameDiagnostics.gpuDrivenCullingStats
        .directOpaqueRasterReadbackQualification =
        m_directOpaqueRasterReadbackQualification.GetDiagnostics();
    m_frameDiagnostics.attemptedExecutionReceipt =
        m_lastAttemptedRenderExecutionReceipt;
    m_frameDiagnostics.presentedExecutionReceipt =
        m_completedRenderExecutionReceipt;
    m_frameDiagnostics.hiZReflectionQualificationReceipt =
        m_completedHiZReflectionQualificationReceipt;
    m_frameDiagnostics.hiZReflectionQualification =
        m_completedHiZReflectionQualificationDiagnostics;
    if (m_toolDiagnosticsSnapshot.frameDiagnosticsAvailable)
    {
        m_toolDiagnosticsSnapshot.frame.gpuDrivenCullingStats
            .gpuSceneDepthQualification =
            m_frameDiagnostics.gpuDrivenCullingStats
                .gpuSceneDepthQualification;
        m_toolDiagnosticsSnapshot.frame.gpuDrivenCullingStats
            .gpuSceneOpaqueQualification =
            m_frameDiagnostics.gpuDrivenCullingStats
                .gpuSceneOpaqueQualification;
        m_toolDiagnosticsSnapshot.frame.gpuDrivenCullingStats
            .directOpaqueRasterReadbackQualification =
            m_frameDiagnostics.gpuDrivenCullingStats
                .directOpaqueRasterReadbackQualification;
        m_toolDiagnosticsSnapshot.frame.attemptedExecutionReceipt =
            m_lastAttemptedRenderExecutionReceipt;
        m_toolDiagnosticsSnapshot.frame.presentedExecutionReceipt =
            m_completedRenderExecutionReceipt;
        m_toolDiagnosticsSnapshot.frame.hiZReflectionQualificationReceipt =
            m_completedHiZReflectionQualificationReceipt;
        m_toolDiagnosticsSnapshot.frame.hiZReflectionQualification =
            m_completedHiZReflectionQualificationDiagnostics;
    }
}

void SceneRenderer::PollRenderQualityPerformanceCompletion(
    const RenderSubmissionTracker& tracker,
    const RenderGpuFrameTimingSampleBatch& graphicsTerminalSpans)
{
    // The source-bound RenderContext span is the Graphics prelude-to-terminal
    // command interval. It must bind before polling this session's RenderGraph pass
    // owners, otherwise a completed per-pass sample could be finalized
    // without proof that it belongs to the same terminal submission.
    static_cast<void>(
        m_qualityPerformanceSession.BindGraphicsTerminalCommandSpans(
            graphicsTerminalSpans));
    m_qualityPerformanceSession.PollCompletion(tracker);
}

bool SceneRenderer::HasPendingGPUSceneCullingQualificationCompletion()
    const noexcept
{
    return (m_depthGPUCulling &&
            m_depthGPUCulling->HasPendingGPUSceneQualificationCapture()) ||
        (m_opaqueGPUCulling &&
         m_opaqueGPUCulling->HasPendingGPUSceneQualificationCapture());
}

bool SceneRenderer::HasPendingDirectOpaqueRasterReadbackQualificationCompletion()
    const noexcept
{
    return m_directOpaqueRasterReadbackQualification.HasPendingCompletion();
}

bool SceneRenderer::HasPendingQualificationCaptureCompletion() const noexcept
{
    return m_qualificationCaptureBridge.HasPendingCompletion() ||
        (m_qualificationPassTiming->IsEnabled() &&
         m_qualificationPassTiming->GetDiagnostics().state ==
             RenderGraphPassTimingState::PendingCompletion);
}

bool SceneRenderer::IsQualificationCaptureAwaitingGraphRecord() const noexcept
{
    return m_qualificationCaptureBridge.IsAwaitingGraphRecord();
}

const QualificationCaptureDiagnostics&
SceneRenderer::GetQualificationCaptureDiagnostics() const noexcept
{
    return m_qualificationCaptureBridge.GetDiagnostics();
}

std::shared_ptr<const QualificationCaptureBundle>
SceneRenderer::GetQualificationCaptureBundle() const noexcept
{
    return m_qualificationCaptureBridge.GetCompletedBundle();
}

const RenderGraphPassTimingDiagnostics&
SceneRenderer::GetQualificationPassTimingDiagnostics() const noexcept
{
    return m_qualificationPassTiming->GetDiagnostics();
}

std::span<const RenderGraphPassTimingSample>
SceneRenderer::GetQualificationPassTimingSamples() const noexcept
{
    return m_qualificationPassTiming->GetCompletedSamples();
}

bool SceneRenderer::HasPendingRenderQualityPerformanceCompletion()
    const noexcept
{
    return m_qualityPerformanceSession.HasPendingCompletion();
}

bool SceneRenderer::IsRenderQualityPerformanceAwaitingFrame() const noexcept
{
    return m_qualityPerformanceSession.RequiresExactGraphicsFrame();
}

const RenderQualityPerformanceSessionDiagnostics&
SceneRenderer::GetRenderQualityPerformanceDiagnostics() const noexcept
{
    return m_qualityPerformanceSession.GetDiagnostics();
}

std::shared_ptr<const RenderQualityPerformanceEvidence>
SceneRenderer::GetRenderQualityPerformanceEvidence() const noexcept
{
    return m_qualityPerformanceSession.GetCompletedEvidence();
}

bool SceneRenderer::HasPendingExposureQualificationCompletion() const noexcept
{
    return m_exposureProbeCompletion != nullptr;
}

bool SceneRenderer::HasPendingAmbientOcclusionQualificationCompletion()
    const noexcept
{
    return m_ambientOcclusionProbeSubmissionAccepted;
}

bool SceneRenderer::HasPendingToonOutlineQualificationCompletion()
    const noexcept
{
    return m_toonOutlineProbeSubmissionAccepted;
}

bool SceneRenderer::HasPendingHiZReflectionQualificationCompletion()
    const noexcept
{
    return m_reflectionProbeSubmissionAccepted;
}

bool SceneRenderer::NotifySubmission(const GPUCompletionToken& completion)
{
    RenderSubmissionTracker* const tracker = m_recordingSubmissionTracker;
    if (tracker == nullptr || !m_hasRecordingSubmissionBoundary ||
        !IsExactPostRecordingGraphicsSubmission(
            *tracker, m_recordingGraphicsSubmissionBaseline, completion))
    {
        // Do not mutate any completion-owned component or transfer the batch.
        // A caller may have supplied P0 after the graph reached the GPU at P1;
        // keeping the whole recording intact permits the exact P1 retry.
        return false;
    }

    bool submissionSemanticsAccepted = true;
    if (!m_qualityPerformanceSession.NotifyGraphicsSubmission(completion))
    {
        // GFX24 performance evidence is completion-qualified independently
        // from physical submission ownership. Keep the already-submitted
        // presentation lifecycle intact while the session records its first
        // exact admission failure.
        RVX_RENDER_WARN(
            "SceneRenderer: GFX24 performance session rejected the submitted Graphics completion value");
    }
    if (m_rayTracingSceneStats.prepared &&
        !m_rayTracingSceneRecordingToken.IsValid())
    {
        // The command stream is already submitted. Reject its qualification,
        // but continue transferring every owner so the acquired presentation
        // image can complete its physical lifecycle.
        RVX_RENDER_ERROR(
            "SceneRenderer: prepared ray-tracing scene has no exact graph recording token");
        submissionSemanticsAccepted = false;
    }

    if (m_gpuSceneUploader && !m_gpuSceneUploader->NotifySubmission(completion))
    {
        RVX_RENDER_ERROR(
            "SceneRenderer: GPU-scene upload ownership rejected exact submission evidence");
        submissionSemanticsAccepted = false;
    }
    bool rayTracingSceneSubmissionAccepted =
        !m_hybridRayTracingFramePlan.has_value();
    if (m_rayTracingSceneManager != nullptr &&
        m_rayTracingSceneRecordingToken.IsValid())
    {
        const bool sceneSubmissionAccepted =
            m_rayTracingSceneManager->NotifySubmission(
                m_rayTracingSceneRecordingToken, completion, *tracker);
        if (!sceneSubmissionAccepted)
        {
            RVX_RENDER_ERROR(
                "SceneRenderer: ray-tracing scene rejected exact submission evidence");
            submissionSemanticsAccepted = false;
        }
        if (m_hybridRayTracingFramePlan.has_value())
        {
            const RayTracingSceneExecutionReceipt& sceneReceipt =
                m_rayTracingSceneManager->GetExecutionReceipt();
            rayTracingSceneSubmissionAccepted = sceneSubmissionAccepted &&
                sceneReceipt.submitted &&
                sceneReceipt.preparationGeneration ==
                    m_hybridRayTracingFramePlan
                        ->GetRayTracingScenePreparationGeneration() &&
                sceneReceipt.structuralSignature.hash ==
                    m_hybridRayTracingFramePlan
                        ->GetRayTracingSceneStructuralHash();
            if (!rayTracingSceneSubmissionAccepted)
            {
                RVX_RENDER_ERROR(
                    "SceneRenderer: submitted ray-tracing scene did not match the frozen HybridRayTracing plan");
                submissionSemanticsAccepted = false;
            }
        }
        m_rayTracingSceneRecordingToken = {};
        m_rayTracingSceneStats = m_rayTracingSceneManager->GetStats();
    }
    const auto notifyGPUSceneQualification =
        [&completion, tracker](GPUCulling* owner,
                                const std::shared_ptr<GPUCullingRecordedState>& recorded)
    {
        if (owner == nullptr || !recorded)
        {
            return;
        }
        if (!recorded->NotifyGPUSceneQualificationSubmission(
                *owner, completion, *tracker))
        {
            // The frame is already submitted and its normal ownership is
            // valid. Preserve that work; qualification diagnostics fail
            // closed instead of retroactively releasing in-flight resources.
            RVX_RENDER_WARN(
                "SceneRenderer: GPU-scene culling qualification completion evidence was rejected");
        }
    };
    notifyGPUSceneQualification(
        m_depthGPUCulling.get(), m_depthGPUCullingRecordedState);
    notifyGPUSceneQualification(
        m_opaqueGPUCulling.get(), m_opaqueGPUCullingRecordedState);
    const DirectRasterReadbackRecordingIdentity directQualificationIdentity{
        m_activeRenderPassIdentity.graphIdentity,
        m_activeRenderPassIdentity.graphRecordingGeneration,
        m_activeRenderPassIdentity.frameSequence,
        m_activeRenderPassIdentity.recordEpoch,
        m_directOpaqueRasterReadbackSourceFrameSlot};
    // An armed request which did not reach OpaquePass must be consumed at the
    // target frame boundary.  It is deliberately not allowed to bleed into a
    // later frame that happens to have a Direct lane.
    if (m_directOpaqueRasterReadbackQualification.IsArmed())
    {
        m_directOpaqueRasterReadbackQualification.RejectNoDirectLane(
            directQualificationIdentity);
    }
    if (!m_directOpaqueRasterReadbackQualification.NotifySubmission(
            directQualificationIdentity, completion, *tracker))
    {
        RVX_RENDER_WARN(
            "SceneRenderer: Direct Opaque readback qualification completion evidence was rejected");
    }
    if (m_qualificationCaptureBridge.GetDiagnostics().state ==
        QualificationCaptureState::Recorded)
    {
        if (!m_qualificationCaptureBridge.NotifySubmission(completion, *tracker))
        {
            // The normal frame has an exact owner boundary and must still be
            // presented. The optional qualification bundle alone fails closed.
            RVX_RENDER_WARN(
                "SceneRenderer: RenderQuality qualification capture rejected exact submission evidence");
        }
    }
    if (GetPostProcessStack() != nullptr)
    {
        const ExposureControllerStats& exposureStats =
            GetPostProcessStack()->GetExposureController().GetLastStats();
        if (exposureStats.qualificationProbeRequested)
        {
            if (!exposureStats.qualificationProbeRecorded ||
                !m_activeViewHistoryCandidate.has_value() ||
                exposureStats.historyIdentity !=
                    *m_activeViewHistoryCandidate ||
                m_exposureProbeCompletion)
            {
                GetPostProcessStack()->GetExposureController()
                    .CancelQualificationProbe();
                RVX_RENDER_WARN(
                    "SceneRenderer: exposure qualification probe did not bind exact submission evidence");
            }
            else
            {
                m_exposureProbeCompletion =
                    std::make_shared<GPUCompletionToken>(completion);
                m_exposureProbeHistoryIdentity =
                    *m_activeViewHistoryCandidate;
                m_exposureProbePresented = false;
            }
        }
    }
    if (FindCompositionFeature<AmbientOcclusionFeatureAdapter>() != nullptr &&
        FindCompositionFeature<AmbientOcclusionFeatureAdapter>()->RequiresHistoryCommit())
    {
        const bool historySubmissionAccepted =
            m_activeViewHistoryCandidate.has_value() &&
            FindCompositionFeature<AmbientOcclusionFeatureAdapter>()->NotifyHistorySubmission(
                *m_activeViewHistoryCandidate);
        if (!historySubmissionAccepted)
        {
            RVX_RENDER_ERROR(
                "SceneRenderer: ambient-occlusion history did not bind the exact submitted view");
        }
    }
    if (FindCompositionFeature<VolumetricLightingFeatureAdapter>() != nullptr &&
        FindCompositionFeature<VolumetricLightingFeatureAdapter>()->RequiresHistoryCommit())
    {
        const bool historySubmissionAccepted =
            m_activeViewHistoryCandidate.has_value() &&
            FindCompositionFeature<VolumetricLightingFeatureAdapter>()->NotifyHistorySubmission(
                *m_activeViewHistoryCandidate);
        if (!historySubmissionAccepted)
        {
            RVX_RENDER_ERROR(
                "SceneRenderer: volumetric-lighting history did not bind the exact submitted view");
            submissionSemanticsAccepted = false;
        }
    }
    if (FindCompositionFeature<AmbientOcclusionFeatureAdapter>() != nullptr &&
        FindCompositionFeature<AmbientOcclusionFeatureAdapter>()->HasPendingQualificationProbe())
    {
        const bool exactIdentityAvailable =
            m_activeViewHistoryCandidate.has_value();
        const bool accepted = exactIdentityAvailable &&
            !m_ambientOcclusionProbeSubmissionAccepted &&
            FindCompositionFeature<AmbientOcclusionFeatureAdapter>()->NotifyQualificationProbeSubmission(
                *m_activeViewHistoryCandidate, completion, *tracker);
        if (!accepted)
        {
            FindCompositionFeature<AmbientOcclusionFeatureAdapter>()->AbandonQualificationProbe();
            RVX_RENDER_WARN(
                "SceneRenderer: ambient-occlusion qualification probe did not bind exact submission evidence");
        }
        else
        {
            m_ambientOcclusionProbeHistoryIdentity =
                *m_activeViewHistoryCandidate;
            m_ambientOcclusionProbeSourceExecutionReceipt.reset();
            m_ambientOcclusionProbeSubmissionAccepted = true;
            m_ambientOcclusionProbePresented = false;
            m_ambientOcclusionProbeAbandoned = false;
        }
    }
    if (FindPostProcessEffect<ScreenSpaceOutlinePass>() != nullptr &&
        FindPostProcessEffect<ScreenSpaceOutlinePass>()->HasPendingQualificationProbe())
    {
        const auto pendingIdentity =
            FindPostProcessEffect<ScreenSpaceOutlinePass>()
                ->GetPendingQualificationProbeIdentity();
        const bool exactIdentityAvailable =
            pendingIdentity.has_value() &&
            m_activeViewHistoryCandidate.has_value() &&
            pendingIdentity->historyIdentity ==
                *m_activeViewHistoryCandidate &&
            pendingIdentity->frameIndex == m_viewData.frameNumber;
        const bool accepted = exactIdentityAvailable &&
            !m_toonOutlineProbeSubmissionAccepted &&
            FindPostProcessEffect<ScreenSpaceOutlinePass>()
                ->NotifyQualificationProbeSubmission(
                    *pendingIdentity, completion, *tracker);
        if (!accepted)
        {
            FindPostProcessEffect<ScreenSpaceOutlinePass>()->AbandonQualificationProbe();
            m_toonOutlineProbeSubmissionAccepted = false;
            m_toonOutlineProbePresented = false;
            m_toonOutlineProbeAbandoned = true;
            RVX_RENDER_WARN(
                "SceneRenderer: Toon-outline qualification probe did not bind exact submission evidence");
        }
        else
        {
            m_toonOutlineProbeSourceExecutionReceipt.reset();
            m_toonOutlineProbeSubmissionAccepted = true;
            m_toonOutlineProbePresented = false;
            m_toonOutlineProbeAbandoned = false;
        }
    }
    if (FindCompositionFeature<ReflectionFeatureAdapter>() != nullptr &&
        FindCompositionFeature<ReflectionFeatureAdapter>()->HasPendingSSRQualificationProbe())
    {
        const auto pendingIdentity =
            FindCompositionFeature<ReflectionFeatureAdapter>()->GetPendingSSRQualificationProbeIdentity();
        const bool exactIdentityAvailable = pendingIdentity.has_value() &&
            pendingIdentity->IsValid() &&
            m_hiZReflectionProbeBinding.generation != 0 &&
            m_pendingRenderExecutionReceipt.has_value() &&
            m_pendingReflectionDiagnostics.has_value() &&
            m_pendingRenderExecutionReceipt->frameIdentity ==
                pendingIdentity->frameIdentity &&
            m_pendingRenderExecutionReceipt->viewIdentity ==
                pendingIdentity->viewIdentity &&
            m_pendingReflectionDiagnostics->frameSequence ==
                pendingIdentity->frameIdentity.frameSequence &&
            m_pendingReflectionDiagnostics->reflectionPlanIdentity ==
                pendingIdentity->reflectionPlanIdentity &&
            m_pendingReflectionDiagnostics->lightingPlanIdentity ==
                pendingIdentity->lightingPlanIdentity &&
            m_pendingReflectionDiagnostics->configurationIdentity ==
                pendingIdentity->configurationIdentity &&
            m_pendingReflectionDiagnostics->pipelineIdentity ==
                pendingIdentity->pipelineIdentity &&
            m_pendingReflectionDiagnostics->depthPyramidIdentity ==
                pendingIdentity->depthPyramidIdentity;
        const bool accepted = exactIdentityAvailable &&
            !m_reflectionProbeSubmissionAccepted &&
            FindCompositionFeature<ReflectionFeatureAdapter>()->NotifySSRQualificationProbeSubmission(
                *pendingIdentity, completion, *tracker);
        if (!accepted)
        {
            FindCompositionFeature<ReflectionFeatureAdapter>()->AbandonSSRQualificationProbe();
            m_hiZReflectionProbeBinding = {};
            m_reflectionProbeSubmissionAccepted = false;
            m_reflectionProbePresented = false;
            m_reflectionProbeAbandoned = true;
            RVX_RENDER_WARN(
                "SceneRenderer: Hi-Z reflection qualification probe did not bind exact submission evidence");
        }
        else
        {
            m_reflectionProbeSourceExecutionReceipt =
                *m_pendingRenderExecutionReceipt;
            m_reflectionProbeSourceDiagnostics =
                *m_pendingReflectionDiagnostics;
            m_hiZReflectionProbeBinding.frameIdentity =
                pendingIdentity->frameIdentity;
            m_hiZReflectionProbeBinding.viewIdentity =
                pendingIdentity->viewIdentity;
            m_hiZReflectionProbeBinding.reflectionPlanIdentity =
                pendingIdentity->reflectionPlanIdentity;
            m_hiZReflectionProbeBinding.lightingPlanIdentity =
                pendingIdentity->lightingPlanIdentity;
            m_hiZReflectionProbeBinding.configurationIdentity =
                pendingIdentity->configurationIdentity;
            m_hiZReflectionProbeBinding.pipelineIdentity =
                pendingIdentity->pipelineIdentity;
            m_hiZReflectionProbeBinding.depthPyramidIdentity =
                pendingIdentity->depthPyramidIdentity;
            m_hiZReflectionProbeBinding.reflectionSurfaceIdentity =
                pendingIdentity->reflectionSurfaceIdentity;
            m_hiZReflectionProbeBinding.exposureStateContentHash =
                pendingIdentity->exposureStateContentHash;
            m_reflectionProbeSubmissionAccepted = true;
            m_reflectionProbePresented = false;
            m_reflectionProbeAbandoned = false;
        }
    }
    if (m_pipelineCache && !m_pipelineCache->NotifyObjectConstantSubmission(completion))
    {
        RVX_RENDER_ERROR("SceneRenderer: object constant pages rejected invalid completion evidence");
    }
    if (m_materialSystem && !m_materialSystem->NotifyMaterialConstantSubmission(completion))
    {
        RVX_RENDER_ERROR("SceneRenderer: material constant pages rejected invalid completion evidence");
    }
    if (m_gpuSceneUploader)
    {
        ReclaimGPUSceneRetiredRows();
    }
    if (FindCompositionPass<RayTracedShadowPass>())
    {
        FindCompositionPass<RayTracedShadowPass>()->NotifySubmission(
            m_activeRenderPassIdentity, completion);
    }
    bool rayTracedReflectionHistorySubmissionAccepted =
        !m_hybridRayTracingFramePlan.has_value();
    if (FindCompositionPass<RayTracedReflectionPass>())
    {
        const bool hybridReflectionRecorded =
            m_hybridRayTracingFramePlan.has_value() &&
            m_hybridRayTracingFramePlan->GetReflectionRoute() ==
                HybridRayTracingReflectionRoute::RayTraced;
        if (hybridReflectionRecorded)
        {
            rayTracedReflectionHistorySubmissionAccepted =
                m_activeViewHistoryCandidate.has_value() &&
                FindCompositionPass<RayTracedReflectionPass>()->NotifySubmission(
                    m_activeRenderPassIdentity,
                    *m_activeViewHistoryCandidate,
                    completion);
            if (!rayTracedReflectionHistorySubmissionAccepted)
            {
                RVX_RENDER_ERROR(
                    "SceneRenderer: ray-traced reflection history did not bind the exact submitted view");
                submissionSemanticsAccepted = false;
            }
        }
        else
        {
            FindCompositionPass<RayTracedReflectionPass>()->NotifySubmission(
                m_activeRenderPassIdentity, completion);
        }
    }
    if (m_hybridRayTracingFramePlan.has_value())
    {
        const RenderHybridRayTracingExecutionReceipt* frozenHybrid = nullptr;
        if (m_pendingRenderExecutionReceipt.has_value())
        {
            const RenderExecutionReceipt& pending =
                *m_pendingRenderExecutionReceipt;
            const RenderHybridRayTracingExecutionReceipt& candidate =
                pending.hybridRayTracing;
            if (pending.executionSucceeded && candidate.available &&
                candidate.planIdentityHash ==
                    m_hybridRayTracingFramePlan->GetIdentityHash() &&
                candidate.frameIdentity ==
                    m_hybridRayTracingFramePlan->GetFrameIdentity() &&
                candidate.viewIdentity ==
                    m_hybridRayTracingFramePlan->GetViewIdentity())
            {
                frozenHybrid = &candidate;
            }
        }
        const bool frozenGraphExecutionAccepted =
            frozenHybrid != nullptr && frozenHybrid->IsGraphExecutionReady();
        m_pendingHybridRayTracingObservation.graphExecutionSucceeded =
            frozenGraphExecutionAccepted;
        m_pendingHybridRayTracingObservation.sceneBuildAccepted =
            rayTracingSceneSubmissionAccepted;
        m_pendingHybridRayTracingObservation.shadowDispatchRecorded =
            frozenHybrid != nullptr &&
            frozenHybrid->shadowDispatchRecorded;
        m_pendingHybridRayTracingObservation.reflectionDispatchRecorded =
            frozenHybrid != nullptr &&
            frozenHybrid->reflectionDispatchRecorded;
        m_pendingHybridRayTracingObservation
            .reflectionSpatialDenoiseRecorded =
                frozenHybrid != nullptr &&
                frozenHybrid->spatialDenoiseRecorded;
        m_pendingHybridRayTracingObservation
            .reflectionTemporalDenoiseRecorded =
                frozenHybrid != nullptr &&
                frozenHybrid->temporalResolveRecorded;
        m_pendingHybridRayTracingObservation.reflectionCompositeRecorded =
            frozenHybrid != nullptr && frozenHybrid->compositeRecorded;
        m_pendingHybridRayTracingObservation
            .reflectionCompositeUsedDenoisedSource =
                frozenHybrid != nullptr &&
                frozenHybrid->compositeUsedDenoisedSource;
        m_pendingHybridRayTracingObservation.submissionAccepted =
            submissionSemanticsAccepted &&
            rayTracedReflectionHistorySubmissionAccepted;
        m_hybridRayTracingExecutionReceipt =
            HybridRayTracingExecutionReceipt::FromPlan(
                *m_hybridRayTracingFramePlan,
                m_pendingHybridRayTracingObservation);
        const bool hybridSubmissionAccepted =
            m_hybridRayTracingExecutionReceipt->IsExecutionSuccessfulFor(
                *m_hybridRayTracingFramePlan);
        if (!hybridSubmissionAccepted)
        {
            RVX_RENDER_ERROR(
                "SceneRenderer: required HybridRayTracing execution evidence is incomplete");
            submissionSemanticsAccepted = false;
        }
        if (m_pendingRenderExecutionReceipt.has_value())
        {
            RenderHybridRayTracingExecutionReceipt& hybrid =
                m_pendingRenderExecutionReceipt->hybridRayTracing;
            hybrid.sceneBuildAccepted = rayTracingSceneSubmissionAccepted;
            hybrid.submissionAccepted = hybridSubmissionAccepted;
            hybrid.presentationQualified = false;
            hybrid.succeeded = false;
            if (!hybrid.IsStructurallyValid())
            {
                RVX_RENDER_ERROR(
                    "SceneRenderer: public HybridRayTracing submission receipt is structurally invalid");
                submissionSemanticsAccepted = false;
            }
        }
    }
    RetireOwnerSnapshots(completion);
    if (m_resourceViewCache)
    {
        m_resourceViewCache->NotifySubmission(completion);
    }
    if (m_submissionBatch)
    {
        RVX_ASSERT_MSG(m_retirementQueue != nullptr,
                       "Submission ownership requires retirement queue");
        m_submissionBatch->SealAndTransfer(completion, *m_retirementQueue);
        m_submissionBatch.reset();
    }
    // RenderSubsystem calls this only after EndFrame produced the exact
    // graphics completion point. The provisional RenderGraph access state now
    // describes submitted GPU work and must survive into the next frame.
    ConfirmProvisionalFrameAccessSnapshots();
    m_submissionQualifiedFrameDiagnostics = true;
    m_hasRecordingSubmissionBoundary = false;
    m_recordingGraphicsSubmissionBaseline = 0;
    m_recordingSubmissionTracker = nullptr;
    m_directOpaqueRasterReadbackSourceFrameSlot = RVX_INVALID_INDEX;
    RefreshSubmissionQualifiedFrameDiagnostics();
    if (!submissionSemanticsAccepted &&
        m_pendingRenderExecutionReceipt.has_value())
    {
        RenderExecutionReceipt& rejected =
            *m_pendingRenderExecutionReceipt;
        rejected.executionSucceeded = false;
        rejected.presented = false;
        rejected.succeeded = false;
        if (rejected.diagnostic.empty())
        {
            rejected.diagnostic =
                "Submitted render work failed exact semantic qualification";
        }
        m_lastAttemptedRenderExecutionReceipt = rejected;
    }
    // The exact outer boundary was accepted and all ordinary submission
    // owners have now transferred. Semantic qualification is deliberately
    // separate: a submitted swapchain image must still reach Present.
    return true;
}

bool SceneRenderer::IsPendingSubmissionSemanticallyQualified() const noexcept
{
    if (!m_pendingRenderExecutionReceipt.has_value())
    {
        return false;
    }

    const RenderExecutionReceipt& receipt =
        *m_pendingRenderExecutionReceipt;
    if (!receipt.executionSucceeded || !receipt.IsStructurallyValid())
    {
        return false;
    }
    if (receipt.resolvedRecipe == RenderPipelineRecipe::HybridRayTracing)
    {
        return receipt.hybridRayTracing.IsSubmissionReady();
    }
    return true;
}

void SceneRenderer::ReleaseUnsubmittedFrame()
{
    // This path covers failed graph recording, failed submission, and frames
    // discarded before recording begins. None reached Present, so their
    // compatibility candidate must not advance committed temporal history.
    AbandonAcceptedFrameHistory();
    if (FindCompositionFeature<AmbientOcclusionFeatureAdapter>() != nullptr &&
        !m_ambientOcclusionProbeSubmissionAccepted)
    {
        // A one-shot request may be armed before frame consumption. Preserve
        // it across ApplyFrameV5, but explicitly cancel it once that frame is
        // known not to have reached a real submission boundary.
        FindCompositionFeature<AmbientOcclusionFeatureAdapter>()->AbandonQualificationProbe();
        m_ambientOcclusionProbeHistoryIdentity.reset();
        m_ambientOcclusionProbeSourceExecutionReceipt.reset();
        m_ambientOcclusionProbePresented = false;
        m_ambientOcclusionProbeAbandoned = false;
    }
    if (FindPostProcessEffect<ScreenSpaceOutlinePass>() != nullptr &&
        !m_toonOutlineProbeSubmissionAccepted &&
        FindPostProcessEffect<ScreenSpaceOutlinePass>()->HasPendingQualificationProbe())
    {
        // Match the AO lifecycle above: accepting a frame must preserve a
        // Render-owner-armed request until that frame can record it.  Once
        // the frame is known not to have reached submission, however, the
        // one-shot request must fail closed instead of leaking to a later
        // frame.
        FindPostProcessEffect<ScreenSpaceOutlinePass>()->AbandonQualificationProbe();
        m_toonOutlineProbeSourceExecutionReceipt.reset();
        m_toonOutlineProbePresented = false;
        m_toonOutlineProbeAbandoned = false;
    }
    if (FindCompositionFeature<ReflectionFeatureAdapter>() != nullptr &&
        !m_reflectionProbeSubmissionAccepted &&
        FindCompositionFeature<ReflectionFeatureAdapter>()->HasPendingSSRQualificationProbe())
    {
        // A render-owner request is armed before ApplyFrameV5 so it can bind
        // the exact admitted frame.  Accepting that frame must preserve the
        // still-unbound request, while a frame that never reaches submission
        // must cancel it rather than allowing it to attach to a later frame.
        FindCompositionFeature<ReflectionFeatureAdapter>()->AbandonSSRQualificationProbe();
        m_reflectionProbeSourceExecutionReceipt.reset();
        m_reflectionProbeSourceDiagnostics.reset();
        m_hiZReflectionProbeBinding = {};
        m_reflectionProbePresented = false;
        m_reflectionProbeAbandoned = true;
    }
    const bool preservePresentedExposureProbe =
        m_exposureProbeCompletion && m_exposureProbePresented &&
        m_exposureProbeSourceExecutionReceipt.has_value();
    if (!preservePresentedExposureProbe && GetPostProcessStack())
    {
        GetPostProcessStack()->GetExposureController()
            .CancelQualificationProbe();
    }
    if (!preservePresentedExposureProbe)
    {
        m_exposureProbeCompletion.reset();
        m_exposureProbeHistoryIdentity.reset();
        m_exposureProbeSourceExecutionReceipt.reset();
        m_exposureProbePresented = false;
    }
    if (m_pendingRenderExecutionReceipt.has_value())
    {
        // Abandoning a graph-backed TAA candidate changes its immutable
        // lifecycle evidence. Copy that post-process receipt before exposing
        // this failed attempt, otherwise diagnostics could falsely claim an
        // unpresented candidate was still pending.
        if (FindCompositionFeature<PostProcessPipelineFeatureAdapter>() != nullptr)
        {
            m_pendingRenderExecutionReceipt->postProcess =
                FindCompositionFeature<PostProcessPipelineFeatureAdapter>()->GetLastExecutionReceipt();
        }
        if (FindCompositionFeature<AmbientOcclusionFeatureAdapter>() != nullptr)
        {
            RenderAmbientOcclusionExecutionReceipt ambientOcclusion =
                FindCompositionFeature<AmbientOcclusionFeatureAdapter>()->GetLastExecutionReceipt();
            ambientOcclusion.lightingConsumerExecuted =
                m_pendingRenderExecutionReceipt->ambientOcclusion
                    .lightingConsumerExecuted;
            ambientOcclusion.lightingAOFieldContentHash =
                m_pendingRenderExecutionReceipt->ambientOcclusion
                    .lightingAOFieldContentHash;
            m_pendingRenderExecutionReceipt->ambientOcclusion =
                std::move(ambientOcclusion);
        }
        RenderExecutionReceipt rejected =
            *m_pendingRenderExecutionReceipt;
        rejected.presented = false;
        rejected.succeeded = false;
        rejected.diagnostic =
            "Render execution did not reach successful submission and presentation";
        m_lastAttemptedRenderExecutionReceipt = std::move(rejected);
        m_pendingRenderExecutionReceipt.reset();
        m_frameDiagnostics.attemptedExecutionReceipt =
            m_lastAttemptedRenderExecutionReceipt;
        m_toolDiagnosticsSnapshot.frame.attemptedExecutionReceipt =
            m_lastAttemptedRenderExecutionReceipt;
    }
    const bool hadUnsubmittedRecording = m_submissionBatch != nullptr ||
        m_hasRecordingSubmissionBoundary;
    m_submissionQualifiedFrameDiagnostics = false;
    m_hasRecordingSubmissionBoundary = false;
    m_recordingGraphicsSubmissionBaseline = 0;
    m_recordingSubmissionTracker = nullptr;
    const DirectRasterReadbackRecordingIdentity directQualificationIdentity{
        m_activeRenderPassIdentity.graphIdentity,
        m_activeRenderPassIdentity.graphRecordingGeneration,
        m_activeRenderPassIdentity.frameSequence,
        m_activeRenderPassIdentity.recordEpoch,
        m_directOpaqueRasterReadbackSourceFrameSlot};
    if (m_directOpaqueRasterReadbackQualification.IsArmed())
    {
        m_directOpaqueRasterReadbackQualification.RejectNoDirectLane(
            directQualificationIdentity);
    }
    m_directOpaqueRasterReadbackQualification.ReleaseUnsubmitted(
        directQualificationIdentity);
    m_qualificationCaptureBridge.RejectPending();
    const RenderGraphPassTimingState timingState =
        m_qualificationPassTiming->GetDiagnostics().state;
    if (timingState == RenderGraphPassTimingState::Recording ||
        timingState == RenderGraphPassTimingState::AwaitingSubmission)
    {
        m_qualificationPassTiming->AbortExactGraphicsExecution(
            "Frame did not reach an exact graphics submission.");
    }
    m_qualityPerformanceSession.AbortCurrentFrame(
        RenderQualityPerformanceFailure::GraphRecordingFailed,
        "Sampled performance frame did not reach exact graphics submission.");
    m_directOpaqueRasterReadbackSourceFrameSlot = RVX_INVALID_INDEX;
    // RenderGraph execution may have realized final depth/back-buffer states,
    // but those states are not persistent until EndFrame submits the frame.
    // Restore the import snapshots first so a failed recording or zero
    // submission cannot poison the next frame's barrier planning.
    RestoreProvisionalFrameAccessSnapshots();
    if (m_pipelineCache)
    {
        m_pipelineCache->ReleaseUnsubmittedObjectConstants();
    }
    if (m_materialSystem)
    {
        m_materialSystem->ReleaseUnsubmittedMaterialConstants();
    }
    if (m_gpuSceneUploader)
    {
        m_gpuSceneUploader->ReleaseUnsubmittedFrame();
    }
    if (FindCompositionPass<RayTracedShadowPass>())
    {
        FindCompositionPass<RayTracedShadowPass>()->ReleaseUnsubmittedFrame(
            m_activeRenderPassIdentity);
    }
    if (FindCompositionPass<RayTracedReflectionPass>())
    {
        FindCompositionPass<RayTracedReflectionPass>()->ReleaseUnsubmittedFrame(
            m_activeRenderPassIdentity);
    }
    if (m_rayTracingSceneManager != nullptr &&
        m_rayTracingSceneRecordingToken.IsValid())
    {
        m_rayTracingSceneManager->ReleaseUnsubmitted(
            m_rayTracingSceneRecordingToken);
        m_rayTracingSceneRecordingToken = {};
        m_rayTracingSceneStats = m_rayTracingSceneManager->GetStats();
    }
    if (m_hybridRayTracingFramePlan.has_value())
    {
        HybridRayTracingExecutionObservation abandoned;
        abandoned.diagnostic =
            "HybridRayTracing recording was abandoned before submission";
        m_hybridRayTracingExecutionReceipt =
            HybridRayTracingExecutionReceipt::FromPlan(
                *m_hybridRayTracingFramePlan, std::move(abandoned));
    }
    if (m_renderContext)
    {
        if (RenderSubmissionTracker* tracker =
                RenderContextInternalAccess::GetSubmissionTracker(
                    *m_renderContext))
        {
            RetireOwnerSnapshots(tracker->CaptureLastSubmittedToken());
        }
    }
    const GPUCompletionToken emptyCompletion;
    if (m_resourceViewCache)
    {
        m_resourceViewCache->NotifySubmission(emptyCompletion);
    }
    if (m_submissionBatch)
    {
        RVX_ASSERT_MSG(m_retirementQueue != nullptr,
                       "Unsubmitted ownership requires retirement queue");
        m_submissionBatch->ReleaseUnsubmitted(*m_retirementQueue);
        m_submissionBatch.reset();
    }
    // Pass record results are graph-owned.  Leaving this identity live after a
    // zero/failed submission would expose direct-stream receipt evidence as a
    // completed frame and let it bleed into the next diagnostic projection.
    m_activeRenderPassResults.reset();
    m_activeRenderDeformationResources.reset();
    m_activeRenderPassIdentity = {};
    if (hadUnsubmittedRecording)
    {
        RefreshFrameDiagnostics(true,
                                false,
                                m_frameDiagnostics.graphBuilt,
                                m_frameDiagnostics.graphCompiled,
                                "Frame recording was not submitted");
    }
}

void SceneRenderer::PublishProvisionalFrameAccessSnapshots(
    const RHITextureAccessSnapshot* depthAccess,
    const RHITextureAccessSnapshot* backBufferAccess) noexcept
{
    RVX_ASSERT_MSG(!m_frameAccessSnapshotRollback.pending,
                   "Previous frame access snapshots were not resolved");

    if (depthAccess != nullptr)
    {
        m_frameAccessSnapshotRollback.depthAccess = m_depthAccessSnapshot;
        m_frameAccessSnapshotRollback.restoreDepth = true;
        m_frameAccessSnapshotRollback.pending = true;
        m_depthAccessSnapshot = *depthAccess;
    }

    if (backBufferAccess != nullptr &&
        m_activeBackBufferIndex < m_backBufferAccessSnapshots.size())
    {
        m_frameAccessSnapshotRollback.backBufferIndex =
            m_activeBackBufferIndex;
        m_frameAccessSnapshotRollback.backBufferAccess =
            m_backBufferAccessSnapshots[m_activeBackBufferIndex];
        m_frameAccessSnapshotRollback.pending = true;
        m_backBufferAccessSnapshots[m_activeBackBufferIndex] =
            *backBufferAccess;
    }

    const auto publishExternalTexture =
        [this](const RenderGraphExternalTextureAccess& access)
    {
        if (!access.resource.IsValid() ||
            !access.graphTexture.IsValid() ||
            m_renderResourceRegistry == nullptr ||
            m_renderGraph == nullptr)
        {
            return;
        }
        const auto duplicate = std::find_if(
            m_frameAccessSnapshotRollback.resourceTextures.begin(),
            m_frameAccessSnapshotRollback.resourceTextures.end(),
            [&access](
                const FrameAccessSnapshotRollback::ResourceTexture& rollback)
            {
                return rollback.resource == access.resource;
            });
        if (duplicate !=
            m_frameAccessSnapshotRollback.resourceTextures.end())
        {
            return;
        }
        const RenderTextureResourceData* texture =
            m_renderResourceRegistry->ResolveTexture(access.resource);
        if (texture == nullptr || !texture->texture)
        {
            return;
        }
        const RHITextureAccessSnapshot realized =
            m_renderGraph->GetRealizedAccess(access.graphTexture);
        m_frameAccessSnapshotRollback.resourceTextures.push_back(
            {access.resource, texture->accessSnapshot});
        if (!m_renderResourceRegistry->CommitTextureAccessSnapshot(
                access.resource, realized))
        {
            m_frameAccessSnapshotRollback.resourceTextures.pop_back();
            RVX_RENDER_ERROR(
                "SceneRenderer: failed to publish provisional texture access for resource {}:{}",
                access.resource.slot,
                access.resource.generation);
            return;
        }
        m_frameAccessSnapshotRollback.pending = true;
    };
    for (const RenderGraphExternalTextureAccess& access :
         m_frameExternalTextureAccesses)
    {
        publishExternalTexture(access);
    }
    if (m_activeRenderPassResults != nullptr &&
        m_activeRenderPassResults->identity == m_activeRenderPassIdentity)
    {
        for (const RenderGraphExternalTextureAccess& access :
             m_activeRenderPassResults->externalTextureAccesses)
        {
            publishExternalTexture(access);
        }
    }
}

void SceneRenderer::ConfirmProvisionalFrameAccessSnapshots() noexcept
{
    m_frameAccessSnapshotRollback = {};
}

void SceneRenderer::RestoreProvisionalFrameAccessSnapshots() noexcept
{
    if (!m_frameAccessSnapshotRollback.pending)
    {
        return;
    }

    if (m_frameAccessSnapshotRollback.restoreDepth)
    {
        m_depthAccessSnapshot = m_frameAccessSnapshotRollback.depthAccess;
    }
    if (m_frameAccessSnapshotRollback.backBufferIndex <
        m_backBufferAccessSnapshots.size())
    {
        m_backBufferAccessSnapshots[
            m_frameAccessSnapshotRollback.backBufferIndex] =
            m_frameAccessSnapshotRollback.backBufferAccess;
    }
    if (m_renderResourceRegistry != nullptr)
    {
        for (auto it =
                 m_frameAccessSnapshotRollback.resourceTextures.rbegin();
             it != m_frameAccessSnapshotRollback.resourceTextures.rend();
             ++it)
        {
            if (!m_renderResourceRegistry->CommitTextureAccessSnapshot(
                    it->resource, it->access))
            {
                RVX_RENDER_ERROR(
                    "SceneRenderer: failed to restore texture access for resource {}:{}",
                    it->resource.slot,
                    it->resource.generation);
            }
        }
    }
    m_frameAccessSnapshotRollback = {};
}

void SceneRenderer::RetireOwnerSnapshots(
    const GPUCompletionToken& completion)
{
    if (!m_retirementQueue)
    {
        return;
    }
    if (m_depthGPUCulling)
    {
        m_depthGPUCulling->RetireOwnerSnapshots(completion, *m_retirementQueue);
    }
    if (m_opaqueGPUCulling)
    {
        m_opaqueGPUCulling->RetireOwnerSnapshots(completion, *m_retirementQueue);
    }
    if (m_pipelineCache)
    {
        m_pipelineCache->RetireOwnerSnapshots(
            completion, *m_retirementQueue);
    }
    if (m_materialSystem)
    {
        m_materialSystem->RetireOwnerSnapshots(
            completion, *m_retirementQueue);
    }
    if (m_rayTracingSceneManager)
    {
        m_rayTracingSceneManager->RetireOwnerSnapshots(
            completion, *m_retirementQueue);
    }
    if (FindCompositionPass<RayTracedShadowPass>())
    {
        FindCompositionPass<RayTracedShadowPass>()->RetireOwnerSnapshots(
            completion, *m_retirementQueue);
    }
    if (FindCompositionPass<RayTracedReflectionPass>())
    {
        FindCompositionPass<RayTracedReflectionPass>()->RetireOwnerSnapshots(
            completion, *m_retirementQueue);
    }
}

void SceneRenderer::MarkAcceptedFramePresented(
    bool externalQualificationAccepted)
{
    const uint64 presentedFrameSequence =
        m_renderScene.GetAcceptedHeader().sequence;
    const RenderViewIdentity presentedViewIdentity =
        m_renderScene.GetView().identity;
    m_renderScene.MarkAcceptedFrameRendered();
    // Motion history is committed only after the full receipt and temporal
    // transaction below qualify. A physical Present with rejected evidence
    // must leave the prior qualified transforms intact.

    const bool hybridRayTracingReceiptMatches =
        !m_hybridRayTracingFramePlan.has_value() ||
        (m_hybridRayTracingExecutionReceipt.has_value() &&
         m_hybridRayTracingExecutionReceipt->IsExecutionSuccessfulFor(
             *m_hybridRayTracingFramePlan) &&
         m_pendingRenderExecutionReceipt.has_value() &&
         m_pendingRenderExecutionReceipt->hybridRayTracing.available &&
         m_pendingRenderExecutionReceipt->hybridRayTracing
                 .planIdentityHash ==
             m_hybridRayTracingFramePlan->GetIdentityHash() &&
         m_pendingRenderExecutionReceipt->hybridRayTracing
             .submissionAccepted);
    const bool receiptMatches = externalQualificationAccepted &&
        m_pendingRenderExecutionReceipt.has_value() &&
        m_pendingRenderExecutionReceipt->executionSucceeded &&
        m_pendingRenderExecutionReceipt->frameIdentity.frameSequence ==
            presentedFrameSequence &&
        m_pendingRenderExecutionReceipt->viewIdentity ==
            presentedViewIdentity &&
        hybridRayTracingReceiptMatches;
    const bool temporalPresentationStateMatches =
        m_activeViewHistoryCandidate.has_value() &&
        m_pendingTemporalPresentationState.has_value() &&
        m_pendingTemporalPresentationState->historyIdentity ==
            *m_activeViewHistoryCandidate &&
        m_pendingTemporalPresentationState->historyIdentity.view ==
            presentedViewIdentity;
    const std::optional<PendingTemporalPresentationState>
        presentedTemporalCandidate = m_pendingTemporalPresentationState;

    const bool postProcessRequiresHistoryCommit =
        FindCompositionFeature<PostProcessPipelineFeatureAdapter>() != nullptr &&
        FindCompositionFeature<PostProcessPipelineFeatureAdapter>()->RequiresTemporalHistoryCommit();
    const bool ambientOcclusionRequiresHistoryCommit =
        FindCompositionFeature<AmbientOcclusionFeatureAdapter>() != nullptr &&
        FindCompositionFeature<AmbientOcclusionFeatureAdapter>()->RequiresHistoryCommit();
    const bool volumetricLightingRequiresHistoryCommit =
        FindCompositionFeature<VolumetricLightingFeatureAdapter>() != nullptr &&
        FindCompositionFeature<VolumetricLightingFeatureAdapter>()->RequiresHistoryCommit();
    const bool rayTracedReflectionHistoryRequired =
        m_hybridRayTracingFramePlan.has_value() &&
        m_hybridRayTracingFramePlan->GetReflectionRoute() ==
            HybridRayTracingReflectionRoute::RayTraced;
    const bool coordinatorCanCommit =
        externalQualificationAccepted && temporalPresentationStateMatches &&
        m_viewHistoryCoordinator && m_activeViewHistoryCandidate.has_value() &&
        (!(postProcessRequiresHistoryCommit ||
           ambientOcclusionRequiresHistoryCommit ||
           volumetricLightingRequiresHistoryCommit ||
           rayTracedReflectionHistoryRequired) || receiptMatches) &&
        m_viewHistoryCoordinator->CanCommitPresented(
            *m_activeViewHistoryCandidate);
    // A presentation is a dual temporal transaction when TAA produced a
    // candidate. Preflight both independently owned stores before advancing
    // either one: an identity mismatch must abandon both candidates, never
    // leave the coordinator committed while TAA rejected its history.
    const bool postProcessCanCommit =
        !postProcessRequiresHistoryCommit ||
        (coordinatorCanCommit && FindCompositionFeature<PostProcessPipelineFeatureAdapter>() != nullptr &&
         m_activeViewHistoryCandidate.has_value() &&
         FindCompositionFeature<PostProcessPipelineFeatureAdapter>()->CanCommitTemporalHistoryAfterPresent(
             *m_activeViewHistoryCandidate));
    const bool ambientOcclusionCanCommit =
        !ambientOcclusionRequiresHistoryCommit ||
        (coordinatorCanCommit && FindCompositionFeature<AmbientOcclusionFeatureAdapter>() != nullptr &&
         m_activeViewHistoryCandidate.has_value() &&
         FindCompositionFeature<AmbientOcclusionFeatureAdapter>()->CanCommitHistoryAfterPresent(
             *m_activeViewHistoryCandidate));
    const bool volumetricLightingCanCommit =
        !volumetricLightingRequiresHistoryCommit ||
        (coordinatorCanCommit && FindCompositionFeature<VolumetricLightingFeatureAdapter>() != nullptr &&
         m_activeViewHistoryCandidate.has_value() &&
         FindCompositionFeature<VolumetricLightingFeatureAdapter>()->CanCommitHistoryAfterPresent(
             *m_activeViewHistoryCandidate));
    const bool rayTracedReflectionCanCommit =
        !rayTracedReflectionHistoryRequired ||
        (coordinatorCanCommit && FindCompositionPass<RayTracedReflectionPass>() != nullptr &&
         FindCompositionPass<RayTracedReflectionPass>()->RequiresHistoryCommit() &&
         m_activeViewHistoryCandidate.has_value() &&
         FindCompositionPass<RayTracedReflectionPass>()->CanCommitHistoryAfterPresent(
             *m_activeViewHistoryCandidate));

    bool historyCommitted = false;
    bool postProcessHistoryCommitted = !postProcessRequiresHistoryCommit;
    bool ambientOcclusionHistoryCommitted =
        !ambientOcclusionRequiresHistoryCommit;
    bool volumetricLightingHistoryCommitted =
        !volumetricLightingRequiresHistoryCommit;
    bool rayTracedReflectionHistoryCommitted =
        !rayTracedReflectionHistoryRequired;
    if (coordinatorCanCommit && postProcessCanCommit &&
        ambientOcclusionCanCommit && volumetricLightingCanCommit &&
        rayTracedReflectionCanCommit)
    {
        // These commits cannot fail after their side-effect-free exact
        // candidate preflights while this renderer owns the frame lifecycle.
        historyCommitted = m_viewHistoryCoordinator->CommitPresented(
            *m_activeViewHistoryCandidate);
        if (historyCommitted && postProcessRequiresHistoryCommit)
        {
            postProcessHistoryCommitted =
                FindCompositionFeature<PostProcessPipelineFeatureAdapter>()->CommitTemporalHistoryAfterPresent(
                    *m_activeViewHistoryCandidate);
        }
        if (historyCommitted && postProcessHistoryCommitted &&
            ambientOcclusionRequiresHistoryCommit)
        {
            ambientOcclusionHistoryCommitted =
                FindCompositionFeature<AmbientOcclusionFeatureAdapter>()->CommitHistoryAfterPresent(
                    *m_activeViewHistoryCandidate);
        }
        if (historyCommitted && postProcessHistoryCommitted &&
            ambientOcclusionHistoryCommitted &&
            volumetricLightingRequiresHistoryCommit)
        {
            volumetricLightingHistoryCommitted =
                FindCompositionFeature<VolumetricLightingFeatureAdapter>()->CommitHistoryAfterPresent(
                    *m_activeViewHistoryCandidate);
        }
        if (historyCommitted && postProcessHistoryCommitted &&
            ambientOcclusionHistoryCommitted &&
            volumetricLightingHistoryCommitted &&
            rayTracedReflectionHistoryRequired)
        {
            rayTracedReflectionHistoryCommitted =
                FindCompositionPass<RayTracedReflectionPass>()->CommitHistoryAfterPresent(
                    *m_activeViewHistoryCandidate);
        }
    }

    if (!historyCommitted && m_viewHistoryCoordinator &&
        m_activeViewHistoryCandidate.has_value())
    {
        m_viewHistoryCoordinator->Abandon(*m_activeViewHistoryCandidate);
    }
    if (!postProcessHistoryCommitted && FindCompositionFeature<PostProcessPipelineFeatureAdapter>() != nullptr)
    {
        FindCompositionFeature<PostProcessPipelineFeatureAdapter>()->AbandonTemporalHistoryCandidate();
    }
    if (!ambientOcclusionHistoryCommitted &&
        FindCompositionFeature<AmbientOcclusionFeatureAdapter>() != nullptr)
    {
        FindCompositionFeature<AmbientOcclusionFeatureAdapter>()->AbandonHistoryCandidate();
    }
    if (!volumetricLightingHistoryCommitted &&
        FindCompositionFeature<VolumetricLightingFeatureAdapter>() != nullptr)
    {
        FindCompositionFeature<VolumetricLightingFeatureAdapter>()->AbandonHistoryCandidate();
    }
    if (!rayTracedReflectionHistoryCommitted &&
        FindCompositionPass<RayTracedReflectionPass>() != nullptr &&
        m_activeViewHistoryCandidate.has_value())
    {
        FindCompositionPass<RayTracedReflectionPass>()->AbandonSubmittedHistory(
            *m_activeViewHistoryCandidate);
    }
    if (!historyCommitted || !postProcessHistoryCommitted ||
        !ambientOcclusionHistoryCommitted ||
        !volumetricLightingHistoryCommitted ||
        !rayTracedReflectionHistoryCommitted)
    {
        // Preserve correctness if a lifecycle caller violates the accepted /
        // presented pairing or either exact-candidate preflight fails. The
        // next accepted view starts fresh rather than reusing a partially
        // qualified temporal state.
        m_pendingTemporalHistoryReset = true;
        if (m_viewHistoryCoordinator)
        {
            m_viewHistoryCoordinator->RequestReset(
                RenderHistoryResetReason::ExplicitDiscontinuity);
        }
        RVX_RENDER_ERROR(
            "SceneRenderer: presented frame failed temporal history preflight or commit");
    }
    else
    {
        // A queued reset remains observable until the candidate carrying it
        // actually presents. Failed or abandoned candidates leave it pending.
        m_pendingTemporalHistoryReset =
            m_viewHistoryCoordinator->HasPendingReset();
    }
    if (m_pendingRenderExecutionReceipt.has_value() &&
        FindCompositionFeature<PostProcessPipelineFeatureAdapter>() != nullptr)
    {
        m_pendingRenderExecutionReceipt->postProcess =
            FindCompositionFeature<PostProcessPipelineFeatureAdapter>()->GetLastExecutionReceipt();
    }
    if (m_pendingRenderExecutionReceipt.has_value() &&
        FindCompositionFeature<AmbientOcclusionFeatureAdapter>() != nullptr)
    {
        RenderAmbientOcclusionExecutionReceipt ambientOcclusion =
            FindCompositionFeature<AmbientOcclusionFeatureAdapter>()->GetLastExecutionReceipt();
        ambientOcclusion.lightingConsumerExecuted =
            m_pendingRenderExecutionReceipt->ambientOcclusion
                .lightingConsumerExecuted;
        ambientOcclusion.lightingAOFieldContentHash =
            m_pendingRenderExecutionReceipt->ambientOcclusion
                .lightingAOFieldContentHash;
        m_pendingRenderExecutionReceipt->ambientOcclusion =
            std::move(ambientOcclusion);
    }

    bool hybridRayTracingPresentationQualified =
        !m_hybridRayTracingFramePlan.has_value();
    if (m_hybridRayTracingFramePlan.has_value())
    {
        m_pendingHybridRayTracingObservation.presentationQualified =
            receiptMatches && historyCommitted &&
            postProcessHistoryCommitted &&
            ambientOcclusionHistoryCommitted &&
            rayTracedReflectionHistoryCommitted;
        m_hybridRayTracingExecutionReceipt =
            HybridRayTracingExecutionReceipt::FromPlan(
                *m_hybridRayTracingFramePlan,
                m_pendingHybridRayTracingObservation);
        hybridRayTracingPresentationQualified =
            m_hybridRayTracingExecutionReceipt->IsPresentationQualifiedFor(
                *m_hybridRayTracingFramePlan);
        if (m_pendingRenderExecutionReceipt.has_value())
        {
            RenderHybridRayTracingExecutionReceipt& hybrid =
                m_pendingRenderExecutionReceipt->hybridRayTracing;
            hybrid.presentationQualified =
                hybridRayTracingPresentationQualified;
            hybrid.succeeded = hybridRayTracingPresentationQualified;
            if (!hybrid.IsStructurallyValid())
            {
                hybridRayTracingPresentationQualified = false;
                hybrid.presentationQualified = false;
                hybrid.succeeded = false;
            }
        }
    }

    m_activeViewHistoryCandidate.reset();
    m_activeViewHistoryAvailability.reset();
    m_pendingTemporalPresentationState.reset();

    const bool presentationQualified =
        receiptMatches && historyCommitted && postProcessHistoryCommitted &&
        ambientOcclusionHistoryCommitted &&
        volumetricLightingHistoryCommitted &&
        rayTracedReflectionHistoryCommitted &&
        hybridRayTracingPresentationQualified;
    // Apply-only callers do not have a render execution receipt, but still
    // use this boundary to commit scene motion history. Once a render receipt
    // exists, the history must follow the fully qualified presentation rather
    // than the physical Present alone.
    const bool objectMotionHistoryQualified =
        externalQualificationAccepted &&
        (!m_pendingRenderExecutionReceipt.has_value() ||
         presentationQualified);
    const bool reflectionMatchesPresentation = presentationQualified &&
        m_pendingReflectionDiagnostics.has_value() &&
        m_pendingReflectionDiagnostics->graphExecutionFinalized &&
        m_pendingReflectionDiagnostics->frameSequence ==
            presentedFrameSequence &&
        m_pendingReflectionDiagnostics->reflectionPlanIdentity != 0 &&
        m_pendingReflectionDiagnostics->lightingPlanIdentity != 0;
    if (m_ambientOcclusionProbeSubmissionAccepted &&
        FindCompositionFeature<AmbientOcclusionFeatureAdapter>() != nullptr)
    {
        const bool probeMatchesPresentation =
            presentationQualified && presentedTemporalCandidate.has_value() &&
            m_ambientOcclusionProbeHistoryIdentity.has_value() &&
            *m_ambientOcclusionProbeHistoryIdentity ==
                presentedTemporalCandidate->historyIdentity;
        if (probeMatchesPresentation)
        {
            m_ambientOcclusionProbePresented = true;
            m_ambientOcclusionProbeAbandoned = false;
        }
        else
        {
            FindCompositionFeature<AmbientOcclusionFeatureAdapter>()->AbandonQualificationProbe();
            m_ambientOcclusionProbePresented = false;
            m_ambientOcclusionProbeAbandoned = true;
        }
    }
    if (m_exposureProbeCompletion &&
        !(m_exposureProbePresented &&
          m_exposureProbeSourceExecutionReceipt.has_value()))
    {
        const bool exposureProbeMatchesPresentation =
            presentationQualified && presentedTemporalCandidate.has_value() &&
            m_exposureProbeHistoryIdentity.has_value() &&
            *m_exposureProbeHistoryIdentity ==
                presentedTemporalCandidate->historyIdentity;
        if (exposureProbeMatchesPresentation)
        {
            m_exposureProbePresented = true;
        }
        else
        {
            if (GetPostProcessStack())
            {
                GetPostProcessStack()->GetExposureController()
                    .CancelQualificationProbe();
            }
            m_exposureProbeCompletion.reset();
            m_exposureProbeHistoryIdentity.reset();
            m_exposureProbeSourceExecutionReceipt.reset();
            m_exposureProbePresented = false;
        }
    }
    if (m_toonOutlineProbeSubmissionAccepted &&
        FindPostProcessEffect<ScreenSpaceOutlinePass>() != nullptr)
    {
        const auto pendingIdentity =
            FindPostProcessEffect<ScreenSpaceOutlinePass>()
                ->GetPendingQualificationProbeIdentity();
        const bool probeMatchesPresentation =
            presentationQualified &&
            presentedTemporalCandidate.has_value() &&
            pendingIdentity.has_value() &&
            pendingIdentity->historyIdentity ==
                presentedTemporalCandidate->historyIdentity &&
            pendingIdentity->frameIndex == presentedFrameSequence;
        if (probeMatchesPresentation &&
            FindPostProcessEffect<ScreenSpaceOutlinePass>()
                ->MarkQualificationProbePresented(*pendingIdentity))
        {
            m_toonOutlineProbePresented = true;
            m_toonOutlineProbeAbandoned = false;
        }
        else
        {
            FindPostProcessEffect<ScreenSpaceOutlinePass>()->AbandonQualificationProbe();
            m_toonOutlineProbePresented = false;
            m_toonOutlineProbeAbandoned = true;
        }
    }
    if (m_reflectionProbeSubmissionAccepted &&
        FindCompositionFeature<ReflectionFeatureAdapter>() != nullptr)
    {
        const auto pendingIdentity =
            FindCompositionFeature<ReflectionFeatureAdapter>()->GetPendingSSRQualificationProbeIdentity();
        const bool probeMatchesPresentation = presentationQualified &&
            pendingIdentity.has_value() && pendingIdentity->IsValid() &&
            m_hiZReflectionProbeBinding.IsBound() &&
            m_reflectionProbeSourceExecutionReceipt.has_value() &&
            m_reflectionProbeSourceDiagnostics.has_value() &&
            pendingIdentity->frameIdentity.frameSequence ==
                presentedFrameSequence &&
            pendingIdentity->viewIdentity == presentedViewIdentity &&
            pendingIdentity->frameIdentity ==
                m_hiZReflectionProbeBinding.frameIdentity &&
            pendingIdentity->viewIdentity ==
                m_hiZReflectionProbeBinding.viewIdentity &&
            pendingIdentity->reflectionPlanIdentity ==
                m_hiZReflectionProbeBinding.reflectionPlanIdentity &&
            pendingIdentity->lightingPlanIdentity ==
                m_hiZReflectionProbeBinding.lightingPlanIdentity &&
            pendingIdentity->configurationIdentity ==
                m_hiZReflectionProbeBinding.configurationIdentity &&
            pendingIdentity->pipelineIdentity ==
                m_hiZReflectionProbeBinding.pipelineIdentity &&
            pendingIdentity->depthPyramidIdentity ==
                m_hiZReflectionProbeBinding.depthPyramidIdentity &&
            pendingIdentity->reflectionSurfaceIdentity ==
                m_hiZReflectionProbeBinding.reflectionSurfaceIdentity &&
            pendingIdentity->exposureStateContentHash ==
                m_hiZReflectionProbeBinding.exposureStateContentHash &&
            m_reflectionProbeSourceExecutionReceipt->frameIdentity ==
                pendingIdentity->frameIdentity &&
            m_reflectionProbeSourceExecutionReceipt->viewIdentity ==
                pendingIdentity->viewIdentity &&
            m_reflectionProbeSourceDiagnostics->reflectionPlanIdentity ==
                pendingIdentity->reflectionPlanIdentity &&
            m_reflectionProbeSourceDiagnostics->lightingPlanIdentity ==
                pendingIdentity->lightingPlanIdentity &&
            m_reflectionProbeSourceDiagnostics->configurationIdentity ==
                pendingIdentity->configurationIdentity &&
            m_reflectionProbeSourceDiagnostics->pipelineIdentity ==
                pendingIdentity->pipelineIdentity &&
            m_reflectionProbeSourceDiagnostics->depthPyramidIdentity ==
                pendingIdentity->depthPyramidIdentity;
        if (probeMatchesPresentation &&
            FindCompositionFeature<ReflectionFeatureAdapter>()->MarkSSRQualificationProbePresented(
                *pendingIdentity))
        {
            m_reflectionProbePresented = true;
            m_reflectionProbeAbandoned = false;
        }
        else
        {
            FindCompositionFeature<ReflectionFeatureAdapter>()->AbandonSSRQualificationProbe();
            m_reflectionProbePresented = false;
            m_reflectionProbeAbandoned = true;
        }
    }
    if (objectMotionHistoryQualified)
    {
        UpdateObjectMotionHistory();
    }
    if (auto* transparent = FindCompositionPass<TransparentPass>())
        transparent->CommitTemporalHistoryAfterPresent(
            presentedFrameSequence, presentedViewIdentity, presentationQualified);
    if (presentationQualified)
    {
        RenderExecutionReceipt presented =
            *m_pendingRenderExecutionReceipt;
        presented.presented = true;
        presented.succeeded = true;
        presented.diagnostic.clear();
        if (presented.IsStructurallyValid())
        {
            if (presentedTemporalCandidate.has_value())
            {
                m_presentedTemporalPresentationState =
                    *presentedTemporalCandidate;
            }
            m_completedRenderExecutionReceipt = presented;
            if (reflectionMatchesPresentation)
            {
                RenderReflectionDiagnostics reflection =
                    *m_pendingReflectionDiagnostics;
                reflection.available = true;
                m_completedReflectionDiagnostics = std::move(reflection);
            }
            if (m_reflectionProbeSubmissionAccepted &&
                m_reflectionProbePresented &&
                m_reflectionProbeSourceDiagnostics.has_value() &&
                m_reflectionProbeSourceDiagnostics->frameSequence ==
                    m_completedRenderExecutionReceipt.frameIdentity.frameSequence &&
                m_completedReflectionDiagnostics.frameSequence ==
                    m_completedRenderExecutionReceipt.frameIdentity.frameSequence)
            {
                // Freeze the completed source pair before later frames can
                // replace the ordinary presentation diagnostics.
                m_reflectionProbeSourceExecutionReceipt =
                    m_completedRenderExecutionReceipt;
                m_reflectionProbeSourceDiagnostics =
                    m_completedReflectionDiagnostics;
            }
            else if (m_reflectionProbeSubmissionAccepted &&
                     FindCompositionFeature<ReflectionFeatureAdapter>() != nullptr)
            {
                FindCompositionFeature<ReflectionFeatureAdapter>()->AbandonSSRQualificationProbe();
                m_reflectionProbePresented = false;
                m_reflectionProbeAbandoned = true;
            }
            if (m_exposureProbeCompletion && m_exposureProbePresented &&
                m_exposureProbeHistoryIdentity.has_value() &&
                presentedTemporalCandidate.has_value() &&
                *m_exposureProbeHistoryIdentity ==
                    presentedTemporalCandidate->historyIdentity)
            {
                m_exposureProbeSourceExecutionReceipt =
                    m_completedRenderExecutionReceipt;
            }
            if (m_ambientOcclusionProbeSubmissionAccepted &&
                m_ambientOcclusionProbePresented &&
                m_ambientOcclusionProbeHistoryIdentity.has_value() &&
                presentedTemporalCandidate.has_value() &&
                *m_ambientOcclusionProbeHistoryIdentity ==
                    presentedTemporalCandidate->historyIdentity)
            {
                m_ambientOcclusionProbeSourceExecutionReceipt =
                    m_completedRenderExecutionReceipt;
            }
            if (m_toonOutlineProbeSubmissionAccepted &&
                m_toonOutlineProbePresented &&
                FindPostProcessEffect<ScreenSpaceOutlinePass>() != nullptr)
            {
                const auto pendingIdentity =
                    FindPostProcessEffect<ScreenSpaceOutlinePass>()
                        ->GetPendingQualificationProbeIdentity();
                if (pendingIdentity.has_value() &&
                    presentedTemporalCandidate.has_value() &&
                    pendingIdentity->historyIdentity ==
                        presentedTemporalCandidate->historyIdentity &&
                    m_completedRenderExecutionReceipt.toonOutline
                        .qualificationRequested)
                {
                    m_toonOutlineProbeSourceExecutionReceipt =
                        m_completedRenderExecutionReceipt;
                }
                else
                {
                    FindPostProcessEffect<ScreenSpaceOutlinePass>()
                        ->AbandonQualificationProbe();
                    m_toonOutlineProbePresented = false;
                    m_toonOutlineProbeAbandoned = true;
                }
            }
            m_lastAttemptedRenderExecutionReceipt = std::move(presented);
        }
        else
        {
            // `presented` is presentation-qualified evidence, not merely the
            // physical Present call. Keep this rejected receipt in the
            // execution-complete / presentation-unqualified state.
            presented.presented = false;
            presented.succeeded = false;
            if (presented.hybridRayTracing.available)
            {
                presented.hybridRayTracing.presentationQualified = false;
                presented.hybridRayTracing.succeeded = false;
                m_pendingHybridRayTracingObservation
                    .presentationQualified = false;
                if (m_hybridRayTracingFramePlan.has_value())
                {
                    m_hybridRayTracingExecutionReceipt =
                        HybridRayTracingExecutionReceipt::FromPlan(
                            *m_hybridRayTracingFramePlan,
                            m_pendingHybridRayTracingObservation);
                }
            }
            presented.diagnostic =
                "Presented render execution receipt failed structural validation";
            if (m_ambientOcclusionProbeSubmissionAccepted &&
                FindCompositionFeature<AmbientOcclusionFeatureAdapter>() != nullptr)
            {
                FindCompositionFeature<AmbientOcclusionFeatureAdapter>()->AbandonQualificationProbe();
                m_ambientOcclusionProbePresented = false;
                m_ambientOcclusionProbeAbandoned = true;
            }
            if (m_toonOutlineProbeSubmissionAccepted &&
                FindPostProcessEffect<ScreenSpaceOutlinePass>() != nullptr)
            {
                FindPostProcessEffect<ScreenSpaceOutlinePass>()
                    ->AbandonQualificationProbe();
                m_toonOutlineProbePresented = false;
                m_toonOutlineProbeAbandoned = true;
            }
            if (m_reflectionProbeSubmissionAccepted &&
                FindCompositionFeature<ReflectionFeatureAdapter>() != nullptr)
            {
                FindCompositionFeature<ReflectionFeatureAdapter>()->AbandonSSRQualificationProbe();
                m_reflectionProbePresented = false;
                m_reflectionProbeAbandoned = true;
            }
            m_lastAttemptedRenderExecutionReceipt = std::move(presented);
        }
    }
    else
    {
        RenderExecutionReceipt rejected =
            m_pendingRenderExecutionReceipt.has_value()
                ? *m_pendingRenderExecutionReceipt
                : m_lastAttemptedRenderExecutionReceipt;
        // The frame may have reached the platform Present call, but it did
        // not qualify for a presentation receipt.  Keeping this false also
        // preserves the receipt invariant for a failed qualification.
        rejected.presented = false;
        rejected.succeeded = false;
        if (rejected.hybridRayTracing.available)
        {
            rejected.hybridRayTracing.presentationQualified = false;
            rejected.hybridRayTracing.succeeded = false;
        }
        rejected.diagnostic = !receiptMatches
            ? "Presented frame did not qualify for a completed render execution receipt"
            : !historyCommitted
                ? "Presented frame failed temporal history qualification"
                : "Presented frame failed TAA temporal history qualification";
        if (m_reflectionProbeSubmissionAccepted &&
            FindCompositionFeature<ReflectionFeatureAdapter>() != nullptr)
        {
            FindCompositionFeature<ReflectionFeatureAdapter>()->AbandonSSRQualificationProbe();
            m_reflectionProbePresented = false;
            m_reflectionProbeAbandoned = true;
        }
        m_lastAttemptedRenderExecutionReceipt = std::move(rejected);
    }
    if (m_qualificationCaptureBridge.GetDiagnostics().state ==
        QualificationCaptureState::PendingCompletion)
    {
        if (!m_qualificationCaptureBridge.FinalizePresentedReceipt(
                m_lastAttemptedRenderExecutionReceipt))
        {
            // The copy resources remain completion-owned. RejectPending only
            // marks the pending owner; it releases no GPU-visible resource
            // until the exact graphics token reaches completion.
            m_qualificationCaptureBridge.RejectPending();
            RVX_RENDER_WARN(
                "SceneRenderer: RenderQuality qualification capture rejected final presentation receipt");
        }
    }
    m_pendingRenderExecutionReceipt.reset();
    m_pendingReflectionDiagnostics.reset();
    m_frameDiagnostics.attemptedExecutionReceipt =
        m_lastAttemptedRenderExecutionReceipt;
    m_frameDiagnostics.presentedExecutionReceipt =
        m_completedRenderExecutionReceipt;
    m_toolDiagnosticsSnapshot.frame.attemptedExecutionReceipt =
        m_lastAttemptedRenderExecutionReceipt;
    m_toolDiagnosticsSnapshot.frame.presentedExecutionReceipt =
        m_completedRenderExecutionReceipt;

    FreezeMutationEvidenceForPresentedFrame();
    if (m_qualityPerformanceSession.RequiresFrameTelemetry(
            m_lastAttemptedRenderExecutionReceipt.frameIdentity))
    {
        RenderQualityPerformanceFrameTelemetry telemetry;
        static_cast<void>(CollectRenderQualityPerformanceTelemetry(
            m_lastAttemptedRenderExecutionReceipt.frameIdentity, telemetry));
        if (!m_qualityPerformanceSession.MarkFramePresented(
                m_lastAttemptedRenderExecutionReceipt, telemetry))
        {
            RVX_RENDER_WARN(
                "SceneRenderer: GFX24 performance session rejected same-frame presentation telemetry");
        }
    }
    FreezePresentedMaterialBindingsForPresentedFrame();
}

void SceneRenderer::SetSurfaceCompatibilityKey(uint64 key) noexcept
{
    m_renderScene.SetSurfaceCompatibilityKey(key);
}

const GPUScenePublicationStats&
SceneRenderer::GetGPUScenePublicationStats() const noexcept
{
    static const GPUScenePublicationStats unavailableStats{};
    return m_gpuSceneUpdate ? m_gpuSceneUpdate->GetStats() : unavailableStats;
}

const GPUSceneUploadDiagnostics&
SceneRenderer::GetGPUSceneUploadDiagnostics() const noexcept
{
    static const GPUSceneUploadDiagnostics unavailableDiagnostics{};
    return m_gpuSceneUploader
        ? m_gpuSceneUploader->GetDiagnostics()
        : unavailableDiagnostics;
}

GPUSceneDiagnostics SceneRenderer::GetGPUSceneDiagnostics() const noexcept
{
    GPUSceneDiagnostics diagnostics;
    diagnostics.informationalOnly = true;
    if (m_gpuSceneUpdate)
    {
        diagnostics = m_gpuSceneUpdate->GetDiagnostics();
        const GPUScenePublicationStats& publication = m_gpuSceneUpdate->GetStats();
        diagnostics.available = true;
        diagnostics.publicationAttempted = publication.attempted;
        diagnostics.publicationCandidate =
            publication.candidateObjectCount != 0 ||
            (diagnostics.publicationAttempted &&
             publication.excludedObjectCount == 0 &&
             publication.excludedDrawCount == 0);
        diagnostics.publicationPublished = publication.attempted &&
            publication.complete &&
            publication.failureReason == GPUScenePublicationFailureReason::None &&
            publication.committedSourceSequence == publication.sourceSequence;
        diagnostics.publicationFailed =
            publication.failureReason != GPUScenePublicationFailureReason::None;
        diagnostics.publicationComplete = publication.complete;
        diagnostics.publicationFailureReason = publication.failureReason;
        diagnostics.attemptedObjectCount = publication.attemptedObjectCount;
        diagnostics.attemptedDrawCount = publication.attemptedDrawCount;
        diagnostics.attemptedMaterialBindingCount =
            publication.attemptedMaterialBindingCount;
        diagnostics.candidateObjectCount = publication.candidateObjectCount;
        diagnostics.candidateDrawCount = publication.candidateDrawCount;
        diagnostics.candidateMaterialBindingCount =
            publication.candidateMaterialBindingCount;
        diagnostics.publishedObjectCount = publication.publishedObjectCount;
        diagnostics.publishedDrawCount = publication.publishedDrawCount;
        diagnostics.publishedMaterialBindingCount =
            publication.publishedMaterialBindingCount;
        diagnostics.excludedMaterialBindingCount =
            publication.excludedMaterialBindingCount;
        diagnostics.renderClassificationParity =
            m_sceneDrawClassification.IsValid() &&
            publication.HasRenderClassificationParity(
                m_sceneDrawClassification.stats.sceneObjectCount,
                m_sceneDrawClassification.stats.classifiedPacketCount,
                m_sceneDrawClassification.stats.materialBindingCount);
        diagnostics.addCount = publication.addCount;
        diagnostics.updateCount = publication.updateCount;
        diagnostics.removeCount = publication.removeCount;
        diagnostics.noOpCount = publication.noOpCount;
        diagnostics.committedVersion = publication.committedVersion;
    }

    if (m_gpuSceneUploader)
    {
        const GPUSceneUploadDiagnostics& upload =
            m_gpuSceneUploader->GetDiagnostics();
        diagnostics.available = true;
        diagnostics.residentVersion = upload.residentVersion;
        diagnostics.safeReclaimVersion = upload.safeReclaimVersion;
        diagnostics.gpuAllocationBytes = upload.gpuAllocationBytes;
        diagnostics.cumulativeUploadBytes = upload.cumulativeUploadBytes;
        diagnostics.peakFrameUploadBytes = upload.peakFrameUploadBytes;
        diagnostics.cumulativeUploadRangeCount = upload.cumulativeUploadRangeCount;
        diagnostics.currentBufferSetCount = upload.bufferSetCount;
        diagnostics.peakBufferSetCount = upload.peakBufferSetCount;
        diagnostics.pendingBufferSetCount = upload.pendingSetCount;
        diagnostics.inFlightBufferSetCount = upload.inFlightSetCount;
        diagnostics.unusableBufferSetCount = upload.unusableSetCount;
        diagnostics.uploadFailureReason = upload.failureReason;
        if (m_submissionQualifiedFrameDiagnostics)
        {
            diagnostics.frameUploadBytes = upload.frameUploadBytes;
            diagnostics.frameUploadRangeCount = upload.frameUploadRangeCount;
            diagnostics.fullUpload = upload.fullUpload;
            diagnostics.uploadWork = upload.uploadWork;
        }
        for (uint32 tableIndex = 0;
             tableIndex < GPU_SCENE_DIAGNOSTICS_TABLE_COUNT;
             ++tableIndex)
        {
            GPUSceneTableDiagnostics& target = diagnostics.tables[tableIndex];
            const GPUSceneTableDiagnostics& source = upload.tables[tableIndex];
            target.residentCapacity = source.residentCapacity;
            target.stride = source.stride != 0 ? source.stride : target.stride;
            target.residentBytes = source.residentBytes;
            target.cumulativeUploadBytes = source.cumulativeUploadBytes;
            target.peakFrameUploadBytes = source.peakFrameUploadBytes;
            target.cumulativeUploadRangeCount = source.cumulativeUploadRangeCount;
            target.resident = source.resident;
            if (m_submissionQualifiedFrameDiagnostics)
            {
                target.frameUploadBytes = source.frameUploadBytes;
                target.frameUploadRangeCount = source.frameUploadRangeCount;
                target.fullUpload = source.fullUpload;
                target.uploadWork = source.uploadWork;
            }
        }
    }

    // This is a projection of an already selected plan, never an input back
    // into the resolver.  It keeps evidence aligned with the frozen frame.
    if (m_renderPolicyDiagnostics.planAvailable)
    {
        diagnostics.requiredResidentVersion =
            m_renderPolicyDiagnostics.selectedPlan.viewPolicy.requiredResidentVersion;
    }
    diagnostics.leaseVersion = m_renderPolicyDiagnostics.gpuSceneLeaseVersion;
    diagnostics.timing.cpuPlanTimingAvailable =
        m_renderPolicyDiagnostics.measurement.planCpuTimingAvailable;
    diagnostics.timing.cpuSubmissionTimingAvailable =
        m_renderPolicyDiagnostics.measurement.submissionCpuTimingAvailable;
    diagnostics.timing.cpuPlanMilliseconds = static_cast<float64>(
        m_renderPolicyDiagnostics.measurement.planCpuNanoseconds) / 1000000.0;
    diagnostics.timing.cpuSubmissionMilliseconds = static_cast<float64>(
        m_renderPolicyDiagnostics.measurement.submissionCpuNanoseconds) / 1000000.0;
    diagnostics.timing.nonGating = true;
    diagnostics.timing.usedForAutoDecision = false;
    return diagnostics;
}

void SceneRenderer::SynchronizeGPUSceneUploader() noexcept
{
    if (!m_gpuSceneUploader || !m_gpuSceneUpdate)
    {
        return;
    }

    m_gpuSceneUploader->Observe(
        m_gpuSceneUpdate->GetCommittedMirrorForUpload(),
        m_gpuSceneUpdate->GetLastChangeSetForUpload());
}

void SceneRenderer::ReclaimGPUSceneRetiredRows() noexcept
{
    if (!m_gpuSceneUploader || !m_gpuSceneUpdate)
    {
        return;
    }

    const uint64 safeVersion = m_gpuSceneUploader->PollSafeReclaimVersion();
    if (safeVersion != 0)
    {
        if (m_gpuSceneUpdate->ReclaimRetiredThrough(safeVersion))
        {
            m_gpuSceneUploader->ConfirmReclaimedThrough(safeVersion);
        }
    }
}

void SceneRenderer::ClearGPUScene()
{
    if (m_gpuSceneUpdate)
    {
        m_gpuSceneUpdate->Clear();
        SynchronizeGPUSceneUploader();
        m_lastGPUSceneResourceContentRevision = 0;
        m_forceFullGPUScenePublication = false;
    }
}

void SceneRenderer::ResolveRenderTargetExtent(uint32& width, uint32& height) const
{
    width = 1280;
    height = 720;

    if (m_externalRenderTarget.IsValid())
    {
        width = m_externalRenderTarget.colorTarget->GetWidth();
        height = m_externalRenderTarget.colorTarget->GetHeight();
    }
    else if (m_renderContext && m_renderContext->GetSwapChain())
    {
        width = m_renderContext->GetSwapChain()->GetWidth();
        height = m_renderContext->GetSwapChain()->GetHeight();
    }
}

void SceneRenderer::BuildMaterialDrawLists()
{
    ClearPreparedDrawState();

    // Direct/Transparent/legacy consumers receive the CPU-final list. GPU
    // candidates deliberately start from the coarser visible+drawable list so
    // the GPU lane never inherits a CPU frustum pre-elimination.
    const RenderDrawClassificationBindingResolver bindingResolver{
        m_renderResourceRegistry, m_materialSystem.get()};
    if (!BuildRenderSceneDrawClassification(
            m_renderScene,
            RenderSceneDrawClassificationSources{&bindingResolver},
            m_sceneDrawClassification))
    {
        m_renderDrawPlanBuildCode =
            RenderDrawPlanBuildCode::InvalidClassification;
        return;
    }

    m_renderCandidates.Clear();
    for (uint32 objectIndex = 0;
         objectIndex < static_cast<uint32>(m_renderScene.GetObjectCount());
         ++objectIndex)
    {
        const RenderObject& object = m_renderScene.GetObject(objectIndex);
        RenderVisibilityCandidate candidate;
        candidate.objectIndex = objectIndex;
        // Scene visibility is view-independent. The view culling mask is
        // applied only while projecting the frozen classification below.
        candidate.objectVisible = object.visible;
        candidate.drawable = object.drawable;
        candidate.layerMask = object.layerMask;
        candidate.worldBounds = object.bounds;
        (void)m_renderCandidates.Add(candidate);
    }

    const GPUCullingConfig initialVisibilityConfig =
        GetGPUDrivenCullingConfig();
    RenderVisibilityRequest visibilityRequest;
    visibilityRequest.viewProjection =
        m_renderScene.GetView().viewProjectionMatrix;
    visibilityRequest.cameraPosition =
        m_renderScene.GetView().cameraPosition;
    visibilityRequest.enableDistanceCulling =
        initialVisibilityConfig.enableDistanceCulling;
    visibilityRequest.maxDrawDistance =
        initialVisibilityConfig.maxDrawDistance;
    visibilityRequest.cullingMask = m_renderScene.GetView().cullingMask;
    GPUVisibilityProvider visibilityProvider;
    visibilityProvider.Evaluate(m_renderCandidates,
                                visibilityRequest,
                                m_renderVisibility);
    m_visibleObjectIndices = m_renderVisibility.cpuVisibleObjectIndices;
    m_coarseCandidateObjectIndices =
        m_renderVisibility.coarseVisibleObjectIndices;
    const uint32 viewCullingMask = m_renderScene.GetView().cullingMask;
    const auto removeViewLayerMismatch = [this, viewCullingMask](uint32 index)
    {
        return index >= m_renderScene.GetObjectCount() ||
               !IsRenderLayerVisible(
                   m_renderScene.GetObject(index).layerMask,
                   viewCullingMask);
    };
    std::erase_if(m_visibleObjectIndices, removeViewLayerMismatch);
    std::erase_if(m_coarseCandidateObjectIndices, removeViewLayerMismatch);
    m_renderScene.SortVisibleObjects(m_visibleObjectIndices,
                                     m_renderScene.GetView().cameraPosition);
    m_renderScene.SortVisibleObjects(m_coarseCandidateObjectIndices,
                                     m_renderScene.GetView().cameraPosition);

    const bool finalProjectionBuilt = ProjectRenderSceneDrawClassification(
        m_sceneDrawClassification,
        m_renderVisibility.cpuVisibleObjectMask,
        viewCullingMask,
        m_viewData.cameraPosition,
        m_viewData.cameraForward,
        m_opaqueDrawItems,
        m_maskedDrawItems,
        m_transparentDrawItems,
        &m_transparentDrawListDiagnostics);

    std::vector<RenderDrawItem> ignoredTransparentCandidates;
    const bool coarseProjectionBuilt = ProjectRenderSceneDrawClassification(
        m_sceneDrawClassification,
        m_renderVisibility.coarseVisibleObjectMask,
        viewCullingMask,
        m_viewData.cameraPosition,
        m_viewData.cameraForward,
        m_coarseOpaqueDrawItems,
        m_coarseMaskedDrawItems,
        ignoredTransparentCandidates);

    if (!finalProjectionBuilt || !coarseProjectionBuilt)
    {
        ClearPreparedDrawState();
        m_renderDrawPlanBuildCode =
            RenderDrawPlanBuildCode::InvalidClassification;
        return;
    }

    PrepareMeshPassPackets();

}

void SceneRenderer::ClearPreparedDrawState() noexcept
{
    m_sceneDrawClassification.Clear();
    m_renderCandidates.Clear();
    m_renderVisibility = {};
    m_renderVisibility.structurallyValid = false;
    m_visibleObjectIndices.clear();
    m_coarseCandidateObjectIndices.clear();
    m_opaqueDrawItems.clear();
    m_maskedDrawItems.clear();
    m_transparentDrawItems.clear();
    m_coarseOpaqueDrawItems.clear();
    m_coarseMaskedDrawItems.clear();
    m_transparentDrawListDiagnostics = {};
    m_meshPassPreparation.Clear();
    m_renderDrawPlan = {};
    m_renderDrawPlanBuildCode =
        RenderDrawPlanBuildCode::InvalidPreparation;
    m_viewData.renderVisibility = nullptr;
    m_viewData.renderDrawPlan = nullptr;
    m_viewData.meshPassPreparation = nullptr;
}

void SceneRenderer::PrepareMeshPassPackets()
{
    m_meshPassPreparation.Clear();

    DepthMeshPassProcessor depthProcessor;
    OpaqueMeshPassProcessor opaqueProcessor;
    TransparentMeshPassProcessor transparentProcessor;
    ShadowMeshPassProcessor shadowProcessor;

    uint32 materialSourceOrdinal = 0;
    const auto prepareMaterialItems =
        [this, &depthProcessor, &opaqueProcessor, &materialSourceOrdinal](
            const std::vector<RenderDrawItem>& drawItems)
    {
        for (const RenderDrawItem& item : drawItems)
        {
            MeshPassProcessorInput input = MakeMeshPassProcessorInput(
                item.packet,
                m_renderResourceRegistry,
                m_materialSystem.get(),
                materialSourceOrdinal++,
                item.viewSpaceDepth,
                item.classificationIndex);
            m_meshPassPreparation.depth.Record(depthProcessor.Process(input));
            m_meshPassPreparation.opaque.Record(opaqueProcessor.Process(input));
        }
    };
    prepareMaterialItems(m_coarseOpaqueDrawItems);
    prepareMaterialItems(m_coarseMaskedDrawItems);

    for (uint32 sourceOrdinal = 0;
         sourceOrdinal < static_cast<uint32>(m_transparentDrawItems.size());
         ++sourceOrdinal)
    {
        const RenderDrawItem& item = m_transparentDrawItems[sourceOrdinal];
        const MeshPassProcessorInput input = MakeMeshPassProcessorInput(
            item.packet,
            m_renderResourceRegistry,
            m_materialSystem.get(),
            sourceOrdinal,
            item.viewSpaceDepth,
            item.classificationIndex);
        m_meshPassPreparation.transparent.Record(
            transparentProcessor.Process(input));
    }

    uint32 shadowSourceOrdinal = 0;
    const auto resolveClassificationIndex = [this](
        const RenderDrawPacket& packet) noexcept
    {
        const auto found = std::find_if(
            m_sceneDrawClassification.entries.begin(),
            m_sceneDrawClassification.entries.end(),
            [&packet](const RenderSceneDrawClassificationEntry& entry)
            {
                const RenderDrawPacket& classified = entry.draw.packet;
                return classified.objectId == packet.objectId &&
                       classified.primitiveData == packet.primitiveData &&
                       classified.submeshIndex == packet.submeshIndex &&
                       classified.geometryKey.mesh == packet.geometryKey.mesh &&
                       classified.materialKey == packet.materialKey;
            });
        return found == m_sceneDrawClassification.entries.end()
                   ? RVX_INVALID_INDEX
                   : found->classificationIndex;
    };
    for (uint32 objectIndex = 0;
         objectIndex < static_cast<uint32>(m_renderScene.GetObjectCount());
         ++objectIndex)
    {
        const RenderObject& object = m_renderScene.GetObject(objectIndex);
        if (object.meshBatchesAuthoritative)
        {
            for (const MeshBatch& batch : object.meshBatches)
            {
                const RenderDrawPacket packet = BuildShadowPacket(
                    m_renderScene, object, batch);
                const MeshPassProcessorInput input =
                    MakeMeshPassProcessorInput(
                        packet,
                        m_renderResourceRegistry,
                        m_materialSystem.get(),
                        shadowSourceOrdinal++,
                        0.0f,
                        resolveClassificationIndex(packet));
                m_meshPassPreparation.shadow.Record(
                    shadowProcessor.Process(input));
            }
            continue;
        }

        // A non-resident legacy object has exactly the same conservative
        // submesh-zero classification as the main surface path. Never invent
        // shadow-only draws outside the frozen classification.
        const MeshBatch batch = BuildLegacyShadowBatch(
            object, objectIndex, 0, nullptr);
        const RenderDrawPacket packet = BuildShadowPacket(
            m_renderScene, object, batch);
        const MeshPassProcessorInput input = MakeMeshPassProcessorInput(
            packet,
            m_renderResourceRegistry,
            m_materialSystem.get(),
            shadowSourceOrdinal++,
            0.0f,
            resolveClassificationIndex(packet));
        m_meshPassPreparation.shadow.Record(shadowProcessor.Process(input));
    }

    m_meshPassPreparation.depth.FinalizeGroups();
    m_meshPassPreparation.opaque.FinalizeGroups();
    m_meshPassPreparation.transparent.FinalizeGroups();
    m_meshPassPreparation.shadow.FinalizeGroups();

    const uint32 firstPassVisibilityCandidate = static_cast<uint32>(
        m_renderCandidates.GetCandidates().size());
    const auto appendVisibilityCandidates = [this](
                                                RenderPassKind pass,
                                                const MeshPassPacketStream& stream)
    {
        for (uint32 sourcePacketIndex = 0;
             sourcePacketIndex < static_cast<uint32>(stream.packets.size());
             ++sourcePacketIndex)
        {
            const MeshPassProcessorResult& source =
                stream.packets[sourcePacketIndex];
            const RenderDrawPacket& packet = source.packet;
            if (packet.primitiveData == RVX_INVALID_PRIMITIVE_DATA_INDEX ||
                packet.primitiveData >= m_renderScene.GetObjectCount())
            {
                continue;
            }
            const RenderObject& object =
                m_renderScene.GetObject(packet.primitiveData);
            if (packet.objectId == 0 || object.entityId != packet.objectId ||
                object.mesh != packet.geometryKey.mesh)
            {
                continue;
            }
            RenderVisibilityCandidate candidate;
            candidate.sourcePacketIndex = sourcePacketIndex;
            candidate.sourceOrdinal = source.sourceOrdinal;
            candidate.objectIndex = packet.primitiveData;
            candidate.pass = pass;
            candidate.objectVisible = object.visible && IsRenderLayerVisible(
                object.layerMask, m_renderScene.GetView().cullingMask);
            candidate.drawable = object.drawable;
            candidate.layerMask = object.layerMask;
            candidate.worldBounds = object.bounds;
            (void)m_renderCandidates.Add(candidate);
        }
    };
    appendVisibilityCandidates(RenderPassKind::Depth,
                               m_meshPassPreparation.depth);
    appendVisibilityCandidates(RenderPassKind::Opaque,
                               m_meshPassPreparation.opaque);
    appendVisibilityCandidates(RenderPassKind::Transparent,
                               m_meshPassPreparation.transparent);
    // Shadow/light views are intentionally not governed by camera visibility.
    GPUVisibilityProvider visibilityProvider;
    visibilityProvider.AppendPassCandidates(m_renderCandidates,
                                            firstPassVisibilityCandidate,
                                            m_renderVisibility);
    m_renderVisibility.diagnostics.occlusionRequested =
        (m_depthGPUCulling && m_depthGPUCulling->WasOcclusionRequested()) ||
        (m_opaqueGPUCulling && m_opaqueGPUCulling->WasOcclusionRequested());
    m_renderVisibility.diagnostics.occlusionAvailable = false;
    m_viewData.renderVisibility = &m_renderVisibility;

    const auto validateStream = [](const MeshPassPacketStream& stream)
    {
        (void)stream;
        RVX_DEBUG_ASSERT(stream.stats.HasCompleteRelevantOutcome());
        RVX_DEBUG_ASSERT(
            stream.stats.HasExactlyOneReasonPerRejectedPacket());
    };
    validateStream(m_meshPassPreparation.depth);
    validateStream(m_meshPassPreparation.opaque);
    validateStream(m_meshPassPreparation.transparent);
    validateStream(m_meshPassPreparation.shadow);

    RenderViewDrawPlanBuildResult drawPlan = BuildRenderViewDrawPlan(
        m_renderScene.GetAcceptedHeader().sequence,
        0,
        m_sceneDrawClassification,
        m_meshPassPreparation,
        m_renderVisibility);
    m_renderDrawPlanBuildCode = drawPlan.code;
    m_renderDrawPlan = std::move(drawPlan.plan);
    m_viewData.renderDrawPlan = m_renderDrawPlan.IsValid()
                                    ? &m_renderDrawPlan
                                    : nullptr;
}

void SceneRenderer::InvalidateRenderFramePlan()
{
    m_renderPolicyDiagnostics = {};
    m_viewData.renderFrameExecutionPlan = nullptr;
    m_viewData.renderDrawPlan = nullptr;
    m_viewData.meshPassPreparation = nullptr;
    m_viewData.instanceBatchPlans = nullptr;
    m_instanceBatchPlans = {};
    m_viewData.renderFrameExecutionReport = nullptr;
    m_viewData.renderVisibility = nullptr;
    m_renderDrawPlan = {};
    m_renderDrawPlanBuildCode =
        RenderDrawPlanBuildCode::InvalidPreparation;
    m_frameDiagnostics.policy = {};
    m_gpuDrivenCullingEnabled = false;
    m_gpuDrivenPolicyDecision = {};
    m_gpuDrivenPolicyDecision.requestedMode = m_gpuDrivenCullingMode;
}

void SceneRenderer::CompileRenderFramePlan()
{
    m_renderPolicyDiagnostics.planAvailable = false;
    m_renderPolicyDiagnostics.reportAvailable = false;
    m_renderPolicyDiagnostics.selectedPlan = {};
    m_renderPolicyDiagnostics.executionReport = {};
    m_renderPolicyDiagnostics.gpuSceneResidentVersion = 0;
    m_renderPolicyDiagnostics.gpuSceneLeaseVersion = 0;
    m_renderPolicyDiagnostics.measurement = {};
    m_confirmedGPUDrivenTier = GPUDrivenTier::Direct;
    m_viewData.renderFrameExecutionPlan = nullptr;
    m_viewData.renderDrawPlan = nullptr;
    m_viewData.meshPassPreparation = nullptr;
    m_viewData.instanceBatchPlans = nullptr;
    m_instanceBatchPlans = {};
    m_viewData.renderFrameExecutionReport = nullptr;

    RenderPolicyResolverInput input;
    input.request.frameSequence = m_renderScene.GetAcceptedHeader().sequence;
    input.request.gpuDrivenMode = m_gpuDrivenCullingMode;
    input.viewOrdinal = 0;
    m_renderPolicyDiagnostics.requestAvailable = true;
    m_renderPolicyDiagnostics.request = input.request;

    const IRHIDevice* device = m_renderContext
        ? m_renderContext->GetDevice()
        : m_featureReportDeviceForTesting;
    if (device)
    {
        const RHICapabilities& capabilities = device->GetCapabilities();
        input.capabilities.backend = device->GetBackendType();
        input.capabilities.supportsComputeVisibility =
            capabilities.supportsComputePipeline;
        input.capabilities.indexedIndirectExecution =
            capabilities.indexedIndirectExecution;
        input.capabilities.supportsFixedCountIndirect =
            capabilities.indexedIndirectExecution.supportsFixedCount;
        input.capabilities.supportsIndirectDrawCount =
            capabilities.indexedIndirectExecution.supportsCountBuffer;
        input.capabilities.supportsDescriptorResourceBindings =
            capabilities.supportsDescriptorSets;
        input.capabilities.maxDescriptorSets = capabilities.maxDescriptorSets;
    }
    input.qualification = GetGPUDrivenBackendQualification(
        input.capabilities.backend);

    const GPUCullingExecutionDecision depthGPUExecution = m_depthGPUCulling
        ? m_depthGPUCulling->GetExecutionDecision()
        : GPUCullingExecutionDecision{};
    const GPUCullingExecutionDecision opaqueGPUExecution = m_opaqueGPUCulling
        ? m_opaqueGPUCulling->GetExecutionDecision()
        : GPUCullingExecutionDecision{};

    bool depthGPUPipelineReady = false;
    bool opaqueGPUPipelineReady = false;
    if (m_pipelineCache && m_pipelineCache->IsInitialized())
    {
        depthGPUPipelineReady =
            m_pipelineCache->GetGPUDrivenDepthOnlyPipeline() != nullptr;

        RHIFormat backBufferFormat = RHIFormat::Unknown;
        if (m_externalRenderTarget.IsValid())
        {
            backBufferFormat =
                m_externalRenderTarget.colorTarget->GetFormat();
        }
        else if (m_renderContext && m_renderContext->GetCurrentBackBuffer())
        {
            backBufferFormat =
                m_renderContext->GetCurrentBackBuffer()->GetFormat();
        }
        const bool deferredRecipeRequested = UsesDeferredOpaquePath(
            m_renderScene.GetSettings().pipelineRequest.recipe);
        PostProcessStackExecuteStats stackStats;
        if (GetPostProcessStack())
        {
            // Derive the presentation transfer from the immutable surface
            // before evaluating the chain. An sRGB target expects linear
            // shader output because the target performs the final encode.
            PostProcessSettings effectiveSettings = m_postProcessSettings;
            effectiveSettings.toneMappingOutputColorSpace =
                ResolveToneMappingOutputColorSpace(backBufferFormat);
            if (deferredRecipeRequested)
            {
                effectiveSettings.enableToneMapping = true;
            }
            GetPostProcessStack()->ApplySettings(effectiveSettings);
            stackStats = GetPostProcessStack()->EvaluateEffects(backBufferFormat);
        }
        const bool postProcessActive =
            stackStats.planValid &&
            stackStats.enabledEffectCount > 0 &&
            stackStats.toneMappingBoundaryValid;
        const bool transparentSnapshotMayExecute =
            !m_transparentDrawItems.empty() && FindCompositionPass<TransparentPass>() != nullptr &&
            FindCompositionPass<TransparentPass>()->GetStatus().requestedEnabled &&
            FindCompositionPass<TransparentPass>()->GetStatus().supported &&
            FindCompositionPass<TransparentPass>()->GetStatus().enabled;
        const bool particleSnapshotMayExecute =
            FindCompositionPass<ParticleFeaturePass>() != nullptr &&
            FindCompositionPass<ParticleFeaturePass>()->GetStatus().requestedEnabled &&
            FindCompositionPass<ParticleFeaturePass>()->GetStatus().supported &&
            FindCompositionPass<ParticleFeaturePass>()->GetStatus().enabled;
        const SceneColorFormatPolicy formatPolicy =
            ResolveSceneColorFormatPolicy(backBufferFormat,
                                          postProcessActive ||
                                              deferredRecipeRequested ||
                                              transparentSnapshotMayExecute ||
                                              particleSnapshotMayExecute);
        opaqueGPUPipelineReady =
            formatPolicy.actualSceneColorFormat != RHIFormat::Unknown;
        for (const RenderDrawGroupRange& group :
             m_meshPassPreparation.opaque.groups)
        {
            opaqueGPUPipelineReady = opaqueGPUPipelineReady &&
                m_pipelineCache->GetGPUDrivenPipelineForVariant(
                    group.key.pipeline.materialVariant,
                    formatPolicy.actualSceneColorFormat,
                    group.key.pipeline.cullMode,
                    group.key.pipeline.reverseWinding) != nullptr;
        }
    }
    input.passes = {
        MakeRenderPassPolicyFacts(RenderPassKind::Depth,
                                  FindCompositionPass<DepthPrepass>(),
                                  m_meshPassPreparation.depth,
                                  depthGPUExecution,
                                  depthGPUPipelineReady),
        MakeRenderPassPolicyFacts(RenderPassKind::Opaque,
                                  FindCompositionPass<OpaquePass>(),
                                  m_meshPassPreparation.opaque,
                                  opaqueGPUExecution,
                                  opaqueGPUPipelineReady),
        MakeRenderPassPolicyFacts(RenderPassKind::Shadow,
                                  FindCompositionPass<ShadowPass>(),
                                  m_meshPassPreparation.shadow,
                                  GPUCullingExecutionDecision{},
                                  false),
        MakeRenderPassPolicyFacts(RenderPassKind::Transparent,
                                  FindCompositionPass<TransparentPass>(),
                                  m_meshPassPreparation.transparent,
                                  GPUCullingExecutionDecision{},
                                  false),
    };

    struct GPUDrivenLaneFacts
    {
        const RenderPassPolicyFacts* passFacts = nullptr;
        const GPUCulling* owner = nullptr;
        GPUCullingExecutionDecision execution{};
    };
    const std::array<GPUDrivenLaneFacts, 2> gpuDrivenLanes = {{
        {&input.passes[0], m_depthGPUCulling.get(), depthGPUExecution},
        {&input.passes[1], m_opaqueGPUCulling.get(), opaqueGPUExecution},
    }};
    const auto isRelevantTierOneLane = [](const GPUDrivenLaneFacts& lane)
    {
        return lane.passFacts != nullptr && lane.passFacts->requested &&
               lane.passFacts->supported && lane.passFacts->gpuDrivenAllowed &&
               lane.passFacts->candidatePacketCount != 0;
    };
    const auto resolveAnyRelevantLane =
        [&gpuDrivenLanes, &isRelevantTierOneLane](auto&& resolver)
    {
        bool hasRelevantLane = false;
        bool hasPendingLane = false;
        for (const GPUDrivenLaneFacts& lane : gpuDrivenLanes)
        {
            if (!isRelevantTierOneLane(lane))
            {
                continue;
            }
            hasRelevantLane = true;
            const RenderPolicyReadiness readiness = lane.owner != nullptr
                ? resolver(lane)
                : RenderPolicyReadiness::Unavailable;
            if (readiness == RenderPolicyReadiness::Ready)
            {
                return RenderPolicyReadiness::Ready;
            }
            hasPendingLane |= readiness == RenderPolicyReadiness::Pending;
        }
        return hasRelevantLane && hasPendingLane
            ? RenderPolicyReadiness::Pending
            : RenderPolicyReadiness::Unavailable;
    };
    const auto isTierOneImplementationAvailable =
        [&gpuDrivenLanes, &isRelevantTierOneLane]()
    {
        for (const GPUDrivenLaneFacts& lane : gpuDrivenLanes)
        {
            if (isRelevantTierOneLane(lane) && lane.owner != nullptr &&
                IsGPUImplementationAvailable(lane.execution))
            {
                return true;
            }
        }
        return false;
    };

    input.view.rendererAllowsGPUDriven = true;
    input.view.viewAllowsGPUDriven = true;
    input.view.implementationAvailable = isTierOneImplementationAvailable();
    const GPUCullingConfig cullingConfig = GetGPUDrivenCullingConfig();
    input.view.requestedVisibility = cullingConfig.enableDistanceCulling
        ? RenderVisibilityMode::GpuFrustumAndDistance
        : RenderVisibilityMode::GpuFrustum;
    input.view.visibilityShaderReadiness = resolveAnyRelevantLane(
        [](const GPUDrivenLaneFacts& lane)
        {
            return lane.passFacts->gpuDrivenShaderReadiness;
        });
    input.view.visibilityPipelineReadiness = resolveAnyRelevantLane(
        [](const GPUDrivenLaneFacts& lane)
        {
            return lane.passFacts->gpuDrivenPipelineReadiness;
        });
    input.view.sharedResourceReadiness = resolveAnyRelevantLane(
        [](const GPUDrivenLaneFacts& lane)
        {
            return lane.passFacts->gpuDrivenResourceReadiness;
        });
    input.view.requiredBindingReadiness = resolveAnyRelevantLane(
        [](const GPUDrivenLaneFacts& lane)
        {
            return ResolveGPUBindingReadiness(lane.execution);
        });

    const auto isPotentialGPUSceneLane =
        [&isRelevantTierOneLane](const GPUDrivenLaneFacts& lane)
    {
        return isRelevantTierOneLane(lane) && lane.owner != nullptr &&
               IsGPUImplementationAvailable(lane.execution) &&
               lane.passFacts->gpuDrivenShaderReadiness ==
                   RenderPolicyReadiness::Ready &&
               lane.passFacts->gpuDrivenPipelineReadiness ==
                   RenderPolicyReadiness::Ready &&
               lane.passFacts->gpuDrivenResourceReadiness ==
                   RenderPolicyReadiness::Ready;
    };
    bool hasPotentialGPUSceneLane = false;
    bool allPotentialGPUSceneOwnersReady = true;
    bool allPotentialGPUSceneBindingsReady = true;
    for (const GPUDrivenLaneFacts& lane : gpuDrivenLanes)
    {
        if (!isPotentialGPUSceneLane(lane))
        {
            continue;
        }
        hasPotentialGPUSceneLane = true;
        allPotentialGPUSceneOwnersReady &= lane.owner->IsGPUSceneExecutionReady();
        allPotentialGPUSceneBindingsReady &=
            ResolveGPUBindingReadiness(lane.execution) ==
                RenderPolicyReadiness::Ready;
    }
    const bool gpuSceneImplementationAvailable =
        hasPotentialGPUSceneLane && m_gpuSceneUpdate && m_gpuSceneUploader &&
        m_pipelineCache && m_pipelineCache->IsInitialized();
    const bool gpuSceneRasterReady = gpuSceneImplementationAvailable &&
        m_pipelineCache->IsGPUSceneRasterReady();
    uint64 requiredResidentVersion = 0;
    if (m_gpuSceneUpdate)
    {
        const GPUScenePublicationStats& publication =
            m_gpuSceneUpdate->GetStats();
        const GPUSceneCommittedMirror& mirror =
            m_gpuSceneUpdate->GetCommittedMirrorForUpload();
        if (m_sceneDrawClassification.IsValid() &&
            publication.HasRenderClassificationParity(
                m_sceneDrawClassification.stats.sceneObjectCount,
                m_sceneDrawClassification.stats.classifiedPacketCount,
                m_sceneDrawClassification.stats.materialBindingCount) &&
            publication.failureReason == GPUScenePublicationFailureReason::None &&
            publication.committedVersion != 0 &&
            publication.committedSourceSequence ==
                m_renderScene.GetAcceptedHeader().sequence &&
            mirror.version == publication.committedVersion)
        {
            requiredResidentVersion = publication.committedVersion;
        }
    }
    GPUSceneResidentReadiness residentReadiness;
    if (m_gpuSceneUploader && requiredResidentVersion != 0)
    {
        residentReadiness =
            m_gpuSceneUploader->QueryExactVersionReadiness(requiredResidentVersion);
    }
    input.gpuResidentScene.requiredResidentVersion = requiredResidentVersion;
    input.gpuResidentScene.implementationReadiness = gpuSceneImplementationAvailable
        ? RenderPolicyReadiness::Ready
        : RenderPolicyReadiness::Unavailable;
    input.gpuResidentScene.shaderReadiness =
        gpuSceneImplementationAvailable && allPotentialGPUSceneOwnersReady
        ? RenderPolicyReadiness::Ready
        : RenderPolicyReadiness::Unavailable;
    input.gpuResidentScene.pipelineReadiness =
        gpuSceneImplementationAvailable && gpuSceneRasterReady
            ? RenderPolicyReadiness::Ready
            : RenderPolicyReadiness::Unavailable;
    input.gpuResidentScene.bindingReadiness =
        gpuSceneImplementationAvailable && allPotentialGPUSceneBindingsReady
        ? RenderPolicyReadiness::Ready
        : RenderPolicyReadiness::Unavailable;
    switch (residentReadiness.status)
    {
        case GPUSceneResidentReadinessStatus::Ready:
            input.gpuResidentScene.resourceReadiness = residentReadiness.IsReady()
                ? RenderPolicyReadiness::Ready
                : RenderPolicyReadiness::Unavailable;
            break;
        case GPUSceneResidentReadinessStatus::Pending:
            input.gpuResidentScene.resourceReadiness =
                RenderPolicyReadiness::Pending;
            break;
        case GPUSceneResidentReadinessStatus::Unavailable:
        default:
            input.gpuResidentScene.resourceReadiness =
                RenderPolicyReadiness::Unavailable;
            break;
    }

    m_renderPolicyDiagnostics.measurement.frameSequence =
        input.request.frameSequence;
    const auto planStart = std::chrono::steady_clock::now();
    const RenderPolicyResolution resolution = ResolveRenderPolicy(input);
    const RenderFramePlanCompileResult compiled =
        CompileRenderFrameExecutionPlan(
            resolution, m_meshPassPreparation, m_renderDrawPlan);
    m_renderPolicyDiagnostics.measurement.planCpuNanoseconds =
        static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - planStart).count());
    m_renderPolicyDiagnostics.measurement.planCpuTimingAvailable = true;
    if (compiled.succeeded)
    {
        m_renderPolicyDiagnostics.planAvailable = true;
        m_renderPolicyDiagnostics.selectedPlan = compiled.plan;
        m_viewData.renderFrameExecutionPlan =
            &m_renderPolicyDiagnostics.selectedPlan;
        m_viewData.renderDrawPlan = &m_renderDrawPlan;
        m_viewData.meshPassPreparation = &m_meshPassPreparation;
        m_instanceBatchPlans = BuildSceneRenderInstanceBatchPlans(
            m_renderPolicyDiagnostics.selectedPlan,
            m_meshPassPreparation,
            m_renderVisibility,
            m_renderScene.GetSettings().instancingMode);
        m_viewData.instanceBatchPlans = &m_instanceBatchPlans;
        m_viewData.renderFrameExecutionReport =
            &m_renderPolicyDiagnostics.executionReport;
        m_renderPolicyDiagnostics.executionReport.frameSequence =
            compiled.plan.frameSequence;
        m_renderPolicyDiagnostics.executionReport.executedTier =
            compiled.plan.viewPolicy.selectedTier;
        m_renderPolicyDiagnostics.executionReport.tierFallbackReason =
            RenderPolicyReason::None;
        m_confirmedGPUDrivenTier = compiled.plan.viewPolicy.selectedTier;
        for (const RenderPassExecutionPlan& passPlan : compiled.plan.passes)
        {
            RenderPassExecutionReport report;
            report.pass = passPlan.pass;
            report.executedVisibility = passPlan.visibility;
            report.gpuDrivenLane.packetRange = passPlan.gpuEligiblePackets;
            report.gpuDrivenLane.submission = passPlan.preferredSubmission;
            report.directLane.packetRange = passPlan.directPackets;
            report.directLane.submission = passPlan.fallbackSubmission;
            report.skippedPacketCount = passPlan.partition.skippedPacketCount;
            report.reason = passPlan.reason;
            m_renderPolicyDiagnostics.executionReport.passes.push_back(report);
        }
        PopulatePlanMeasurement(m_renderPolicyDiagnostics, &m_renderDrawPlan);
    }
    ApplyRenderFramePlanProjection();
}

void SceneRenderer::ApplyRenderFramePlanProjection()
{
    bool depthGPU = false;
    bool opaqueGPU = false;
    m_gpuDrivenPolicyDecision = {};
    m_gpuDrivenPolicyDecision.requestedMode = m_gpuDrivenCullingMode;

    if (m_renderPolicyDiagnostics.planAvailable)
    {
        const RenderFrameExecutionPlan& plan =
            m_renderPolicyDiagnostics.selectedPlan;
        for (const RenderPassExecutionPlan& pass : plan.passes)
        {
            const bool gpu = pass.partition.gpuDrivenPacketCount != 0;
            depthGPU |= pass.pass == RenderPassKind::Depth && gpu;
            opaqueGPU |= pass.pass == RenderPassKind::Opaque && gpu;
        }
        m_gpuDrivenPolicyDecision.reason =
            ProjectLegacyPolicyReason(plan.viewPolicy.reason);
        m_gpuDrivenPolicyDecision.qualificationLevel =
            plan.qualification.level;
        m_gpuDrivenPolicyDecision.qualificationRevision =
            plan.qualification.revision;
        m_gpuDrivenPolicyDecision.passedQualificationGateMask =
            plan.qualification.passedGateMask;
        m_gpuDrivenPolicyDecision.requiredQualificationGateMask =
            plan.qualification.requiredGateMask;
        m_gpuDrivenPolicyDecision.missingQualificationGateMask =
            plan.qualification.requiredGateMask &
            ~plan.qualification.passedGateMask;
        m_gpuDrivenPolicyDecision.backendQualified =
            plan.qualification.level ==
            GPUDrivenQualificationLevel::Qualified;
        m_gpuDrivenPolicyDecision.capabilitiesReady =
            plan.capabilities.supportsComputeVisibility &&
            plan.capabilities.supportsDescriptorResourceBindings &&
            plan.capabilities.indexedIndirectExecution.supportsCountBuffer;
        m_gpuDrivenPolicyDecision.pipelineReady = depthGPU || opaqueGPU;
    }

    m_gpuDrivenCullingEnabled = depthGPU || opaqueGPU;
    m_gpuDrivenPolicyDecision.enabled = m_gpuDrivenCullingEnabled;
}

void SceneRenderer::BuildGPUDrivenVisibilityInputs()
{
    m_depthGPUCullingFramePrepared = false;
    m_opaqueGPUCullingFramePrepared = false;
    m_gpuDrivenTier1PreparationFailed = false;
    m_gpuDrivenFrameFailure = false;
    m_gpuDrivenFrameFailureStage = GPUDrivenFrameFailureStage::None;
    // These values are diagnostics for this render attempt, not the last
    // prepared GPU lane. Reset both owners before policy, packet, or lane
    // preflight can return so GPU-to-Direct and lane-count transitions cannot
    // leak a prior frame's upload bytes into the completed-frame report.
    if (m_depthGPUCulling)
    {
        m_depthGPUCulling->ResetFrameUploadDiagnostics();
    }
    if (m_opaqueGPUCulling)
    {
        m_opaqueGPUCulling->ResetFrameUploadDiagnostics();
    }
    // An explicitly armed capture must drain when its source slot is reused
    // even if a later policy selects Direct rendering.  This is intentionally
    // outside GPU-driven preparation; normal frames have no accepted capture
    // and therefore perform no qualification map or copy work here.
    const bool depthQualificationPending = m_depthGPUCulling &&
        m_depthGPUCulling->GetGPUSceneQualificationDiagnostics()
            .submissionAccepted;
    const bool opaqueQualificationPending = m_opaqueGPUCulling &&
        m_opaqueGPUCulling->GetGPUSceneQualificationDiagnostics()
            .submissionAccepted;
    if ((depthQualificationPending || opaqueQualificationPending) &&
        m_renderContext != nullptr)
    {
        const uint32 frameSlot = m_renderContext->GetFrameIndex();
        const bool depthSlotSelected = !depthQualificationPending ||
            m_depthGPUCulling->SetFrameSlot(frameSlot);
        const bool opaqueSlotSelected = !opaqueQualificationPending ||
            m_opaqueGPUCulling->SetFrameSlot(frameSlot);
        if (depthSlotSelected && opaqueSlotSelected)
        {
            if (RenderSubmissionTracker* tracker =
                    RenderContextInternalAccess::GetSubmissionTracker(
                        *m_renderContext))
            {
                if (depthQualificationPending)
                {
                    static_cast<void>(
                        m_depthGPUCulling->CompleteGPUSceneQualificationCapture(
                            frameSlot, *tracker));
                }
                if (opaqueQualificationPending)
                {
                    static_cast<void>(
                        m_opaqueGPUCulling->CompleteGPUSceneQualificationCapture(
                            frameSlot, *tracker));
                }
            }
        }
    }
    m_gpuDrivenCullingStats = {};
    m_instancingDiagnostics = {};
    m_gpuDrivenCullingStats.policyDecisionAvailable = true;
    m_gpuDrivenCullingStats.policyDecision = m_gpuDrivenPolicyDecision;
    m_gpuDrivenCullingStats.enabled = m_gpuDrivenCullingEnabled;
    m_gpuDrivenCullingStats.opaqueMeshPassProcessorStats =
        m_meshPassPreparation.opaque.stats;
    m_gpuDrivenCullingStats.visibilityCandidateCount =
        m_renderVisibility.diagnostics.sceneObjectCandidateCount;
    m_gpuDrivenCullingStats.cpuVisibleCandidateCount =
        m_renderVisibility.diagnostics.cpuVisibleObjectCount;
    m_gpuDrivenCullingStats.passVisibilityCandidateCount =
        m_renderVisibility.diagnostics.passCandidateCount;
    m_gpuDrivenCullingStats.invalidVisibilityBoundsCount =
        m_renderVisibility.diagnostics.invalidBoundsCount;
    m_gpuDrivenCullingStats.gpuDeferredVisibilityCandidateCount =
        m_renderVisibility.diagnostics.gpuDeferredCandidateCount;
    m_gpuDrivenCullingStats.gpuVisibilityReadbackPerformed =
        m_renderVisibility.diagnostics.gpuReadbackPerformed;
    m_gpuDrivenCullingStats.occlusionRequestedButUnavailable =
        (m_depthGPUCulling && m_depthGPUCulling->WasOcclusionRequested() &&
         !m_depthGPUCulling->IsOcclusionAvailable()) ||
        (m_opaqueGPUCulling && m_opaqueGPUCulling->WasOcclusionRequested() &&
         !m_opaqueGPUCulling->IsOcclusionAvailable());
    m_gpuDrivenCullingStats.gpuPlannedVisibilityCandidateCount = 0;
    if (m_viewData.renderFrameExecutionPlan)
    {
        for (const RenderPassExecutionPlan& passPlan :
             m_viewData.renderFrameExecutionPlan->passes)
        {
            if (passPlan.pass == RenderPassKind::Depth ||
                passPlan.pass == RenderPassKind::Opaque)
            {
                m_gpuDrivenCullingStats.gpuPlannedVisibilityCandidateCount +=
                    passPlan.gpuEligiblePackets.count;
            }
            if (passPlan.pass == RenderPassKind::Opaque)
            {
                for (uint32 offset = 0;
                     offset < passPlan.gpuEligiblePackets.count;
                     ++offset)
                {
                    const RenderDrawPacketReference& reference =
                        m_viewData.renderFrameExecutionPlan->packetReferences[
                            static_cast<size_t>(
                                passPlan.gpuEligiblePackets.first + offset)];
                    if (m_renderVisibility.IsDirectPacketVisible(
                            RenderPassKind::Opaque,
                            reference.sourcePacketIndex))
                    {
                        ++m_gpuDrivenCullingStats
                              .cpuReferenceVisibleCullableDrawItemCount;
                    }
                    else
                    {
                        ++m_gpuDrivenCullingStats
                              .cpuReferenceCulledDrawItemCount;
                    }
                }
            }
        }
    }
    m_gpuDrivenCullingStats.gpuDeferredVisibilityCandidateCount =
        m_gpuDrivenCullingStats.gpuPlannedVisibilityCandidateCount;
    m_renderVisibility.diagnostics.gpuDeferredCandidateCount =
        m_gpuDrivenCullingStats.gpuDeferredVisibilityCandidateCount;
    m_gpuDrivenCullingStats.inputOpaqueDrawItemCount =
        static_cast<uint32>(m_coarseOpaqueDrawItems.size());
    m_gpuDrivenCullingStats.inputMaskedDrawItemCount =
        static_cast<uint32>(m_coarseMaskedDrawItems.size());
    m_gpuDrivenCullingStats.outputOpaqueDrawItemCount =
        static_cast<uint32>(m_opaqueDrawItems.size());
    m_gpuDrivenCullingStats.outputMaskedDrawItemCount =
        static_cast<uint32>(m_maskedDrawItems.size());
    if (m_viewData.renderFrameExecutionPlan)
    {
        for (const RenderPassExecutionPlan& passPlan :
             m_viewData.renderFrameExecutionPlan->passes)
        {
            if (passPlan.pass != RenderPassKind::Opaque)
            {
                continue;
            }
            for (uint32 offset = 0;
                 offset < passPlan.gpuEligiblePackets.count;
                 ++offset)
            {
                const size_t referenceIndex = static_cast<size_t>(
                    passPlan.gpuEligiblePackets.first + offset);
                if (referenceIndex >=
                    m_viewData.renderFrameExecutionPlan->packetReferences.size())
                {
                    ++m_gpuDrivenCullingStats.skippedMissingGpuDataCount;
                    break;
                }
                const uint32 sourcePacketIndex =
                    m_viewData.renderFrameExecutionPlan
                        ->packetReferences[referenceIndex]
                        .sourcePacketIndex;
                if (sourcePacketIndex >= m_meshPassPreparation.opaque.packets.size())
                {
                    ++m_gpuDrivenCullingStats.skippedMissingGpuDataCount;
                    break;
                }
                const MaterialPipelineVariant variant =
                    m_meshPassPreparation.opaque.packets[sourcePacketIndex]
                        .packet.pipelineKey.materialVariant;
                if (variant == MaterialPipelineVariant::Masked)
                {
                    ++m_gpuDrivenCullingStats.cullableMaskedDrawItemCount;
                }
                else
                {
                    ++m_gpuDrivenCullingStats.cullableOpaqueDrawItemCount;
                }
            }
            break;
        }
    }
    const GPUCulling* diagnosticGPUCulling = nullptr;
    if (m_viewData.renderFrameExecutionPlan)
    {
        for (const RenderPassExecutionPlan& passPlan :
             m_viewData.renderFrameExecutionPlan->passes)
        {
            if (passPlan.gpuEligiblePackets.count == 0)
            {
                continue;
            }
            if (passPlan.pass == RenderPassKind::Opaque && m_opaqueGPUCulling)
            {
                diagnosticGPUCulling = m_opaqueGPUCulling.get();
            }
            else if (passPlan.pass == RenderPassKind::Depth &&
                     m_depthGPUCulling && diagnosticGPUCulling == nullptr)
            {
                diagnosticGPUCulling = m_depthGPUCulling.get();
            }
        }
    }
    if (diagnosticGPUCulling)
    {
        m_gpuDrivenCullingStats.executionDecisionAvailable = true;
        m_gpuDrivenCullingStats.executionDecision =
            diagnosticGPUCulling->GetExecutionDecision();
    }

    if (m_gpuDrivenCullingEnabled &&
        (m_depthGPUCulling || m_opaqueGPUCulling) &&
        m_renderResourceRegistry != nullptr)
    {
        PrepareGPUDrivenGraphCullInputs();
    }
    ConfirmGPUDrivenActualTier();
}

void SceneRenderer::PrepareGPUDrivenGraphCullInputs()
{
    if (!m_gpuDrivenCullingEnabled ||
        (!m_depthGPUCulling && !m_opaqueGPUCulling) ||
        m_renderResourceRegistry == nullptr)
    {
        return;
    }

    m_gpuDrivenCullingStats.graphInputDrawItemCount = 0;
    const RenderFrameExecutionPlan* framePlan =
        m_viewData.renderFrameExecutionPlan;
    if (framePlan == nullptr || !m_renderVisibility.structurallyValid)
    {
        return;
    }
    const bool buildGPUSceneCandidates =
        framePlan->viewPolicy.selectedTier == GPUDrivenTier::GPUResidentScene;
    const uint64 requiredResidentVersion =
        framePlan->viewPolicy.requiredResidentVersion;

    // RenderContext::BeginFrame has already waited this slot's previous
    // submission before RenderAcceptedFrame reaches SceneRenderer. Select the
    // matching per-flight CPU-written inputs before either owner uploads data.
    const uint32 frameSlot = m_renderContext != nullptr
        ? m_renderContext->GetFrameIndex()
        : 0u;
    if ((m_depthGPUCulling && !m_depthGPUCulling->SetFrameSlot(frameSlot)) ||
        (m_opaqueGPUCulling && !m_opaqueGPUCulling->SetFrameSlot(frameSlot)))
    {
        ++m_gpuDrivenCullingStats.skippedMissingGpuDataCount;
        m_gpuDrivenTier1PreparationFailed = true;
        MarkGPUDrivenFrameFailure(GPUDrivenFrameFailureStage::Tier1FrameSlot);
        return;
    }
    // RenderContext::BeginFrame waited this exact slot before this method is
    // reached. Query the tracker anyway so a stale, foreign, or lost token
    // cannot turn an explicitly armed capture into a host mapping.
    if (RenderSubmissionTracker* tracker = m_renderContext
            ? RenderContextInternalAccess::GetSubmissionTracker(*m_renderContext)
            : nullptr)
    {
        if (m_depthGPUCulling)
        {
            static_cast<void>(m_depthGPUCulling->CompleteGPUSceneQualificationCapture(
                frameSlot, *tracker));
        }
        if (m_opaqueGPUCulling)
        {
            static_cast<void>(m_opaqueGPUCulling->CompleteGPUSceneQualificationCapture(
                frameSlot, *tracker));
        }
    }

    const auto preparePass =
        [this, framePlan, buildGPUSceneCandidates, requiredResidentVersion](RenderPassKind pass,
                          const MeshPassPacketStream& stream,
                          GPUCulling* owner,
                          bool& outPrepared)
    {
        outPrepared = false;
        if (owner == nullptr)
        {
            return;
        }

        const RenderPassExecutionPlan* passPlan = nullptr;
        for (const RenderPassExecutionPlan& candidatePlan : framePlan->passes)
        {
            if (candidatePlan.pass == pass)
            {
                passPlan = &candidatePlan;
                break;
            }
        }
        if (passPlan == nullptr || passPlan->gpuEligiblePackets.count == 0)
        {
            return;
        }
        if (!ValidatePlannedGPUDrivenPacketRange(*framePlan, pass, stream) ||
            passPlan->gpuEligiblePackets.count > owner->GetConfig().maxInstances)
        {
            ++m_gpuDrivenCullingStats.skippedMissingGpuDataCount;
            MarkGPUDrivenFrameFailure(
                GPUDrivenFrameFailureStage::Tier1PacketRange);
            return;
        }

        // The post-fence capture must start from the immutable execution plan,
        // before this owner starts building its mutable active-row stream.
        // Record every terminal partition so Direct/Skip work cannot be
        // mistaken for an owner/candidate omission.
        if (owner->IsGPUSceneQualificationCaptureArmed())
        {
            GPUCullingQualificationInputPlan inputPlan;
            inputPlan.expectedPacketCount =
                passPlan->partition.inputPacketCount;
            inputPlan.expectedGPUInputPacketCount =
                passPlan->gpuEligiblePackets.count;
            inputPlan.directPacketCount = passPlan->directPackets.count;
            inputPlan.skippedPacketCount = passPlan->skippedPackets.count;
            inputPlan.planPacketIdentityHash = 0xCBF29CE484222325ull;
            const std::array<DrawPacketRange, 3> ranges = {
                passPlan->gpuEligiblePackets,
                passPlan->directPackets,
                passPlan->skippedPackets};
            std::vector<bool> seenPacketIndices(
                inputPlan.expectedPacketCount, false);
            uint64 terminalPacketCount = 0;
            bool planCoverageValid = inputPlan.expectedPacketCount != 0;
            for (uint32 rangeIndex = 0; rangeIndex < ranges.size() &&
                 planCoverageValid; ++rangeIndex)
            {
                const DrawPacketRange range = ranges[rangeIndex];
                const uint64 rangeEnd =
                    static_cast<uint64>(range.first) + range.count;
                if (rangeEnd > framePlan->packetReferences.size())
                {
                    planCoverageValid = false;
                    break;
                }
                terminalPacketCount += range.count;
                for (uint64 referenceIndex = range.first;
                     referenceIndex < rangeEnd; ++referenceIndex)
                {
                    const RenderDrawPacketReference& reference =
                        framePlan->packetReferences[
                            static_cast<size_t>(referenceIndex)];
                    if (reference.pass != pass ||
                        reference.sourcePacketIndex >= seenPacketIndices.size() ||
                        seenPacketIndices[reference.sourcePacketIndex] ||
                        reference.sourcePacketIndex >= stream.packets.size())
                    {
                        planCoverageValid = false;
                        break;
                    }
                    seenPacketIndices[reference.sourcePacketIndex] = true;
                    const uint64 identityHash =
                        HashGPUCullingQualificationPacketReference(reference);
                    if (identityHash == 0)
                    {
                        planCoverageValid = false;
                        break;
                    }
                    inputPlan.planPacketIdentityHash =
                        MixGPUCullingQualificationIdentity(
                            inputPlan.planPacketIdentityHash, identityHash);
                    if (rangeIndex == 0)
                    {
                        inputPlan.expectedGPUInputIdentityHashes.push_back(
                            identityHash);
                        // Direct visibility is an independent canonical
                        // source.  Retain its GPU-eligible subset before the
                        // mutable owner/candidate stream is collected, then
                        // require the post-fence GPU visibility result to
                        // include it.  GPU-conservative extras remain
                        // diagnostic-only.
                        if (m_renderVisibility.IsDirectPacketVisible(
                                pass, reference.sourcePacketIndex))
                        {
                            inputPlan.expectedDirectVisibleIdentityHashes
                                .push_back(identityHash);
                        }
                    }
                }
            }
            inputPlan.exactlyOncePartitioned = planCoverageValid &&
                terminalPacketCount == inputPlan.expectedPacketCount &&
                std::all_of(seenPacketIndices.begin(), seenPacketIndices.end(),
                            [](bool seen) { return seen; }) &&
                passPlan->identityAccounting.IsExactlyOnce();
            // Qualification is observational only.  A malformed accounting
            // record is retained as fail-closed evidence by the owner, but it
            // must not remove a draw packet or alter this frame's renderer
            // execution.
            static_cast<void>(
                owner->BeginGPUSceneQualificationInputCoverage(inputPlan));
        }

        // Complete structural preflight occurs before the per-pass owner is
        // reset. A malformed Depth plan therefore cannot touch Opaque state,
        // and vice versa.
        for (uint32 offset = 0;
             offset < passPlan->gpuEligiblePackets.count;
             ++offset)
        {
            const uint32 sourcePacketIndex =
                stream.sortedGPUCandidatePacketIndices[offset];
            const RenderDrawPacketReference& reference =
                framePlan->packetReferences[static_cast<size_t>(
                    passPlan->gpuEligiblePackets.first + offset)];
            const RenderVisibilityCandidate* visibilityCandidate =
                m_renderCandidates.Find(pass, sourcePacketIndex);
            if (reference.sourcePacketIndex != sourcePacketIndex ||
                visibilityCandidate == nullptr ||
                !visibilityCandidate->objectVisible ||
                !visibilityCandidate->drawable ||
                visibilityCandidate->objectIndex >= m_renderScene.GetObjectCount() ||
                sourcePacketIndex >= stream.packets.size() ||
                stream.packets[sourcePacketIndex].packet.arguments.indexCount == 0)
            {
                ++m_gpuDrivenCullingStats.skippedMissingGpuDataCount;
                MarkGPUDrivenFrameFailure(
                    GPUDrivenFrameFailureStage::Tier1PacketRange);
                return;
            }
        }

        owner->BeginFrame(
            m_renderScene.GetAcceptedHeader().requiredSceneRevision,
            m_renderScene.GetGPUSceneChangedObjectIds(),
            m_renderScene.GetGPUSceneRemovedObjectIds(),
            m_renderScene.IsFullGPUSceneMutation());
        bool gpuSceneCandidateStreamValid = buildGPUSceneCandidates &&
            requiredResidentVersion != 0 && m_gpuSceneUpdate != nullptr &&
            owner->IsGPUSceneExecutionReady();
        uint32 plannedOffset = 0;
        for (const RenderDrawGroupRange& group : stream.groups)
        {
            const RenderDrawGroupKey& key = group.key;
            if (group.count == 0 ||
                group.first >= stream.sortedGPUCandidatePacketIndices.size() ||
                group.count > stream.sortedGPUCandidatePacketIndices.size() -
                    group.first)
            {
                ++m_gpuDrivenCullingStats.skippedMissingGpuDataCount;
                owner->EndFrame();
                MarkGPUDrivenFrameFailure(
                    GPUDrivenFrameFailureStage::Tier1DrawGroup);
                return;
            }
            const uint32 leaderSourcePacketIndex =
                stream.sortedGPUCandidatePacketIndices[group.first];
            if (leaderSourcePacketIndex >= stream.packets.size())
            {
                ++m_gpuDrivenCullingStats.skippedMissingGpuDataCount;
                owner->EndFrame();
                MarkGPUDrivenFrameFailure(
                    GPUDrivenFrameFailureStage::Tier1DrawGroup);
                return;
            }
            const RenderDrawPacket& leaderPacket =
                stream.packets[leaderSourcePacketIndex].packet;
            const uint64 meshId =
                (static_cast<uint64>(key.geometry.mesh.slot) << 32U) |
                key.geometry.mesh.generation;
            const uint64 materialId =
                (static_cast<uint64>(leaderPacket.materialKey.material.slot) << 32U) |
                leaderPacket.materialKey.material.generation;
            const uint32 groupIndex = owner->BeginDrawGroup(
                meshId,
                materialId,
                key.pipeline.materialVariant,
                key.geometry.mesh,
                leaderPacket.materialKey.material,
                key);
            if (groupIndex == RVX_INVALID_INDEX)
            {
                ++m_gpuDrivenCullingStats.skippedMissingGpuDataCount;
                owner->EndFrame();
                MarkGPUDrivenFrameFailure(
                    GPUDrivenFrameFailureStage::Tier1DrawGroup);
                return;
            }

            for (uint32 offset = 0; offset < group.count; ++offset)
            {
                const uint32 sourcePacketIndex =
                    stream.sortedGPUCandidatePacketIndices[group.first + offset];
                const size_t qualificationReferenceIndex = static_cast<size_t>(
                    passPlan->gpuEligiblePackets.first + plannedOffset);
                if (qualificationReferenceIndex >=
                    framePlan->packetReferences.size())
                {
                    ++m_gpuDrivenCullingStats.skippedMissingGpuDataCount;
                    owner->EndDrawGroup();
                    owner->EndFrame();
                    MarkGPUDrivenFrameFailure(
                        GPUDrivenFrameFailureStage::Tier1PacketRange);
                    return;
                }
                const RenderDrawPacketReference& qualificationReference =
                    framePlan->packetReferences[qualificationReferenceIndex];
                const uint64 qualificationIdentityHash =
                    HashGPUCullingQualificationPacketReference(
                        qualificationReference);
                const MeshPassProcessorResult& result =
                    stream.packets[sourcePacketIndex];
                const RenderVisibilityCandidate* visibilityCandidate =
                    m_renderCandidates.Find(pass, sourcePacketIndex);
                GPUIndexedDrawDesc drawDesc;
                drawDesc.indexCount = result.packet.arguments.indexCount;
                drawDesc.firstIndex = result.packet.arguments.firstIndex;
                drawDesc.vertexOffset = result.packet.arguments.vertexOffset;
                const uint32 rasterInstanceIndex = visibilityCandidate != nullptr
                    ? owner->AddVisibilityCandidateInstance(
                          m_renderScene,
                          *visibilityCandidate,
                          result.packet,
                          drawDesc,
                          m_renderResourceRegistry)
                    : RVX_INVALID_INDEX;
                if (rasterInstanceIndex == RVX_INVALID_INDEX)
                {
                    ++m_gpuDrivenCullingStats.skippedMissingGpuDataCount;
                    owner->EndDrawGroup();
                    owner->EndFrame();
                    MarkGPUDrivenFrameFailure(
                        GPUDrivenFrameFailureStage::Tier1Instance);
                    return;
                }

                // GPU-scene candidates are an optional companion stream. A
                // failed exact lookup must not disturb the authoritative Tier
                // 1 instance stream, and no ordinal-based reconstruction is
                // permitted after that failure.
                bool ownerCandidateMapped = !buildGPUSceneCandidates;
                if (gpuSceneCandidateStreamValid)
                {
                    const std::optional<GPUSceneAcceptedDrawLookup> acceptedDraw =
                        m_gpuSceneUpdate->ResolveAcceptedDraw(
                            m_renderScene, *visibilityCandidate, result.packet);
                    const uint32 requiredPassMask =
                        GetGPUSceneCullingCandidatePassMask(pass);
                    const std::vector<GPUCullingDrawGroup>& drawGroups =
                        owner->GetDrawGroups();
                    if (!acceptedDraw || !acceptedDraw->IsValid() ||
                        acceptedDraw->committedVersion != requiredResidentVersion ||
                        requiredPassMask == 0 || groupIndex >= drawGroups.size())
                    {
                        owner->InvalidateGPUSceneCandidates();
                        gpuSceneCandidateStreamValid = false;
                    }
                    else
                    {
                        GPUSceneCullingCandidate gpuSceneCandidate;
                        gpuSceneCandidate.primitiveSlot = acceptedDraw->primitive.slot;
                        gpuSceneCandidate.primitiveGeneration =
                            acceptedDraw->primitive.generation;
                        gpuSceneCandidate.drawSlot = acceptedDraw->draw.slot;
                        gpuSceneCandidate.drawGeneration = acceptedDraw->draw.generation;
                        gpuSceneCandidate.objectIdLow =
                            static_cast<uint32>(result.packet.objectId);
                        gpuSceneCandidate.objectIdHigh = static_cast<uint32>(
                            result.packet.objectId >> 32U);
                        gpuSceneCandidate.requiredPassMask = requiredPassMask;
                        gpuSceneCandidate.drawGroupIndex = groupIndex;
                        gpuSceneCandidate.drawGroupVisibleOffset =
                            drawGroups[groupIndex].visibleInstanceOffset;
                        gpuSceneCandidate.rasterInstanceIndex = rasterInstanceIndex;
                        gpuSceneCandidate.materialParameterSlot =
                            result.packet.materialKey.material.slot;
                        if (!owner->AddGPUSceneCandidate(
                                gpuSceneCandidate,
                                requiredResidentVersion))
                        {
                            owner->InvalidateGPUSceneCandidates();
                            gpuSceneCandidateStreamValid = false;
                        }
                        else
                        {
                            ownerCandidateMapped = true;
                        }
                    }
                }
                if (owner->IsGPUSceneQualificationCaptureArmed() &&
                    ownerCandidateMapped)
                {
                    // As above, collection evidence is never permitted to
                    // change rendering behavior.  Failure remains visible in
                    // the qualification diagnostics and report.
                    static_cast<void>(
                        owner->ObserveGPUSceneQualificationInputIdentity(
                            qualificationIdentityHash));
                }
                ++plannedOffset;
            }
            owner->EndDrawGroup();
        }

        if (plannedOffset != passPlan->gpuEligiblePackets.count ||
            owner->GetInstanceCount() != passPlan->gpuEligiblePackets.count)
        {
            ++m_gpuDrivenCullingStats.skippedMissingGpuDataCount;
            owner->EndFrame();
            MarkGPUDrivenFrameFailure(
                GPUDrivenFrameFailureStage::Tier1FinalCount);
            return;
        }
        owner->EndFrame();
        outPrepared = true;
        m_gpuDrivenCullingStats.graphInputDrawItemCount +=
            owner->GetInstanceCount();
    };

    preparePass(RenderPassKind::Depth,
                m_meshPassPreparation.depth,
                m_depthGPUCulling.get(),
                m_depthGPUCullingFramePrepared);
    preparePass(RenderPassKind::Opaque,
                m_meshPassPreparation.opaque,
                m_opaqueGPUCulling.get(),
                m_opaqueGPUCullingFramePrepared);
}

const char* SceneRenderer::GetGPUDrivenFrameFailureStageName(
    GPUDrivenFrameFailureStage stage) noexcept
{
    switch (stage)
    {
    case GPUDrivenFrameFailureStage::None:
        return "none";
    case GPUDrivenFrameFailureStage::Tier1FrameSlot:
        return "tier1-frame-slot";
    case GPUDrivenFrameFailureStage::Tier1PacketRange:
        return "tier1-packet-range";
    case GPUDrivenFrameFailureStage::Tier1DrawGroup:
        return "tier1-draw-group";
    case GPUDrivenFrameFailureStage::Tier1Instance:
        return "tier1-instance";
    case GPUDrivenFrameFailureStage::Tier1FinalCount:
        return "tier1-final-count";
    case GPUDrivenFrameFailureStage::Tier1Seal:
        return "tier1-seal";
    case GPUDrivenFrameFailureStage::TierConfirmationMissingTier1:
        return "tier-confirmation-missing-tier1";
    case GPUDrivenFrameFailureStage::TierConfirmationNoFallback:
        return "tier-confirmation-no-fallback";
    case GPUDrivenFrameFailureStage::TierConfirmationResidentNotReady:
        return "tier-confirmation-resident-not-ready";
    case GPUDrivenFrameFailureStage::GPUSceneLeaseAcquire:
        return "gpu-scene-lease-acquire";
    case GPUDrivenFrameFailureStage::GPUSceneLeaseSeal:
        return "gpu-scene-lease-seal";
    case GPUDrivenFrameFailureStage::GPUSceneRasterBinding:
        return "gpu-scene-raster-binding";
    case GPUDrivenFrameFailureStage::PassRegistration:
        return "pass-registration";
    case GPUDrivenFrameFailureStage::CullCommandRecording:
        return "cull-command-recording";
    case GPUDrivenFrameFailureStage::RasterCommandRecording:
        return "raster-command-recording";
    case GPUDrivenFrameFailureStage::AccessSnapshotCommit:
        return "access-snapshot-commit";
    case GPUDrivenFrameFailureStage::GraphExecution:
        return "graph-execution";
    case GPUDrivenFrameFailureStage::GraphAdoption:
        return "graph-adoption";
    }
    return "unknown";
}

void SceneRenderer::MarkGPUDrivenFrameFailure(
    GPUDrivenFrameFailureStage stage) noexcept
{
    if (stage == GPUDrivenFrameFailureStage::None)
    {
        return;
    }
    if (m_gpuDrivenFrameFailureStage == GPUDrivenFrameFailureStage::None)
    {
        m_gpuDrivenFrameFailureStage = stage;
    }
    m_gpuDrivenFrameFailure = true;
    m_gpuDrivenTierExecutionTestProbe.frameFailed = true;
    if (!m_renderPolicyDiagnostics.planAvailable)
    {
        return;
    }
    RenderFrameExecutionReport& report =
        m_renderPolicyDiagnostics.executionReport;
    report.status = RenderExecutionStatus::Failed;
    for (RenderPassExecutionReport& passReport : report.passes)
    {
        passReport.status = RenderExecutionStatus::Failed;
        passReport.reason = RenderPolicyReason::UnexpectedRecordingFailure;
    }
}
void SceneRenderer::ConfirmGPUDrivenActualTier()
{
    const RenderFrameExecutionPlan* framePlan =
        m_viewData.renderFrameExecutionPlan;
    m_confirmedGPUDrivenTier = framePlan
        ? framePlan->viewPolicy.selectedTier
        : GPUDrivenTier::Direct;
    if (framePlan == nullptr)
    {
        return;
    }

    RenderFrameExecutionReport& report =
        m_renderPolicyDiagnostics.executionReport;
    report.executedTier = m_confirmedGPUDrivenTier;
    report.tierFallbackReason = RenderPolicyReason::None;
    const auto laneIsPlanned = [framePlan](RenderPassKind pass)
    {
        for (const RenderPassExecutionPlan& passPlan : framePlan->passes)
        {
            if (passPlan.pass == pass)
            {
                return passPlan.gpuEligiblePackets.count != 0;
            }
        }
        return false;
    };
    const bool depthTier1Complete = !laneIsPlanned(RenderPassKind::Depth) ||
        (m_depthGPUCullingFramePrepared && m_depthGPUCulling != nullptr &&
         m_depthGPUCulling->GetInstanceCount() != 0);
    const bool opaqueTier1Complete = !laneIsPlanned(RenderPassKind::Opaque) ||
        (m_opaqueGPUCullingFramePrepared && m_opaqueGPUCulling != nullptr &&
         m_opaqueGPUCulling->GetInstanceCount() != 0);
    if (m_gpuDrivenTier1PreparationFailed || !depthTier1Complete ||
        !opaqueTier1Complete)
    {
        MarkGPUDrivenFrameFailure(
            GPUDrivenFrameFailureStage::TierConfirmationMissingTier1);
        return;
    }
    if (m_confirmedGPUDrivenTier != GPUDrivenTier::GPUResidentScene)
    {
        return;
    }

    const uint64 requiredResidentVersion =
        framePlan->viewPolicy.requiredResidentVersion;
    const bool depthCompanionComplete = !laneIsPlanned(RenderPassKind::Depth) ||
        (requiredResidentVersion != 0 && m_depthGPUCulling != nullptr &&
         m_depthGPUCulling->HasCompleteGPUSceneCandidates(
             requiredResidentVersion));
    const bool opaqueCompanionComplete = !laneIsPlanned(RenderPassKind::Opaque) ||
        (requiredResidentVersion != 0 && m_opaqueGPUCulling != nullptr &&
         m_opaqueGPUCulling->HasCompleteGPUSceneCandidates(
             requiredResidentVersion));
    if (depthCompanionComplete && opaqueCompanionComplete)
    {
        // Readiness is an observation only.  The graph lease is acquired
        // later, after all Tier 2 preflight checks have succeeded.
        const GPUSceneResidentReadiness readiness =
            m_gpuSceneUploader
                ? m_gpuSceneUploader->QueryExactVersionReadiness(
                      requiredResidentVersion)
                : GPUSceneResidentReadiness{};
        if (readiness.IsReady())
        {
            m_renderPolicyDiagnostics.gpuSceneResidentVersion =
                readiness.residentVersion;
        }
        return;
    }
    if (framePlan->viewPolicy.immediateFallbackTier !=
        GPUDrivenTier::IndirectGrouped)
    {
        MarkGPUDrivenFrameFailure(
            (depthCompanionComplete && opaqueCompanionComplete)
                ? GPUDrivenFrameFailureStage::TierConfirmationResidentNotReady
                : GPUDrivenFrameFailureStage::TierConfirmationNoFallback);
        return;
    }
    m_confirmedGPUDrivenTier = GPUDrivenTier::IndirectGrouped;
    report.executedTier = m_confirmedGPUDrivenTier;
    report.tierFallbackReason = RenderPolicyReason::PlannedFallback;
}
void SceneRenderer::ApplyObjectMotionHistory()
{
    for (size_t objectIndex = 0; objectIndex < m_renderScene.GetObjectCount(); ++objectIndex)
    {
        RenderObject& object = m_renderScene.GetMutableObject(objectIndex);
        object.previousWorldMatrix = object.worldMatrix;
        object.previousWorldMatrixValid = 0;

        const uint64 historyKey = GetRenderObjectHistoryKey(object);
        if (historyKey == 0 || m_viewData.resetTemporalHistory)
        {
            continue;
        }

        const auto previousIt = m_previousObjectWorldMatrices.find(historyKey);
        if (previousIt == m_previousObjectWorldMatrices.end() ||
            !previousIt->second.IsCompatible(object))
        {
            continue;
        }

        object.previousWorldMatrix = previousIt->second.worldMatrix;
        object.previousWorldMatrixValid = 1;
    }
}

void SceneRenderer::UpdateObjectMotionHistory()
{
    std::unordered_map<uint64, RenderObjectMotionHistory> currentObjectWorldMatrices;
    currentObjectWorldMatrices.reserve(m_renderScene.GetObjectCount());

    for (const RenderObject& object : m_renderScene.GetObjects())
    {
        const uint64 historyKey = GetRenderObjectHistoryKey(object);
        if (historyKey != 0)
        {
            currentObjectWorldMatrices[historyKey] = RenderObjectMotionHistory::Capture(object);
        }
    }

    m_previousObjectWorldMatrices = std::move(currentObjectWorldMatrices);
}

const RayTracingSceneManagerStats& SceneRenderer::GetRayTracingSceneStats() const
{
    return m_rayTracingSceneStats;
}

const CameraVelocityPassStats& SceneRenderer::GetCameraVelocityStats() const
{
    static const CameraVelocityPassStats emptyStats;
    if (m_activeRenderPassResults &&
        m_activeRenderPassResults->identity == m_activeRenderPassIdentity)
    {
        return m_activeRenderPassResults->cameraVelocityStats;
    }
    return FindCompositionPass<CameraVelocityPass>() ? FindCompositionPass<CameraVelocityPass>()->GetStats()
                                : emptyStats;
}

const ObjectVelocityPassStats& SceneRenderer::GetObjectVelocityStats() const
{
    static const ObjectVelocityPassStats emptyStats;
    if (m_activeRenderPassResults &&
        m_activeRenderPassResults->identity == m_activeRenderPassIdentity)
    {
        return m_activeRenderPassResults->objectVelocityStats;
    }
    return FindCompositionPass<ObjectVelocityPass>() ? FindCompositionPass<ObjectVelocityPass>()->GetStats() : emptyStats;
}

  const ParticleFeaturePassStats& SceneRenderer::GetParticleFeaturePassStats() const
{
    static const ParticleFeaturePassStats emptyStats;
    return FindCompositionPass<ParticleFeaturePass>() ? FindCompositionPass<ParticleFeaturePass>()->GetStats() : emptyStats;
  }

  const WaterFeaturePassStats& SceneRenderer::GetWaterFeaturePassStats() const
  {
      static const WaterFeaturePassStats emptyStats;
      return FindCompositionPass<WaterFeaturePass>() ? FindCompositionPass<WaterFeaturePass>()->GetStats() : emptyStats;
  }

RHIAccelerationStructure* SceneRenderer::GetRayTracingTopLevelAS() const
{
    return m_rayTracingSceneManager ? m_rayTracingSceneManager->GetTopLevelAS() : nullptr;
}

const RayTracedShadowPassStats& SceneRenderer::GetRayTracedShadowStats() const
{
    static const RayTracedShadowPassStats emptyStats;
    // This accessor feeds the next frame's GPU budget policy.  It must return
    // the completion-published sample rather than a current graph-owned
    // snapshot, which may be unsubmitted, rejected, or awaiting timestamp
    // readback.
    return FindCompositionPass<RayTracedShadowPass>() ? FindCompositionPass<RayTracedShadowPass>()->GetStats() : emptyStats;
}

const RayTracedReflectionPassStats& SceneRenderer::GetRayTracedReflectionStats() const
{
    static const RayTracedReflectionPassStats emptyStats;
    return FindCompositionPass<RayTracedReflectionPass>() ? FindCompositionPass<RayTracedReflectionPass>()->GetStats() : emptyStats;
}

const RayTracedReflectionDenoisePassStats& SceneRenderer::GetRayTracedReflectionDenoiseStats() const
{
    static const RayTracedReflectionDenoisePassStats emptyStats;
    return FindCompositionPass<RayTracedReflectionDenoisePass>() ? FindCompositionPass<RayTracedReflectionDenoisePass>()->GetStats() : emptyStats;
}

const RayTracedReflectionCompositePassStats& SceneRenderer::GetRayTracedReflectionCompositeStats() const
{
    static const RayTracedReflectionCompositePassStats emptyStats;
    return FindCompositionPass<RayTracedReflectionCompositePass>() ? FindCompositionPass<RayTracedReflectionCompositePass>()->GetStats() : emptyStats;
}

SceneRayTracingFrameStats SceneRenderer::GetRayTracingFrameStats() const
{
    // Frame diagnostics intentionally report the active graph snapshot after
    // execution.  Budget consumers use GetRayTracedShadowStats(), above, and
    // therefore observe only submitted/completed timing data.
    const RayTracedShadowPassStats& shadowStats =
        m_activeRenderPassResults &&
        m_activeRenderPassResults->identity == m_activeRenderPassIdentity
            ? m_activeRenderPassResults->rayTracedShadowStats
            : GetRayTracedShadowStats();
    const bool hasExactGraphResults = m_activeRenderPassResults &&
        m_activeRenderPassResults->identity == m_activeRenderPassIdentity;
    const RayTracedReflectionPassStats& reflectionStats =
        hasExactGraphResults
            ? m_activeRenderPassResults->rayTracedReflectionStats
            : GetRayTracedReflectionStats();
    const RayTracedReflectionDenoisePassStats& denoiseStats =
        hasExactGraphResults
            ? m_activeRenderPassResults->rayTracedReflectionDenoiseStats
            : GetRayTracedReflectionDenoiseStats();
    const RayTracedReflectionCompositePassStats& compositeStats =
        hasExactGraphResults
            ? m_activeRenderPassResults->rayTracedReflectionCompositeStats
            : GetRayTracedReflectionCompositeStats();

    SceneRayTracingFrameStats stats = m_rayTracingFrameBudgetStats;
    stats.sceneSupported = m_rayTracingSceneStats.supported;
    stats.scenePrepared = m_rayTracingSceneStats.prepared;
    stats.tlasAvailable = m_rayTracingSceneStats.hasTopLevelAS || GetRayTracingTopLevelAS() != nullptr;
    stats.shadowRequested = shadowStats.requested;
    stats.shadowSupported = shadowStats.supported;
    stats.shadowRecorded = shadowStats.dispatchRecorded;
    stats.reflectionRequested = reflectionStats.requested;
    stats.reflectionSupported = reflectionStats.supported;
    stats.reflectionRecorded = reflectionStats.dispatchRecorded;
    stats.reflectionDenoiseRequested = denoiseStats.requested;
    stats.reflectionDenoiseSupported = denoiseStats.supported;
    stats.reflectionDenoiseRecorded = denoiseStats.denoiseRecorded;
    stats.reflectionCompositeRequested = compositeStats.requested;
    stats.reflectionCompositeSupported = compositeStats.supported;
    stats.reflectionCompositeRecorded = compositeStats.compositeRecorded;
    stats.denoiseFallbackToRaw = compositeStats.denoiseFallbackToRaw;
    stats.reflectionMaterialTextureTableAvailable = reflectionStats.materialTextureTableAvailable;
    stats.reflectionGeometryMetadataAvailable = reflectionStats.geometryMetadataAvailable;
    stats.reflectionGeometryTableAvailable = reflectionStats.geometryTableAvailable;
    stats.reflectionExposureStateAvailable =
        reflectionStats.exposureStateAvailable;
    stats.reflectionExposureStateReadDeclared =
        reflectionStats.exposureStateReadDeclared;
    stats.reflectionExposureStateDescriptorBound =
        reflectionStats.exposureStateDescriptorBound;
    stats.reflectionExposureStateContentHash =
        reflectionStats.exposureStateContentHash;
    stats.shadowMaterialTextureTableAvailable = shadowStats.materialTextureTableAvailable;
    stats.shadowAlphaMetadataAvailable = shadowStats.alphaMetadataAvailable;
    stats.shadowAlphaTextureTableAvailable = shadowStats.alphaTextureTableAvailable;
    stats.shadowAlphaGeometryTableAvailable = shadowStats.alphaGeometryTableAvailable;
    stats.shadowHistoryAvailable = shadowStats.historyAvailable;
    stats.shadowDepthHistoryAvailable = shadowStats.depthHistoryAvailable;
    stats.shadowNormalHistoryAvailable = shadowStats.normalHistoryAvailable;
    stats.shadowHistoryReset = shadowStats.historyReset;
    stats.shadowHistoryRecreated = shadowStats.historyRecreated;
    stats.shadowHistoryResolutionChanged = shadowStats.historyResolutionChanged;
    stats.shadowHistoryConfigChanged = shadowStats.historyConfigChanged;
    stats.shadowTemporalAccumulated = shadowStats.temporalAccumulated;
    stats.reflectionHistoryAvailable = reflectionStats.historyAvailable;
    stats.reflectionDepthHistoryAvailable = reflectionStats.depthHistoryAvailable;
    stats.reflectionNormalHistoryAvailable = reflectionStats.normalHistoryAvailable;
    stats.reflectionHistoryReset = reflectionStats.historyReset;
    stats.reflectionHistoryRecreated = reflectionStats.historyRecreated;
    stats.reflectionHistoryResolutionChanged = reflectionStats.historyResolutionChanged;
    stats.reflectionHistoryConfigChanged = reflectionStats.historyConfigChanged;
    stats.reflectionTemporalAccumulated = reflectionStats.temporalAccumulated;
    stats.shadowGpuTimingSupported = shadowStats.gpuTimingSupported;
    stats.shadowGpuTimingQueriesRecorded = shadowStats.gpuTimingQueriesRecorded;
    stats.shadowGpuTimingResolveRecorded = shadowStats.gpuTimingResolveRecorded;
    stats.shadowGpuTimingReadbackBufferAvailable = shadowStats.gpuTimingReadbackBufferAvailable;
    stats.shadowGpuTimingResultAvailable = shadowStats.gpuTimingResultAvailable;
    stats.reflectionGpuTimingSupported = reflectionStats.gpuTimingSupported;
    stats.reflectionGpuTimingQueriesRecorded = reflectionStats.gpuTimingQueriesRecorded;
    stats.reflectionGpuTimingResolveRecorded = reflectionStats.gpuTimingResolveRecorded;
    stats.reflectionGpuTimingReadbackBufferAvailable = reflectionStats.gpuTimingReadbackBufferAvailable;
    stats.reflectionGpuTimingResultAvailable = reflectionStats.gpuTimingResultAvailable;
    stats.shadowGpuTimestampFrequency = shadowStats.gpuTimestampFrequency;
    stats.reflectionGpuTimestampFrequency = reflectionStats.gpuTimestampFrequency;
    stats.shadowGpuTimingReadbackBytes = shadowStats.gpuTimingReadbackBytes;
    stats.reflectionGpuTimingReadbackBytes = reflectionStats.gpuTimingReadbackBytes;
    stats.shadowGpuTimingStartTimestamp = shadowStats.gpuTimingStartTimestamp;
    stats.shadowGpuTimingEndTimestamp = shadowStats.gpuTimingEndTimestamp;
    stats.shadowGpuTimingElapsedTicks = shadowStats.gpuTimingElapsedTicks;
    stats.reflectionGpuTimingStartTimestamp = reflectionStats.gpuTimingStartTimestamp;
    stats.reflectionGpuTimingEndTimestamp = reflectionStats.gpuTimingEndTimestamp;
    stats.reflectionGpuTimingElapsedTicks = reflectionStats.gpuTimingElapsedTicks;
    stats.shadowGpuTimingElapsedMs = shadowStats.gpuTimingElapsedMs;
    stats.reflectionGpuTimingElapsedMs = reflectionStats.gpuTimingElapsedMs;
    stats.totalMeasuredRayTracingGpuMs =
        (stats.shadowGpuTimingResultAvailable ? stats.shadowGpuTimingElapsedMs : 0.0f) +
        (stats.reflectionGpuTimingResultAvailable ? stats.reflectionGpuTimingElapsedMs : 0.0f);
    stats.shadowGpuTimingReadbackBufferCount = shadowStats.gpuTimingReadbackBufferCount;
    stats.reflectionGpuTimingReadbackBufferCount = reflectionStats.gpuTimingReadbackBufferCount;
    stats.shadowGpuTimingReadbackFrameIndex = shadowStats.gpuTimingReadbackFrameIndex;
    stats.reflectionGpuTimingReadbackFrameIndex = reflectionStats.gpuTimingReadbackFrameIndex;
    stats.shadowGpuTimingStartQueryIndex = shadowStats.gpuTimingStartQueryIndex;
    stats.shadowGpuTimingEndQueryIndex = shadowStats.gpuTimingEndQueryIndex;
    stats.reflectionGpuTimingStartQueryIndex = reflectionStats.gpuTimingStartQueryIndex;
    stats.reflectionGpuTimingEndQueryIndex = reflectionStats.gpuTimingEndQueryIndex;
    stats.estimatedShadowRayCount = shadowStats.estimatedRayCount;
    stats.estimatedReflectionRayCount = reflectionStats.estimatedRayCount;
    stats.estimatedTotalRayCount = stats.estimatedShadowRayCount + stats.estimatedReflectionRayCount;
    stats.estimatedReflectionDenoiseTapCount = denoiseStats.estimatedTapCount;
    stats.trackedResourceBudget = m_rayTracingBudgetSettings.maxTrackedResourceBytes;
    stats.blasCacheEvictionFrameThreshold = m_rayTracingBudgetSettings.blasCacheEvictionFrameThreshold;
    stats.cachedBLASCount = m_rayTracingSceneStats.cachedBLASCount;
    stats.evictedBLASCount = m_rayTracingSceneStats.evictedBLASCount;
    stats.resourceBudgetEvictedBLASCount = m_rayTracingSceneStats.resourceBudgetEvictedBLASCount;
    stats.releasedBLASScratchCount = m_rayTracingSceneStats.releasedBLASScratchCount;
    stats.pendingBLASScratchReleaseCount = m_rayTracingSceneStats.pendingBLASScratchReleaseCount;
    stats.cachedBLASAccelerationStructureBytes = m_rayTracingSceneStats.cachedBLASAccelerationStructureBytes;
    stats.cachedBLASScratchBytes = m_rayTracingSceneStats.cachedBLASScratchBytes;
    stats.releasedBLASScratchBytes = m_rayTracingSceneStats.releasedBLASScratchBytes;
    stats.topLevelAccelerationStructureBytes = m_rayTracingSceneStats.topLevelAccelerationStructureBytes;
    stats.topLevelScratchBytes = m_rayTracingSceneStats.topLevelScratchBytes;
    stats.instanceBufferBytes = m_rayTracingSceneStats.instanceBufferBytes;
    stats.materialMetadataBufferBytes = m_rayTracingSceneStats.materialMetadataBufferBytes;
    stats.alphaMetadataBufferBytes = m_rayTracingSceneStats.alphaMetadataBufferBytes;
    stats.totalTrackedResourceBytes = m_rayTracingSceneStats.totalTrackedResourceBytes;
    stats.resourceByteAccountingOverflowed = m_rayTracingSceneStats.resourceByteAccountingOverflowed;
    stats.resourceBudgetExceeded =
        m_rayTracingBudgetSettings.enabled &&
        m_rayTracingBudgetSettings.maxTrackedResourceBytes > 0 &&
        m_rayTracingSceneStats.resourceBudgetExceeded;
    stats.resourceBudgetEvictionAttempted = m_rayTracingSceneStats.resourceBudgetEvictionAttempted;
    stats.shadowMaterialTextureCount = shadowStats.materialTextureCount;
    stats.shadowMaterialTexturesBound = shadowStats.materialTexturesBound;
    stats.shadowAlphaTextureCount = shadowStats.alphaTextureCount;
    stats.shadowAlphaTexturesBound = shadowStats.alphaTexturesBound;
    stats.shadowAlphaIndexBufferCount = shadowStats.alphaIndexBufferCount;
    stats.shadowAlphaUVBufferCount = shadowStats.alphaUVBufferCount;
    stats.reflectionMaterialTextureCount = reflectionStats.materialTextureCount;
    stats.reflectionMaterialTexturesBound = reflectionStats.materialTexturesBound;
    stats.reflectionGeometryIndexBufferCount = reflectionStats.geometryIndexBufferCount;
    stats.reflectionGeometryUVBufferCount = reflectionStats.geometryUVBufferCount;
    stats.reflectionGeometryNormalBufferCount = reflectionStats.geometryNormalBufferCount;
    stats.reflectionGeometryTangentBufferCount = reflectionStats.geometryTangentBufferCount;
    stats.shadowSamplesPerPixel = shadowStats.requested ? shadowStats.samplesPerPixel : stats.shadowSamplesPerPixel;
    stats.reflectionResolutionScale = reflectionStats.requested
                                          ? reflectionStats.resolutionScale
                                          : stats.reflectionResolutionScale;
    stats.reflectionSamplesPerPixel = reflectionStats.requested
                                          ? reflectionStats.samplesPerPixel
                                          : stats.reflectionSamplesPerPixel;
    stats.reflectionDenoiseRadius = denoiseStats.requested ? denoiseStats.radius : stats.reflectionDenoiseRadius;
    stats.reflectionDenoiseKernelTapCount = denoiseStats.requested
                                                ? denoiseStats.kernelTapCount
                                                : stats.reflectionDenoiseKernelTapCount;
    stats.shadowWidth = shadowStats.width;
    stats.shadowHeight = shadowStats.height;
    stats.reflectionWidth = reflectionStats.width;
    stats.reflectionHeight = reflectionStats.height;
    return stats;
}

void SceneRenderer::ApplyPostProcessSettings(const PostProcessSettings& settings)
{
    m_postProcessSettings = settings;
    if (GetPostProcessStack())
    {
        GetPostProcessStack()->ApplySettings(m_postProcessSettings);
    }
}

void SceneRenderer::ApplyRayTracingBudgetSettings(const SceneRayTracingBudgetSettings& settings)
{
    m_rayTracingBudgetSettings = settings;
    m_rayTracingBudgetSettings.maxTrackedResourceBytes = settings.maxTrackedResourceBytes;
    m_rayTracingBudgetSettings.maxMeasuredGpuMs =
        ClampFiniteRange(settings.maxMeasuredGpuMs, 0.0f, 0.0f, 1000.0f);
    m_rayTracingBudgetSettings.maxShadowMeasuredGpuMs =
        ClampFiniteRange(settings.maxShadowMeasuredGpuMs, 0.0f, 0.0f, 1000.0f);
    m_rayTracingBudgetSettings.maxReflectionMeasuredGpuMs =
        ClampFiniteRange(settings.maxReflectionMeasuredGpuMs, 0.0f, 0.0f, 1000.0f);
    m_rayTracingBudgetSettings.gpuTimingHysteresis =
        ClampFiniteRange(settings.gpuTimingHysteresis, 0.15f, 0.0f, 0.95f);
    m_rayTracingBudgetSettings.gpuTimingRecoveryRate =
        ClampFiniteRange(settings.gpuTimingRecoveryRate, 0.05f, 0.001f, 1.0f);
    m_rayTracingBudgetSettings.gpuTimingAdjustmentFrameCount =
        std::clamp(settings.gpuTimingAdjustmentFrameCount, 1u, 120u);
    m_rayTracingBudgetSettings.minReflectionResolutionScale =
        ClampFiniteRange(settings.minReflectionResolutionScale, 0.25f, 0.25f, 1.0f);
    m_rayTracingBudgetSettings.minShadowSamplesPerPixel =
        std::clamp(settings.minShadowSamplesPerPixel, 1u, 8u);
    m_rayTracingBudgetSettings.minReflectionSamplesPerPixel =
        std::clamp(settings.minReflectionSamplesPerPixel, 1u, 4u);
    m_rayTracingBudgetSettings.minReflectionDenoiseRadius =
        std::min(settings.minReflectionDenoiseRadius, 3u);
    m_rayTracingBudgetSettings.blasCacheEvictionFrameThreshold =
        std::min<uint64>(settings.blasCacheEvictionFrameThreshold, 36000u);
    if (!m_rayTracingBudgetSettings.enabled)
    {
        m_rayTracingGpuBudgetQualityScale = 1.0f;
        m_rayTracingShadowGpuBudgetQualityScale = 1.0f;
        m_rayTracingReflectionGpuBudgetQualityScale = 1.0f;
        m_rayTracingLastMeasuredGpuMs = 0.0f;
        m_rayTracingLastShadowMeasuredGpuMs = 0.0f;
        m_rayTracingLastReflectionMeasuredGpuMs = 0.0f;
        m_rayTracingLastMeasuredGpuMsValid = false;
        m_rayTracingLastShadowMeasuredGpuMsValid = false;
        m_rayTracingLastReflectionMeasuredGpuMsValid = false;
        m_rayTracingGpuBudgetLastShadowEndTimestamp = 0;
        m_rayTracingGpuBudgetLastReflectionEndTimestamp = 0;
        m_rayTracingGpuBudgetOverBudgetFrameCount = 0;
        m_rayTracingGpuBudgetUnderBudgetFrameCount = 0;
        m_rayTracingShadowGpuBudgetOverBudgetFrameCount = 0;
        m_rayTracingShadowGpuBudgetUnderBudgetFrameCount = 0;
        m_rayTracingReflectionGpuBudgetOverBudgetFrameCount = 0;
        m_rayTracingReflectionGpuBudgetUnderBudgetFrameCount = 0;
    }
    else
    {
        if (m_rayTracingBudgetSettings.maxMeasuredGpuMs <= 0.0f)
        {
            m_rayTracingGpuBudgetQualityScale = 1.0f;
            m_rayTracingLastMeasuredGpuMs = 0.0f;
            m_rayTracingLastMeasuredGpuMsValid = false;
            m_rayTracingGpuBudgetOverBudgetFrameCount = 0;
            m_rayTracingGpuBudgetUnderBudgetFrameCount = 0;
        }
        if (m_rayTracingBudgetSettings.maxShadowMeasuredGpuMs <= 0.0f)
        {
            m_rayTracingShadowGpuBudgetQualityScale = 1.0f;
            m_rayTracingLastShadowMeasuredGpuMs = 0.0f;
            m_rayTracingLastShadowMeasuredGpuMsValid = false;
            m_rayTracingShadowGpuBudgetOverBudgetFrameCount = 0;
            m_rayTracingShadowGpuBudgetUnderBudgetFrameCount = 0;
        }
        if (m_rayTracingBudgetSettings.maxReflectionMeasuredGpuMs <= 0.0f)
        {
            m_rayTracingReflectionGpuBudgetQualityScale = 1.0f;
            m_rayTracingLastReflectionMeasuredGpuMs = 0.0f;
            m_rayTracingLastReflectionMeasuredGpuMsValid = false;
            m_rayTracingReflectionGpuBudgetOverBudgetFrameCount = 0;
            m_rayTracingReflectionGpuBudgetUnderBudgetFrameCount = 0;
        }
    }
}

void SceneRenderer::ApplyShadowPassConfig(const ShadowPassConfig& config)
{
    m_shadowPassConfig = config;
    if (FindCompositionPass<ShadowPass>())
    {
        FindCompositionPass<ShadowPass>()->SetConfig(m_shadowPassConfig);
    }
    if (FindCompositionPass<RayTracedShadowPass>())
    {
        FindCompositionPass<RayTracedShadowPass>()->SetConfig(m_shadowPassConfig);
    }
}

void SceneRenderer::FinalizeRenderExecutionReportStatus(bool graphExecuted) noexcept
{
    if (!m_renderPolicyDiagnostics.planAvailable ||
        (!graphExecuted && !m_gpuDrivenFrameFailure))
    {
        return;
    }

    m_renderPolicyDiagnostics.reportAvailable = true;
    bool anyFailed = false;
    bool anyCompleted = false;
    for (const RenderPassExecutionReport& report :
         m_renderPolicyDiagnostics.executionReport.passes)
    {
        anyFailed |= report.status == RenderExecutionStatus::Failed;
        anyCompleted |= report.status == RenderExecutionStatus::Completed;
    }

    // A planned pass can correctly remain NotAttempted (for example, a disabled
    // optional depth pass). Preserve that per-pass fact while reporting a
    // successful frame when the graph ran and at least one planned pass recorded.
    m_renderPolicyDiagnostics.executionReport.status = anyFailed
        ? RenderExecutionStatus::Failed
        : (graphExecuted && anyCompleted ? RenderExecutionStatus::Completed
                                         : RenderExecutionStatus::NotAttempted);
}

bool SceneRenderer::HasSubmissionFailure() const noexcept
{
    if (!m_frameDiagnostics.rendered ||
        !m_frameDiagnostics.graphCompileValid)
    {
        return true;
    }

    // A compiled graph is not sufficient evidence for a required renderer
    // feature. CollectExecutionReceipt() validates the exact graph recording,
    // its semantic consumers, and every required feature receipt. If that
    // validation rejected the current frame, do not submit or present it.
    if (GetRenderFeatureRegistry() != nullptr &&
        (!m_pendingRenderExecutionReceipt.has_value() ||
         !m_pendingRenderExecutionReceipt->executionSucceeded ||
         !m_pendingRenderExecutionReceipt->IsStructurallyValid()))
    {
        return true;
    }

    // Passes may have recorded work before a later lane fails.  Submission is
    // atomic at the accepted-frame boundary: never present a partial hybrid
    // frame just because the graph itself compiled and executed.
    return m_renderPolicyDiagnostics.executionReport.status ==
        RenderExecutionStatus::Failed;
}

void SceneRenderer::Render()
{
    if (!m_initialized || !m_renderContext)
    {
        RefreshFrameDiagnostics(true,
                                false,
                                false,
                                false,
                                "SceneRenderer is not initialized or is missing a render context");
        return;
    }

    const auto renderPrepareStart = std::chrono::steady_clock::now();

    // Begin new frame for transient resource pool and view cache
    if (m_transientResourcePool)
    {
        m_transientResourcePool->BeginFrame();
    }

    if (m_resourceViewCache)
    {
        m_resourceViewCache->BeginFrame();
    }

    if (m_pipelineCache && m_pipelineCache->IsInitialized())
    {
        m_pipelineCache->BeginFrame();
    }

    if (m_materialSystem && m_materialSystem->IsInitialized())
    {
        m_materialSystem->BeginFrame();
    }

    PreparePassesForFrame();
    // The ViewData directional values are a projection of this frame's
    // value-owned primary-light record. Upload only after that projection.
    if (m_pipelineCache && m_pipelineCache->IsInitialized())
    {
        m_pipelineCache->UpdateViewConstants(m_viewData);
    }
    RunPreGraphPrepareCallbacks();
    PrepareRayTracingScene();
    m_currentCpuFrameTiming.renderPrepare =
        static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - renderPrepareStart)
                                 .count());

    const auto policyStart = std::chrono::steady_clock::now();

    const bool rayTracedReflectionsRequested =
        m_postProcessSettings.enableRayTracedReflections && FindCompositionPass<RayTracedReflectionPass>() != nullptr;
    const bool rayTracedReflectionDenoiseRequested =
        rayTracedReflectionsRequested && m_postProcessSettings.enableRayTracedReflectionDenoise;
    const bool rayTracedShadowsRequested =
        FindCompositionPass<RayTracedShadowPass>() && FindCompositionPass<RayTracedShadowPass>()->IsRequestedEnabled();
    // Motion vectors are a resolved feature dependency. TAA and temporally
    // filtered GTAO request the same canonical Camera + Object field even
    // when a caller did not explicitly ask to expose velocity diagnostics.
    const bool cameraVelocityRequested =
        m_postProcessSettings.enableVelocityOutput;
    if (FindCompositionPass<CameraVelocityPass>())
    {
        FindCompositionPass<CameraVelocityPass>()->SetEnabled(cameraVelocityRequested);
    }
    if (FindCompositionPass<ObjectVelocityPass>())
    {
        const bool objectVelocityRequested = FindCompositionPass<CameraVelocityPass>() && FindCompositionPass<CameraVelocityPass>()->IsEnabled();
        FindCompositionPass<ObjectVelocityPass>()->SetEnabled(objectVelocityRequested);
    }

    ShadowPassConfig effectiveShadowConfig = m_shadowPassConfig;
    RayTracedReflectionPassConfig reflectionConfig;
    RayTracedReflectionDenoisePassConfig denoiseConfig;
    if (rayTracedReflectionsRequested)
    {
        reflectionConfig.intensity = std::max(0.0f, m_postProcessSettings.rayTracedReflectionIntensity);
        reflectionConfig.resolutionScale =
            std::clamp(m_postProcessSettings.rayTracedReflectionResolutionScale, 0.25f, 1.0f);
        reflectionConfig.maxRoughness =
            std::clamp(m_postProcessSettings.rayTracedReflectionMaxRoughness, 0.0f, 1.0f);
        reflectionConfig.maxTraceDistance =
            std::max(0.0f, m_postProcessSettings.rayTracedReflectionMaxDistance);
        reflectionConfig.distanceFadeStart =
            std::clamp(m_postProcessSettings.rayTracedReflectionDistanceFadeStart, 0.0f, 1.0f);
        reflectionConfig.instanceMask =
            std::min<uint32>(m_postProcessSettings.rayTracedReflectionInstanceMask, 0xFFu);
        reflectionConfig.samplesPerPixel =
            std::clamp(m_postProcessSettings.rayTracedReflectionSamplesPerPixel, 1u, 4u);
        reflectionConfig.roughnessConeSpread =
            std::clamp(m_postProcessSettings.rayTracedReflectionRoughnessConeSpread, 0.0f, 1.0f);
        reflectionConfig.normalBias =
            std::max(0.0f, m_postProcessSettings.rayTracedReflectionNormalBias);
        reflectionConfig.rayMinT =
            std::max(0.0f, m_postProcessSettings.rayTracedReflectionRayMinT);
        reflectionConfig.fireflyClamp =
            std::max(0.0f, m_postProcessSettings.rayTracedReflectionFireflyClamp);
        reflectionConfig.temporalAccumulation =
            rayTracedReflectionDenoiseRequested;
        reflectionConfig.temporalBlendFactor =
            std::clamp(m_postProcessSettings.rayTracedReflectionTemporalBlendFactor, 0.0f, 1.0f);
        reflectionConfig.historyDepthThreshold =
            std::max(0.0f, m_postProcessSettings.rayTracedReflectionHistoryDepthThreshold);
        reflectionConfig.historyNormalThreshold =
            std::clamp(m_postProcessSettings.rayTracedReflectionHistoryNormalThreshold, 0.0f, 1.0f);
        reflectionConfig.historyLuminanceTolerance =
            std::max(0.0f, m_postProcessSettings.rayTracedReflectionHistoryLuminanceTolerance);
        reflectionConfig.historyConfidenceThreshold =
            std::clamp(m_postProcessSettings.rayTracedReflectionHistoryConfidenceThreshold, 0.0f, 1.0f);
        reflectionConfig.historyVelocityRejectionScale =
            std::max(0.0f, m_postProcessSettings.rayTracedReflectionHistoryVelocityRejectionScale);
        reflectionConfig.fixedHalfResolution =
            m_renderScene.GetSettings().pipelineRequest.recipe ==
            RenderPipelineRecipe::HybridRayTracing;

        denoiseConfig.radius = std::min<uint32>(
            std::max<uint32>(m_postProcessSettings.rayTracedReflectionDenoiseRadius, 0u),
            3u);
        denoiseConfig.depthSigma =
            std::max(m_postProcessSettings.rayTracedReflectionDenoiseDepthSigma, 1.0e-5f);
        denoiseConfig.normalThreshold =
            std::clamp(m_postProcessSettings.rayTracedReflectionDenoiseNormalThreshold, 0.0f, 1.0f);
        denoiseConfig.confidencePower =
            std::max(m_postProcessSettings.rayTracedReflectionDenoiseConfidencePower, 0.01f);
        denoiseConfig.centerWeight =
            std::max(m_postProcessSettings.rayTracedReflectionDenoiseCenterWeight, 0.0f);
        denoiseConfig.lowConfidenceDepthScale =
            std::max(m_postProcessSettings.rayTracedReflectionDenoiseLowConfidenceDepthScale, 1.0f);
    }

    ApplyRayTracingBudget(effectiveShadowConfig,
                          reflectionConfig,
                          denoiseConfig,
                          rayTracedShadowsRequested,
                          rayTracedReflectionsRequested,
                          rayTracedReflectionDenoiseRequested);
    if (FindCompositionPass<RayTracedShadowPass>())
    {
        FindCompositionPass<RayTracedShadowPass>()->SetConfig(effectiveShadowConfig);
    }

    if (FindCompositionPass<RayTracedReflectionPass>() && rayTracedReflectionsRequested)
    {
        FindCompositionPass<RayTracedReflectionPass>()->SetRayTracingScene(m_rayTracingSceneManager.get());
        FindCompositionPass<RayTracedReflectionPass>()->SetConfig(reflectionConfig);
        FindCompositionPass<RayTracedReflectionPass>()->SetEnabled(true);
        if (FindCompositionPass<RayTracedReflectionDenoisePass>())
        {
            FindCompositionPass<RayTracedReflectionDenoisePass>()->SetReflectionSource(FindCompositionPass<RayTracedReflectionPass>());
            FindCompositionPass<RayTracedReflectionDenoisePass>()->SetConfig(denoiseConfig);
            FindCompositionPass<RayTracedReflectionDenoisePass>()->SetEnabled(rayTracedReflectionDenoiseRequested);
        }
        if (FindCompositionPass<RayTracedReflectionCompositePass>())
        {
            FindCompositionPass<RayTracedReflectionCompositePass>()->SetReflectionSource(FindCompositionPass<RayTracedReflectionPass>());
            FindCompositionPass<RayTracedReflectionCompositePass>()->SetDenoisedReflectionSource(FindCompositionPass<RayTracedReflectionDenoisePass>());
            FindCompositionPass<RayTracedReflectionCompositePass>()->SetEnabled(true);
        }
    }
    else
    {
        if (FindCompositionPass<RayTracedReflectionPass>())
        {
            FindCompositionPass<RayTracedReflectionPass>()->SetEnabled(false);
        }
        if (FindCompositionPass<RayTracedReflectionDenoisePass>())
        {
            FindCompositionPass<RayTracedReflectionDenoisePass>()->SetEnabled(false);
        }
        if (FindCompositionPass<RayTracedReflectionCompositePass>())
        {
            FindCompositionPass<RayTracedReflectionCompositePass>()->SetEnabled(false);
        }
    }

    // Freeze the per-view policy after pass configuration is stable and before
    // any RenderGraph construction or command-recording decisions occur.
    CompileRenderFramePlan();
    BuildGPUDrivenVisibilityInputs();
    m_currentCpuFrameTiming.policy =
        static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - policyStart)
                                 .count());
    if (m_gpuDrivenFrameFailure)
    {
        m_renderPolicyDiagnostics.reportAvailable =
            m_renderPolicyDiagnostics.planAvailable;
        RefreshFrameDiagnostics(true,
                                false,
                                true,
                                true,
                                "GPU-driven pre-graph confirmation failed");
        return;
    }

    // A frame owns one definition/plan/execution lineage. Reusing a mutable
    // graph across frames would make stale handles and partial realization
    // observable, so start with a fresh recording object.
    m_renderGraph = std::make_unique<RenderGraph>();

    // Build the render graph (creates depth buffer if needed, imports resources)
    const auto graphBuildStart = std::chrono::steady_clock::now();
    BuildRenderGraph();
    m_currentCpuFrameTiming.graphBuild =
        static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - graphBuildStart)
                                 .count());
    if (!m_featureCompositionFailureReason.empty())
    {
        RVX_RENDER_ERROR(
            "SceneRenderer: render feature composition failed for frame {}: {}",
            m_renderScene.GetAcceptedHeader().sequence,
            m_featureCompositionFailureReason);
        RefreshFrameDiagnostics(true,
                                false,
                                true,
                                false,
                                m_featureCompositionFailureReason.c_str());
        return;
    }
    if (m_gpuDrivenFrameFailure)
    {
        m_renderPolicyDiagnostics.reportAvailable =
            m_renderPolicyDiagnostics.planAvailable;
        RefreshFrameDiagnostics(true,
                                false,
                                true,
                                true,
                                "GPU-driven RenderGraph construction failed");
        return;
    }

    // Finalize one immutable queue policy and allocation-free plan. Physical
    // resources are deliberately unavailable during this stage.
    const bool performanceTimingRequested =
        m_recordedGraphQueuePolicy == RGQueuePolicy::GraphicsOnly;
    RenderGraphCompileOptions compileOptions;
    // Performance timing requires one Graphics-only timestamp span. A visual
    // attachment bundle instead follows the renderer's normal queue policy:
    // the terminal Graphics join still owns its exact copy completion, while
    // optional per-pass timestamps remain unavailable for MultiQueue plans.
    compileOptions.queuePolicy = m_recordedGraphQueuePolicy;
    compileOptions.canonicalizeTransientPoolAccess =
        m_transientResourcePool != nullptr &&
        m_transientResourcePool->IsInitialized();
    if (IRHIDevice* device = m_renderContext->GetDevice())
    {
        compileOptions.capabilities = device->GetCapabilities();
        compileOptions.hasCapabilitySnapshot = true;
    }
    RenderGraphCompiler graphCompiler;
    const auto graphCompileStart = std::chrono::steady_clock::now();
    CompiledRenderGraphPlan compiledPlan =
        graphCompiler.Compile(*m_renderGraph, compileOptions);
    m_currentCpuFrameTiming.graphCompile =
        static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - graphCompileStart)
                                 .count());
    bool graphCompileValid = static_cast<bool>(compiledPlan);

    if (RenderSubmissionTracker* tracker =
            RenderContextInternalAccess::GetSubmissionTracker(*m_renderContext))
    {
        RetireOwnerSnapshots(tracker->CaptureLastSubmittedToken());
    }

    // Prepare contexts, realize physical resources exactly once, record the
    // final plan, and transfer every lease/view into one completion owner.
    const auto graphRealizeRecordStart = std::chrono::steady_clock::now();
    RHICommandContext* ctx = m_renderContext->GetGraphicsContext();
    bool graphExecuted = false;
    bool performanceFramePrepared = false;
    const char* executionSkippedReason = nullptr;
    std::string deferredToneMappingSemanticDiagnostic;
    if (ctx && graphCompileValid)
    {
        RenderGraphExecutionEnvironment executionEnvironment;
        executionEnvironment.device = m_renderContext->GetDevice();
        executionEnvironment.transientResourcePool =
            m_transientResourcePool.get();
        executionEnvironment.graphicsContext = ctx;
        const RenderGraph::CompileStats& timingStats =
            m_renderGraph->GetCompileStats();
        const uint32 activePassCount = timingStats.totalPasses >=
                timingStats.culledPasses
            ? timingStats.totalPasses - timingStats.culledPasses
            : 0;
        if (!performanceTimingRequested &&
            m_qualificationCaptureBridge.GetDiagnostics().state ==
                QualificationCaptureState::Recorded)
        {
            const RenderGraphPassTimingState priorTimingState =
                m_qualificationPassTiming->GetDiagnostics().state;
            const bool canReplaceTimingSample =
                priorTimingState == RenderGraphPassTimingState::Unavailable ||
                priorTimingState == RenderGraphPassTimingState::Idle ||
                priorTimingState == RenderGraphPassTimingState::Completed ||
                priorTimingState == RenderGraphPassTimingState::Rejected;
            if (!canReplaceTimingSample)
            {
                // Never destroy a nonterminal completion-owned sample merely
                // because a later visual bundle was requested.
                RVX_RENDER_WARN(
                    "SceneRenderer: RenderQuality pass timing is still completion-owned; this visual bundle has no performance sample");
            }
            else if (m_renderGraph->GetQueueExecutionMode() ==
                     RenderGraph::QueueExecutionMode::MultiQueue)
            {
                m_qualificationPassTiming->Shutdown(false);
                RVX_RENDER_DEBUG(
                    "SceneRenderer: visual qualification uses the normal MultiQueue plan; optional per-pass timing is unavailable");
            }
            else if (!m_qualificationPassTiming->Initialize(
                         executionEnvironment.device, activePassCount))
            {
                // Visual attachment capture remains independently valid, but
                // performance qualification receives an explicit unavailable
                // native-timestamp status. It must not substitute CPU timing.
                RVX_RENDER_WARN(
                    "SceneRenderer: RenderQuality pass timing is unavailable; performance evidence will fail closed");
            }
            else
            {
                executionEnvironment.passTimestampRecorder =
                    m_qualificationPassTiming.get();
            }
        }
        if (performanceTimingRequested)
        {
            IRenderGraphPassTimestampRecorder* recorder =
                m_qualityPerformanceSession.PrepareFrame(
                    executionEnvironment.device,
                    activePassCount,
                    m_renderScene.GetViewFamily().identity,
                    m_renderScene.GetView().identity);
            if (recorder != nullptr)
            {
                executionEnvironment.passTimestampRecorder = recorder;
                performanceFramePrepared = true;
            }
        }
        RenderGraphExecutor graphExecutor;
        RenderGraphExecution execution =
            graphExecutor.Prepare(compiledPlan, executionEnvironment);
        if (!execution)
        {
            graphCompileValid = false;
            executionSkippedReason =
                "RenderGraph execution preparation failed";
            MarkGPUDrivenFrameFailure(
                GPUDrivenFrameFailureStage::GraphExecution);
        }
        else if (!m_renderContext->AdoptRenderGraphExecution(
                     std::move(execution)))
        {
            graphCompileValid = false;
            executionSkippedReason =
                "RenderContext rejected the completion-owned graph execution";
            MarkGPUDrivenFrameFailure(
                GPUDrivenFrameFailureStage::GraphAdoption);
        }
        graphExecuted = !m_gpuSceneCullingCommandRecordingFailed &&
            graphCompileValid &&
            (!m_gpuSceneRasterCommandRecordingFailed ||
             !m_gpuSceneRasterCommandRecordingFailed->load());
        if (!graphExecuted)
        {
            if (m_gpuSceneCullingCommandRecordingFailed)
            {
                executionSkippedReason =
                    "GPU-scene culling command recording failed";
                MarkGPUDrivenFrameFailure(
                    GPUDrivenFrameFailureStage::CullCommandRecording);
            }
            else if (m_gpuSceneRasterCommandRecordingFailed &&
                     m_gpuSceneRasterCommandRecordingFailed->load())
            {
                executionSkippedReason =
                    "GPU-scene raster command recording failed";
                MarkGPUDrivenFrameFailure(
                    GPUDrivenFrameFailureStage::RasterCommandRecording);
            }
            else
            {
                MarkGPUDrivenFrameFailure(
                    GPUDrivenFrameFailureStage::GraphExecution);
            }
        }
        if (graphExecuted)
        {
            if (m_gpuSceneUploader)
            {
                m_gpuSceneUploader->CommitRealizedAccess(*m_renderGraph);
            }
            CommitGPUDrivenAccessSnapshots();
            if (m_activeRenderPassResults &&
                m_activeRenderPassResults->identity == m_activeRenderPassIdentity)
            {
                m_renderPolicyDiagnostics.executionReport =
                    m_activeRenderPassResults->executionReport;
                m_renderPolicyDiagnostics.executionReport.executedTier =
                    m_confirmedGPUDrivenTier;
                m_renderPolicyDiagnostics.executionReport.tierFallbackReason =
                    (m_viewData.renderFrameExecutionPlan &&
                     m_confirmedGPUDrivenTier !=
                         m_viewData.renderFrameExecutionPlan
                             ->viewPolicy.selectedTier)
                        ? RenderPolicyReason::PlannedFallback
                        : RenderPolicyReason::None;
                PopulateSubmissionMeasurement(m_renderPolicyDiagnostics);
                if (FindCompositionPass<ShadowPass>())
                {
                    FindCompositionPass<ShadowPass>()->PublishRecordResults(
                        m_activeRenderPassResults,
                        m_activeRenderPassIdentity);
                }
                if (FindCompositionPass<DepthPrepass>())
                {
                    FindCompositionPass<DepthPrepass>()->PublishRecordResults(
                        m_activeRenderPassResults,
                        m_activeRenderPassIdentity);
                }
                if (FindCompositionPass<OpaquePass>())
                {
                    FindCompositionPass<OpaquePass>()->PublishRecordResults(
                        m_activeRenderPassResults,
                        m_activeRenderPassIdentity);
                }
                if (FindCompositionPass<TransparentPass>())
                {
                    FindCompositionPass<TransparentPass>()->PublishRecordResults(
                        m_activeRenderPassResults,
                        m_activeRenderPassIdentity);
                }
                if (FindCompositionPass<ObjectVelocityPass>())
                {
                    FindCompositionPass<ObjectVelocityPass>()->PublishRecordResults(
                        m_activeRenderPassResults,
                        m_activeRenderPassIdentity);
                }
                if (FindCompositionPass<CameraVelocityPass>())
                {
                    FindCompositionPass<CameraVelocityPass>()->PublishRecordResults(
                        m_activeRenderPassResults,
                        m_activeRenderPassIdentity);
                }
            }
            RHITextureAccessSnapshot realizedDepthAccess;
            const RHITextureAccessSnapshot* realizedDepthAccessPtr = nullptr;
            if (m_depthTexture && m_depthGraphHandle.IsValid())
            {
                realizedDepthAccess = m_renderGraph->GetRealizedAccess(
                    m_depthGraphHandle);
                realizedDepthAccessPtr = &realizedDepthAccess;
            }
            RHITextureAccessSnapshot realizedBackBufferAccess;
            const RHITextureAccessSnapshot* realizedBackBufferAccessPtr = nullptr;
            if (m_backBufferGraphHandle.IsValid() &&
                m_activeBackBufferIndex < m_backBufferAccessSnapshots.size())
            {
                realizedBackBufferAccess = m_renderGraph->GetRealizedAccess(
                    m_backBufferGraphHandle);
                realizedBackBufferAccessPtr = &realizedBackBufferAccess;
            }
            PublishProvisionalFrameAccessSnapshots(realizedDepthAccessPtr,
                                                   realizedBackBufferAccessPtr);
            if (FindCompositionPass<OpaquePass>())
            {
                const OpaquePassDrawStats& opaqueStats = FindCompositionPass<OpaquePass>()->GetDrawStats();
                m_instancingDiagnostics.requestedMode =
                    opaqueStats.instancingMode;
                m_instancingDiagnostics.opaquePlanAvailable =
                    opaqueStats.instancingPlanAvailable;
                m_instancingDiagnostics.opaquePreflightSucceeded =
                    opaqueStats.instancingPreflightSucceeded;
                m_instancingDiagnostics.opaquePlannedPacketCount =
                    opaqueStats.plannedInstancingPacketCount;
                m_instancingDiagnostics.opaquePlannedDrawCount =
                    opaqueStats.plannedInstancingDrawCount;
                m_instancingDiagnostics.opaquePlannedInstanceCount =
                    opaqueStats.plannedInstancingInstanceCount;
                m_instancingDiagnostics.opaquePlannedBatchCount =
                    opaqueStats.plannedInstancingBatchCount;
                m_instancingDiagnostics.opaqueExecutedPacketCount =
                    opaqueStats.executedPacketCount;
                m_instancingDiagnostics.opaqueSubmittedDrawCount =
                    opaqueStats.submittedDrawCount;
                m_instancingDiagnostics.opaqueSubmittedInstanceCount =
                    opaqueStats.submittedInstanceCount;
                m_instancingDiagnostics.opaqueInstancedBatchCount =
                    opaqueStats.instancedBatchCount;
                m_instancingDiagnostics.opaqueFallbackBatchCount =
                    opaqueStats.instancingFallbackBatchCount;
                m_gpuDrivenCullingStats.opaqueIndirectRequested = opaqueStats.gpuDrivenRequested;
                m_gpuDrivenCullingStats.opaqueCullingReady = opaqueStats.gpuDrivenCullingReady;
                m_gpuDrivenCullingStats.opaquePipelineReady = opaqueStats.gpuDrivenPipelineReady;
                m_gpuDrivenCullingStats.opaqueIndirectEligible = opaqueStats.gpuDrivenEligible;
                m_gpuDrivenCullingStats.opaqueIndirectSubmitted = opaqueStats.gpuDrivenSubmitted;
                m_gpuDrivenCullingStats.opaqueDirectDrawCount = opaqueStats.directDrawCount;
                m_gpuDrivenCullingStats.opaqueGpuDrivenIndirectBatchCount =
                    opaqueStats.gpuDrivenIndirectBatchCount;
                m_gpuDrivenCullingStats
                    .opaqueGpuDrivenSubmittedSourcePacketCount =
                    opaqueStats.gpuDrivenSubmittedSourcePacketCount;
                m_gpuDrivenCullingStats
                    .opaqueGpuDrivenIndirectSubmittedDrawUpperBound =
                    opaqueStats.gpuDrivenIndirectSubmittedDrawUpperBound;
                m_gpuDrivenCullingStats
                    .opaqueGpuDrivenExecutedDrawCountAvailable =
                    opaqueStats.gpuDrivenIndirectExecutedDrawCountAvailable;
                m_gpuDrivenCullingStats.opaqueGpuDrivenIndirectDrawCount =
                    opaqueStats.gpuDrivenIndirectDrawCount;
                m_gpuDrivenCullingStats.opaqueFallbackReason =
                    opaqueStats.gpuDrivenFallbackReason;
            }
            if (m_opaqueGPUCulling && m_opaqueGPUCullingFramePrepared)
            {
                m_gpuDrivenCullingStats.gpuVisibilityCountsAvailable =
                    m_opaqueGPUCulling->WasCpuFallbackUsedLastCull();
                if (m_gpuDrivenCullingStats.gpuVisibilityCountsAvailable)
                {
                    const GPUCulling::Statistics cullingStats =
                        m_opaqueGPUCulling->GetStatistics();
                    m_gpuDrivenCullingStats.visibleCullableDrawItemCount =
                        cullingStats.visibleInstances;
                    m_gpuDrivenCullingStats.frustumCulledDrawItemCount =
                        cullingStats.frustumCulled;
                    m_gpuDrivenCullingStats.distanceCulledDrawItemCount =
                        cullingStats.distanceCulled;
                }
            }
        }
    }
    else
    {
        executionSkippedReason = graphCompileValid
                                     ? "Graphics command context is unavailable"
                                     : "RenderGraph compile reported validation errors";
    }

    if (graphExecuted && graphCompileValid &&
        UsesDeferredOpaquePath(
            m_renderScene.GetSettings().pipelineRequest.recipe) &&
        !ValidateDeferredToneMappingSemanticEvidence(
            *m_renderGraph,
            m_postProcessStats.stackStats,
            m_postProcessStats.backBufferFormat,
            deferredToneMappingSemanticDiagnostic))
    {
        graphExecuted = false;
        executionSkippedReason = deferredToneMappingSemanticDiagnostic.empty()
            ? "DeferredQuality ToneMapping semantic execution did not complete"
            : deferredToneMappingSemanticDiagnostic.c_str();
    }

    m_currentCpuFrameTiming.graphRealizeRecord =
        static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - graphRealizeRecordStart)
                                 .count());

    if (m_activeLocalShadowPass)
    {
        m_activeLocalShadowPass->FinalizeGraphExecution(
            graphExecuted && graphCompileValid);
        const LocalShadowPassExecutionEvidence localShadowEvidence =
            m_activeLocalShadowPass->GetLastExecutionEvidence();
        const bool localShadowWorkRequested =
            m_localShadowAtlasPlan != nullptr &&
            !m_localShadowAtlasPlan->GetPages().empty();
        const bool localShadowEvidenceMatchesPlan =
            localShadowWorkRequested &&
            localShadowEvidence.atlasPlanIdentity ==
                m_localShadowAtlasPlan->GetIdentityHash() &&
            localShadowEvidence.atlasGeneration ==
                m_localShadowAtlasPlan->GetAtlasGeneration();
        m_localLightingStats.localShadowCasterDrawCount =
            localShadowEvidenceMatchesPlan
                ? localShadowEvidence.casterDrawCount
                : 0;
        const bool exactLocalShadowCasterExecution =
            localShadowEvidenceMatchesPlan &&
            localShadowEvidence.HasExactGpuExecution() &&
            localShadowEvidence.realizedPageCount != 0;
        bool hasLocalShadowReceiverExecution = false;
        bool hasPrimaryLocalShadowReceiverExecution = false;
        bool allLocalShadowReceiversExact = true;
        if (localShadowWorkRequested && m_activeRenderPassResults != nullptr &&
            m_activeRenderPassResults->identity == m_activeRenderPassIdentity)
        {
            for (const RenderLightingSurfaceExecutionEvidence& evidence :
                 m_activeRenderPassResults->lightingSurfaceEvidence)
            {
                if (!evidence.IsValid())
                {
                    continue;
                }
                hasLocalShadowReceiverExecution = true;
                hasPrimaryLocalShadowReceiverExecution =
                    hasPrimaryLocalShadowReceiverExecution ||
                    evidence.route ==
                        RenderLightingSurfaceRoute::ForwardOpaque ||
                    evidence.route ==
                        RenderLightingSurfaceRoute::DeferredOpaque;
                allLocalShadowReceiversExact =
                    allLocalShadowReceiversExact &&
                    evidence.localShadowBound &&
                    evidence.lightingPlanIdentity ==
                        m_localShadowAtlasPlan->GetLightingPlanIdentity() &&
                    evidence.localShadowPlanIdentity ==
                        m_localShadowAtlasPlan->GetLocalShadowPlanIdentity() &&
                    evidence.localShadowAtlasPlanIdentity ==
                        m_localShadowAtlasPlan->GetIdentityHash() &&
                    evidence.localShadowAtlasGeneration ==
                        m_localShadowAtlasPlan->GetAtlasGeneration() &&
                    evidence.localShadowPageCount ==
                        m_localShadowAtlasPlan->GetPages().size();
            }
        }
        const bool exactLocalShadowReceiverExecution =
            hasLocalShadowReceiverExecution &&
            hasPrimaryLocalShadowReceiverExecution &&
            allLocalShadowReceiversExact;
        const bool exactLocalShadowExecution =
            exactLocalShadowCasterExecution &&
            exactLocalShadowReceiverExecution;
        m_localLightingStats.localShadowAtlasReady =
            exactLocalShadowExecution;
        if (m_localLightingStats.localShadowRequestCount != 0)
        {
            m_localLightingStats.pointShadowSupported =
                m_localLightingStats.pointShadowRequestCount == 0 ||
                exactLocalShadowExecution;
            m_localLightingStats.spotShadowSupported =
                m_localLightingStats.spotShadowRequestCount == 0 ||
                exactLocalShadowExecution;
            if (!exactLocalShadowExecution)
            {
                const std::string executionFailure =
                    exactLocalShadowCasterExecution &&
                            !exactLocalShadowReceiverExecution
                        ? "local-shadow atlas was rendered but no exact lighting receiver consumed it"
                        : (localShadowEvidence.diagnostic.empty()
                               ? "local-shadow graph execution was not accepted"
                               : localShadowEvidence.diagnostic);
                m_localLightingStats.localShadowFallbackReason =
                    executionFailure;
                if (m_localLightingStats.pointShadowRequestCount != 0)
                {
                    m_localLightingStats.pointShadowUnsupportedReason =
                        executionFailure;
                }
                if (m_localLightingStats.spotShadowRequestCount != 0)
                {
                    m_localLightingStats.spotShadowUnsupportedReason =
                        executionFailure;
                }
            }
            else
            {
                m_localLightingStats.localShadowFallbackReason.clear();
                m_localLightingStats.pointShadowUnsupportedReason.clear();
                m_localLightingStats.spotShadowUnsupportedReason.clear();
            }
        }
    }

    if (GetRenderFeatureRegistry() && graphExecuted && graphCompileValid)
    {
        const RenderFeatureRegistryCode evidenceCode =
            GetRenderFeatureRegistry()->FinalizeGraphExecution(*m_renderGraph);
        if (evidenceCode != RenderFeatureRegistryCode::Success &&
            executionSkippedReason == nullptr)
        {
            executionSkippedReason =
                "Render feature graph execution evidence could not be finalized";
        }
    }

    // The adapter only exposes this snapshot after it received exact graph
    // completion evidence. Keep it pending until the matching render receipt
    // passes the separate presentation transaction below.
    m_pendingReflectionDiagnostics.reset();
    if (FindCompositionFeature<ReflectionFeatureAdapter>() != nullptr &&
        m_reflectionFramePlan != nullptr &&
        FindCompositionFeature<ReflectionFeatureAdapter>()->HasFinalizedSSRComputeStats())
    {
        const SSRComputeStats& stats =
            FindCompositionFeature<ReflectionFeatureAdapter>()->GetFinalizedSSRComputeStats();
        const ReflectionSSRIdentity& ssrIdentity =
            m_reflectionFramePlan->GetSSRIdentity();
        const bool screenSpaceIntentMatches =
            stats.screenSpaceRequested == ssrIdentity.requested;
        const bool configurationMatches = !stats.screenSpaceRequested ||
            (stats.configurationIdentity != 0 &&
             stats.configurationIdentity ==
                 ssrIdentity.configurationIdentityHash);
        const bool traceReadinessMatches = !stats.screenSpaceRequested ||
            stats.screenSpaceTraceInputsReady == ssrIdentity.inputsReady;
        const bool pipelineMatches = !stats.screenSpaceRequested ||
            (stats.screenSpaceTraceInputsReady
                ? stats.pipelineIdentity != 0 &&
                    stats.pipelineIdentity == ssrIdentity.pipelineIdentityHash
                : stats.pipelineIdentity == 0 &&
                    ssrIdentity.pipelineIdentityHash == 0);
        const bool depthPyramidMatches =
            stats.screenSpaceTraceScheduled
                ? stats.depthPyramidIdentity != 0 &&
                    stats.depthPyramidIdentity ==
                        ssrIdentity.depthPyramidIdentityHash
                : stats.depthPyramidIdentity == 0;
        const bool identitiesMatch = m_lightingFramePlan != nullptr &&
            stats.reflectionPlanIdentity ==
                m_reflectionFramePlan->GetIdentityHash() &&
            stats.lightingPlanIdentity ==
                m_lightingFramePlan->GetIdentityHash() &&
            stats.reflectionSurfaceIdentity != 0 &&
            stats.exposureStateContentHash != 0 &&
            screenSpaceIntentMatches && configurationMatches &&
            traceReadinessMatches && pipelineMatches &&
            depthPyramidMatches;
        if (identitiesMatch)
        {
            RenderReflectionDiagnostics reflection;
            reflection.graphExecutionFinalized = true;
            reflection.requested = stats.requested;
            reflection.supported = stats.supported;
            reflection.scheduled = stats.scheduled;
            reflection.executed = stats.executed;
            reflection.hiZBuilt = stats.hiZBuilt;
            reflection.globalIBLReady = stats.globalIBLReady;
            reflection.fallbackChainResolved = stats.fallbackChainResolved;
            reflection.screenSpaceRequested = stats.screenSpaceRequested;
            reflection.screenSpaceTraceInputsReady =
                stats.screenSpaceTraceInputsReady;
            reflection.screenSpaceTraceScheduled =
                stats.screenSpaceTraceScheduled;
            reflection.halfResolutionTrace = stats.halfResolutionTrace;
            reflection.guidedUpscaleScheduled = stats.guidedUpscaleScheduled;
            reflection.guidedUpscaleExecuted = stats.guidedUpscaleExecuted;
            // Route-mask pixels become trustworthy only when the separately
            // armed post-present readback maps. Graph finalization is not
            // permitted to claim either route was visually applied.
            reflection.screenSpaceApplied = false;
            reflection.probesApplied = false;
            reflection.reflectionRouteEvidenceReady = false;
            reflection.routeMaskRadianceSchemaVersion = 0;
            reflection.localProbeRadianceQualified = false;
            reflection.globalIBLRadianceQualified = false;
            reflection.readyProbeCount = stats.readyProbeCount;
            reflection.hiZMipCount = stats.hiZMipCount;
            reflection.hiZGraphPassCount = stats.hiZGraphPassCount;
            reflection.traceGraphPassCount = stats.traceGraphPassCount;
            reflection.resolveGraphPassCount = stats.resolveGraphPassCount;
            reflection.traceExtent = stats.traceExtent;
            reflection.resolvedExtent = stats.resolvedExtent;
            reflection.frameSequence =
                m_reflectionFramePlan->GetFrameIdentity().frameSequence;
            reflection.reflectionPlanIdentity = stats.reflectionPlanIdentity;
            reflection.lightingPlanIdentity = stats.lightingPlanIdentity;
            reflection.configurationIdentity = stats.screenSpaceRequested
                ? stats.configurationIdentity : 0;
            reflection.pipelineIdentity = stats.screenSpaceRequested
                ? stats.pipelineIdentity : 0;
            reflection.depthPyramidIdentity = stats.depthPyramidIdentity;
            reflection.reflectionSurfaceIdentity =
                stats.reflectionSurfaceIdentity;
            reflection.exposureStateContentHash =
                stats.exposureStateContentHash;
            reflection.screenSpaceHitPixelCount = 0;
            reflection.localProbePixelCount = 0;
            reflection.globalIBLPixelCount = 0;
            reflection.reflectionReadbackSampleCount = 0;
            reflection.reflectionRouteSampleCount = 0;
            reflection.backgroundPixelCount = 0;
            reflection.nonReflectivePixelCount = 0;
            reflection.fallbackReason = stats.fallbackReason;
            m_pendingReflectionDiagnostics = std::move(reflection);
        }
    }

    FinalizeRenderExecutionReportStatus(graphExecuted);

    if (GetRenderFeatureRegistry())
    {
        RenderExecutionReceipt receipt =
            GetRenderFeatureRegistry()->CollectExecutionReceipt(
                m_executionBuildIdentity);
        receipt.schemaVersion = RVX_RENDER_EXECUTION_RECEIPT_SCHEMA_VERSION;
        if (m_activeRenderPassResults != nullptr &&
            m_activeRenderPassResults->identity == m_activeRenderPassIdentity &&
            m_activeRenderPassIdentity.frameSequence ==
                receipt.frameIdentity.frameSequence)
        {
            const OpaqueSurfaceRouteExecutionEvidence& deferredRoute =
                m_activeRenderPassResults->deferredOpaqueRoute;
            receipt.deferredOpaque.available = deferredRoute.available;
            receipt.deferredOpaque.status =
                ToDeferredOpaqueExecutionStatus(deferredRoute.status);
            receipt.deferredOpaque.plannedPacketCount =
                deferredRoute.drawStats.plannedPacketCount;
            receipt.deferredOpaque.executedPacketCount =
                deferredRoute.drawStats.executedPacketCount;
        }
        const bool lightingPlanMatchesReceipt = m_lightingFramePlan != nullptr &&
            m_lightingFramePlan->IsValid() &&
            m_lightingFramePlan->GetFrameIdentity() == receipt.frameIdentity &&
            m_lightingFramePlan->GetViewIdentity() == receipt.viewIdentity &&
            m_lightManager != nullptr &&
            m_lightManager->IsLightingPlanBound() &&
            m_lightManager->GetLightingPlanIdentity() ==
                m_lightingFramePlan->GetIdentityHash();
        const bool exactGraphExecutionSucceeded =
            receipt.executionSucceeded && graphExecuted && graphCompileValid;
        const bool activeRenderPassResultsMatchReceipt =
            m_activeRenderPassResults != nullptr &&
            m_activeRenderPassResults->identity == m_activeRenderPassIdentity &&
            m_activeRenderPassIdentity.frameSequence ==
                receipt.frameIdentity.frameSequence;
        if (activeRenderPassResultsMatchReceipt)
        {
            const OpaquePassDrawStats& toonStats =
                UsesDeferredOpaquePath(receipt.resolvedRecipe)
                    ? m_activeRenderPassResults->deferredOpaqueRoute.drawStats
                    : m_activeRenderPassResults->opaqueStats;
            const bool observedToonWork =
                toonStats.toonMaterialCount != 0 ||
                toonStats.toonDrawCount != 0 ||
                toonStats.toonFallbackDrawCount != 0;
            if (observedToonWork)
            {
                RenderToonLightingExecutionReceipt toon;
                toon.requested = true;
                toon.pipelineReady =
                    toonStats.planValidated &&
                    toonStats.toonProgramFamilyHash != 0;
                toon.inputsReady =
                    toonStats.toonMaterialCount != 0 &&
                    toonStats.materialTableSchemaVersion != 0 &&
                    toonStats.materialTableContentHash != 0;
                toon.executed =
                    exactGraphExecutionSucceeded &&
                    toonStats.toonDrawCount != 0;
                toon.programFamilyHash = toonStats.toonProgramFamilyHash;
                toon.materialCount = toonStats.toonMaterialCount;
                toon.drawCount = toonStats.toonDrawCount;
                toon.forwardShadedDrawCount =
                    receipt.resolvedRecipe ==
                            RenderPipelineRecipe::ForwardStandard
                        ? toonStats.toonDrawCount
                        : 0;
                toon.deferredShadedDrawCount =
                    UsesDeferredOpaquePath(receipt.resolvedRecipe)
                        ? toonStats.toonDrawCount
                        : 0;
                // The shared Toon program has no StandardLit compatibility
                // branch. A resource fallback remains explicit and invalid.
                toon.standardLitFallbackCount = 0;
                toon.materialFallbackCount =
                    toonStats.toonFallbackDrawCount;
                toon.bandsApplied = toon.executed;
                toon.specularThresholdApplied = toon.executed;
                toon.rimApplied = toon.executed;
                toon.shadowTintApplied = toon.executed;
                toon.materialTableSchemaVersion =
                    toonStats.materialTableSchemaVersion;
                toon.materialTableContentHash =
                    toonStats.materialTableContentHash;
                receipt.toonLighting = std::move(toon);
            }
        }
        const RenderDeformationFrameIdentity activeDeformationIdentity =
            MakeRenderDeformationFrameIdentity(m_activeRenderPassIdentity);
        const bool deformationResourcesMatchReceipt =
            activeRenderPassResultsMatchReceipt && m_renderGraph != nullptr &&
            m_activeRenderDeformationResources != nullptr &&
            m_activeRenderDeformationResources->MatchesIdentity(
                activeDeformationIdentity) &&
            m_activeRenderDeformationResources->MatchesGraphProvenance(
                *m_renderGraph);
        bool deformationExecutionEvidenceValid = false;
        if (deformationResourcesMatchReceipt)
        {
            const RenderDeformationFrameResources& resources =
                *m_activeRenderDeformationResources;
            RenderDeformationExecutionReceipt deformation;
            deformation.deformationContentHash = resources.GetContentHash();
            deformation.skinnedObjectCount = static_cast<uint32>(
                resources.GetSlices().size());
            deformation.paletteMatrixCount = resources.GetMatrixCount();
            deformation.previousPaletteObjectCount = static_cast<uint32>(
                std::count_if(
                    resources.GetSlices().begin(),
                    resources.GetSlices().end(),
                    [](const RenderDeformationPaletteSlice& slice)
                    {
                        return slice.previousValid;
                    }));
            deformation.currentPaletteBufferReady =
                resources.GetMatrixCount() != 0 &&
                resources.GetCurrentPaletteBuffer() != nullptr &&
                resources.GetCurrentPaletteHandle().IsValid();
            deformation.previousPaletteBufferReady =
                resources.GetMatrixCount() != 0 &&
                resources.GetPreviousPaletteBuffer() != nullptr &&
                resources.GetPreviousPaletteHandle().IsValid();

            const OpaquePassDrawStats& opaqueDeformationStats =
                UsesDeferredOpaquePath(receipt.resolvedRecipe)
                    ? m_activeRenderPassResults->deferredOpaqueRoute.drawStats
                    : m_activeRenderPassResults->opaqueStats;
            const ShadowPassStats& shadowDeformationStats =
                m_activeRenderPassResults->shadowStats;
            const ObjectVelocityPassStats& velocityDeformationStats =
                m_activeRenderPassResults->objectVelocityStats;
            deformation.opaqueSkinnedDrawCount =
                opaqueDeformationStats.skinnedDrawCount;
            deformation.shadowSkinnedDrawCount =
                shadowDeformationStats.skinnedDrawCount;
            deformation.velocitySkinnedDrawCount =
                velocityDeformationStats.skinnedDrawCount;
            deformation.velocityPreviousPaletteDrawCount =
                velocityDeformationStats.previousPaletteBoundDrawCount;

            const auto validatesCurrentPaletteDraws =
                [&resources](uint32 skinnedDrawCount,
                             uint32 currentPaletteBoundDrawCount,
                             uint64 contentHash) noexcept
            {
                return skinnedDrawCount == 0 ||
                    (contentHash == resources.GetContentHash() &&
                     currentPaletteBoundDrawCount == skinnedDrawCount);
            };
            deformationExecutionEvidenceValid =
                deformation.IsStructurallyValid() &&
                validatesCurrentPaletteDraws(
                    opaqueDeformationStats.skinnedDrawCount,
                    opaqueDeformationStats.currentPaletteBoundDrawCount,
                    opaqueDeformationStats.deformationContentHash) &&
                validatesCurrentPaletteDraws(
                    shadowDeformationStats.skinnedDrawCount,
                    shadowDeformationStats.currentPaletteBoundDrawCount,
                    shadowDeformationStats.deformationContentHash) &&
                validatesCurrentPaletteDraws(
                    velocityDeformationStats.skinnedDrawCount,
                    velocityDeformationStats.currentPaletteBoundDrawCount,
                    velocityDeformationStats.deformationContentHash) &&
                (velocityDeformationStats.skinnedDrawCount == 0 ||
                 velocityDeformationStats.previousPaletteBoundDrawCount ==
                     velocityDeformationStats.skinnedDrawCount);
            receipt.deformation = deformation;
        }
        if (lightingPlanMatchesReceipt)
        {
            const LightingFramePlanCapacityEvidence& capacity =
                m_lightingFramePlan->GetCapacityEvidence();
            RenderLightingExecutionReceipt lighting;
            const auto hasLightingConsumer =
                [this, activeRenderPassResultsMatchReceipt](
                    uint64 lightingPlanIdentity,
                    bool requireDirectionalShadow,
                    bool requireTextureIBL) noexcept
            {
                if (!activeRenderPassResultsMatchReceipt)
                {
                    return false;
                }
                return std::any_of(
                    m_activeRenderPassResults->lightingSurfaceEvidence.begin(),
                    m_activeRenderPassResults->lightingSurfaceEvidence.end(),
                    [lightingPlanIdentity,
                     requireDirectionalShadow,
                     requireTextureIBL](
                        const RenderLightingSurfaceExecutionEvidence& evidence)
                    {
                        return evidence.IsValid() &&
                            evidence.lightingPlanIdentity ==
                                lightingPlanIdentity &&
                            (!requireDirectionalShadow ||
                             evidence.directionalShadowBound) &&
                            (!requireTextureIBL ||
                             (evidence.textureIBLBound &&
                              evidence.exactIBLViews));
                    });
            };
            lighting.lightingPlanIdentity =
                m_lightingFramePlan->GetIdentityHash();
            lighting.sceneLightStateHash =
                m_lightingFramePlan->GetSceneLightStateHash();
            if (const LightingLightCandidate* primary =
                    m_lightingFramePlan->GetPrimaryDirectionalLight();
                primary != nullptr)
            {
                lighting.primaryDirectionalLightId = primary->lightId;
            }
            lighting.pointLightRequestedCount =
                capacity.requestedPointLightCount;
            lighting.pointLightAdmittedCount =
                capacity.selectedPointLightCount;
            lighting.pointLightCapacity = capacity.pointLightCapacity;
            lighting.pointLightOverflowCount =
                capacity.overflowPointLightCount;
            lighting.spotLightRequestedCount =
                capacity.requestedSpotLightCount;
            lighting.spotLightAdmittedCount =
                capacity.selectedSpotLightCount;
            lighting.spotLightCapacity = capacity.spotLightCapacity;
            lighting.spotLightOverflowCount =
                capacity.overflowSpotLightCount;
            lighting.csmRequested =
                m_lightingFramePlan->GetCSMRequest().requested;
            lighting.csmInputsReady =
                m_lightingFramePlan->GetCSMRequest().inputsReady;
            const uint32 plannedCascadeCount =
                m_lightingFramePlan->GetCSMRequest().cascadeCount;
            lighting.csmExecutionReady = exactGraphExecutionSucceeded &&
                lighting.csmInputsReady &&
                activeRenderPassResultsMatchReceipt &&
                m_activeRenderPassResults->directionalShadowOutput.enabled &&
                m_activeRenderPassResults->directionalShadowOutput.shadowMap.IsValid() &&
                m_activeRenderPassResults->shadowStats.configuredCascadeCount ==
                    plannedCascadeCount &&
                m_activeRenderPassResults->shadowStats.declaredCascadeResourceCount ==
                    plannedCascadeCount &&
                m_activeRenderPassResults->shadowStats.resolvedCascadeViewCount ==
                    plannedCascadeCount &&
                m_activeRenderPassResults->directionalShadowOutput.IsCompatibleWith(
                    m_activeRenderPassIdentity,
                    lighting.lightingPlanIdentity) &&
                hasLightingConsumer(
                    lighting.lightingPlanIdentity, true, false);
            lighting.iblRequested =
                m_lightingFramePlan->GetIBLRequest().requested;
            lighting.iblInputsReady =
                m_lightingFramePlan->GetIBLRequest().inputsReady;
            lighting.iblExecutionReady = exactGraphExecutionSucceeded &&
                lighting.iblInputsReady &&
                hasLightingConsumer(
                    lighting.lightingPlanIdentity, false, true);
            receipt.lighting = lighting;
        }
        const bool ambientOcclusionFeatureExecuted = std::any_of(
            receipt.features.begin(),
            receipt.features.end(),
            [](const RenderFeatureExecutionReceipt& feature)
            {
                return feature.featureId ==
                           BuiltinRenderFeatureIds::AmbientOcclusion &&
                       feature.readiness.executed &&
                       feature.graphPassCount != 0 &&
                       feature.executedGraphPassCount ==
                           feature.graphPassCount;
            });
        if (FindCompositionFeature<AmbientOcclusionFeatureAdapter>() != nullptr)
        {
            receipt.ambientOcclusion =
                FindCompositionFeature<AmbientOcclusionFeatureAdapter>()->GetLastExecutionReceipt();
        }
        if (receipt.ambientOcclusion.requested &&
            activeRenderPassResultsMatchReceipt)
        {
            const RenderLightingSurfaceRoute route =
                UsesDeferredOpaquePath(receipt.resolvedRecipe)
                    ? RenderLightingSurfaceRoute::DeferredOpaque
                    : RenderLightingSurfaceRoute::ForwardOpaque;
            const RenderLightingSurfaceExecutionEvidence& evidence =
                m_activeRenderPassResults->lightingSurfaceEvidence[
                    static_cast<size_t>(route)];
            if (evidence.IsValid() && evidence.route == route)
            {
                receipt.ambientOcclusion.lightingConsumerExecuted = true;
                receipt.ambientOcclusion.lightingAOFieldContentHash =
                    evidence.ambientOcclusionFieldContentHash;
            }
        }
        const bool ambientOcclusionExecutionEvidenceValid =
            receipt.ambientOcclusion.IsEmpty() ||
            (exactGraphExecutionSucceeded &&
             ambientOcclusionFeatureExecuted &&
             receipt.ambientOcclusion.IsStructurallyValid());
        const bool postProcessFeatureExecuted = std::any_of(
            receipt.features.begin(),
            receipt.features.end(),
            [](const RenderFeatureExecutionReceipt& feature)
            {
                return feature.featureId ==
                           BuiltinRenderFeatureIds::PostProcessPipeline &&
                       feature.readiness.executed &&
                       feature.graphPassCount != 0 &&
                       feature.executedGraphPassCount ==
                           feature.graphPassCount;
            });
        if (FindCompositionFeature<PostProcessPipelineFeatureAdapter>() != nullptr)
        {
            receipt.postProcess =
                FindCompositionFeature<PostProcessPipelineFeatureAdapter>()->GetLastExecutionReceipt();
            receipt.toonOutline =
                FindCompositionFeature<PostProcessPipelineFeatureAdapter>()
                    ->GetLastToonOutlineExecutionReceipt();
        }
        bool hybridRayTracingExecutionEvidenceValid = true;
        if (m_hybridRayTracingFramePlan.has_value())
        {
            const HybridRayTracingFramePlan& plan =
                *m_hybridRayTracingFramePlan;
            const bool hybridResultsMatchReceipt =
                activeRenderPassResultsMatchReceipt &&
                plan.GetFrameIdentity() == receipt.frameIdentity &&
                plan.GetViewIdentity() == receipt.viewIdentity;
            RayTracedShadowPassStats shadowStats{};
            RayTracedReflectionPassStats reflectionStats{};
            RayTracedReflectionDenoisePassStats denoiseStats{};
            RayTracedReflectionCompositePassStats compositeStats{};
            if (hybridResultsMatchReceipt)
            {
                shadowStats =
                    m_activeRenderPassResults->rayTracedShadowStats;
                reflectionStats =
                    m_activeRenderPassResults->rayTracedReflectionStats;
                denoiseStats =
                    m_activeRenderPassResults
                        ->rayTracedReflectionDenoiseStats;
                compositeStats =
                    m_activeRenderPassResults
                        ->rayTracedReflectionCompositeStats;
            }
            RenderHybridRayTracingExecutionReceipt& hybrid =
                receipt.hybridRayTracing;
            hybrid.available = true;
            hybrid.planIdentityHash = plan.GetIdentityHash();
            hybrid.scenePreparationGeneration =
                plan.GetRayTracingScenePreparationGeneration();
            hybrid.sceneStructuralHash =
                plan.GetRayTracingSceneStructuralHash();
            hybrid.frameIdentity = plan.GetFrameIdentity();
            hybrid.viewIdentity = plan.GetViewIdentity();
            hybrid.fullResolution = plan.GetReflectionExtent().fullResolution;
            hybrid.halfResolution = plan.GetReflectionExtent().halfResolution;
            hybrid.historyResetReasons = plan.GetReflectionHistoryReset();
            hybrid.recipe = plan.GetRecipe();
            hybrid.quality = plan.GetQuality();
            hybrid.profileHash = plan.GetProfileHash();
            hybrid.shadowRoute =
                ToPublicHybridShadowRoute(plan.GetShadowRoute());
            hybrid.reflectionRoute =
                ToPublicHybridReflectionRoute(plan.GetReflectionRoute());
            hybrid.graphExecutionSucceeded =
                exactGraphExecutionSucceeded && hybridResultsMatchReceipt;
            hybrid.shadowDispatchRecorded = shadowStats.dispatchRecorded;
            hybrid.reflectionDispatchRecorded =
                reflectionStats.dispatchRecorded;
            hybrid.spatialDenoiseRecorded = denoiseStats.denoiseRecorded;
            hybrid.temporalResolveRecorded =
                reflectionStats.temporalResolveRecorded;
            hybrid.compositeRecorded = compositeStats.compositeRecorded;
            hybrid.compositeUsedDenoisedSource =
                compositeStats.source ==
                    RayTracedReflectionCompositeSource::DenoisedReflection &&
                compositeStats.denoisedSourceUsed;
            hybridRayTracingExecutionEvidenceValid =
                hybrid.IsStructurallyValid() &&
                hybrid.graphExecutionSucceeded &&
                hybrid.shadowDispatchRecorded &&
                hybrid.reflectionDispatchRecorded &&
                hybrid.spatialDenoiseRecorded &&
                hybrid.temporalResolveRecorded &&
                hybrid.compositeRecorded &&
                hybrid.compositeUsedDenoisedSource;
        }
        else if (receipt.resolvedRecipe ==
                 RenderPipelineRecipe::HybridRayTracing)
        {
            hybridRayTracingExecutionEvidenceValid = false;
        }
        const bool postProcessExecutionEvidenceValid =
            exactGraphExecutionSucceeded && postProcessFeatureExecuted &&
            receipt.postProcess.IsStructurallyValid();
        const bool toonLightingExecutionEvidenceValid =
            receipt.toonLighting.IsEmpty() ||
            (exactGraphExecutionSucceeded &&
             receipt.toonLighting.IsStructurallyValid());
        const bool toonOutlineExecutionEvidenceValid =
            receipt.toonOutline.IsEmpty() ||
            (exactGraphExecutionSucceeded && postProcessFeatureExecuted &&
             receipt.toonOutline.IsStructurallyValid() &&
             (!receipt.toonOutline.IsExecutionReady() ||
              (receipt.toonLighting.IsExecutionReady() &&
               receipt.toonOutline.materialTableContentHash ==
                   receipt.toonLighting.materialTableContentHash)));
        receipt.executionSucceeded = receipt.executionSucceeded &&
            graphExecuted && graphCompileValid &&
            deformationExecutionEvidenceValid &&
            ambientOcclusionExecutionEvidenceValid &&
            postProcessExecutionEvidenceValid &&
            toonLightingExecutionEvidenceValid &&
            toonOutlineExecutionEvidenceValid &&
            hybridRayTracingExecutionEvidenceValid;
        receipt.presented = false;
        receipt.succeeded = false;
        if (!receipt.executionSucceeded && receipt.diagnostic.empty())
        {
            receipt.diagnostic = !deformationExecutionEvidenceValid
                ? "Render deformation execution evidence did not match the active graph recording"
                : !ambientOcclusionExecutionEvidenceValid
                    ? "Ambient-occlusion execution evidence did not match its opaque lighting consumer"
                : !postProcessExecutionEvidenceValid
                    ? "Post-process execution evidence did not match the active graph recording"
                : !toonLightingExecutionEvidenceValid
                    ? "Toon lighting execution evidence did not match the active opaque recording"
                : !toonOutlineExecutionEvidenceValid
                    ? "Toon outline execution evidence did not match its material table or post-process recording"
                : !hybridRayTracingExecutionEvidenceValid
                    ? "Hybrid ray-tracing execution evidence did not match its frozen plan"
                : executionSkippedReason != nullptr
                    ? executionSkippedReason
                    : "Render feature execution did not complete";
        }
        m_lastAttemptedRenderExecutionReceipt = receipt;
        if (m_qualificationCaptureBridge.GetDiagnostics().state ==
            QualificationCaptureState::Recorded)
        {
            // Receipt identity does not exist while RenderGraph records. Bind
            // it only after executor/feature evidence has accepted this exact
            // graph; a failed optional capture never rewrites normal frame
            // execution truth.
            if (!m_qualificationCaptureBridge.FinalizeExecutionReceipt(receipt))
            {
                RVX_RENDER_WARN(
                    "SceneRenderer: RenderQuality qualification capture rejected final execution receipt");
            }
        }
        if (performanceFramePrepared &&
            !m_qualityPerformanceSession.BindExecutionReceipt(receipt))
        {
            RVX_RENDER_WARN(
                "SceneRenderer: GFX24 performance session rejected the exact execution receipt");
        }
        if (receipt.executionSucceeded && receipt.IsStructurallyValid())
        {
            if (m_pendingReflectionDiagnostics.has_value() &&
                m_pendingReflectionDiagnostics->frameSequence !=
                    receipt.frameIdentity.frameSequence)
            {
                // A finalization result from another accepted frame is never
                // allowed to cross the receipt/presentation boundary.
                m_pendingReflectionDiagnostics.reset();
            }
            m_pendingRenderExecutionReceipt = receipt;
        }
        else
        {
            m_qualificationCaptureBridge.RejectPending();
            m_pendingRenderExecutionReceipt.reset();
            m_pendingReflectionDiagnostics.reset();
        }
    }

    RefreshFrameDiagnostics(true,
                            graphExecuted && graphCompileValid,
                            true,
                            true,
                            executionSkippedReason);

    if (m_gpuDrivenFrameFailure)
    {
        return;
    }

    // Log compile stats periodically for debugging
    static uint64_t frameCount = 0;
    if (++frameCount == 1)
    {
        const auto& stats = m_renderGraph->GetCompileStats();
        RVX_CORE_INFO("RenderGraph stats: {} passes ({} culled), {} barriers, memory savings: {:.1f}%",
                      stats.totalPasses, stats.culledPasses, stats.barrierCount,
                      stats.GetMemorySavingsPercent());
    }

    // End frame for transient resource pool
    if (m_transientResourcePool)
    {
        m_transientResourcePool->EndFrame();

        // Evict unused resources every 60 frames (approx 1 second at 60fps)
        if (frameCount % 60 == 0)
        {
            m_transientResourcePool->EvictUnused(3);  // Evict resources unused for 3 frames
        }
    }
}

void SceneRenderer::ApplyRayTracingBudget(ShadowPassConfig& shadowConfig,
                                          RayTracedReflectionPassConfig& reflectionConfig,
                                          RayTracedReflectionDenoisePassConfig& denoiseConfig,
                                          bool shadowRequested,
                                          bool reflectionRequested,
                                          bool denoiseRequested)
{
    if (FindCompositionPass<RayTracedShadowPass>())
    {
        FindCompositionPass<RayTracedShadowPass>()->RefreshCompletionDiagnostics();
    }

    SceneRayTracingFrameStats budgetStats;
    budgetStats.budgetEnabled = m_rayTracingBudgetSettings.enabled;
    budgetStats.rayBudget = m_rayTracingBudgetSettings.maxRayCount;
    budgetStats.denoiseTapBudget = m_rayTracingBudgetSettings.maxDenoiseTapCount;
    budgetStats.trackedResourceBudget = m_rayTracingBudgetSettings.maxTrackedResourceBytes;
    budgetStats.shadowRequested = shadowRequested;
    budgetStats.reflectionRequested = reflectionRequested;
    budgetStats.reflectionDenoiseRequested = denoiseRequested;
    budgetStats.cachedBLASCount = m_rayTracingSceneStats.cachedBLASCount;
    budgetStats.evictedBLASCount = m_rayTracingSceneStats.evictedBLASCount;
    budgetStats.resourceBudgetEvictedBLASCount = m_rayTracingSceneStats.resourceBudgetEvictedBLASCount;
    budgetStats.releasedBLASScratchCount = m_rayTracingSceneStats.releasedBLASScratchCount;
    budgetStats.pendingBLASScratchReleaseCount = m_rayTracingSceneStats.pendingBLASScratchReleaseCount;
    budgetStats.cachedBLASAccelerationStructureBytes =
        m_rayTracingSceneStats.cachedBLASAccelerationStructureBytes;
    budgetStats.cachedBLASScratchBytes = m_rayTracingSceneStats.cachedBLASScratchBytes;
    budgetStats.releasedBLASScratchBytes = m_rayTracingSceneStats.releasedBLASScratchBytes;
    budgetStats.topLevelAccelerationStructureBytes =
        m_rayTracingSceneStats.topLevelAccelerationStructureBytes;
    budgetStats.topLevelScratchBytes = m_rayTracingSceneStats.topLevelScratchBytes;
    budgetStats.instanceBufferBytes = m_rayTracingSceneStats.instanceBufferBytes;
    budgetStats.materialMetadataBufferBytes = m_rayTracingSceneStats.materialMetadataBufferBytes;
    budgetStats.alphaMetadataBufferBytes = m_rayTracingSceneStats.alphaMetadataBufferBytes;
    budgetStats.totalTrackedResourceBytes = m_rayTracingSceneStats.totalTrackedResourceBytes;
    budgetStats.resourceByteAccountingOverflowed = m_rayTracingSceneStats.resourceByteAccountingOverflowed;
    budgetStats.resourceBudgetExceeded =
        m_rayTracingBudgetSettings.enabled &&
        m_rayTracingBudgetSettings.maxTrackedResourceBytes > 0 &&
        m_rayTracingSceneStats.resourceBudgetExceeded;
    budgetStats.resourceBudgetEvictionAttempted = m_rayTracingSceneStats.resourceBudgetEvictionAttempted;
    budgetStats.shadowWidth = shadowRequested ? m_viewData.viewportWidth : 0;
    budgetStats.shadowHeight = shadowRequested ? m_viewData.viewportHeight : 0;

    const uint32 requestedShadowSamples =
        shadowRequested ? std::clamp(shadowConfig.rayTracedSamplesPerPixel, 1u, 8u) : 0u;
    const uint32 requestedReflectionSamples =
        reflectionRequested ? std::clamp(reflectionConfig.samplesPerPixel, 1u, 4u) : 0u;
    const float requestedReflectionScale =
        reflectionRequested ? ClampFiniteRange(reflectionConfig.resolutionScale, 1.0f, 0.25f, 1.0f) : 0.0f;
    const uint32 requestedDenoiseRadius = denoiseRequested ? std::min(denoiseConfig.radius, 3u) : 0u;

    uint32 appliedShadowSamples = requestedShadowSamples;
    uint32 appliedReflectionSamples = requestedReflectionSamples;
    float appliedReflectionScale = requestedReflectionScale;
    uint32 appliedDenoiseRadius = requestedDenoiseRadius;

    const auto estimateShadowRays =
        [this](uint32 samplesPerPixel) -> uint64
        {
            return EstimateRayTracingPixelCount(m_viewData.viewportWidth, m_viewData.viewportHeight) *
                   static_cast<uint64>(samplesPerPixel);
        };
    const auto estimateReflectionRays =
        [this](float resolutionScale, uint32 samplesPerPixel) -> uint64
        {
            return EstimateReflectionRayCount(m_viewData.viewportWidth,
                                              m_viewData.viewportHeight,
                                              resolutionScale,
                                              samplesPerPixel);
        };
    const auto estimateTotalRays =
        [&](uint32 shadowSamples, float reflectionScale, uint32 reflectionSamples) -> uint64
        {
            const uint64 shadowRays = shadowRequested ? estimateShadowRays(shadowSamples) : 0u;
            const uint64 reflectionRays = reflectionRequested
                                              ? estimateReflectionRays(reflectionScale, reflectionSamples)
                                              : 0u;
            return shadowRays + reflectionRays;
        };

    budgetStats.requestedShadowSamplesPerPixel = requestedShadowSamples;
    budgetStats.requestedReflectionSamplesPerPixel = requestedReflectionSamples;
    budgetStats.requestedReflectionResolutionScale = requestedReflectionScale;
    budgetStats.requestedReflectionDenoiseRadius = requestedDenoiseRadius;
    budgetStats.requestedReflectionDenoiseKernelTapCount =
        EstimateReflectionDenoiseKernelTapCount(requestedDenoiseRadius);
    budgetStats.estimatedRayCountBeforeBudget =
        estimateTotalRays(requestedShadowSamples, requestedReflectionScale, requestedReflectionSamples);
    budgetStats.estimatedDenoiseTapCountBeforeBudget =
        denoiseRequested
            ? EstimateReflectionDenoiseTapCount(m_viewData.viewportWidth,
                                                m_viewData.viewportHeight,
                                                requestedReflectionScale,
                                                requestedDenoiseRadius)
            : 0u;

    const RayTracedShadowPassStats& previousShadowStats = GetRayTracedShadowStats();
    const RayTracedReflectionPassStats& previousReflectionStats = GetRayTracedReflectionStats();
    const bool shadowTimingAvailable = previousShadowStats.gpuTimingResultAvailable;
    const bool reflectionTimingAvailable = previousReflectionStats.gpuTimingResultAvailable;
    const bool hasNewShadowGpuTimingSample =
        shadowTimingAvailable &&
        previousShadowStats.gpuTimingEndTimestamp != m_rayTracingGpuBudgetLastShadowEndTimestamp;
    const bool hasNewReflectionGpuTimingSample =
        reflectionTimingAvailable &&
        previousReflectionStats.gpuTimingEndTimestamp != m_rayTracingGpuBudgetLastReflectionEndTimestamp;
    const bool hasNewGpuTimingSample = hasNewShadowGpuTimingSample || hasNewReflectionGpuTimingSample;

    if (hasNewGpuTimingSample)
    {
        float measuredGpuMs = 0.0f;
        if (shadowTimingAvailable)
        {
            measuredGpuMs += previousShadowStats.gpuTimingElapsedMs;
        }
        if (reflectionTimingAvailable)
        {
            measuredGpuMs += previousReflectionStats.gpuTimingElapsedMs;
        }

        if (hasNewShadowGpuTimingSample && std::isfinite(previousShadowStats.gpuTimingElapsedMs) &&
            previousShadowStats.gpuTimingElapsedMs >= 0.0f)
        {
            m_rayTracingLastShadowMeasuredGpuMs = previousShadowStats.gpuTimingElapsedMs;
            m_rayTracingLastShadowMeasuredGpuMsValid = true;
        }
        if (hasNewReflectionGpuTimingSample && std::isfinite(previousReflectionStats.gpuTimingElapsedMs) &&
            previousReflectionStats.gpuTimingElapsedMs >= 0.0f)
        {
            m_rayTracingLastReflectionMeasuredGpuMs = previousReflectionStats.gpuTimingElapsedMs;
            m_rayTracingLastReflectionMeasuredGpuMsValid = true;
        }
        if (std::isfinite(measuredGpuMs) && measuredGpuMs >= 0.0f)
        {
            m_rayTracingLastMeasuredGpuMs = measuredGpuMs;
            m_rayTracingLastMeasuredGpuMsValid = true;
        }

        if (hasNewShadowGpuTimingSample)
        {
            m_rayTracingGpuBudgetLastShadowEndTimestamp = previousShadowStats.gpuTimingEndTimestamp;
        }
        if (hasNewReflectionGpuTimingSample)
        {
            m_rayTracingGpuBudgetLastReflectionEndTimestamp = previousReflectionStats.gpuTimingEndTimestamp;
        }
    }

    budgetStats.gpuTimeBudget = m_rayTracingBudgetSettings.maxMeasuredGpuMs;
    budgetStats.shadowGpuTimeBudget = m_rayTracingBudgetSettings.maxShadowMeasuredGpuMs;
    budgetStats.reflectionGpuTimeBudget = m_rayTracingBudgetSettings.maxReflectionMeasuredGpuMs;
    budgetStats.measuredGpuTimeAvailable = m_rayTracingLastMeasuredGpuMsValid;
    budgetStats.measuredGpuTimeForBudgetMs = m_rayTracingLastMeasuredGpuMsValid
                                               ? m_rayTracingLastMeasuredGpuMs
                                               : 0.0f;
    budgetStats.measuredShadowGpuTimeForBudgetMs = m_rayTracingLastShadowMeasuredGpuMsValid
                                                     ? m_rayTracingLastShadowMeasuredGpuMs
                                                     : 0.0f;
    budgetStats.measuredReflectionGpuTimeForBudgetMs = m_rayTracingLastReflectionMeasuredGpuMsValid
                                                         ? m_rayTracingLastReflectionMeasuredGpuMs
                                                         : 0.0f;
    budgetStats.gpuTimingRecoveryRate = m_rayTracingBudgetSettings.gpuTimingRecoveryRate;
    budgetStats.gpuTimeBudgetAdjustmentFrameCount =
        m_rayTracingBudgetSettings.gpuTimingAdjustmentFrameCount;

    const auto updateGpuBudgetFeedback =
        [this](float budget,
               float measuredGpuMs,
               bool measuredGpuMsValid,
               bool hasNewSample,
               bool requested,
               float& qualityScale,
               uint32& overBudgetFrameCount,
               uint32& underBudgetFrameCount,
               bool& budgetExceeded,
               bool& qualityScaleAdjusted)
        {
            if (!m_rayTracingBudgetSettings.enabled || budget <= 0.0f || !requested)
            {
                qualityScale = 1.0f;
                overBudgetFrameCount = 0;
                underBudgetFrameCount = 0;
                return;
            }

            if (!measuredGpuMsValid)
                return;

            budgetExceeded = measuredGpuMs > budget;
            if (!hasNewSample)
                return;

            const float recoveryThreshold = budget * (1.0f - m_rayTracingBudgetSettings.gpuTimingHysteresis);
            const uint32 adjustmentFrameCount = m_rayTracingBudgetSettings.gpuTimingAdjustmentFrameCount;
            if (budgetExceeded && measuredGpuMs > 0.0f)
            {
                ++overBudgetFrameCount;
                underBudgetFrameCount = 0;
                if (overBudgetFrameCount >= adjustmentFrameCount)
                {
                    const float targetScale = static_cast<float>(
                        std::sqrt(static_cast<double>(budget) / static_cast<double>(measuredGpuMs)));
                    const float previousQualityScale = qualityScale;
                    qualityScale = std::clamp(std::min(qualityScale, targetScale),
                                              m_rayTracingBudgetSettings.minReflectionResolutionScale,
                                              1.0f);
                    qualityScaleAdjusted = qualityScale < previousQualityScale;
                    overBudgetFrameCount = 0;
                }
            }
            else if (measuredGpuMs < recoveryThreshold && qualityScale < 1.0f)
            {
                ++underBudgetFrameCount;
                overBudgetFrameCount = 0;
                if (underBudgetFrameCount >= adjustmentFrameCount)
                {
                    const float previousQualityScale = qualityScale;
                    qualityScale = std::min(
                        1.0f,
                        qualityScale * (1.0f + m_rayTracingBudgetSettings.gpuTimingRecoveryRate));
                    qualityScaleAdjusted = qualityScale > previousQualityScale;
                    underBudgetFrameCount = 0;
                }
            }
            else
            {
                overBudgetFrameCount = 0;
                underBudgetFrameCount = 0;
            }
        };

    updateGpuBudgetFeedback(m_rayTracingBudgetSettings.maxMeasuredGpuMs,
                            m_rayTracingLastMeasuredGpuMs,
                            m_rayTracingLastMeasuredGpuMsValid,
                            hasNewGpuTimingSample,
                            shadowRequested || reflectionRequested,
                            m_rayTracingGpuBudgetQualityScale,
                            m_rayTracingGpuBudgetOverBudgetFrameCount,
                            m_rayTracingGpuBudgetUnderBudgetFrameCount,
                            budgetStats.gpuTimeBudgetExceeded,
                            budgetStats.gpuTimeBudgetQualityScaleAdjusted);
    updateGpuBudgetFeedback(m_rayTracingBudgetSettings.maxShadowMeasuredGpuMs,
                            m_rayTracingLastShadowMeasuredGpuMs,
                            m_rayTracingLastShadowMeasuredGpuMsValid,
                            hasNewShadowGpuTimingSample,
                            shadowRequested,
                            m_rayTracingShadowGpuBudgetQualityScale,
                            m_rayTracingShadowGpuBudgetOverBudgetFrameCount,
                            m_rayTracingShadowGpuBudgetUnderBudgetFrameCount,
                            budgetStats.shadowGpuTimeBudgetExceeded,
                            budgetStats.shadowGpuTimeBudgetQualityScaleAdjusted);
    updateGpuBudgetFeedback(m_rayTracingBudgetSettings.maxReflectionMeasuredGpuMs,
                            m_rayTracingLastReflectionMeasuredGpuMs,
                            m_rayTracingLastReflectionMeasuredGpuMsValid,
                            hasNewReflectionGpuTimingSample,
                            reflectionRequested,
                            m_rayTracingReflectionGpuBudgetQualityScale,
                            m_rayTracingReflectionGpuBudgetOverBudgetFrameCount,
                            m_rayTracingReflectionGpuBudgetUnderBudgetFrameCount,
                            budgetStats.reflectionGpuTimeBudgetExceeded,
                            budgetStats.reflectionGpuTimeBudgetQualityScaleAdjusted);

    budgetStats.gpuTimeBudgetOverBudgetFrameCount = m_rayTracingGpuBudgetOverBudgetFrameCount;
    budgetStats.gpuTimeBudgetUnderBudgetFrameCount = m_rayTracingGpuBudgetUnderBudgetFrameCount;
    budgetStats.shadowGpuTimeBudgetOverBudgetFrameCount = m_rayTracingShadowGpuBudgetOverBudgetFrameCount;
    budgetStats.shadowGpuTimeBudgetUnderBudgetFrameCount = m_rayTracingShadowGpuBudgetUnderBudgetFrameCount;
    budgetStats.reflectionGpuTimeBudgetOverBudgetFrameCount = m_rayTracingReflectionGpuBudgetOverBudgetFrameCount;
    budgetStats.reflectionGpuTimeBudgetUnderBudgetFrameCount = m_rayTracingReflectionGpuBudgetUnderBudgetFrameCount;

    if (m_rayTracingBudgetSettings.enabled && m_rayTracingBudgetSettings.maxRayCount > 0 &&
        budgetStats.estimatedRayCountBeforeBudget > m_rayTracingBudgetSettings.maxRayCount)
    {
        const uint32 minReflectionSamples = std::min(m_rayTracingBudgetSettings.minReflectionSamplesPerPixel,
                                                     appliedReflectionSamples == 0 ? 1u : appliedReflectionSamples);
        const uint32 minShadowSamples = std::min(m_rayTracingBudgetSettings.minShadowSamplesPerPixel,
                                                 appliedShadowSamples == 0 ? 1u : appliedShadowSamples);

        if (reflectionRequested && appliedReflectionSamples > minReflectionSamples)
        {
            appliedReflectionSamples = minReflectionSamples;
            budgetStats.budgetApplied = true;
        }
        if (shadowRequested && appliedShadowSamples > minShadowSamples)
        {
            appliedShadowSamples = minShadowSamples;
            budgetStats.budgetApplied = true;
        }

        uint64 estimatedRays = estimateTotalRays(appliedShadowSamples,
                                                 appliedReflectionScale,
                                                 appliedReflectionSamples);
        if (reflectionRequested && estimatedRays > m_rayTracingBudgetSettings.maxRayCount &&
            appliedReflectionScale > m_rayTracingBudgetSettings.minReflectionResolutionScale)
        {
            const uint64 shadowRays = shadowRequested ? estimateShadowRays(appliedShadowSamples) : 0u;
            const uint64 remainingReflectionBudget =
                m_rayTracingBudgetSettings.maxRayCount > shadowRays
                    ? m_rayTracingBudgetSettings.maxRayCount - shadowRays
                    : 0u;
            float budgetScale = m_rayTracingBudgetSettings.minReflectionResolutionScale;
            if (remainingReflectionBudget > 0 && appliedReflectionSamples > 0 &&
                m_viewData.viewportWidth > 0 && m_viewData.viewportHeight > 0)
            {
                const double fullResolutionPixels = static_cast<double>(m_viewData.viewportWidth) *
                                                    static_cast<double>(m_viewData.viewportHeight);
                const double affordablePixels =
                    static_cast<double>(remainingReflectionBudget) /
                    static_cast<double>(appliedReflectionSamples);
                budgetScale = static_cast<float>(std::sqrt(std::max(0.0, affordablePixels / fullResolutionPixels)));
            }
            const float previousReflectionScale = appliedReflectionScale;
            appliedReflectionScale = std::clamp(budgetScale,
                                                m_rayTracingBudgetSettings.minReflectionResolutionScale,
                                                appliedReflectionScale);
            appliedReflectionScale = ReduceReflectionResolutionScaleToFitRayBudget(
                m_viewData.viewportWidth,
                m_viewData.viewportHeight,
                appliedReflectionScale,
                m_rayTracingBudgetSettings.minReflectionResolutionScale,
                appliedReflectionSamples,
                remainingReflectionBudget);
            budgetStats.budgetApplied = budgetStats.budgetApplied ||
                                        appliedReflectionScale < previousReflectionScale;
            estimatedRays = estimateTotalRays(appliedShadowSamples,
                                              appliedReflectionScale,
                                              appliedReflectionSamples);
        }

        budgetStats.rayBudgetExceeded = estimatedRays > m_rayTracingBudgetSettings.maxRayCount;
    }

    if (m_rayTracingBudgetSettings.enabled && m_rayTracingBudgetSettings.maxDenoiseTapCount > 0 &&
        budgetStats.estimatedDenoiseTapCountBeforeBudget > m_rayTracingBudgetSettings.maxDenoiseTapCount)
    {
        const uint32 minDenoiseRadius = std::min(m_rayTracingBudgetSettings.minReflectionDenoiseRadius,
                                                 appliedDenoiseRadius);
        while (appliedDenoiseRadius > minDenoiseRadius)
        {
            const uint64 estimatedTapCount =
                EstimateReflectionDenoiseTapCount(m_viewData.viewportWidth,
                                                  m_viewData.viewportHeight,
                                                  appliedReflectionScale,
                                                  appliedDenoiseRadius);
            if (estimatedTapCount <= m_rayTracingBudgetSettings.maxDenoiseTapCount)
                break;

            --appliedDenoiseRadius;
            budgetStats.budgetApplied = true;
        }
    }

    const bool gpuTimeBudgetingEnabled =
        m_rayTracingBudgetSettings.enabled &&
        (m_rayTracingBudgetSettings.maxMeasuredGpuMs > 0.0f ||
         m_rayTracingBudgetSettings.maxShadowMeasuredGpuMs > 0.0f ||
         m_rayTracingBudgetSettings.maxReflectionMeasuredGpuMs > 0.0f);
    const float shadowGpuQualityScale =
        std::min(m_rayTracingGpuBudgetQualityScale, m_rayTracingShadowGpuBudgetQualityScale);
    const float reflectionGpuQualityScale =
        std::min(m_rayTracingGpuBudgetQualityScale, m_rayTracingReflectionGpuBudgetQualityScale);
    if (gpuTimeBudgetingEnabled && shadowRequested && shadowGpuQualityScale < 0.999f)
    {
        appliedShadowSamples = ApplyRayTracingGpuBudgetScale(
            appliedShadowSamples,
            std::min(m_rayTracingBudgetSettings.minShadowSamplesPerPixel,
                     appliedShadowSamples == 0 ? 1u : appliedShadowSamples),
            shadowGpuQualityScale);
        budgetStats.shadowGpuTimeBudgetApplied = true;
    }
    if (gpuTimeBudgetingEnabled && reflectionRequested && reflectionGpuQualityScale < 0.999f)
    {
        appliedReflectionScale = std::max(m_rayTracingBudgetSettings.minReflectionResolutionScale,
                                          appliedReflectionScale * reflectionGpuQualityScale);
        appliedReflectionSamples = ApplyRayTracingGpuBudgetScale(
            appliedReflectionSamples,
            std::min(m_rayTracingBudgetSettings.minReflectionSamplesPerPixel,
                     appliedReflectionSamples == 0 ? 1u : appliedReflectionSamples),
            reflectionGpuQualityScale);
        budgetStats.reflectionGpuTimeBudgetApplied = true;
    }
    if (gpuTimeBudgetingEnabled && denoiseRequested && reflectionGpuQualityScale < 0.999f)
    {
        appliedDenoiseRadius = ApplyRayTracingGpuBudgetScale(
            appliedDenoiseRadius,
            std::min(m_rayTracingBudgetSettings.minReflectionDenoiseRadius, appliedDenoiseRadius),
            reflectionGpuQualityScale);
        budgetStats.reflectionGpuTimeBudgetApplied = true;
    }
    budgetStats.gpuTimeBudgetApplied =
        budgetStats.shadowGpuTimeBudgetApplied || budgetStats.reflectionGpuTimeBudgetApplied;
    if (budgetStats.gpuTimeBudgetApplied)
    {
        budgetStats.budgetApplied = true;
    }
    budgetStats.gpuTimeBudgetQualityScale = m_rayTracingGpuBudgetQualityScale;
    budgetStats.shadowGpuTimeBudgetQualityScale = m_rayTracingShadowGpuBudgetQualityScale;
    budgetStats.reflectionGpuTimeBudgetQualityScale = m_rayTracingReflectionGpuBudgetQualityScale;

    if (shadowRequested)
    {
        shadowConfig.rayTracedSamplesPerPixel = appliedShadowSamples;
    }
    if (reflectionRequested)
    {
        reflectionConfig.samplesPerPixel = appliedReflectionSamples;
        reflectionConfig.resolutionScale = appliedReflectionScale;
    }
    if (denoiseRequested)
    {
        denoiseConfig.radius = appliedDenoiseRadius;
    }

    budgetStats.shadowSamplesPerPixel = appliedShadowSamples;
    budgetStats.reflectionSamplesPerPixel = appliedReflectionSamples;
    budgetStats.reflectionResolutionScale = appliedReflectionScale;
    budgetStats.reflectionDenoiseRadius = appliedDenoiseRadius;
    budgetStats.reflectionDenoiseKernelTapCount = EstimateReflectionDenoiseKernelTapCount(appliedDenoiseRadius);
    budgetStats.reflectionWidth = reflectionRequested
                                      ? ResolveRayTracingBudgetReflectionDimension(m_viewData.viewportWidth,
                                                                                   appliedReflectionScale)
                                      : 0u;
    budgetStats.reflectionHeight = reflectionRequested
                                       ? ResolveRayTracingBudgetReflectionDimension(m_viewData.viewportHeight,
                                                                                    appliedReflectionScale)
                                       : 0u;
    budgetStats.estimatedShadowRayCount = shadowRequested ? estimateShadowRays(appliedShadowSamples) : 0u;
    budgetStats.estimatedReflectionRayCount = reflectionRequested
                                                  ? estimateReflectionRays(appliedReflectionScale,
                                                                           appliedReflectionSamples)
                                                  : 0u;
    budgetStats.estimatedTotalRayCount = budgetStats.estimatedShadowRayCount +
                                         budgetStats.estimatedReflectionRayCount;
    budgetStats.estimatedReflectionDenoiseTapCount =
        denoiseRequested
            ? EstimateReflectionDenoiseTapCount(m_viewData.viewportWidth,
                                                m_viewData.viewportHeight,
                                                appliedReflectionScale,
                                                appliedDenoiseRadius)
            : 0u;
    if (m_rayTracingBudgetSettings.enabled && m_rayTracingBudgetSettings.maxRayCount > 0)
    {
        budgetStats.rayBudgetExceeded =
            budgetStats.estimatedTotalRayCount > m_rayTracingBudgetSettings.maxRayCount;
    }
    if (m_rayTracingBudgetSettings.enabled && m_rayTracingBudgetSettings.maxMeasuredGpuMs > 0.0f &&
        m_rayTracingLastMeasuredGpuMsValid)
    {
        budgetStats.gpuTimeBudgetExceeded =
            m_rayTracingLastMeasuredGpuMs > m_rayTracingBudgetSettings.maxMeasuredGpuMs;
    }
    if (m_rayTracingBudgetSettings.enabled && m_rayTracingBudgetSettings.maxShadowMeasuredGpuMs > 0.0f &&
        m_rayTracingLastShadowMeasuredGpuMsValid)
    {
        budgetStats.shadowGpuTimeBudgetExceeded =
            m_rayTracingLastShadowMeasuredGpuMs > m_rayTracingBudgetSettings.maxShadowMeasuredGpuMs;
    }
    if (m_rayTracingBudgetSettings.enabled && m_rayTracingBudgetSettings.maxReflectionMeasuredGpuMs > 0.0f &&
        m_rayTracingLastReflectionMeasuredGpuMsValid)
    {
        budgetStats.reflectionGpuTimeBudgetExceeded =
            m_rayTracingLastReflectionMeasuredGpuMs > m_rayTracingBudgetSettings.maxReflectionMeasuredGpuMs;
    }
    if (m_rayTracingBudgetSettings.enabled && m_rayTracingBudgetSettings.maxDenoiseTapCount > 0)
    {
        budgetStats.denoiseTapBudgetExceeded =
            budgetStats.estimatedReflectionDenoiseTapCount > m_rayTracingBudgetSettings.maxDenoiseTapCount;
    }
    if (m_rayTracingBudgetSettings.enabled && m_rayTracingBudgetSettings.maxTrackedResourceBytes > 0)
    {
        budgetStats.resourceBudgetExceeded = m_rayTracingSceneStats.resourceBudgetExceeded;
    }

    m_rayTracingFrameBudgetStats = budgetStats;
}

bool SceneRenderer::PrepareLightingFramePlan(std::string& outDiagnostic)
{
    outDiagnostic.clear();
    m_lightingFramePlan.reset();

    const RenderFrameHeaderV5& header = m_renderScene.GetAcceptedHeader();
    const RenderViewSnapshot& view = m_renderScene.GetView();
    const RenderFrameSettings& settings = m_renderScene.GetSettings();

    LightingFramePlanRequest request;
    request.frameIdentity = RenderFrameIdentity{
        header.sequence,
        header.requiredSceneRevision,
        header.surfaceGeneration,
        header.configurationRevision};
    request.viewIdentity = view.identity;
    request.lightingConfigurationRevision = header.configurationRevision;
    request.quality = settings.pipelineRequest.quality;
    request.qualityRevision = ComputeRenderPipelineProfileHash(
        settings.pipelineRequest.recipe,
        settings.pipelineRequest.quality);
    request.pointLightCapacity = LightManager::MaxPointLights;
    request.spotLightCapacity = LightManager::MaxSpotLights;
    // No quality tier silently drops authored lights. A future recipe may opt
    // into a declared stable truncation fallback, but required rendering and
    // qualification remain fail-closed until that policy is machine-visible.
    request.allowLocalLightTruncation = false;

    request.lightCandidates.reserve(m_renderScene.GetLightCount());
    for (const RenderLight& light : m_renderScene.GetLights())
    {
        if (!(light.intensity > 0.0f) ||
            !IsRenderLayerVisible(light.layerMask, m_viewData.cullingMask))
        {
            continue;
        }

        LightingLightCandidate candidate;
        candidate.lightId = light.lightId;
        switch (light.type)
        {
            case RenderLight::Type::Directional:
                candidate.type = RenderLightType::Directional;
                break;
            case RenderLight::Type::Point:
                candidate.type = RenderLightType::Point;
                break;
            case RenderLight::Type::Spot:
                candidate.type = RenderLightType::Spot;
                break;
            default:
                outDiagnostic = "RenderScene contains an undeclared light type";
                return false;
        }
        // Equal authoring priority is intentional for the first contract:
        // stable lightId is the deterministic tie-breaker and scene order is
        // never used as hidden policy.
        candidate.priority = 0;
        candidate.position = light.position;
        candidate.direction = light.direction;
        candidate.color = light.color;
        if (light.colorTemperatureKelvin.has_value())
        {
            candidate.color *= EvaluateBlackbodyColorTemperature(
                *light.colorTemperatureKelvin);
        }
        candidate.normalizedIntensity = light.intensity;
        candidate.range = light.range;
        candidate.innerConeRadians = light.innerConeAngle;
        candidate.outerConeRadians = light.outerConeAngle;
        candidate.layerMask = light.layerMask;
        candidate.castsShadows = light.castsShadow;
        request.lightCandidates.push_back(std::move(candidate));
    }

    // Particle systems contribute at most one value-owned, unshadowed point
    // light each. This happens before LightingFramePlan is sealed so every
    // raster path consumes the same admitted set; particle draw submission
    // never reaches back into LightManager.
    std::vector<ParticleRenderLightHint> particleLightHints;
    if (FindCompositionPass<ParticleFeaturePass>() != nullptr)
    {
        std::string particleLightDiagnostic;
        if (!FindCompositionPass<ParticleFeaturePass>()->CollectLightingHints(
                particleLightHints, particleLightDiagnostic))
        {
            FindCompositionPass<ParticleFeaturePass>()->SetLightingHintAdmissionStats(0u, 0u, 0u);
            outDiagnostic = particleLightDiagnostic.empty()
                ? "Particle light hint validation failed"
                : std::move(particleLightDiagnostic);
            return false;
        }
        if (particleLightHints.size() > std::numeric_limits<uint32>::max())
        {
            FindCompositionPass<ParticleFeaturePass>()->SetLightingHintAdmissionStats(0u, 0u, 0u);
            outDiagnostic = "Particle light hint count exceeds uint32 diagnostics";
            return false;
        }
        request.lightCandidates.reserve(request.lightCandidates.size() +
                                        particleLightHints.size());
        for (const ParticleRenderLightHint& hint : particleLightHints)
        {
            LightingLightCandidate candidate;
            candidate.lightId = hint.stableLightId;
            candidate.type = RenderLightType::Point;
            candidate.priority = 0u;
            candidate.position = hint.position;
            candidate.color = hint.color;
            candidate.normalizedIntensity = hint.intensityCandela;
            candidate.range = hint.rangeMeters;
            candidate.layerMask = m_viewData.cullingMask;
            candidate.castsShadows = false;
            request.lightCandidates.push_back(std::move(candidate));
        }
    }

    // The candidate order and capacity selection are data, not insertion
    // order. In particular this gives particle hints stable admission when a
    // declared local-light truncation policy is enabled in a future recipe.
    std::sort(request.lightCandidates.begin(), request.lightCandidates.end(),
              [](const LightingLightCandidate& left,
                 const LightingLightCandidate& right)
              {
                  return left.lightId < right.lightId;
              });
    for (size_t index = 1; index < request.lightCandidates.size(); ++index)
    {
        if (request.lightCandidates[index - 1].lightId ==
            request.lightCandidates[index].lightId)
        {
            if (FindCompositionPass<ParticleFeaturePass>() != nullptr)
            {
                const uint32 requested = static_cast<uint32>(
                    particleLightHints.size());
                FindCompositionPass<ParticleFeaturePass>()->SetLightingHintAdmissionStats(
                    requested, 0u, requested);
            }
            outDiagnostic =
                "Lighting frame plan rejects duplicate authored or particle stable light identities";
            return false;
        }
    }

    // The frame's accepted scene revision is the authoritative incremental
    // lighting-state revision. Candidate values are additionally mixed into
    // the plan identity by LightingFramePlanBuilder.
    uint64 sceneLightStateHash = 0xCBF29CE484222325ull;
    sceneLightStateHash = MixGPUCullingQualificationIdentity(
        sceneLightStateHash, header.requiredSceneRevision);
    sceneLightStateHash = MixGPUCullingQualificationIdentity(
        sceneLightStateHash, request.lightCandidates.size());
    request.sceneLightStateHash = sceneLightStateHash == 0
        ? 1 : sceneLightStateHash;

    const auto primaryDirectional = std::min_element(
        request.lightCandidates.begin(),
        request.lightCandidates.end(),
        [](const LightingLightCandidate& left,
           const LightingLightCandidate& right)
        {
            if (left.type != RenderLightType::Directional)
            {
                return false;
            }
            if (right.type != RenderLightType::Directional)
            {
                return true;
            }
            return left.priority != right.priority
                ? left.priority > right.priority
                : left.lightId < right.lightId;
        });
    const bool primaryDirectionalSelected =
        primaryDirectional != request.lightCandidates.end() &&
        primaryDirectional->type == RenderLightType::Directional;
    request.csm.requested = settings.shadows.enabled &&
        primaryDirectionalSelected && primaryDirectional->castsShadows;
    if (request.csm.requested)
    {
        request.csm.inputsReady = m_shadowPassConfig.shadowMapSize != 0 &&
            m_shadowPassConfig.numCascades != 0 &&
            m_shadowPassConfig.numCascades <=
                RVX_MAX_DIRECTIONAL_SHADOW_CASCADES;
        request.csm.cascadeCount = m_shadowPassConfig.numCascades;
        request.csm.configurationRevision = header.configurationRevision;
    }

    const RenderEnvironmentSnapshot& environment =
        m_renderScene.GetEnvironment();
    const bool completeEnvironmentHandles =
        environment.irradianceTexture.IsValid() &&
        environment.prefilteredTexture.IsValid() &&
        environment.brdfLutTexture.IsValid();
    const RenderTextureResourceData* irradiance = nullptr;
    const RenderTextureResourceData* prefiltered = nullptr;
    const RenderTextureResourceData* brdfLUT = nullptr;
    if (completeEnvironmentHandles && m_renderResourceRegistry)
    {
        irradiance = m_renderResourceRegistry->ResolveTexture(
            environment.irradianceTexture);
        prefiltered = m_renderResourceRegistry->ResolveTexture(
            environment.prefilteredTexture);
        brdfLUT = m_renderResourceRegistry->ResolveTexture(
            environment.brdfLutTexture);
    }
    const bool completeEnvironmentResources = irradiance && irradiance->texture &&
        prefiltered && prefiltered->texture && brdfLUT && brdfLUT->texture;
    request.ibl.requested = m_environmentIBLRequested;
    if (request.ibl.requested)
    {
        request.ibl.inputsReady = completeEnvironmentResources;
        request.ibl.normalizedIntensity = environment.intensity;
        if (request.ibl.inputsReady)
        {
            uint64 resourceRevision = 0xCBF29CE484222325ull;
            const auto mixHandle = [&resourceRevision](RenderResourceHandle handle)
            {
                resourceRevision = MixGPUCullingQualificationIdentity(
                    resourceRevision,
                    (static_cast<uint64>(handle.slot) << 32U) |
                        handle.generation);
            };
            mixHandle(environment.irradianceTexture);
            mixHandle(environment.prefilteredTexture);
            mixHandle(environment.brdfLutTexture);
            request.ibl.resourceRevision = resourceRevision == 0
                ? 1 : resourceRevision;
            request.ibl.prefilteredMipLevels =
                std::max(1U, prefiltered->texture->GetMipLevels());
        }
    }

    LightingFramePlanBuilder builder;
    LightingFramePlanBuildResult result = builder.Build(request);
    if (!result.Succeeded() || !result.plan.IsValid())
    {
        if (FindCompositionPass<ParticleFeaturePass>() != nullptr)
        {
            const uint32 requested = static_cast<uint32>(particleLightHints.size());
            // A failed plan publishes no local-light set, so none of the
            // packet hints may be reported as admitted.
            FindCompositionPass<ParticleFeaturePass>()->SetLightingHintAdmissionStats(
                requested, 0u, requested);
        }
        outDiagnostic = result.diagnostic.empty()
            ? GetLightingFramePlanCodeName(result.code)
            : std::move(result.diagnostic);
        return false;
    }

    if (FindCompositionPass<ParticleFeaturePass>() != nullptr)
    {
        uint32 admitted = 0u;
        const std::vector<LightingLightCandidate>& localLights =
            result.plan.GetLocalLights();
        for (const ParticleRenderLightHint& hint : particleLightHints)
        {
            const bool wasAdmitted = std::any_of(
                localLights.begin(), localLights.end(),
                [&hint](const LightingLightCandidate& candidate)
                {
                    return candidate.lightId == hint.stableLightId;
                });
            admitted += wasAdmitted ? 1u : 0u;
        }
        const uint32 requested = static_cast<uint32>(particleLightHints.size());
        FindCompositionPass<ParticleFeaturePass>()->SetLightingHintAdmissionStats(
            requested, admitted, requested - admitted);
    }

    m_lightingFramePlan =
        std::make_shared<const LightingFramePlan>(std::move(result.plan));
    return true;
}

bool SceneRenderer::PrepareLocalShadowPlans(std::string& outDiagnostic)
{
    outDiagnostic.clear();
    m_localShadowFramePlan.reset();
    m_localShadowAtlasPlan.reset();
    if (!m_lightingFramePlan || !m_lightingFramePlan->IsValid())
    {
        outDiagnostic =
            "Local-shadow planning requires the current sealed LightingFramePlan";
        return false;
    }

    const LocalShadowAtlasConfig atlasConfig =
        ResolveLocalShadowAtlasConfig(m_lightingFramePlan->GetQuality());
    if (atlasConfig.pageResolution == 0 || atlasConfig.pageCapacity == 0 ||
        atlasConfig.pointLightCapacity == 0 ||
        atlasConfig.spotLightCapacity == 0)
    {
        outDiagnostic = "The selected quality tier has no local-shadow atlas contract";
        return false;
    }

    const uint64 configurationHash = HashLocalShadowAtlasConfig(atlasConfig);
    if (m_localShadowAtlasConfigurationHash != 0 &&
        m_localShadowAtlasConfigurationHash != configurationHash)
    {
        ++m_localShadowAtlasGeneration;
        if (m_localShadowAtlasGeneration == 0)
        {
            m_localShadowAtlasGeneration = 1;
        }
    }
    m_localShadowAtlasConfigurationHash = configurationHash;

    LocalShadowFramePlanRequest frameRequest;
    frameRequest.atlas = atlasConfig;
    frameRequest.atlasGeneration = m_localShadowAtlasGeneration;
    frameRequest.expectedLightingPlanIdentity =
        m_lightingFramePlan->GetIdentityHash();
    LocalShadowFramePlanBuilder frameBuilder;
    LocalShadowFramePlanBuildResult frameResult =
        frameBuilder.Build(*m_lightingFramePlan, frameRequest);
    if (!frameResult.Succeeded() || !frameResult.plan.IsValid())
    {
        outDiagnostic = frameResult.diagnostic.empty()
            ? GetLocalShadowFramePlanCodeName(frameResult.code)
            : std::move(frameResult.diagnostic);
        return false;
    }

    auto framePlan = std::make_shared<const LocalShadowFramePlan>(
        std::move(frameResult.plan));
    LocalShadowAtlasPlanRequest atlasRequest;
    atlasRequest.atlasGeneration = m_localShadowAtlasGeneration;
    atlasRequest.expectedLocalShadowPlanIdentity = framePlan->GetIdentityHash();
    LocalShadowAtlasPlanBuilder atlasBuilder;
    LocalShadowAtlasPlanBuildResult atlasResult = atlasBuilder.Build(
        *m_lightingFramePlan, *framePlan, atlasRequest);
    if (!atlasResult.Succeeded() || !atlasResult.plan.IsValid())
    {
        outDiagnostic = atlasResult.diagnostic.empty()
            ? GetLocalShadowAtlasPlanCodeName(atlasResult.code)
            : std::move(atlasResult.diagnostic);
        return false;
    }

    m_localShadowFramePlan = std::move(framePlan);
    m_localShadowAtlasPlan = std::make_shared<const LocalShadowAtlasPlan>(
        std::move(atlasResult.plan));
    return true;
}

bool SceneRenderer::PrepareReflectionFramePlan(std::string& outDiagnostic)
{
    outDiagnostic.clear();
    m_reflectionFramePlan.reset();
    if (!m_lightingFramePlan || !m_lightingFramePlan->IsValid())
    {
        outDiagnostic =
            "Reflection planning requires the current sealed LightingFramePlan";
        return false;
    }

    const RenderFrameHeaderV5& header = m_renderScene.GetAcceptedHeader();
    const RenderViewSnapshot& view = m_renderScene.GetView();
    const RenderFrameSettings& settings = m_renderScene.GetSettings();
    const RenderFrameIdentity frameIdentity{
        header.sequence,
        header.requiredSceneRevision,
        header.surfaceGeneration,
        header.configurationRevision};
    const RenderOutputExtent extent{
        m_viewData.viewportWidth, m_viewData.viewportHeight};

    ReflectionFramePlanRequest request;
    request.frameIdentity = frameIdentity;
    request.viewIdentity = view.identity;
    request.sceneProbeStateHash = m_reflectionProbeStateHash;
    request.viewPosition = view.cameraPosition;
    // Probe admission must use the sealed non-jittered camera frustum, not
    // ReflectionFramePlanRequest's identity default.
    request.viewProjection = view.viewProjectionMatrix;
    request.viewCullingMask = m_viewData.cullingMask;
    request.probeCandidates = m_reflectionProbeSnapshots;

    const std::optional<SSRConfig> resolvedSSRQualityPolicy =
        ResolveSSRQualityPolicy(settings.pipelineRequest.quality);
    if (!resolvedSSRQualityPolicy.has_value())
    {
        outDiagnostic =
            "Reflection planning rejected an undeclared SSR quality tier";
        return false;
    }
    const SSRConfig& ssrConfig = *resolvedSSRQualityPolicy;
    const uint64 ssrConfigurationIdentity =
        ComputeSSRConfigurationIdentity(ssrConfig);
    const SSRPipelineIdentityResolution ssrPipeline =
        ResolveSSRPipelineIdentity(m_pipelineCache.get());

    request.ssr.requested = settings.postProcess.enabled &&
        settings.postProcess.enableSSR;
    if (request.ssr.requested)
    {
        request.ssr.configurationIdentityHash = ssrConfigurationIdentity;
        request.ssr.inputsReady = ssrPipeline.ready && extent.IsValid();
        if (request.ssr.inputsReady)
        {
            request.ssr.pipelineIdentityHash = ssrPipeline.identity;
            request.ssr.depthPyramidIdentityHash =
                ComputeSSRDepthPyramidIdentity(
                    frameIdentity, view.identity, extent,
                    ssrConfigurationIdentity, ssrPipeline.identity);
        }
    }

    request.ibl.lightingPlanIdentityHash =
        m_lightingFramePlan->GetIdentityHash();
    const LightingIBLRequest& lightingIBL =
        m_lightingFramePlan->GetIBLRequest();
    if (lightingIBL.requested && lightingIBL.inputsReady)
    {
        request.ibl.source = ReflectionIBLSource::Texture;
        request.ibl.resourceRevision = lightingIBL.resourceRevision;
        request.ibl.prefilteredMipLevels =
            lightingIBL.prefilteredMipLevels;
    }
    else
    {
        request.ibl.source = ReflectionIBLSource::Analytic;
        uint64 analyticIdentity = 0xCBF29CE484222325ULL;
        analyticIdentity = MixGPUCullingQualificationIdentity(
            analyticIdentity, request.ibl.lightingPlanIdentityHash);
        analyticIdentity = MixGPUCullingQualificationIdentity(
            analyticIdentity, settings.pipelineRequest.quality ==
                RenderQualityTier::Low ? 1U : 2U);
        request.ibl.analyticFallbackIdentityHash = analyticIdentity == 0
            ? 1 : analyticIdentity;
    }

    uint64 fallbackIdentity = 0xCBF29CE484222325ULL;
    fallbackIdentity = MixGPUCullingQualificationIdentity(
        fallbackIdentity, header.configurationRevision);
    fallbackIdentity = MixGPUCullingQualificationIdentity(
        fallbackIdentity, ssrConfigurationIdentity);
    request.fallback.configurationIdentityHash = fallbackIdentity == 0
        ? 1 : fallbackIdentity;
    uint64 reflectionRevision = MixGPUCullingQualificationIdentity(
        request.fallback.configurationIdentityHash,
        m_reflectionProbeStateHash);
    request.reflectionConfigurationRevision = reflectionRevision == 0
        ? 1 : reflectionRevision;

    ReflectionFramePlanBuilder builder;
    ReflectionFramePlanBuildResult result = builder.Build(request);
    if (!result.Succeeded() || !result.plan.IsValid())
    {
        outDiagnostic = result.diagnostic.empty()
            ? GetReflectionFramePlanCodeName(result.code)
            : std::move(result.diagnostic);
        return false;
    }
    m_reflectionFramePlan =
        std::make_shared<const ReflectionFramePlan>(std::move(result.plan));
    return true;
}

void SceneRenderer::PreparePassesForFrame()
{
    m_activeLocalShadowPass.reset();
    m_lightingFramePlanFailureReason.clear();
    if (!PrepareLightingFramePlan(m_lightingFramePlanFailureReason))
    {
        RVX_CORE_ERROR(
            "SceneRenderer: LightingFramePlan preparation failed: {}",
            m_lightingFramePlanFailureReason);
        m_lightingFramePlan.reset();
    }

    m_localShadowPlanFailureReason.clear();
    if (!PrepareLocalShadowPlans(m_localShadowPlanFailureReason))
    {
        RVX_CORE_ERROR(
            "SceneRenderer: local-shadow planning failed: {}",
            m_localShadowPlanFailureReason);
        m_localShadowFramePlan.reset();
        m_localShadowAtlasPlan.reset();
    }

    m_reflectionFramePlanFailureReason.clear();
    if (!PrepareReflectionFramePlan(m_reflectionFramePlanFailureReason))
    {
        RVX_CORE_ERROR(
            "SceneRenderer: ReflectionFramePlan preparation failed: {}",
            m_reflectionFramePlanFailureReason);
        m_reflectionFramePlan.reset();
    }

    m_primaryDirectionalLight = {};
    m_primaryDirectionalLight.intensity = 0.0f;
    m_primaryDirectionalLight.color = Vec3(0.0f);
    if (m_lightingFramePlan)
    {
        if (const LightingLightCandidate* primary =
                m_lightingFramePlan->GetPrimaryDirectionalLight();
            primary != nullptr)
        {
            m_primaryDirectionalLight.selected = true;
            m_primaryDirectionalLight.castsShadow = primary->castsShadows;
            m_primaryDirectionalLight.direction = primary->direction;
            m_primaryDirectionalLight.color = primary->color;
            m_primaryDirectionalLight.intensity =
                primary->normalizedIntensity;
        }
    }
    m_viewData.directionalLightDirection = m_primaryDirectionalLight.direction;
    m_viewData.directionalLightIntensity = m_primaryDirectionalLight.intensity;
    m_viewData.directionalLightColor = m_primaryDirectionalLight.color;
    m_viewData.directionalShadowEnabled = 0;
    m_viewData.directionalShadowCascadeCount = 0;
    m_viewData.directionalShadowCascadeSplits = Vec4(0.0f, 0.0f, 0.0f, 0.0f);
    m_viewData.directionalShadowCascadeFadeDistances = Vec4(0.0f, 0.0f, 0.0f, 0.0f);
    m_viewData.directionalShadowViewProjection = Mat4Identity();
    m_viewData.directionalShadowInvMapSize = 0.0f;
    m_viewData.directionalShadowFilterRadiusTexels = 1.0f;
    m_viewData.directionalShadowNormalBias = 0.0f;
    m_viewData.rayTracedShadowEnabled = 0;
    m_viewData.rayTracedShadowFilterRadiusPixels = 1.0f;
    m_viewData.rayTracedShadowMode = RayTracedShadowMode::ComplementRaster;

    if (FindCompositionPass<RayTracedShadowPass>())
    {
        FindCompositionPass<RayTracedShadowPass>()->SetRayTracingScene(m_rayTracingSceneManager.get());
    }
    if (FindCompositionPass<CameraVelocityPass>())
    {
        FindCompositionPass<CameraVelocityPass>()->SetEnabled(false);
    }
    if (FindCompositionPass<ObjectVelocityPass>())
    {
        FindCompositionPass<ObjectVelocityPass>()->SetEnabled(false);
    }
    if (FindCompositionPass<RayTracedReflectionPass>())
    {
        FindCompositionPass<RayTracedReflectionPass>()->SetEnabled(false);
        FindCompositionPass<RayTracedReflectionPass>()->SetRayTracingScene(m_rayTracingSceneManager.get());
    }
    if (FindCompositionPass<RayTracedReflectionDenoisePass>())
    {
        FindCompositionPass<RayTracedReflectionDenoisePass>()->SetEnabled(false);
        FindCompositionPass<RayTracedReflectionDenoisePass>()->SetReflectionSource(FindCompositionPass<RayTracedReflectionPass>());
    }
    if (FindCompositionPass<RayTracedReflectionCompositePass>())
    {
        FindCompositionPass<RayTracedReflectionCompositePass>()->SetEnabled(false);
        FindCompositionPass<RayTracedReflectionCompositePass>()->SetReflectionSource(FindCompositionPass<RayTracedReflectionPass>());
        FindCompositionPass<RayTracedReflectionCompositePass>()->SetDenoisedReflectionSource(FindCompositionPass<RayTracedReflectionDenoisePass>());
    }

    if (m_lightManager)
    {
        if (!m_lightingFramePlan || !m_localShadowFramePlan ||
            !m_localShadowAtlasPlan ||
            !m_lightManager->CollectLights(*m_lightingFramePlan))
        {
            m_lightingFramePlanFailureReason =
                "LightManager requires matching lighting and local-shadow plans";
        }
        else if (!m_lightManager->ApplyLocalShadowPlan(
                     *m_localShadowFramePlan, *m_localShadowAtlasPlan))
        {
            m_lightingFramePlanFailureReason =
                std::string("LightManager rejected the immutable local-shadow plan: ") +
                GetLocalShadowBindingCodeName(
                    m_lightManager->GetLocalShadowBindingCode());
            m_lightingFramePlan.reset();
        }
        else
        {
            const LightingGpuUploadResult uploadResult =
                m_lightManager->UpdateGPUBuffers();
            if (!uploadResult.Succeeded())
            {
                m_lightingFramePlanFailureReason =
                    std::string("LightManager GPU light upload failed: ") +
                    GetLightingGpuUploadCodeName(uploadResult.code);
                // An upload failure must not reach clustering, frozen graph
                // resources, or feature composition.  The next frame starts
                // from a fresh immutable plan and overwrites every buffer.
                m_lightingFramePlan.reset();
            }
        }
        m_localLightingStats.frameCount++;
        m_localLightingStats.pointLightRequestedCount =
            m_lightManager->GetPointLightRequestedCount();
        m_localLightingStats.pointLightAdmittedCount =
            m_lightManager->GetPointLightAdmittedCount();
        m_localLightingStats.pointLightCapacity =
            m_lightManager->GetPointLightCapacity();
        m_localLightingStats.pointLightOverflowCount =
            m_lightManager->GetPointLightOverflowCount();
        m_localLightingStats.spotLightRequestedCount =
            m_lightManager->GetSpotLightRequestedCount();
        m_localLightingStats.spotLightAdmittedCount =
            m_lightManager->GetSpotLightAdmittedCount();
        m_localLightingStats.spotLightCapacity =
            m_lightManager->GetSpotLightCapacity();
        m_localLightingStats.spotLightOverflowCount =
            m_lightManager->GetSpotLightOverflowCount();
        m_localLightingStats.pointLightCount = m_lightManager->GetPointLightCount();
        m_localLightingStats.spotLightCount = m_lightManager->GetSpotLightCount();
        m_localLightingStats.lightConstantsBufferReady = m_lightManager->GetLightConstantsBuffer() != nullptr;
        m_localLightingStats.pointLightsBufferReady = m_lightManager->GetPointLightsBuffer() != nullptr;
        m_localLightingStats.spotLightsBufferReady = m_lightManager->GetSpotLightsBuffer() != nullptr;
        m_localLightingStats.pointShadowRequestCount = m_lightManager->GetPointShadowRequestCount();
        m_localLightingStats.spotShadowRequestCount = m_lightManager->GetSpotShadowRequestCount();
        m_localLightingStats.localShadowRequestCount = m_lightManager->GetLocalShadowRequestCount();
        m_localLightingStats.localShadowCasterDrawCount = 0;
        const bool localShadowPlanReady =
            m_lightManager->HasLocalShadowPlan() &&
            m_localShadowFramePlan != nullptr &&
            m_localShadowAtlasPlan != nullptr;
        m_localLightingStats.localShadowAtlasReady = false;
        m_localLightingStats.localShadowFallbackReason =
            m_localLightingStats.localShadowRequestCount > 0 &&
                    !localShadowPlanReady
                ? std::string("local shadow planning failed: ") +
                    GetLocalShadowBindingCodeName(
                        m_lightManager->GetLocalShadowBindingCode())
                : "";
        m_localLightingStats.pointShadowSupported =
            m_localLightingStats.pointShadowRequestCount == 0;
        m_localLightingStats.pointShadowUnsupportedReason =
            m_localLightingStats.pointShadowRequestCount > 0
                ? "point-light shadow atlas execution is pending"
                : "";
        m_localLightingStats.spotShadowSupported =
            m_localLightingStats.spotShadowRequestCount == 0;
        m_localLightingStats.spotShadowUnsupportedReason =
            m_localLightingStats.spotShadowRequestCount > 0
                ? "spot-light shadow atlas execution is pending"
                : "";
    }

    const uint64 clusteredLightingFrameCount = m_clusteredLightingStats.frameCount + 1;
    m_clusteredLightingStats = {};
    m_clusteredLightingStats.frameCount = clusteredLightingFrameCount;
    if (!m_clusteredLighting)
    {
        m_clusteredLightingStats.fallbackReason = "clustered lighting system is not created";
    }
    else
    {
        m_clusteredLightingStats.initialized = m_clusteredLighting->IsInitialized();
        m_clusteredLightingStats.clusterAABBBufferReady = m_clusteredLighting->GetClusterAABBBuffer() != nullptr;
        m_clusteredLightingStats.clusterBufferReady = m_clusteredLighting->GetClusterBuffer() != nullptr;
        m_clusteredLightingStats.lightIndexBufferReady = m_clusteredLighting->GetLightIndexBuffer() != nullptr;
        m_clusteredLightingStats.clusterConstantsBufferReady =
            m_clusteredLighting->GetClusterConstantsBuffer() != nullptr;

        if (!m_clusteredLightingStats.initialized)
        {
            m_clusteredLightingStats.fallbackReason = m_clusteredLighting->GetLastError();
            if (m_clusteredLightingStats.fallbackReason.empty())
            {
                m_clusteredLightingStats.fallbackReason = "clustered lighting system is not initialized";
            }
        }
        else if (!m_lightManager)
        {
            m_clusteredLightingStats.fallbackReason = "frame light manager is not available";
        }
        else if (!m_lightingFramePlan || !m_lightManager->IsLightingPlanBound())
        {
            m_clusteredLightingStats.fallbackReason =
                m_lightingFramePlanFailureReason.empty()
                    ? "frame light plan is not GPU-published"
                    : m_lightingFramePlanFailureReason;
        }
        else
        {
            ClusteredLightingView clusteredView;
            clusteredView.viewMatrix = m_viewData.viewMatrix;
            clusteredView.projectionMatrix = m_viewData.projectionMatrix;
            clusteredView.inverseProjectionMatrix =
                m_viewData.inverseProjectionMatrix;
            clusteredView.nearPlane = m_viewData.nearPlane;
            clusteredView.farPlane = m_viewData.farPlane;
            clusteredView.screenWidth = m_viewData.viewportWidth;
            clusteredView.screenHeight = m_viewData.viewportHeight;
            // Core camera extraction is canonical [0, 1] NDC for DX12,
            // Vulkan, Metal, and the engine's OpenGL compatibility projection.
            clusteredView.ndcDepthRange = ClusterNdcDepthRange::ZeroToOne;
            clusteredView.reverseZ = m_pipelineCache &&
                m_pipelineCache->IsReverseZ();

            if (!m_clusteredLighting->BeginFrame(clusteredView))
            {
                m_clusteredLightingStats.fallbackReason = m_clusteredLighting->GetLastError();
            }
            else
            {
                m_clusteredLightingStats.frameBegun = true;
                const bool lightsAssigned =
                    m_clusteredLighting->AssignLights(*m_lightManager);
                const ClusteredLighting::Statistics clusteredStats =
                    m_clusteredLighting->GetStatistics();
                m_clusteredLightingStats.clusterCount = clusteredStats.clusterCount;
                m_clusteredLightingStats.lightIndexCount =
                    clusteredStats.lightIndexCount;
                m_clusteredLightingStats.activeClusters =
                    clusteredStats.activeClusters;
                m_clusteredLightingStats.totalLightAssignments =
                    clusteredStats.totalLightAssignments;
                m_clusteredLightingStats.maxLightsInCluster =
                    clusteredStats.maxLightsInCluster;
                m_clusteredLightingStats.overflowedClusterCount =
                    clusteredStats.overflowedClusterCount;
                m_clusteredLightingStats.overflowLightAssignmentCount =
                    clusteredStats.overflowLightAssignmentCount;
                m_clusteredLightingStats.avgLightsPerCluster =
                    clusteredStats.avgLightsPerCluster;
                if (!lightsAssigned)
                {
                    m_clusteredLightingStats.fallbackReason = m_clusteredLighting->GetLastError();
                }
                else
                {
                    m_clusteredLightingStats.lightsAssigned = true;
                    if (!m_clusteredLighting->UploadFrameData())
                    {
                        m_clusteredLightingStats.fallbackReason = m_clusteredLighting->GetLastError();
                    }
                    else
                    {
                        m_clusteredLightingStats.gpuBuffersUploaded = true;
                    }
                }
            }
        }
    }

}

void SceneRenderer::EnsureDepthBuffer(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0)
        return;

    // Check if we need to create or resize the depth buffer
    if (!m_depthTexture || m_depthWidth != width || m_depthHeight != height)
    {
        IRHIDevice* device = m_renderContext ? m_renderContext->GetDevice() : nullptr;
        if (!device)
            return;

        RVX_CORE_DEBUG("SceneRenderer: Creating depth buffer {}x{}", width, height);

        // Create depth texture
        RHITextureDesc depthDesc;
        depthDesc.width = width;
        depthDesc.height = height;
        depthDesc.depth = 1;
        depthDesc.mipLevels = 1;
        depthDesc.arraySize = 1;
        depthDesc.format = PipelineCache::GetDefaultDepthStencilFormat();
        depthDesc.dimension = RHITextureDimension::Texture2D;
        depthDesc.usage = RHITextureUsage::DepthStencil |
            RHITextureUsage::ShaderResource | RHITextureUsage::CopySrc;
        depthDesc.SetOptimizedClearDepthStencil({
            m_pipelineCache ? m_pipelineCache->GetDepthClearValue()
                            : PipelineCache::GetDepthClearValue(false),
            0});
        depthDesc.debugName = "SceneDepthBuffer";

        m_depthTexture = device->CreateTexture(depthDesc);
        if (!m_depthTexture)
        {
            RVX_CORE_ERROR("SceneRenderer: Failed to create depth buffer");
            return;
        }

        // Create depth texture view
        RHITextureViewDesc viewDesc;
        viewDesc.format = PipelineCache::GetDefaultDepthStencilFormat();
        viewDesc.dimension = RHITextureDimension::Texture2D;
        viewDesc.subresourceRange = RHISubresourceRange::All();
        viewDesc.subresourceRange.aspect = RHITextureAspect::Depth;
        viewDesc.type = RHITextureViewType::DepthStencil;
        viewDesc.debugName = "SceneDepthBufferView";

        m_depthTextureView = device->CreateTextureView(m_depthTexture.Get(), viewDesc);
        if (!m_depthTextureView)
        {
            RVX_CORE_ERROR("SceneRenderer: Failed to create depth buffer view");
            m_depthTexture.Reset();
            return;
        }

        m_depthWidth = width;
        m_depthHeight = height;
        m_depthAccessSnapshot = MakeRHITextureAccessSnapshot(
            RHIResourceState::Undefined,
            RHIShaderStage::None,
            GPUQueueDomain::Graphics,
            RHIContentValidity::Invalid);

        RVX_CORE_INFO("SceneRenderer: Created depth buffer {}x{}", width, height);
    }
}

void SceneRenderer::PrepareRayTracingScene()
{
    m_rayTracingSceneStats = {};
    m_rayTracingSceneRecordingToken = {};

    if (!m_rayTracingSceneManager || !m_renderResourceRegistry)
    {
        m_rayTracingSceneStats.fallbackReason = "ray tracing scene dependencies are unavailable";
        return;
    }

    if (!m_rayTracingSceneManager->IsSupported())
    {
        m_rayTracingSceneStats = m_rayTracingSceneManager->GetStats();
        m_rayTracingSceneStats.fallbackReason = "RHI device does not support ray tracing";
        return;
    }

    if (m_renderContext != nullptr)
    {
        if (RenderSubmissionTracker* const tracker =
                RenderContextInternalAccess::GetSubmissionTracker(
                    *m_renderContext))
        {
            (void)m_rayTracingSceneManager->PollCompletedBLASCompaction(
                *tracker);
        }
    }

    m_rayTracingSceneManager->SetBLASCacheEvictionFrameThreshold(
        m_rayTracingBudgetSettings.blasCacheEvictionFrameThreshold);
    m_rayTracingSceneManager->SetTrackedResourceBudget(
        m_rayTracingBudgetSettings.enabled ? m_rayTracingBudgetSettings.maxTrackedResourceBytes : 0u);

    RayTracingSceneOptions options;
    RayTracingSceneBuildPlan plan = BuildRayTracingSceneBuildPlan(
        m_renderScene,
        m_coarseCandidateObjectIndices,
        *m_renderResourceRegistry,
        options);

    m_rayTracingSceneManager->Prepare(plan);
    m_rayTracingSceneStats = m_rayTracingSceneManager->GetStats();
}

RayTracingSceneRecordInput SceneRenderer::AddRayTracingSceneBuildPass()
{
    RayTracingSceneRecordInput result;
    result.required = m_hybridRayTracingFramePlan.has_value() &&
        (m_hybridRayTracingFramePlan->GetShadowRoute() ==
             HybridRayTracingShadowRoute::RayTraced ||
         m_hybridRayTracingFramePlan->GetReflectionRoute() ==
             HybridRayTracingReflectionRoute::RayTraced);
    if (!m_renderGraph || !m_rayTracingSceneManager ||
        !m_rayTracingSceneStats.prepared)
    {
        return result;
    }

    RHIBuffer* instanceBuffer = m_rayTracingSceneManager->GetInstanceBuffer();
    RHIBuffer* tlasScratch = m_rayTracingSceneManager->GetTopLevelScratchBuffer();
    RHIAccelerationStructureRef topLevelAS(
        m_rayTracingSceneManager->GetTopLevelAS());
    RHIBuffer* const topLevelASStorage =
        topLevelAS ? topLevelAS->GetStorageBuffer() : nullptr;
    const RayTracingTLASStructuralSignature& structuralSignature =
        m_rayTracingSceneManager->GetPreparedTLASStructuralSignature();
    if (!instanceBuffer || !tlasScratch || !topLevelAS ||
        !topLevelASStorage ||
        !HasFlag(topLevelASStorage->GetUsage(),
                 RHIBufferUsage::AccelerationStructureStorage) ||
        !topLevelASStorage->GetResourceInstanceId().IsValid() ||
        m_rayTracingSceneStats.preparationGeneration == 0 ||
        structuralSignature.hash == 0)
    {
        return result;
    }

    std::vector<RHIBuffer*> blasScratchBuffers;
    m_rayTracingSceneManager->GatherPendingBuildScratchBuffers(blasScratchBuffers);

    std::vector<RGBufferHandle> blasScratchHandles;
    blasScratchHandles.reserve(blasScratchBuffers.size());
    for (RHIBuffer* scratchBuffer : blasScratchBuffers)
    {
        if (!scratchBuffer)
            continue;

        RGBufferHandle scratchBufferHandle =
            m_renderGraph->ImportBuffer(RHIBufferRef(scratchBuffer), RHIResourceState::Common);
        m_renderGraph->SetExportState(scratchBufferHandle, RHIResourceState::Common);
        blasScratchHandles.push_back(scratchBufferHandle);
    }

    struct RayTracingSceneBuildPassData
    {
        RayTracingSceneManager* manager = nullptr;
        RayTracingSceneRecordingToken* recordingToken = nullptr;
        RGBufferHandle instanceBuffer;
        std::vector<RGBufferHandle> blasScratchBuffers;
        RGBufferHandle tlasScratch;
        RHIAccelerationStructureRef topLevelAS;
        RHIResourceInstanceId topLevelASStorageId{};
        RGBufferHandle topLevelASStorage;
    };

    RGBufferHandle instanceHandle =
        m_renderGraph->ImportBuffer(RHIBufferRef(instanceBuffer), RHIResourceState::ShaderResource);
    RGBufferHandle scratchHandle =
        m_renderGraph->ImportBuffer(RHIBufferRef(tlasScratch), RHIResourceState::Common);
    const RHIBufferAccessSnapshot topLevelASStorageAccess =
        MakeRHIBufferAccessSnapshot(
            RHIResourceState::AccelerationStructureRead,
            RHIShaderStage::AllRayTracing);
    RGBufferHandle topLevelASStorageHandle = m_renderGraph->ImportBuffer(
        RHIBufferRef(topLevelASStorage),
        topLevelASStorageAccess);
    m_renderGraph->SetExportState(instanceHandle, RHIResourceState::ShaderResource);
    m_renderGraph->SetExportState(scratchHandle, RHIResourceState::Common);
    m_renderGraph->SetExportAccess(
        topLevelASStorageHandle,
        topLevelASStorageAccess.uniformAccess);

    result.topLevelAS = topLevelAS;
    result.storageResourceId = topLevelASStorage->GetResourceInstanceId();
    result.topLevelASStorage = topLevelASStorageHandle;
    result.preparationGeneration =
        m_rayTracingSceneStats.preparationGeneration;
    result.structuralHash = structuralSignature.hash;

    m_renderGraph->AddPass<RayTracingSceneBuildPassData>(
        "RayTracingSceneBuild",
        RenderGraphPassType::RayTracing,
        [this,
         instanceHandle,
         blasScratchHandles,
         scratchHandle,
         topLevelAS,
         topLevelASStorageHandle,
         topLevelASStorageId = result.storageResourceId](
            RenderGraphBuilder& builder,
            RayTracingSceneBuildPassData& data)
        {
            data.manager = m_rayTracingSceneManager.get();
            data.recordingToken = &m_rayTracingSceneRecordingToken;
            data.instanceBuffer = builder.Read(instanceHandle, RHIShaderStage::AllRayTracing);
            data.blasScratchBuffers.clear();
            data.blasScratchBuffers.reserve(blasScratchHandles.size());
            for (RGBufferHandle handle : blasScratchHandles)
            {
                data.blasScratchBuffers.push_back(builder.Write(handle, RHIResourceState::UnorderedAccess));
            }
            data.tlasScratch = builder.Write(scratchHandle, RHIResourceState::UnorderedAccess);
            data.topLevelAS = topLevelAS;
            data.topLevelASStorageId = topLevelASStorageId;
            data.topLevelASStorage = builder.Write(
                topLevelASStorageHandle,
                RHIResourceState::AccelerationStructureBuildWrite);
        },
        [](const RayTracingSceneBuildPassData& data,
           RenderGraphPassContext& context)
        {
            if (data.manager == nullptr || data.recordingToken == nullptr ||
                !data.topLevelAS || !data.topLevelASStorage.IsValid() ||
                data.manager->GetTopLevelAS() != data.topLevelAS.Get() ||
                data.topLevelAS->GetStorageBuffer() == nullptr ||
                data.topLevelAS->GetStorageBuffer()->GetResourceInstanceId() !=
                    data.topLevelASStorageId ||
                context.GetBuffer(data.topLevelASStorage) !=
                    data.topLevelAS->GetStorageBuffer() ||
                !data.manager->RecordBuildCommands(
                    context.Commands(),
                    *data.recordingToken,
                    RayTracingSceneRecordingFlags::EnableBLASCompaction))
            {
                context.MarkExecutionFailed(
                    "RayTracingSceneBuild did not produce a submission-bound recording token");
            }
        });
    return result;
}

void SceneRenderer::AddGPUDrivenCullingPass(
    const RenderPassRecordIdentity& recordIdentity)
{
    m_depthGPUCullingGraphHandles = {};
    m_opaqueGPUCullingGraphHandles = {};
    m_depthGPUCullingRecordedState.reset();
    m_opaqueGPUCullingRecordedState.reset();
    m_gpuDrivenTierExecutionTestProbe = {};
    m_gpuSceneCullingCommandRecordingFailed = false;
    m_gpuSceneRasterCommandRecordingFailed =
        std::make_shared<std::atomic_bool>(false);

    if (!m_renderGraph || !m_gpuDrivenCullingEnabled || !recordIdentity.IsValid())
    {
        return;
    }
    if (m_gpuDrivenFrameFailure)
    {
        return;
    }
    const RenderFrameExecutionPlan* const frozenPlan =
        m_viewData.renderFrameExecutionPlan;
    if (frozenPlan == nullptr)
    {
        MarkGPUDrivenFrameFailure(
            GPUDrivenFrameFailureStage::TierConfirmationMissingTier1);
        return;
    }

    struct GPUDrivenCullPassData
    {
        RGBufferHandle constants;
        RGBufferHandle instances;
        RGBufferHandle gpuSceneCandidates;
        RGBufferHandle instanceIndices;
        RGBufferHandle visibility;
        RGBufferHandle visibleInstances;
        RGBufferHandle indirectDraws;
        RGBufferHandle drawCount;
        std::array<RGBufferHandle, GPU_SCENE_RESIDENT_TABLE_COUNT> gpuSceneTables;
        bool usesGPUScene = false;
    };

    GPUQueueDomain graphicsDomain = GPUQueueDomain::Graphics;
    IRHIDevice* device = m_renderContext ? m_renderContext->GetDevice() : nullptr;
    const RHIQueueTopology queueTopology = device
        ? device->GetCapabilities().queueTopology
        : RHIQueueTopology{};
    TryGetGPUQueueDomain(
        queueTopology,
        RHICommandQueueType::Graphics,
        graphicsDomain);
    // SceneRenderer currently submits RenderGraph through Execute(graphicsCtx),
    // so compute shader execution still belongs to the graphics physical domain.
    const GPUQueueDomain computeExecutionDomain = graphicsDomain;
    const Mat4 cullViewMatrix = m_viewData.viewMatrix;
    const Mat4 cullProjectionMatrix = m_viewData.projectionMatrix;
    SceneGPUDrivenCullingStats* const cullingStatsSink =
        &m_gpuDrivenCullingStats;
    bool* const gpuSceneCullingFailureSink =
        &m_gpuSceneCullingCommandRecordingFailed;

    const bool requestedGPUSceneTier =
        frozenPlan->viewPolicy.selectedTier == GPUDrivenTier::GPUResidentScene;
    const bool useGPUSceneTier = requestedGPUSceneTier &&
        m_confirmedGPUDrivenTier == GPUDrivenTier::GPUResidentScene;
    if (requestedGPUSceneTier && !useGPUSceneTier &&
        m_confirmedGPUDrivenTier != GPUDrivenTier::IndirectGrouped)
    {
        MarkGPUDrivenFrameFailure(
            GPUDrivenFrameFailureStage::TierConfirmationNoFallback);
        return;
    }
    const auto laneIsPlanned = [frozenPlan](RenderPassKind pass)
    {
        for (const RenderPassExecutionPlan& passPlan : frozenPlan->passes)
        {
            if (passPlan.pass == pass)
            {
                return passPlan.gpuEligiblePackets.count != 0;
            }
        }
        return false;
    };
    const bool depthLanePlanned = laneIsPlanned(RenderPassKind::Depth);
    const bool opaqueLanePlanned = laneIsPlanned(RenderPassKind::Opaque);
    struct GPUScenePassPreflight
    {
        GPUCulling* owner = nullptr;
        std::shared_ptr<GPUCullingRecordedState> recordedState;
        std::shared_ptr<const GPUSceneRasterBindingSnapshot> binding;
        uint64 leaseVersion = 0;
    };
    std::optional<GPUSceneResidentGraphLease> gpuSceneLease;
    const uint64 requiredResidentVersion =
        frozenPlan->viewPolicy.requiredResidentVersion;
    const auto canUseGPUSceneLease = [requiredResidentVersion](
        const GPUCulling* owner, bool framePrepared)
    {
        return framePrepared && owner != nullptr &&
               owner->GetInstanceCount() != 0 &&
               owner->IsGPUSceneExecutionReady() &&
               owner->HasCompleteGPUSceneCandidates(requiredResidentVersion);
    };
    GPUScenePassPreflight depthGPUScenePreflight;
    GPUScenePassPreflight opaqueGPUScenePreflight;
    if (useGPUSceneTier)
    {
        if (!m_gpuSceneUploader || !m_pipelineCache || !m_submissionBatch ||
            requiredResidentVersion == 0 ||
            (depthLanePlanned && !canUseGPUSceneLease(
                m_depthGPUCulling.get(), m_depthGPUCullingFramePrepared)) ||
            (opaqueLanePlanned && !canUseGPUSceneLease(
                m_opaqueGPUCulling.get(), m_opaqueGPUCullingFramePrepared)))
        {
            MarkGPUDrivenFrameFailure(
                GPUDrivenFrameFailureStage::TierConfirmationResidentNotReady);
            return;
        }
        if (m_gpuDrivenGraphFailureInjection ==
            GPUDrivenGraphFailureInjection::Acquire)
        {
            MarkGPUDrivenFrameFailure(
                GPUDrivenFrameFailureStage::GPUSceneLeaseAcquire);
            return;
        }
        ++m_gpuDrivenTierExecutionTestProbe.gpuSceneLeaseAcquireAttempts;
        gpuSceneLease = m_gpuSceneUploader->AcquireCurrentGraphLease(
            *m_renderGraph, m_submissionBatch.get(), requiredResidentVersion);
        if (!gpuSceneLease)
        {
            MarkGPUDrivenFrameFailure(
                GPUDrivenFrameFailureStage::GPUSceneLeaseAcquire);
            return;
        }
        m_renderPolicyDiagnostics.gpuSceneLeaseVersion =
            gpuSceneLease->version;
        // Acquisition is the authoritative exact-version observation for
        // this frame.  Do not retain either value across CompileRenderFramePlan.
        m_renderPolicyDiagnostics.gpuSceneResidentVersion =
            gpuSceneLease->version;
        const GPUCullingRecordingIdentity cullingIdentity{
            recordIdentity.graphIdentity,
            recordIdentity.graphRecordingGeneration,
            recordIdentity.frameSequence,
            recordIdentity.viewOrdinal,
            recordIdentity.recordEpoch};
        GPUDrivenFrameFailureStage preflightFailureStage =
            GPUDrivenFrameFailureStage::GPUSceneLeaseSeal;
        const auto preflightGPUScenePass =
            [this,
             &gpuSceneLease,
             &cullingIdentity,
             &preflightFailureStage](GPUCulling* owner,
                                      GPUScenePassPreflight& out)
        {
            if (owner == nullptr || !gpuSceneLease)
            {
                preflightFailureStage =
                    GPUDrivenFrameFailureStage::GPUSceneLeaseAcquire;
                return false;
            }
            if (m_gpuDrivenGraphFailureInjection ==
                GPUDrivenGraphFailureInjection::Seal)
            {
                preflightFailureStage =
                    GPUDrivenFrameFailureStage::GPUSceneLeaseSeal;
                return false;
            }
            ++m_gpuDrivenTierExecutionTestProbe.gpuSceneSealAttempts;
            owner->PublishGPUSceneQualificationRequiredLane(
                GPUDrivenTier::GPUResidentScene);
            std::shared_ptr<GPUCullingRecordedState> recordedState =
                owner->SealForGPUSceneGraph(cullingIdentity, *gpuSceneLease);
            if (!recordedState || !recordedState->IsValid())
            {
                preflightFailureStage =
                    GPUDrivenFrameFailureStage::GPUSceneLeaseSeal;
                return false;
            }
            const GPUSceneRasterResourceSnapshot rasterResources =
                recordedState->GetGPUSceneRasterResourceSnapshot();
            ObjectConstants objectConstants{};
            objectConstants.world = Mat4Identity();
            objectConstants.normalMatrix = Mat4Identity();
            objectConstants.previousWorldViewProjection = Mat4Identity();
            objectConstants.objectVelocityParams = Vec4(0.0f);
            objectConstants.skinningParams = Vec4(0.0f);
            GPUSceneRasterBindingSnapshot binding;
            if (m_gpuDrivenGraphFailureInjection ==
                    GPUDrivenGraphFailureInjection::Binding ||
                !rasterResources.IsValid() ||
                rasterResources.GetLeaseVersion() != gpuSceneLease->version ||
                !m_pipelineCache->CreateGPUSceneRasterBindingSnapshot(
                    rasterResources, objectConstants, binding) ||
                !binding.IsReadyForBinding() ||
                binding.leaseVersion != gpuSceneLease->version ||
                binding.candidateBuffer.Get() != rasterResources.GetCandidates() ||
                binding.primitiveBuffer.Get() != rasterResources.GetPrimitives() ||
                binding.transformBuffer.Get() != rasterResources.GetTransforms())
            {
                preflightFailureStage =
                    GPUDrivenFrameFailureStage::GPUSceneRasterBinding;
                return false;
            }
            std::shared_ptr<const GPUSceneRasterBindingSnapshot> retainedBinding =
                std::make_shared<GPUSceneRasterBindingSnapshot>(std::move(binding));
            if (!retainedBinding->RetainSubmissionResources(*m_submissionBatch))
            {
                preflightFailureStage =
                    GPUDrivenFrameFailureStage::GPUSceneRasterBinding;
                return false;
            }
            out.owner = owner;
            out.recordedState = std::move(recordedState);
            out.binding = std::move(retainedBinding);
            out.leaseVersion = gpuSceneLease->version;
            return true;
        };
        if ((depthLanePlanned && !preflightGPUScenePass(
                 m_depthGPUCulling.get(), depthGPUScenePreflight)) ||
            (opaqueLanePlanned && !preflightGPUScenePass(
                 m_opaqueGPUCulling.get(), opaqueGPUScenePreflight)))
        {
            RVX_VERIFY(m_gpuSceneUploader->CancelCurrentGraphLease(),
                       "SceneRenderer: failed to cancel GPU-scene preflight lease");
            MarkGPUDrivenFrameFailure(preflightFailureStage);
            return;
        }
    }

    const auto addPass =
        [this,
         graphicsDomain,
         computeExecutionDomain,
         cullViewMatrix,
         cullProjectionMatrix,
         cullingStatsSink,
         gpuSceneCullingFailureSink,
         recordIdentity,
         &gpuSceneLease](
            const char* name,
            GPUCulling* owner,
            bool framePrepared,
            const GPUScenePassPreflight* gpuScenePreflight,
            GPUCullingGraphHandles& outHandles,
            std::shared_ptr<GPUCullingRecordedState>& outRecordedState,
            bool& outGPUSceneRegistered) -> bool
    {
        outGPUSceneRegistered = false;
        if (!framePrepared || owner == nullptr ||
            owner->GetInstanceCount() == 0)
        {
            return false;
        }

        const bool usesGPUScene = gpuScenePreflight != nullptr;
        if (owner == m_depthGPUCulling.get())
        {
            m_gpuDrivenTierExecutionTestProbe.depthActualTier = usesGPUScene
                ? GPUDrivenTier::GPUResidentScene
                : GPUDrivenTier::IndirectGrouped;
            m_gpuDrivenTierExecutionTestProbe.depthGPUSceneLeaseVersion =
                usesGPUScene ? gpuScenePreflight->leaseVersion : 0;
        }
        else if (owner == m_opaqueGPUCulling.get())
        {
            m_gpuDrivenTierExecutionTestProbe.opaqueActualTier = usesGPUScene
                ? GPUDrivenTier::GPUResidentScene
                : GPUDrivenTier::IndirectGrouped;
            m_gpuDrivenTierExecutionTestProbe.opaqueGPUSceneLeaseVersion =
                usesGPUScene ? gpuScenePreflight->leaseVersion : 0;
        }
        std::shared_ptr<GPUCullingRecordedState> recordedState;
        std::shared_ptr<const GPUSceneRasterBindingSnapshot> gpuSceneBinding;
        uint64 gpuSceneLeaseVersion = 0;
        if (usesGPUScene)
        {
            if (!gpuSceneLease || gpuScenePreflight->owner != owner ||
                !gpuScenePreflight->recordedState ||
                !gpuScenePreflight->binding)
            {
                MarkGPUDrivenFrameFailure(
                    GPUDrivenFrameFailureStage::GPUSceneLeaseSeal);
                return false;
            }
            recordedState = gpuScenePreflight->recordedState;
            gpuSceneBinding = gpuScenePreflight->binding;
            gpuSceneLeaseVersion = gpuScenePreflight->leaseVersion;
        }
        else
        {
            const GPUCullingRecordingIdentity cullingIdentity{
                recordIdentity.graphIdentity,
                recordIdentity.graphRecordingGeneration,
                recordIdentity.frameSequence,
                recordIdentity.viewOrdinal,
                recordIdentity.recordEpoch};
            ++m_gpuDrivenTierExecutionTestProbe.tierOneSealAttempts;
            owner->PublishGPUSceneQualificationRequiredLane(
                GPUDrivenTier::IndirectGrouped);
            recordedState = owner->SealForGraph(cullingIdentity);
        }
        if (!recordedState || !recordedState->IsValid())
        {
            RVX_RENDER_WARN("SceneRenderer: failed to seal {} GPU culling inputs", name);
            MarkGPUDrivenFrameFailure(
                usesGPUScene
                    ? GPUDrivenFrameFailureStage::GPUSceneLeaseSeal
                    : GPUDrivenFrameFailureStage::Tier1Seal);
            return false;
        }

        const GPUCulling& recordedCulling = recordedState->GetCulling();
        if (!usesGPUScene &&
            recordedCulling.GetExecutionDecision().mode !=
                GPUCullingExecutionMode::GpuCompute)
        {
            // Graph records a slim GPU-only seal. A normal Tier 1 CPU
            // fallback would need the deliberately unsealed dense payload,
            // so reject the lane before any graph mutation instead.
            RVX_RENDER_WARN(
                "SceneRenderer: {} GPU culling lost compute execution before graph registration",
                name);
            MarkGPUDrivenFrameFailure(
                GPUDrivenFrameFailureStage::Tier1Seal);
            return false;
        }

        RHIBuffer* constantsBuffer = recordedCulling.GetCullingConstantsBuffer();
        RHIBuffer* instanceBuffer = usesGPUScene
            ? nullptr
            : recordedCulling.GetInstanceBuffer();
        RHIBuffer* gpuSceneCandidateBuffer = usesGPUScene
            ? recordedCulling.GetGPUSceneCandidateBuffer()
            : nullptr;
        RHIBuffer* instanceIndexBuffer = recordedCulling.GetInstanceIndexBuffer();
        RHIBuffer* visibilityBuffer = recordedCulling.GetVisibilityBuffer();
        RHIBuffer* visibleInstanceBuffer = recordedCulling.GetVisibleInstanceBuffer();
        RHIBuffer* indirectDrawBuffer = recordedCulling.GetIndirectBuffer();
        RHIBuffer* drawCountBuffer = recordedCulling.GetDrawCountBuffer();
        if (!constantsBuffer || !instanceIndexBuffer ||
            !visibilityBuffer || !visibleInstanceBuffer ||
            !indirectDrawBuffer || !drawCountBuffer ||
            (!usesGPUScene && !instanceBuffer) ||
            (usesGPUScene && !gpuSceneCandidateBuffer))
        {
            MarkGPUDrivenFrameFailure(
                GPUDrivenFrameFailureStage::PassRegistration);
            return false;
        }

        const GPUCullingAccessSnapshots& accessSnapshots =
            recordedState->GetAccessSnapshots();
        GPUDrivenCullPassData handles;
        handles.constants = m_renderGraph->ImportBuffer(
            RHIBufferRef(constantsBuffer), accessSnapshots.constants);
        if (usesGPUScene)
        {
            handles.gpuSceneCandidates = m_renderGraph->ImportBuffer(
                RHIBufferRef(gpuSceneCandidateBuffer), accessSnapshots.gpuSceneCandidates);
            handles.gpuSceneTables = gpuSceneLease->handles;
            handles.usesGPUScene = true;
        }
        else
        {
            handles.instances = m_renderGraph->ImportBuffer(
                RHIBufferRef(instanceBuffer), accessSnapshots.instances);
        }
        handles.instanceIndices = m_renderGraph->ImportBuffer(
            RHIBufferRef(instanceIndexBuffer), accessSnapshots.instanceIndices);
        handles.visibility = m_renderGraph->ImportBuffer(
            RHIBufferRef(visibilityBuffer), accessSnapshots.visibility);
        handles.visibleInstances = m_renderGraph->ImportBuffer(
            RHIBufferRef(visibleInstanceBuffer), accessSnapshots.visibleInstances);
        handles.indirectDraws = m_renderGraph->ImportBuffer(
            RHIBufferRef(indirectDrawBuffer), accessSnapshots.indirectDraws);
        handles.drawCount = m_renderGraph->ImportBuffer(
            RHIBufferRef(drawCountBuffer), accessSnapshots.drawCount);

        m_renderGraph->SetExportAccess(
            handles.constants,
            MakeRHIAccessSnapshot(RHIResourceState::ConstantBuffer,
                                  RHIShaderStage::Compute,
                                  computeExecutionDomain));
        if (!usesGPUScene)
        {
            m_renderGraph->SetExportAccess(
                handles.instances,
                MakeRHIAccessSnapshot(RHIResourceState::ShaderResource,
                                      RHIShaderStage::AllGraphics,
                                      graphicsDomain));
        }
        if (usesGPUScene)
        {
            m_renderGraph->SetExportAccess(
                handles.gpuSceneCandidates,
                MakeRHIAccessSnapshot(RHIResourceState::ShaderResource,
                                      RHIShaderStage::Vertex,
                                      graphicsDomain));
        }
        m_renderGraph->SetExportAccess(
            handles.instanceIndices,
            MakeRHIAccessSnapshot(RHIResourceState::ShaderResource,
                                  RHIShaderStage::Compute,
                                  computeExecutionDomain));
        m_renderGraph->SetExportAccess(
            handles.visibility,
            MakeRHIAccessSnapshot(RHIResourceState::UnorderedAccess,
                                  RHIShaderStage::Compute,
                                  computeExecutionDomain));
        m_renderGraph->SetExportAccess(
            handles.visibleInstances,
            MakeRHIAccessSnapshot(RHIResourceState::VertexBuffer,
                                  RHIShaderStage::Vertex,
                                  graphicsDomain));
        m_renderGraph->SetExportAccess(
            handles.indirectDraws,
            MakeRHIAccessSnapshot(RHIResourceState::IndirectArgument,
                                  RHIShaderStage::None,
                                  graphicsDomain));
        m_renderGraph->SetExportAccess(
            handles.drawCount,
            MakeRHIAccessSnapshot(RHIResourceState::IndirectArgument,
                                  RHIShaderStage::None,
                                  graphicsDomain));
        outHandles = {};
        outHandles.constants = handles.constants;
        outHandles.instances = handles.instances;
        outHandles.gpuSceneCandidates = handles.gpuSceneCandidates;
        if (usesGPUScene)
        {
            constexpr uint32 primitiveTableIndex =
                static_cast<uint32>(GPUSceneResidentTable::Primitives);
            constexpr uint32 transformTableIndex =
                static_cast<uint32>(GPUSceneResidentTable::Transforms);
            outHandles.gpuScenePrimitives = handles.gpuSceneTables[primitiveTableIndex];
            outHandles.gpuSceneTransforms = handles.gpuSceneTables[transformTableIndex];
            outHandles.gpuSceneRasterBinding = std::move(gpuSceneBinding);
            outHandles.gpuSceneLeaseVersion = gpuSceneLeaseVersion;
            outHandles.gpuSceneRasterEnabled = true;
        }
        outHandles.instanceIndices = handles.instanceIndices;
        outHandles.visibility = handles.visibility;
        outHandles.visibleInstances = handles.visibleInstances;
        outHandles.indirectDraws = handles.indirectDraws;
        outHandles.drawCount = handles.drawCount;
        outRecordedState = recordedState;
        outGPUSceneRegistered = usesGPUScene;

        if (usesGPUScene &&
            m_gpuDrivenGraphFailureInjection ==
                GPUDrivenGraphFailureInjection::PassRegistration)
        {
            MarkGPUDrivenFrameFailure(
                GPUDrivenFrameFailureStage::PassRegistration);
            return false;
        }
        ++m_gpuDrivenTierExecutionTestProbe.graphPassRegistrationAttempts;
        m_renderGraph->AddPass<GPUDrivenCullPassData>(
            name,
            RenderGraphPassType::Compute,
            [handles, computeExecutionDomain](RenderGraphBuilder& builder,
                                               GPUDrivenCullPassData& data)
            {
                data = handles;
                data.constants = builder.Read(
                    data.constants,
                    MakeRHIAccessSnapshot(RHIResourceState::ConstantBuffer,
                                          RHIShaderStage::Compute,
                                          computeExecutionDomain));
                if (data.usesGPUScene)
                {
                    data.gpuSceneCandidates = builder.Read(
                        data.gpuSceneCandidates,
                        MakeRHIAccessSnapshot(RHIResourceState::ShaderResource,
                                              RHIShaderStage::Compute,
                                              computeExecutionDomain));
                    for (uint32 tableIndex = 0;
                         tableIndex < GPU_SCENE_RESIDENT_TABLE_COUNT;
                         ++tableIndex)
                    {
                        data.gpuSceneTables[tableIndex] = builder.Read(
                            data.gpuSceneTables[tableIndex],
                            MakeRHIAccessSnapshot(
                                RHIResourceState::ShaderResource,
                                RHIShaderStage::Compute,
                                computeExecutionDomain));
                    }
                }
                else
                {
                    data.instances = builder.Read(
                        data.instances,
                        MakeRHIAccessSnapshot(RHIResourceState::ShaderResource,
                                              RHIShaderStage::Compute,
                                              computeExecutionDomain));
                }
                data.instanceIndices = builder.Read(
                    data.instanceIndices,
                    MakeRHIAccessSnapshot(RHIResourceState::ShaderResource,
                                          RHIShaderStage::Compute,
                                          computeExecutionDomain));
                const RHIAccessSnapshot unorderedAccess = MakeRHIAccessSnapshot(
                    RHIResourceState::UnorderedAccess,
                    RHIShaderStage::Compute,
                    computeExecutionDomain);
                data.visibility = builder.Write(data.visibility, unorderedAccess);
                data.visibleInstances = builder.Write(
                    data.visibleInstances, unorderedAccess);
                data.indirectDraws = builder.Write(data.indirectDraws, unorderedAccess);
                data.drawCount = builder.Write(data.drawCount, unorderedAccess);
            },
            [recordedState,
             cullViewMatrix,
             cullProjectionMatrix,
             cullingStatsSink,
             gpuSceneCullingFailureSink,
             name,
             usesGPUScene](const GPUDrivenCullPassData&, RHICommandContext& ctx)
            {
                if (usesGPUScene)
                {
                    if (!recordedState->CullGPUScene(
                            ctx, cullViewMatrix, cullProjectionMatrix))
                    {
                        *gpuSceneCullingFailureSink = true;
                        RVX_RENDER_ERROR(
                            "SceneRenderer: {} GPU-scene culling command recording failed",
                            name);
                    }
                }
                else
                {
                    recordedState->Cull(ctx, cullViewMatrix, cullProjectionMatrix);
                }
                cullingStatsSink->graphPassRecorded = true;
                cullingStatsSink->executionDecisionAvailable = true;
                cullingStatsSink->executionDecision =
                    recordedState->GetCulling().GetExecutionDecision();
                cullingStatsSink->gpuExecutionRecorded =
                    cullingStatsSink->gpuExecutionRecorded ||
                    recordedState->GetCulling().WasGpuExecutionUsedLastCull();
                cullingStatsSink->fallbackUsed =
                    cullingStatsSink->fallbackUsed ||
                    recordedState->GetCulling().WasCpuFallbackUsedLastCull();
            });
        return true;
    };

    bool depthGPUSceneRegistered = false;
    const bool depthAdded = addPass("GPUDrivenDepthCull",
                                    m_depthGPUCulling.get(),
                                    m_depthGPUCullingFramePrepared,
                                    useGPUSceneTier && depthLanePlanned
                                        ? &depthGPUScenePreflight
                                        : nullptr,
                                    m_depthGPUCullingGraphHandles,
                                    m_depthGPUCullingRecordedState,
                                    depthGPUSceneRegistered);
    bool opaqueGPUSceneRegistered = false;
    const bool opaqueAdded = addPass("GPUDrivenOpaqueCull",
                                     m_opaqueGPUCulling.get(),
                                     m_opaqueGPUCullingFramePrepared,
                                     useGPUSceneTier && opaqueLanePlanned
                                         ? &opaqueGPUScenePreflight
                                         : nullptr,
                                     m_opaqueGPUCullingGraphHandles,
                                     m_opaqueGPUCullingRecordedState,
                                     opaqueGPUSceneRegistered);
    if ((depthLanePlanned && !depthAdded) ||
        (opaqueLanePlanned && !opaqueAdded))
    {
        if (gpuSceneLease && !depthAdded && !opaqueAdded &&
            m_gpuSceneUploader)
        {
            RVX_VERIFY(m_gpuSceneUploader->CancelCurrentGraphLease(),
                       "SceneRenderer: failed to cancel GPU-scene lease after pass registration failure");
        }
        MarkGPUDrivenFrameFailure(
            GPUDrivenFrameFailureStage::PassRegistration);
        return;
    }
    m_gpuDrivenCullingStats.graphPassAdded = depthAdded || opaqueAdded;
    m_gpuDrivenCullingStats.gpuCullingGraphPassCount =
        (depthAdded ? 1u : 0u) + (opaqueAdded ? 1u : 0u);
}

void SceneRenderer::CommitGPUDrivenAccessSnapshots()
{
    if (!m_renderGraph)
    {
        return;
    }
    const auto commit = [this](GPUCulling* owner,
                               const std::shared_ptr<GPUCullingRecordedState>& recordedState,
                               const GPUCullingGraphHandles& handles)
    {
        if (!recordedState || !handles.IsValid())
        {
            return;
        }
        GPUCullingAccessSnapshots snapshots;
        snapshots.constants = m_renderGraph->GetRealizedAccess(handles.constants);
        snapshots.instances = handles.instances.IsValid()
            ? m_renderGraph->GetRealizedAccess(handles.instances)
            : recordedState->GetAccessSnapshots().instances;
        snapshots.gpuSceneCandidates = handles.gpuSceneCandidates.IsValid()
            ? m_renderGraph->GetRealizedAccess(handles.gpuSceneCandidates)
            : recordedState->GetAccessSnapshots().gpuSceneCandidates;
        snapshots.instanceIndices = m_renderGraph->GetRealizedAccess(handles.instanceIndices);
        snapshots.visibility = m_renderGraph->GetRealizedAccess(handles.visibility);
        snapshots.visibleInstances = m_renderGraph->GetRealizedAccess(handles.visibleInstances);
        snapshots.indirectDraws = m_renderGraph->GetRealizedAccess(handles.indirectDraws);
        snapshots.drawCount = m_renderGraph->GetRealizedAccess(handles.drawCount);
        recordedState->CommitAccessSnapshots(snapshots);
        if (owner == nullptr ||
            !owner->CommitFrameSlotAccessSnapshots(
                recordedState->GetSourceFrameSlot(), snapshots))
        {
            RVX_RENDER_ERROR(
                "SceneRenderer: failed to publish GPU-culling access to source frame slot {}",
                recordedState->GetSourceFrameSlot());
            MarkGPUDrivenFrameFailure(
                GPUDrivenFrameFailureStage::AccessSnapshotCommit);
        }
    };
    commit(m_depthGPUCulling.get(),
           m_depthGPUCullingRecordedState,
           m_depthGPUCullingGraphHandles);
    commit(m_opaqueGPUCulling.get(),
           m_opaqueGPUCullingRecordedState,
           m_opaqueGPUCullingGraphHandles);
}

namespace
{
    /**
     * @brief Creates a deterministic first writer for a transient color target.
     *
     * Transient scene inputs must never rely on allocation contents. Scene
     * color needs a stable value when Opaque fails closed, while the temporal
     * reactive mask uses zero as the identity until a later feature writes
     * unstable pixels.
     */
    void AddColorInitializationPass(RenderGraph& graph,
                                    RGTextureHandle colorTarget,
                                    RHIClearColor clearColor,
                                    const char* passName,
                                    const char* viewName)
    {
        struct ColorInitializationPassData
        {
            RGTextureHandle colorTarget{};
            RGTextureViewHandle colorTargetView{};
        };

        graph.AddPass<ColorInitializationPassData>(
            passName,
            RenderGraphPassType::Graphics,
            [colorTarget, viewName](RenderGraphBuilder& builder,
                                    ColorInitializationPassData& data)
            {
                if (!colorTarget.IsValid())
                {
                    return;
                }

                const RHITextureDesc* const colorDesc =
                    builder.GetTextureDesc(colorTarget);
                if (colorDesc == nullptr)
                {
                    return;
                }

                RHITextureViewDesc colorViewDesc;
                colorViewDesc.format = colorDesc->format;
                colorViewDesc.dimension = colorDesc->dimension;
                colorViewDesc.subresourceRange = RHISubresourceRange::All();
                colorViewDesc.type = RHITextureViewType::RenderTarget;
                colorViewDesc.debugName = viewName;

                data.colorTarget = colorTarget;
                data.colorTargetView = builder.CreateTextureView(
                    colorTarget, colorViewDesc);
                data.colorTargetView = builder.Write(
                    data.colorTargetView,
                    MakeRGAccessDesc(
                        RHIResourceState::RenderTarget,
                        RHIShaderStage::None));
                if (!data.colorTargetView.IsValid())
                {
                    data.colorTarget = {};
                }
            },
            [clearColor](const ColorInitializationPassData& data,
                         RenderGraphPassContext& context)
            {
                if (!data.colorTarget.IsValid() ||
                    !data.colorTargetView.IsValid())
                {
                    context.MarkExecutionFailed(
                        "Color initialization pass has invalid graph resources");
                    return;
                }

                RHITexture* const colorTexture =
                    context.GetTexture(data.colorTarget);
                RHITextureView* const colorView =
                    context.GetTextureView(data.colorTargetView);
                if (colorTexture == nullptr || colorView == nullptr)
                {
                    context.MarkExecutionFailed(
                        "Color initialization pass could not resolve graph resources");
                    return;
                }

                RHIRenderPassDesc renderPassDesc;
                renderPassDesc.AddColorAttachment(
                    colorView,
                    RHILoadOp::Clear,
                    RHIStoreOp::Store,
                    clearColor);
                renderPassDesc.SetRenderArea(
                    0, 0, colorTexture->GetWidth(), colorTexture->GetHeight());
                RHICommandContext& commands = context.Commands();
                commands.BeginRenderPass(renderPassDesc);
                commands.EndRenderPass();
            });
    }

    void AddSceneColorInitializationPass(RenderGraph& graph,
                                         RGTextureHandle sceneColorTarget,
                                         RHIClearColor clearColor)
    {
        AddColorInitializationPass(
            graph,
            sceneColorTarget,
            clearColor,
            "SceneColorInitializePass",
            "SceneColorInitializeRTV");
    }

    void AddReactiveMaskInitializationPass(
        RenderGraph& graph,
        RGTextureHandle reactiveMaskTarget)
    {
        AddColorInitializationPass(
            graph,
            reactiveMaskTarget,
            RHIClearColor{0.0f, 0.0f, 0.0f, 0.0f},
            "ReactiveMaskInitializePass",
            "ReactiveMaskInitializeRTV");
    }
} // namespace

void SceneRenderer::UpdateEnvironmentIBLBindingDiagnostics(
    bool requested,
    bool textureIBLEnabled,
    std::string fallbackReason)
{
    m_environmentIBLStats.requested = requested;
    m_environmentIBLStats.textureIBLEnabled =
        requested && textureIBLEnabled;
    if (!requested || m_environmentIBLStats.textureIBLEnabled)
    {
        m_environmentIBLStats.fallbackReason.clear();
        return;
    }

    m_environmentIBLStats.fallbackReason = fallbackReason.empty()
        ? "PacketEnvironmentResourcesUnavailable"
        : std::move(fallbackReason);
}

void SceneRenderer::BuildRenderGraph()
{
    // Freeze first-use queue ownership and compilation policy together.
    m_recordedGraphQueuePolicy = m_qualityPerformanceSession.RequiresExactGraphicsFrame()
        ? RGQueuePolicy::GraphicsOnly : RGQueuePolicy::PreferMultiQueue;
    m_featureCompositionFailureReason.clear();
    if (!m_lightingFramePlan || !m_lightingFramePlan->IsValid())
    {
        m_featureCompositionFailureReason =
            m_lightingFramePlanFailureReason.empty()
                ? "Renderer has no valid LightingFramePlan for this frame/view"
                : m_lightingFramePlanFailureReason;
        return;
    }
    // RenderContext's logical frame slot may advance independently from the
    // physical RHI device slot used by the Direct resident streams. Freeze the
    // latter before graph recording and retain it through submission closure.
    IRHIDevice* const directReadbackDevice = m_renderContext
        ? m_renderContext->GetDevice() : nullptr;
    m_directOpaqueRasterReadbackSourceFrameSlot = directReadbackDevice
        ? directReadbackDevice->GetCurrentFrameIndex() : RVX_INVALID_INDEX;

    // Store RenderGraph and ViewCache pointers in ViewData so passes can access resources
    m_viewData.velocityTarget = {};
    m_viewData.reactiveMaskTarget = {};
    m_viewData.environmentSkyTexture = {};
    m_viewData.environmentIrradianceTexture = {};
    m_viewData.environmentPrefilteredTexture = {};
    m_viewData.environmentBRDFLUTTexture = {};
    m_viewData.exposureState = {};
    m_viewData.exposureStateSchemaVersion = 0;
    m_viewData.exposureStateStride = 0;
    m_viewData.exposureStateMode = RenderExposureMode::ManualEV100;
    m_viewData.exposureStateConfigurationHash = 0;
    m_viewData.exposureStateContentHash = 0;
    m_depthGraphHandle = {};
    m_backBufferGraphHandle = {};
    m_activeBackBufferIndex = RVX_INVALID_INDEX;
    m_frameExternalTextureAccesses.clear();
    const uint64 postProcessFrameCount = m_postProcessStats.frameCount + 1;
    m_postProcessStats = {};
    m_postProcessStats.frameCount = postProcessFrameCount;
    m_externalRenderTargetStats = {};
    m_externalRenderTargetStats.requested =
        m_externalRenderTarget.colorTarget != nullptr || m_externalRenderTarget.depthTarget != nullptr;

    EnvironmentIBLFrameResources environmentFrameResources;
    RHITextureViewRef irradianceView;
    RHITextureViewRef prefilteredView;
    RHITextureViewRef brdfLUTView;
    if (m_renderResourceRegistry && m_resourceViewCache && m_pipelineCache)
    {
        const RenderSkySnapshot& sky = m_renderScene.GetSky();
        const RenderTextureResourceData* skyTexture =
            m_renderResourceRegistry->ResolveTexture(sky.skyTexture);
        if (sky.mode == RenderSkyMode::Cubemap && skyTexture &&
            skyTexture->texture &&
            skyTexture->texture->GetDimension() == RHITextureDimension::TextureCube &&
            RetainRenderSubmissionResource(
                m_submissionBatch.get(), Ref<RefCounted>(skyTexture->texture)))
        {
            m_viewData.environmentSkyTexture = m_renderGraph->ImportTexture(
                skyTexture->texture, skyTexture->accessSnapshot);
            if (m_viewData.environmentSkyTexture.IsValid())
            {
                m_frameExternalTextureAccesses.push_back(
                    {sky.skyTexture, m_viewData.environmentSkyTexture});
            }
            m_renderGraph->SetExportState(
                m_viewData.environmentSkyTexture,
                RHIResourceState::ShaderResource);
        }

        const RenderEnvironmentSnapshot& environment =
            m_renderScene.GetEnvironment();
        const RenderTextureResourceData* irradiance =
            m_renderResourceRegistry->ResolveTexture(
                environment.irradianceTexture);
        const RenderTextureResourceData* prefiltered =
            m_renderResourceRegistry->ResolveTexture(
                environment.prefilteredTexture);
        const RenderTextureResourceData* brdfLUT =
            m_renderResourceRegistry->ResolveTexture(
                environment.brdfLutTexture);
        if (irradiance && irradiance->texture &&
            prefiltered && prefiltered->texture &&
            brdfLUT && brdfLUT->texture)
        {
            irradianceView = RHITextureViewRef(
                m_resourceViewCache->GetDefaultSRV(
                    irradiance->texture.Get()));
            prefilteredView = RHITextureViewRef(
                m_resourceViewCache->GetDefaultSRV(
                    prefiltered->texture.Get()));
            brdfLUTView = RHITextureViewRef(
                m_resourceViewCache->GetDefaultSRV(
                    brdfLUT->texture.Get()));

            const bool retained =
                RetainRenderSubmissionResource(
                    m_submissionBatch.get(),
                    Ref<RefCounted>(irradianceView)) &&
                RetainRenderSubmissionResource(
                    m_submissionBatch.get(),
                    Ref<RefCounted>(irradiance->texture)) &&
                RetainRenderSubmissionResource(
                    m_submissionBatch.get(),
                    Ref<RefCounted>(prefilteredView)) &&
                RetainRenderSubmissionResource(
                    m_submissionBatch.get(),
                    Ref<RefCounted>(prefiltered->texture)) &&
                RetainRenderSubmissionResource(
                    m_submissionBatch.get(),
                    Ref<RefCounted>(brdfLUTView)) &&
                RetainRenderSubmissionResource(
                    m_submissionBatch.get(),
                    Ref<RefCounted>(brdfLUT->texture));
            if (irradianceView && prefilteredView && brdfLUTView && retained)
            {
                m_viewData.environmentIrradianceTexture =
                    m_renderGraph->ImportTexture(
                        irradiance->texture, irradiance->accessSnapshot);
                m_viewData.environmentPrefilteredTexture =
                    m_renderGraph->ImportTexture(
                        prefiltered->texture, prefiltered->accessSnapshot);
                m_viewData.environmentBRDFLUTTexture =
                    m_renderGraph->ImportTexture(
                        brdfLUT->texture, brdfLUT->accessSnapshot);
                m_frameExternalTextureAccesses.push_back(
                    {environment.irradianceTexture,
                     m_viewData.environmentIrradianceTexture});
                m_frameExternalTextureAccesses.push_back(
                    {environment.prefilteredTexture,
                     m_viewData.environmentPrefilteredTexture});
                m_frameExternalTextureAccesses.push_back(
                    {environment.brdfLutTexture,
                     m_viewData.environmentBRDFLUTTexture});
                m_renderGraph->SetExportState(
                    m_viewData.environmentIrradianceTexture,
                    RHIResourceState::ShaderResource);
                m_renderGraph->SetExportState(
                    m_viewData.environmentPrefilteredTexture,
                    RHIResourceState::ShaderResource);
                m_renderGraph->SetExportState(
                    m_viewData.environmentBRDFLUTTexture,
                    RHIResourceState::ShaderResource);
                environmentFrameResources.enabled = true;
                environmentFrameResources.irradianceView =
                    irradianceView.Get();
                environmentFrameResources.prefilteredEnvironmentView =
                    prefilteredView.Get();
                environmentFrameResources.brdfLUTView = brdfLUTView.Get();
            }
        }
    }

    EnvironmentIBLFrameBindingResult environmentBinding;
    if (m_pipelineCache)
    {
        environmentBinding =
            m_pipelineCache->UpdateEnvironmentIBLFrameResources(
                environmentFrameResources);
    }
    m_viewData.textureIBLEnabled =
        environmentBinding.textureIBLSamplingEnabled ? 1 : 0;
    m_viewData.ambientFloorIntensity =
        environmentBinding.textureIBLSamplingEnabled ? 0.0f : 0.08f;
    std::string environmentFallbackReason =
        PipelineCache::GetEnvironmentIBLFallbackReasonName(
            environmentBinding.fallbackReason);
    // PipelineCache correctly uses Disabled when no frame binding was supplied,
    // but a scene can have requested IBL while view construction or retention
    // left that binding incomplete. Keep that state distinct and actionable.
    if (environmentBinding.fallbackReason ==
        EnvironmentIBLFallbackReason::Disabled)
    {
        environmentFallbackReason = "PacketEnvironmentResourcesUnavailable";
    }
    UpdateEnvironmentIBLBindingDiagnostics(
        m_environmentIBLRequested,
        environmentBinding.textureIBLSamplingEnabled,
        std::move(environmentFallbackReason));

    // Record pending GPUScene row uploads before execution passes import the
    // exact resident version. Publication itself does not select a render tier.
    if (m_gpuSceneUploader)
    {
        m_gpuSceneUploader->BuildRenderGraph(
            *m_renderGraph, m_submissionBatch.get(), m_recordedGraphQueuePolicy);
    }

    RGTextureHandle backBufferTarget;
    RGTextureHandle sceneColorTarget;

    if (m_externalRenderTarget.IsValid())
    {
        RHITexture* colorTarget = m_externalRenderTarget.colorTarget.Get();
        backBufferTarget = m_renderGraph->ImportTexture(
            m_externalRenderTarget.colorTarget,
            m_externalRenderTarget.colorInitialState);
        m_viewData.colorTarget = backBufferTarget;
        m_renderGraph->SetExportState(backBufferTarget, m_externalRenderTarget.colorFinalState);

        m_externalRenderTargetStats.active = true;
        m_externalRenderTargetStats.importedColor = true;
        m_externalRenderTargetStats.width = colorTarget->GetWidth();
        m_externalRenderTargetStats.height = colorTarget->GetHeight();
        m_externalRenderTargetStats.colorFormat = colorTarget->GetFormat();
        m_externalRenderTargetStats.colorInitialState = m_externalRenderTarget.colorInitialState;
        m_externalRenderTargetStats.colorFinalState = m_externalRenderTarget.colorFinalState;

        if (m_viewData.viewportWidth == 0 || m_viewData.viewportHeight == 0)
        {
            m_viewData.viewportWidth = colorTarget->GetWidth();
            m_viewData.viewportHeight = colorTarget->GetHeight();
        }
    }
    else
    {
        if (m_externalRenderTargetStats.requested)
        {
            m_externalRenderTargetStats.fallbackReason = "External color target is missing or has zero extent";
        }

        // Import back buffer from swap chain
        RHISwapChain* swapChain = m_renderContext ? m_renderContext->GetSwapChain() : nullptr;
        if (swapChain)
        {
            // Track swapchain state - reset if resized or recreated
            uint32_t currentWidth = swapChain->GetWidth();
            uint32_t currentHeight = swapChain->GetHeight();
            uint32_t bufferCount = swapChain->GetBufferCount();

            if (m_backBufferAccessSnapshots.size() != bufferCount ||
                m_lastSwapChainWidth != currentWidth ||
                m_lastSwapChainHeight != currentHeight)
            {
                // Swap chain was recreated, reset all buffer states to Undefined
                m_backBufferAccessSnapshots.assign(
                    bufferCount,
                    MakeRHITextureAccessSnapshot(
                        RHIResourceState::Undefined,
                        RHIShaderStage::None,
                        GPUQueueDomain::Graphics,
                        RHIContentValidity::Invalid));
                m_lastSwapChainWidth = currentWidth;
                m_lastSwapChainHeight = currentHeight;
            }

            RHITexture* backBuffer = m_renderContext->GetCurrentBackBuffer();
            if (backBuffer)
            {
                // Get the current state for this back buffer
                // First use: Undefined, subsequent uses: Present (after presentation)
                uint32_t backBufferIndex = swapChain->GetCurrentBackBufferIndex();
                // Import with the actual lifetime snapshot (invalid on first use,
                // Present after a realized presentation export).
                backBufferTarget = m_renderGraph->ImportTexture(
                    RHITextureRef(backBuffer),
                    m_backBufferAccessSnapshots[backBufferIndex]);
                m_viewData.colorTarget = backBufferTarget;
                // Export back to Present state for display
                GPUQueueDomain graphicsDomain = GPUQueueDomain::Graphics;
                TryGetGPUQueueDomain(
                    m_renderContext->GetDevice()->GetCapabilities().queueTopology,
                    RHICommandQueueType::Graphics,
                    graphicsDomain);
                m_renderGraph->SetExportAccess(
                    backBufferTarget,
                    MakeRHIAccessSnapshot(RHIResourceState::Present,
                                          RHIShaderStage::None,
                                          graphicsDomain));
                m_backBufferGraphHandle = backBufferTarget;
                m_activeBackBufferIndex = backBufferIndex;
            }
        }
    }

    bool importedExternalDepth = false;
    if (m_externalRenderTargetStats.active && m_externalRenderTarget.depthTarget)
    {
        RHITexture* depthTarget = m_externalRenderTarget.depthTarget.Get();
        const bool matchingExtent =
            depthTarget->GetWidth() == m_externalRenderTargetStats.width &&
            depthTarget->GetHeight() == m_externalRenderTargetStats.height &&
            depthTarget->GetWidth() > 0 &&
            depthTarget->GetHeight() > 0;
        if (matchingExtent)
        {
            m_viewData.depthTarget = m_renderGraph->ImportTexture(
                m_externalRenderTarget.depthTarget,
                m_externalRenderTarget.depthInitialState);
            m_renderGraph->SetExportState(m_viewData.depthTarget, m_externalRenderTarget.depthFinalState);
            m_externalRenderTargetStats.importedDepth = true;
            m_externalRenderTargetStats.depthFormat = depthTarget->GetFormat();
            m_externalRenderTargetStats.depthInitialState = m_externalRenderTarget.depthInitialState;
            m_externalRenderTargetStats.depthFinalState = m_externalRenderTarget.depthFinalState;
            importedExternalDepth = true;
        }
        else
        {
            m_externalRenderTargetStats.fallbackReason =
                "External depth target dimensions do not match the color target";
        }
    }

    // Ensure we have a depth buffer and import it into the RenderGraph
    if (!importedExternalDepth)
    {
        EnsureDepthBuffer(m_viewData.viewportWidth, m_viewData.viewportHeight);
        if (m_depthTexture)
        {
            // Import the existing depth buffer so RenderGraph can manage its barriers
            m_viewData.depthTarget = m_renderGraph->ImportTexture(
                m_depthTexture,
                m_depthAccessSnapshot);
            GPUQueueDomain graphicsDomain = GPUQueueDomain::Graphics;
            TryGetGPUQueueDomain(
                m_renderContext->GetDevice()->GetCapabilities().queueTopology,
                RHICommandQueueType::Graphics,
                graphicsDomain);
            m_renderGraph->SetExportAccess(
                m_viewData.depthTarget,
                MakeRHIAccessSnapshot(RHIResourceState::DepthWrite,
                                      RHIShaderStage::None,
                                      graphicsDomain));
            m_depthGraphHandle = m_viewData.depthTarget;
        }
    }

    const bool velocityTargetRequested =
        (FindCompositionPass<CameraVelocityPass>() && FindCompositionPass<CameraVelocityPass>()->IsEnabled()) ||
        (FindCompositionPass<ObjectVelocityPass>() && FindCompositionPass<ObjectVelocityPass>()->IsEnabled());
    if (velocityTargetRequested &&
        m_viewData.depthTarget.IsValid() &&
        m_viewData.viewportWidth > 0 &&
        m_viewData.viewportHeight > 0)
    {
        RHITextureDesc velocityDesc = RHITextureDesc::Texture2D(
            m_viewData.viewportWidth,
            m_viewData.viewportHeight,
            RHIFormat::RG16_FLOAT,
            RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource);
        velocityDesc.debugName = "SceneCameraVelocity";
        m_viewData.velocityTarget = m_renderGraph->CreateTexture(velocityDesc);
        // Keep the transient velocity chain live through graph export. Typed
        // consumers also read this exact handle, while export preserves
        // standalone velocity diagnostics and deformation evidence.
        GPUQueueDomain velocityDomain = GPUQueueDomain::Graphics;
        TryGetGPUQueueDomain(
            m_renderContext->GetDevice()->GetCapabilities().queueTopology,
            RHICommandQueueType::Graphics,
            velocityDomain);
        m_renderGraph->SetExportAccess(
            m_viewData.velocityTarget,
            MakeRHIAccessSnapshot(RHIResourceState::ShaderResource,
                                  RHIShaderStage::All,
                                  velocityDomain));
    }
    const RHITextureDesc* backBufferDesc = backBufferTarget.IsValid()
                                               ? m_renderGraph->GetTextureDesc(backBufferTarget)
                                               : nullptr;
    const RHIFormat backBufferFormat = backBufferDesc ? backBufferDesc->format : RHIFormat::Unknown;
    const bool deferredRecipeRequested = UsesDeferredOpaquePath(
        m_renderScene.GetSettings().pipelineRequest.recipe);

    // Deferred lighting always writes linear RGBA16F. Tone mapping is the
    // required presentation transform for that color domain, not an optional
    // visual effect. The typed PostProcessPipeline feature owns the single
    // HDR-to-presentation graph boundary for every presentable recipe.
    PostProcessSettings effectivePostProcessSettings = m_postProcessSettings;
    effectivePostProcessSettings.toneMappingOutputColorSpace =
        ResolveToneMappingOutputColorSpace(backBufferFormat);
    if (deferredRecipeRequested)
    {
        effectivePostProcessSettings.enableToneMapping = true;
    }
    m_viewData.toonOutlineRequested =
        effectivePostProcessSettings.enableToonOutline ? 1 : 0;
    m_postProcessSettings.toneMappingOutputColorSpace =
        effectivePostProcessSettings.toneMappingOutputColorSpace;
    std::string deferredToneMappingPresentationDiagnostic;
    if (GetPostProcessStack())
    {
        GetPostProcessStack()->ApplySettings(effectivePostProcessSettings);
        m_postProcessStats.stackStats =
            GetPostProcessStack()->EvaluateEffects(backBufferFormat);
    }
    const bool deferredToneMappingPresentationReady =
        !deferredRecipeRequested ||
        ValidateDeferredToneMappingPresentationContract(
            m_postProcessStats.stackStats,
            backBufferFormat,
            deferredToneMappingPresentationDiagnostic);
    const bool postProcessActive = m_postProcessStats.stackStats.enabledEffectCount > 0 &&
                                   m_postProcessStats.stackStats.planValid &&
                                   m_postProcessStats.stackStats.toneMappingBoundaryValid &&
                                   deferredToneMappingPresentationReady;
    const RenderPassStatus transparentPassStatus = FindCompositionPass<TransparentPass>() != nullptr
        ? FindCompositionPass<TransparentPass>()->GetStatus()
        : RenderPassStatus{};
    const bool transparentSnapshotMayExecute =
        !m_transparentDrawItems.empty() &&
        transparentPassStatus.requestedEnabled &&
        transparentPassStatus.supported && transparentPassStatus.enabled;
    const ParticleVfxRequirements particleVfxRequirements =
        FindCompositionPass<ParticleFeaturePass>() != nullptr
            ? FindCompositionPass<ParticleFeaturePass>()->GetVfxRequirements()
            : ParticleVfxRequirements{};
    const bool particleSnapshotMayExecute =
        FindCompositionPass<ParticleFeaturePass>() != nullptr &&
        FindCompositionPass<ParticleFeaturePass>()->GetStatus().requestedEnabled &&
        FindCompositionPass<ParticleFeaturePass>()->GetStatus().supported &&
        FindCompositionPass<ParticleFeaturePass>()->GetStatus().enabled;
    const bool requiresHDRSceneColor = postProcessActive ||
        deferredRecipeRequested || transparentSnapshotMayExecute ||
        particleSnapshotMayExecute;
    m_sceneColorFormatPolicy = ResolveSceneColorFormatPolicy(
        backBufferFormat, requiresHDRSceneColor);
    if (m_sceneColorFormatPolicy.toneMappingOutputColorSpace !=
        effectivePostProcessSettings.toneMappingOutputColorSpace)
    {
        m_featureCompositionFailureReason =
            "post-process presentation color-space policy changed after preflight";
        return;
    }

    // Select the exact prior-presented exposure state before any HDR producer
    // records work. A configuration/history discontinuity deliberately picks
    // the finite bootstrap buffer; it never mutates committed history until a
    // matching candidate crosses Present later in this frame lifecycle.
    ExposureFrameInput exposureFrameInput;
    if (GetPostProcessStack() == nullptr || m_pipelineCache == nullptr ||
        !m_activeViewHistoryCandidate.has_value() ||
        !m_activeViewHistoryAvailability.has_value())
    {
        m_featureCompositionFailureReason =
            "Physical exposure frame input is unavailable";
        return;
    }
    exposureFrameInput =
        GetPostProcessStack()->GetExposureController().PrepareFrameInput(
            effectivePostProcessSettings.exposureSettings,
            *m_activeViewHistoryCandidate,
            m_activeViewHistoryAvailability->available,
            m_activeViewHistoryAvailability->resetReasons);
    if (!exposureFrameInput.IsValid() ||
        !m_pipelineCache->UpdateFrameExposureStateBuffer(
            exposureFrameInput.stateBuffer.Get()))
    {
        m_featureCompositionFailureReason =
            "Physical exposure state could not be prepared for HDR producers";
        return;
    }
    const ExposureTemporalHistoryDecision effectiveTemporalHistory =
        ResolveExposureTemporalHistory(
            m_activeViewHistoryAvailability->available,
            m_viewData.resetTemporalHistory,
            m_activeViewHistoryAvailability->resetReasons,
            exposureFrameInput);
    if (!effectiveTemporalHistory.IsConsistent())
    {
        m_featureCompositionFailureReason =
            "Physical exposure produced an inconsistent temporal-history decision";
        return;
    }
    // Exposure configuration is part of every HDR history's color-space
    // identity. Propagate this one decision before reflection and post-process
    // features record so no temporal consumer samples a stale exposure domain.
    m_viewData.resetTemporalHistory = effectiveTemporalHistory.reset;
    m_viewData.exposureState = m_renderGraph->ImportBuffer(
        exposureFrameInput.stateBuffer,
        RHIResourceState::ShaderResource,
        {"ExposureCommittedState", RGImportedResourceClass::History});
    if (!m_viewData.exposureState.IsValid())
    {
        m_featureCompositionFailureReason =
            "Physical exposure state could not be imported into the render graph";
        return;
    }
    m_renderGraph->SetExportState(
        m_viewData.exposureState, RHIResourceState::ShaderResource);
    m_viewData.exposureStateSchemaVersion =
        SceneTextures::ExposureStateSchemaVersion;
    m_viewData.exposureStateStride = SceneTextures::ExposureStateStride;
    m_viewData.exposureStateMode =
        effectivePostProcessSettings.exposureSettings.mode;
    m_viewData.exposureStateConfigurationHash =
        exposureFrameInput.configurationHash;
    m_viewData.exposureStateContentHash = exposureFrameInput.contentHash;

    m_postProcessStats.requestedSceneColorFormat = m_sceneColorFormatPolicy.requestedSceneColorFormat;
    m_postProcessStats.actualSceneColorFormat = m_sceneColorFormatPolicy.actualSceneColorFormat;
    m_postProcessStats.backBufferFormat = m_sceneColorFormatPolicy.backBufferFormat;
    m_postProcessStats.toneMappingOutputFormat = m_sceneColorFormatPolicy.toneMappingOutputFormat;
    m_postProcessStats.toneMappingOutputColorSpace = m_sceneColorFormatPolicy.toneMappingOutputColorSpace;
    m_postProcessStats.hdrSceneColorEnabled = m_sceneColorFormatPolicy.hdrSceneColorEnabled;
    m_postProcessStats.hdrFallbackReason = m_sceneColorFormatPolicy.hdrFallbackReason;
    if (!deferredToneMappingPresentationReady)
    {
        m_featureCompositionFailureReason =
            deferredToneMappingPresentationDiagnostic.empty()
                ? "DeferredQuality ToneMapping presentation contract is unavailable"
                : deferredToneMappingPresentationDiagnostic;
        return;
    }
    if (m_pipelineCache)
    {
        m_pipelineCache->SetRenderTargetFormats(m_sceneColorFormatPolicy.actualSceneColorFormat,
                                                m_sceneColorFormatPolicy.actualSceneColorFormat,
                                                m_sceneColorFormatPolicy.toneMappingOutputFormat);
    }
    m_postProcessStats.frameInputDepthAvailable = m_viewData.depthTarget.IsValid();
    m_postProcessStats.frameInputVelocityAvailable = m_viewData.velocityTarget.IsValid();
    m_postProcessStats.frameInputTemporalHistoryAvailable =
        !m_viewData.resetTemporalHistory && m_viewData.previousViewProjectionValid != 0;

    if ((effectivePostProcessSettings.antiAliasingMode ==
             RenderAntiAliasingMode::TAA ||
         transparentSnapshotMayExecute ||
         particleSnapshotMayExecute) &&
        m_viewData.viewportWidth > 0 && m_viewData.viewportHeight > 0)
    {
        RHITextureDesc reactiveMaskDesc = RHITextureDesc::RenderTarget(
            m_viewData.viewportWidth,
            m_viewData.viewportHeight,
            SceneTextures::ReactiveMaskContract.format);
        reactiveMaskDesc.sampleCount =
            SceneTextures::ReactiveMaskContract.sampleCount;
        reactiveMaskDesc.SetOptimizedClearColor(
            RHIClearColor{0.0f, 0.0f, 0.0f, 0.0f});
        reactiveMaskDesc.debugName = "SceneReactiveMask";
        m_viewData.reactiveMaskTarget =
            m_renderGraph->CreateTexture(reactiveMaskDesc);
        if (!m_viewData.reactiveMaskTarget.IsValid())
        {
            m_featureCompositionFailureReason =
                "TAA or transparent rendering requires a graph-owned full-resolution reactive mask";
            return;
        }
        AddReactiveMaskInitializationPass(
            *m_renderGraph, m_viewData.reactiveMaskTarget);
    }

    if (backBufferTarget.IsValid() &&
        (postProcessActive || transparentSnapshotMayExecute ||
         particleSnapshotMayExecute))
    {
        if (backBufferDesc)
        {
            RHITextureDesc sceneColorDesc = *backBufferDesc;
            sceneColorDesc.format = m_sceneColorFormatPolicy.actualSceneColorFormat;
            sceneColorDesc.usage = RHITextureUsage::RenderTarget |
                RHITextureUsage::ShaderResource | RHITextureUsage::CopySrc;
            if (deferredRecipeRequested)
            {
                sceneColorDesc.usage = sceneColorDesc.usage |
                    RHITextureUsage::UnorderedAccess;
            }
            const RenderViewClearActions clearActions =
                ResolveRenderViewClearActions(m_viewData.clearPolicy,
                                              m_viewData.clearColor);
            const RHIClearColor transientInitializationColor =
                clearActions.requiresDeterministicTransientColorInitialization
                ? RVX_SCENE_COLOR_TRANSIENT_LOAD_FALLBACK_VALUE
                : clearActions.colorClearValue;
            sceneColorDesc.SetOptimizedClearColor(transientInitializationColor);
            sceneColorDesc.debugName = "SceneColorGraphInput";

            sceneColorTarget = m_renderGraph->CreateTexture(sceneColorDesc);
            m_viewData.colorTarget = sceneColorTarget;
            // This target is transient and may be consumed by SkyboxPass or
            // post-process even when OpaquePass correctly fails closed. Give
            // it a concrete first write instead of depending on a conditional
            // main-chain producer. For DepthOnly/Nothing, this graph cannot
            // import a previous scene-color image, so the fresh target gets a
            // deterministic fallback before the later requested Load. This
            // does not turn the raster policy into SolidColor: Opaque still
            // loads and Skybox remains disabled.
            AddSceneColorInitializationPass(*m_renderGraph,
                                            sceneColorTarget,
                                            transientInitializationColor);
            m_postProcessStats.sceneColorStagingUsed = true;
            m_postProcessStats.directToBackBuffer = false;
            m_postProcessStats.sceneColorWidth = sceneColorDesc.width;
            m_postProcessStats.sceneColorHeight = sceneColorDesc.height;
            m_postProcessStats.sceneColorFormat = sceneColorDesc.format;
        }
        else
        {
            RVX_CORE_WARN("SceneRenderer: post-process requested but back buffer description is unavailable");
        }
    }

    m_hybridRayTracingFramePlan.reset();
    m_pendingHybridRayTracingObservation = {};
    m_hybridRayTracingExecutionReceipt.reset();
    const bool hybridRayTracingRequested =
        m_renderScene.GetSettings().pipelineRequest.recipe ==
        RenderPipelineRecipe::HybridRayTracing;
    if (hybridRayTracingRequested)
    {
        IRHIDevice* const hybridDevice = m_renderContext != nullptr
            ? m_renderContext->GetDevice() : m_featureReportDeviceForTesting;
        const RHICapabilities hybridCapabilities = hybridDevice != nullptr
            ? hybridDevice->GetCapabilities() : RHICapabilities{};
        const RHIBackendType hybridBackend = hybridDevice != nullptr
            ? hybridDevice->GetBackendType() : RHIBackendType::None;
        const bool productionHybridBackend =
            hybridBackend == RHIBackendType::DX12 ||
            hybridBackend == RHIBackendType::Vulkan;
        const bool sceneInputsReady =
            m_rayTracingSceneManager != nullptr &&
            m_rayTracingSceneStats.prepared &&
            m_rayTracingSceneManager->GetTopLevelAS() != nullptr &&
            m_rayTracingSceneManager->GetInstanceBuffer() != nullptr;
        const bool shadowPipelineReady = m_pipelineCache != nullptr &&
            m_pipelineCache->GetRayTracedShadowPipeline() != nullptr &&
            m_pipelineCache->GetRayTracedShadowShaderTable() != nullptr &&
            FindCompositionPass<RayTracedShadowPass>() != nullptr &&
            FindCompositionPass<RayTracedShadowPass>()->IsSupported();
        const bool reflectionPipelineReady = m_pipelineCache != nullptr &&
            m_pipelineCache->GetRayTracedReflectionPipeline() != nullptr &&
            m_pipelineCache->GetRayTracedReflectionShaderTable() != nullptr &&
            FindCompositionPass<RayTracedReflectionPass>() != nullptr &&
            FindCompositionPass<RayTracedReflectionPass>()->IsSupported() &&
            FindCompositionPass<RayTracedReflectionDenoisePass>() != nullptr &&
            FindCompositionPass<RayTracedReflectionDenoisePass>()->IsEnabled() &&
            FindCompositionPass<RayTracedReflectionCompositePass>() != nullptr &&
            FindCompositionPass<RayTracedReflectionCompositePass>()->IsEnabled();

        HybridRayTracingFramePlanRequest request;
        const RenderFrameHeaderV5& hybridHeader =
            m_renderScene.GetAcceptedHeader();
        request.frameIdentity = {
            hybridHeader.sequence,
            hybridHeader.requiredSceneRevision,
            hybridHeader.surfaceGeneration,
            hybridHeader.configurationRevision};
        request.viewIdentity = m_renderScene.GetView().identity;
        request.outputExtent = m_renderScene.GetViewFamily().extent;
        request.recipe = RenderPipelineRecipe::HybridRayTracing;
        request.quality =
            m_renderScene.GetSettings().pipelineRequest.quality;
        request.profileHash = ComputeRenderPipelineProfileHash(
            request.recipe, request.quality);
        request.rayTracingScenePreparationGeneration =
            m_rayTracingSceneStats.preparationGeneration;
        request.rayTracingSceneStructuralHash =
            m_rayTracingSceneManager != nullptr
                ? m_rayTracingSceneManager
                      ->GetPreparedTLASStructuralSignature().hash
                : 0;
        request.reflectionHistoryReset =
            effectiveTemporalHistory.resetReasons;
        if (!effectiveTemporalHistory.reset && m_viewHistoryCoordinator != nullptr)
        {
            const std::optional<RenderViewHistoryIdentity>& presentedHistory =
                m_viewHistoryCoordinator->GetLastPresentedIdentity();
            if (presentedHistory.has_value())
            {
                request.reflectionHistoryIdentity = *presentedHistory;
                request.reflectionHistoryIdentity.extent =
                    ResolveHybridRayTracingHalfResolution(
                        request.outputExtent).halfResolution;
            }
        }
        if (!request.reflectionHistoryIdentity.IsValid() &&
            request.reflectionHistoryReset == RenderHistoryResetReason::None)
        {
            request.reflectionHistoryReset =
                RenderHistoryResetReason::HistoryUnavailable;
        }

        request.shadowRequirement = HybridRayTracingRequirement::Required;
        request.reflectionRequirement = HybridRayTracingRequirement::Required;
        request.shadow.requested = FindCompositionPass<RayTracedShadowPass>() != nullptr &&
            FindCompositionPass<RayTracedShadowPass>()->IsRequestedEnabled();
        request.shadow.compiled = FindCompositionPass<RayTracedShadowPass>() != nullptr;
        request.shadow.hardwareSupported =
            hybridCapabilities.supportsRaytracing;
        request.shadow.implementationSupported = productionHybridBackend;
        request.shadow.pipelineReady = shadowPipelineReady;
        request.shadow.inputsReady = sceneInputsReady &&
            m_lightingFramePlan->GetPrimaryDirectionalLight() != nullptr;
        request.shadow.dispatchReady = request.shadow.requested &&
            request.shadow.pipelineReady && request.shadow.inputsReady;
        request.reflection.requested =
            FindCompositionPass<RayTracedReflectionPass>() != nullptr &&
            FindCompositionPass<RayTracedReflectionPass>()->IsRequestedEnabled() &&
            FindCompositionPass<RayTracedReflectionDenoisePass>() != nullptr &&
            FindCompositionPass<RayTracedReflectionDenoisePass>()->IsRequestedEnabled() &&
            FindCompositionPass<RayTracedReflectionCompositePass>() != nullptr &&
            FindCompositionPass<RayTracedReflectionCompositePass>()->IsRequestedEnabled();
        request.reflection.compiled =
            FindCompositionPass<RayTracedReflectionPass>() != nullptr &&
            FindCompositionPass<RayTracedReflectionDenoisePass>() != nullptr &&
            FindCompositionPass<RayTracedReflectionCompositePass>() != nullptr;
        request.reflection.hardwareSupported =
            hybridCapabilities.supportsRaytracing;
        request.reflection.implementationSupported = productionHybridBackend;
        request.reflection.pipelineReady = reflectionPipelineReady;
        request.reflection.inputsReady = sceneInputsReady &&
            m_viewData.colorTarget.IsValid() &&
            m_viewData.depthTarget.IsValid() &&
            m_viewData.exposureState.IsValid();
        request.reflection.dispatchReady = request.reflection.requested &&
            request.reflection.pipelineReady && request.reflection.inputsReady;
        request.rasterCSMReady = FindCompositionPass<ShadowPass>() != nullptr &&
            FindCompositionPass<ShadowPass>()->IsEnabled() &&
            m_lightingFramePlan->GetCSMRequest().inputsReady;
        request.screenSpaceReflectionReady =
            m_reflectionFramePlan != nullptr &&
            m_reflectionFramePlan->GetSSRIdentity().inputsReady;
        request.reflectionProbeReady =
            m_reflectionFramePlan != nullptr &&
            !m_reflectionFramePlan->GetSelectedProbes().empty();
        request.imageBasedLightingReady =
            m_reflectionFramePlan != nullptr &&
            m_reflectionFramePlan->GetIBLIdentity()
                    .lightingPlanIdentityHash != 0;

        HybridRayTracingFramePlan hybridPlan =
            HybridRayTracingFramePlan::Build(request);
        if (!hybridPlan.IsValid())
        {
            m_featureCompositionFailureReason =
                "HybridRayTracing requires ready DX12/Vulkan RT shadow and half-resolution reflection paths";
            return;
        }
        m_hybridRayTracingFramePlan = std::move(hybridPlan);
    }

    RayTracingSceneRecordInput rayTracingSceneRecordInput =
        AddRayTracingSceneBuildPass();

    auto lightingResources =
        std::make_shared<RenderLightingFrameResourceSnapshot>();
    lightingResources->lightingPlanIdentity =
        m_lightingFramePlan->GetIdentityHash();
    FrameLightResources mutableLightResources;
    if (m_lightManager)
    {
        mutableLightResources.lightConstantsBuffer =
            m_lightManager->GetLightConstantsBuffer();
        mutableLightResources.pointLightsBuffer =
            m_lightManager->GetPointLightsBuffer();
        mutableLightResources.spotLightsBuffer =
            m_lightManager->GetSpotLightsBuffer();
    }
    if (m_clusteredLighting && m_clusteredLighting->IsInitialized())
    {
        mutableLightResources.clusterConstantsBuffer =
            m_clusteredLighting->GetClusterConstantsBuffer();
        mutableLightResources.clusterBuffer =
            m_clusteredLighting->GetClusterBuffer();
        mutableLightResources.clusterLightIndexBuffer =
            m_clusteredLighting->GetLightIndexBuffer();
    }
    if (m_pipelineCache &&
        m_pipelineCache->FreezeFrameLightResources(
            mutableLightResources,
            lightingResources->frozenLightResources))
    {
        const FrozenFrameLightResources& frozen =
            lightingResources->frozenLightResources;
        lightingResources->lightConstants = m_renderGraph->ImportBuffer(
            frozen.lightConstantsBuffer, RHIResourceState::ConstantBuffer);
        lightingResources->pointLights = m_renderGraph->ImportBuffer(
            frozen.pointLightsBuffer, RHIResourceState::ShaderResource);
        lightingResources->spotLights = m_renderGraph->ImportBuffer(
            frozen.spotLightsBuffer, RHIResourceState::ShaderResource);
        lightingResources->clusterConstants = m_renderGraph->ImportBuffer(
            frozen.clusterConstantsBuffer, RHIResourceState::ConstantBuffer);
        lightingResources->clusterData = m_renderGraph->ImportBuffer(
            frozen.clusterBuffer, RHIResourceState::ShaderResource);
        lightingResources->clusterLightIndices = m_renderGraph->ImportBuffer(
            frozen.clusterLightIndexBuffer, RHIResourceState::ShaderResource);
    }
    lightingResources->textureIBLEnabled =
        m_viewData.textureIBLEnabled != 0;
    if (lightingResources->textureIBLEnabled)
    {
        lightingResources->environmentIBLIrradianceView = irradianceView;
        lightingResources->environmentIBLPrefilteredView = prefilteredView;
        lightingResources->environmentIBLBRDFLUTView = brdfLUTView;
        lightingResources->environmentIBLIrradiance =
            m_viewData.environmentIrradianceTexture;
        lightingResources->environmentIBLPrefiltered =
            m_viewData.environmentPrefilteredTexture;
        lightingResources->environmentIBLBRDFLUT =
            m_viewData.environmentBRDFLUTTexture;
    }
    const LightingLightCandidate* const plannedPrimaryDirectional =
        m_lightingFramePlan->GetPrimaryDirectionalLight();
    const uint64 plannedPrimaryDirectionalId = plannedPrimaryDirectional
        ? plannedPrimaryDirectional->lightId : 0;
    if (!m_lightManager || !m_lightManager->IsLightingPlanBound() ||
        m_lightManager->GetLightingPlanIdentity() !=
            m_lightingFramePlan->GetIdentityHash() ||
        m_lightManager->GetPrimaryDirectionalLightId() !=
            plannedPrimaryDirectionalId ||
        m_lightManager->GetPlanLocalLightAdmittedCount() !=
            m_lightingFramePlan->GetCapacityEvidence()
                .selectedLocalLightCount ||
        !m_clusteredLightingStats.frameBegun ||
        !m_clusteredLightingStats.lightsAssigned ||
        !m_clusteredLightingStats.gpuBuffersUploaded ||
        !lightingResources->IsComplete() ||
        m_lightingFramePlan->GetIBLRequest().inputsReady !=
            lightingResources->textureIBLEnabled)
    {
        m_featureCompositionFailureReason =
            "LightingFramePlan RHI resources are incomplete or identity-mismatched";
        return;
    }

    // All main-chain raster passes record from this immutable, graph-specific
    // snapshot.  The epoch advances after every RenderGraph::Clear() / build;
    // a GPU slice from an older recording cannot be accepted by Depth/Opaque.
    RenderPassRecordContext passRecordContext;
    passRecordContext.view = m_viewData;
    passRecordContext.primaryDirectionalLight = m_primaryDirectionalLight;
    passRecordContext.identity.graph = m_renderGraph.get();
    passRecordContext.identity.graphIdentity = m_renderGraph->GetGraphIdentity();
    passRecordContext.identity.graphRecordingGeneration =
        m_renderGraph->GetRecordingGeneration();
    passRecordContext.identity.frameSequence =
        m_viewData.renderFrameExecutionPlan != nullptr
            ? m_viewData.renderFrameExecutionPlan->frameSequence
            : m_renderScene.GetAcceptedHeader().sequence;
    passRecordContext.identity.viewOrdinal =
        m_viewData.renderFrameExecutionPlan != nullptr
            ? m_viewData.renderFrameExecutionPlan->viewOrdinal
            : 0;
    ++m_renderPassRecordEpoch;
    if (m_renderPassRecordEpoch == 0)
    {
        ++m_renderPassRecordEpoch;
    }
    passRecordContext.identity.recordEpoch = m_renderPassRecordEpoch;
    if (!rayTracingSceneRecordInput.IsEmpty())
    {
        rayTracingSceneRecordInput.identity = passRecordContext.identity;
        if (!rayTracingSceneRecordInput.IsCompatibleWith(
                passRecordContext.identity))
        {
            m_featureCompositionFailureReason =
                "Ray tracing scene graph dependency is unavailable or stale";
            return;
        }
        passRecordContext.rayTracingScene =
            std::move(rayTracingSceneRecordInput);
    }
    const RenderFrameHeaderV5& recordHeader =
        m_renderScene.GetAcceptedHeader();
    passRecordContext.featureFrameIdentity = RenderFrameIdentity{
        recordHeader.sequence,
        recordHeader.requiredSceneRevision,
        recordHeader.surfaceGeneration,
        recordHeader.configurationRevision};
    passRecordContext.effectTime = m_renderScene.GetViewFamily().effectTime;
    passRecordContext.featureViewIdentity =
        m_renderScene.GetView().identity;
    std::shared_ptr<const RenderDeformationFrameResources>
        deformationResources;
    std::string deformationFailure;
    IRHIDevice* const deformationDevice = m_renderContext != nullptr
        ? m_renderContext->GetDevice()
        : m_featureReportDeviceForTesting;
    if (!BuildRenderDeformationFrameResources(
            deformationDevice,
            *m_renderGraph,
            MakeRenderDeformationFrameIdentity(passRecordContext.identity),
            m_renderScene,
            deformationResources,
            deformationFailure))
    {
        m_featureCompositionFailureReason =
            "Render deformation resources are unavailable: " +
            deformationFailure;
        return;
    }
    passRecordContext.deformationResources =
        std::move(deformationResources);
    passRecordContext.lightingPlan = m_lightingFramePlan;
    passRecordContext.lightingResources = std::move(lightingResources);
    // Surface MRT allocation is a sealed-plan decision.  It is required when
    // either the optional SSR route was requested or the selected local probe
    // route needs a materialized receiver, never inferred from mutable pass
    // registration state later in composition.
    passRecordContext.requiresReflectionSurfaceData =
        m_reflectionFramePlan != nullptr &&
        (m_reflectionFramePlan->GetSSRIdentity().requested ||
         !m_reflectionFramePlan->GetSelectedProbes().empty());
    passRecordContext.requiresQualificationSurfaceData =
        m_qualificationCaptureBridge.IsAwaitingGraphRecord();
    passRecordContext.localShadowAtlasPlan = m_localShadowAtlasPlan;
    m_activeLocalShadowPass = std::make_shared<LocalShadowPass>();
    passRecordContext.localShadowPass = m_activeLocalShadowPass;
    AddGPUDrivenCullingPass(passRecordContext.identity);
    passRecordContext.executionPlan = m_viewData.renderFrameExecutionPlan;
    passRecordContext.drawPlan = m_viewData.renderDrawPlan;
    passRecordContext.meshPassPreparation = m_viewData.meshPassPreparation;
    passRecordContext.instanceBatchPlans = m_viewData.instanceBatchPlans;
    passRecordContext.visibility = m_viewData.renderVisibility;
    passRecordContext.executionReport = m_viewData.renderFrameExecutionReport;
    passRecordContext.renderScene = &m_renderScene;
    passRecordContext.opaqueDrawItems = &m_opaqueDrawItems;
    passRecordContext.maskedDrawItems = &m_maskedDrawItems;
    passRecordContext.transparentDrawItems = &m_transparentDrawItems;
    passRecordContext.directOpaqueRasterReadbackQualification =
        &m_directOpaqueRasterReadbackQualification;
    passRecordContext.directOpaqueRasterReadbackSourceFrameSlot =
        m_directOpaqueRasterReadbackSourceFrameSlot;
    passRecordContext.results = std::make_shared<RenderPassRecordResults>();
    passRecordContext.results->identity = passRecordContext.identity;
    passRecordContext.results->directionalShadowOutput = {};
    passRecordContext.results->directionalShadowOutput.identity =
        passRecordContext.identity;
    passRecordContext.results->directionalShadowOutput.lightingPlanIdentity =
        m_lightingFramePlan->GetIdentityHash();
    passRecordContext.results->localShadowOutput = {};
    passRecordContext.results->localShadowOutput.identity =
        passRecordContext.identity;
    passRecordContext.results->localShadowOutput.frameIdentity =
        passRecordContext.featureFrameIdentity;
    passRecordContext.results->localShadowOutput.viewIdentity =
        passRecordContext.featureViewIdentity;
    passRecordContext.results->localShadowOutput.lightingPlanIdentity =
        m_lightingFramePlan->GetIdentityHash();
    passRecordContext.results->shadowStats = {};
    passRecordContext.frameSnapshot = MakeRenderPassFrameSnapshot(
        passRecordContext, *passRecordContext.results);
    m_activeRenderPassResults = passRecordContext.results;
    m_activeRenderDeformationResources =
        passRecordContext.deformationResources;
    m_activeRenderPassIdentity = passRecordContext.identity;
    const std::shared_ptr<std::atomic_bool> gpuSceneRasterRecordingFailure =
        m_gpuSceneRasterCommandRecordingFailed;
    const auto makeGPUDrivenInputs =
        [&passRecordContext,
         gpuSceneRasterRecordingFailure](
            const std::shared_ptr<GPUCullingRecordedState>& recordedState,
            const GPUCullingGraphHandles& handles)
    {
        RenderPassGPUDrivenInputs inputs;
        inputs.identity = passRecordContext.identity;
        inputs.recordedState = handles.IsValid() ? recordedState : nullptr;
        inputs.instances = handles.instances;
        inputs.gpuSceneCandidates = handles.gpuSceneCandidates;
        inputs.gpuScenePrimitives = handles.gpuScenePrimitives;
        inputs.gpuSceneTransforms = handles.gpuSceneTransforms;
        // GPU culling produces the compacted raster-instance indirection
        // stream. Graphics passes consume that stream directly at IA slot 6;
        // the identity stream remains a Direct-instancing owner resource.
        inputs.instanceIndices = handles.visibleInstances;
        inputs.indirectDraws = handles.indirectDraws;
        inputs.drawCount = handles.drawCount;
        inputs.gpuSceneRasterBinding = handles.gpuSceneRasterBinding;
        inputs.gpuSceneRecordingFailure =
            handles.gpuSceneRasterEnabled ? gpuSceneRasterRecordingFailure : nullptr;
        inputs.gpuSceneLeaseVersion = handles.gpuSceneLeaseVersion;
        inputs.gpuSceneRasterEnabled = handles.gpuSceneRasterEnabled;
        return inputs;
    };
    passRecordContext.depthGPUDriven = makeGPUDrivenInputs(
        m_depthGPUCullingRecordedState, m_depthGPUCullingGraphHandles);
    passRecordContext.opaqueGPUDriven = makeGPUDrivenInputs(
        m_opaqueGPUCullingRecordedState, m_opaqueGPUCullingGraphHandles);

    // ForwardStandard retains the established chain. DeferredQuality splits
    // that same inventory around shared opaque submission and real compute
    // lighting; RenderGraph edges remain the authoritative execution order.
    const bool selectedCompositionMissing =
        GetRenderFeatureRegistry() == nullptr ||
        FindCompositionFeature<LegacyForwardFeatureAdapter>() == nullptr ||
        FindCompositionFeature<AmbientOcclusionFeatureAdapter>() == nullptr ||
        FindCompositionFeature<ForwardLightingFeatureAdapter>() == nullptr ||
        FindCompositionFeature<ReflectionFeatureAdapter>() == nullptr ||
        FindCompositionFeature<VolumetricLightingFeatureAdapter>() == nullptr ||
        FindCompositionFeature<ForwardSpecialFeatureAdapter>() == nullptr ||
        FindCompositionFeature<TranslucencyFeatureAdapter>() == nullptr ||
        FindCompositionFeature<PostProcessPipelineFeatureAdapter>() == nullptr ||
        (deferredRecipeRequested &&
         (FindCompositionFeature<DeferredGeometryFeatureAdapter>() == nullptr ||
          FindCompositionFeature<DeferredLightingFeatureAdapter>() == nullptr ||
          FindCompositionFeature<DeferredForwardSpecialFeatureAdapter>() == nullptr));
    if (GetRenderPassRegistry() != nullptr && selectedCompositionMissing &&
        !EnsureDefaultRenderComposition())
    {
        m_featureCompositionFailureReason =
            "Raster render feature composition could not be established";
        return;
    }
    if (!GetRenderPassRegistry() || !GetRenderFeatureRegistry() ||
        !FindCompositionFeature<LegacyForwardFeatureAdapter>() || !FindCompositionFeature<AmbientOcclusionFeatureAdapter>() ||
        !FindCompositionFeature<ForwardLightingFeatureAdapter>() || !FindCompositionFeature<ReflectionFeatureAdapter>() ||
        !FindCompositionFeature<VolumetricLightingFeatureAdapter>() ||
        !FindCompositionFeature<ForwardSpecialFeatureAdapter>() || !FindCompositionFeature<TranslucencyFeatureAdapter>() ||
        !FindCompositionFeature<PostProcessPipelineFeatureAdapter>() ||
        (deferredRecipeRequested &&
         (!FindCompositionFeature<DeferredGeometryFeatureAdapter>() || !FindCompositionFeature<DeferredLightingFeatureAdapter>() ||
          !FindCompositionFeature<DeferredForwardSpecialFeatureAdapter>())))
    {
        m_featureCompositionFailureReason =
            "Raster render feature composition is unavailable";
        return;
    }

    FindCompositionFeature<LegacyForwardFeatureAdapter>()->SetRecordContext(passRecordContext);
    FindCompositionFeature<ForwardLightingFeatureAdapter>()->SetRecordContext(passRecordContext);
    ReflectionFeatureRecordContext reflectionRecordContext;
    reflectionRecordContext.pass = passRecordContext;
    reflectionRecordContext.plan = m_reflectionFramePlan;
    const std::optional<SSRConfig> resolvedSSRQualityPolicy =
        ResolveSSRQualityPolicy(
            m_renderScene.GetSettings().pipelineRequest.quality);
    if (!resolvedSSRQualityPolicy.has_value())
    {
        m_featureCompositionFailureReason =
            "Reflection composition rejected an undeclared SSR quality tier";
        return;
    }
    reflectionRecordContext.ssrQualityPolicy = *resolvedSSRQualityPolicy;
    reflectionRecordContext.ssrConfigurationIdentity =
        ComputeSSRConfigurationIdentity(*resolvedSSRQualityPolicy);
    if (m_reflectionFramePlan != nullptr &&
        m_reflectionFramePlan->GetSSRIdentity().requested)
    {
        const SSRPipelineIdentityResolution ssrPipeline =
            ResolveSSRPipelineIdentity(m_pipelineCache.get());
        reflectionRecordContext.ssrPipelineIdentity =
            ssrPipeline.ready ? ssrPipeline.identity : 0;
    }
    if (m_hybridRayTracingFramePlan.has_value())
    {
        reflectionRecordContext.hybridRayTracingPlan =
            *m_hybridRayTracingFramePlan;
    }
    if (m_reflectionFramePlan != nullptr)
    {
        const std::vector<ReflectionProbeSelection>& probes =
            m_reflectionFramePlan->GetSelectedProbes();
        const size_t realizedProbeCount = std::min(
            probes.size(), reflectionRecordContext.probeTextures.size());
        for (size_t probeIndex = 0; probeIndex < realizedProbeCount;
             ++probeIndex)
        {
            const RenderResourceHandle handle =
                probes[probeIndex].snapshot.texture;
            const RenderTextureResourceData* const texture =
                m_renderResourceRegistry != nullptr
                    ? m_renderResourceRegistry->ResolveTexture(handle)
                    : nullptr;
            if (texture == nullptr || !texture->texture ||
                texture->texture->GetDimension() !=
                    RHITextureDimension::TextureCube ||
                !RetainRenderSubmissionResource(
                    m_submissionBatch.get(),
                    Ref<RefCounted>(texture->texture)))
            {
                continue;
            }
            const RGTextureHandle graphTexture = m_renderGraph->ImportTexture(
                texture->texture, texture->accessSnapshot);
            if (!graphTexture.IsValid())
            {
                continue;
            }
            reflectionRecordContext.probeTextures[probeIndex] = graphTexture;
            reflectionRecordContext.probeReady[probeIndex] = true;
            m_frameExternalTextureAccesses.push_back({handle, graphTexture});
        }
    }
    FindCompositionFeature<ReflectionFeatureAdapter>()->SetRecordContext(
        std::move(reflectionRecordContext));
    VolumetricLightingFeatureRecordContext volumetricLightingRecordContext;
    volumetricLightingRecordContext.passRecordContext = passRecordContext;
    volumetricLightingRecordContext.settings =
        m_renderScene.GetSettings().volumetricLighting;
    if (m_activeViewHistoryCandidate.has_value() &&
        m_activeViewHistoryAvailability.has_value())
    {
        volumetricLightingRecordContext.historyCandidate =
            *m_activeViewHistoryCandidate;
        volumetricLightingRecordContext.historyResetReasons =
            effectiveTemporalHistory.resetReasons;
        volumetricLightingRecordContext.historyAvailable =
            effectiveTemporalHistory.available;
        if (effectiveTemporalHistory.available &&
            m_viewHistoryCoordinator != nullptr)
        {
            const std::optional<RenderViewHistoryIdentity>& presentedHistory =
                m_viewHistoryCoordinator->GetLastPresentedIdentity();
            if (presentedHistory.has_value())
            {
                volumetricLightingRecordContext.historySource =
                    *presentedHistory;
            }
        }
    }
    FindCompositionFeature<VolumetricLightingFeatureAdapter>()->SetRecordContext(
        std::move(volumetricLightingRecordContext));
    FindCompositionFeature<ForwardSpecialFeatureAdapter>()->SetRecordContext(passRecordContext);
    FindCompositionFeature<TranslucencyFeatureAdapter>()->SetRecordContext(passRecordContext);
    AmbientOcclusionFeatureRecordContext ambientOcclusionRecordContext;
    ambientOcclusionRecordContext.passRecordContext = passRecordContext;
    ambientOcclusionRecordContext.settings = ResolveAmbientOcclusionSettings(
        m_renderScene.GetSettings());
    if (m_activeViewHistoryCandidate.has_value() &&
        m_activeViewHistoryAvailability.has_value())
    {
        ambientOcclusionRecordContext.historyIdentity =
            *m_activeViewHistoryCandidate;
        ambientOcclusionRecordContext.historyResetReasons =
            effectiveTemporalHistory.resetReasons;
        ambientOcclusionRecordContext.historyAvailable =
            effectiveTemporalHistory.available;
    }
    FindCompositionFeature<AmbientOcclusionFeatureAdapter>()->SetRecordContext(
        std::move(ambientOcclusionRecordContext));
    if (FindCompositionFeature<DeferredGeometryFeatureAdapter>() != nullptr)
    {
        FindCompositionFeature<DeferredGeometryFeatureAdapter>()->SetRecordContext(passRecordContext);
    }
    if (FindCompositionFeature<DeferredLightingFeatureAdapter>() != nullptr)
    {
        FindCompositionFeature<DeferredLightingFeatureAdapter>()->SetRecordContext(passRecordContext);
    }
    if (FindCompositionFeature<DeferredForwardSpecialFeatureAdapter>() != nullptr)
    {
        FindCompositionFeature<DeferredForwardSpecialFeatureAdapter>()->SetRecordContext(passRecordContext);
    }
    PostProcessPipelineRecordContext postProcessRecordContext;
    postProcessRecordContext.frameIdentity =
        passRecordContext.featureFrameIdentity;
    postProcessRecordContext.viewIdentity =
        passRecordContext.featureViewIdentity;
    postProcessRecordContext.presentableColor = backBufferTarget;
    postProcessRecordContext.presentableColorMetadata =
        SceneTextures::Metadata{
            RenderOutputExtent{
                m_viewData.viewportWidth, m_viewData.viewportHeight},
            m_sceneColorFormatPolicy.toneMappingOutputFormat,
            RHISampleCount::Count1};
    postProcessRecordContext.frameInputs.depth = m_viewData.depthTarget;
    postProcessRecordContext.frameInputs.velocity = m_viewData.velocityTarget;
    postProcessRecordContext.frameInputs.reactiveMask =
        m_viewData.reactiveMaskTarget;
    postProcessRecordContext.frameInputs.exposureState =
        m_viewData.exposureState;
    postProcessRecordContext.frameInputs.exposureStateContentHash =
        exposureFrameInput.contentHash;
    postProcessRecordContext.frameInputs.outputFormat =
        m_sceneColorFormatPolicy.toneMappingOutputFormat;
    postProcessRecordContext.frameInputs.frameIndex = m_viewData.frameNumber;
    postProcessRecordContext.frameInputs.fixedDeltaSeconds =
        m_renderScene.GetViewFamily().effectTime.fixedDeltaSeconds;
    postProcessRecordContext.frameInputs.jitterOffset =
        m_viewData.currentTemporalJitter.pixelOffset;
    postProcessRecordContext.frameInputs.currentViewProjection =
        m_viewData.jitteredViewProjectionMatrix;
    postProcessRecordContext.frameInputs.previousViewProjection =
        m_viewData.previousJitteredViewProjectionMatrix;
    postProcessRecordContext.frameInputs.currentViewProjectionValid = true;
    postProcessRecordContext.frameInputs.previousViewProjectionValid =
        m_viewData.previousViewProjectionValid != 0;
    postProcessRecordContext.frameInputs.resetTemporalHistory =
        effectiveTemporalHistory.reset;
    if (m_activeViewHistoryCandidate.has_value() &&
        m_activeViewHistoryAvailability.has_value())
    {
        postProcessRecordContext.frameInputs.historyIdentity =
            *m_activeViewHistoryCandidate;
        postProcessRecordContext.frameInputs.historyResetReasons =
            effectiveTemporalHistory.resetReasons;
        postProcessRecordContext.frameInputs.historyAvailable =
            effectiveTemporalHistory.available;
    }
    postProcessRecordContext.frameInputs.sceneColorDomain =
        PostProcessColorDomain::SceneLinearHDR;
    postProcessRecordContext.frameInputs.outputColorDomain =
        // sRGB render targets receive linear shader output and apply the
        // final display encode in hardware. The feature later publishes the
        // stored encoded presentation contract on the blackboard.
        IsSRGBFormat(backBufferFormat)
            ? PostProcessColorDomain::DisplayLinearLDR
            : PostProcessColorDomain::DisplayEncodedLDR;
    postProcessRecordContext.passRecordContext = passRecordContext;
    FindCompositionFeature<PostProcessPipelineFeatureAdapter>()->SetRecordContext(
        std::move(postProcessRecordContext));

    const RenderFrameHeaderV5& acceptedHeader =
        m_renderScene.GetAcceptedHeader();
    const RenderViewFamilySnapshot& acceptedViewFamily =
        m_renderScene.GetViewFamily();
    const RenderViewSnapshot& acceptedView = m_renderScene.GetView();
    IRHIDevice* const featureDevice =
        m_renderContext != nullptr
            ? m_renderContext->GetDevice()
            : m_featureReportDeviceForTesting;

    RenderFeatureContext featureContext;
    featureContext.frameIdentity = RenderFrameIdentity{
        acceptedHeader.sequence,
        acceptedHeader.requiredSceneRevision,
        acceptedHeader.surfaceGeneration,
        acceptedHeader.configurationRevision};
    featureContext.frameStamp = acceptedViewFamily.frame;
    featureContext.effectTime = acceptedViewFamily.effectTime;
    featureContext.outputExtent = acceptedViewFamily.extent;
    featureContext.viewFamilyId = acceptedViewFamily.familyId;
    featureContext.primaryViewIndex =
        acceptedViewFamily.primaryViewIndex;
    featureContext.viewCount =
        static_cast<uint32>(acceptedViewFamily.views.size());
    featureContext.viewIdentity = acceptedView.identity;
    featureContext.recordingViewOrdinal =
        passRecordContext.identity.viewOrdinal;
    featureContext.recordEpoch = passRecordContext.identity.recordEpoch;
    featureContext.pipeline = m_renderScene.GetSettings().pipelineRequest;
    featureContext.viewFamily =
        std::make_shared<const RenderViewFamilySnapshot>(acceptedViewFamily);
    featureContext.snapshotInputs = RenderFeatureSnapshotView(
        m_featureSnapshot,
        GetBuiltinRenderFeatureChannelRegistry());
    if (featureDevice != nullptr)
    {
        const RHICapabilities& capabilities =
            featureDevice->GetCapabilities();
        featureContext.backend = capabilities.backendType;
        featureContext.hardwareCapabilities =
            BuildRenderFeatureCapabilityMask(capabilities);
    }

    const RenderFeatureFramePrepareResult featurePrepare =
        GetRenderFeatureRegistry()->PrepareFrame(featureContext);
    if (!featurePrepare.Succeeded())
    {
        m_featureCompositionFailureReason =
            featurePrepare.diagnostic.empty()
                ? GetRenderFeatureRegistryCodeName(featurePrepare.code)
                : featurePrepare.diagnostic;
        m_lastAttemptedRenderExecutionReceipt =
            GetRenderFeatureRegistry()->CollectExecutionReceipt(
                m_executionBuildIdentity);
        return;
    }

    const RenderGraphBlackboardProvenance provenance{
        passRecordContext.identity.graphIdentity,
        passRecordContext.identity.graphRecordingGeneration,
        passRecordContext.identity.frameSequence,
        passRecordContext.identity.viewOrdinal,
        passRecordContext.identity.recordEpoch};
    RenderGraphBlackboard blackboard(provenance);
    RenderFeatureGraphBuilder featureGraphBuilder(*m_renderGraph, provenance);
    const RenderFeatureRegistryCode compositionCode =
        GetRenderFeatureRegistry()->AddPasses(featureGraphBuilder, blackboard);

    LegacyForwardFeaturePassChainStats passChain;
    const TranslucencyFeaturePassChainStats translucencyChain =
        FindCompositionFeature<TranslucencyFeatureAdapter>()->GetPassChainStats();
    if (UsesDeferredOpaquePath(featurePrepare.pipeline.resolvedRecipe))
    {
        const DeferredRecipeFeaturePassChainStats geometryChain =
            FindCompositionFeature<DeferredGeometryFeatureAdapter>()->GetPassChainStatsSnapshot();
        const DeferredRecipeFeaturePassChainStats postLightingChain =
            FindCompositionFeature<DeferredForwardSpecialFeatureAdapter>()->GetPassChainStatsSnapshot();
        passChain.frameSequence = geometryChain.frameSequence != 0
            ? geometryChain.frameSequence
            : (postLightingChain.frameSequence != 0
                ? postLightingChain.frameSequence
                : translucencyChain.frameSequence);
        passChain.registeredPassCount = geometryChain.registeredPassCount;
        passChain.graphPassCount = geometryChain.graphPassCount +
            postLightingChain.graphPassCount +
            translucencyChain.graphPassCount + 1U;
        passChain.skippedDisabledPassCount =
            geometryChain.skippedDisabledPassCount +
            postLightingChain.skippedDisabledPassCount +
            translucencyChain.skippedDisabledPassCount;
        passChain.skippedUnsupportedPassCount =
            geometryChain.skippedUnsupportedPassCount +
            postLightingChain.skippedUnsupportedPassCount +
            translucencyChain.skippedUnsupportedPassCount;
        passChain.passStatuses = geometryChain.passStatuses;
        passChain.graphPassNames = geometryChain.graphPassNames;
        passChain.graphPassNames.emplace_back("DeferredLightingPass");
        passChain.graphPassNames.insert(
            passChain.graphPassNames.end(),
            postLightingChain.graphPassNames.begin(),
            postLightingChain.graphPassNames.end());
        passChain.graphPassNames.insert(
            passChain.graphPassNames.end(),
            translucencyChain.graphPassNames.begin(),
            translucencyChain.graphPassNames.end());
        passChain.unsupportedPassNames = geometryChain.unsupportedPassNames;
        passChain.unsupportedPassNames.insert(
            passChain.unsupportedPassNames.end(),
            postLightingChain.unsupportedPassNames.begin(),
            postLightingChain.unsupportedPassNames.end());
        passChain.unsupportedPassNames.insert(
            passChain.unsupportedPassNames.end(),
            translucencyChain.unsupportedPassNames.begin(),
            translucencyChain.unsupportedPassNames.end());
    }
    else
    {
        const LegacyForwardFeaturePassChainStats surfaceChain =
            FindCompositionFeature<LegacyForwardFeatureAdapter>()->GetPassChainStatsSnapshot();
        const LegacyForwardFeaturePassChainStats lightingChain =
            FindCompositionFeature<ForwardLightingFeatureAdapter>()->GetPassChainStatsSnapshot();
        const LegacyForwardFeaturePassChainStats specialChain =
            FindCompositionFeature<ForwardSpecialFeatureAdapter>()->GetPassChainStatsSnapshot();
        passChain.frameSequence = surfaceChain.frameSequence != 0
            ? surfaceChain.frameSequence
            : (lightingChain.frameSequence != 0
                ? lightingChain.frameSequence : specialChain.frameSequence);
        passChain.registeredPassCount = surfaceChain.registeredPassCount != 0
            ? surfaceChain.registeredPassCount
            : (lightingChain.registeredPassCount != 0
                ? lightingChain.registeredPassCount
                : specialChain.registeredPassCount);
        passChain.graphPassCount = surfaceChain.graphPassCount +
            lightingChain.graphPassCount + specialChain.graphPassCount +
            translucencyChain.graphPassCount;
        passChain.skippedDisabledPassCount =
            surfaceChain.skippedDisabledPassCount +
            lightingChain.skippedDisabledPassCount +
            specialChain.skippedDisabledPassCount +
            translucencyChain.skippedDisabledPassCount;
        passChain.skippedUnsupportedPassCount =
            surfaceChain.skippedUnsupportedPassCount +
            lightingChain.skippedUnsupportedPassCount +
            specialChain.skippedUnsupportedPassCount +
            translucencyChain.skippedUnsupportedPassCount;
        passChain.passStatuses = surfaceChain.passStatuses.empty()
            ? lightingChain.passStatuses : surfaceChain.passStatuses;
        passChain.graphPassNames = surfaceChain.graphPassNames;
        passChain.graphPassNames.insert(
            passChain.graphPassNames.end(),
            lightingChain.graphPassNames.begin(),
            lightingChain.graphPassNames.end());
        passChain.graphPassNames.insert(
            passChain.graphPassNames.end(),
            specialChain.graphPassNames.begin(),
            specialChain.graphPassNames.end());
        passChain.graphPassNames.insert(
            passChain.graphPassNames.end(),
            translucencyChain.graphPassNames.begin(),
            translucencyChain.graphPassNames.end());
        passChain.unsupportedPassNames = surfaceChain.unsupportedPassNames;
        passChain.unsupportedPassNames.insert(
            passChain.unsupportedPassNames.end(),
            lightingChain.unsupportedPassNames.begin(),
            lightingChain.unsupportedPassNames.end());
        passChain.unsupportedPassNames.insert(
            passChain.unsupportedPassNames.end(),
            specialChain.unsupportedPassNames.begin(),
            specialChain.unsupportedPassNames.end());
        passChain.unsupportedPassNames.insert(
            passChain.unsupportedPassNames.end(),
            translucencyChain.unsupportedPassNames.begin(),
            translucencyChain.unsupportedPassNames.end());
    }
    ++m_passChainStats.frameCount;
    m_passChainStats.passStatuses = passChain.passStatuses;
    m_passChainStats.registeredPassCount = passChain.registeredPassCount;
    m_passChainStats.graphPassCount = passChain.graphPassCount;
    m_passChainStats.skippedDisabledPassCount =
        passChain.skippedDisabledPassCount;
    m_passChainStats.skippedUnsupportedPassCount =
        passChain.skippedUnsupportedPassCount;

    for (const std::string& unsupportedName :
         passChain.unsupportedPassNames)
    {
        const auto statusIt = std::find_if(
            passChain.passStatuses.begin(),
            passChain.passStatuses.end(),
            [&unsupportedName](const RenderPassStatus& status)
            {
                return status.name == unsupportedName;
            });
        const bool alreadyLogged = std::find(
            m_loggedUnsupportedPassNames.begin(),
            m_loggedUnsupportedPassNames.end(),
            unsupportedName) != m_loggedUnsupportedPassNames.end();
        if (!alreadyLogged)
        {
            const std::string reason =
                statusIt != passChain.passStatuses.end() &&
                        !statusIt->unsupportedReason.empty()
                    ? statusIt->unsupportedReason
                    : "Unsupported";
            RVX_CORE_WARN(
                "SceneRenderer: Skipping unsupported requested pass '{}': {}",
                unsupportedName,
                reason);
            m_loggedUnsupportedPassNames.push_back(unsupportedName);
        }
    }

    if (compositionCode != RenderFeatureRegistryCode::Success)
    {
        m_featureCompositionFailureReason =
            GetRenderFeatureRegistryCodeName(compositionCode);
        std::vector<RenderFeatureReadiness> readinessCandidates;
        if (UsesDeferredOpaquePath(featurePrepare.pipeline.resolvedRecipe))
        {
            readinessCandidates = {
                FindCompositionFeature<DeferredGeometryFeatureAdapter>()->ResolveReadiness(),
                FindCompositionFeature<AmbientOcclusionFeatureAdapter>()->ResolveReadiness(),
                FindCompositionFeature<DeferredLightingFeatureAdapter>()->ResolveReadiness(),
                FindCompositionFeature<ReflectionFeatureAdapter>()->ResolveReadiness(),
                FindCompositionFeature<VolumetricLightingFeatureAdapter>()->ResolveReadiness(),
                FindCompositionFeature<DeferredForwardSpecialFeatureAdapter>()->ResolveReadiness(),
                FindCompositionFeature<TranslucencyFeatureAdapter>()->ResolveReadiness(),
                FindCompositionFeature<PostProcessPipelineFeatureAdapter>()->ResolveReadiness()};
        }
        else
        {
            readinessCandidates.push_back(
                FindCompositionFeature<LegacyForwardFeatureAdapter>()->ResolveReadiness());
            readinessCandidates.push_back(
                FindCompositionFeature<AmbientOcclusionFeatureAdapter>()->ResolveReadiness());
            readinessCandidates.push_back(
                FindCompositionFeature<ForwardLightingFeatureAdapter>()->ResolveReadiness());
            readinessCandidates.push_back(
                FindCompositionFeature<ReflectionFeatureAdapter>()->ResolveReadiness());
            readinessCandidates.push_back(
                FindCompositionFeature<VolumetricLightingFeatureAdapter>()->ResolveReadiness());
            readinessCandidates.push_back(
                FindCompositionFeature<ForwardSpecialFeatureAdapter>()->ResolveReadiness());
            readinessCandidates.push_back(
                FindCompositionFeature<TranslucencyFeatureAdapter>()->ResolveReadiness());
            readinessCandidates.push_back(
                FindCompositionFeature<PostProcessPipelineFeatureAdapter>()->ResolveReadiness());
        }
        const auto failedReadiness = std::find_if(
            readinessCandidates.begin(), readinessCandidates.end(),
            [](const RenderFeatureReadiness& readiness)
            {
                return !readiness.IsReadyToExecute() &&
                       !readiness.diagnostic.empty();
            });
        if (failedReadiness != readinessCandidates.end())
        {
            m_featureCompositionFailureReason = failedReadiness->diagnostic;
        }
        m_lastAttemptedRenderExecutionReceipt =
            GetRenderFeatureRegistry()->CollectExecutionReceipt(
                m_executionBuildIdentity);
        return;
    }

    if (m_qualificationCaptureBridge.IsAwaitingGraphRecord())
    {
        // The capture consumer is deliberately local to the renderer facade:
        // it adds no feature implementation and cannot publish a substitute
        // scene surface. It only retains the exact typed feature outputs
        // already composed into this graph.
        RenderFeatureDescriptor captureConsumer;
        captureConsumer.featureId = BuiltinRenderFeatureIds::PostProcessPipeline;
        captureConsumer.schemaVersion = 1;
        captureConsumer.name = "RenderQualityCapture";
        captureConsumer.phase = RenderFeaturePhase::Present;
        captureConsumer.consumedResources = {
            {PostProcessOutputs::ResourceId, PostProcessOutputs::SchemaVersion},
            {TranslucencyOutputs::ResourceId, TranslucencyOutputs::SchemaVersion},
            {AmbientOcclusionOutputs::ResourceId,
             AmbientOcclusionOutputs::SchemaVersion}};

        RenderGraphBlackboardCode captureCode =
            RenderGraphBlackboardCode::Success;
        const PostProcessOutputs* const postProcess =
            blackboard.Require<PostProcessOutputs>(
                captureConsumer.featureId, captureConsumer, &captureCode);
        const TranslucencyOutputs* const translucency =
            captureCode == RenderGraphBlackboardCode::Success
                ? blackboard.Require<TranslucencyOutputs>(
                    captureConsumer.featureId, captureConsumer, &captureCode)
                : nullptr;
        const AmbientOcclusionOutputs* const ambientOcclusion =
            captureCode == RenderGraphBlackboardCode::Success
                ? blackboard.Require<AmbientOcclusionOutputs>(
                    captureConsumer.featureId, captureConsumer, &captureCode)
                : nullptr;

        bool reverseZ = false;
        const bool captureInputsValid =
            captureCode == RenderGraphBlackboardCode::Success &&
            postProcess != nullptr && translucency != nullptr &&
            ambientOcclusion != nullptr &&
            postProcess->provenance == provenance &&
            translucency->provenance == provenance &&
            ambientOcclusion->provenance == provenance &&
            postProcess->frameIdentity == featureContext.frameIdentity &&
            translucency->frameIdentity == featureContext.frameIdentity &&
            ambientOcclusion->frameIdentity == featureContext.frameIdentity &&
            postProcess->viewIdentity == featureContext.viewIdentity &&
            translucency->viewIdentity == featureContext.viewIdentity &&
            ambientOcclusion->viewIdentity == featureContext.viewIdentity &&
            TryDerivePerspectiveCaptureDepthConvention(acceptedView, reverseZ);
        if (!captureInputsValid)
        {
            // Qualification is intentionally optional for the visualizer but
            // never partial: the ordinary render remains valid while this
            // one-shot request receives an observable rejected state.
            m_qualificationCaptureBridge.RejectPending();
        }
        else
        {
            QualificationCaptureGraphInputs captureInputs;
            captureInputs.provenance = provenance;
            captureInputs.frameIdentity = featureContext.frameIdentity;
            captureInputs.viewIdentity = featureContext.viewIdentity;
            captureInputs.nearPlane = acceptedView.nearPlane;
            captureInputs.farPlane = acceptedView.farPlane;
            captureInputs.originBottomLeft = featureDevice != nullptr &&
                featureDevice->GetBackendType() == RHIBackendType::OpenGL;
            captureInputs.reverseZ = reverseZ;
            captureInputs.perspectiveProjection = true;
            captureInputs.sources = {{
                {QualificationCaptureAttachmentSemantic::BeautyLDR,
                 postProcess->presentableColor,
                 postProcess->presentableColorMetadata,
                 blackboard.GetProducer(PostProcessOutputs::ResourceId)},
                {QualificationCaptureAttachmentSemantic::PreToneHDR,
                 translucency->hdrSceneColor,
                 translucency->hdrSceneColorMetadata,
                 blackboard.GetProducer(TranslucencyOutputs::ResourceId)},
                {QualificationCaptureAttachmentSemantic::LinearDepth,
                 translucency->temporalLinearDepth.IsValid() ? translucency->temporalLinearDepth : translucency->deviceDepth,
                 translucency->temporalLinearDepth.IsValid() ? translucency->temporalLinearDepthMetadata : translucency->deviceDepthMetadata,
                 blackboard.GetProducer(TranslucencyOutputs::ResourceId)},
                {QualificationCaptureAttachmentSemantic::Normal,
                 translucency->worldNormal,
                 translucency->worldNormalMetadata,
                 blackboard.GetProducer(TranslucencyOutputs::ResourceId)},
                {QualificationCaptureAttachmentSemantic::Velocity,
                 translucency->velocity,
                 translucency->velocityMetadata,
                 blackboard.GetProducer(TranslucencyOutputs::ResourceId)},
                {QualificationCaptureAttachmentSemantic::AmbientOcclusion,
                 ambientOcclusion->ambientOcclusion,
                 ambientOcclusion->ambientOcclusionMetadata,
                 blackboard.GetProducer(AmbientOcclusionOutputs::ResourceId)},
                {QualificationCaptureAttachmentSemantic::ReactiveMask,
                 translucency->reactiveMask,
                 translucency->reactiveMaskMetadata,
                 blackboard.GetProducer(TranslucencyOutputs::ResourceId)}}};
            if (featureDevice == nullptr ||
                !m_qualificationCaptureBridge.Record(
                    *m_renderGraph, *featureDevice, captureInputs))
            {
                RVX_RENDER_WARN(
                    "SceneRenderer: RenderQuality qualification capture could not record a complete typed attachment bundle");
            }
        }
    }

    // The PostProcessPipeline feature is the sole owner of the typed HDR to
    // presentation boundary. SceneRenderer deliberately has no direct stack
    // execution bypass: the feature consumes TranslucencyOutputs v5 and
    // publishes PostProcessOutputs v2 inside the same graph composition.
    m_postProcessStats.stackStats = FindCompositionFeature<PostProcessPipelineFeatureAdapter>()->GetLastPlanStats();
}

void SceneRenderer::AddPass(std::unique_ptr<IRenderPass> pass)
{
    InvalidateRenderFramePlan();
    if (m_defaultRenderComposition == nullptr)
    {
        m_defaultRenderComposition =
            std::make_unique<DefaultRenderComposition>();
    }
    m_defaultRenderComposition->GetPassBundle().AddPass(
        std::move(pass),
        m_renderContext != nullptr ? m_renderContext->GetDevice() : nullptr);
}

void SceneRenderer::RefreshFrameDiagnosticsForTesting(bool graphBuilt)
{
    m_passChainStats = {};
    if (GetRenderPassRegistry())
    {
        m_passChainStats.passStatuses = GetRenderPassRegistry()->GetPassStatuses();
        m_passChainStats.registeredPassCount = m_passChainStats.passStatuses.size();

        for (const RenderPassStatus& status : m_passChainStats.passStatuses)
        {
            if (!status.requestedEnabled)
            {
                ++m_passChainStats.skippedDisabledPassCount;
                continue;
            }

            if (!status.supported)
            {
                ++m_passChainStats.skippedUnsupportedPassCount;
                continue;
            }

            if (!status.enabled)
            {
                ++m_passChainStats.skippedDisabledPassCount;
                continue;
            }

            ++m_passChainStats.graphPassCount;
        }
    }

    RefreshFrameDiagnostics(false, false, graphBuilt, false, nullptr);
}

bool SceneRenderer::RemovePass(const char* name)
{
    InvalidateRenderFramePlan();
    if (m_defaultRenderComposition == nullptr || name == nullptr)
    {
        return false;
    }
    return m_defaultRenderComposition->GetPassBundle().RemovePass(name);
}

void SceneRenderer::ClearPasses()
{
    InvalidateRenderFramePlan();
    if (m_defaultRenderComposition != nullptr)
    {
        m_defaultRenderComposition->GetPassBundle().Clear();
    }
    m_passChainStats = {};
    m_loggedUnsupportedPassNames.clear();
}

size_t SceneRenderer::GetPassCount() const
{
    return GetRenderPassRegistry() ? GetRenderPassRegistry()->GetPassCount() : 0;
}

bool SceneRenderer::AddPreGraphPrepareCallback(const void* owner, PreGraphPrepareCallback callback)
{
    if (!owner || !callback)
        return false;

    const auto it = std::find_if(
        m_preGraphPrepareCallbacks.begin(),
        m_preGraphPrepareCallbacks.end(),
        [owner](const PreGraphPrepareCallbackEntry& entry)
        {
            return entry.owner == owner;
        });
    if (it != m_preGraphPrepareCallbacks.end())
        return false;

    InvalidateRenderFramePlan();

    PreGraphPrepareCallbackEntry entry;
    entry.owner = owner;
    entry.callback = std::move(callback);
    m_preGraphPrepareCallbacks.push_back(std::move(entry));
    return true;
}

bool SceneRenderer::RemovePreGraphPrepareCallback(const void* owner)
{
    if (!owner)
        return false;

    const auto it = std::find_if(
        m_preGraphPrepareCallbacks.begin(),
        m_preGraphPrepareCallbacks.end(),
        [owner](const PreGraphPrepareCallbackEntry& entry)
        {
            return entry.owner == owner;
        });
    if (it == m_preGraphPrepareCallbacks.end())
        return false;

    InvalidateRenderFramePlan();
    m_preGraphPrepareCallbacks.erase(it);
    return true;
}

void SceneRenderer::RunPreGraphPrepareCallbacks()
{
    const auto callbacks = m_preGraphPrepareCallbacks;
    for (const PreGraphPrepareCallbackEntry& entry : callbacks)
    {
        if (!entry.owner || !entry.callback)
            continue;

        const auto stillRegistered = std::find_if(
            m_preGraphPrepareCallbacks.begin(),
            m_preGraphPrepareCallbacks.end(),
            [&entry](const PreGraphPrepareCallbackEntry& current)
            {
                return current.owner == entry.owner;
            });
        if (stillRegistered == m_preGraphPrepareCallbacks.end())
            continue;

        entry.callback(m_viewData);
    }
}

void SceneRenderer::SetupDefaultPostProcess()
{
    if (!m_renderContext || !m_renderContext->GetDevice() ||
        m_defaultRenderComposition == nullptr)
    {
        return;
    }

    if (!m_defaultRenderComposition->InitializePostProcess(
            m_renderContext->GetDevice(),
            m_pipelineCache.get(),
            m_resourceViewCache.get()))
    {
        RVX_RENDER_ERROR(
            "SceneRenderer: failed to initialize default post-process composition");
        return;
    }

    ApplyPostProcessSettings(MakeDefaultRuntimePostProcessSettings());

    // SetupDefaultRenderFeatures may seal the registry before this stack is
    // constructed. Bind only after the stack, its effects, and its settings
    // are complete; SetStack invalidates any prior frozen graph state.
    if (FindCompositionFeature<PostProcessPipelineFeatureAdapter>() != nullptr)
    {
        FindCompositionFeature<PostProcessPipelineFeatureAdapter>()->SetStack(
            GetPostProcessStack());
    }
}

} // namespace RVX
