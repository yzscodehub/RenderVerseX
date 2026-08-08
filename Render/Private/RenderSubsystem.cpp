/**
 * @file RenderSubsystem.cpp
 * @brief RenderSubsystem implementation
 */

#include "Render/RenderSubsystem.h"
#include "Context/RenderContextInternal.h"
#include "Core/Log.h"
#include "Render/Context/RenderContext.h"
#include "Render/Renderer/RenderScene.h"
#include "Render/Renderer/SceneRenderer.h"
#include "Resources/RenderResourceRegistry.h"
#include "Resources/RenderRetirementQueue.h"
#include "Resources/RenderUploadProcessor.h"
#include "Resources/RenderSubmissionTracker.h"
#include "Runtime/DedicatedRenderExecutor.h"
#include "Runtime/RenderThreadRuntime.h"

#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace RVX
{

namespace
{
    class ClearPresentFrameConsumer final : public IRenderFrameConsumer,
                                            public NonMovable
    {
    public:
        RenderRuntimeResult Initialize(
            const RenderRuntimeConfig& config,
            const NativeSurfaceDesc& surface,
            RenderResourceStatusTable& statusTable) override
        {
            RenderRuntimeResult result;
            RHIBackendType backend = config.backendType;
            if (backend == RHIBackendType::Auto)
            {
                backend = SelectBestBackend();
            }
            result.backend = backend;
            result.surfaceGeneration = surface.generation;
            if (!surface.IsValidFor(backend))
            {
                result.code = RenderRuntimeCode::InvalidSurface;
                result.message = "Initial surface is invalid for the selected backend";
                return result;
            }

            m_context = std::make_unique<RenderContext>();
            RenderContextConfig contextConfig;
            contextConfig.backendType = backend;
            contextConfig.enableValidation = config.enableValidation;
            contextConfig.enableGPUValidation = config.enableGPUValidation;
            contextConfig.allowSoftwareAdapter =
                config.allowSoftwareAdapter;
            contextConfig.vsync = surface.vsync;
            contextConfig.frameBuffering = config.frameBuffering;
            contextConfig.appName = "RenderVerseX";
            if (!m_context->Initialize(contextConfig, surface))
            {
                m_context.reset();
                result.code = RenderRuntimeCode::DeviceCreationFailed;
                result.message = "Render device creation failed";
                return result;
            }
            if (!m_context->CreateSwapChain(surface))
            {
                m_context->Shutdown();
                m_context.reset();
                result.code = RenderRuntimeCode::SurfaceCreationFailed;
                result.message = "Render surface creation failed";
                return result;
            }

            RenderSubmissionTracker* submissionTracker =
                RenderContextInternalAccess::GetSubmissionTracker(*m_context);
            if (submissionTracker == nullptr ||
                !m_retirementQueue.Initialize(submissionTracker) ||
                !m_resourceRegistry.Initialize(&statusTable,
                                               &m_retirementQueue) ||
                !m_uploadProcessor.Initialize(m_context->GetDevice(),
                                              &statusTable,
                                              &m_resourceRegistry,
                                              submissionTracker))
            {
                m_resourceRegistry.Shutdown();
                m_context->Shutdown();
                m_context.reset();
                result.code = RenderRuntimeCode::DeviceCreationFailed;
                result.message = "Render resource runtime initialization failed";
                return result;
            }

            m_sceneRenderer = std::make_unique<SceneRenderer>();
            m_sceneRenderer->Initialize(m_context.get(),
                                        &m_resourceRegistry,
                                        &m_retirementQueue);
            if (!m_sceneRenderer->IsInitialized())
            {
                m_sceneRenderer.reset();
                m_uploadProcessor.Shutdown();
                m_resourceRegistry.Shutdown();
                m_context->Shutdown();
                m_context.reset();
                result.code = RenderRuntimeCode::DeviceCreationFailed;
                result.message = "Packet renderer initialization failed";
                return result;
            }
            m_sceneRenderer->SetSurfaceCompatibilityKey(surface.generation);

            result.code = RenderRuntimeCode::Running;
            return result;
        }

        RenderRuntimeResult ApplySurface(
            const NativeSurfaceDesc& surface) override
        {
            RenderRuntimeResult result;
            result.code = RenderRuntimeCode::Running;
            result.backend = m_context != nullptr && m_context->GetDevice() != nullptr
                                 ? m_context->GetDevice()->GetBackendType()
                                 : RHIBackendType::None;
            result.surfaceGeneration = surface.generation;
            if (m_context == nullptr || m_context->GetDevice() == nullptr ||
                !surface.IsValidFor(result.backend))
            {
                result.code = RenderRuntimeCode::InvalidSurface;
                result.message = "Surface update is invalid";
                return result;
            }

            const NativeSurfaceUpdateKind updateKind =
                ClassifyNativeSurfaceUpdate(m_context->GetSurface(), surface);
            bool applied = false;
            if (updateKind == NativeSurfaceUpdateKind::Resize)
            {
                if (!m_context->WaitForSurfaceGeneration())
                {
                    result.code = RenderRuntimeCode::DeviceLost;
                    result.message = "Surface generation completion was lost";
                    return result;
                }
                if (m_sceneRenderer != nullptr)
                {
                    m_sceneRenderer->PrepareForSwapChainResize();
                }
                static_cast<void>(m_retirementQueue.Poll());
                applied = m_context->ResizeSwapChain(surface);
            }
            else if (updateKind == NativeSurfaceUpdateKind::Replace &&
                     m_context->GetDevice()->SupportsSurfaceRebind(
                         m_context->GetSurface(), surface))
            {
                if (!m_context->WaitForSurfaceGeneration())
                {
                    result.code = RenderRuntimeCode::DeviceLost;
                    result.message = "Surface generation completion was lost";
                    return result;
                }
                if (m_sceneRenderer != nullptr)
                {
                    m_sceneRenderer->PrepareForSwapChainResize();
                }
                static_cast<void>(m_retirementQueue.Poll());
                applied = m_context->CreateSwapChain(surface);
            }
            if (!applied)
            {
                result.code = RenderRuntimeCode::SurfaceCreationFailed;
                result.message = "Surface update could not be applied";
                return result;
            }
            m_sceneRenderer->SetSurfaceCompatibilityKey(surface.generation);
            // Accepted packets retain the prior surface's viewport and attachments.
            // Clear the new surface until a packet for this generation arrives.
            return PresentDeterministicClear(surface.generation);
        }

        void ProcessRelease(RenderResourceHandle handle) override
        {
            m_uploadProcessor.ProcessRelease(handle);
        }

        void ProcessUpload(ResourceUploadRequestRef request) override
        {
            static_cast<void>(
                m_uploadProcessor.ProcessUpload(std::move(request)));
        }

        RenderRuntimeResult ConsumeFrame(
            const RenderFramePacket& packet) override
        {
            RenderRuntimeResult result;
            result.code = RenderRuntimeCode::Running;
            result.frameSequence = packet.GetHeader().sequence;
            if (m_context == nullptr || !m_context->HasSwapChain())
            {
                result.code = RenderRuntimeCode::SurfaceCreationFailed;
                result.message = "Cannot consume a frame without a surface";
                return result;
            }

            if (m_sceneRenderer == nullptr)
            {
                result.code = RenderRuntimeCode::OwnershipViolation;
                result.message = "Packet renderer is unavailable";
                return result;
            }

            const RenderFrameApplyResult applyResult =
                m_sceneRenderer->ApplyFramePacket(packet,
                                                  m_resourceRegistry);
            if (!applyResult.IsApplied())
            {
                result.code = RenderRuntimeCode::RenderGraphValidationFailed;
                result.message =
                    "Immutable frame packet was rejected before recording, code=" +
                    std::to_string(static_cast<uint32>(applyResult.code));
                return result;
            }

            return PresentAcceptedFrame(
                packet.GetHeader().sequence,
                m_context->GetSurface().generation,
                packet.GetCaptureRequest());
        }

        void PollCompletion() override
        {
            static_cast<void>(m_uploadProcessor.PollCompletion());
        }

        void RetireCompleted() override
        {
            static_cast<void>(m_retirementQueue.Poll());
        }

        void PopulateDiagnostics(
            RenderDiagnosticsSnapshot& outDiagnostics) const override
        {
            outDiagnostics.lastCapture = m_lastCaptureResult;
            if (m_context != nullptr && m_context->GetDevice() != nullptr)
            {
                const RHICapabilities& capabilities =
                    m_context->GetDevice()->GetCapabilities();
                outDiagnostics.adapterName = capabilities.adapterName;
                outDiagnostics.driverVersion = capabilities.driverVersion;
            }
            if (m_sceneRenderer == nullptr)
            {
                return;
            }

            const SceneRendererFrameDiagnostics frame =
                m_sceneRenderer->GetFrameDiagnostics();
            const SceneEnvironmentIBLStats ibl =
                m_sceneRenderer->GetEnvironmentIBLStats();
            const SceneRayTracingFrameStats rayTracing =
                m_sceneRenderer->GetRayTracingFrameStats();
            RenderFrameFeatureDiagnostics features;
            features.available = true;
            features.frameSequence =
                m_sceneRenderer->GetLastPresentedFrameSequence();
            features.renderAttempted = frame.renderAttempted;
            features.rendered = frame.rendered;
            features.graphBuilt = frame.graphBuilt;
            features.graphCompiled = frame.graphCompiled;
            features.renderGraphTotalPasses = frame.renderGraphTotalPasses;
            features.visibleObjectCount =
                static_cast<uint32>(frame.visibleObjectCount);
            features.renderSceneLightCount =
                static_cast<uint32>(frame.renderSceneLightCount);
            features.requestedPostProcessEffectCount =
                frame.requestedPostProcessEffectCount;
            features.enabledPostProcessEffectCount =
                frame.enabledPostProcessEffectCount;
            features.unsupportedPostProcessSkippedCount =
                frame.unsupportedPostProcessSkippedCount;
            features.postProcessGraphPassCount =
                frame.postProcessGraphPassCount;
            features.clusteredLightingInitialized =
                frame.clusteredLightingInitialized;
            features.clusteredLightingActiveClusters =
                frame.clusteredLightingActiveClusters;
            features.textureIBLEnabled = ibl.textureIBLEnabled;

            for (const RenderPassStatus& status : frame.passStatuses)
            {
                if (status.name == "SkyboxPass")
                {
                    features.skybox.requested = status.requestedEnabled;
                    features.skybox.supported = status.supported;
                    features.skybox.enabled = status.enabled;
                    features.skybox.reason = status.unsupportedReason;
                }
            }
            if (PipelineCache* pipelineCache =
                    m_sceneRenderer->GetPipelineCache())
            {
                const DirectionalShadowFrameBindingResult& shadow =
                    pipelineCache->GetLastDirectionalShadowFrameBindingResult();
                features.directionalShadow.samplingEnabled =
                    shadow.shadowSamplingEnabled &&
                    shadow.fallbackReason ==
                        DirectionalShadowFallbackReason::None;
                features.directionalShadow.reason =
                    PipelineCache::GetDirectionalShadowFallbackReasonName(
                        shadow.fallbackReason);
            }

            const SceneGPUDrivenCullingStats& gpuCulling =
                frame.gpuDrivenCullingStats;
#define RVX_COPY_GPU_CULLING_FIELD(name) \
            features.gpuDrivenCulling.name = gpuCulling.name
            RVX_COPY_GPU_CULLING_FIELD(policyDecisionAvailable);
            RVX_COPY_GPU_CULLING_FIELD(policyDecision);
            RVX_COPY_GPU_CULLING_FIELD(enabled);
            RVX_COPY_GPU_CULLING_FIELD(graphPassAdded);
            RVX_COPY_GPU_CULLING_FIELD(graphPassRecorded);
            RVX_COPY_GPU_CULLING_FIELD(gpuCullingGraphPassCount);
            RVX_COPY_GPU_CULLING_FIELD(gpuExecutionRecorded);
            RVX_COPY_GPU_CULLING_FIELD(graphInputDrawItemCount);
            RVX_COPY_GPU_CULLING_FIELD(visibilityCandidateCount);
            RVX_COPY_GPU_CULLING_FIELD(cpuVisibleCandidateCount);
            RVX_COPY_GPU_CULLING_FIELD(passVisibilityCandidateCount);
            RVX_COPY_GPU_CULLING_FIELD(gpuPlannedVisibilityCandidateCount);
            RVX_COPY_GPU_CULLING_FIELD(invalidVisibilityBoundsCount);
            RVX_COPY_GPU_CULLING_FIELD(gpuDeferredVisibilityCandidateCount);
            RVX_COPY_GPU_CULLING_FIELD(gpuVisibilityReadbackPerformed);
            RVX_COPY_GPU_CULLING_FIELD(occlusionRequestedButUnavailable);
            RVX_COPY_GPU_CULLING_FIELD(gpuVisibilityCountsAvailable);
            RVX_COPY_GPU_CULLING_FIELD(visibleCullableDrawItemCount);
            RVX_COPY_GPU_CULLING_FIELD(frustumCulledDrawItemCount);
            RVX_COPY_GPU_CULLING_FIELD(distanceCulledDrawItemCount);
            RVX_COPY_GPU_CULLING_FIELD(cpuReferenceVisibleCullableDrawItemCount);
            RVX_COPY_GPU_CULLING_FIELD(cpuReferenceCulledDrawItemCount);
            RVX_COPY_GPU_CULLING_FIELD(skippedMissingGpuDataCount);
            RVX_COPY_GPU_CULLING_FIELD(opaqueIndirectRequested);
            RVX_COPY_GPU_CULLING_FIELD(opaqueCullingReady);
            RVX_COPY_GPU_CULLING_FIELD(opaquePipelineReady);
            RVX_COPY_GPU_CULLING_FIELD(opaqueIndirectEligible);
            RVX_COPY_GPU_CULLING_FIELD(opaqueIndirectSubmitted);
            RVX_COPY_GPU_CULLING_FIELD(opaqueDirectDrawCount);
            RVX_COPY_GPU_CULLING_FIELD(opaqueGpuDrivenIndirectBatchCount);
            RVX_COPY_GPU_CULLING_FIELD(opaqueGpuDrivenIndirectSubmittedDrawUpperBound);
            RVX_COPY_GPU_CULLING_FIELD(opaqueGpuDrivenExecutedDrawCountAvailable);
            RVX_COPY_GPU_CULLING_FIELD(opaqueGpuDrivenIndirectDrawCount);
            RVX_COPY_GPU_CULLING_FIELD(opaqueFallbackReason);
#undef RVX_COPY_GPU_CULLING_FIELD
            features.policy = frame.policy;
            features.gpuScene = m_sceneRenderer->GetGPUSceneDiagnostics();

            const ParticleFeaturePassStats& particles =
                m_sceneRenderer->GetParticleFeaturePassStats();
            features.particles.requested = particles.requested;
            features.particles.supported = particles.supported;
            features.particles.enabled = particles.enabled;
            features.particles.graphPassScheduled =
                particles.graphPassScheduled;
            features.particles.drawSubmitted = particles.drawSubmitted;
            features.particles.itemCount = particles.itemCount;
            features.particles.renderPayloadReadyItemCount =
                particles.renderPayloadReadyItemCount;
            features.particles.totalAliveParticles =
                particles.totalAliveParticles;
            features.particles.reason = particles.unsupportedReason;

            if (MaterialSystem* materials =
                    m_sceneRenderer->GetMaterialSystem())
            {
                const MaterialBindingResult& binding =
                    materials->GetLastBindingResult();
                features.material.ready =
                    binding.status == MaterialBindingStatus::Ready;
                features.material.usedFallback = binding.usedFallback;
                features.material.constantsUpdated =
                    binding.constantsUpdated;
                features.material.descriptorSetAvailable =
                    binding.descriptorSet != nullptr;
                features.material.textureFlags = binding.textureFlags;
                features.material.requiredTextureFlags =
                    static_cast<uint32>(MaterialTextureFlags::HasBaseColor) |
                    static_cast<uint32>(MaterialTextureFlags::HasNormal) |
                    static_cast<uint32>(
                        MaterialTextureFlags::HasMetallicRoughness) |
                    static_cast<uint32>(MaterialTextureFlags::HasOcclusion) |
                    static_cast<uint32>(MaterialTextureFlags::HasEmissive);
                features.material.materialName = binding.materialName;
                features.material.message = binding.message;
            }

#define RVX_COPY_RAY_TRACING_FIELD(name) \
            features.rayTracing.name = rayTracing.name
            RVX_COPY_RAY_TRACING_FIELD(scenePrepared);
            RVX_COPY_RAY_TRACING_FIELD(tlasAvailable);
            RVX_COPY_RAY_TRACING_FIELD(shadowRequested);
            RVX_COPY_RAY_TRACING_FIELD(shadowSupported);
            RVX_COPY_RAY_TRACING_FIELD(shadowRecorded);
            RVX_COPY_RAY_TRACING_FIELD(reflectionRequested);
            RVX_COPY_RAY_TRACING_FIELD(reflectionSupported);
            RVX_COPY_RAY_TRACING_FIELD(reflectionRecorded);
            RVX_COPY_RAY_TRACING_FIELD(reflectionDenoiseRequested);
            RVX_COPY_RAY_TRACING_FIELD(reflectionDenoiseSupported);
            RVX_COPY_RAY_TRACING_FIELD(reflectionDenoiseRecorded);
            RVX_COPY_RAY_TRACING_FIELD(reflectionCompositeRequested);
            RVX_COPY_RAY_TRACING_FIELD(reflectionCompositeSupported);
            RVX_COPY_RAY_TRACING_FIELD(reflectionCompositeRecorded);
            RVX_COPY_RAY_TRACING_FIELD(reflectionMaterialTextureTableAvailable);
            RVX_COPY_RAY_TRACING_FIELD(reflectionGeometryMetadataAvailable);
            RVX_COPY_RAY_TRACING_FIELD(reflectionGeometryTableAvailable);
            RVX_COPY_RAY_TRACING_FIELD(shadowHistoryAvailable);
            RVX_COPY_RAY_TRACING_FIELD(shadowDepthHistoryAvailable);
            RVX_COPY_RAY_TRACING_FIELD(shadowNormalHistoryAvailable);
            RVX_COPY_RAY_TRACING_FIELD(shadowHistoryReset);
            RVX_COPY_RAY_TRACING_FIELD(shadowHistoryRecreated);
            RVX_COPY_RAY_TRACING_FIELD(shadowHistoryResolutionChanged);
            RVX_COPY_RAY_TRACING_FIELD(shadowHistoryConfigChanged);
            RVX_COPY_RAY_TRACING_FIELD(shadowTemporalAccumulated);
            RVX_COPY_RAY_TRACING_FIELD(shadowMaterialTextureTableAvailable);
            RVX_COPY_RAY_TRACING_FIELD(shadowAlphaMetadataAvailable);
            RVX_COPY_RAY_TRACING_FIELD(shadowAlphaTextureTableAvailable);
            RVX_COPY_RAY_TRACING_FIELD(shadowAlphaGeometryTableAvailable);
            RVX_COPY_RAY_TRACING_FIELD(reflectionHistoryAvailable);
            RVX_COPY_RAY_TRACING_FIELD(reflectionDepthHistoryAvailable);
            RVX_COPY_RAY_TRACING_FIELD(reflectionNormalHistoryAvailable);
            RVX_COPY_RAY_TRACING_FIELD(reflectionHistoryReset);
            RVX_COPY_RAY_TRACING_FIELD(reflectionHistoryRecreated);
            RVX_COPY_RAY_TRACING_FIELD(reflectionHistoryResolutionChanged);
            RVX_COPY_RAY_TRACING_FIELD(reflectionHistoryConfigChanged);
            RVX_COPY_RAY_TRACING_FIELD(reflectionTemporalAccumulated);
            RVX_COPY_RAY_TRACING_FIELD(denoiseFallbackToRaw);
            RVX_COPY_RAY_TRACING_FIELD(shadowGpuTimingSupported);
            RVX_COPY_RAY_TRACING_FIELD(shadowGpuTimingQueriesRecorded);
            RVX_COPY_RAY_TRACING_FIELD(shadowGpuTimingResolveRecorded);
            RVX_COPY_RAY_TRACING_FIELD(shadowGpuTimingReadbackBufferAvailable);
            RVX_COPY_RAY_TRACING_FIELD(shadowGpuTimingResultAvailable);
            RVX_COPY_RAY_TRACING_FIELD(reflectionGpuTimingSupported);
            RVX_COPY_RAY_TRACING_FIELD(reflectionGpuTimingQueriesRecorded);
            RVX_COPY_RAY_TRACING_FIELD(reflectionGpuTimingResolveRecorded);
            RVX_COPY_RAY_TRACING_FIELD(reflectionGpuTimingReadbackBufferAvailable);
            RVX_COPY_RAY_TRACING_FIELD(reflectionGpuTimingResultAvailable);
            RVX_COPY_RAY_TRACING_FIELD(budgetEnabled);
            RVX_COPY_RAY_TRACING_FIELD(budgetApplied);
            RVX_COPY_RAY_TRACING_FIELD(rayBudgetExceeded);
            RVX_COPY_RAY_TRACING_FIELD(denoiseTapBudgetExceeded);
            RVX_COPY_RAY_TRACING_FIELD(resourceBudgetExceeded);
            RVX_COPY_RAY_TRACING_FIELD(resourceBudgetEvictionAttempted);
            RVX_COPY_RAY_TRACING_FIELD(resourceByteAccountingOverflowed);
            RVX_COPY_RAY_TRACING_FIELD(gpuTimeBudgetExceeded);
            RVX_COPY_RAY_TRACING_FIELD(gpuTimeBudgetApplied);
            RVX_COPY_RAY_TRACING_FIELD(shadowGpuTimeBudgetExceeded);
            RVX_COPY_RAY_TRACING_FIELD(shadowGpuTimeBudgetApplied);
            RVX_COPY_RAY_TRACING_FIELD(reflectionGpuTimeBudgetExceeded);
            RVX_COPY_RAY_TRACING_FIELD(reflectionGpuTimeBudgetApplied);
            RVX_COPY_RAY_TRACING_FIELD(measuredGpuTimeAvailable);
            RVX_COPY_RAY_TRACING_FIELD(rayBudget);
            RVX_COPY_RAY_TRACING_FIELD(denoiseTapBudget);
            RVX_COPY_RAY_TRACING_FIELD(trackedResourceBudget);
            RVX_COPY_RAY_TRACING_FIELD(estimatedShadowRayCount);
            RVX_COPY_RAY_TRACING_FIELD(estimatedReflectionRayCount);
            RVX_COPY_RAY_TRACING_FIELD(estimatedTotalRayCount);
            RVX_COPY_RAY_TRACING_FIELD(estimatedReflectionDenoiseTapCount);
            RVX_COPY_RAY_TRACING_FIELD(shadowGpuTimestampFrequency);
            RVX_COPY_RAY_TRACING_FIELD(reflectionGpuTimestampFrequency);
            RVX_COPY_RAY_TRACING_FIELD(shadowGpuTimingElapsedMs);
            RVX_COPY_RAY_TRACING_FIELD(reflectionGpuTimingElapsedMs);
            RVX_COPY_RAY_TRACING_FIELD(gpuTimeBudget);
            RVX_COPY_RAY_TRACING_FIELD(shadowGpuTimeBudget);
            RVX_COPY_RAY_TRACING_FIELD(reflectionGpuTimeBudget);
            RVX_COPY_RAY_TRACING_FIELD(measuredGpuTimeForBudgetMs);
            RVX_COPY_RAY_TRACING_FIELD(measuredShadowGpuTimeForBudgetMs);
            RVX_COPY_RAY_TRACING_FIELD(measuredReflectionGpuTimeForBudgetMs);
            RVX_COPY_RAY_TRACING_FIELD(gpuTimeBudgetQualityScale);
            RVX_COPY_RAY_TRACING_FIELD(shadowGpuTimeBudgetQualityScale);
            RVX_COPY_RAY_TRACING_FIELD(reflectionGpuTimeBudgetQualityScale);
            RVX_COPY_RAY_TRACING_FIELD(gpuTimeBudgetOverBudgetFrameCount);
            RVX_COPY_RAY_TRACING_FIELD(reflectionGpuTimeBudgetOverBudgetFrameCount);
            RVX_COPY_RAY_TRACING_FIELD(reflectionGpuTimeBudgetUnderBudgetFrameCount);
            RVX_COPY_RAY_TRACING_FIELD(blasCacheEvictionFrameThreshold);
            features.rayTracing.cachedBLASCount =
                static_cast<uint32>(rayTracing.cachedBLASCount);
            features.rayTracing.evictedBLASCount =
                static_cast<uint32>(rayTracing.evictedBLASCount);
            features.rayTracing.resourceBudgetEvictedBLASCount =
                static_cast<uint32>(rayTracing.resourceBudgetEvictedBLASCount);
            features.rayTracing.releasedBLASScratchCount =
                static_cast<uint32>(rayTracing.releasedBLASScratchCount);
            features.rayTracing.pendingBLASScratchReleaseCount =
                static_cast<uint32>(rayTracing.pendingBLASScratchReleaseCount);
            RVX_COPY_RAY_TRACING_FIELD(cachedBLASAccelerationStructureBytes);
            RVX_COPY_RAY_TRACING_FIELD(cachedBLASScratchBytes);
            RVX_COPY_RAY_TRACING_FIELD(releasedBLASScratchBytes);
            RVX_COPY_RAY_TRACING_FIELD(topLevelAccelerationStructureBytes);
            RVX_COPY_RAY_TRACING_FIELD(topLevelScratchBytes);
            RVX_COPY_RAY_TRACING_FIELD(instanceBufferBytes);
            RVX_COPY_RAY_TRACING_FIELD(materialMetadataBufferBytes);
            RVX_COPY_RAY_TRACING_FIELD(alphaMetadataBufferBytes);
            RVX_COPY_RAY_TRACING_FIELD(totalTrackedResourceBytes);
            RVX_COPY_RAY_TRACING_FIELD(shadowMaterialTextureCount);
            RVX_COPY_RAY_TRACING_FIELD(shadowMaterialTexturesBound);
            RVX_COPY_RAY_TRACING_FIELD(shadowAlphaTextureCount);
            RVX_COPY_RAY_TRACING_FIELD(shadowAlphaTexturesBound);
            RVX_COPY_RAY_TRACING_FIELD(shadowAlphaIndexBufferCount);
            RVX_COPY_RAY_TRACING_FIELD(shadowAlphaUVBufferCount);
            RVX_COPY_RAY_TRACING_FIELD(reflectionMaterialTextureCount);
            RVX_COPY_RAY_TRACING_FIELD(reflectionMaterialTexturesBound);
            RVX_COPY_RAY_TRACING_FIELD(reflectionGeometryIndexBufferCount);
            RVX_COPY_RAY_TRACING_FIELD(reflectionGeometryUVBufferCount);
            RVX_COPY_RAY_TRACING_FIELD(reflectionGeometryNormalBufferCount);
            RVX_COPY_RAY_TRACING_FIELD(reflectionGeometryTangentBufferCount);
            RVX_COPY_RAY_TRACING_FIELD(requestedReflectionResolutionScale);
            RVX_COPY_RAY_TRACING_FIELD(reflectionResolutionScale);
            RVX_COPY_RAY_TRACING_FIELD(requestedShadowSamplesPerPixel);
            RVX_COPY_RAY_TRACING_FIELD(requestedReflectionSamplesPerPixel);
            RVX_COPY_RAY_TRACING_FIELD(requestedReflectionDenoiseRadius);
            RVX_COPY_RAY_TRACING_FIELD(shadowSamplesPerPixel);
            RVX_COPY_RAY_TRACING_FIELD(reflectionSamplesPerPixel);
            RVX_COPY_RAY_TRACING_FIELD(reflectionDenoiseRadius);
            RVX_COPY_RAY_TRACING_FIELD(shadowWidth);
            RVX_COPY_RAY_TRACING_FIELD(shadowHeight);
            RVX_COPY_RAY_TRACING_FIELD(reflectionWidth);
            RVX_COPY_RAY_TRACING_FIELD(reflectionHeight);
#undef RVX_COPY_RAY_TRACING_FIELD
            features.gpuMemoryBudget = 0;
            features.gpuUsedMemory = frame.gpuResourceStats.usedMemory;
            features.residentMeshCount = static_cast<uint32>(
                frame.gpuResourceStats.residentMeshCount);
            features.residentTextureCount = static_cast<uint32>(
                frame.gpuResourceStats.residentTextureCount);
            features.pendingUploadCount = static_cast<uint32>(
                frame.gpuResourceStats.pendingUploadCount);
            features.queuedUploadCount = 0;
            features.failedUploadCount = 0;

            const auto appendReason = [&features](const std::string& reason)
            {
                if (!reason.empty())
                {
                    features.fallbackReasons.push_back(reason);
                }
            };
            appendReason(frame.skippedReason);
            appendReason(frame.hdrFallbackReason);
            appendReason(frame.localShadowFallbackReason);
            appendReason(frame.clusteredLightingFallbackReason);
            appendReason(frame.externalTargetFallbackReason);
            appendReason(frame.postProcessToneMappingBoundaryWarning);
            appendReason(ibl.fallbackReason);
            for (const RenderPassStatus& status : frame.passStatuses)
            {
                if (status.requestedEnabled && !status.supported &&
                    !status.unsupportedReason.empty())
                {
                    features.unsupportedFeatures.push_back(
                        status.name + ": " + status.unsupportedReason);
                }
            }
            outDiagnostics.frameFeatures = std::move(features);
        }

        RenderRuntimeResult QueryRuntimeStatus() const override
        {
            return MakeDeviceRuntimeResult();
        }

        RenderShutdownResult Shutdown(
            RenderTeardownMode mode) noexcept override
        {
            RenderShutdownResult result;
            result.code = RenderShutdownCode::Completed;
            try
            {
                if (m_context != nullptr)
                {
                    if (m_context->GetDevice() != nullptr)
                    {
                        result.backend =
                            m_context->GetDevice()->GetBackendType();
                    }
                    result.surfaceGeneration =
                        m_context->GetSurface().generation;
                    const RHIDeviceFault fault =
                        m_context->GetDevice() != nullptr
                            ? m_context->GetDevice()->GetLastDeviceFault()
                            : RHIDeviceFault{};
                    const bool bypassCompletion =
                        mode != RenderTeardownMode::NormalDrain;
                    RenderSubmissionTracker* submissionTracker =
                        RenderContextInternalAccess::GetSubmissionTracker(
                            *m_context);
                    if (bypassCompletion && submissionTracker != nullptr)
                    {
                        submissionTracker->MarkDeviceLost();
                    }
                    if (!bypassCompletion)
                    {
                        m_context->WaitIdle();
                    }
                    if (m_sceneRenderer != nullptr)
                    {
                        m_sceneRenderer->Shutdown();
                        m_sceneRenderer.reset();
                    }
                    if (bypassCompletion)
                    {
                        m_uploadProcessor.ShutdownDeviceLost();
                    }
                    else
                    {
                        static_cast<void>(m_uploadProcessor.PollCompletion());
                        m_uploadProcessor.Shutdown();
                    }
                    m_resourceRegistry.Shutdown();
                    if (bypassCompletion)
                    {
                        static_cast<void>(
                            m_retirementQueue.ForceDeviceLostTeardown());
                    }
                    else
                    {
                        static_cast<void>(m_retirementQueue.Poll());
                        if (m_retirementQueue.GetDiagnostics().entryCount != 0)
                        {
                            static_cast<void>(
                                m_retirementQueue.ForceDeviceLostTeardown());
                        }
                    }
                    // The normal path already waited above; the failure path
                    // must never issue a device-wide wait.
                    m_context->Shutdown(false);
                    m_context.reset();

                    if (mode == RenderTeardownMode::DeviceLostTeardown)
                    {
                        result.code = RenderShutdownCode::DeviceLost;
                        result.nativeError = fault.nativeError;
                        result.message = fault.message.empty()
                                             ? "Device-lost teardown completed without waiting for GPU progress"
                                             : fault.message;
                    }
                }
            }
            catch (const std::exception& exception)
            {
                result.code = RenderShutdownCode::ExecutorJoinFailed;
                result.message =
                    std::string("Render consumer cleanup threw: ") +
                    exception.what();
            }
            catch (...)
            {
                result.code = RenderShutdownCode::ExecutorJoinFailed;
                result.message = "Render consumer cleanup threw";
            }
            return result;
        }

    private:
        struct CaptureReadback
        {
            RHIBufferRef buffer{};
            uint64 byteSize = 0;
            bool recorded = false;
        };

        RenderRuntimeResult PresentAcceptedFrame(
            uint64 frameSequence,
            uint64 surfaceGeneration,
            const RenderFrameCaptureRequest& captureRequest)
        {
            RenderRuntimeResult result;
            result.code = RenderRuntimeCode::Running;
            result.backend = m_context != nullptr &&
                                     m_context->GetDevice() != nullptr
                                 ? m_context->GetDevice()->GetBackendType()
                                 : RHIBackendType::None;
            result.frameSequence = frameSequence;
            result.surfaceGeneration = surfaceGeneration;
            if (m_context == nullptr || m_sceneRenderer == nullptr)
            {
                result.code = RenderRuntimeCode::OwnershipViolation;
                result.message = "Accepted frame has no render context or scene renderer";
                return result;
            }
            if (!m_context->BeginFrame())
            {
                m_sceneRenderer->ReleaseUnsubmittedFrame();
                RenderRuntimeResult health = MakeDeviceRuntimeResult();
                health.frameSequence = frameSequence;
                health.surfaceGeneration = surfaceGeneration;
                if (health.code != RenderRuntimeCode::Running)
                {
                    return health;
                }
                result.code = RenderRuntimeCode::RenderGraphValidationFailed;
                result.message =
                    "Accepted frame could not acquire a renderable frame slot";
                return result;
            }

            RenderFrameExecutionResult executionResult =
                m_sceneRenderer->RenderAcceptedFrame();
            if (executionResult.code != RenderFrameExecutionCode::Rendered)
            {
                m_sceneRenderer->ReleaseUnsubmittedFrame();
                m_context->AbortFrame();
                result.code = RenderRuntimeCode::RenderGraphValidationFailed;
                result.message =
                    "Accepted frame failed RenderGraph validation or recording";
                return result;
            }

            CaptureReadback capture = PrepareCapture(
                captureRequest,
                frameSequence);
            const GPUCompletionPoint submittedPoint = m_context->EndFrame();
            if (submittedPoint.value == 0)
            {
                m_sceneRenderer->ReleaseUnsubmittedFrame();
                RenderRuntimeResult health = MakeDeviceRuntimeResult();
                health.frameSequence = frameSequence;
                health.surfaceGeneration = surfaceGeneration;
                if (health.code != RenderRuntimeCode::Running)
                {
                    return health;
                }
                result.code = RenderRuntimeCode::RenderGraphValidationFailed;
                result.message =
                    "Graphics submission did not produce a completion point";
                return result;
            }

            GPUCompletionToken completion;
            if (!InsertGPUCompletionPoint(completion, submittedPoint))
            {
                result.code = RenderRuntimeCode::OwnershipViolation;
                result.message =
                    "Graphics submission point could not be represented by an exact token";
                return result;
            }

            // Submission ownership must be transferred as soon as exact completion
            // evidence exists, even if later registry stamping rejects the frame.
            m_sceneRenderer->NotifySubmission(completion);
            if (!StampReferencedResources(executionResult.referencedResources,
                                          completion))
            {
                result.code = RenderRuntimeCode::OwnershipViolation;
                result.message =
                    "Submitted frame resources could not be stamped by exact generation";
                return result;
            }
            m_context->Present();
            CompleteCapture(capture);
            RenderRuntimeResult health = MakeDeviceRuntimeResult();
            health.frameSequence = frameSequence;
            health.surfaceGeneration = surfaceGeneration;
            if (health.code != RenderRuntimeCode::Running)
            {
                return health;
            }
            m_sceneRenderer->MarkAcceptedFramePresented();
            return result;
        }

        RenderRuntimeResult PresentDeterministicClear(
            uint64 surfaceGeneration)
        {
            RenderRuntimeResult result;
            result.code = RenderRuntimeCode::Running;
            result.backend = m_context != nullptr &&
                                     m_context->GetDevice() != nullptr
                                 ? m_context->GetDevice()->GetBackendType()
                                 : RHIBackendType::None;
            result.surfaceGeneration = surfaceGeneration;
            if (m_context == nullptr)
            {
                result.code = RenderRuntimeCode::OwnershipViolation;
                result.message = "Resize redraw has no render context";
                return result;
            }
            if (!m_context->BeginFrame())
            {
                RenderRuntimeResult health = MakeDeviceRuntimeResult();
                health.surfaceGeneration = surfaceGeneration;
                if (health.code != RenderRuntimeCode::Running)
                {
                    return health;
                }
                result.code = RenderRuntimeCode::RenderGraphValidationFailed;
                result.message = "Resize redraw could not acquire a renderable frame slot";
                return result;
            }
            RHICommandContext* commandContext =
                m_context->GetGraphicsContext();
            RHITexture* backBuffer = m_context->GetCurrentBackBuffer();
            RHITextureView* backBufferView =
                m_context->GetCurrentBackBufferView();
            if (commandContext == nullptr || backBuffer == nullptr ||
                backBufferView == nullptr)
            {
                m_context->AbortFrame();
                result.code = RenderRuntimeCode::SurfaceCreationFailed;
                result.message = "Resize redraw has no renderable back buffer";
                return result;
            }
            commandContext->TextureBarrier(backBuffer,
                                           RHIResourceState::Undefined,
                                           RHIResourceState::RenderTarget);
            RHIRenderPassDesc clearPass;
            clearPass.AddColorAttachment(
                backBufferView,
                RHILoadOp::Clear,
                RHIStoreOp::Store,
                RHIClearColor{0.015625f, 0.0234375f, 0.03125f, 1.0f});
            commandContext->BeginRenderPass(clearPass);
            commandContext->EndRenderPass();
            commandContext->TextureBarrier(backBuffer,
                                           RHIResourceState::RenderTarget,
                                           RHIResourceState::Present);
            const GPUCompletionPoint submittedPoint = m_context->EndFrame();
            if (submittedPoint.value == 0)
            {
                RenderRuntimeResult health = MakeDeviceRuntimeResult();
                health.surfaceGeneration = surfaceGeneration;
                if (health.code != RenderRuntimeCode::Running)
                {
                    return health;
                }
                result.code = RenderRuntimeCode::RenderGraphValidationFailed;
                result.message = "Resize redraw submission failed";
                return result;
            }
            m_context->Present();
            return MakeDeviceRuntimeResult();
        }

        RenderRuntimeResult MakeDeviceRuntimeResult() const
        {
            RenderRuntimeResult result;
            result.code = RenderRuntimeCode::Running;
            result.lifecycle = RenderLifecycleState::Running;
            if (m_context == nullptr || m_context->GetDevice() == nullptr)
            {
                return result;
            }

            IRHIDevice* device = m_context->GetDevice();
            result.backend = device->GetBackendType();
            result.surfaceGeneration = m_context->GetSurface().generation;
            const RHIDeviceRuntimeStatus status =
                device->QueryRuntimeStatus();
            if (status == RHIDeviceRuntimeStatus::Ready)
            {
                return result;
            }

            const RHIDeviceFault fault = device->GetLastDeviceFault();
            result.code = RenderRuntimeCode::DeviceLost;
            result.nativeError = fault.nativeError;
            result.message = fault.message;
            if (result.message.empty())
            {
                result.message =
                    status == RHIDeviceRuntimeStatus::DeviceLost
                        ? "RHI backend reported device loss"
                        : "RHI backend reported a terminal runtime error";
            }
            return result;
        }

        bool StampReferencedResources(
            const std::vector<RenderResourceHandle>& resources,
            const GPUCompletionToken& completion)
        {
            return m_resourceRegistry.MergeLastUseClosure(resources,
                                                          completion);
        }

        CaptureReadback PrepareCapture(
            const RenderFrameCaptureRequest& request,
            uint64 frameSequence)
        {
            CaptureReadback readback;
            if (request.kind == RenderFrameCaptureKind::None)
            {
                return readback;
            }
            if (request.requestId == m_lastCaptureResult.requestId &&
                m_lastCaptureResult.code !=
                    RenderFrameCaptureResultCode::None)
            {
                return readback;
            }

            RenderFrameCaptureResult capture;
            capture.requestId = request.requestId;
            capture.frameSequence = frameSequence;
            capture.kind = request.kind;
            capture.width = request.width;
            capture.height = request.height;
            if (request.kind != RenderFrameCaptureKind::Color)
            {
                capture.code =
                    RenderFrameCaptureResultCode::UnsupportedKind;
                capture.message =
                    "Only color capture is implemented by the M1 runtime";
                m_lastCaptureResult = std::move(capture);
                return readback;
            }

            RHITexture* backBuffer = m_context->GetCurrentBackBuffer();
            RHICommandContext* commandContext =
                m_context->GetGraphicsContext();
            if (backBuffer == nullptr || commandContext == nullptr ||
                request.width != backBuffer->GetWidth() ||
                request.height != backBuffer->GetHeight())
            {
                capture.code =
                    RenderFrameCaptureResultCode::UnsupportedExtent;
                capture.message =
                    "Capture extent must match the current render surface";
                m_lastCaptureResult = std::move(capture);
                return readback;
            }

            capture.format = backBuffer->GetFormat();
            capture.bytesPerPixel =
                GetFormatBytesPerPixel(capture.format);
            const bool rgba8 = capture.format == RHIFormat::RGBA8_UNORM ||
                               capture.format == RHIFormat::RGBA8_UNORM_SRGB ||
                               capture.format == RHIFormat::BGRA8_UNORM ||
                               capture.format == RHIFormat::BGRA8_UNORM_SRGB;
            if (!rgba8 || capture.bytesPerPixel != 4)
            {
                capture.code =
                    RenderFrameCaptureResultCode::UnsupportedFormat;
                capture.message =
                    "Color capture requires an RGBA8 or BGRA8 surface";
                m_lastCaptureResult = std::move(capture);
                return readback;
            }

            const RHIBackendType backend =
                m_context->GetDevice()->GetBackendType();
            const uint64 tightPitch =
                static_cast<uint64>(request.width) *
                capture.bytesPerPixel;
            const uint64 alignment =
                backend == RHIBackendType::DX12 ||
                        backend == RHIBackendType::Metal
                    ? 256U
                    : 1U;
            const uint64 rowPitch =
                (tightPitch + alignment - 1U) & ~(alignment - 1U);
            if (rowPitch > std::numeric_limits<uint32>::max() ||
                request.height >
                    std::numeric_limits<uint64>::max() / rowPitch)
            {
                capture.code =
                    RenderFrameCaptureResultCode::UnsupportedExtent;
                capture.message = "Capture byte size overflow";
                m_lastCaptureResult = std::move(capture);
                return readback;
            }

            capture.rowPitch = static_cast<uint32>(rowPitch);
            capture.originBottomLeft =
                backend == RHIBackendType::OpenGL;
            readback.byteSize = rowPitch * request.height;

            RHIBufferDesc bufferDesc;
            bufferDesc.size = readback.byteSize;
            bufferDesc.usage = RHIBufferUsage::CopyDst;
            bufferDesc.memoryType = RHIMemoryType::Readback;
            bufferDesc.debugName = "RenderFrameCaptureReadback";
            readback.buffer =
                m_context->GetDevice()->CreateBuffer(bufferDesc);
            if (!readback.buffer)
            {
                capture.code =
                    RenderFrameCaptureResultCode::ResourceCreationFailed;
                capture.message = "Capture readback allocation failed";
                m_lastCaptureResult = std::move(capture);
                return readback;
            }

            RHIBufferTextureCopyDesc copyDesc;
            copyDesc.bufferRowPitch = capture.rowPitch;
            copyDesc.textureRegion =
                {0, 0, request.width, request.height};
            commandContext->TextureBarrier(
                backBuffer,
                RHIResourceState::Present,
                RHIResourceState::CopySource);
            commandContext->CopyTextureToBuffer(
                backBuffer,
                readback.buffer.Get(),
                copyDesc);
            commandContext->TextureBarrier(
                backBuffer,
                RHIResourceState::CopySource,
                RHIResourceState::Present);
            readback.recorded = true;
            m_lastCaptureResult = std::move(capture);
            return readback;
        }

        void CompleteCapture(CaptureReadback& readback)
        {
            if (!readback.recorded || !readback.buffer)
            {
                return;
            }

            m_context->WaitIdle();
            const void* mapped = readback.buffer->Map();
            if (mapped == nullptr)
            {
                m_lastCaptureResult.code =
                    RenderFrameCaptureResultCode::MapFailed;
                m_lastCaptureResult.message =
                    "Capture readback mapping failed";
                return;
            }
            const auto* first = static_cast<const uint8*>(mapped);
            m_lastCaptureResult.bytes.assign(
                first,
                first + readback.byteSize);
            readback.buffer->Unmap();
            m_lastCaptureResult.code =
                RenderFrameCaptureResultCode::Completed;
            m_lastCaptureResult.message.clear();
        }

        std::unique_ptr<RenderContext> m_context;
        std::unique_ptr<SceneRenderer> m_sceneRenderer;
        RenderRetirementQueue m_retirementQueue;
        RenderResourceRegistry m_resourceRegistry;
        RenderUploadProcessor m_uploadProcessor;
        RenderFrameCaptureResult m_lastCaptureResult{};
    };

    class ClearPresentRuntimeFactory final : public IRenderRuntimeFactory,
                                             public NonMovable
    {
    public:
        [[nodiscard]] std::unique_ptr<IRenderFrameConsumer>
            CreateFrameConsumer() override
        {
            return std::make_unique<ClearPresentFrameConsumer>();
        }
    };
} // namespace

RenderSubsystem::RenderSubsystem() = default;
RenderSubsystem::~RenderSubsystem() = default;

void RenderSubsystem::Initialize()
{
    if (m_initializeAttempted)
    {
        if (m_runtimeConfigured)
        {
            const RenderRuntimeResult result = GetLastRuntimeResult();
            if (result.code != RenderRuntimeCode::Running)
            {
                throw RenderSubsystemInitializationError(result);
            }
        }
        return;
    }
    m_initializeAttempted = true;

    if (!m_runtimeConfigured || m_runtime == nullptr)
    {
        throw std::logic_error(
            "RenderSubsystem requires Configure before Initialize");
    }
    const RenderRuntimeResult result = m_runtime->Start();
    m_preRuntimeResult = result;
    if (result.code != RenderRuntimeCode::Running)
    {
        throw RenderSubsystemInitializationError(result);
    }
}

void RenderSubsystem::Configure(const RenderRuntimeConfig& config,
                                const NativeSurfaceDesc& surface)
{
    if (m_initializeAttempted)
    {
        throw std::logic_error(
            "RenderSubsystem::Configure is only valid before Initialize");
    }

    auto runtime = std::make_unique<RenderThreadRuntime>(
        config,
        surface,
        RenderExecutorKind::Dedicated,
        CreateDedicatedRenderExecutor(),
        std::make_unique<ClearPresentRuntimeFactory>());
    m_runtime = std::move(runtime);
    m_runtimeConfig = config;
    m_runtimeSurface = surface;
    m_runtimeConfigured = true;
    m_preRuntimeResult = {};
    m_preShutdownResult = {};
}

RenderFramePublishResult RenderSubsystem::TryPublishFrame(
    std::unique_ptr<const RenderFramePacket> packet)
{
    if (m_runtime == nullptr)
    {
        RenderFramePublishResult result;
        result.code = RenderFramePublishCode::NotRunning;
        result.resultClass = ClassifyRenderFramePublishCode(result.code);
        result.sequence = packet != nullptr ? packet->GetHeader().sequence : 0U;
        return result;
    }
    return m_runtime->TryPublishFrame(std::move(packet));
}

RenderFramePublishResult RenderSubsystem::TryPublishFrameSet(
    std::unique_ptr<const RenderSceneUpdateBatch> sceneUpdate,
    std::unique_ptr<const RenderFramePacketV5> frameV5,
    std::unique_ptr<const RenderFramePacket> compatibilityFrame)
{
    if (m_runtime == nullptr)
    {
        RenderFramePublishResult result;
        result.code = RenderFramePublishCode::NotRunning;
        result.resultClass = ClassifyRenderFramePublishCode(result.code);
        return result;
    }
    return m_runtime->TryPublishFrameSet(std::move(sceneUpdate),
                                         std::move(frameV5),
                                         std::move(compatibilityFrame));
}

RenderResizeResult RenderSubsystem::RequestResize(
    const NativeSurfaceDesc& surface)
{
    if (m_runtime == nullptr)
    {
        RenderResizeResult result;
        result.code = RenderResizeCode::NotRunning;
        result.resultClass = ClassifyRenderResizeCode(result.code);
        result.generation = surface.generation;
        return result;
    }
    RenderResizeResult result = m_runtime->RequestResize(surface);
    if (result.code == RenderResizeCode::Accepted ||
        result.code == RenderResizeCode::CoalescedOlder)
    {
        m_runtimeSurface = surface;
    }
    return result;
}

RenderDiagnosticsSnapshot RenderSubsystem::GetDiagnosticsSnapshot() const
{
    return m_runtime != nullptr ? m_runtime->GetDiagnosticsSnapshot()
                                : RenderDiagnosticsSnapshot{};
}

RenderRuntimeResult RenderSubsystem::GetLastRuntimeResult() const
{
    return m_runtime != nullptr ? m_runtime->GetLastRuntimeResult()
                                : m_preRuntimeResult;
}

RenderShutdownResult RenderSubsystem::GetLastShutdownResult() const
{
    return m_runtime != nullptr ? m_runtime->GetLastShutdownResult()
                                : m_preShutdownResult;
}

void RenderSubsystem::Deinitialize()
{
    RVX_CORE_DEBUG("RenderSubsystem deinitializing...");

    if (m_runtime != nullptr)
    {
        m_preShutdownResult = m_runtime->Stop();
    }
    RVX_CORE_INFO("RenderSubsystem dedicated runtime deinitialized");
}

RenderResourceReserveResult RenderSubsystem::ReserveResource(
    AssetId assetId,
    RenderResourceKind kind) noexcept
{
    return m_runtime != nullptr ? m_runtime->ReserveResource(assetId, kind)
                                : RenderResourceReserveResult{};
}

RenderUploadEnqueueResult RenderSubsystem::TryEnqueueUpload(
    const ResourceUploadRequestRef& request) noexcept
{
    return m_runtime != nullptr ? m_runtime->TryEnqueueUpload(request)
                                : RenderUploadEnqueueResult{};
}

RenderReleaseResult RenderSubsystem::RequestRelease(
    RenderResourceHandle handle) noexcept
{
    return m_runtime != nullptr ? m_runtime->RequestRelease(handle)
                                : RenderReleaseResult{};
}

RenderResourceStatus RenderSubsystem::QueryResourceStatus(
    RenderResourceHandle handle) const noexcept
{
    return m_runtime != nullptr ? m_runtime->QueryResourceStatus(handle)
                                : RenderResourceStatus{};
}

bool RenderSubsystem::IsReady() const
{
    return m_runtime != nullptr && m_runtime->IsReady();
}

} // namespace RVX
