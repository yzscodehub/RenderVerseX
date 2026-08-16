/**
 * @file RenderSubsystem.cpp
 * @brief RenderSubsystem implementation
 */

#include "Render/RenderSubsystem.h"
#include "Context/RenderContextInternal.h"
#include "Core/Diagnostics/Trace.h"
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

#include <chrono>
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
            m_startupTraceContext = config.startupTraceContext;
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
            if (!request)
                return;
            const uint64 sequence = request->GetSequence();
            const uint64 sourceRevision = request->GetSourceRevision();
            const uint64 bytes = request->GetDerivedPayloadBytes();
            const RenderResourceKind kind = request->GetKind();
            const RenderUploadProcessCode result =
                m_uploadProcessor.ProcessUpload(std::move(request));
            if (result == RenderUploadProcessCode::Accepted)
            {
                Diagnostics::RecordTraceInstant(
                    m_startupTraceContext,
                    "UploadSubmitted",
                    {{"requestSequence", sequence},
                     {"sourceRevision", sourceRevision},
                     {"bytes", bytes},
                     {"kind", static_cast<uint64>(kind)}});
            }
        }

        RenderRuntimeResult ConsumeFrameV5(
            const RenderFramePacketV5& packet,
            const RenderSceneDatabase& scene) override
        {
            RenderRuntimeResult result;
            result.code = RenderRuntimeCode::Running;
            result.frameSequence = packet.GetHeader().sequence;
            if (m_context == nullptr || !m_context->HasSwapChain())
            {
                result.code = RenderRuntimeCode::SurfaceCreationFailed;
                result.message = "Cannot consume a v5 frame without a surface";
                return result;
            }
            if (m_sceneRenderer == nullptr)
            {
                result.code = RenderRuntimeCode::OwnershipViolation;
                result.message = "Packet renderer is unavailable";
                return result;
            }

            const auto frameApplyStart = std::chrono::steady_clock::now();
            const RenderFrameApplyResult applyResult =
                m_sceneRenderer->ApplyFrameV5(packet,
                                              scene,
                                              m_resourceRegistry);
            RenderCpuFramePhaseDurations phases;
            phases.frameApply = ElapsedNanoseconds(frameApplyStart);
            if (!applyResult.IsApplied())
            {
                result.code = RenderRuntimeCode::RenderGraphValidationFailed;
                result.message =
                    "v5 frame or persistent RenderScene was rejected before recording, code=" +
                    std::to_string(static_cast<uint32>(applyResult.code));
                return result;
            }
            result = PresentAcceptedFrame(
                packet.GetHeader().sequence,
                m_context->GetSurface().generation,
                packet.GetCaptureRequest(),
                phases);
            if (result.code == RenderRuntimeCode::Running)
            {
                RenderCpuFrameTimingDiagnostics timing;
                timing.sourceFrameSequence = packet.GetHeader().sequence;
                timing.requiredSceneRevision =
                    packet.GetHeader().requiredSceneRevision;
                timing.appliedSceneRevision = scene.GetRevision();
                timing.phases = DiagnosticValue<RenderCpuFramePhaseDurations>::Available(
                    phases);
                m_lastCpuFrameTiming = std::move(timing);
            }
            return result;
        }

        bool RequestGPUSceneCullingQualificationCapture() override
        {
            return m_sceneRenderer != nullptr &&
                   m_sceneRenderer->ArmGPUSceneCullingQualificationCapture();
        }

        bool RequestDirectOpaqueRasterReadbackQualificationCapture() override
        {
            return m_sceneRenderer != nullptr &&
                   m_sceneRenderer
                       ->ArmDirectOpaqueRasterReadbackQualificationCapture();
        }

        void PollCompletion() override
        {
            static_cast<void>(m_uploadProcessor.PollCompletion());
            if (m_sceneRenderer != nullptr && m_context != nullptr)
            {
                if (RenderSubmissionTracker* const tracker =
                        RenderContextInternalAccess::GetSubmissionTracker(
                            *m_context))
                {
                    m_sceneRenderer->PollGPUSceneCullingQualificationCompletion(
                        *tracker);
                }
            }
            if (m_context != nullptr)
            {
                m_context->PollFrameTiming();
            }
        }

        void RetireCompleted() override
        {
            static_cast<void>(m_retirementQueue.Poll());
        }

        [[nodiscard]] bool HasPendingCompletionWork() const noexcept override
        {
            return m_uploadProcessor.HasPendingCompletionWork() ||
                   m_retirementQueue.GetDiagnostics().entryCount != 0U ||
                   (m_sceneRenderer != nullptr &&
                    m_sceneRenderer
                        ->HasPendingGPUSceneCullingQualificationCompletion()) ||
                   (m_sceneRenderer != nullptr &&
                    m_sceneRenderer
                        ->HasPendingDirectOpaqueRasterReadbackQualificationCompletion());
        }

        void PopulateDiagnostics(
            RenderDiagnosticsSnapshot& outDiagnostics) const override
        {
            outDiagnostics.lastCapture = m_lastCaptureResult;
            outDiagnostics.cpuFrameTiming = m_lastCpuFrameTiming;
            PopulateGpuTimingDiagnostics(outDiagnostics);
            if (m_context != nullptr && m_context->GetDevice() != nullptr)
            {
                const RHICapabilities& capabilities =
                    m_context->GetDevice()->GetCapabilities();
                outDiagnostics.adapterName = capabilities.adapterName;
                outDiagnostics.driverVersion = capabilities.driverVersion;
            }
            // PopulateDiagnostics executes on the RenderThreadRuntime owner
            // thread.  Query the queue here so the value-only sample
            // projection describes an observed retirement state rather than
            // an update-thread default snapshot.
            outDiagnostics.retirement = m_retirementQueue.GetDiagnostics();
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

            RenderGraphLifetimeDiagnostics lifetime;
            lifetime.frameSequence =
                m_sceneRenderer->GetLastPresentedFrameSequence();
            if (const RenderGraph* graph = m_sceneRenderer->GetRenderGraph())
            {
                const RenderGraph::Diagnostics graphDiagnostics =
                    graph->GetDiagnostics();
                lifetime.planHash = graphDiagnostics.compileStats.planHash;
                lifetime.physicalRealizationCount =
                    graphDiagnostics.execution.physicalRealizationCount;
                lifetime.partialRealizationRollbackCount =
                    graphDiagnostics.execution.partialRollbackCount;
            }
            if (const TransientResourcePool* pool =
                    m_sceneRenderer->GetTransientResourcePool())
            {
                const TransientResourcePool::Stats poolStats =
                    pool->GetStats();
                lifetime.available = lifetime.planHash != 0;
                lifetime.physicalTextureAllocationCount =
                    poolStats.texturePoolSize;
                lifetime.physicalBufferAllocationCount =
                    poolStats.bufferPoolSize;
                lifetime.totalPooledMemoryBytes =
                    poolStats.totalPooledMemory;
                lifetime.texturePoolMissCount = poolStats.textureMisses;
                lifetime.bufferPoolMissCount = poolStats.bufferMisses;
                lifetime.transientViewCount = poolStats.textureViewCount;
                lifetime.transientViewMissCount =
                    poolStats.textureViewMisses;
                lifetime.transientViewCreationFailureCount =
                    poolStats.textureViewCreationFailureCount;
                lifetime.recordingTextureLeases =
                    poolStats.recordingTextureLeases;
                lifetime.recordingBufferLeases =
                    poolStats.recordingBufferLeases;
                lifetime.inFlightTextureLeases =
                    poolStats.inFlightTextureLeases;
                lifetime.inFlightBufferLeases =
                    poolStats.inFlightBufferLeases;
                lifetime.leaseCommitCount = poolStats.leaseCommitCount;
                lifetime.leaseAbortCount = poolStats.leaseAbortCount;
                lifetime.leaseDeviceLostCount =
                    poolStats.leaseDeviceLostCount;
                lifetime.leaseValidationFailureCount =
                    poolStats.leaseValidationFailureCount;
                lifetime.completionRetirementCount =
                    poolStats.completionRetirementCount;
            }
            if (m_context != nullptr && m_context->GetDevice() != nullptr)
            {
                const RHINativeValidationDiagnostics nativeValidation =
                    m_context->GetDevice()
                        ->GetNativeValidationDiagnostics();
                outDiagnostics.nativeValidation.available =
                    nativeValidation.available;
                outDiagnostics.nativeValidation.enabled =
                    nativeValidation.enabled;
                outDiagnostics.nativeValidation.readComplete =
                    nativeValidation.readComplete;
                outDiagnostics.nativeValidation.messageCount =
                    nativeValidation.messageCount;
                outDiagnostics.nativeValidation.warningCount =
                    nativeValidation.warningCount;
                outDiagnostics.nativeValidation.errorCount =
                    nativeValidation.errorCount;
                outDiagnostics.nativeValidation.corruptionCount =
                    nativeValidation.corruptionCount;

                const RHIDescriptorDiagnostics descriptors =
                    m_context->GetDevice()->GetDescriptorDiagnostics();
                const auto copyDescriptor = [](
                    const RHIDescriptorAllocatorStats& source,
                    RenderDescriptorAllocatorDiagnostics& target)
                {
                    target.currentPages = source.currentPages;
                    target.peakPages = source.peakPages;
                    target.activeDescriptors = source.activeDescriptors;
                    target.peakActiveDescriptors =
                        source.peakActiveDescriptors;
                    target.allocationFailures = source.allocationFailures;
                    target.validationFailures = source.validationFailures;
                };
                copyDescriptor(descriptors.resourceViews,
                               lifetime.descriptors.resourceViews);
                copyDescriptor(descriptors.samplers,
                               lifetime.descriptors.samplers);
                copyDescriptor(descriptors.renderTargets,
                               lifetime.descriptors.renderTargets);
                copyDescriptor(descriptors.depthStencils,
                               lifetime.descriptors.depthStencils);
            }
            outDiagnostics.renderGraphLifetime = std::move(lifetime);

            RenderFrameFeatureDiagnostics features;
            features.available = true;
            features.frameSequence =
                m_sceneRenderer->GetLastPresentedFrameSequence();
            features.renderAttempted = frame.renderAttempted;
            features.rendered = frame.rendered;
            features.graphBuilt = frame.graphBuilt;
            features.graphCompiled = frame.graphCompiled;
            features.mutationEvidence = frame.mutationEvidence;
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
            features.environmentIBL.requested = ibl.requested;
            features.environmentIBL.enabled = ibl.textureIBLEnabled;
            features.environmentIBL.prefilteredMipLevels =
                ibl.prefilteredMipLevels;
            features.environmentIBL.intensity = ibl.intensity;
            features.environmentIBL.reason = ibl.fallbackReason;
            features.material.presentedBindingsAvailable =
                frame.presentedMaterialBindingsAvailable;
            features.material.presentedBindingsOverflow =
                frame.presentedMaterialBindingsOverflow;
            features.material.presentedBindings =
                frame.presentedMaterialBindings;
            features.skinning.presentedReceiptsAvailable =
                frame.presentedSkinningPalettesAvailable;
            features.skinning.presentedReceiptsOverflow =
                frame.presentedSkinningPalettesOverflow;
            features.skinning.presentedReceipts =
                frame.presentedSkinningPalettes;
            features.transparent.available = true;
            features.transparent.orderValid = frame.transparentOrderValid;
            features.transparent.orderHash = frame.transparentOrderHash;
            features.transparent.rejectedNonFiniteDepthCount =
                frame.transparentRejectedNonFiniteDepthCount;
            features.transparent.candidateDrawItemCount =
                frame.transparentCandidateDrawItemCount;
            features.transparent.preparedDrawItemCount =
                frame.transparentPreparedDrawItemCount;
            features.transparent.executedPacketCount =
                frame.transparentExecutedPacketCount;
            features.transparent.executedDrawCount =
                frame.transparentExecutedDrawCount;
            features.transparent.skippedMaterialBindingCount =
                frame.transparentSkippedMaterialBindingCount;
            features.transparent.skippedResourceCount =
                frame.transparentSkippedResourceCount;
            features.transparent.skippedExecutionDrawCount =
                frame.transparentSkippedExecutionDrawCount;
            features.transparent.materialBindingCount =
                frame.transparentMaterialBindingCount;
            features.transparent.materialFallbackBindingCount =
                frame.transparentMaterialFallbackBindingCount;
            features.transparent.materialTextureFlags =
                frame.transparentMaterialTextureFlags;
            features.transparent.materialFallbackTextureFlags =
                frame.transparentMaterialFallbackTextureFlags;
            features.transparent.noWork = frame.transparentNoWork;
            features.transparent.preflightFailed =
                frame.transparentPreflightFailed;
            features.transparent.executionFailed =
                frame.transparentExecutionFailed;

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
            features.directionalShadow.available =
                frame.directionalShadowAvailable;
            features.directionalShadow.requested =
                frame.directionalShadowRequested;
            features.directionalShadow.supported =
                frame.directionalShadowSupported;
            features.directionalShadow.outputReady =
                frame.directionalShadowOutputReady;
            features.directionalShadow.samplingEnabled =
                frame.directionalShadowSamplingEnabled;
            features.directionalShadow.requestedCascadeCount =
                frame.directionalShadowRequestedCascadeCount;
            features.directionalShadow.producedCascadeCount =
                frame.directionalShadowProducedCascadeCount;
            features.directionalShadow.resolvedCascadeCount =
                frame.directionalShadowResolvedCascadeCount;
            features.directionalShadow.shadowMapSize =
                frame.directionalShadowMapSize;
            features.directionalShadow.shadowCasterCount =
                frame.directionalShadowCasterCount;
            features.directionalShadow.drawCount =
                frame.directionalShadowDrawCount;
            features.directionalShadow.reason =
                frame.directionalShadowReason;

            features.localLighting.available = frame.localLightingAvailable;
            features.localLighting.pointLightRequested =
                frame.pointLightRequestedCount;
            features.localLighting.pointLightAdmitted =
                frame.pointLightAdmittedCount;
            features.localLighting.pointLightCapacity =
                frame.pointLightCapacity;
            features.localLighting.pointLightOverflow =
                frame.pointLightOverflowCount;
            features.localLighting.spotLightRequested =
                frame.spotLightRequestedCount;
            features.localLighting.spotLightAdmitted =
                frame.spotLightAdmittedCount;
            features.localLighting.spotLightCapacity =
                frame.spotLightCapacity;
            features.localLighting.spotLightOverflow =
                frame.spotLightOverflowCount;
            features.localLighting.pointShadowRequested =
                frame.pointShadowRequestCount;
            features.localLighting.pointShadowSupported =
                frame.pointShadowSupported;
            features.localLighting.pointShadowReason =
                frame.pointShadowUnsupportedReason;
            features.localLighting.spotShadowRequested =
                frame.spotShadowRequestCount;
            features.localLighting.spotShadowSupported =
                frame.spotShadowSupported;
            features.localLighting.spotShadowReason =
                frame.spotShadowUnsupportedReason;

            features.hzb.requested = frame.hzbRequested;
            features.hzb.supported = frame.hzbSupported;
            features.hzb.enabled = frame.hzbEnabled;
            features.hzb.reason = frame.hzbReason;

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
            RVX_COPY_GPU_CULLING_FIELD(instanceUploadBytes);
            RVX_COPY_GPU_CULLING_FIELD(gpuSceneCandidateUploadBytes);
            RVX_COPY_GPU_CULLING_FIELD(activeRowUploadBytes);
            RVX_COPY_GPU_CULLING_FIELD(canonicalInstanceUploadWork);
            RVX_COPY_GPU_CULLING_FIELD(canonicalCandidateUploadWork);
            RVX_COPY_GPU_CULLING_FIELD(canonicalActiveRowUploadWork);
            RVX_COPY_GPU_CULLING_FIELD(activeRowCount);
            RVX_COPY_GPU_CULLING_FIELD(activeRowHighWatermark);
            RVX_COPY_GPU_CULLING_FIELD(instancePatchedRowCount);
            RVX_COPY_GPU_CULLING_FIELD(gpuSceneCandidatePatchedRowCount);
            RVX_COPY_GPU_CULLING_FIELD(activeRowPatchedRowCount);
            RVX_COPY_GPU_CULLING_FIELD(instanceFullMaterializationCount);
            RVX_COPY_GPU_CULLING_FIELD(gpuSceneCandidateFullMaterializationCount);
            RVX_COPY_GPU_CULLING_FIELD(activeRowFullMaterializationCount);
            RVX_COPY_GPU_CULLING_FIELD(continuityFullMaterializationCount);
            RVX_COPY_GPU_CULLING_FIELD(capacityFullMaterializationCount);
            RVX_COPY_GPU_CULLING_FIELD(directRasterInstanceUploadBytes);
            RVX_COPY_GPU_CULLING_FIELD(
                directRasterInstanceIndexUploadBytes);
            RVX_COPY_GPU_CULLING_FIELD(directRasterInstanceUploadWork);
            RVX_COPY_GPU_CULLING_FIELD(directRasterIndexUploadWork);
            RVX_COPY_GPU_CULLING_FIELD(
                directRasterInstancePatchedRowCount);
            RVX_COPY_GPU_CULLING_FIELD(directRasterIndexPatchedRowCount);
            RVX_COPY_GPU_CULLING_FIELD(directRasterActiveInstanceCount);
            RVX_COPY_GPU_CULLING_FIELD(directRasterActiveInstanceCapacity);
            RVX_COPY_GPU_CULLING_FIELD(
                directRasterInstanceFullMaterializationCount);
            RVX_COPY_GPU_CULLING_FIELD(
                directRasterIndexFullMaterializationCount);
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
            RVX_COPY_GPU_CULLING_FIELD(directOpaqueRasterTranscript);
            RVX_COPY_GPU_CULLING_FIELD(gpuSceneDepthQualification);
            RVX_COPY_GPU_CULLING_FIELD(gpuSceneOpaqueQualification);
#undef RVX_COPY_GPU_CULLING_FIELD
            features.policy = frame.policy;
            features.instancing = frame.instancing;
            const RenderSceneRetainedStats retainedScene =
                m_sceneRenderer->GetRenderSceneRetainedStats();
            const RenderDrawPacketCacheStats drawPackets =
                m_sceneRenderer->GetRenderDrawPacketCacheStats();
            features.sceneWork.available = true;
            features.sceneWork.fullRebuildCount =
                retainedScene.fullRebuildCount;
            features.sceneWork.incrementalUpdateCount =
                retainedScene.incrementalUpdateCount;
            features.sceneWork.staticReuseCount =
                retainedScene.staticReuseCount;
            features.sceneWork.appliedSceneRevision =
                retainedScene.appliedSceneRevision;
            features.sceneWork.lastRebuiltObjectCount =
                retainedScene.lastRebuiltObjectCount;
            features.sceneWork.lastRemovedObjectCount =
                retainedScene.lastRemovedObjectCount;
            features.sceneWork.drawPacketResolveCount =
                drawPackets.resolveCount;
            features.sceneWork.drawPacketHitCount = drawPackets.hitCount;
            features.sceneWork.drawPacketMissCount = drawPackets.missCount;
            features.sceneWork.drawPacketDynamicBypassCount =
                drawPackets.dynamicBypassCount;
            features.sceneWork.drawPacketBuildCount =
                drawPackets.packetBuildCount;
            features.sceneWork.drawPacketEntryCreationCount =
                drawPackets.entryCreationCount;
            uint64 drawPacketInvalidationCount = 0;
            for (const uint64 count : drawPackets.invalidationCounts)
                drawPacketInvalidationCount += count;
            features.sceneWork.drawPacketInvalidationCount =
                drawPacketInvalidationCount;
            features.sceneWork.drawPacketObjectRevisionInvalidationCount =
                drawPackets.GetInvalidationCount(
                    RenderDrawPacketCacheInvalidationReason::
                        ObjectRevisionChanged);
            features.sceneWork.drawPacketClearCount = drawPackets.clearCount;
            features.sceneWork.drawPacketEntryCount = static_cast<uint64>(
                drawPackets.entryCount);
            features.gpuScene = m_sceneRenderer->GetGPUSceneDiagnostics();
            outDiagnostics.mutationEvidence = features.mutationEvidence;

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
            // Aggregate only a failed requested binding. An ambient-floor frame
            // without any environment handles is an explicit policy, not a
            // fallback; a requested but unresolved binding is always surfaced.
            if (HasEnvironmentIBLFallback(features.environmentIBL))
            {
                appendReason(features.environmentIBL.reason);
            }
            if (features.directionalShadow.requested)
            {
                appendReason(features.directionalShadow.reason);
            }
            if (features.localLighting.pointShadowRequested > 0)
            {
                appendReason(features.localLighting.pointShadowReason);
            }
            if (features.localLighting.spotShadowRequested > 0)
            {
                appendReason(features.localLighting.spotShadowReason);
            }
            if (features.hzb.requested)
            {
                appendReason(features.hzb.reason);
            }
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

        void PopulateCompletionDiagnostics(
            RenderDiagnosticsSnapshot& outDiagnostics) const override
        {
            PopulateGpuTimingDiagnostics(outDiagnostics);
            // Completion polling owns only post-fence qualification results.
            // Do not call the broad frame projection here: it would stamp
            // current-frame values onto this delayed source-frame evidence.
            if (m_sceneRenderer != nullptr)
            {
                const SceneRendererFrameDiagnostics frame =
                    m_sceneRenderer->GetFrameDiagnostics();
                outDiagnostics.frameFeatures.gpuDrivenCulling
                    .gpuSceneDepthQualification =
                    frame.gpuDrivenCullingStats.gpuSceneDepthQualification;
                outDiagnostics.frameFeatures.gpuDrivenCulling
                    .gpuSceneOpaqueQualification =
                    frame.gpuDrivenCullingStats.gpuSceneOpaqueQualification;
                outDiagnostics.frameFeatures.gpuDrivenCulling
                    .directOpaqueRasterReadbackQualification =
                    frame.gpuDrivenCullingStats
                        .directOpaqueRasterReadbackQualification;
            }
            // PollCompletion/RetireCompleted has just run on the owning
            // thread. Refresh the queue so an idle completion pump can expose
            // terminal retirement without another frame submission.
            outDiagnostics.retirement = m_retirementQueue.GetDiagnostics();
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
                        // WaitIdle makes the terminal Graphics submission
                        // readable. Poll before tearing the context down so
                        // the runtime can publish this final delayed sample.
                        m_context->PollFrameTiming();
                        m_shutdownGpuFrameTiming =
                            GetGpuTimingDiagnostics();
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
            uint64 requestId = 0;
            uint64 frameSequence = 0;
            bool pixelProbeRequested = false;
            bool recorded = false;
        };

        [[nodiscard]] static uint64 ElapsedNanoseconds(
            std::chrono::steady_clock::time_point start) noexcept
        {
            return static_cast<uint64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - start)
                    .count());
        }

        void PopulateGpuTimingDiagnostics(
            RenderDiagnosticsSnapshot& outDiagnostics) const
        {
            outDiagnostics.gpuFrameTiming = GetGpuTimingDiagnostics();
        }

        [[nodiscard]] RenderGpuFrameTimingDiagnostics
            GetGpuTimingDiagnostics() const
        {
            if (m_context == nullptr)
            {
                return m_shutdownGpuFrameTiming;
            }

            RenderGpuFrameTimingDiagnostics timing =
                m_context->GetGpuFrameTimingDiagnostics();
            if (timing.timestampFrequency == 0 &&
                m_context->GetDevice() != nullptr)
            {
                const RHICapabilities& capabilities =
                    m_context->GetDevice()->GetCapabilities();
                if (capabilities.supportsTimestampQueries &&
                    capabilities.timestampFrequency != 0)
                {
                    // Preserve explicit startup capability evidence even
                    // before the first delayed completion sample arrives.
                    timing.timestampFrequency = capabilities.timestampFrequency;
                }
            }
            return timing;
        }

        RenderRuntimeResult PresentAcceptedFrame(
            uint64 frameSequence,
            uint64 surfaceGeneration,
            const RenderFrameCaptureRequest& captureRequest,
            RenderCpuFramePhaseDurations& phases)
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
            const auto frameBeginAcquireStart = std::chrono::steady_clock::now();
            const bool frameBegan = m_context->BeginFrame();
            phases.frameBeginAcquire =
                ElapsedNanoseconds(frameBeginAcquireStart);
            if (!frameBegan)
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
                // End the active recording before releasing descriptor sets and
                // other objects referenced by that command buffer. Releasing
                // first invalidates Vulkan command-buffer state while AbortFrame
                // is still required to finish the recording.
                m_context->AbortFrame();
                m_sceneRenderer->ReleaseUnsubmittedFrame();
                result.code = RenderRuntimeCode::RenderGraphValidationFailed;
                result.message =
                    "Accepted frame failed RenderGraph validation or recording";
                return result;
            }

            const SceneRendererCpuFrameTiming& rendererTiming =
                m_sceneRenderer->GetCurrentCpuFrameTiming();
            phases.renderPrepare = rendererTiming.renderPrepare;
            phases.policy = rendererTiming.policy;
            phases.graphBuild = rendererTiming.graphBuild;
            phases.graphCompile = rendererTiming.graphCompile;
            phases.graphRealizeRecord = rendererTiming.graphRealizeRecord;

            CaptureReadback capture = PrepareCapture(
                captureRequest,
                frameSequence);
            const auto frameEndSubmitStart = std::chrono::steady_clock::now();
            const GPUCompletionPoint submittedPoint = m_context->EndFrame();
            phases.frameEndSubmit =
                ElapsedNanoseconds(frameEndSubmitStart);
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
            RecordFirstSubmitted(frameSequence, submittedPoint.value);

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
            if (!m_sceneRenderer->NotifySubmission(completion))
            {
                result.code = RenderRuntimeCode::OwnershipViolation;
                result.message =
                    "Submitted frame completion did not match its recorded ownership boundary";
                return result;
            }
            if (!StampReferencedResources(executionResult.referencedResources,
                                          completion))
            {
                result.code = RenderRuntimeCode::OwnershipViolation;
                result.message =
                    "Submitted frame resources could not be stamped by exact generation";
                return result;
            }
            const auto presentStart = std::chrono::steady_clock::now();
            m_context->Present();
            phases.present = ElapsedNanoseconds(presentStart);
            RenderRuntimeResult health = MakeDeviceRuntimeResult();
            health.frameSequence = frameSequence;
            health.surfaceGeneration = surfaceGeneration;
            if (health.code != RenderRuntimeCode::Running)
            {
                return health;
            }
            // Binding must precede an optional capture's WaitIdle: otherwise
            // the submitted slot may be drained before timing ownership is
            // attached to the exact submitted completion point.
            static_cast<void>(
                m_context->BindSubmittedFrameTiming(submittedPoint,
                                                    frameSequence));
            CompleteCapture(capture);
            m_sceneRenderer->MarkAcceptedFramePresented();
            RecordFirstPresent(frameSequence);
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
            RenderRuntimeResult health = MakeDeviceRuntimeResult();
            if (health.code == RenderRuntimeCode::Running)
            {
                // A resize clear only establishes the new swap-chain surface.
                // It is not an accepted RenderFramePacket and must never
                // consume the startup milestones used to measure the first
                // application frame.
                RecordFirstSurfaceClear(surfaceGeneration, submittedPoint.value);
            }
            return health;
        }

        void RecordFirstSubmitted(uint64 frameSequence,
                                  uint64 completionValue)
        {
            if (m_firstSubmittedTraceRecorded)
                return;
            Diagnostics::RecordTraceInstant(
                m_startupTraceContext,
                "FirstFrameSubmitted",
                {{"sequence", frameSequence},
                 {"completionValue", completionValue}});
            m_firstSubmittedTraceRecorded = true;
        }

        void RecordFirstPresent(uint64 frameSequence)
        {
            if (m_firstPresentTraceRecorded)
                return;
            Diagnostics::RecordTraceInstant(
                m_startupTraceContext,
                "FirstSwapchainPresentAccepted",
                {{"sequence", frameSequence}});
            m_firstPresentTraceRecorded = true;
        }

        void RecordFirstSurfaceClear(uint64 surfaceGeneration,
                                     uint64 completionValue)
        {
            if (m_firstSurfaceClearTraceRecorded)
                return;
            Diagnostics::RecordTraceInstant(
                m_startupTraceContext,
                "FirstSurfaceClearPresentAccepted",
                {{"surfaceGeneration", surfaceGeneration},
                 {"completionValue", completionValue}});
            m_firstSurfaceClearTraceRecorded = true;
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
            readback.requestId = request.requestId;
            readback.frameSequence = frameSequence;
            readback.pixelProbeRequested = request.pixelProbeEnabled;
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
                if (readback.pixelProbeRequested)
                {
                    RenderFramePixelProbeResult& probe =
                        m_lastCaptureResult.pixelProbe;
                    probe.code =
                        RenderFramePixelProbeResultCode::FinalCaptureUnavailable;
                    probe.requestId = readback.requestId;
                    probe.frameSequence = readback.frameSequence;
                    probe.message =
                        "Final BGRA8 capture could not be mapped for the pixel probe";
                }
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

            if (!readback.pixelProbeRequested)
            {
                return;
            }

            RenderFramePixelProbeResult probe;
            if (m_sceneRenderer == nullptr ||
                !m_sceneRenderer->CompleteToneMappingPixelProbe(
                    readback.requestId,
                    readback.frameSequence,
                    probe))
            {
                probe.code = RenderFramePixelProbeResultCode::NotRecorded;
                probe.requestId = readback.requestId;
                probe.frameSequence = readback.frameSequence;
                probe.message =
                    "ToneMapping did not retain a pixel probe for the completed frame";
            }

            const auto clearProbeBits = [&probe]()
            {
                probe.preToneRGBA16FloatBits.fill(0);
                probe.finalBGRA8Bits.fill(0);
            };
            if (!probe.IsComplete() ||
                probe.requestId != m_lastCaptureResult.requestId ||
                probe.frameSequence != m_lastCaptureResult.frameSequence)
            {
                if (probe.code == RenderFramePixelProbeResultCode::Completed)
                {
                    probe.code = RenderFramePixelProbeResultCode::ForeignFrame;
                    probe.message =
                        "ToneMapping pixel probe did not match the final capture frame";
                }
                clearProbeBits();
                m_lastCaptureResult.pixelProbe = std::move(probe);
                return;
            }

            const bool bgra8 =
                m_lastCaptureResult.format == RHIFormat::BGRA8_UNORM ||
                m_lastCaptureResult.format == RHIFormat::BGRA8_UNORM_SRGB;
            if (!bgra8 || m_lastCaptureResult.bytesPerPixel != 4 ||
                probe.x >= m_lastCaptureResult.width ||
                probe.y >= m_lastCaptureResult.height)
            {
                probe.code = RenderFramePixelProbeResultCode::UnsupportedFormat;
                probe.message =
                    "Pixel probe requires the completed final capture to be BGRA8";
                clearProbeBits();
                m_lastCaptureResult.pixelProbe = std::move(probe);
                return;
            }

            const uint32 sourceY = m_lastCaptureResult.originBottomLeft
                                       ? m_lastCaptureResult.height - 1U - probe.y
                                       : probe.y;
            const uint64 byteOffset =
                static_cast<uint64>(sourceY) * m_lastCaptureResult.rowPitch +
                static_cast<uint64>(probe.x) * m_lastCaptureResult.bytesPerPixel;
            if (byteOffset > m_lastCaptureResult.bytes.size() ||
                m_lastCaptureResult.bytes.size() - byteOffset < 4U)
            {
                probe.code = RenderFramePixelProbeResultCode::FinalCaptureUnavailable;
                probe.message =
                    "Completed final capture does not contain the requested pixel";
                clearProbeBits();
                m_lastCaptureResult.pixelProbe = std::move(probe);
                return;
            }

            const uint8* const finalPixel =
                m_lastCaptureResult.bytes.data() + byteOffset;
            probe.finalBGRA8Bits = {
                finalPixel[0], finalPixel[1], finalPixel[2], finalPixel[3]};
            m_lastCaptureResult.pixelProbe = std::move(probe);
        }

        std::unique_ptr<RenderContext> m_context;
        std::unique_ptr<SceneRenderer> m_sceneRenderer;
        RenderRetirementQueue m_retirementQueue;
        RenderResourceRegistry m_resourceRegistry;
        RenderUploadProcessor m_uploadProcessor;
        RenderFrameCaptureResult m_lastCaptureResult{};
        RenderCpuFrameTimingDiagnostics m_lastCpuFrameTiming{};
        RenderGpuFrameTimingDiagnostics m_shutdownGpuFrameTiming{};
        Diagnostics::TraceContext m_startupTraceContext{};
        bool m_firstSubmittedTraceRecorded = false;
        bool m_firstPresentTraceRecorded = false;
        bool m_firstSurfaceClearTraceRecorded = false;
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

RenderFramePublishResult RenderSubsystem::TryPublishFrameSet(
    std::unique_ptr<const RenderSceneUpdateBatch> sceneUpdate,
    std::unique_ptr<const RenderFramePacketV5> frameV5)
{
    if (m_runtime == nullptr)
    {
        RenderFramePublishResult result;
        result.code = RenderFramePublishCode::NotRunning;
        result.resultClass = ClassifyRenderFramePublishCode(result.code);
        return result;
    }
    return m_runtime->TryPublishFrameSet(std::move(sceneUpdate),
                                         std::move(frameV5));
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

bool RenderSubsystem::RequestCompletionPump() noexcept
{
    return m_runtime != nullptr && m_runtime->RequestCompletionPump();
}

    bool RenderSubsystem::RequestCompletionPoll() noexcept
    {
        return m_runtime != nullptr && m_runtime->RequestCompletionPoll();
    }

    bool RenderSubsystem::RequestGPUSceneCullingQualificationCapture() noexcept
    {
        return m_runtime != nullptr &&
               m_runtime->RequestGPUSceneCullingQualificationCapture();
    }

    bool RenderSubsystem::RequestDirectOpaqueRasterReadbackQualificationCapture()
        noexcept
    {
        return m_runtime != nullptr &&
               m_runtime
                   ->RequestDirectOpaqueRasterReadbackQualificationCapture();
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
