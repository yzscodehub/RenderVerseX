/** @file SampleRunner.cpp @brief Shared sample executable host. */

#include "Samples/SampleRunner.h"

#include "Core/Core.h"
#include "Core/Diagnostics/Trace.h"
#include "Core/Hash/SHA256.h"
#include "Engine/Engine.h"
#include "HAL/Input/KeyCodes.h"
#include "Render/RenderSubsystem.h"
#include "RenderContracts/RenderPipelineProfile.h"
#include "Resource/ResourceSubsystem.h"
#include "Runtime/Input/InputSubsystem.h"
#include "Runtime/Window/WindowSubsystem.h"
#include "Samples/FrameworkAssessment.h"
#include "Samples/RenderQualityPerformanceWriter.h"
#include "Samples/RenderQualityQualificationWriter.h"
#include "Samples/RuntimeFrameDriver.h"
#include "Samples/SampleAnimationLoader.h"
#include "Samples/SampleAssetCatalog.h"
#include "Samples/SampleContext.h"
#include "Samples/SampleEnvironmentLoader.h"
#include "Samples/SampleHostComposition.h"
#include "Samples/SampleLifetimeQualification.h"
#include "Samples/SampleModelLoader.h"
#include "Samples/SampleParticleEffectLoader.h"
#include "Samples/SampleRegistry.h"
#include "Samples/SampleScreenshotWriter.h"
#include "World/World.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#elif defined(__linux__) || defined(__APPLE__)
#include <sys/utsname.h>
#endif

namespace RVX
{
namespace
{
    static_assert(
        RVX_RENDER_QUALITY_PERFORMANCE_REQUIRED_TRACKED_UPLOAD_COVERAGE ==
            RVX_RENDER_QUALITY_PERFORMANCE_REQUIRED_UPLOAD_COVERAGE,
        "Sample performance report upload coverage must match Render evidence");
    static_assert(
        RVX_RENDER_QUALITY_PERFORMANCE_REQUIRED_TRACKED_FOOTPRINT_COVERAGE ==
            RVX_RENDER_QUALITY_PERFORMANCE_REQUIRED_FOOTPRINT_COVERAGE,
        "Sample performance report footprint coverage must match Render evidence");

    [[nodiscard]] bool TryMapSamplePipelineRecipe(
        SamplePipelineRecipe sampleRecipe,
        RenderPipelineRecipe& outRecipe) noexcept
    {
        switch (sampleRecipe)
        {
            case SamplePipelineRecipe::Minimal:
                outRecipe = RenderPipelineRecipe::Minimal;
                return true;
            case SamplePipelineRecipe::ForwardStandard:
                outRecipe = RenderPipelineRecipe::ForwardStandard;
                return true;
            case SamplePipelineRecipe::DeferredQuality:
                outRecipe = RenderPipelineRecipe::DeferredQuality;
                return true;
            case SamplePipelineRecipe::HybridRayTracing:
                outRecipe = RenderPipelineRecipe::HybridRayTracing;
                return true;
            case SamplePipelineRecipe::Headless:
                outRecipe = RenderPipelineRecipe::Headless;
                return true;
            default: return false;
        }
    }

    [[nodiscard]] bool TryMapRenderPipelineRecipe(
        RenderPipelineRecipe renderRecipe,
        SamplePipelineRecipe& outRecipe) noexcept
    {
        switch (renderRecipe)
        {
            case RenderPipelineRecipe::Minimal:
                outRecipe = SamplePipelineRecipe::Minimal;
                return true;
            case RenderPipelineRecipe::ForwardStandard:
                outRecipe = SamplePipelineRecipe::ForwardStandard;
                return true;
            case RenderPipelineRecipe::DeferredQuality:
                outRecipe = SamplePipelineRecipe::DeferredQuality;
                return true;
            case RenderPipelineRecipe::HybridRayTracing:
                outRecipe = SamplePipelineRecipe::HybridRayTracing;
                return true;
            case RenderPipelineRecipe::Headless:
                outRecipe = SamplePipelineRecipe::Headless;
                return true;
            default: return false;
        }
    }

    [[nodiscard]] bool TryMapSampleQualityTier(
        SampleQualityTier sampleTier,
        RenderQualityTier& outTier) noexcept
    {
        switch (sampleTier)
        {
            case SampleQualityTier::Low:
                outTier = RenderQualityTier::Low;
                return true;
            case SampleQualityTier::Medium:
                outTier = RenderQualityTier::Medium;
                return true;
            case SampleQualityTier::High:
                outTier = RenderQualityTier::High;
                return true;
            case SampleQualityTier::Ultra:
                outTier = RenderQualityTier::Ultra;
                return true;
            case SampleQualityTier::Default:
            default: return false;
        }
    }

    [[nodiscard]] std::string ReadEnvironmentString(const char* name)
    {
#if defined(_WIN32)
        char* value = nullptr;
        size_t valueLength = 0;
        if (_dupenv_s(&value, &valueLength, name) != 0 || value == nullptr)
        {
            return {};
        }
        std::string result = valueLength > 1 ? std::string(value) : std::string{};
        std::free(value);
        return result;
#else
        const char* value = std::getenv(name);
        return value != nullptr ? std::string(value) : std::string{};
#endif
    }

    /**
     * @brief Return an OS build captured at performance-session admission.
     *
     * Windows uses RtlGetVersion because compatibility manifests can make
     * GetVersionEx report an older build. POSIX hosts use uname's kernel
     * release from the running host. Unsupported hosts fail closed.
     */
    [[nodiscard]] std::string ResolveHostOperatingSystemBuild()
    {
#if defined(_WIN32)
        struct RtlOsVersionInformation
        {
            ULONG size = 0;
            ULONG major = 0;
            ULONG minor = 0;
            ULONG build = 0;
            ULONG platformId = 0;
            WCHAR servicePack[128]{};
        };
        using RtlGetVersionFunction = LONG(WINAPI*)(
            RtlOsVersionInformation*);
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        const auto getVersion = ntdll != nullptr
            ? reinterpret_cast<RtlGetVersionFunction>(
                  GetProcAddress(ntdll, "RtlGetVersion"))
            : nullptr;
        RtlOsVersionInformation version{};
        version.size = sizeof(version);
        if (getVersion == nullptr || getVersion(&version) != 0 ||
            version.major == 0 || version.build == 0)
        {
            return {};
        }
        return "Windows-" + std::to_string(version.major) + "." +
               std::to_string(version.minor) + "." +
               std::to_string(version.build);
#elif defined(__linux__) || defined(__APPLE__)
        struct utsname version{};
        if (uname(&version) != 0 || version.sysname[0] == '\0' ||
            version.release[0] == '\0')
        {
            return {};
        }
        return std::string(version.sysname) + "-" + version.release;
#else
        return {};
#endif
    }

    [[nodiscard]] RenderExecutionBuildIdentity
    ReadValidationExecutionBuildIdentity()
    {
        return RenderExecutionBuildIdentity{
            ReadEnvironmentString("RVX_VALIDATION_SOURCE_COMMIT"),
            ReadEnvironmentString("RVX_VALIDATION_PRESET")};
    }

    struct FrozenRenderQualityPerformanceHostProvenance
    {
        std::string runId{};
        std::string adapterName{};
        std::string driverVersion{};
        std::string osBuild{};
        std::string capabilityHash{};
    };

    [[nodiscard]] float64 NanosecondsToMilliseconds(
        const RenderQualityPerformanceDuration duration) noexcept
    {
        return static_cast<float64>(duration.nanoseconds) / 1000000.0;
    }

    [[nodiscard]] bool BuildRenderQualityPerformanceWriteRequest(
        const RenderQualityPerformanceEvidence& evidence,
        const FrozenRenderQualityPerformanceHostProvenance& host,
        const SampleRunnerCLIOptions& options,
        const SampleScenarioManifest& manifest,
        RenderQualityPerformanceWriteRequest& outRequest,
        std::string* outError)
    {
        const auto fail = [outError](std::string message)
        {
            if (outError != nullptr)
            {
                *outError = std::move(message);
            }
            return false;
        };
        if (!evidence.IsComplete() || evidence.samples.empty() ||
            host.runId.empty() || host.adapterName.empty() ||
            host.driverVersion.empty() || host.osBuild.empty() ||
            host.capabilityHash.empty() || manifest.captureTicks.empty() ||
            options.qualificationPerformancePath.empty() ||
            options.qualificationPerformanceBaselineIdentity.empty() ||
            !options.captureTickResolved || options.captureTick == 0 ||
            evidence.hardware.capabilityReportSha256 != host.capabilityHash)
        {
            return fail(
                "GFX24 performance evidence or frozen host provenance is incomplete");
        }
        if (options.common.width != 2560U || options.common.height != 1440U)
        {
            return fail(
                "GFX24 performance qualification requires the exact 2560x1440 extent");
        }

        const RenderQualityPerformanceReceiptBinding& firstBinding =
            evidence.samples.front().binding;
        if (!firstBinding.IsValid() ||
            firstBinding.backend != evidence.hardware.backend)
        {
            return fail(
                "GFX24 performance evidence has invalid frozen build or backend provenance");
        }

        RenderQualityPerformanceWriteRequest request;
        request.outputPath = options.qualificationPerformancePath;
        request.sourceCommit = firstBinding.sourceCommit;
        request.runId = host.runId;
        request.preset = firstBinding.preset;
        request.qualificationIdentity = {
            .sampleId = "render-quality-lab",
            .pipeline = GetRenderPipelineRecipeName(firstBinding.recipe),
            .quality = GetRenderQualityTierName(firstBinding.quality),
            .backend = firstBinding.backend,
            .view = options.viewBookmarkId,
            .qualificationWidth = options.common.width,
            .qualificationHeight = options.common.height,
            .captureTick = options.captureTick,
            .visualContractVersion = manifest.visualContractVersion};
        request.hardwareIdentity = {
            .backend = evidence.hardware.backend,
            .adapterName = host.adapterName,
            .driverVersion = host.driverVersion,
            .osBuild = host.osBuild,
            .capabilityHash = evidence.hardware.capabilityReportSha256};
        request.telemetryCoverage = {
            .schemaVersion =
                RVX_RENDER_QUALITY_PERFORMANCE_TELEMETRY_COVERAGE_SCHEMA_VERSION,
            .trackedUploadCoverageMask =
                RVX_RENDER_QUALITY_PERFORMANCE_REQUIRED_UPLOAD_COVERAGE,
            .trackedEstimatedFootprintCoverageMask =
                RVX_RENDER_QUALITY_PERFORMANCE_REQUIRED_FOOTPRINT_COVERAGE};
        request.baselineIdentity =
            options.qualificationPerformanceBaselineIdentity;
        request.completedWarmupFrames = evidence.completedWarmupSamples;
        request.samples.reserve(evidence.samples.size());

        for (const RenderQualityPerformanceSample& source : evidence.samples)
        {
            if (!source.IsValid() ||
                source.binding.sourceCommit != firstBinding.sourceCommit ||
                source.binding.preset != firstBinding.preset ||
                source.binding.backend != firstBinding.backend ||
                source.binding.recipe != firstBinding.recipe ||
                source.binding.quality != firstBinding.quality ||
                source.binding.viewIdentity != firstBinding.viewIdentity ||
                !source.cpu.extraction.available ||
                !source.cpu.visibilityAndDrawPlanning.available ||
                !source.cpu.graphCompile.available ||
                !source.cpu.frameEndSubmit.available)
            {
                return fail(
                    "GFX24 performance samples are not uniformly frozen by one Render session");
            }

            RenderQualityPerformanceReportSample target;
            target.frameSequence = source.binding.frameIdentity.frameSequence;
            target.submittedFrame = source.submittedFrame;
            target.completedFrame = source.completedFrame;
            target.renderExecutionReceiptSha256 = Hash::FormatSHA256Digest(
                source.binding.receiptDigest);
            target.completionOwned = true;
            target.extractionMilliseconds =
                NanosecondsToMilliseconds(source.cpu.extraction);
            target.visibilityMilliseconds = NanosecondsToMilliseconds(
                source.cpu.visibilityAndDrawPlanning);
            target.graphCompileMilliseconds = NanosecondsToMilliseconds(
                source.cpu.graphCompile);
            target.submissionMilliseconds = NanosecondsToMilliseconds(
                source.cpu.frameEndSubmit);
            // RenderGraph's terminal critical path can be shorter than an
            // independent pass-envelope branch. The policy-owned value is
            // therefore the conservative maximum of both completion-owned
            // spans, never a whole-frame, CPU-submit, or Present duration.
            target.graphicsTerminalCommandSpanMilliseconds =
                source.graphicsTerminalCommandSpanMilliseconds;
            target.renderGraphPassEnvelopeMilliseconds =
                source.graphSpanMilliseconds;
            target.graphicsQualificationSpanMilliseconds = std::max(
                source.graphicsTerminalCommandSpanMilliseconds,
                source.graphSpanMilliseconds);
            target.trackedUploadBytes = source.trackedUploadBytes;
            target.trackedUploadCoverageMask =
                source.trackedUploadCoverageMask;
            target.trackedEstimatedFootprint = {
                .schemaVersion =
                    source.trackedEstimatedFootprint.schemaVersion,
                .coverageMask =
                    source.trackedEstimatedFootprint.coverageMask,
                .transientPoolPhysicalEstimatedBytes =
                    source.trackedEstimatedFootprint
                        .transientPoolPhysicalEstimatedBytes,
                .importedPersistentEstimatedBytes =
                    source.trackedEstimatedFootprint
                        .importedPersistentEstimatedBytes,
                .importedHistoryEstimatedBytes =
                    source.trackedEstimatedFootprint
                        .importedHistoryEstimatedBytes,
                .resourceRegistryResidentEstimatedBytes =
                    source.trackedEstimatedFootprint
                        .resourceRegistryResidentEstimatedBytes};
            target.resourceCountsSchemaVersion =
                source.resourceCounts.schemaVersion;
            target.resourceCounts = {
                {"stableImportedBuffers",
                 source.resourceCounts.stableImportedBufferCount},
                {"stableImportedTextures",
                 source.resourceCounts.stableImportedTextureCount},
                {"residentMaterials", source.resourceCounts.residentMaterialCount},
                {"residentMeshes", source.resourceCounts.residentMeshCount},
                {"residentTextures", source.resourceCounts.residentTextureCount},
                {"transientBuffers", source.resourceCounts.transientBufferCount},
                {"transientTextures", source.resourceCounts.transientTextureCount}};
            for (const RenderQualityPerformancePassTiming& pass :
                 source.passTimings)
            {
                target.passGpuTimings.push_back(
                    {.executionIndex = pass.executionIndex,
                     .name = pass.passName,
                     .durationMilliseconds = pass.durationMilliseconds});
            }
            request.samples.push_back(std::move(target));
        }

        outRequest = std::move(request);
        return true;
    }
} // namespace

    bool HasRequiredGPUCullingQualificationComparison(
        const SampleRenderDiagnostics& diagnostics) noexcept
    {
        const auto compared = [](const SampleGPUSceneCullingQualification& lane)
        {
            return !lane.required ||
                (lane.requested && lane.submissionAccepted &&
                 lane.completionObserved && lane.compared &&
                 lane.inputCoverageCompared &&
                 lane.directVisibilityCoverageCompared &&
                 lane.cullOutputsCompared && lane.indirectArgumentsCompared &&
                 (lane.capturedTier != "IndirectGrouped" ||
                  lane.rasterPayloadCompared));
        };
        const bool hasRequiredLane =
            diagnostics.gpuSceneDepthQualification.required ||
            diagnostics.gpuSceneOpaqueQualification.required;
        return hasRequiredLane &&
            compared(diagnostics.gpuSceneDepthQualification) &&
            compared(diagnostics.gpuSceneOpaqueQualification);
    }

    bool HasRequiredGPUCullingQualificationMatch(
        const SampleRenderDiagnostics& diagnostics) noexcept
    {
        const auto matched = [](const SampleGPUSceneCullingQualification& lane)
        {
            return !lane.required ||
                (lane.requested && lane.submissionAccepted &&
                 lane.completionObserved && lane.compared && lane.matched &&
                 lane.inputCoverageCompared && lane.inputCoverageMatched &&
                 lane.directVisibilityCoverageCompared &&
                 lane.directVisibilityCoverageMatched &&
                 lane.cullOutputsCompared && lane.cullOutputsMatched &&
                 lane.indirectArgumentsCompared &&
                 lane.indirectArgumentsMatched &&
                 (lane.capturedTier != "IndirectGrouped" ||
                  (lane.rasterPayloadCompared && lane.rasterPayloadMatched)));
        };
        const bool hasRequiredLane =
            diagnostics.gpuSceneDepthQualification.required ||
            diagnostics.gpuSceneOpaqueQualification.required;
        return hasRequiredLane &&
            matched(diagnostics.gpuSceneDepthQualification) &&
            matched(diagnostics.gpuSceneOpaqueQualification);
    }

    bool HasDirectRasterReadbackQualificationComparison(
        const SampleRenderDiagnostics& diagnostics) noexcept
    {
        const SampleDirectRasterReadbackQualification& capture =
            diagnostics.directOpaqueRasterReadbackQualification;
        return capture.required && capture.requested && capture.identity &&
               capture.allDirectDrawsInstanced && capture.readbackAllocated &&
               capture.copyRecorded && capture.submissionAccepted &&
               capture.completionObserved && capture.compared;
    }

    bool HasDirectRasterReadbackQualificationMatch(
        const SampleRenderDiagnostics& diagnostics) noexcept
    {
        return HasDirectRasterReadbackQualificationComparison(diagnostics) &&
               diagnostics.directOpaqueRasterReadbackQualification.matched;
    }

    namespace
    {
        [[nodiscard]] bool RequiredGPUCullingQualificationMatchesFrame(
            const SampleRenderDiagnostics& diagnostics,
            uint64 targetFrameSequence) noexcept
        {
            if (targetFrameSequence == 0)
            {
                return false;
            }
            const auto matches = [targetFrameSequence](
                                     const SampleGPUSceneCullingQualification& lane)
            {
                return !lane.required ||
                    lane.frameSequence == targetFrameSequence;
            };
            const bool hasRequiredLane =
                diagnostics.gpuSceneDepthQualification.required ||
                diagnostics.gpuSceneOpaqueQualification.required;
            return hasRequiredLane &&
                matches(diagnostics.gpuSceneDepthQualification) &&
                matches(diagnostics.gpuSceneOpaqueQualification);
        }

        [[nodiscard]] bool DirectRasterReadbackQualificationMatchesFrame(
            const SampleRenderDiagnostics& diagnostics,
            uint64 targetFrameSequence) noexcept
        {
            return targetFrameSequence != 0 &&
                   diagnostics.directOpaqueRasterReadbackQualification.required &&
                   diagnostics.directOpaqueRasterReadbackQualification
                           .frameSequence == targetFrameSequence;
        }

        [[nodiscard]] SampleRenderUploadWorkDiagnostics
            CopyRenderUploadWorkDiagnostics(
                const RenderUploadWorkDiagnostics& source)
        {
            SampleRenderUploadWorkDiagnostics result;
            result.cpuCopiedPayloadBytes = source.cpuCopiedPayloadBytes;
            result.committedPayloadBytes = source.committedPayloadBytes;
            result.mappedRangeCount = source.mappedRangeCount;
            result.committedRangeCount = source.committedRangeCount;
            result.hostVisibilitySynchronizedBytes =
                source.hostVisibilitySynchronizedBytes;
            result.hostVisibilitySynchronizationScopeRangeCounts =
                source.hostVisibilitySynchronizationScopeRangeCounts;
            result.hostVisibilitySynchronizationScopeBytes =
                source.hostVisibilitySynchronizationScopeBytes;
            result.gpuCopyBytes = source.gpuCopyBytes;
            result.gpuCopyRangeCount = source.gpuCopyRangeCount;
            return result;
        }

        std::string ResolveActualRenderPath(
            SampleRenderPath requestedPath,
            const SampleRenderDiagnostics& diagnostics)
        {
            if (!diagnostics.renderPolicyRequestAvailable ||
                !diagnostics.renderPolicyPlanAvailable ||
                !diagnostics.renderPolicyReportAvailable ||
                diagnostics.renderPolicyRequestFrameSequence == 0 ||
                diagnostics.renderPolicyRequestFrameSequence !=
                    diagnostics.renderPolicyPlanFrameSequence ||
                diagnostics.renderPolicyRequestFrameSequence !=
                    diagnostics.renderPolicyReportFrameSequence ||
                diagnostics.renderPolicyRequestFrameSequence !=
                    diagnostics.completedPresentedFrameSequence ||
                diagnostics.renderPolicyExecutionStatus != "Completed")
            {
                return "unavailable";
            }

            std::string_view resolved;
            if (diagnostics.renderPolicyExecutedTier == "Direct")
            {
                resolved = "direct";
            }
            else if (diagnostics.renderPolicyExecutedTier == "IndirectGrouped" ||
                     diagnostics.renderPolicyExecutedTier == "GPUResidentScene")
            {
                resolved = "gpu-driven";
            }
            else
            {
                return "unavailable";
            }

            const char* const expectedRequest =
                requestedPath == SampleRenderPath::Direct
                    ? "ForceDisabled"
                    : requestedPath == SampleRenderPath::GPUDriven
                          ? "ForceEnabled"
                          : "Auto";
            if (diagnostics.renderPolicyRequestedMode != expectedRequest)
            {
                return "unavailable";
            }
            if ((requestedPath == SampleRenderPath::Direct &&
                resolved != "direct") ||
                (requestedPath == SampleRenderPath::GPUDriven &&
                 resolved != "gpu-driven"))
            {
                return "unavailable";
            }
            return std::string(resolved);
        }

        [[nodiscard]] bool HasGPUSceneCullingQualificationComparison(
            const SampleRenderDiagnostics& diagnostics) noexcept
        {
            return HasRequiredGPUCullingQualificationComparison(diagnostics);
        }

        [[nodiscard]] bool HasMatchedGPUSceneCullingQualification(
            const SampleRenderDiagnostics& diagnostics) noexcept
        {
            return HasRequiredGPUCullingQualificationMatch(diagnostics);
        }

        [[nodiscard]] const char* GetPhysicsBackendName(
            Physics::PhysicsBackendType backend)
        {
            switch (backend)
            {
                case Physics::PhysicsBackendType::Auto:
                    return "auto";
                case Physics::PhysicsBackendType::BuiltIn:
                    return "built-in";
                case Physics::PhysicsBackendType::Jolt:
                    return "jolt";
            }
            return "unavailable";
        }

        [[nodiscard]] SampleEcsRuntimeDiagnostics MakeEcsRuntimeDiagnostics(
            const SceneECS::SceneEcsDiagnosticsSnapshot& scene,
            const WorldEcsRuntimeServicesDiagnostics& services)
        {
            SampleEcsRuntimeDiagnostics result;
            result.available = services.available && services.initialized &&
                scene.sceneRuntimeId.IsValid() &&
                scene.sceneRuntimeId.GetValue() == services.sceneRuntimeId;
            result.sceneRuntimeId = scene.sceneRuntimeId.GetValue();
            result.sceneFrameSequence = scene.frameSequence;
            result.sceneFixedStepSequence = scene.fixedStepSequence;
            result.sceneSnapshotRevision = scene.sceneSnapshotRevision;
            result.entityCount = scene.entityCount;
            result.pendingDestroyEntityCount = scene.pendingDestroyCount;
            result.cleanupRequiredEntityCount = scene.cleanupRequiredCount;
            result.retiringEntityCount = scene.retiringCount;
            result.recyclableEntityCount = scene.recyclableCount;
            result.spatialEntryCount = scene.spatialEntryCount;
            result.nextStructuralJournalSequence = scene.nextStructuralSequence;
            result.nextCleanupJournalSequence = scene.nextCleanupSequence;
            result.cleanupJournalContinuityLossCount =
                scene.cleanupContinuityLossCount;
            result.queuedCommandBufferCount = scene.queuedCommandBufferCount;
            result.appliedCommandBufferCount = scene.appliedCommandBufferCount;
            result.rejectedCommandBufferCount = scene.rejectedCommandBufferCount;
            result.localTransformWriteVersion = scene.localTransformWriteVersion;
            result.renderWorldTransformWriteVersion =
                scene.renderWorldTransformWriteVersion;
            result.physicsBodySideTableEntryCount =
                services.physicsBridgeBindingSideTableEntryCount;
            result.animationBindingSideTableEntryCount =
                services.animationBindingSideTableEntryCount;
            result.resourceAnimationPlaybackSideTableEntryCount =
                services.resourceAnimationPlaybackSideTableEntryCount;
            result.audioPlaybackSideTableEntryCount =
                services.audioPlaybackSideTableEntryCount;
            result.scriptInstanceSideTableEntryCount =
                services.scriptInstanceSideTableEntryCount;
            result.particleFeatureSnapshotCount =
                services.particleFeatureSnapshotCount;
            result.waterFeatureSnapshotCount = services.waterFeatureSnapshotCount;
            result.terrainFeatureSnapshotCount =
                services.terrainFeatureSnapshotCount;
            result.trackedModelRequestCount = services.trackedModelRequestCount;
            result.trackedEnvironmentRequestCount =
                services.trackedEnvironmentRequestCount;
            result.trackedAnimationRequestCount =
                services.trackedAnimationRequestCount;
            result.legacyFallbackCount = 0;
            return result;
        }

        void SetError(std::string* outError, std::string error)
        {
            if (outError)
            {
                *outError = std::move(error);
            }
        }

        AssessmentMetricValue MakeAssessmentMetric(
            const char* code,
            const char* unit,
            const char* description,
            AssessmentScalar value)
        {
            return {{AssessmentCode(code), unit, description},
                    DiagnosticValue<AssessmentScalar>::Available(
                        std::move(value))};
        }

        AssessmentMetricValue MakeUnavailableAssessmentMetric(
            const char* code,
            const char* unit,
            const char* description,
            std::string reason)
        {
            return {{AssessmentCode(code), unit, description},
                    DiagnosticValue<AssessmentScalar>::Unavailable(
                        std::move(reason))};
        }

        void AppendFrameTimingAssessmentMetrics(
            FrameworkAssessmentSession& session,
            AssessmentSnapshot& snapshot,
            const RenderDiagnosticsSnapshot* diagnostics,
            bool requireGpuTiming,
            bool includeCpuTiming,
            bool includeGpuCapabilityObservation,
            bool includeGpuLossFinding)
        {
            constexpr const char* CpuUnavailableReason =
                "No successfully presented frame CPU timing sample is available.";
            constexpr const char* GpuUnavailableReason =
                "No completion-owned Graphics frame timing sample is available.";

            const RenderCpuFrameTimingDiagnostics* cpu =
                diagnostics != nullptr ? &diagnostics->cpuFrameTiming : nullptr;
            const RenderGpuFrameTimingDiagnostics* gpu =
                diagnostics != nullptr ? &diagnostics->gpuFrameTiming : nullptr;

            bool cpuTimingMatchesCompletedScene = false;
            std::string cpuTimingReason = CpuUnavailableReason;
            if (cpu == nullptr)
            {
                cpuTimingReason =
                    "Render diagnostics are unavailable for CPU timing provenance.";
            }
            else if (!cpu->phases.IsAvailable())
            {
                cpuTimingReason = cpu->phases.GetReason();
                if (cpuTimingReason.empty())
                {
                    cpuTimingReason = CpuUnavailableReason;
                }
            }
            else if (diagnostics == nullptr || !diagnostics->sceneValues.available)
            {
                cpuTimingReason =
                    "CPU timing cannot be matched because completed RenderScene values are unavailable.";
            }
            else if (cpu->sourceFrameSequence ==
                         diagnostics->lastPresentedFrameSequence &&
                     cpu->requiredSceneRevision ==
                         diagnostics->sceneValues.requiredSceneRevision &&
                     cpu->appliedSceneRevision ==
                         diagnostics->sceneValues.appliedSceneRevision)
            {
                cpuTimingMatchesCompletedScene = true;
            }
            else
            {
                cpuTimingReason =
                    "CPU timing provenance mismatch with the completed RenderScene snapshot: source=" +
                    std::to_string(cpu->sourceFrameSequence) +
                    ", presented=" +
                    std::to_string(diagnostics->lastPresentedFrameSequence) +
                    ", required=" +
                    std::to_string(cpu->requiredSceneRevision) +
                    ", sceneRequired=" +
                    std::to_string(diagnostics->sceneValues.requiredSceneRevision) +
                    ", applied=" +
                    std::to_string(cpu->appliedSceneRevision) +
                    ", sceneApplied=" +
                    std::to_string(diagnostics->sceneValues.appliedSceneRevision) +
                    ".";
            }

            const auto appendCpu = [&snapshot, cpu, cpuTimingMatchesCompletedScene,
                                    &cpuTimingReason, includeCpuTiming](
                                       const char* code,
                                       const char* description,
                                       uint64 RenderCpuFramePhaseDurations::*member)
            {
                if (!includeCpuTiming)
                {
                    return;
                }
                if (cpuTimingMatchesCompletedScene)
                {
                    snapshot.metrics.push_back(MakeAssessmentMetric(
                        code, "ns", description,
                        (*cpu->phases.GetValue()).*member));
                    return;
                }
                snapshot.metrics.push_back(MakeUnavailableAssessmentMetric(
                    code, "ns", description, cpuTimingReason));
            };
            appendCpu("RENDER.CPU.FRAME_APPLY_NS",
                      "CPU duration of persistent RenderScene frame application.",
                      &RenderCpuFramePhaseDurations::frameApply);
            appendCpu("RENDER.CPU.FRAME_BEGIN_ACQUIRE_NS",
                      "CPU duration of the complete frame-slot acquire and begin call.",
                      &RenderCpuFramePhaseDurations::frameBeginAcquire);
            appendCpu("RENDER.CPU.RENDER_PREPARE_NS",
                      "CPU duration of SceneRenderer frame preparation.",
                      &RenderCpuFramePhaseDurations::renderPrepare);
            appendCpu("RENDER.CPU.POLICY_NS",
                      "CPU duration of render policy configuration and resolution.",
                      &RenderCpuFramePhaseDurations::policy);
            appendCpu("RENDER.CPU.GRAPH_BUILD_NS",
                      "CPU duration of RenderGraph definition construction.",
                      &RenderCpuFramePhaseDurations::graphBuild);
            appendCpu("RENDER.CPU.GRAPH_COMPILE_NS",
                      "CPU duration of immutable RenderGraph plan compilation.",
                      &RenderCpuFramePhaseDurations::graphCompile);
            appendCpu("RENDER.CPU.GRAPH_REALIZE_RECORD_NS",
                      "CPU duration of graph realization and command recording.",
                      &RenderCpuFramePhaseDurations::graphRealizeRecord);
            appendCpu("RENDER.CPU.FRAME_END_SUBMIT_NS",
                      "CPU duration of the complete frame submission call.",
                      &RenderCpuFramePhaseDurations::frameEndSubmit);
            appendCpu("RENDER.CPU.PRESENT_NS",
                      "CPU duration of the presentation call.",
                      &RenderCpuFramePhaseDurations::present);
            appendCpu("RENDER.CPU.ACCEPTED_FRAME_TOTAL_NS",
                      "CPU duration of the complete successful ConsumeFrameV5 call.",
                      &RenderCpuFramePhaseDurations::acceptedFrameTotal);

            const auto appendCpuProvenance = [&snapshot, cpu,
                                              cpuTimingMatchesCompletedScene,
                                              &cpuTimingReason,
                                              includeCpuTiming](
                                                 const char* code,
                                                 const char* description,
                                                 uint64 value)
            {
                if (!includeCpuTiming)
                {
                    return;
                }
                if (cpuTimingMatchesCompletedScene)
                {
                    snapshot.metrics.push_back(MakeAssessmentMetric(
                        code, "provenance", description, value));
                    return;
                }
                snapshot.metrics.push_back(MakeUnavailableAssessmentMetric(
                    code, "provenance", description, cpuTimingReason));
            };
            appendCpuProvenance("RENDER.CPU.SOURCE_FRAME_SEQUENCE",
                                "Source frame sequence owning this CPU timing sample.",
                                cpu != nullptr ? cpu->sourceFrameSequence : 0);
            appendCpuProvenance("RENDER.CPU.REQUIRED_SCENE_REVISION",
                                "Scene revision required by the CPU timing sample.",
                                cpu != nullptr ? cpu->requiredSceneRevision : 0);
            appendCpuProvenance("RENDER.CPU.APPLIED_SCENE_REVISION",
                                "Scene revision applied by the CPU timing sample.",
                                cpu != nullptr ? cpu->appliedSceneRevision : 0);

            const bool gpuSupported =
                gpu != nullptr && gpu->timestampFrequency != 0;
            if (includeGpuCapabilityObservation)
            {
                AssessmentCapability gpuCapability;
                gpuCapability.code =
                    AssessmentCode("RENDER.GPU.GRAPHICS_TIMESTAMP");
                gpuCapability.description =
                    "Completion-owned Graphics timestamp timing can be observed.";
                gpuCapability.gating = false;

                AssessmentCapabilityObservation capabilityObservation;
                capabilityObservation.capability = std::move(gpuCapability);
                capabilityObservation.checkpoint =
                    AssessmentCheckpoints::ScenarioStable;
                capabilityObservation.value =
                    DiagnosticValue<bool>::Available(gpuSupported);
                capabilityObservation.reason = gpuSupported
                    ? "The startup RHI capability snapshot exposes Graphics timestamp timing."
                    : (gpu != nullptr &&
                       !gpu->terminalCriticalPathMilliseconds.GetReason().empty()
                           ? gpu->terminalCriticalPathMilliseconds.GetReason()
                           : GpuUnavailableReason);
                static_cast<void>(session.RecordCapabilityObservation(
                    std::move(capabilityObservation)));
            }

            const auto appendGpu = [&snapshot, gpu](
                                       const char* code,
                                       const char* unit,
                                       const char* description,
                                       auto value)
            {
                if (gpu != nullptr &&
                    gpu->terminalCriticalPathMilliseconds.IsAvailable() &&
                    value != 0)
                {
                    snapshot.metrics.push_back(MakeAssessmentMetric(
                        code, unit, description, value));
                    return;
                }
                const std::string reason =
                    gpu != nullptr
                        ? (gpu->terminalCriticalPathMilliseconds.IsAvailable()
                               ? "Completion-owned Graphics timing did not retain non-zero provenance."
                               : gpu->terminalCriticalPathMilliseconds.GetReason())
                        : GpuUnavailableReason;
                snapshot.metrics.push_back(MakeUnavailableAssessmentMetric(
                    code, unit, description, reason));
            };
            if (gpu != nullptr &&
                gpu->terminalCriticalPathMilliseconds.IsAvailable())
            {
                snapshot.metrics.push_back(MakeAssessmentMetric(
                    "RENDER.GPU.TERMINAL_CRITICAL_PATH_MS", "ms",
                    "Completion-owned Graphics prelude-to-terminal critical path.",
                    *gpu->terminalCriticalPathMilliseconds.GetValue()));
            }
            else
            {
                snapshot.metrics.push_back(MakeUnavailableAssessmentMetric(
                    "RENDER.GPU.TERMINAL_CRITICAL_PATH_MS", "ms",
                    "Completion-owned Graphics prelude-to-terminal critical path.",
                    gpu != nullptr
                        ? gpu->terminalCriticalPathMilliseconds.GetReason()
                        : GpuUnavailableReason));
            }
            appendGpu("RENDER.GPU.SOURCE_FRAME_SEQUENCE", "provenance",
                      "Source frame sequence explicitly bound to this GPU timing sample.",
                      gpu != nullptr ? gpu->sourceFrameSequence : uint64{0});
            appendGpu("RENDER.GPU.COMPLETION_VALUE", "provenance",
                      "Exact Graphics completion value that made this GPU sample readable.",
                      gpu != nullptr ? gpu->completionValue : uint64{0});

            if (includeGpuLossFinding && gpuSupported && gpu != nullptr &&
                (gpu->lostSampleCount != 0 || gpu->droppedSampleCount != 0))
            {
                Finding finding;
                finding.code = AssessmentCode("RENDER.GPU.TIMING.SAMPLE_LOSS");
                finding.subsystemCode = AssessmentCode("RENDER.GPU");
                finding.invariantCode =
                    AssessmentCode("RENDER.GPU.TIMING.COMPLETION_INTEGRITY");
                finding.checkpoint = AssessmentCheckpoints::ScenarioStable;
                finding.classification = FindingClass::InstrumentationGap;
                finding.severity = requireGpuTiming ? FindingSeverity::Error
                                                    : FindingSeverity::Warning;
                finding.confidence = FindingConfidence::Confirmed;
                finding.summary =
                    "Completion-owned Graphics timing lost or dropped one or more samples.";
                finding.expected = "0 lost and 0 dropped Graphics timing samples.";
                finding.observed = "lost=" +
                    std::to_string(gpu->lostSampleCount) + ", dropped=" +
                    std::to_string(gpu->droppedSampleCount);
                finding.gating = requireGpuTiming;
                if (requireGpuTiming)
                {
                    finding.blockingReason =
                        "Required Graphics timing provenance is incomplete.";
                }
                static_cast<void>(session.RecordFinding(std::move(finding)));
            }
        }

        void AddAssessmentContractMetric(SampleAssessmentContract& contract,
                                         const char* code,
                                         const char* unit,
                                         const char* description)
        {
            const AssessmentCode assessmentCode(code);
            const auto existing = std::find_if(
                contract.metrics.begin(), contract.metrics.end(),
                [&assessmentCode](const AssessmentMetric& metric)
                {
                    return metric.code == assessmentCode;
                });
            if (existing == contract.metrics.end())
            {
                contract.metrics.push_back(
                    {assessmentCode, unit, description});
            }
        }

        void ExtendAssessmentContractWithTiming(
            SampleAssessmentContract& contract,
            SampleAssessmentProfile profile,
            bool graphicsTimestampSupported)
        {
            if (contract.revision.find("timing-v1") == std::string::npos)
            {
                contract.revision += contract.revision.empty() ? "timing-v1"
                                                               : "+timing-v1";
            }

            const AssessmentCode capabilityCode(
                "RENDER.GPU.GRAPHICS_TIMESTAMP");
            const auto capability = std::find_if(
                contract.capabilities.begin(), contract.capabilities.end(),
                [&capabilityCode](const AssessmentCapability& candidate)
                {
                    return candidate.code == capabilityCode;
                });
            if (capability == contract.capabilities.end())
            {
                contract.capabilities.push_back(
                    {capabilityCode,
                     "Completion-owned Graphics timestamp timing can be observed.",
                     false,
                     {}});
            }

            if (profile == SampleAssessmentProfile::Smoke)
            {
                return;
            }

            static constexpr std::array<std::tuple<const char*, const char*, const char*>, 13>
                CpuMetrics{{
                    {"RENDER.CPU.FRAME_APPLY_NS", "ns", "CPU duration of persistent RenderScene frame application."},
                    {"RENDER.CPU.FRAME_BEGIN_ACQUIRE_NS", "ns", "CPU duration of the complete frame-slot acquire and begin call."},
                    {"RENDER.CPU.RENDER_PREPARE_NS", "ns", "CPU duration of SceneRenderer frame preparation."},
                    {"RENDER.CPU.POLICY_NS", "ns", "CPU duration of render policy configuration and resolution."},
                    {"RENDER.CPU.GRAPH_BUILD_NS", "ns", "CPU duration of RenderGraph definition construction."},
                    {"RENDER.CPU.GRAPH_COMPILE_NS", "ns", "CPU duration of immutable RenderGraph plan compilation."},
                    {"RENDER.CPU.GRAPH_REALIZE_RECORD_NS", "ns", "CPU duration of graph realization and command recording."},
                    {"RENDER.CPU.FRAME_END_SUBMIT_NS", "ns", "CPU duration of the complete frame submission call."},
                    {"RENDER.CPU.PRESENT_NS", "ns", "CPU duration of the presentation call."},
                    {"RENDER.CPU.ACCEPTED_FRAME_TOTAL_NS", "ns", "CPU duration of the complete successful ConsumeFrameV5 call."},
                    {"RENDER.CPU.SOURCE_FRAME_SEQUENCE", "provenance", "Source frame sequence owning this CPU timing sample."},
                    {"RENDER.CPU.REQUIRED_SCENE_REVISION", "provenance", "Scene revision required by the CPU timing sample."},
                    {"RENDER.CPU.APPLIED_SCENE_REVISION", "provenance", "Scene revision applied by the CPU timing sample."},
                }};
            for (const auto& [code, unit, description] : CpuMetrics)
            {
                AddAssessmentContractMetric(contract, code, unit, description);
            }

            if (graphicsTimestampSupported)
            {
                AddAssessmentContractMetric(
                    contract, "RENDER.GPU.TERMINAL_CRITICAL_PATH_MS", "ms",
                    "Completion-owned Graphics prelude-to-terminal critical path.");
                AddAssessmentContractMetric(
                    contract, "RENDER.GPU.SOURCE_FRAME_SEQUENCE", "provenance",
                    "Source frame sequence explicitly bound to this GPU timing sample.");
                AddAssessmentContractMetric(
                    contract, "RENDER.GPU.COMPLETION_VALUE", "provenance",
                    "Exact Graphics completion value that made this GPU sample readable.");
            }
        }

        void AppendRenderUploadWorkAssessmentMetrics(
            AssessmentSnapshot& snapshot,
            const char* codePrefix,
            const RenderUploadWorkDiagnostics& diagnostics)
        {
            const std::string prefix(codePrefix);
            snapshot.metrics.push_back(MakeAssessmentMetric(
                (prefix + ".CPU_COPIED_BYTES").c_str(), "bytes",
                "CPU payload bytes copied through mapped upload ranges.",
                diagnostics.cpuCopiedPayloadBytes));
            snapshot.metrics.push_back(MakeAssessmentMetric(
                (prefix + ".COMMITTED_BYTES").c_str(), "bytes",
                "Payload bytes with a successful mapped-write receipt.",
                diagnostics.committedPayloadBytes));
            snapshot.metrics.push_back(MakeAssessmentMetric(
                (prefix + ".MAPPED_RANGE_COUNT").c_str(), "count",
                "Contiguous mapped upload ranges attempted.",
                static_cast<uint64>(diagnostics.mappedRangeCount)));
            snapshot.metrics.push_back(MakeAssessmentMetric(
                (prefix + ".COMMITTED_RANGE_COUNT").c_str(), "count",
                "Contiguous mapped upload ranges with a successful receipt.",
                static_cast<uint64>(diagnostics.committedRangeCount)));
            snapshot.metrics.push_back(MakeAssessmentMetric(
                (prefix + ".GPU_COPY_BYTES").c_str(), "bytes",
                "GPU copy bytes associated with a valid submission token.",
                diagnostics.gpuCopyBytes));
            snapshot.metrics.push_back(MakeAssessmentMetric(
                (prefix + ".GPU_COPY_RANGE_COUNT").c_str(), "count",
                "GPU copy ranges associated with a valid submission token.",
                static_cast<uint64>(diagnostics.gpuCopyRangeCount)));
            if (diagnostics.hostVisibilitySynchronizedBytes.IsAvailable())
            {
                snapshot.metrics.push_back(MakeAssessmentMetric(
                    (prefix + ".HOST_VISIBILITY_SYNCHRONIZED_BYTES").c_str(),
                    "bytes",
                    "Host-visible bytes evidenced by mapped-write receipts.",
                    *diagnostics.hostVisibilitySynchronizedBytes.GetValue()));
            }
            else
            {
                snapshot.metrics.push_back(MakeUnavailableAssessmentMetric(
                    (prefix + ".HOST_VISIBILITY_SYNCHRONIZED_BYTES").c_str(),
                    "bytes",
                    "Host-visible bytes evidenced by mapped-write receipts.",
                    diagnostics.hostVisibilitySynchronizedBytes.GetReason()));
            }
        }

        void AppendResourceDiagnosticMetric(
            AssessmentSnapshot& snapshot,
            const char* code,
            const char* unit,
            const char* description,
            const DiagnosticValue<uint64>& value)
        {
            if (value.IsAvailable())
            {
                snapshot.metrics.push_back(MakeAssessmentMetric(
                    code, unit, description, *value.GetValue()));
                return;
            }

            snapshot.metrics.push_back(MakeUnavailableAssessmentMetric(
                code, unit, description, value.GetReason()));
        }

        void AppendResourceDiagnosticsMetrics(
            AssessmentSnapshot& snapshot,
            const ResourceDiagnosticsSnapshot& diagnostics)
        {
            const auto appendCount =
                [&snapshot](const char* code,
                            const char* description,
                            uint64 value)
                {
                    snapshot.metrics.push_back(MakeAssessmentMetric(
                        code, "count", description, value));
                };
            const auto appendBytes =
                [&snapshot](const char* code,
                            const char* description,
                            uint64 value)
                {
                    snapshot.metrics.push_back(MakeAssessmentMetric(
                        code, "bytes", description, value));
                };

            appendCount("RESOURCE.ACTIVE_OPERATIONS",
                        "Active coalesced resource operations.",
                        diagnostics.activeOperations);
            appendCount("RESOURCE.ACTIVE_SUBSCRIBERS",
                        "Active resource request subscribers.",
                        diagnostics.activeSubscribers);
            appendCount("RESOURCE.PENDING_ASYNC_JOBS",
                        "Worker resource jobs that have not completed.",
                        diagnostics.pendingAsyncJobs);
            appendCount("RESOURCE.PENDING_ASYNC_COMPLETIONS",
                        "Worker completions awaiting owner-thread publication.",
                        diagnostics.pendingAsyncCompletions);

            appendCount("RESOURCE.CACHE_ENTRY_COUNT",
                        "Resource cache entry count.",
                        diagnostics.cacheEntryCount);
            appendBytes("RESOURCE.CACHE_CPU_BYTES",
                        "CPU bytes retained by the resource cache.",
                        diagnostics.cacheCPUBytes);
            appendBytes("RESOURCE.CACHE_GPU_BYTES",
                        "GPU-byte accounting retained by the resource cache.",
                        diagnostics.cacheGPUBytes);
            appendCount("RESOURCE.CACHE_HITS",
                        "Resource cache hits.", diagnostics.cacheHits);
            appendCount("RESOURCE.CACHE_MISSES",
                        "Resource cache misses.", diagnostics.cacheMisses);
            AppendResourceDiagnosticMetric(
                snapshot,
                "RESOURCE.SOURCE_READ_OPERATIONS",
                "count",
                "Measured source-read operations.",
                diagnostics.sourceReadOperationCount);
            AppendResourceDiagnosticMetric(
                snapshot,
                "RESOURCE.SOURCE_READ_BYTES",
                "bytes",
                "Measured source bytes read.",
                diagnostics.sourceReadBytes);
            appendCount("RESOURCE.CANCELLED_LOADS",
                        "Cancelled resource load operations.",
                        diagnostics.cancelledLoadCount);

            appendCount("RESOURCE.DECODE_QUEUED",
                        "Texture decodes queued under the decoded-byte budget.",
                        diagnostics.decodeQueuedCount);
            appendCount("RESOURCE.DECODE_ACTIVE",
                        "Texture decodes currently executing.",
                        diagnostics.decodeActiveCount);
            appendCount("RESOURCE.DECODE_COMPLETED",
                        "Completed texture decodes.",
                        diagnostics.decodeCompletedCount);
            appendCount("RESOURCE.DECODE_FAILED",
                        "Failed texture decodes.",
                        diagnostics.decodeFailedCount);
            appendCount("RESOURCE.DECODE_CANCELLED",
                        "Cancelled texture decodes.",
                        diagnostics.decodeCancelledCount);
            appendBytes("RESOURCE.DECODE_COMPLETED_BYTES",
                        "Decoded texture bytes completed by streaming.",
                        diagnostics.decodeCompletedBytes);
            appendBytes("RESOURCE.DECODE_RESERVED_BYTES",
                        "Decoded-byte budget currently reserved.",
                        diagnostics.decodeReservedBytes);
            appendBytes("RESOURCE.DECODE_PEAK_RESERVED_BYTES",
                        "Peak decoded-byte budget reservation.",
                        diagnostics.decodePeakReservedBytes);
            appendBytes("RESOURCE.DECODE_BUDGET_BYTES",
                        "Configured decoded-byte budget.",
                        diagnostics.decodeBudgetBytes);

            appendCount("RESOURCE.PENDING_PUBLICATIONS",
                        "Prepared bundles awaiting owner publication.",
                        diagnostics.pendingPublicationCount);
            appendCount("RESOURCE.PENDING_UPLOADS",
                        "Resource uploads not yet terminal.",
                        diagnostics.pendingUploadCount);
            appendCount("RESOURCE.PENDING_REPLACEMENTS",
                        "Transactional content replacements not yet terminal.",
                        diagnostics.pendingReplacementCount);
            appendCount("RESOURCE.PENDING_ROLLBACKS",
                        "Local terminal upload requests awaiting rollback cleanup.",
                        diagnostics.pendingRollbackCount);
            appendCount("RESOURCE.PENDING_RETIREMENTS",
                        "Resource retirements awaiting GPU completion.",
                        diagnostics.pendingRetirementCount);

            appendCount("RESOURCE.ACTIVE_LEASES",
                        "Active asset residency leases.",
                        diagnostics.activeLeaseCount);
            appendCount("RESOURCE.PROTECTED_RESOURCES",
                        "Resources protected from eviction by residency leases.",
                        diagnostics.protectedResourceCount);
            appendCount("RESOURCE.QUEUED_LEASE_UNLOADS",
                        "Unload requests deferred by residency leases.",
                        diagnostics.queuedLeaseUnloadCount);
            appendCount("RESOURCE.LEASE_BLOCKED_EVICTIONS",
                        "Evictions blocked by residency leases.",
                        diagnostics.leaseBlockedEvictionCount);

            appendCount("RESOURCE.CLOSURE_UNLOAD_REQUESTS",
                        "Exact AssetKey closure-unload requests.",
                        diagnostics.closureUnloadRequestCount);
            appendCount("RESOURCE.CLOSURE_UNLOADED_RESOURCES",
                        "Resources released by exact closure unload.",
                        diagnostics.closureUnloadedResourceCount);
            appendCount("RESOURCE.CLOSURE_RETAINED_RESOURCES",
                        "Dependency resources retained by another owner.",
                        diagnostics.closureRetainedResourceCount);
            appendCount("RESOURCE.CLOSURE_UNLOAD_REJECTED",
                        "Rejected exact closure-unload requests.",
                        diagnostics.closureUnloadRejectedCount);
        }

        void AppendUnavailableResourceDiagnosticsMetrics(
            AssessmentSnapshot& snapshot,
            std::string reason)
        {
            constexpr std::array<std::tuple<const char*, const char*, const char*>, 34>
                Metrics = {{
                    {"RESOURCE.ACTIVE_OPERATIONS", "count", "Active coalesced resource operations."},
                    {"RESOURCE.ACTIVE_SUBSCRIBERS", "count", "Active resource request subscribers."},
                    {"RESOURCE.PENDING_ASYNC_JOBS", "count", "Worker resource jobs that have not completed."},
                    {"RESOURCE.PENDING_ASYNC_COMPLETIONS", "count", "Worker completions awaiting owner-thread publication."},
                    {"RESOURCE.CACHE_ENTRY_COUNT", "count", "Resource cache entry count."},
                    {"RESOURCE.CACHE_CPU_BYTES", "bytes", "CPU bytes retained by the resource cache."},
                    {"RESOURCE.CACHE_GPU_BYTES", "bytes", "GPU-byte accounting retained by the resource cache."},
                    {"RESOURCE.CACHE_HITS", "count", "Resource cache hits."},
                    {"RESOURCE.CACHE_MISSES", "count", "Resource cache misses."},
                    {"RESOURCE.SOURCE_READ_OPERATIONS", "count", "Measured source-read operations."},
                    {"RESOURCE.SOURCE_READ_BYTES", "bytes", "Measured source bytes read."},
                    {"RESOURCE.CANCELLED_LOADS", "count", "Cancelled resource load operations."},
                    {"RESOURCE.DECODE_QUEUED", "count", "Texture decodes queued under the decoded-byte budget."},
                    {"RESOURCE.DECODE_ACTIVE", "count", "Texture decodes currently executing."},
                    {"RESOURCE.DECODE_COMPLETED", "count", "Completed texture decodes."},
                    {"RESOURCE.DECODE_FAILED", "count", "Failed texture decodes."},
                    {"RESOURCE.DECODE_CANCELLED", "count", "Cancelled texture decodes."},
                    {"RESOURCE.DECODE_COMPLETED_BYTES", "bytes", "Decoded texture bytes completed by streaming."},
                    {"RESOURCE.DECODE_RESERVED_BYTES", "bytes", "Decoded-byte budget currently reserved."},
                    {"RESOURCE.DECODE_PEAK_RESERVED_BYTES", "bytes", "Peak decoded-byte budget reservation."},
                    {"RESOURCE.DECODE_BUDGET_BYTES", "bytes", "Configured decoded-byte budget."},
                    {"RESOURCE.PENDING_PUBLICATIONS", "count", "Prepared bundles awaiting owner publication."},
                    {"RESOURCE.PENDING_UPLOADS", "count", "Resource uploads not yet terminal."},
                    {"RESOURCE.PENDING_REPLACEMENTS", "count", "Transactional content replacements not yet terminal."},
                    {"RESOURCE.PENDING_ROLLBACKS", "count", "Local terminal upload requests awaiting rollback cleanup."},
                    {"RESOURCE.PENDING_RETIREMENTS", "count", "Resource retirements awaiting GPU completion."},
                    {"RESOURCE.ACTIVE_LEASES", "count", "Active asset residency leases."},
                    {"RESOURCE.PROTECTED_RESOURCES", "count", "Resources protected from eviction by residency leases."},
                    {"RESOURCE.QUEUED_LEASE_UNLOADS", "count", "Unload requests deferred by residency leases."},
                    {"RESOURCE.LEASE_BLOCKED_EVICTIONS", "count", "Evictions blocked by residency leases."},
                    {"RESOURCE.CLOSURE_UNLOAD_REQUESTS", "count", "Exact AssetKey closure-unload requests."},
                    {"RESOURCE.CLOSURE_UNLOADED_RESOURCES", "count", "Resources released by exact closure unload."},
                    {"RESOURCE.CLOSURE_RETAINED_RESOURCES", "count", "Dependency resources retained by another owner."},
                    {"RESOURCE.CLOSURE_UNLOAD_REJECTED", "count", "Rejected exact closure-unload requests."},
                }};
            for (const auto& [code, unit, description] : Metrics)
            {
                snapshot.metrics.push_back(MakeUnavailableAssessmentMetric(
                    code, unit, description, reason));
            }
        }

        bool RecordRuntimeAssessmentSnapshot(
            FrameworkAssessmentSession& session,
            const AssessmentCheckpoint& checkpoint,
            const SceneECS::SceneEcsRuntime* scene,
            const Resource::IResourceDiagnosticsView* resourceDiagnostics,
            const IWorldEcsRuntimeServices* runtimeServices,
            const RenderDiagnosticsSnapshot* renderDiagnostics,
            const Engine* engine,
            uint64 worldCount,
            bool requireGpuTiming)
        {
            AssessmentSnapshot snapshot;
            snapshot.checkpoint = checkpoint;

            if (scene != nullptr)
            {
                const SceneECS::SceneEcsDiagnosticsSnapshot diagnostics =
                    scene->GetDiagnosticsSnapshot();
                snapshot.metrics.push_back(MakeAssessmentMetric(
                    "SCENE.ENTITY_COUNT", "count", "Live ECS entity count.",
                    static_cast<uint64>(diagnostics.entityCount)));
                snapshot.metrics.push_back(MakeAssessmentMetric(
                    "SCENE.PENDING_DESTROY_COUNT", "count",
                    "ECS entities awaiting cleanup.",
                    static_cast<uint64>(diagnostics.pendingDestroyCount)));
                snapshot.metrics.push_back(MakeAssessmentMetric(
                    "SCENE.CLEANUP_REQUIRED_COUNT", "count",
                    "ECS entities waiting for external cleanup.",
                    static_cast<uint64>(diagnostics.cleanupRequiredCount)));
                snapshot.metrics.push_back(MakeAssessmentMetric(
                    "SCENE.RETIRING_COUNT", "count",
                    "ECS entities waiting for retirement proof.",
                    static_cast<uint64>(diagnostics.retiringCount)));
                snapshot.metrics.push_back(MakeAssessmentMetric(
                    "SCENE.RECYCLABLE_COUNT", "count",
                    "ECS entities ready for generation recycle.",
                    static_cast<uint64>(diagnostics.recyclableCount)));
                snapshot.metrics.push_back(MakeAssessmentMetric(
                    "SCENE.SNAPSHOT_REVISION", "revision",
                    "Frozen ECS scene snapshot revision.",
                    diagnostics.sceneSnapshotRevision));
                snapshot.metrics.push_back(MakeAssessmentMetric(
                    "SCENE.STRUCTURAL_SEQUENCE", "sequence",
                    "Next ECS structural journal sequence.",
                    diagnostics.nextStructuralSequence));
                snapshot.metrics.push_back(MakeAssessmentMetric(
                    "SCENE.COMMAND_BUFFER_QUEUED", "count",
                    "Queued ECS command buffers.",
                    static_cast<uint64>(diagnostics.queuedCommandBufferCount)));
                snapshot.metrics.push_back(MakeAssessmentMetric(
                    "SCENE.COMMAND_BUFFER_APPLIED", "count",
                    "Applied ECS command buffers.", diagnostics.appliedCommandBufferCount));
                snapshot.metrics.push_back(MakeAssessmentMetric(
                    "SCENE.COMMAND_BUFFER_REJECTED", "count",
                    "Rejected ECS command buffers.", diagnostics.rejectedCommandBufferCount));
                snapshot.metrics.push_back(MakeAssessmentMetric(
                    "SCENE.CLEANUP_CONTINUITY_LOSS", "count",
                    "ECS cleanup journal continuity losses.",
                    diagnostics.cleanupContinuityLossCount));
                snapshot.metrics.push_back(MakeAssessmentMetric(
                    "SCENE.LOCAL_TRANSFORM_WRITE_VERSION", "version",
                    "Local transform fragment write version.",
                    diagnostics.localTransformWriteVersion));
            }
            else
            {
                constexpr const char* Reason =
                    "ECS scene runtime is unavailable at this lifecycle checkpoint.";
                static constexpr std::array<std::tuple<const char*, const char*, const char*>, 12>
                    Metrics{{
                        {"SCENE.ENTITY_COUNT", "count", "Live ECS entity count."},
                        {"SCENE.PENDING_DESTROY_COUNT", "count", "ECS entities awaiting cleanup."},
                        {"SCENE.CLEANUP_REQUIRED_COUNT", "count", "ECS entities waiting for external cleanup."},
                        {"SCENE.RETIRING_COUNT", "count", "ECS entities waiting for retirement proof."},
                        {"SCENE.RECYCLABLE_COUNT", "count", "ECS entities ready for generation recycle."},
                        {"SCENE.SNAPSHOT_REVISION", "revision", "Frozen ECS scene snapshot revision."},
                        {"SCENE.STRUCTURAL_SEQUENCE", "sequence", "Next ECS structural journal sequence."},
                        {"SCENE.COMMAND_BUFFER_QUEUED", "count", "Queued ECS command buffers."},
                        {"SCENE.COMMAND_BUFFER_APPLIED", "count", "Applied ECS command buffers."},
                        {"SCENE.COMMAND_BUFFER_REJECTED", "count", "Rejected ECS command buffers."},
                        {"SCENE.CLEANUP_CONTINUITY_LOSS", "count", "ECS cleanup journal continuity losses."},
                        {"SCENE.LOCAL_TRANSFORM_WRITE_VERSION", "version", "Local transform fragment write version."},
                    }};
                for (const auto& [code, unit, description] : Metrics)
                {
                    snapshot.metrics.push_back(MakeUnavailableAssessmentMetric(
                        code, unit, description, Reason));
                }
            }

            if (resourceDiagnostics != nullptr)
            {
                const Resource::ResourceDiagnosticsQueryResult result =
                    resourceDiagnostics->QueryResourceDiagnostics();
                if (result.IsAvailable())
                {
                    AppendResourceDiagnosticsMetrics(
                        snapshot, *result.snapshot.GetValue());
                }
                else
                {
                    AppendUnavailableResourceDiagnosticsMetrics(
                        snapshot, result.snapshot.GetReason());
                }
            }
            else
            {
                AppendUnavailableResourceDiagnosticsMetrics(
                    snapshot,
                    "Resource subsystem diagnostics view is unavailable.");
            }

            if (runtimeServices != nullptr)
            {
                const WorldEcsRuntimeServicesDiagnostics diagnostics =
                    runtimeServices->GetRuntimeDiagnostics();
                if (diagnostics.available && diagnostics.physicsInitialized)
                {
                    snapshot.metrics.push_back(MakeAssessmentMetric(
                        "PHYSICS.BODY_COUNT", "count", "Physics body count.",
                        static_cast<uint64>(diagnostics.activePhysicsBodyCount)));
                    snapshot.metrics.push_back(MakeAssessmentMetric(
                        "PHYSICS.FIXED_STEPS", "count",
                        "Completed fixed physics steps.",
                        diagnostics.executedFixedStepCount));
                }
                else
                {
                    constexpr const char* Reason =
                        "The ECS Physics runtime is unavailable.";
                    snapshot.metrics.push_back(MakeUnavailableAssessmentMetric(
                        "PHYSICS.BODY_COUNT", "count", "Physics body count.",
                        Reason));
                    snapshot.metrics.push_back(MakeUnavailableAssessmentMetric(
                        "PHYSICS.FIXED_STEPS", "count",
                        "Completed fixed physics steps.", Reason));
                }
            }
            else
            {
                constexpr const char* Reason =
                    "The ECS World runtime service is unavailable.";
                snapshot.metrics.push_back(MakeUnavailableAssessmentMetric(
                    "PHYSICS.BODY_COUNT", "count", "Physics body count.",
                    Reason));
                snapshot.metrics.push_back(MakeUnavailableAssessmentMetric(
                    "PHYSICS.FIXED_STEPS", "count",
                    "Completed fixed physics steps.", Reason));
            }

            if (renderDiagnostics != nullptr)
            {
                snapshot.metrics.push_back(MakeAssessmentMetric(
                    "RENDER.SUBMITTED_SEQUENCE", "sequence",
                    "Latest submitted render frame sequence.",
                    renderDiagnostics->lastSubmittedFrameSequence));
                snapshot.metrics.push_back(MakeAssessmentMetric(
                    "RENDER.PRESENTED_SEQUENCE", "sequence",
                    "Latest presented render frame sequence.",
                    renderDiagnostics->lastPresentedFrameSequence));
                snapshot.metrics.push_back(MakeAssessmentMetric(
                    "RENDER.PENDING_UPLOADS", "count",
                    "Render-side pending uploads.",
                    static_cast<uint64>(
                        renderDiagnostics->frameFeatures.pendingUploadCount)));
                const RenderGPUDrivenCullingDiagnostics& culling =
                    renderDiagnostics->frameFeatures.gpuDrivenCulling;
                AppendRenderUploadWorkAssessmentMetrics(
                    snapshot,
                    "RENDER.UPLOAD.GPU_DRIVEN.CANONICAL_INSTANCE",
                    culling.canonicalInstanceUploadWork);
                AppendRenderUploadWorkAssessmentMetrics(
                    snapshot,
                    "RENDER.UPLOAD.GPU_DRIVEN.CANONICAL_CANDIDATE",
                    culling.canonicalCandidateUploadWork);
                AppendRenderUploadWorkAssessmentMetrics(
                    snapshot,
                    "RENDER.UPLOAD.GPU_DRIVEN.CANONICAL_ACTIVE_ROW",
                    culling.canonicalActiveRowUploadWork);
                AppendRenderUploadWorkAssessmentMetrics(
                    snapshot,
                    "RENDER.UPLOAD.DIRECT_RASTER.INSTANCE",
                    culling.directRasterInstanceUploadWork);
                AppendRenderUploadWorkAssessmentMetrics(
                    snapshot,
                    "RENDER.UPLOAD.DIRECT_RASTER.INDEX",
                    culling.directRasterIndexUploadWork);
                AppendRenderUploadWorkAssessmentMetrics(
                    snapshot,
                    "RENDER.UPLOAD.GPU_SCENE",
                    renderDiagnostics->frameFeatures.gpuScene.uploadWork);
                static constexpr std::array<const char*,
                                             GPU_SCENE_DIAGNOSTICS_TABLE_COUNT>
                    GPUSceneUploadMetricPrefixes{{
                        "RENDER.UPLOAD.GPU_SCENE.PRIMITIVES",
                        "RENDER.UPLOAD.GPU_SCENE.BOUNDS",
                        "RENDER.UPLOAD.GPU_SCENE.TRANSFORMS",
                        "RENDER.UPLOAD.GPU_SCENE.MATERIALS",
                        "RENDER.UPLOAD.GPU_SCENE.GEOMETRIES",
                        "RENDER.UPLOAD.GPU_SCENE.DRAWS"}};
                for (uint32 tableIndex = 0;
                     tableIndex < GPU_SCENE_DIAGNOSTICS_TABLE_COUNT;
                     ++tableIndex)
                {
                    AppendRenderUploadWorkAssessmentMetrics(
                        snapshot,
                        GPUSceneUploadMetricPrefixes[tableIndex],
                        renderDiagnostics->frameFeatures.gpuScene
                            .tables[tableIndex].uploadWork);
                }
                if (renderDiagnostics->sceneValues.available)
                {
                    snapshot.metrics.push_back(MakeAssessmentMetric(
                        "RENDER.SCENE_APPLIED_REVISION", "revision",
                        "Persistent RenderScene revision consumed by the renderer.",
                        renderDiagnostics->sceneValues.appliedSceneRevision));
                    snapshot.metrics.push_back(MakeAssessmentMetric(
                        "RENDER.SCENE_REQUIRED_REVISION", "revision",
                        "Scene revision required by the consumed frame packet.",
                        renderDiagnostics->sceneValues.requiredSceneRevision));
                    snapshot.metrics.push_back(MakeAssessmentMetric(
                        "RENDER.SCENE_LIGHT_STATE_HASH", "hash",
                        "Deterministic retained RenderScene light-state hash.",
                        renderDiagnostics->sceneValues.lightStateHash));
                }
                else
                {
                    constexpr const char* SceneReason =
                        "No RenderScene frame has completed consumption at this checkpoint.";
                    snapshot.metrics.push_back(MakeUnavailableAssessmentMetric(
                        "RENDER.SCENE_APPLIED_REVISION", "revision",
                        "Persistent RenderScene revision consumed by the renderer.",
                        SceneReason));
                    snapshot.metrics.push_back(MakeUnavailableAssessmentMetric(
                        "RENDER.SCENE_REQUIRED_REVISION", "revision",
                        "Scene revision required by the consumed frame packet.",
                        SceneReason));
                    snapshot.metrics.push_back(MakeUnavailableAssessmentMetric(
                        "RENDER.SCENE_LIGHT_STATE_HASH", "hash",
                        "Deterministic retained RenderScene light-state hash.",
                        SceneReason));
                }
            }
            else
            {
                constexpr const char* Reason =
                    "Render diagnostics are unavailable at this checkpoint.";
                snapshot.metrics.push_back(MakeUnavailableAssessmentMetric(
                    "RENDER.SUBMITTED_SEQUENCE", "sequence",
                    "Latest submitted render frame sequence.", Reason));
                snapshot.metrics.push_back(MakeUnavailableAssessmentMetric(
                    "RENDER.PRESENTED_SEQUENCE", "sequence",
                    "Latest presented render frame sequence.", Reason));
                snapshot.metrics.push_back(MakeUnavailableAssessmentMetric(
                    "RENDER.SCENE_APPLIED_REVISION", "revision",
                    "Persistent RenderScene revision consumed by the renderer.",
                    Reason));
                snapshot.metrics.push_back(MakeUnavailableAssessmentMetric(
                    "RENDER.SCENE_REQUIRED_REVISION", "revision",
                    "Scene revision required by the consumed frame packet.",
                    Reason));
            }

            const bool isStableTimingCheckpoint =
                checkpoint.code == AssessmentCheckpoints::ScenarioStable.code;
            const bool isFinalGpuTimingCheckpoint =
                checkpoint.code ==
                AssessmentCheckpoints::TeardownRenderDrained.code;
            if (isStableTimingCheckpoint || isFinalGpuTimingCheckpoint)
            {
                // CPU phases belong to the exact stable presented frame. GPU
                // samples are delayed completion-owned values: retain the
                // stable observation, then re-observe after the normal Render
                // drain so a valid last-frame sample cannot be lost merely
                // because its fence completed after readiness was latched.
                AppendFrameTimingAssessmentMetrics(
                    session,
                    snapshot,
                    renderDiagnostics,
                    requireGpuTiming,
                    isStableTimingCheckpoint,
                    isStableTimingCheckpoint,
                    isFinalGpuTimingCheckpoint);
            }

            if (engine != nullptr)
            {
                const EngineRenderRuntimeDiagnostics engineDiagnostics =
                    engine->GetRenderRuntimeDiagnostics();
                if (engineDiagnostics.available)
                {
                    snapshot.metrics.push_back(MakeAssessmentMetric(
                        "ENGINE.REQUIRED_SCENE_REVISION", "revision",
                        "Latest Scene revision accepted for render publication.",
                        engineDiagnostics.requiredSceneRevision));
                    snapshot.metrics.push_back(MakeAssessmentMetric(
                        "ENGINE.TEMPORAL_EPOCH", "epoch",
                        "Current temporal-history epoch.",
                        engineDiagnostics.temporalEpoch));
                    snapshot.metrics.push_back(MakeAssessmentMetric(
                        "ENGINE.CAMERA_CUT_REVISION", "revision",
                        "Observed active ECS camera cut revision.",
                        engineDiagnostics.cameraCutRevision));
                }
                else
                {
                    constexpr const char* EngineReason =
                        "Engine render composition is unavailable at this checkpoint.";
                    snapshot.metrics.push_back(MakeUnavailableAssessmentMetric(
                        "ENGINE.REQUIRED_SCENE_REVISION", "revision",
                        "Latest Scene revision accepted for render publication.",
                        EngineReason));
                    snapshot.metrics.push_back(MakeUnavailableAssessmentMetric(
                        "ENGINE.TEMPORAL_EPOCH", "epoch",
                        "Current temporal-history epoch.", EngineReason));
                    snapshot.metrics.push_back(MakeUnavailableAssessmentMetric(
                        "ENGINE.CAMERA_CUT_REVISION", "revision",
                        "Observed active ECS camera cut revision.",
                        EngineReason));
                }
            }

            snapshot.metrics.push_back(MakeAssessmentMetric(
                "ENGINE.WORLD_COUNT", "count",
                "World count at the checkpoint.", worldCount));
            return session.RecordSnapshot(std::move(snapshot));
        }

        Finding MakeHostFinding(const char* code,
                                const char* subsystem,
                                const char* invariant,
                                const AssessmentCheckpoint& checkpoint,
                                FindingClass classification,
                                FindingSeverity severity,
                                FindingConfidence confidence,
                                std::string summary,
                                std::string expected,
                                std::string observed,
                                bool gating,
                                std::string blockingReason = {})
        {
            Finding finding;
            finding.code = AssessmentCode(code);
            finding.subsystemCode = AssessmentCode(subsystem);
            finding.invariantCode = AssessmentCode(invariant);
            finding.checkpoint = checkpoint;
            finding.classification = classification;
            finding.severity = severity;
            finding.confidence = confidence;
            finding.summary = std::move(summary);
            finding.expected = std::move(expected);
            finding.observed = std::move(observed);
            finding.gating = gating;
            finding.blockingReason = std::move(blockingReason);
            return finding;
        }

        std::string GetAssessmentPlatformFingerprint();

        bool WriteEarlyFailureAssessment(
            const std::filesystem::path& reportPath,
            const ISample* sample,
            std::string_view sampleId,
            RHIBackendType requestedBackend,
            const char* findingCode,
            const char* subsystemCode,
            const AssessmentCheckpoint& checkpoint,
            std::string summary,
            std::string observed)
        {
            if (reportPath.empty())
            {
                return true;
            }

            SampleAssessmentContract contract =
                sample != nullptr ? sample->GetAssessmentContract()
                                  : MakeDefaultSampleAssessmentContract();
            AssessmentMetadata metadata;
            metadata.fingerprint.sampleId =
                sampleId.empty() ? "unavailable" : std::string(sampleId);
            metadata.fingerprint.sampleRevision =
                contract.revision.empty() ? "unavailable" : contract.revision;
            metadata.fingerprint.engineBuild =
                std::string(__DATE__) + 'T' + __TIME__;
            metadata.fingerprint.backend =
                GetSampleBackendName(requestedBackend);
            metadata.fingerprint.device = "unavailable";
            metadata.fingerprint.renderConfiguration =
                "startup-failure";
            metadata.fingerprint.assetSet = "unavailable";
            metadata.fingerprint.platform = GetAssessmentPlatformFingerprint();
            metadata.values.emplace("terminalState", "startup-failure");

            FrameworkAssessmentSession session(std::move(contract),
                                                std::move(metadata));
            static_cast<void>(session.RecordFinding(MakeHostFinding(
                findingCode,
                subsystemCode,
                "SAMPLE.HOST.INITIALIZED",
                checkpoint,
                FindingClass::ContractViolation,
                FindingSeverity::Fatal,
                FindingConfidence::Confirmed,
                std::move(summary),
                "The Sample host reaches the first assessment checkpoint.",
                std::move(observed),
                true,
                "The host could not establish a valid assessment runtime.")));
            const SampleAssessmentReport report = session.Finalize();
            std::string writeError;
            if (!FrameworkAssessmentJsonWriter::WriteFile(
                    reportPath, report, &writeError))
            {
                std::cerr << writeError << '\n';
                return false;
            }
            return true;
        }

        std::string GetAssessmentPlatformFingerprint()
        {
#if defined(_WIN32)
            constexpr const char* Platform = "windows";
#elif defined(__APPLE__)
            constexpr const char* Platform = "apple";
#elif defined(__linux__)
            constexpr const char* Platform = "linux";
#else
            constexpr const char* Platform = "unknown-os";
#endif
#if defined(_M_X64) || defined(__x86_64__)
            constexpr const char* Architecture = "x64";
#elif defined(_M_ARM64) || defined(__aarch64__)
            constexpr const char* Architecture = "arm64";
#else
            constexpr const char* Architecture = "unknown-arch";
#endif
            return std::string(Platform) + '-' + Architecture;
        }

        std::optional<AssessmentFingerprint> ReadAssessmentFingerprint(
            const std::filesystem::path& path,
            std::string& outError)
        {
            std::ifstream stream(path);
            if (!stream)
            {
                outError = "Failed to open assessment baseline: " +
                           path.string();
                return std::nullopt;
            }
            try
            {
                const nlohmann::json root = nlohmann::json::parse(stream);
                if (!root.is_object() ||
                    root.value("schema", std::string{}) !=
                        std::string(FrameworkAssessmentJsonWriter::SchemaName) ||
                    root.value("schemaVersion", 0u) !=
                        FrameworkAssessmentJsonWriter::SchemaVersion)
                {
                    outError =
                        "Assessment baseline must use RVX.FrameworkAssessmentReport schema v2";
                    return std::nullopt;
                }
                const nlohmann::json& fingerprint =
                    root.at("metadata").at("fingerprint");
                AssessmentFingerprint result;
                result.sampleId = fingerprint.at("sampleId").get<std::string>();
                result.sampleRevision =
                    fingerprint.at("sampleRevision").get<std::string>();
                result.engineBuild =
                    fingerprint.at("engineBuild").get<std::string>();
                result.backend = fingerprint.at("backend").get<std::string>();
                result.device = fingerprint.at("device").get<std::string>();
                result.renderConfiguration =
                    fingerprint.at("renderConfiguration").get<std::string>();
                result.assetSet = fingerprint.at("assetSet").get<std::string>();
                result.platform = fingerprint.at("platform").get<std::string>();
                if (!result.IsComplete())
                {
                    outError = "Assessment baseline fingerprint is incomplete";
                    return std::nullopt;
                }
                return result;
            }
            catch (const std::exception& exception)
            {
                outError = "Failed to parse assessment baseline: " +
                           std::string(exception.what());
                return std::nullopt;
            }
        }

        bool ParsePositiveRunnerUInt(std::string_view text, uint32& output)
        {
            if (text.empty())
            {
                return false;
            }
            uint32 value = 0;
            const std::from_chars_result result = std::from_chars(
                text.data(), text.data() + text.size(), value);
            if (result.ec != std::errc{} ||
                result.ptr != text.data() + text.size() || value == 0)
            {
                return false;
            }
            output = value;
            return true;
        }

        bool ParsePositiveRunnerUInt64(std::string_view text, uint64& output)
        {
            if (text.empty())
            {
                return false;
            }
            uint64 value = 0;
            const std::from_chars_result result = std::from_chars(
                text.data(), text.data() + text.size(), value);
            if (result.ec != std::errc{} ||
                result.ptr != text.data() + text.size() || value == 0)
            {
                return false;
            }
            output = value;
            return true;
        }

        bool ParsePositiveRunnerFloat64(std::string_view text,
                                        float64& output)
        {
            if (text.empty())
            {
                return false;
            }
            float64 value = 0.0;
            const std::from_chars_result result = std::from_chars(
                text.data(),
                text.data() + text.size(),
                value,
                std::chars_format::general);
            if (result.ec != std::errc{} ||
                result.ptr != text.data() + text.size() ||
                !std::isfinite(value) || value <= 0.0 || value > 1.0)
            {
                return false;
            }
            output = value;
            return true;
        }

        bool ParseRunnerRenderPath(std::string_view text,
                                   SampleRenderPath& output)
        {
            if (text == "auto")
            {
                output = SampleRenderPath::Auto;
                return true;
            }
            if (text == "direct")
            {
                output = SampleRenderPath::Direct;
                return true;
            }
            if (text == "gpu-driven")
            {
                output = SampleRenderPath::GPUDriven;
                return true;
            }
            return false;
        }

        bool ParseRunnerInstancingMode(std::string_view text,
                                       RenderInstancingMode& output)
        {
            if (text == "disabled")
            {
                output = RenderInstancingMode::Disabled;
                return true;
            }
            if (text == "auto")
            {
                output = RenderInstancingMode::Auto;
                return true;
            }
            return false;
        }

        bool ParseAssessmentProfile(std::string_view text,
                                    SampleAssessmentProfile& output)
        {
            if (text == "smoke")
            {
                output = SampleAssessmentProfile::Smoke;
                return true;
            }
            if (text == "qualification")
            {
                output = SampleAssessmentProfile::Qualification;
                return true;
            }
            if (text == "benchmark")
            {
                output = SampleAssessmentProfile::Benchmark;
                return true;
            }
            return false;
        }

        std::string DescribeFrameWait(
            const RuntimeFrameWaitResult& result,
            uint64 requestedTargetSequence)
        {
            std::ostringstream stream;
            stream << "waitCode=" << static_cast<uint32>(result.code)
                   << ", ticks=" << result.ticks
                   << ", requestedTarget=" << requestedTargetSequence
                   << ", published="
                   << result.diagnostics.lastPublishedFrameSequence
                   << ", acquired="
                   << result.diagnostics.lastAcquiredFrameSequence
                   << ", applied="
                   << result.diagnostics.lastAppliedFrameSequence
                   << ", submitted="
                   << result.diagnostics.lastSubmittedFrameSequence
                   << ", presented="
                   << result.diagnostics.lastPresentedFrameSequence
                   << ", frameTransportUsage="
                   << result.diagnostics.frameTransport.currentUsage
                   << ", frameTransportHighWater="
                   << result.diagnostics.frameTransport.highWaterMark
                   << ", frameTransportReplacements="
                   << result.diagnostics.frameTransport.replacements
                   << ", captureCode="
                   << static_cast<uint32>(
                          result.diagnostics.lastCapture.code)
                   << ", captureFrame="
                   << result.diagnostics.lastCapture.frameSequence
                   << ", captureBytes="
                   << result.diagnostics.lastCapture.bytes.size();
            if (result.diagnostics.lastFailure.available)
            {
                stream << ", failureCode="
                       << static_cast<uint32>(
                              result.diagnostics.lastFailure.runtime.code)
                       << ", failureClass="
                       << static_cast<uint32>(
                              result.diagnostics.lastFailure.runtime.resultClass)
                       << ", failureMessage="
                       << result.diagnostics.lastFailure.runtime.message
                       << ", failureContext="
                       << result.diagnostics.lastFailure.context;
            }
            return stream.str();
        }

        bool RequiresRunnerValue(std::string_view argument)
        {
            return argument == "--sample" || argument == "--asset" ||
                   argument == "--model" || argument == "--catalog" ||
                   argument == "--asset-root" ||
                   argument == "--cooked-shader-root" ||
                   argument == "--environment" ||
                   argument == "--environment-file" ||
                   argument == "--pipeline" ||
                   argument == "--view" ||
                   argument == "--fixed-delta" ||
                   argument == "--capture-tick" ||
                   argument == "--render-path" ||
                   argument == "--workload-scale" ||
                   argument == "--instancing" ||
                   argument == "--ready-timeout-ms" ||
                   argument == "--ready-max-frames" ||
                   argument == "--startup-report" ||
                   argument == "--lifetime-report" ||
                   argument == "--assessment-report" ||
                   argument == "--assessment-profile" ||
                   argument == "--assessment-baseline" ||
                   argument == "--qualification-bundle" ||
                   argument == "--qualification-series" ||
                   argument == "--qualification-performance" ||
                   argument == "--qualification-performance-baseline" ||
                   argument == "--lifetime-warmup-frames" ||
                   argument == "--lifetime-observation-frames" ||
                   argument == "--lifetime-min-duration-ms" ||
                   argument == "--lifetime-resize-interval" ||
                   argument == "--lifetime-resize-settle-frames";
        }

        struct ResolvedSampleAsset
        {
            SampleAssetEntry entry;
            std::filesystem::path catalogPath;
            std::filesystem::path assetRoot;
            std::optional<SampleCookedAssetAdmissionReceipt> cookAdmission;
            bool selected = false;
        };

        struct ResolvedSampleAssets
        {
            ResolvedSampleAsset model;
            std::vector<ResolvedSampleAsset> additionalModels;
            ResolvedSampleAsset environment;
            std::vector<ResolvedSampleAsset> animations;
            std::vector<ResolvedSampleAsset> effects;
        };

        struct AssetContentVerificationSummary
        {
            std::string assetSet;
            bool allSelectedAssetsVerified = false;
        };

        class StartupTimelineReporter final
        {
        public:
            explicit StartupTimelineReporter(std::filesystem::path path)
                : m_path(std::move(path))
            {
                if (m_path.empty())
                {
                    return;
                }

                Diagnostics::TraceSessionConfig config;
                config.enabled = true;
                config.name = "sample-startup";
                config.metadata.emplace("build", "RenderVerseX");
                m_session = Diagnostics::TraceSession::Create(std::move(config));
                m_rootContext = m_session->CreateRootContext();
                m_session->RecordInstant("ProcessStart", m_rootContext);
                m_rootSpan = m_session->BeginSpan("Sample.Startup",
                                                  m_rootContext);
            }

            ~StartupTimelineReporter()
            {
                static_cast<void>(Finalize());
            }

            StartupTimelineReporter(const StartupTimelineReporter&) = delete;
            StartupTimelineReporter& operator=(const StartupTimelineReporter&) = delete;

            [[nodiscard]] Diagnostics::TraceContext GetContext() const
            {
                return m_rootSpan.GetChildContext();
            }

            void SetMetadata(std::string name,
                             Diagnostics::TraceAttributeValue value)
            {
                if (m_session != nullptr)
                {
                    m_session->SetMetadata(std::move(name), std::move(value));
                }
            }

            [[nodiscard]] bool Finalize()
            {
                if (m_finalized)
                {
                    return m_written;
                }
                m_finalized = true;
                if (m_session == nullptr)
                {
                    m_written = true;
                    return true;
                }

                m_rootSpan.End();
                m_written = m_session->SaveJson(m_path);
                return m_written;
            }

        private:
            std::filesystem::path m_path;
            std::shared_ptr<Diagnostics::TraceSession> m_session;
            Diagnostics::TraceContext m_rootContext;
            Diagnostics::TraceSpan m_rootSpan;
            bool m_finalized = false;
            bool m_written = false;
        };

        std::filesystem::path GetExecutableDirectory(const char* argv0)
        {
#ifdef _WIN32
            char path[MAX_PATH] = {};
            const DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
            if (length > 0 && length < MAX_PATH)
            {
                return std::filesystem::path(path).parent_path();
            }
#endif
            if (argv0 && argv0[0] != '\0')
            {
                std::error_code error;
                const std::filesystem::path executable =
                    std::filesystem::absolute(argv0, error);
                if (!error)
                {
                    return executable.parent_path();
                }
            }
            return std::filesystem::current_path();
        }

        bool NormalizeExistingAssetPath(const std::filesystem::path& input,
                                        const char* label,
                                        std::filesystem::path& output,
                                        std::string& outError)
        {
            std::error_code error;
            const std::filesystem::path absolute = input.is_absolute()
                                                       ? input
                                                       : std::filesystem::absolute(input, error);
            if (error)
            {
                outError = std::string("Failed to resolve ") + label +
                           " path: " + error.message();
                return false;
            }
            output = std::filesystem::weakly_canonical(absolute, error);
            if (error || !std::filesystem::is_regular_file(output, error) || error)
            {
                outError = std::string(label) + " file does not exist: " +
                           input.string();
                return false;
            }
            return true;
        }

        void SetExplicitAsset(const std::filesystem::path& path,
                              SampleAssetKind kind,
                              const char* label,
                              ResolvedSampleAsset& output)
        {
            output.selected = true;
            output.entry.kind = kind;
            output.entry.resolvedPath = path;
            output.entry.source.name =
                std::string("Explicit command-line ") + label;
            output.entry.source.uri = path.string();
            output.entry.source.author = "Unknown";
            output.entry.attribution =
                "License and redistribution status are supplied by the user.";
            output.entry.redistributable = false;
        }

        bool ResolveCatalogAsset(const SampleAssetCatalog& catalog,
                                 std::string_view id,
                                 SampleAssetKind expectedKind,
                                 const char* role,
                                 bool smoke,
                                 ResolvedSampleAsset& output,
                                 std::string& outError)
        {
            const SampleAssetEntry* entry = catalog.Find(id);
            if (!entry)
            {
                outError = std::string("Unknown ") + role +
                           " asset id: " + std::string(id);
                return false;
            }
            if (entry->kind != expectedKind)
            {
                outError = std::string("Sample requires a ") + role +
                           " asset, but catalog entry '" + std::string(id) +
                           "' is " + GetSampleAssetKindName(entry->kind);
                return false;
            }
            if (smoke && !entry->redistributable)
            {
                outError = std::string("Smoke validation requires a redistributable ") +
                           role + " asset: " + std::string(id);
                return false;
            }

            output.entry = *entry;
            output.catalogPath = catalog.GetCatalogPath();
            output.assetRoot = catalog.GetAssetRoot();
            output.selected = true;
            return true;
        }

        std::vector<SampleModelExpectedContentIdentity>
        MakeModelContentIdentityRegistry(const ResolvedSampleAssets& assets)
        {
            std::vector<SampleModelExpectedContentIdentity> result;
            const auto append = [&result](const ResolvedSampleAsset& asset)
            {
                if (asset.selected &&
                    asset.entry.kind == SampleAssetKind::Model &&
                    asset.entry.contentIdentity.IsValid())
                {
                    result.push_back({asset.entry.resolvedPath,
                                      asset.entry.contentIdentity});
                }
            };
            append(assets.model);
            for (const ResolvedSampleAsset& additional : assets.additionalModels)
            {
                append(additional);
            }
            return result;
        }

        SampleAssetRegistry MakeSampleAssetRegistry(
            const ResolvedSampleAssets& assets,
            const bool runtimeOnlyComposition)
        {
            std::vector<SampleAssetRegistryEntry> entries;
            const auto append = [&entries, runtimeOnlyComposition](
                                    const ResolvedSampleAsset& asset)
            {
                // Explicit paths intentionally have no catalog id and are not
                // visible to catalog-id loader requests.
                if (!asset.selected || asset.entry.id.empty() ||
                    asset.entry.resolvedPath.empty())
                {
                    return;
                }

                SampleAssetRegistryEntry entry;
                entry.id = asset.entry.id;
                entry.kind = asset.entry.kind;
                entry.resolvedPath = asset.entry.resolvedPath;
                entry.contentIdentity = asset.entry.contentIdentity;
                entry.assetContentId = asset.entry.assetContentId;
                entry.packageOnly = asset.entry.cook.packageOnly;
                entry.requiresCookedResourcePath = runtimeOnlyComposition;
                if (runtimeOnlyComposition && entry.packageOnly)
                {
                    const std::string logicalPath =
                        asset.entry.path.lexically_normal().generic_string();
                    const std::string cookedPath = "cooked://" + logicalPath;
                    if (IsStrictSampleCookedResourcePath(cookedPath))
                    {
                        entry.resourcePath = cookedPath;
                    }
                }
                entries.push_back(std::move(entry));
            };
            append(assets.model);
            for (const ResolvedSampleAsset& additional : assets.additionalModels)
            {
                append(additional);
            }
            append(assets.environment);
            for (const ResolvedSampleAsset& animation : assets.animations)
            {
                append(animation);
            }
            for (const ResolvedSampleAsset& effect : assets.effects)
            {
                append(effect);
            }
            return SampleAssetRegistry(std::move(entries));
        }

        std::string FormatContentIdentity(
            const Resource::ResourceContentIdentity& identity)
        {
            if (!identity.IsValid())
            {
                return "unavailable";
            }
            return std::string(Resource::GetResourceContentIdentityDomainName(
                                   identity.domain)) +
                   ':' +
                   Resource::GetResourceContentIdentityScopeName(identity.scope) +
                   ':' +
                   Resource::GetResourceContentHashAlgorithmName(identity.algorithm) +
                   ':' + identity.digest + ":bytes=" +
                   std::to_string(identity.byteCount) + ":files=" +
                   std::to_string(identity.fileCount);
        }

        std::optional<Resource::ResourceContentVerificationReceipt>
        GetModelContentVerificationReceipt(
            const SampleModelLoader& models,
            const ResolvedSampleAsset& asset)
        {
            return asset.selected && asset.entry.kind == SampleAssetKind::Model
                       ? models.GetContentVerificationReceipt(
                             asset.entry.resolvedPath)
                       : std::nullopt;
        }

        std::optional<Resource::ResourceContentVerificationReceipt>
        GetEnvironmentContentVerificationReceipt(
            const SampleEnvironmentLoader& environments,
            const ResolvedSampleAsset& asset)
        {
            return asset.selected && asset.entry.kind == SampleAssetKind::Environment
                       ? environments.GetContentVerificationReceipt(
                             asset.entry.resolvedPath)
                       : std::nullopt;
        }

        std::optional<Resource::ResourceContentVerificationReceipt>
        GetAnimationContentVerificationReceipt(
            const SampleAnimationLoader& animations,
            const ResolvedSampleAsset& asset)
        {
            return asset.selected && asset.entry.kind == SampleAssetKind::Animation
                       ? animations.GetContentVerificationReceipt(
                             asset.entry.resolvedPath)
                       : std::nullopt;
        }

        std::optional<Resource::ResourceContentVerificationReceipt>
        GetEffectContentVerificationReceipt(
            const SampleParticleEffectLoader& effects,
            const ResolvedSampleAsset& asset)
        {
            return asset.selected && asset.entry.kind == SampleAssetKind::Effect
                ? effects.GetContentVerificationReceipt(asset.entry.resolvedPath)
                : std::nullopt;
        }

        AssetContentVerificationSummary BuildAssetContentVerificationSummary(
            const ResolvedSampleAssets& assets,
            const SampleModelLoader& models,
            const SampleEnvironmentLoader& environments,
            const SampleAnimationLoader& animations,
            const SampleParticleEffectLoader& effects,
            FrameworkAssessmentSession& assessmentSession)
        {
            struct AssetEvidence
            {
                std::string kind;
                std::string id;
                Resource::ResourceContentIdentity expected;
                Resource::ResourceContentVerificationReceipt receipt;
                bool hasReceipt = false;
            };

            std::map<std::pair<std::string, std::string>, AssetEvidence> evidence;
            const auto append = [&evidence, &models, &environments, &animations,
                                 &effects](
                                    const ResolvedSampleAsset& asset)
            {
                if (!asset.selected)
                {
                    return;
                }

                AssetEvidence value;
                value.kind = GetSampleAssetKindName(asset.entry.kind);
                value.id = asset.entry.id;
                value.expected = asset.entry.contentIdentity;
                if (asset.entry.kind == SampleAssetKind::Model)
                {
                    const std::optional<Resource::ResourceContentVerificationReceipt>
                        receipt = GetModelContentVerificationReceipt(models, asset);
                    if (receipt.has_value())
                    {
                        value.receipt = *receipt;
                        value.hasReceipt = true;
                    }
                }
                else if (asset.entry.kind == SampleAssetKind::Environment)
                {
                    const std::optional<Resource::ResourceContentVerificationReceipt>
                        receipt = GetEnvironmentContentVerificationReceipt(
                            environments, asset);
                    if (receipt.has_value())
                    {
                        value.receipt = *receipt;
                        value.hasReceipt = true;
                    }
                }
                else if (asset.entry.kind == SampleAssetKind::Animation)
                {
                    const std::optional<Resource::ResourceContentVerificationReceipt>
                        receipt = GetAnimationContentVerificationReceipt(
                            animations, asset);
                    if (receipt.has_value())
                    {
                        value.receipt = *receipt;
                        value.hasReceipt = true;
                    }
                }
                else if (asset.entry.kind == SampleAssetKind::Effect)
                {
                    const std::optional<Resource::ResourceContentVerificationReceipt>
                        receipt = GetEffectContentVerificationReceipt(
                            effects, asset);
                    if (receipt.has_value())
                    {
                        value.receipt = *receipt;
                        value.hasReceipt = true;
                    }
                }

                // Catalog ids are the portable logical identity. An explicit
                // path intentionally has no id and cannot enter assetSet.
                evidence.try_emplace(
                    std::make_pair(value.kind, value.id), std::move(value));
            };
            append(assets.model);
            for (const ResolvedSampleAsset& additional : assets.additionalModels)
            {
                append(additional);
            }
            append(assets.environment);
            for (const ResolvedSampleAsset& animation : assets.animations)
            {
                append(animation);
            }
            for (const ResolvedSampleAsset& effect : assets.effects)
            {
                append(effect);
            }

            AssetContentVerificationSummary result;
            bool allVerified = !evidence.empty();
            std::ostringstream assetSet;
            assetSet << "rvx-asset-set-v1";
            for (const auto& [key, value] : evidence)
            {
                const bool verified = value.hasReceipt &&
                                      value.receipt.IsVerified() &&
                                      value.receipt.observed.IsValid() &&
                                      value.expected.IsValid() &&
                                      value.receipt.expected == value.expected;
                const std::string keyPrefix =
                    "assetContent." + value.kind + "." +
                    (value.id.empty() ? "explicit" : value.id);
                static_cast<void>(assessmentSession.RecordMetadata(
                    keyPrefix + ".expected",
                    FormatContentIdentity(value.expected)));
                static_cast<void>(assessmentSession.RecordMetadata(
                    keyPrefix + ".observed",
                    value.hasReceipt
                        ? FormatContentIdentity(value.receipt.observed)
                        : std::string("unavailable")));
                static_cast<void>(assessmentSession.RecordMetadata(
                    keyPrefix + ".status",
                    value.hasReceipt
                        ? Resource::GetResourceContentVerificationStatusName(
                              value.receipt.status)
                        : "not-requested"));

                if (!verified || value.id.empty())
                {
                    allVerified = false;
                    continue;
                }
                assetSet << '|' << value.kind << ':' << value.id << '@'
                         << FormatContentIdentity(value.receipt.observed);
            }

            result.allSelectedAssetsVerified = allVerified;
            if (allVerified)
            {
                result.assetSet = assetSet.str();
                static_cast<void>(assessmentSession.RecordMetadata(
                    "assetContent.assetSetStatus", "verified"));
            }
            else
            {
                static_cast<void>(assessmentSession.RecordMetadata(
                    "assetContent.assetSetStatus", "unavailable"));
            }
            return result;
        }

        bool ResolveSceneOptions(const SampleInfo& info,
                                 const SampleRunnerCLIOptions& options,
                                 const std::filesystem::path& executableDirectory,
                                 SampleSceneOptions& sceneOptions,
                                 ResolvedSampleAssets& resolvedAssets,
                                 std::string& outError)
        {
            const std::string selectedModelId = options.assetId.empty()
                                                    ? info.defaultAssetId
                                                    : options.assetId;
            const std::string selectedEnvironmentId =
                options.environmentId.empty() ? info.defaultEnvironmentId
                                              : options.environmentId;
            const std::vector<std::string>& declaredEffectIds =
                info.requiredEffectAssetIds;
            std::vector<std::string> selectedEffectIds = declaredEffectIds;
            if (selectedEffectIds.empty() && !info.defaultEffectAssetId.empty())
            {
                selectedEffectIds.push_back(info.defaultEffectAssetId);
            }
            if (!declaredEffectIds.empty() &&
                (info.defaultEffectAssetId.empty() ||
                 selectedEffectIds.front() != info.defaultEffectAssetId))
            {
                outError = "Sample multi-effect declaration must retain its first effect as defaultEffectAssetId: " +
                    info.id;
                return false;
            }
            sceneOptions.assetId = selectedModelId;
            sceneOptions.environmentAssetId = selectedEnvironmentId;
            sceneOptions.effectAssetId = selectedEffectIds.empty()
                ? std::string{}
                : selectedEffectIds.front();
            sceneOptions.width = options.common.width;
            sceneOptions.height = options.common.height;
            sceneOptions.quality = options.common.quality;
            sceneOptions.qualityTier = options.qualityTier;
            sceneOptions.pipelineRecipe = options.pipelineRecipe;
            sceneOptions.fixedTimeNumerator = options.fixedTimeNumerator;
            sceneOptions.fixedTimeDenominator = options.fixedTimeDenominator;
            sceneOptions.deterministicSeed = options.deterministicSeed;
            RenderPipelineRecipe validatedRecipe{};
            RenderQualityTier validatedQuality{};
            if (!TryMapSamplePipelineRecipe(
                    sceneOptions.pipelineRecipe, validatedRecipe) ||
                !TryMapSampleQualityTier(
                    sceneOptions.qualityTier, validatedQuality))
            {
                outError =
                    "Selected Sample pipeline recipe or quality profile is invalid";
                return false;
            }
            sceneOptions.renderPath = options.renderPathExplicit
                                          ? options.renderPath
                                          : info.defaultRenderPath;
            if (options.workloadScaleExplicit &&
                !info.supportsWorkloadScaleSelection)
            {
                outError = "Sample does not accept workload-scale selection: " +
                           info.id;
                return false;
            }
            if (info.supportsWorkloadScaleSelection)
            {
                const SampleWorkloadScale workloadScale =
                    options.workloadScaleExplicit
                        ? options.workloadScale
                        : info.defaultWorkloadScale;
                sceneOptions.workloadProfile =
                    BuildSampleWorkloadProfile(workloadScale);
            }
            else
            {
                sceneOptions.workloadProfile.reset();
            }
            sceneOptions.smoke = options.common.smoke;
            sceneOptions.diagnostics = options.common.diagnostics;
            sceneOptions.deterministicCameraOrbit =
                options.deterministicCameraOrbit;

            // This is a deliberately narrow qualification intent, not a
            // renderer control surface. The cooked GFX15 scenario consumes
            // its normal readiness receipt; unrelated Samples must not accept
            // the flag and accidentally advertise transparency evidence.
            if (options.common.translucencyQualificationProbeEnabled &&
                info.id != "transparent-materials")
            {
                outError =
                    "--translucency-qualification-probe requires the transparent-materials sample";
                return false;
            }
            if (options.common.hiZReflectionQualificationProbeEnabled &&
                info.id != "post-processing")
            {
                outError =
                    "--hiz-reflection-qualification-probe requires the post-processing sample";
                return false;
            }

            if (options.renderPathExplicit &&
                !info.supportsRenderPathSelection)
            {
                outError = "Sample does not accept render-path selection: " +
                           info.id;
                return false;
            }

            const bool environmentOverride = !options.environmentId.empty() ||
                                             !options.environmentPath.empty();
            if (info.environmentPolicy == SampleEnvironmentPolicy::None &&
                environmentOverride)
            {
                outError = "Sample does not accept environment overrides: " +
                           info.id;
                return false;
            }
            if (info.environmentPolicy == SampleEnvironmentPolicy::Required &&
                selectedEnvironmentId.empty() && options.environmentPath.empty())
            {
                outError = "Sample requires an environment asset: " + info.id;
                return false;
            }
            if (selectedModelId.empty() && options.modelPath.empty() &&
                selectedEffectIds.empty())
            {
                outError = "Sample has no default model or effect asset and --asset was not provided: " +
                           info.id;
                return false;
            }
            if (info.assetPolicy == SampleAssetPolicy::Fixed &&
                ((!options.modelPath.empty()) ||
                 selectedModelId != info.defaultAssetId))
            {
                outError = "Sample does not accept model asset overrides: " +
                           info.id;
                return false;
            }

            if (options.common.smoke &&
                (!options.modelPath.empty() || !options.environmentPath.empty()))
            {
                outError =
                    "Smoke validation requires catalog-backed redistributable assets";
                return false;
            }

            const bool modelUsesCatalog = options.modelPath.empty() &&
                                          !selectedModelId.empty();
            const bool environmentUsesCatalog = options.environmentPath.empty() &&
                                                !selectedEnvironmentId.empty();
            const bool animationsUseCatalog =
                !info.requiredAnimationAssetIds.empty();
            const bool effectsUseCatalog = !selectedEffectIds.empty();
            const bool cookedAdmissionUsesCatalog =
                !info.requiredCookedAssetIds.empty();

            std::filesystem::path catalogPath = options.catalogPath;
            std::filesystem::path assetRoot = options.assetRoot;
            SampleAssetCatalog catalog;
            if (modelUsesCatalog || environmentUsesCatalog ||
                animationsUseCatalog || effectsUseCatalog ||
                cookedAdmissionUsesCatalog)
            {
                if (catalogPath.empty() && assetRoot.empty())
                {
                    assetRoot = executableDirectory / "Assets/Samples";
                    catalogPath = assetRoot / "catalog.json";
                }
                else if (catalogPath.empty())
                {
                    catalogPath = assetRoot / "catalog.json";
                }
                else if (assetRoot.empty())
                {
                    assetRoot = catalogPath.parent_path();
                    if (assetRoot.empty())
                    {
                        assetRoot = std::filesystem::current_path();
                    }
                }

                if (!catalog.Load(catalogPath, assetRoot, &outError))
                {
                    return false;
                }
            }

            if (!options.modelPath.empty())
            {
                sceneOptions.assetId.clear();
                if (!NormalizeExistingAssetPath(options.modelPath,
                                                "Model",
                                                sceneOptions.modelPath,
                                                outError))
                {
                    return false;
                }
                SetExplicitAsset(sceneOptions.modelPath,
                                 SampleAssetKind::Model,
                                 "model",
                                 resolvedAssets.model);
            }
            else if (modelUsesCatalog)
            {
                if (!ResolveCatalogAsset(catalog,
                                         selectedModelId,
                                         SampleAssetKind::Model,
                                         "model",
                                         options.common.smoke,
                                         resolvedAssets.model,
                                         outError))
                {
                    return false;
                }
                sceneOptions.modelPath =
                    resolvedAssets.model.entry.resolvedPath;
            }
            if (!sceneOptions.assetId.empty())
            {
                sceneOptions.modelAssets.push_back({
                    sceneOptions.assetId,
                    sceneOptions.modelPath,
                });
                sceneOptions.modelAssetIds.push_back(sceneOptions.assetId);
            }

            const bool useDefaultAssetSet = options.assetId.empty() &&
                                            options.modelPath.empty();
            if (useDefaultAssetSet)
            {
                for (const std::string& additionalId :
                     info.additionalDefaultAssetIds)
                {
                    if (additionalId.empty() ||
                        additionalId == selectedModelId ||
                        std::any_of(
                            sceneOptions.modelAssets.begin(),
                            sceneOptions.modelAssets.end(),
                            [&additionalId](const SampleSceneModelAsset& asset)
                            {
                                return asset.id == additionalId;
                            }))
                    {
                        outError =
                            "Sample declares an invalid or duplicate additional asset id: " +
                            additionalId;
                        return false;
                    }

                    ResolvedSampleAsset additional;
                    if (!ResolveCatalogAsset(catalog,
                                             additionalId,
                                             SampleAssetKind::Model,
                                             "model",
                                             options.common.smoke,
                                             additional,
                                             outError))
                    {
                        return false;
                    }
                    sceneOptions.modelAssets.push_back({
                        additional.entry.id,
                        additional.entry.resolvedPath,
                    });
                    sceneOptions.modelAssetIds.push_back(additional.entry.id);
                    resolvedAssets.additionalModels.push_back(
                        std::move(additional));
                }
            }

            if (!options.environmentPath.empty())
            {
                sceneOptions.environmentAssetId.clear();
                if (!NormalizeExistingAssetPath(options.environmentPath,
                                                "Environment",
                                                sceneOptions.environmentPath,
                                                outError))
                {
                    return false;
                }
                SetExplicitAsset(sceneOptions.environmentPath,
                                 SampleAssetKind::Environment,
                                 "environment",
                                 resolvedAssets.environment);
            }
            else if (environmentUsesCatalog)
            {
                if (!ResolveCatalogAsset(catalog,
                                         selectedEnvironmentId,
                                         SampleAssetKind::Environment,
                                         "environment",
                                         options.common.smoke,
                                         resolvedAssets.environment,
                                         outError))
                {
                    return false;
                }
                sceneOptions.environmentPath =
                    resolvedAssets.environment.entry.resolvedPath;
            }
            if (resolvedAssets.environment.selected)
            {
                sceneOptions.environmentContentIdentity =
                    resolvedAssets.environment.entry.contentIdentity;
            }

            for (const std::string& animationId :
                 info.requiredAnimationAssetIds)
            {
                if (animationId.empty() ||
                    std::find(sceneOptions.animationAssetIds.begin(),
                              sceneOptions.animationAssetIds.end(),
                              animationId) !=
                        sceneOptions.animationAssetIds.end())
                {
                    outError =
                        "Sample declares an invalid or duplicate animation asset id: " +
                        animationId;
                    return false;
                }

                ResolvedSampleAsset animation;
                if (!ResolveCatalogAsset(catalog,
                                         animationId,
                                         SampleAssetKind::Animation,
                                         "animation",
                                         options.common.smoke,
                                         animation,
                                         outError))
                {
                    return false;
                }
                sceneOptions.animationAssetIds.push_back(animation.entry.id);
                resolvedAssets.animations.push_back(std::move(animation));
            }

            for (const std::string& selectedEffectId : selectedEffectIds)
            {
                if (selectedEffectId.empty() ||
                    std::find(sceneOptions.effectAssetIds.begin(),
                              sceneOptions.effectAssetIds.end(),
                              selectedEffectId) !=
                        sceneOptions.effectAssetIds.end())
                {
                    outError = "Sample declares an invalid or duplicate effect asset id: " +
                        selectedEffectId;
                    return false;
                }
                ResolvedSampleAsset effect;
                if (!ResolveCatalogAsset(catalog,
                                         selectedEffectId,
                                         SampleAssetKind::Effect,
                                         "effect",
                                         options.common.smoke,
                                         effect,
                                         outError))
                {
                    return false;
                }
                sceneOptions.effectAssetIds.push_back(effect.entry.id);
                resolvedAssets.effects.push_back(std::move(effect));
            }

            std::vector<std::string> admittedCookedIds;
            admittedCookedIds.reserve(info.requiredCookedAssetIds.size());
            for (const std::string& cookedId : info.requiredCookedAssetIds)
            {
                if (cookedId.empty() ||
                    std::find(admittedCookedIds.begin(),
                              admittedCookedIds.end(),
                              cookedId) != admittedCookedIds.end())
                {
                    outError =
                        "Sample declares an invalid or duplicate cooked asset id: " +
                        cookedId;
                    return false;
                }

                ResolvedSampleAsset* selectedAsset = nullptr;
                if (resolvedAssets.model.selected &&
                    resolvedAssets.model.entry.id == cookedId)
                {
                    selectedAsset = &resolvedAssets.model;
                }
                if (!selectedAsset)
                {
                    const auto additionalIt = std::find_if(
                        resolvedAssets.additionalModels.begin(),
                        resolvedAssets.additionalModels.end(),
                        [&cookedId](const ResolvedSampleAsset& asset)
                        {
                            return asset.entry.id == cookedId;
                        });
                    if (additionalIt != resolvedAssets.additionalModels.end())
                    {
                        selectedAsset = &*additionalIt;
                    }
                }
                if (!selectedAsset && resolvedAssets.environment.selected &&
                    resolvedAssets.environment.entry.id == cookedId)
                {
                    selectedAsset = &resolvedAssets.environment;
                }
                if (!selectedAsset)
                {
                    const auto animationIt = std::find_if(
                        resolvedAssets.animations.begin(),
                        resolvedAssets.animations.end(),
                        [&cookedId](const ResolvedSampleAsset& asset)
                        {
                            return asset.entry.id == cookedId;
                        });
                    if (animationIt != resolvedAssets.animations.end())
                    {
                        selectedAsset = &*animationIt;
                    }
                }
                if (!selectedAsset)
                {
                    const auto effectIt = std::find_if(
                        resolvedAssets.effects.begin(),
                        resolvedAssets.effects.end(),
                        [&cookedId](const ResolvedSampleAsset& asset)
                        {
                            return asset.entry.id == cookedId;
                        });
                    if (effectIt != resolvedAssets.effects.end())
                    {
                        selectedAsset = &*effectIt;
                    }
                }
                if (!selectedAsset)
                {
                    outError =
                        "Sample requires cooked admission for an asset that is not selected: " +
                        cookedId;
                    return false;
                }

                SampleCookedAssetAdmissionReceipt admission =
                    catalog.RequireCookedAdmission(cookedId);
                if (!admission.IsAccepted())
                {
                    outError =
                        "Cooked asset admission failed for '" + cookedId +
                        "' (" +
                        GetSampleCookedAssetAdmissionCodeName(admission.code) +
                        "): " + admission.detail;
                    return false;
                }
                selectedAsset->cookAdmission = std::move(admission);
                admittedCookedIds.push_back(cookedId);
            }
            return true;
        }

        SampleReportAsset MakeReportAsset(
            const char* role,
            const ResolvedSampleAsset& resolved,
            const std::optional<Resource::ResourceContentVerificationReceipt>&
                receipt = std::nullopt)
        {
            SampleReportAsset result;
            result.role = role;
            result.id = resolved.entry.id;
            result.path = resolved.entry.resolvedPath;
            result.kind = GetSampleAssetKindName(resolved.entry.kind);
            result.catalogPath = resolved.catalogPath;
            result.assetRoot = resolved.assetRoot;
            result.licenseSpdx = resolved.entry.license.spdxId;
            result.licenseFile = resolved.entry.license.resolvedFile;
            result.sourceName = resolved.entry.source.name;
            result.sourceUri = resolved.entry.source.uri;
            result.author = resolved.entry.source.author;
            result.attribution = resolved.entry.attribution;
            result.redistributable = resolved.entry.redistributable;
            // A scenario can still be pending after the concrete asset is
            // fully resident (for example while Render retires its final
            // submission). Asset residency must therefore not be inferred
            // from the aggregate scenario-ready bit. The owner-thread receipt
            // is populated only from the exact Model/Environment resource and
            // carries the verified immutable content identity.
            result.loaded = receipt.has_value() && receipt->IsVerified();
            result.sourceContentId = resolved.entry.assetContentId;
            result.packageContentId = resolved.entry.assetContentId;
            result.expectedContentIdentity = resolved.entry.contentIdentity;
            if (receipt.has_value())
            {
                result.observedContentIdentity = receipt->observed;
                result.verificationStatus = receipt->status;
                result.verified = receipt->IsVerified();
                switch (receipt->observed.domain)
                {
                    case Resource::ResourceContentIdentityDomain::Source:
                        result.actualLoadDomain = "SourceAsset";
                        break;
                    case Resource::ResourceContentIdentityDomain::CookedArtifact:
                        result.actualLoadDomain = "CookedArtifact";
                        break;
                    case Resource::ResourceContentIdentityDomain::CookManifest:
                        result.actualLoadDomain = "CookManifest";
                        break;
                    default:
                        result.actualLoadDomain = "Unknown";
                        break;
                }
            }
            result.cookedAdmissionRequired =
                resolved.cookAdmission.has_value();
            result.cookedAdmissionAccepted =
                resolved.cookAdmission.has_value() &&
                resolved.cookAdmission->IsAccepted();
            result.cookPackageOnly = resolved.entry.cook.declared &&
                resolved.entry.cook.packageOnly;
            if (resolved.entry.cook.declared)
            {
                result.cookManifestPath =
                    (resolved.assetRoot /
                     resolved.entry.cook.manifestPath).lexically_normal();
                result.cookedRoot =
                    (resolved.assetRoot /
                     resolved.entry.cook.cookedRoot).lexically_normal();
                result.declaredCookSourceContentIdentity =
                    resolved.entry.cook.sourceContentIdentity;
                result.declaredCookedContentIdentity =
                    resolved.entry.cook.cookedContentIdentity;
                result.declaredCookManifestContentIdentity =
                    resolved.entry.cook.manifestContentIdentity;
                result.cookSettingsHash =
                    resolved.entry.cook.cookSettingsHash;
                result.cookRecipeHash = resolved.entry.cook.recipeHash;
                result.cookToolName = resolved.entry.cook.toolName;
                result.cookToolVersion = resolved.entry.cook.toolVersion;
            }
            if (resolved.cookAdmission.has_value())
            {
                result.cookedAdmissionCode =
                    GetSampleCookedAssetAdmissionCodeName(
                        resolved.cookAdmission->code);
                result.cookedAdmissionDetail =
                    resolved.cookAdmission->detail;
                result.observedCookSourceContentIdentity =
                    resolved.cookAdmission->resourceReceipt
                        .observedSourceContentIdentity;
                result.observedCookedContentIdentity =
                    resolved.cookAdmission->resourceReceipt
                        .observedCookedContentIdentity;
                result.observedCookManifestContentIdentity =
                    resolved.cookAdmission->resourceReceipt
                        .observedManifestContentIdentity;
            }
            return result;
        }

        [[nodiscard]] SamplePostProcessExecutionDiagnostics
            MakeSamplePostProcessExecutionDiagnostics(
                const RenderDiagnosticsSnapshot& renderDiagnostics)
        {
            SamplePostProcessExecutionDiagnostics result;
            const RenderFrameFeatureDiagnostics& frameFeatures =
                renderDiagnostics.frameFeatures;
            const RenderExecutionReceipt& receipt =
                frameFeatures.executionReceipt;

            result.available = frameFeatures.available && receipt.available;
            result.executionReceiptSchemaVersion = receipt.schemaVersion;
            result.executionReceiptStructurallyValid =
                receipt.IsStructurallyValid();
            result.executionReceiptAvailable = receipt.available;
            result.executionReceiptGraphComposed = receipt.graphComposed;
            result.executionReceiptExecutionSucceeded =
                receipt.executionSucceeded;
            result.executionReceiptPresented = receipt.presented;
            result.executionReceiptSucceeded = receipt.succeeded;
            result.frameFeatureSequence = frameFeatures.frameSequence;
            result.receiptFrameSequence = receipt.frameIdentity.frameSequence;
            result.lastPresentedFrameSequence =
                renderDiagnostics.lastPresentedFrameSequence;
            result.presentedFrameAligned =
                result.frameFeatureSequence != 0 &&
                result.frameFeatureSequence == result.receiptFrameSequence &&
                result.receiptFrameSequence ==
                    result.lastPresentedFrameSequence;
            result.mainView = receipt.viewIdentity.role == ViewRole::Main &&
                receipt.viewIdentity.subViewIndex == 0;
            result.sceneRuntimeId = receipt.viewIdentity.sceneRuntimeId;
            result.cameraEntityPacked = receipt.viewIdentity.cameraEntityPacked;
            result.subViewIndex = receipt.viewIdentity.subViewIndex;

            const auto postProcessFeature = std::find_if(
                receipt.features.begin(), receipt.features.end(),
                [](const RenderFeatureExecutionReceipt& feature)
                {
                    return feature.featureId ==
                        BuiltinRenderFeatureIds::PostProcessPipeline;
                });
            if (postProcessFeature != receipt.features.end())
            {
                result.postProcessFeatureFound = true;
                result.postProcessFeatureSelected = postProcessFeature->selected;
                result.postProcessFeatureExecuted =
                    postProcessFeature->readiness.executed;
                result.postProcessFeatureFallbackUsed =
                    postProcessFeature->fallbackUsed;
                result.postProcessFeatureGraphPassCount =
                    postProcessFeature->graphPassCount;
                result.postProcessFeatureExecutedGraphPassCount =
                    postProcessFeature->executedGraphPassCount;
                result.diagnostic = postProcessFeature->diagnostic;
            }

            const RenderPostProcessExecutionReceipt& postProcess =
                receipt.postProcess;
            result.postProcessReceiptStructurallyValid =
                postProcess.IsStructurallyValid();
            result.planContentHash = postProcess.postProcessContentHash;
            // GFX12 integration point: this is a direct value projection of
            // renderer-owned receipt fields. Samples never derive exposure
            // execution from their requested lighting or post-process policy.
            result.exposureStateContentHash =
                postProcess.exposureStateContentHash;
            result.toneMapExposureStateHash =
                postProcess.toneMapExposureStateHash;
            result.bloomExposureStateHash =
                postProcess.bloomExposureStateHash;
            result.taaExposureStateHash = postProcess.taaExposureStateHash;
            result.exposureMode = postProcess.exposureMode;
            result.exposureRequested = postProcess.exposureRequested;
            result.exposurePipelineReady = postProcess.exposurePipelineReady;
            result.exposureInputsReady = postProcess.exposureInputsReady;
            result.exposureExecuted = postProcess.exposureExecuted;
            result.exposureHistogramPassCount =
                postProcess.exposureHistogramPassCount;
            result.exposureAdaptationPassCount =
                postProcess.exposureAdaptationPassCount;
            result.exposureHistoryAvailableBefore =
                postProcess.exposureHistoryAvailableBefore;
            result.exposureHistorySampled =
                postProcess.exposureHistorySampled;
            result.exposureHistoryCandidateProduced =
                postProcess.exposureHistoryCandidateProduced;
            result.exposureHistoryCommitted =
                postProcess.exposureHistoryCommitted;
            result.exposureHistoryAbandoned =
                postProcess.exposureHistoryAbandoned;
            result.exposureHistoryResetReasons =
                postProcess.exposureHistoryResetReasons;
            result.exposureProbeRequested = postProcess.exposureProbeRequested;
            result.exposureProbeReady = postProcess.exposureProbeReady;
            result.targetEV100 = postProcess.targetEV100;
            result.adaptedEV100 = postProcess.adaptedEV100;
            result.exposureScale = postProcess.exposureScale;
            result.preExposure = postProcess.preExposure;
            result.previousPreExposure = postProcess.previousPreExposure;
            result.toneMappingPassCount = postProcess.toneMapPassCount;
            result.bloomPassCount = postProcess.bloomPassCount;
            result.colorGradingPassCount =
                postProcess.colorGradingPassCount;
            result.fxaaPassCount = postProcess.fxaaPassCount;
            result.taaPassCount = postProcess.taaPassCount;
            result.antiAliasingMode = postProcess.antiAliasingMode;
            result.taaRequested = postProcess.taaRequested;
            result.taaPipelineReady = postProcess.taaPipelineReady;
            result.taaInputsReady = postProcess.taaInputsReady;
            result.taaExecuted = postProcess.taaExecuted;
            result.historyAvailableBefore =
                postProcess.historyAvailableBefore;
            result.historySampled = postProcess.historySampled;
            result.historyCandidateProduced =
                postProcess.historyCandidateProduced;
            result.historyCommitted = postProcess.historyCommitted;
            result.historyAbandoned = postProcess.historyAbandoned;
            result.historyResetReasons = postProcess.historyResetReasons;
            result.currentJitterPixels = postProcess.currentJitterPixels;
            result.previousJitterPixels = postProcess.previousJitterPixels;
            result.taaResolveGraphPassCount =
                postProcess.taaResolveGraphPassCount;
            result.taaHistoryStoreGraphPassCount =
                postProcess.taaHistoryStoreGraphPassCount;
            result.inputColorDomain = postProcess.inputColorDomain;
            result.plannedOutputColorDomain =
                postProcess.plannedOutputColorDomain;
            result.storedOutputColorDomain =
                postProcess.storedOutputColorDomain;
            result.finalOutputFormat = postProcess.finalOutputFormat;
            result.toneMappingTransfer = postProcess.toneMappingTransfer;
            result.presentationTransfer = postProcess.presentationTransfer;
            result.hdrInputReady = postProcess.hdrInputReady;
            result.displayLinearOutputReady =
                postProcess.displayLinearOutputReady;
            result.encodedOutputReady = postProcess.encodedOutputReady;
            result.fallbackUsed = postProcess.fallbackUsed;
            if (result.diagnostic.empty())
            {
                result.diagnostic = receipt.diagnostic;
            }
            return result;
        }

        [[nodiscard]] SampleAmbientOcclusionExecutionDiagnostics
            MakeSampleAmbientOcclusionExecutionDiagnostics(
                const RenderDiagnosticsSnapshot& renderDiagnostics)
        {
            SampleAmbientOcclusionExecutionDiagnostics result;
            const RenderFrameFeatureDiagnostics& frameFeatures =
                renderDiagnostics.frameFeatures;
            const RenderExecutionReceipt& receipt =
                frameFeatures.executionReceipt;

            result.available = frameFeatures.available && receipt.available;
            result.executionReceiptSchemaVersion = receipt.schemaVersion;
            result.executionReceiptStructurallyValid =
                receipt.IsStructurallyValid();
            result.executionReceiptAvailable = receipt.available;
            result.executionReceiptGraphComposed = receipt.graphComposed;
            result.executionReceiptExecutionSucceeded =
                receipt.executionSucceeded;
            result.executionReceiptPresented = receipt.presented;
            result.executionReceiptSucceeded = receipt.succeeded;
            result.frameFeatureSequence = frameFeatures.frameSequence;
            result.receiptFrameSequence = receipt.frameIdentity.frameSequence;
            result.lastPresentedFrameSequence =
                renderDiagnostics.lastPresentedFrameSequence;
            result.presentedFrameAligned =
                result.frameFeatureSequence != 0 &&
                result.frameFeatureSequence == result.receiptFrameSequence &&
                result.receiptFrameSequence ==
                    result.lastPresentedFrameSequence;
            result.mainView = receipt.viewIdentity.role == ViewRole::Main &&
                receipt.viewIdentity.subViewIndex == 0;
            result.sceneRuntimeId = receipt.viewIdentity.sceneRuntimeId;
            result.cameraEntityPacked = receipt.viewIdentity.cameraEntityPacked;
            result.subViewIndex = receipt.viewIdentity.subViewIndex;

            // GFX13 integration point: every field is a direct immutable
            // projection of the renderer-owned receipt. Samples never infer
            // GTAO execution, consumers, or temporal commits from settings.
            const RenderAmbientOcclusionExecutionReceipt& ambientOcclusion =
                receipt.ambientOcclusion;
            result.receiptStructurallyValid =
                ambientOcclusion.IsStructurallyValid();
            result.planContentHash = ambientOcclusion.planContentHash;
            result.requested = ambientOcclusion.requested;
            result.pipelineReady = ambientOcclusion.pipelineReady;
            result.inputsReady = ambientOcclusion.inputsReady;
            result.executed = ambientOcclusion.executed;
            result.lightingConsumerExecuted =
                ambientOcclusion.lightingConsumerExecuted;
            result.linearDepthInputReady =
                ambientOcclusion.linearDepthInputReady;
            result.worldNormalInputReady =
                ambientOcclusion.worldNormalInputReady;
            result.velocityInputReady = ambientOcclusion.velocityInputReady;
            result.velocityProducerExecuted =
                ambientOcclusion.velocityProducerExecuted;
            result.outputPublished = ambientOcclusion.outputPublished;
            result.outputFormat = ambientOcclusion.outputFormat;
            result.horizonSearchGraphPassCount =
                ambientOcclusion.horizonSearchGraphPassCount;
            result.bilateralDenoiseGraphPassCount =
                ambientOcclusion.bilateralDenoiseGraphPassCount;
            result.temporalResolveGraphPassCount =
                ambientOcclusion.temporalResolveGraphPassCount;
            result.historyAvailableBefore =
                ambientOcclusion.historyAvailableBefore;
            result.historySampled = ambientOcclusion.historySampled;
            result.motionReprojectionUsed =
                ambientOcclusion.motionReprojectionUsed;
            result.viewProjectionMotionObserved =
                ambientOcclusion.viewProjectionMotionObserved;
            result.historyCandidateProduced =
                ambientOcclusion.historyCandidateProduced;
            result.historyCommitted = ambientOcclusion.historyCommitted;
            result.historyAbandoned = ambientOcclusion.historyAbandoned;
            result.historyResetReasons = ambientOcclusion.historyResetReasons;
            result.aoFieldContentHash = ambientOcclusion.aoFieldContentHash;
            result.lightingAOFieldContentHash =
                ambientOcclusion.lightingAOFieldContentHash;
            result.velocityFieldIdentity =
                ambientOcclusion.velocityFieldIdentity;
            result.consumerMask =
                static_cast<uint32>(ambientOcclusion.consumerMask);
            result.probeRequested = ambientOcclusion.probeRequested;
            result.probeReady = ambientOcclusion.probeReady;
            result.probeSampleCount = ambientOcclusion.probeSampleCount;
            result.probeOccludedSampleCount =
                ambientOcclusion.probeOccludedSampleCount;
            if (result.probeReady)
            {
                result.probeMinVisibility = ambientOcclusion.probeMinVisibility;
                result.probeMeanVisibility =
                    ambientOcclusion.probeMeanVisibility;
                result.probeMaxVisibility = ambientOcclusion.probeMaxVisibility;
            }
            result.fallbackUsed = ambientOcclusion.fallbackUsed;
            result.diagnostic = ambientOcclusion.diagnostic;
            if (result.diagnostic.empty())
            {
                result.diagnostic = receipt.diagnostic;
            }
            return result;
        }

        [[nodiscard]] SampleHiZReflectionExecutionDiagnostics
            MakeSampleHiZReflectionExecutionDiagnostics(
                const RenderDiagnosticsSnapshot& renderDiagnostics)
        {
            SampleHiZReflectionExecutionDiagnostics result;
            const RenderFrameFeatureDiagnostics& frameFeatures =
                renderDiagnostics.frameFeatures;
            const RenderExecutionReceipt& receipt =
                frameFeatures.hiZReflectionQualificationReceipt;
            const RenderReflectionDiagnostics& reflection =
                frameFeatures.hiZReflectionQualification;

            result.available = reflection.available && receipt.available;
            result.executionReceiptSchemaVersion = receipt.schemaVersion;
            result.executionReceiptStructurallyValid =
                receipt.IsStructurallyValid();
            result.executionReceiptAvailable = receipt.available;
            result.executionReceiptGraphComposed = receipt.graphComposed;
            result.executionReceiptExecutionSucceeded =
                receipt.executionSucceeded;
            result.executionReceiptPresented = receipt.presented;
            result.executionReceiptSucceeded = receipt.succeeded;
            result.frameFeatureSequence = receipt.frameIdentity.frameSequence;
            result.receiptFrameSequence = receipt.frameIdentity.frameSequence;
            result.lastPresentedFrameSequence = receipt.presented
                ? receipt.frameIdentity.frameSequence
                : 0;
            result.presentedFrameAligned =
                result.frameFeatureSequence != 0 &&
                result.frameFeatureSequence == result.receiptFrameSequence &&
                result.receiptFrameSequence ==
                    result.lastPresentedFrameSequence &&
                reflection.frameSequence == result.receiptFrameSequence;
            result.mainView = receipt.viewIdentity.role == ViewRole::Main &&
                receipt.viewIdentity.subViewIndex == 0;

            // This is a one-to-one projection of the presentation-frozen
            // renderer receipt. No sample setting or graph registration is
            // allowed to synthesize SSR execution evidence.
            result.graphExecutionFinalized = reflection.graphExecutionFinalized;
            result.requested = reflection.requested;
            result.supported = reflection.supported;
            result.scheduled = reflection.scheduled;
            result.executed = reflection.executed;
            result.hiZBuilt = reflection.hiZBuilt;
            result.globalIBLReady = reflection.globalIBLReady;
            result.fallbackChainResolved = reflection.fallbackChainResolved;
            result.screenSpaceRequested = reflection.screenSpaceRequested;
            result.screenSpaceTraceInputsReady =
                reflection.screenSpaceTraceInputsReady;
            result.screenSpaceTraceScheduled =
                reflection.screenSpaceTraceScheduled;
            result.halfResolutionTrace = reflection.halfResolutionTrace;
            result.guidedUpscaleScheduled = reflection.guidedUpscaleScheduled;
            result.guidedUpscaleExecuted = reflection.guidedUpscaleExecuted;
            result.screenSpaceApplied = reflection.screenSpaceApplied;
            result.probesApplied = reflection.probesApplied;
            result.reflectionRouteEvidenceReady =
                reflection.reflectionRouteEvidenceReady;
            result.routeMaskRadianceSchemaVersion =
                reflection.routeMaskRadianceSchemaVersion;
            result.localProbeRadianceQualified =
                reflection.localProbeRadianceQualified;
            result.globalIBLRadianceQualified =
                reflection.globalIBLRadianceQualified;
            result.readyProbeCount = reflection.readyProbeCount;
            result.hiZMipCount = reflection.hiZMipCount;
            result.hiZGraphPassCount = reflection.hiZGraphPassCount;
            result.traceGraphPassCount = reflection.traceGraphPassCount;
            result.resolveGraphPassCount = reflection.resolveGraphPassCount;
            result.traceExtent = reflection.traceExtent;
            result.resolvedExtent = reflection.resolvedExtent;
            result.reflectionFrameSequence = reflection.frameSequence;
            result.reflectionPlanIdentity = reflection.reflectionPlanIdentity;
            result.lightingPlanIdentity = reflection.lightingPlanIdentity;
            result.configurationIdentity = reflection.configurationIdentity;
            result.pipelineIdentity = reflection.pipelineIdentity;
            result.depthPyramidIdentity = reflection.depthPyramidIdentity;
            result.reflectionSurfaceIdentity =
                reflection.reflectionSurfaceIdentity;
            result.exposureStateContentHash =
                reflection.exposureStateContentHash;
            result.screenSpaceHitPixelCount =
                reflection.screenSpaceHitPixelCount;
            result.localProbePixelCount = reflection.localProbePixelCount;
            result.globalIBLPixelCount = reflection.globalIBLPixelCount;
            result.reflectionReadbackSampleCount =
                reflection.reflectionReadbackSampleCount;
            result.reflectionRouteSampleCount =
                reflection.reflectionRouteSampleCount;
            result.backgroundPixelCount = reflection.backgroundPixelCount;
            result.nonReflectivePixelCount = reflection.nonReflectivePixelCount;
            result.fallbackReason = reflection.fallbackReason;
            return result;
        }

        [[nodiscard]] SampleWeightedBlendedOITExecutionDiagnostics
            MakeSampleWeightedBlendedOITExecutionDiagnostics(
                const RenderDiagnosticsSnapshot& renderDiagnostics)
        {
            constexpr std::string_view ClearPassName =
                "WeightedBlendedOITClear";
            constexpr std::string_view MeshWeightedPassName =
                "TransparentWeightedBlendedOIT";
            constexpr std::string_view ParticleWeightedPassName =
                "ParticleWeightedBlendedOIT";
            constexpr std::string_view CompositePassName =
                "WeightedBlendedOITComposite";

            SampleWeightedBlendedOITExecutionDiagnostics result;
            const RenderFrameFeatureDiagnostics& frameFeatures =
                renderDiagnostics.frameFeatures;
            const RenderExecutionReceipt& receipt =
                frameFeatures.executionReceipt;

            result.available = frameFeatures.available && receipt.available;
            result.executionReceiptSchemaVersion = receipt.schemaVersion;
            result.executionReceiptStructurallyValid =
                receipt.IsStructurallyValid();
            result.executionReceiptAvailable = receipt.available;
            result.executionReceiptGraphComposed = receipt.graphComposed;
            result.executionReceiptExecutionSucceeded =
                receipt.executionSucceeded;
            result.executionReceiptPresented = receipt.presented;
            result.executionReceiptSucceeded = receipt.succeeded;
            result.frameFeatureSequence = frameFeatures.frameSequence;
            result.receiptFrameSequence = receipt.frameIdentity.frameSequence;
            result.lastPresentedFrameSequence =
                renderDiagnostics.lastPresentedFrameSequence;
            result.presentedFrameAligned =
                result.frameFeatureSequence != 0 &&
                result.frameFeatureSequence == result.receiptFrameSequence &&
                result.receiptFrameSequence ==
                    result.lastPresentedFrameSequence;
            result.mainView = receipt.viewIdentity.role == ViewRole::Main &&
                receipt.viewIdentity.subViewIndex == 0;

            const auto feature = std::find_if(
                receipt.features.begin(), receipt.features.end(),
                [](const RenderFeatureExecutionReceipt& candidate)
                {
                    return candidate.featureId ==
                        BuiltinRenderFeatureIds::Translucency;
                });
            if (feature == receipt.features.end())
            {
                result.diagnostic = receipt.diagnostic.empty()
                    ? "Presented render receipt does not contain the translucent feature"
                    : receipt.diagnostic;
                return result;
            }

            result.featureFound = true;
            result.featureSelected = feature->selected;
            result.featureRequired =
                feature->requirement == RenderFeatureRequirement::Required;
            result.featureExecuted = feature->readiness.executed;
            result.featureFallbackUsed = feature->fallbackUsed;
            result.featureGraphPassCount = feature->graphPassCount;
            result.featureExecutedGraphPassCount =
                feature->executedGraphPassCount;
            result.allFeatureGraphPassesExecuted =
                result.featureGraphPassCount != 0 &&
                result.featureExecutedGraphPassCount ==
                    result.featureGraphPassCount;
            result.diagnostic = feature->diagnostic;

            uint32 clearIndex = std::numeric_limits<uint32>::max();
            uint32 firstWeightedIndex = std::numeric_limits<uint32>::max();
            uint32 lastWeightedIndex = 0;
            uint32 compositeIndex = std::numeric_limits<uint32>::max();
            for (uint32 index = 0;
                 index < static_cast<uint32>(feature->graphPassNames.size());
                 ++index)
            {
                const std::string& passName = feature->graphPassNames[index];
                if (passName == ClearPassName)
                {
                    ++result.clearGraphPassCount;
                    clearIndex = index;
                }
                else if (passName == MeshWeightedPassName)
                {
                    ++result.meshWeightedGraphPassCount;
                    firstWeightedIndex = std::min(firstWeightedIndex, index);
                    lastWeightedIndex = std::max(lastWeightedIndex, index);
                }
                else if (passName == ParticleWeightedPassName)
                {
                    ++result.particleWeightedGraphPassCount;
                    firstWeightedIndex = std::min(firstWeightedIndex, index);
                    lastWeightedIndex = std::max(lastWeightedIndex, index);
                }
                else if (passName == CompositePassName)
                {
                    ++result.compositeGraphPassCount;
                    compositeIndex = index;
                }
            }

            result.passOrderValid = result.clearGraphPassCount == 1 &&
                result.meshWeightedGraphPassCount +
                        result.particleWeightedGraphPassCount !=
                    0 &&
                result.compositeGraphPassCount == 1 &&
                clearIndex < firstWeightedIndex &&
                lastWeightedIndex < compositeIndex;
            if (result.diagnostic.empty() && !result.passOrderValid)
            {
                result.diagnostic =
                    "Translucent feature did not record clear, weighted accumulation, and composite in the required order";
            }
            return result;
        }

        SampleRenderDiagnostics MakeSampleRenderDiagnostics(
            const RenderDiagnosticsSnapshot& renderDiagnostics,
            const Engine& engine)
        {
            SampleRenderDiagnostics result;
            const EngineRenderRuntimeDiagnostics engineDiagnostics =
                engine.GetRenderRuntimeDiagnostics();
            const RenderFrameFeatureDiagnostics& diagnostics =
                renderDiagnostics.frameFeatures;
            result.available = diagnostics.available;
            result.renderAttempted = diagnostics.renderAttempted;
            result.rendered = diagnostics.rendered;
            result.graphBuilt = diagnostics.graphBuilt;
            result.graphCompiled = diagnostics.graphCompiled;
            result.renderGraphTotalPasses = diagnostics.renderGraphTotalPasses;
            result.visibleObjectCount = diagnostics.visibleObjectCount;
            result.renderSceneLightCount = diagnostics.renderSceneLightCount;
            result.renderSceneValuesAvailable =
                renderDiagnostics.sceneValues.available;
            result.renderSceneFrameSequence =
                renderDiagnostics.sceneValues.frameSequence;
            result.renderSceneAppliedRevision =
                renderDiagnostics.sceneValues.appliedSceneRevision;
            result.renderSceneRequiredRevision =
                renderDiagnostics.sceneValues.requiredSceneRevision;
            result.renderSceneObjectCount =
                renderDiagnostics.sceneValues.objectCount;
            result.renderSceneValueLightCount =
                renderDiagnostics.sceneValues.lightCount;
            result.renderSceneLightStateHash =
                renderDiagnostics.sceneValues.lightStateHash;
            const RenderMutationEvidenceDiagnostics& mutationEvidence =
                renderDiagnostics.mutationEvidence;
            SampleRenderMutationEvidenceDiagnostics& sampleMutationEvidence =
                result.mutationEvidence;
            sampleMutationEvidence.available = mutationEvidence.completion.available;
            sampleMutationEvidence.saturated = mutationEvidence.completion.saturated;
            sampleMutationEvidence.evidenceEpoch =
                mutationEvidence.completion.evidenceEpoch;
            sampleMutationEvidence.completedPresentationCount =
                mutationEvidence.completion.completedPresentationCount;
            sampleMutationEvidence.completedFrameSequence =
                mutationEvidence.completion.completedFrameSequence;
            sampleMutationEvidence.requiredSceneRevision =
                mutationEvidence.completion.requiredSceneRevision;
            sampleMutationEvidence.appliedSceneRevision =
                mutationEvidence.completion.appliedSceneRevision;
            sampleMutationEvidence.sceneFullRebuildCount =
                mutationEvidence.scene.fullRebuildCount;
            sampleMutationEvidence.sceneIncrementalCommitCount =
                mutationEvidence.scene.incrementalCommitCount;
            sampleMutationEvidence.sceneRebuiltObjectCount =
                mutationEvidence.scene.rebuiltObjectCount;
            sampleMutationEvidence.sceneRemovedObjectCount =
                mutationEvidence.scene.removedObjectCount;
            sampleMutationEvidence.gpuSceneFullPublicationCount =
                mutationEvidence.gpuScenePublication.fullPublicationCount;
            sampleMutationEvidence.gpuSceneIncrementalPublicationCount =
                mutationEvidence.gpuScenePublication.incrementalPublicationCount;
            sampleMutationEvidence.gpuSceneIdentityOnlyPublicationCount =
                mutationEvidence.gpuScenePublication.identityOnlyPublicationCount;
            sampleMutationEvidence.gpuSceneMaterializedObjectCount =
                mutationEvidence.gpuScenePublication.materializedObjectCount;
            sampleMutationEvidence.gpuSceneAddCount =
                mutationEvidence.gpuScenePublication.addCount;
            sampleMutationEvidence.gpuSceneUpdateCount =
                mutationEvidence.gpuScenePublication.updateCount;
            sampleMutationEvidence.gpuSceneRemoveCount =
                mutationEvidence.gpuScenePublication.removeCount;
            sampleMutationEvidence.gpuSceneNoOpCount =
                mutationEvidence.gpuScenePublication.noOpCount;
            sampleMutationEvidence.gpuSceneSubmittedUploadCount =
                mutationEvidence.gpuSceneUpload.submittedUploadCount;
            sampleMutationEvidence.gpuSceneUploadBytes =
                mutationEvidence.gpuSceneUpload.uploadBytes;
            sampleMutationEvidence.gpuSceneUploadRangeCount =
                mutationEvidence.gpuSceneUpload.uploadRangeCount;
            sampleMutationEvidence.gpuSceneFullUploadCount =
                mutationEvidence.gpuSceneUpload.fullUploadCount;
            sampleMutationEvidence.gpuSceneSubmittedUploadedRowCount =
                mutationEvidence.gpuSceneUpload.submittedUploadedRowCount;
            sampleMutationEvidence.gpuCullingInstancePatchedRowCount =
                mutationEvidence.gpuCulling.instancePatchedRowCount;
            sampleMutationEvidence.gpuCullingCandidatePatchedRowCount =
                mutationEvidence.gpuCulling.candidatePatchedRowCount;
            sampleMutationEvidence.gpuCullingActiveRowPatchedRowCount =
                mutationEvidence.gpuCulling.activeRowPatchedRowCount;
            sampleMutationEvidence.gpuCullingInstanceUploadedRowCount =
                mutationEvidence.gpuCulling.instanceUploadedRowCount;
            sampleMutationEvidence.gpuCullingCandidateUploadedRowCount =
                mutationEvidence.gpuCulling.candidateUploadedRowCount;
            sampleMutationEvidence.gpuCullingActiveRowUploadedRowCount =
                mutationEvidence.gpuCulling.activeRowUploadedRowCount;
            sampleMutationEvidence.gpuCullingInstanceUploadBytes =
                mutationEvidence.gpuCulling.instanceUploadBytes;
            sampleMutationEvidence.gpuCullingCandidateUploadBytes =
                mutationEvidence.gpuCulling.candidateUploadBytes;
            sampleMutationEvidence.gpuCullingActiveRowUploadBytes =
                mutationEvidence.gpuCulling.activeRowUploadBytes;
            sampleMutationEvidence.gpuCullingInstanceFullMaterializationCount =
                mutationEvidence.gpuCulling.instanceFullMaterializationCount;
            sampleMutationEvidence.gpuCullingCandidateFullMaterializationCount =
                mutationEvidence.gpuCulling.candidateFullMaterializationCount;
            sampleMutationEvidence.gpuCullingActiveRowFullMaterializationCount =
                mutationEvidence.gpuCulling.activeRowFullMaterializationCount;
            sampleMutationEvidence.gpuCullingContinuityFullMaterializationCount =
                mutationEvidence.gpuCulling.continuityFullMaterializationCount;
            sampleMutationEvidence.gpuCullingCapacityFullMaterializationCount =
                mutationEvidence.gpuCulling.capacityFullMaterializationCount;
            sampleMutationEvidence.directRasterInstancePatchedRowCount =
                mutationEvidence.directRaster.instancePatchedRowCount;
            sampleMutationEvidence.directRasterIndexPatchedRowCount =
                mutationEvidence.directRaster.indexPatchedRowCount;
            sampleMutationEvidence.directRasterInstanceUploadBytes =
                mutationEvidence.directRaster.instanceUploadBytes;
            sampleMutationEvidence.directRasterIndexUploadBytes =
                mutationEvidence.directRaster.indexUploadBytes;
            sampleMutationEvidence.directRasterInstanceFullMaterializationCount =
                mutationEvidence.directRaster.instanceFullMaterializationCount;
            sampleMutationEvidence.directRasterIndexFullMaterializationCount =
                mutationEvidence.directRaster.indexFullMaterializationCount;
            sampleMutationEvidence.gpuCullingOwnerCount =
                mutationEvidence.gpuCullingOwnerCount;
            sampleMutationEvidence.directRasterOwnerCount =
                mutationEvidence.directRasterOwnerCount;
            result.engineRenderRuntimeAvailable = engineDiagnostics.available;
            result.engineRequiredSceneFrameSequence =
                engineDiagnostics.requiredSceneFrameSequence;
            result.engineRequiredSceneRevision =
                engineDiagnostics.requiredSceneRevision;
            result.temporalEpoch = engineDiagnostics.temporalEpoch;
            result.activeCameraIdentity = engineDiagnostics.cameraIdentity;
            result.activeCameraCutRevision =
                engineDiagnostics.cameraCutRevision;
            result.temporalResetCount = engineDiagnostics.temporalResetCount;
            result.acceptedExtractionDiagnosticsAvailable =
                engineDiagnostics.available;
            const AcceptedExtractionDiagnosticsSnapshot& acceptedExtraction =
                engineDiagnostics.acceptedExtractionDiagnostics;
            result.acceptedExtractionPublicationCount =
                acceptedExtraction.acceptedPublicationCount;
            result.acceptedExtractionLastSourceFrameSequence =
                acceptedExtraction.lastAcceptedSourceFrameSequence;
            result.acceptedExtractionLastSceneRevision =
                acceptedExtraction.lastAcceptedSceneRevision;
            result.acceptedExtractionCumulativeFullScanCount =
                acceptedExtraction.cumulativeFullScanCount;
            result.acceptedExtractionCumulativeChangeFeedChangeCount =
                acceptedExtraction.cumulativeChangeFeedChangeCount;
            result.acceptedExtractionCumulativeActorRebuildCount =
                acceptedExtraction.cumulativeActorRebuildCount;
            result.acceptedExtractionCumulativeProxyVisitCount =
                acceptedExtraction.cumulativeProxyVisitCount;
            result.acceptedExtractionCumulativeComponentVisitCount =
                acceptedExtraction.cumulativeComponentVisitCount;
            result.acceptedExtractionCumulativeFeatureProviderVisitCount =
                acceptedExtraction.cumulativeFeatureProviderVisitCount;
            result.acceptedExtractionContinuityLossCount =
                acceptedExtraction.continuityLossCount;
            if (engineDiagnostics.available)
            {
                // Sequence zero is an observed state before the first
                // application frame; it is not the same as a missing runtime
                // observation.
                result.lastSubmittedFrameSequence =
                    DiagnosticValue<uint64>::Available(
                        renderDiagnostics.lastSubmittedFrameSequence);
                result.lastPresentedFrameSequence =
                    DiagnosticValue<uint64>::Available(
                        renderDiagnostics.lastPresentedFrameSequence);
            }
            else
            {
                constexpr const char* RenderRuntimeUnavailable =
                    "Engine render-runtime composition is unavailable.";
                result.lastSubmittedFrameSequence =
                    DiagnosticValue<uint64>::Unavailable(
                        RenderRuntimeUnavailable);
                result.lastPresentedFrameSequence =
                    DiagnosticValue<uint64>::Unavailable(
                        RenderRuntimeUnavailable);
            }
            if (engine.IsInitialized())
            {
                result.engineUpdateTickCount =
                    DiagnosticValue<uint64>::Available(
                        engine.GetFrameNumber());
            }
            else
            {
                result.engineUpdateTickCount =
                    DiagnosticValue<uint64>::Unavailable(
                        "Engine is not initialized.");
            }
            const RenderNativeValidationDiagnostics& nativeValidation =
                renderDiagnostics.nativeValidation;
            result.nativeValidationAvailable = nativeValidation.available;
            result.nativeValidationEnabled = nativeValidation.enabled;
            result.nativeValidationReadComplete =
                nativeValidation.readComplete;
            result.nativeValidationWarningCount =
                nativeValidation.warningCount;
            result.nativeValidationErrorCount = nativeValidation.errorCount;
            result.nativeValidationCorruptionCount =
                nativeValidation.corruptionCount;
            result.requestedPostProcessEffectCount =
                diagnostics.requestedPostProcessEffectCount;
            result.enabledPostProcessEffectCount =
                diagnostics.enabledPostProcessEffectCount;
            result.unsupportedPostProcessSkippedCount =
                diagnostics.unsupportedPostProcessSkippedCount;
            result.postProcessGraphPassCount =
                diagnostics.postProcessGraphPassCount;
            result.postProcessActualExecution =
                MakeSamplePostProcessExecutionDiagnostics(renderDiagnostics);
            result.ambientOcclusionActualExecution =
                MakeSampleAmbientOcclusionExecutionDiagnostics(renderDiagnostics);
            result.hizReflectionActualExecution =
                MakeSampleHiZReflectionExecutionDiagnostics(renderDiagnostics);
            result.toonLightingActualExecution =
                ProjectSampleToonLightingExecutionDiagnostics(
                    renderDiagnostics);
            result.toonOutlineActualExecution =
                ProjectSampleToonOutlineExecutionDiagnostics(
                    renderDiagnostics);
            result.particleVfxActualExecution =
                ProjectSampleParticleVfxExecutionDiagnostics(
                    renderDiagnostics);
            result.waterSurfaceActualExecution =
                ProjectSampleWaterSurfaceExecutionDiagnostics(
                    renderDiagnostics);
            result.weightedBlendedOITActualExecution =
                MakeSampleWeightedBlendedOITExecutionDiagnostics(
                    renderDiagnostics);
            result.transparentAlphaBlendDrawCount =
                diagnostics.transparent.alphaBlendDrawCount;
            result.transparentPremultipliedDrawCount =
                diagnostics.transparent.premultipliedDrawCount;
            result.transparentAdditiveDrawCount =
                diagnostics.transparent.additiveDrawCount;
            result.transparentThinTransmissionDrawCount =
                diagnostics.transparent.thinTransmissionDrawCount;
            result.transparentOpaqueSceneColorPyramidReady =
                diagnostics.transparent.opaqueSceneColorPyramidReady;
            result.transparentOpaqueSceneColorPyramidBuilt =
                diagnostics.transparent.opaqueSceneColorPyramidBuilt;
            result.transparentReactiveMaskAttachmentBound =
                diagnostics.transparent.reactiveMaskAttachmentBound;
            result.transparentReactiveMaskWriteDrawCount =
                diagnostics.transparent.reactiveMaskWriteDrawCount;
            result.transparentPriorityBucketCount =
                diagnostics.transparent.translucencyPriorityBucketCount;
            result.transparentFallbackCount =
                diagnostics.transparent.translucencyFallbackCount;
            const RenderFrameFeatureDiagnostics& frameFeatures =
                renderDiagnostics.frameFeatures;
            const RenderExecutionReceipt& executionReceipt =
                frameFeatures.executionReceipt;
            result.transparentFrameFeatureSequence = frameFeatures.frameSequence;
            result.transparentReceiptFrameSequence =
                executionReceipt.frameIdentity.frameSequence;
            result.transparentLastPresentedFrameSequence =
                renderDiagnostics.lastPresentedFrameSequence;
            result.transparentPresentedFrameAligned =
                frameFeatures.available &&
                executionReceipt.schemaVersion ==
                    RVX_RENDER_EXECUTION_RECEIPT_SCHEMA_VERSION &&
                executionReceipt.IsStructurallyValid() &&
                executionReceipt.available && executionReceipt.graphComposed &&
                executionReceipt.executionSucceeded && executionReceipt.presented &&
                executionReceipt.succeeded &&
                executionReceipt.viewIdentity.role == ViewRole::Main &&
                executionReceipt.viewIdentity.subViewIndex == 0 &&
                result.transparentFrameFeatureSequence != 0 &&
                result.transparentFrameFeatureSequence ==
                    result.transparentReceiptFrameSequence &&
                result.transparentReceiptFrameSequence ==
                    result.transparentLastPresentedFrameSequence;
            result.clusteredLightingInitialized =
                diagnostics.clusteredLightingInitialized;
            result.clusteredLightingActiveClusters =
                diagnostics.clusteredLightingActiveClusters;
            result.textureIBLEnabled = diagnostics.textureIBLEnabled;
            result.directionalShadowAvailable =
                diagnostics.directionalShadow.available;
            result.directionalShadowRequested =
                diagnostics.directionalShadow.requested;
            result.directionalShadowSupported =
                diagnostics.directionalShadow.supported;
            result.directionalShadowOutputReady =
                diagnostics.directionalShadow.outputReady;
            result.directionalShadowSamplingEnabled =
                diagnostics.directionalShadow.samplingEnabled;
            result.directionalShadowRequestedCascadeCount =
                diagnostics.directionalShadow.requestedCascadeCount;
            result.directionalShadowProducedCascadeCount =
                diagnostics.directionalShadow.producedCascadeCount;
            result.directionalShadowResolvedCascadeCount =
                diagnostics.directionalShadow.resolvedCascadeCount;
            result.directionalShadowMapSize =
                diagnostics.directionalShadow.shadowMapSize;
            result.directionalShadowCasterCount =
                diagnostics.directionalShadow.shadowCasterCount;
            result.directionalShadowDrawCount =
                diagnostics.directionalShadow.drawCount;
            result.directionalShadowReason =
                diagnostics.directionalShadow.reason;
            const RenderLocalLightingDiagnostics& localLighting =
                diagnostics.localLighting;
            result.localLightingAvailable = localLighting.available;
            result.pointLightRequestedCount =
                localLighting.pointLightRequested;
            result.pointLightAdmittedCount =
                localLighting.pointLightAdmitted;
            result.pointLightCapacity = localLighting.pointLightCapacity;
            result.pointLightOverflowCount =
                localLighting.pointLightOverflow;
            result.spotLightRequestedCount =
                localLighting.spotLightRequested;
            result.spotLightAdmittedCount =
                localLighting.spotLightAdmitted;
            result.spotLightCapacity = localLighting.spotLightCapacity;
            result.spotLightOverflowCount =
                localLighting.spotLightOverflow;
            result.pointShadowRequestedCount =
                localLighting.pointShadowRequested;
            result.pointShadowSupported =
                localLighting.pointShadowSupported;
            result.pointShadowReason = localLighting.pointShadowReason;
            result.spotShadowRequestedCount =
                localLighting.spotShadowRequested;
            result.spotShadowSupported = localLighting.spotShadowSupported;
            result.spotShadowReason = localLighting.spotShadowReason;
            result.localShadowRequestCount =
                localLighting.localShadowRequestCount;
            result.localShadowCasterDrawCount =
                localLighting.localShadowCasterDrawCount;
            result.localShadowAtlasReady =
                localLighting.localShadowAtlasReady;
            result.localShadowFallbackReason =
                localLighting.localShadowReason;
            result.hzbRequested = diagnostics.hzb.requested;
            result.hzbSupported = diagnostics.hzb.supported;
            result.hzbEnabled = diagnostics.hzb.enabled;
            result.hzbReason = diagnostics.hzb.reason;
            const RenderTransparentPassDiagnostics& transparent =
                diagnostics.transparent;
            result.transparentAvailable = transparent.available;
            result.transparentOrderValid = transparent.orderValid;
            result.transparentOrderHash = transparent.orderHash;
            result.transparentRejectedNonFiniteDepthCount =
                transparent.rejectedNonFiniteDepthCount;
            result.transparentCandidateDrawItemCount =
                transparent.candidateDrawItemCount;
            result.transparentPreparedDrawItemCount =
                transparent.preparedDrawItemCount;
            result.transparentExecutedPacketCount =
                transparent.executedPacketCount;
            result.transparentExecutedDrawCount =
                transparent.executedDrawCount;
            result.transparentSkippedMaterialBindingCount =
                transparent.skippedMaterialBindingCount;
            result.transparentSkippedResourceCount =
                transparent.skippedResourceCount;
            result.transparentSkippedExecutionDrawCount =
                transparent.skippedExecutionDrawCount;
            result.transparentMaterialBindingCount =
                transparent.materialBindingCount;
            result.transparentMaterialFallbackBindingCount =
                transparent.materialFallbackBindingCount;
            result.transparentMaterialTextureFlags =
                transparent.materialTextureFlags;
            result.transparentMaterialFallbackTextureFlags =
                transparent.materialFallbackTextureFlags;
            result.transparentNoWork = transparent.noWork;
            result.transparentPreflightFailed = transparent.preflightFailed;
            result.transparentExecutionFailed = transparent.executionFailed;
            result.materialReady = diagnostics.material.ready;
            result.materialUsedFallback = diagnostics.material.usedFallback;
            result.materialConstantsUpdated =
                diagnostics.material.constantsUpdated;
            result.materialDescriptorSetAvailable =
                diagnostics.material.descriptorSetAvailable;
            result.materialTextureFlags = diagnostics.material.textureFlags;
            result.presentedMaterialBindingsAvailable =
                diagnostics.material.presentedBindingsAvailable;
            result.presentedMaterialBindingsOverflow =
                diagnostics.material.presentedBindingsOverflow;
            result.presentedMaterialBindings.reserve(
                diagnostics.material.presentedBindings.size());
            for (const RenderPresentedMaterialBindingReceipt& receipt :
                 diagnostics.material.presentedBindings)
            {
                result.presentedMaterialBindings.push_back(
                    {receipt.material,
                     receipt.contentRevision,
                     receipt.descriptorContentKey,
                     receipt.descriptorRevision,
                     receipt.textureEntries,
                     receipt.fallbackUsed,
                     receipt.frameSequence,
                     receipt.presentationSequence,
                     receipt.parameterLayoutHash,
                     receipt.featureMask,
                     receipt.shadingModel,
                     receipt.surfacePathEligibility});
            }
            result.presentedSkinningPalettesAvailable =
                diagnostics.skinning.presentedReceiptsAvailable;
            result.presentedSkinningPalettesOverflow =
                diagnostics.skinning.presentedReceiptsOverflow;
            result.presentedSkinningPalettes.reserve(
                diagnostics.skinning.presentedReceipts.size());
            for (const RenderPresentedSkinningPaletteReceipt& receipt :
                 diagnostics.skinning.presentedReceipts)
            {
                result.presentedSkinningPalettes.push_back(
                    {receipt.providerComponentId,
                     receipt.sourceModelResourceId,
                     receipt.poseSequence,
                     receipt.paletteHash,
                     receipt.paletteCount,
                     receipt.lane,
                     receipt.frameSequence,
                     receipt.presentationSequence});
            }
            result.renderPendingUploadCount = diagnostics.pendingUploadCount;
            result.renderRetirementEntryCount =
                renderDiagnostics.retirement.entryCount;
            const RenderInstancingDiagnostics& instancing =
                diagnostics.instancing;
            result.instancingRequestedMode = GetRenderInstancingModeName(
                instancing.requestedMode);
            result.opaqueInstancingPlanAvailable =
                instancing.opaquePlanAvailable;
            result.opaqueInstancingPreflightSucceeded =
                instancing.opaquePreflightSucceeded;
            result.opaqueInstancingPlannedPacketCount =
                instancing.opaquePlannedPacketCount;
            result.opaqueInstancingPlannedDrawCount =
                instancing.opaquePlannedDrawCount;
            result.opaqueInstancingPlannedInstanceCount =
                instancing.opaquePlannedInstanceCount;
            result.opaqueInstancingPlannedBatchCount =
                instancing.opaquePlannedBatchCount;
            result.opaqueInstancingExecutedPacketCount =
                instancing.opaqueExecutedPacketCount;
            result.opaqueInstancingSubmittedDrawCount =
                instancing.opaqueSubmittedDrawCount;
            result.opaqueInstancingSubmittedInstanceCount =
                instancing.opaqueSubmittedInstanceCount;
            result.opaqueInstancingBatchCount =
                instancing.opaqueInstancedBatchCount;
            result.opaqueInstancingFallbackBatchCount =
                instancing.opaqueFallbackBatchCount;
            const RenderGPUDrivenCullingDiagnostics& gpuDriven =
                diagnostics.gpuDrivenCulling;
            const auto copyRasterTranscript =
                [](const RasterTranscriptDigest& source,
                   SampleRasterTranscript& destination)
            {
                destination.available = source.available;
                destination.entryCount = source.entryCount;
                destination.orderedIdentityHash = source.orderedIdentityHash;
                destination.consumedPayloadHash = source.consumedPayloadHash;
                destination.unorderedIdentityHash = source.unorderedIdentityHash;
                destination.unorderedIdentityHashSecondary =
                    source.unorderedIdentityHashSecondary;
                destination.unorderedConsumedPayloadHash =
                    source.unorderedConsumedPayloadHash;
                destination.unorderedConsumedPayloadHashSecondary =
                    source.unorderedConsumedPayloadHashSecondary;
            };
            const auto copyGPUSceneQualification =
                [&copyRasterTranscript](
                    const GPUSceneCullingQualificationDiagnostics& source,
                    SampleGPUSceneCullingQualification& destination)
            {
                destination.requested = source.requested;
                destination.required = source.required;
                destination.readbackAllocated = source.readbackAllocated;
                destination.copyRecorded = source.copyRecorded;
                destination.submissionAccepted = source.submissionAccepted;
                destination.completionObserved = source.completionObserved;
                destination.compared = source.compared;
                destination.matched = source.matched;
                destination.inputCoverageCompared = source.inputCoverageCompared;
                destination.inputCoverageMatched = source.inputCoverageMatched;
                destination.directVisibilityCoverageCompared =
                    source.directVisibilityCoverageCompared;
                destination.directVisibilityCoverageMatched =
                    source.directVisibilityCoverageMatched;
                destination.cullOutputsCompared = source.cullOutputsCompared;
                destination.cullOutputsMatched = source.cullOutputsMatched;
                destination.indirectArgumentsCompared =
                    source.indirectArgumentsCompared;
                destination.indirectArgumentsMatched =
                    source.indirectArgumentsMatched;
                destination.rasterPayloadCompared = source.rasterPayloadCompared;
                destination.rasterPayloadMatched = source.rasterPayloadMatched;
                destination.mismatch =
                    GetGPUSceneCullingQualificationMismatchName(source.mismatch);
                destination.activeRowCount = source.activeRowCount;
                destination.drawGroupCount = source.drawGroupCount;
                destination.expectedInputPacketCount =
                    source.expectedInputPacketCount;
                destination.expectedGPUInputPacketCount =
                    source.expectedGPUInputPacketCount;
                destination.observedGPUInputPacketCount =
                    source.observedGPUInputPacketCount;
                destination.expectedDirectVisiblePacketCount =
                    source.expectedDirectVisiblePacketCount;
                destination.observedGPUVisiblePacketCount =
                    source.observedGPUVisiblePacketCount;
                destination.missingDirectVisiblePacketCount =
                    source.missingDirectVisiblePacketCount;
                destination.gpuOnlyVisiblePacketCount =
                    source.gpuOnlyVisiblePacketCount;
                destination.directInputPacketCount = source.directInputPacketCount;
                destination.skippedInputPacketCount =
                    source.skippedInputPacketCount;
                destination.expectedVisibleInstanceCount =
                    source.expectedVisibleInstanceCount;
                destination.observedVisibleInstanceCount =
                    source.observedVisibleInstanceCount;
                destination.expectedSubmittedDrawCount =
                    source.expectedSubmittedDrawCount;
                destination.observedSubmittedDrawCount =
                    source.observedSubmittedDrawCount;
                destination.firstMismatchActiveRow = source.firstMismatchActiveRow;
                destination.firstMismatchDrawGroup = source.firstMismatchDrawGroup;
                destination.firstMismatchResidentRow =
                    source.firstMismatchResidentRow;
                destination.expectedValue = source.expectedValue;
                destination.observedValue = source.observedValue;
                destination.expectedGPUInputIdentityHash =
                    source.expectedGPUInputIdentityHash;
                destination.observedGPUInputIdentityHash =
                    source.observedGPUInputIdentityHash;
                destination.planPacketIdentityHash =
                    source.planPacketIdentityHash;
                destination.expectedDirectVisibleIdentityHash =
                    source.expectedDirectVisibleIdentityHash;
                destination.observedGPUVisibleIdentityHash =
                    source.observedGPUVisibleIdentityHash;
                destination.expectedRasterPayloadHash =
                    source.expectedRasterPayloadHash;
                destination.observedRasterPayloadHash =
                    source.observedRasterPayloadHash;
                destination.expectedIndirectArgumentsHash =
                    source.expectedIndirectArgumentsHash;
                destination.observedIndirectArgumentsHash =
                    source.observedIndirectArgumentsHash;
                copyRasterTranscript(source.tierOneRasterTranscript,
                                     destination.tierOneRasterTranscript);
                copyRasterTranscript(
                    source.tierOneRasterTranscriptReference,
                    destination.tierOneRasterTranscriptReference);
                destination.tierOneRasterTranscriptCompared =
                    source.tierOneRasterTranscriptCompared;
                destination.tierOneRasterTranscriptMatched =
                    source.tierOneRasterTranscriptMatched;
                destination.firstRasterTranscriptMismatchEntry =
                    source.firstRasterTranscriptMismatchEntry;
                destination.expectedRasterTranscriptIdentityHash =
                    source.expectedRasterTranscriptIdentityHash;
                destination.observedRasterTranscriptIdentityHash =
                    source.observedRasterTranscriptIdentityHash;
                destination.expectedRasterTranscriptPayloadHash =
                    source.expectedRasterTranscriptPayloadHash;
                destination.observedRasterTranscriptPayloadHash =
                    source.observedRasterTranscriptPayloadHash;
                destination.cpuPayloadBytes = source.cpuPayloadBytes;
                destination.capturedTier = GetGPUDrivenTierName(source.capturedTier);
                destination.frameSequence = source.frameSequence;
                destination.recordEpoch = source.recordEpoch;
                destination.gpuSceneLeaseVersion = source.gpuSceneLeaseVersion;
                destination.candidateVersion = source.candidateVersion;
                destination.activeRowVersion = source.activeRowVersion;
                destination.completionValue = source.completionValue;
            };
            copyGPUSceneQualification(
                gpuDriven.gpuSceneDepthQualification,
                result.gpuSceneDepthQualification);
            copyGPUSceneQualification(
                gpuDriven.gpuSceneOpaqueQualification,
                result.gpuSceneOpaqueQualification);
            const auto copyDirectRasterReadbackQualification =
                [&copyRasterTranscript](
                    const DirectRasterReadbackQualificationDiagnostics& source,
                    SampleDirectRasterReadbackQualification& destination)
            {
                destination.requested = source.requested;
                destination.required = source.required;
                destination.readbackAllocated = source.readbackAllocated;
                destination.copyRecorded = source.copyRecorded;
                destination.submissionAccepted = source.submissionAccepted;
                destination.completionObserved = source.completionObserved;
                destination.compared = source.compared;
                destination.matched = source.matched;
                destination.identity = source.identity;
                destination.allDirectDrawsInstanced =
                    source.allDirectDrawsInstanced;
                destination.mismatch =
                    GetDirectRasterReadbackQualificationMismatchName(
                        source.mismatch);
                destination.frameSequence = source.frameSequence;
                destination.recordEpoch = source.recordEpoch;
                destination.sourceFrameSlot = source.sourceFrameSlot;
                destination.firstMismatchIndex = source.firstMismatchIndex;
                destination.firstMismatchRow = source.firstMismatchRow;
                destination.cpuPayloadBytes = source.cpuPayloadBytes;
                destination.completionValue = source.completionValue;
                copyRasterTranscript(source.expectedTranscript,
                                     destination.expectedTranscript);
                copyRasterTranscript(source.observedTranscript,
                                     destination.observedTranscript);
            };
            copyDirectRasterReadbackQualification(
                gpuDriven.directOpaqueRasterReadbackQualification,
                result.directOpaqueRasterReadbackQualification);
            result.gpuDrivenPolicyDecisionAvailable =
                gpuDriven.policyDecisionAvailable;
            result.gpuDrivenRequestedMode = GetRenderGPUDrivenModeName(
                gpuDriven.policyDecision.requestedMode);
            result.gpuDrivenPolicyReason = GetGPUDrivenPolicyReasonName(
                gpuDriven.policyDecision.reason);
            result.gpuDrivenQualification =
                GetGPUDrivenQualificationLevelName(
                    gpuDriven.policyDecision.qualificationLevel);
            result.gpuDrivenBackendQualified =
                gpuDriven.policyDecision.backendQualified;
            result.gpuDrivenCapabilitiesReady =
                gpuDriven.policyDecision.capabilitiesReady;
            result.gpuDrivenPipelineReady =
                gpuDriven.policyDecision.pipelineReady;
            result.gpuDrivenEnabled = gpuDriven.enabled;
            result.gpuDrivenGraphPassAdded = gpuDriven.graphPassAdded;
            result.gpuDrivenGraphPassRecorded = gpuDriven.graphPassRecorded;
            result.gpuDrivenExecutionRecorded =
                gpuDriven.gpuExecutionRecorded;
            result.gpuDrivenInstanceUploadBytes =
                gpuDriven.instanceUploadBytes;
            result.gpuDrivenCandidateUploadBytes =
                gpuDriven.gpuSceneCandidateUploadBytes;
            result.gpuDrivenActiveRowUploadBytes =
                gpuDriven.activeRowUploadBytes;
            result.gpuDrivenActiveRowCount = gpuDriven.activeRowCount;
            result.gpuDrivenActiveRowHighWatermark =
                gpuDriven.activeRowHighWatermark;
            result.gpuDrivenInstancePatchedRowCount =
                gpuDriven.instancePatchedRowCount;
            result.gpuDrivenCandidatePatchedRowCount =
                gpuDriven.gpuSceneCandidatePatchedRowCount;
            result.gpuDrivenActiveRowPatchedRowCount =
                gpuDriven.activeRowPatchedRowCount;
            result.gpuDrivenInstanceFullMaterializationCount =
                gpuDriven.instanceFullMaterializationCount;
            result.gpuDrivenCandidateFullMaterializationCount =
                gpuDriven.gpuSceneCandidateFullMaterializationCount;
            result.gpuDrivenActiveRowFullMaterializationCount =
                gpuDriven.activeRowFullMaterializationCount;
            result.gpuDrivenContinuityFullMaterializationCount =
                gpuDriven.continuityFullMaterializationCount;
            result.gpuDrivenCapacityFullMaterializationCount =
                gpuDriven.capacityFullMaterializationCount;
            result.directRasterInstanceUploadBytes =
                gpuDriven.directRasterInstanceUploadBytes;
            result.directRasterInstanceIndexUploadBytes =
                gpuDriven.directRasterInstanceIndexUploadBytes;
            result.gpuDrivenCanonicalInstanceUploadWork =
                CopyRenderUploadWorkDiagnostics(
                    gpuDriven.canonicalInstanceUploadWork);
            result.gpuDrivenCanonicalCandidateUploadWork =
                CopyRenderUploadWorkDiagnostics(
                    gpuDriven.canonicalCandidateUploadWork);
            result.gpuDrivenCanonicalActiveRowUploadWork =
                CopyRenderUploadWorkDiagnostics(
                    gpuDriven.canonicalActiveRowUploadWork);
            result.directRasterInstanceUploadWork =
                CopyRenderUploadWorkDiagnostics(
                    gpuDriven.directRasterInstanceUploadWork);
            result.directRasterIndexUploadWork =
                CopyRenderUploadWorkDiagnostics(
                    gpuDriven.directRasterIndexUploadWork);
            result.directRasterInstancePatchedRowCount =
                gpuDriven.directRasterInstancePatchedRowCount;
            result.directRasterIndexPatchedRowCount =
                gpuDriven.directRasterIndexPatchedRowCount;
            result.directRasterActiveInstanceCount =
                gpuDriven.directRasterActiveInstanceCount;
            result.directRasterActiveInstanceCapacity =
                gpuDriven.directRasterActiveInstanceCapacity;
            result.directRasterInstanceFullMaterializationCount =
                gpuDriven.directRasterInstanceFullMaterializationCount;
            result.directRasterIndexFullMaterializationCount =
                gpuDriven.directRasterIndexFullMaterializationCount;
            const RenderExtractionDiagnostics& extraction =
                diagnostics.extraction;
            result.extractionAvailable =
                extraction.complete &&
                extraction.code == RenderExtractionCode::Complete;
            result.extractionFullScanCount = extraction.fullScanCount;
            result.extractionChangeFeedChangeCount =
                extraction.changeFeedChangeCount;
            result.extractionActorRebuildCount =
                extraction.actorRebuildCount;
            result.extractionProxyVisitCount = extraction.proxyVisitCount;
            result.extractionComponentVisitCount =
                extraction.componentVisitCount;
            result.extractionFeatureProviderVisitCount =
                extraction.featureProviderVisitCount;
            result.extractionContinuityLost = extraction.continuityLost;
            result.gpuDrivenGraphInputDrawItemCount =
                gpuDriven.graphInputDrawItemCount;
            result.gpuDrivenVisibilityCandidateCount =
                gpuDriven.visibilityCandidateCount;
            result.gpuDrivenVisibleCullableDrawItemCount =
                gpuDriven.visibleCullableDrawItemCount;
            result.gpuDrivenCpuReferenceVisibleCount =
                gpuDriven.cpuReferenceVisibleCullableDrawItemCount;
            result.gpuDrivenCpuReferenceCulledCount =
                gpuDriven.cpuReferenceCulledDrawItemCount;
            result.gpuDrivenOpaqueIndirectRequested =
                gpuDriven.opaqueIndirectRequested;
            result.gpuDrivenOpaqueIndirectEligible =
                gpuDriven.opaqueIndirectEligible;
            result.gpuDrivenOpaqueIndirectSubmitted =
                gpuDriven.opaqueIndirectSubmitted;
            result.gpuDrivenOpaqueDirectDrawCount =
                gpuDriven.opaqueDirectDrawCount;
            result.gpuDrivenOpaqueIndirectBatchCount =
                gpuDriven.opaqueGpuDrivenIndirectBatchCount;
            result.gpuDrivenOpaqueSubmittedSourcePacketCount =
                gpuDriven.opaqueGpuDrivenSubmittedSourcePacketCount;
            result.gpuDrivenOpaqueIndirectDrawUpperBound =
                gpuDriven.opaqueGpuDrivenIndirectSubmittedDrawUpperBound;
            result.gpuDrivenOpaqueFallbackReason =
                GetGPUDrivenDrawFallbackReasonName(
                    gpuDriven.opaqueFallbackReason);
            copyRasterTranscript(gpuDriven.directOpaqueRasterTranscript,
                                 result.directOpaqueRasterTranscript);

            const RenderSceneWorkDiagnostics& sceneWork =
                diagnostics.sceneWork;
            result.sceneWorkAvailable = sceneWork.available;
            result.sceneFullRebuildCount = sceneWork.fullRebuildCount;
            result.sceneIncrementalUpdateCount =
                sceneWork.incrementalUpdateCount;
            result.sceneStaticReuseCount = sceneWork.staticReuseCount;
            result.sceneAppliedRevision = sceneWork.appliedSceneRevision;
            result.sceneLastRebuiltObjectCount =
                sceneWork.lastRebuiltObjectCount;
            result.sceneLastRemovedObjectCount =
                sceneWork.lastRemovedObjectCount;
            result.drawPacketResolveCount = sceneWork.drawPacketResolveCount;
            result.drawPacketHitCount = sceneWork.drawPacketHitCount;
            result.drawPacketMissCount = sceneWork.drawPacketMissCount;
            result.drawPacketDynamicBypassCount =
                sceneWork.drawPacketDynamicBypassCount;
            result.drawPacketBuildCount = sceneWork.drawPacketBuildCount;
            result.drawPacketEntryCreationCount =
                sceneWork.drawPacketEntryCreationCount;
            result.drawPacketInvalidationCount =
                sceneWork.drawPacketInvalidationCount;
            result.drawPacketObjectRevisionInvalidationCount =
                sceneWork.drawPacketObjectRevisionInvalidationCount;
            result.drawPacketClearCount = sceneWork.drawPacketClearCount;
            result.drawPacketEntryCount = sceneWork.drawPacketEntryCount;

            const GPUSceneDiagnostics& gpuScene = diagnostics.gpuScene;
            result.gpuSceneAvailable = gpuScene.available;
            result.gpuScenePublicationAttempted =
                gpuScene.publicationAttempted;
            result.gpuScenePublicationPublished =
                gpuScene.publicationPublished;
            result.gpuScenePublicationFailed = gpuScene.publicationFailed;
            result.gpuScenePublicationComplete =
                gpuScene.publicationComplete;
            result.gpuSceneAttemptedObjectCount =
                gpuScene.attemptedObjectCount;
            result.gpuScenePublishedObjectCount =
                gpuScene.publishedObjectCount;
            result.gpuSceneAddCount = gpuScene.addCount;
            result.gpuSceneUpdateCount = gpuScene.updateCount;
            result.gpuSceneRemoveCount = gpuScene.removeCount;
            result.gpuSceneNoOpCount = gpuScene.noOpCount;
            result.gpuSceneCommittedVersion = gpuScene.committedVersion;
            result.gpuSceneResidentVersion = gpuScene.residentVersion;
            result.gpuSceneCpuPayloadBytes = gpuScene.cpuPayloadBytes;
            result.gpuSceneCpuReservedBytes = gpuScene.cpuReservedBytes;
            result.gpuSceneAllocationBytes = gpuScene.gpuAllocationBytes;
            result.gpuSceneFrameUploadBytes = gpuScene.frameUploadBytes;
            result.gpuSceneCumulativeUploadBytes =
                gpuScene.cumulativeUploadBytes;
            result.gpuSceneFrameUploadRangeCount =
                gpuScene.frameUploadRangeCount;
            result.gpuSceneCurrentBufferSetCount =
                gpuScene.currentBufferSetCount;
            result.gpuScenePendingBufferSetCount =
                gpuScene.pendingBufferSetCount;
            result.gpuSceneInFlightBufferSetCount =
                gpuScene.inFlightBufferSetCount;
            result.gpuSceneFullUpload = gpuScene.fullUpload;
            result.gpuSceneUploadWork = CopyRenderUploadWorkDiagnostics(
                gpuScene.uploadWork);
            for (uint32 tableIndex = 0;
                 tableIndex < GPU_SCENE_DIAGNOSTICS_TABLE_COUNT;
                 ++tableIndex)
            {
                result.gpuSceneTableUploadWork[tableIndex] =
                    CopyRenderUploadWorkDiagnostics(
                        gpuScene.tables[tableIndex].uploadWork);
            }

            const RenderPolicyDiagnostics& policy = diagnostics.policy;
            result.renderPolicyRequestAvailable = policy.requestAvailable;
            result.renderPolicyPlanAvailable = policy.planAvailable;
            result.renderPolicyReportAvailable = policy.reportAvailable;
            result.renderPolicyRequestFrameSequence = policy.request.frameSequence;
            result.renderPolicyPlanFrameSequence =
                policy.selectedPlan.frameSequence;
            result.renderPolicyReportFrameSequence =
                policy.executionReport.frameSequence;
            result.completedPresentedFrameSequence =
                engineDiagnostics.available
                    ? renderDiagnostics.lastPresentedFrameSequence
                    : 0;
            result.renderPolicyRequestedMode = GetRenderGPUDrivenModeName(
                policy.request.gpuDrivenMode);
            result.renderPolicyExecutionStatus = GetRenderExecutionStatusName(
                policy.executionReport.status);
            result.renderPolicySelectedTier = GetGPUDrivenTierName(
                policy.selectedPlan.viewPolicy.selectedTier);
            result.renderPolicyExecutedTier = GetGPUDrivenTierName(
                policy.executionReport.executedTier);
            result.renderPolicyTierFallbackReason =
                GetRenderPolicyReasonName(
                    policy.executionReport.tierFallbackReason);
            for (const RenderPassExecutionReport& pass :
                 policy.executionReport.passes)
            {
                if (pass.pass != RenderPassKind::Opaque)
                {
                    continue;
                }
                result.opaqueExecutionCompleted =
                    pass.status == RenderExecutionStatus::Completed;
                result.opaqueMaterialBindingsAvailable =
                    pass.materialBindingsAvailable;
                result.opaqueMaterialBindingCount =
                    pass.materialBindingCount;
                result.opaqueMaterialFallbackBindingCount =
                    pass.materialFallbackBindingCount;
                result.opaqueMaterialTextureFlags =
                    pass.materialTextureFlags;
                result.opaqueMaterialFallbackTextureFlags =
                    pass.materialFallbackTextureFlags;
                const RenderPassLaneExecutionReport* lanes[] = {
                    &pass.gpuDrivenLane,
                    &pass.directLane,
                };
                for (const RenderPassLaneExecutionReport* lane : lanes)
                {
                    if (lane->status == RenderExecutionStatus::Completed &&
                        lane->executedCountsAvailable)
                    {
                        result.opaqueExecutedDrawCountAvailable = true;
                        result.opaqueExecutedDrawCount +=
                            lane->executedDrawCount;
                    }
                }
                break;
            }
            return result;
        }

        SamplePixelProbeReport MakeSamplePixelProbeReport(
            const RenderFrameCaptureResult& capture)
        {
            SamplePixelProbeReport result;
            const RenderFramePixelProbeResult& probe = capture.pixelProbe;
            result.complete = capture.HasMatchingCompletedPixelProbe();
            result.resultCode = static_cast<uint32>(probe.code);
            result.requestId = probe.requestId;
            result.frameSequence = probe.frameSequence;
            result.requiredSceneRevision = probe.requiredSceneRevision;
            result.runtimeSurfaceGeneration = probe.runtimeSurfaceGeneration;
            result.x = probe.x;
            result.y = probe.y;
            result.message = probe.message;
            if (result.complete)
            {
                result.preToneRGBA16FloatBits = probe.preToneRGBA16FloatBits;
                result.finalBGRA8Bits = probe.finalBGRA8Bits;
            }
            return result;
        }

        void AppendRuntimeReport(const RenderDiagnosticsSnapshot& diagnostics,
                                 SampleFeatureReporter& reporter)
        {
            const RenderFrameFeatureDiagnostics& features =
                diagnostics.frameFeatures;
            if (features.graphCompiled)
            {
                reporter.Enable("RenderGraph");
            }
            if (features.rendered)
            {
                reporter.Enable("SceneRendering");
            }
            if (features.skybox.enabled)
            {
                reporter.Enable("SkyboxPass");
            }
            if (features.textureIBLEnabled)
            {
                reporter.Enable("TextureIBL");
            }
            if (features.directionalShadow.samplingEnabled)
            {
                reporter.Enable("DirectionalShadow");
            }
            if (features.enabledPostProcessEffectCount > 0 &&
                features.postProcessGraphPassCount > 0)
            {
                reporter.Enable("PostProcessRenderGraph");
            }
            if (features.gpuDrivenCulling.opaqueIndirectSubmitted)
            {
                reporter.Enable("GPUDrivenIndirectSubmission");
            }
            if (features.gpuDrivenCulling.opaqueDirectDrawCount > 0)
            {
                reporter.Enable("DirectDrawSubmission");
            }
            for (const std::string& feature : features.unsupportedFeatures)
            {
                reporter.Unsupported(feature);
            }
            for (const std::string& reason : features.fallbackReasons)
            {
                reporter.Fallback(reason);
            }
        }
    } // namespace

    const char* GetSampleAssessmentProfileName(
        SampleAssessmentProfile profile) noexcept
    {
        switch (profile)
        {
        case SampleAssessmentProfile::Smoke:
            return "smoke";
        case SampleAssessmentProfile::Qualification:
            return "qualification";
        case SampleAssessmentProfile::Benchmark:
            return "benchmark";
        default:
            return "invalid";
        }
    }

    SampleReadinessWaitBudget ResolveSampleReadinessWaitBudget(
        const SampleRunnerCLIOptions& options,
        const SampleWorkloadProfile* workload) noexcept
    {
        SampleReadinessWaitBudget budget{
            options.readyTimeoutMs,
            options.readyMaxFrames,
        };
        if (!options.RequiresReadinessWait() || workload == nullptr)
        {
            return budget;
        }

        switch (workload->scale)
        {
        case SampleWorkloadScale::PullRequest:
            if (!options.readyTimeoutExplicit)
            {
                budget.timeoutMs = 120000u;
            }
            if (!options.readyMaxFramesExplicit)
            {
                budget.maximumFrames = 600u;
            }
            break;
        case SampleWorkloadScale::Nightly:
            if (!options.readyTimeoutExplicit)
            {
                budget.timeoutMs = 900000u;
            }
            if (!options.readyMaxFramesExplicit)
            {
                budget.maximumFrames = 2000u;
            }
            break;
        case SampleWorkloadScale::Qualification:
            if (!options.readyTimeoutExplicit)
            {
                budget.timeoutMs = 3600000u;
            }
            if (!options.readyMaxFramesExplicit)
            {
                budget.maximumFrames = 10000u;
            }
            break;
        default:
            break;
        }
        return budget;
    }

    bool ParseSampleRunnerCLI(int argc,
                              const char* const* argv,
                              SampleRunnerCLIOptions& options,
                              std::string* outError)
    {
        std::vector<const char*> commonArguments;
        commonArguments.reserve(static_cast<size_t>(std::max(argc, 1)));
        commonArguments.push_back(argc > 0 && argv[0] ? argv[0]
                                                        : "RenderVerseSamples");

        for (int i = 1; i < argc; ++i)
        {
            const std::string_view argument = argv[i] ? argv[i] : "";
            if (argument == "--list")
            {
                options.listSamples = true;
                continue;
            }
            if (argument == "--wait-ready")
            {
                options.waitReady = true;
                continue;
            }
            if (argument == "--deterministic-orbit")
            {
                options.deterministicCameraOrbit = true;
                continue;
            }
            if (argument == "--runtime-only-composition")
            {
                options.runtimeOnlyComposition = true;
                continue;
            }
            if (argument == "--quality")
            {
                options.qualityExplicit = true;
            }
            if (RequiresRunnerValue(argument))
            {
                if (i + 1 >= argc || !argv[i + 1])
                {
                    SetError(outError,
                             "Missing value for " + std::string(argument));
                    return false;
                }
                const char* value = argv[++i];
                if (argument == "--sample")
                {
                    options.sampleId = value;
                }
                else if (argument == "--asset")
                {
                    options.assetId = value;
                }
                else if (argument == "--model")
                {
                    options.modelPath = value;
                }
                else if (argument == "--environment")
                {
                    options.environmentId = value;
                }
                else if (argument == "--environment-file")
                {
                    options.environmentPath = value;
                }
                else if (argument == "--pipeline")
                {
                    if (!ParseSamplePipelineRecipe(value,
                                                   options.pipelineRecipe) ||
                        options.pipelineRecipe ==
                            SamplePipelineRecipe::Minimal ||
                        options.pipelineRecipe ==
                            SamplePipelineRecipe::Headless)
                    {
                        SetError(outError,
                                 "Invalid --pipeline value: " +
                                     std::string(value));
                        return false;
                    }
                    options.pipelineExplicit = true;
                }
                else if (argument == "--view")
                {
                    options.viewBookmarkId = value;
                    if (options.viewBookmarkId.empty())
                    {
                        SetError(outError, "--view must not be empty");
                        return false;
                    }
                    options.viewBookmarkExplicit = true;
                }
                else if (argument == "--fixed-delta")
                {
                    if (!ParsePositiveRunnerFloat64(
                            value, options.fixedDeltaSeconds))
                    {
                        SetError(outError,
                                 "Invalid --fixed-delta value: " +
                                     std::string(value));
                        return false;
                    }
                    options.fixedDeltaExplicit = true;
                }
                else if (argument == "--capture-tick")
                {
                    if (!ParsePositiveRunnerUInt64(value,
                                                   options.captureTick) ||
                        options.captureTick > 1000000u)
                    {
                        SetError(outError,
                                 "Invalid --capture-tick value: " +
                                     std::string(value));
                        return false;
                    }
                    options.captureTickExplicit = true;
                }
                else if (argument == "--qualification-bundle")
                {
                    options.qualificationBundlePath = value;
                    if (options.qualificationBundlePath.empty())
                    {
                        SetError(outError,
                                 "--qualification-bundle must not be empty");
                        return false;
                    }
                }
                else if (argument == "--qualification-series")
                {
                    options.qualificationSeriesPath = value;
                    if (options.qualificationSeriesPath.empty())
                    {
                        SetError(outError,
                                 "--qualification-series must not be empty");
                        return false;
                    }
                }
                else if (argument == "--qualification-performance")
                {
                    options.qualificationPerformancePath = value;
                    if (options.qualificationPerformancePath.empty())
                    {
                        SetError(outError,
                                 "--qualification-performance must not be empty");
                        return false;
                    }
                }
                else if (argument == "--qualification-performance-baseline")
                {
                    options.qualificationPerformanceBaselineIdentity = value;
                    if (options.qualificationPerformanceBaselineIdentity.empty())
                    {
                        SetError(outError,
                                 "--qualification-performance-baseline must not be empty");
                        return false;
                    }
                }
                else if (argument == "--render-path")
                {
                    if (!ParseRunnerRenderPath(value, options.renderPath))
                    {
                        SetError(outError,
                                 "Invalid --render-path value: " +
                                     std::string(value));
                        return false;
                    }
                    options.renderPathExplicit = true;
                }
                else if (argument == "--workload-scale")
                {
                    if (!ParseSampleWorkloadScale(value,
                                                  options.workloadScale))
                    {
                        SetError(outError,
                                 "Invalid --workload-scale value: " +
                                     std::string(value));
                        return false;
                    }
                    options.workloadScaleExplicit = true;
                }
                else if (argument == "--instancing")
                {
                    if (!ParseRunnerInstancingMode(value,
                                                   options.instancingMode))
                    {
                        SetError(outError,
                                 "Invalid --instancing value: " +
                                     std::string(value));
                        return false;
                    }
                }
                else if (argument == "--catalog")
                {
                    options.catalogPath = value;
                }
                else if (argument == "--cooked-shader-root")
                {
                    options.cookedShaderRoot = value;
                }
                else if (argument == "--ready-timeout-ms")
                {
                    if (!ParsePositiveRunnerUInt(value,
                                                 options.readyTimeoutMs) ||
                        options.readyTimeoutMs > 3600000u)
                    {
                        SetError(outError,
                                 "Invalid --ready-timeout-ms value: " +
                                     std::string(value));
                        return false;
                    }
                    options.readyTimeoutExplicit = true;
                }
                else if (argument == "--ready-max-frames")
                {
                    if (!ParsePositiveRunnerUInt(value,
                                                 options.readyMaxFrames) ||
                        options.readyMaxFrames > 10000u)
                    {
                        SetError(outError,
                                 "Invalid --ready-max-frames value: " +
                                     std::string(value));
                        return false;
                    }
                    options.readyMaxFramesExplicit = true;
                }
                else if (argument == "--startup-report")
                {
                    options.startupReportPath = value;
                }
                else if (argument == "--lifetime-report")
                {
                    options.lifetimeReportPath = value;
                }
                else if (argument == "--assessment-report")
                {
                    options.assessmentReportPath = value;
                }
                else if (argument == "--assessment-profile")
                {
                    if (!ParseAssessmentProfile(value,
                                                options.assessmentProfile))
                    {
                        SetError(outError,
                                 "Invalid --assessment-profile value: " +
                                     std::string(value));
                        return false;
                    }
                }
                else if (argument == "--assessment-baseline")
                {
                    options.assessmentBaselinePath = value;
                }
                else if (argument == "--lifetime-warmup-frames")
                {
                    if (!ParsePositiveRunnerUInt(
                            value,
                            options.lifetimeConfig.warmupFrames) ||
                        options.lifetimeConfig.warmupFrames > 1000000u)
                    {
                        SetError(outError,
                                 "Invalid --lifetime-warmup-frames value: " +
                                     std::string(value));
                        return false;
                    }
                }
                else if (argument == "--lifetime-observation-frames")
                {
                    if (!ParsePositiveRunnerUInt(
                            value,
                            options.lifetimeConfig.observationFrames) ||
                        options.lifetimeConfig.observationFrames > 1000000u)
                    {
                        SetError(outError,
                                 "Invalid --lifetime-observation-frames value: " +
                                     std::string(value));
                        return false;
                    }
                }
                else if (argument == "--lifetime-min-duration-ms")
                {
                    if (!ParsePositiveRunnerUInt(
                            value,
                            options.lifetimeConfig.minimumDurationMs) ||
                        options.lifetimeConfig.minimumDurationMs > 86400000u)
                    {
                        SetError(outError,
                                 "Invalid --lifetime-min-duration-ms value: " +
                                     std::string(value));
                        return false;
                    }
                }
                else if (argument == "--lifetime-resize-interval")
                {
                    if (!ParsePositiveRunnerUInt(
                            value,
                            options.lifetimeConfig.resizeIntervalFrames) ||
                        options.lifetimeConfig.resizeIntervalFrames > 1000000u)
                    {
                        SetError(outError,
                                 "Invalid --lifetime-resize-interval value: " +
                                     std::string(value));
                        return false;
                    }
                }
                else if (argument == "--lifetime-resize-settle-frames")
                {
                    if (!ParsePositiveRunnerUInt(
                            value,
                            options.lifetimeConfig.resizeSettleFrames) ||
                        options.lifetimeConfig.resizeSettleFrames > 10000u)
                    {
                        SetError(outError,
                                 "Invalid --lifetime-resize-settle-frames value: " +
                                     std::string(value));
                        return false;
                    }
                }
                else
                {
                    options.assetRoot = value;
                }
                continue;
            }
            commonArguments.push_back(argv[i]);
        }

        if (!ParseSampleCLI(static_cast<int>(commonArguments.size()),
                            commonArguments.data(),
                            options.common,
                            outError))
        {
            return false;
        }
        if (!ParseSampleQualityTier(options.common.quality,
                                    options.qualityTier))
        {
            SetError(outError,
                     "Invalid --quality value: " + options.common.quality);
            return false;
        }
        if (options.common.showHelp || options.listSamples)
        {
            return true;
        }
        if (options.sampleId.empty())
        {
            SetError(outError, "--sample must not be empty");
            return false;
        }
        if (!options.assetId.empty() && !options.modelPath.empty())
        {
            SetError(outError, "--asset and --model are mutually exclusive");
            return false;
        }
        if (!options.environmentId.empty() && !options.environmentPath.empty())
        {
            SetError(outError,
                     "--environment and --environment-file are mutually exclusive");
            return false;
        }
        if (options.HasLifetimeQualification())
        {
            options.waitReady = true;
            if (options.common.frames == 0)
            {
                options.common.frames = 1;
            }
            if (options.sampleId != "model-viewer")
            {
                SetError(outError,
                         "Lifetime qualification currently requires --sample model-viewer");
                return false;
            }
            if (!options.deterministicCameraOrbit)
            {
                SetError(outError,
                         "Lifetime qualification requires --deterministic-orbit");
                return false;
            }
        }
        if (!options.assessmentBaselinePath.empty() &&
            !options.HasFrameworkAssessment())
        {
            SetError(outError,
                     "--assessment-baseline requires --assessment-report");
            return false;
        }
        if (options.assessmentProfile != SampleAssessmentProfile::Smoke &&
            !options.HasFrameworkAssessment())
        {
            SetError(outError,
                     "--assessment-profile qualification or benchmark requires --assessment-report");
            return false;
        }
        if (options.HasFrameworkAssessment())
        {
            options.waitReady = true;
            if (options.common.frames == 0)
            {
                options.common.frames =
                    options.assessmentProfile ==
                            SampleAssessmentProfile::Smoke
                        ? 120u
                : 600u;
            }
        }
        if (!options.qualificationBundlePath.empty())
        {
            if (options.sampleId != "render-quality-lab")
            {
                SetError(outError,
                         "--qualification-bundle requires --sample render-quality-lab");
                return false;
            }
            if (options.qualificationBundlePath.filename() != "bundle.json")
            {
                SetError(outError,
                         "--qualification-bundle must name bundle.json inside its own artifact directory");
                return false;
            }
        }
        if (!options.qualificationSeriesPath.empty())
        {
            if (options.qualificationSeriesPath.filename() != "series.json")
            {
                SetError(outError,
                         "--qualification-series must name series.json inside its artifact directory");
                return false;
            }
            options.waitReady = true;
        }
        if (!options.qualificationBundlePath.empty() &&
            !options.qualificationSeriesPath.empty())
        {
            SetError(outError,
                     "--qualification-bundle and --qualification-series are mutually exclusive");
            return false;
        }
        if (!options.qualificationPerformanceBaselineIdentity.empty() &&
            options.qualificationPerformancePath.empty())
        {
            SetError(outError,
                     "--qualification-performance-baseline requires --qualification-performance");
            return false;
        }
        if (!options.qualificationPerformancePath.empty())
        {
            if (options.qualificationPerformanceBaselineIdentity.empty())
            {
                options.qualificationPerformanceBaselineIdentity =
                    ReadEnvironmentString(
                        "RVX_RENDER_QUALITY_PERFORMANCE_BASELINE_ID");
            }
            if (options.qualificationPerformanceOsBuild.empty())
            {
                options.qualificationPerformanceOsBuild =
                    ResolveHostOperatingSystemBuild();
            }
            if (options.qualificationPerformanceBaselineIdentity.empty())
            {
                SetError(outError,
                         "--qualification-performance requires an explicit --qualification-performance-baseline or RVX_RENDER_QUALITY_PERFORMANCE_BASELINE_ID");
                return false;
            }
            if (options.qualificationPerformanceOsBuild.empty())
            {
                SetError(outError,
                         "--qualification-performance requires a trustworthy host OS build identity");
                return false;
            }
            if (options.common.frames == 0)
            {
                // The fixed Render-owned session, not this generic counter,
                // decides how many frame submissions are accepted.
                options.common.frames = 1;
            }
            options.waitReady = true;
            constexpr uint32 MinimumPerformanceHostFrameBudget = 512U;
            if (options.readyMaxFrames < MinimumPerformanceHostFrameBudget)
            {
                if (options.readyMaxFramesExplicit)
                {
                    SetError(outError,
                             "--qualification-performance requires --ready-max-frames of at least 512");
                    return false;
                }
                options.readyMaxFrames = MinimumPerformanceHostFrameBudget;
            }
            if (options.common.width != 2560U || options.common.height != 1440U)
            {
                SetError(outError,
                         "--qualification-performance requires the fixed 2560x1440 raster qualification extent");
                return false;
            }
        }
        if (options.common.gpuSceneCullingQualificationEnabled &&
            options.common.directOpaqueRasterReadbackQualificationEnabled)
        {
            SetError(outError,
                     "GPUScene and Direct Opaque readback qualifications are mutually exclusive");
            return false;
        }
        if (!options.qualificationPerformancePath.empty() &&
            (!options.common.screenshotPath.empty() ||
             options.common.pixelProbeEnabled ||
             !options.qualificationBundlePath.empty() ||
             !options.qualificationSeriesPath.empty()))
        {
            SetError(outError,
                     "--qualification-performance is mutually exclusive with screenshot, pixel-probe, qualification-bundle, and qualification-series producers");
            return false;
        }
        if (!options.runtimeOnlyComposition && !options.cookedShaderRoot.empty())
        {
            SetError(outError,
                     "--cooked-shader-root requires --runtime-only-composition");
            return false;
        }
        if (options.runtimeOnlyComposition && options.cookedShaderRoot.empty())
        {
            SetError(outError,
                     "--runtime-only-composition requires --cooked-shader-root");
            return false;
        }
        const uint32 executionReceiptProbeCount =
            (options.common.exposureQualificationProbeEnabled ? 1u : 0u) +
            (options.common.ambientOcclusionQualificationProbeEnabled ? 1u : 0u) +
            (options.common.hiZReflectionQualificationProbeEnabled ? 1u : 0u) +
            (options.common.toonOutlineQualificationProbeEnabled ? 1u : 0u);
        if (executionReceiptProbeCount > 1u)
        {
            // Each probe owns the capture frame's immutable execution
            // receipt. Do not accept a configuration that would let one
            // completion replace the other before both can be reported.
            SetError(outError,
                     "Exposure, ambient-occlusion, Hi-Z reflection, and Toon-outline qualification probes are mutually exclusive");
            return false;
        }
        if (options.common.gpuSceneCullingQualificationEnabled ||
            options.common.directOpaqueRasterReadbackQualificationEnabled ||
            options.common.exposureQualificationProbeEnabled ||
            options.common.ambientOcclusionQualificationProbeEnabled ||
            options.common.hiZReflectionQualificationProbeEnabled ||
            options.common.toonOutlineQualificationProbeEnabled)
        {
            // One-shot readbacks are consumed by the target frame and observed
            // only after a later Render-owner completion poll. Reuse the
            // bounded readiness wait instead of coupling this to pixel capture.
            options.waitReady = true;
            if (options.common.frames == 0)
            {
                options.common.frames = 1;
            }
        }
        if (!options.common.screenshotPath.empty() && options.common.frames == 0)
        {
            options.common.frames = 8;
        }
        if (options.captureTickExplicit &&
            options.common.screenshotPath.empty() &&
            options.qualificationPerformancePath.empty() &&
            options.qualificationSeriesPath.empty())
        {
            SetError(outError,
                     "--capture-tick requires --screenshot or --qualification-performance");
            return false;
        }
        if (options.captureTickExplicit)
        {
            options.common.frames = std::max(
                options.common.frames,
                static_cast<uint32>(options.captureTick));
        }
        if (options.readyMaxFrames == 0)
        {
            options.readyMaxFrames = std::max(options.common.frames, 120u);
        }
        if (options.waitReady && options.common.frames == 0)
        {
            SetError(outError,
                     "--wait-ready requires a finite --frames value");
            return false;
        }
        if (options.waitReady &&
            options.readyMaxFrames < options.common.frames)
        {
            SetError(outError,
                     "--ready-max-frames must be greater than or equal to --frames");
            return false;
        }
        if (options.common.smoke && !options.listSamples &&
            options.common.backend == RHIBackendType::Auto)
        {
            SetError(outError,
                     "--smoke requires an explicit --backend for reproducible validation");
            return false;
        }
        return true;
    }

    void PrintSampleRunnerUsage(const SampleRegistry& registry,
                                const char* executableName)
    {
        PrintSampleCLIUsage(std::cout,
                            executableName ? executableName : "RenderVerseSamples");
        std::cout
            << "  --list\n"
            << "  --sample <id>\n"
            << "  --asset <id>\n"
            << "  --model <path>\n"
            << "  --environment <id>\n"
            << "  --environment-file <path.hdr|path.exr>\n"
            << "  --catalog <catalog.json>\n"
            << "  --asset-root <directory>\n"
            << "  --runtime-only-composition\n"
            << "  --cooked-shader-root <directory>\n"
            << "  --pipeline <forward-plus|deferred|hybrid-rt>\n"
            << "  --view <bookmark-id>\n"
            << "  --fixed-delta <seconds>\n"
            << "  --capture-tick <positive-tick>\n"
            << "  --render-path <auto|direct|gpu-driven>\n"
            << "  --workload-scale <pr|nightly|qualification>\n"
            << "  --instancing <disabled|auto>\n"
            << "  --wait-ready\n"
            << "  --ready-timeout-ms <milliseconds>\n"
            << "  --ready-max-frames <count>\n"
            << "  --deterministic-orbit\n"
            << "  --startup-report <path.json>\n"
            << "  --lifetime-report <path.json>\n"
            << "  --assessment-report <path.json>\n"
            << "  --assessment-profile <smoke|qualification|benchmark>\n"
            << "  --assessment-baseline <path.json>\n"
            << "  --qualification-bundle <path.json>\n"
            << "  --qualification-series <path/series.json>\n"
            << "  --qualification-performance <path/performance.json>\n"
            << "  --qualification-performance-baseline <opaque-id>\n"
            << "  --lifetime-warmup-frames <count>\n"
            << "  --lifetime-observation-frames <count>\n"
            << "  --lifetime-min-duration-ms <milliseconds>\n"
            << "  --lifetime-resize-interval <frames>\n"
            << "  --lifetime-resize-settle-frames <frames>\n"
            << "\nAvailable samples:\n";
        for (const SampleInfo& info : registry.List())
        {
            std::cout << "  " << info.id << " - " << info.description << "\n";
        }
    }

    SampleRunner::SampleRunner(const SampleRegistry& registry) noexcept
        : m_registry(registry)
    {
    }

    SampleRunner::SampleRunner(
        const SampleRegistry& registry,
        const ISampleHostComposition& composition) noexcept
        : m_registry(registry)
        , m_composition(&composition)
    {
    }

    uint64 SampleRunner::ResolveFrameWaitTargetSequence(
        uint64 publishedBeforeOuterTick,
        uint64 publishedAfterOuterTick) noexcept
    {
        if (publishedAfterOuterTick > publishedBeforeOuterTick)
        {
            return publishedAfterOuterTick;
        }
        return publishedBeforeOuterTick ==
                       std::numeric_limits<uint64>::max()
                   ? publishedBeforeOuterTick
                   : publishedBeforeOuterTick + 1u;
    }

    bool SampleRunner::CanBeginFinalRenderDrain(
        uint32 executedFrames,
        uint32 minimumFrames,
        uint32 maximumFrames) noexcept
    {
        return (minimumFrames == 0 || executedFrames >= minimumFrames) &&
               executedFrames < maximumFrames;
    }

    bool SampleRunner::ShouldArmRenderQualityPerformanceSession(
        const bool sampleReady,
        const bool deterministicEpochActive,
        const uint64 deterministicTick,
        const uint64 sessionStartTick) noexcept
    {
        return sampleReady && deterministicEpochActive &&
               sessionStartTick != 0 && deterministicTick == sessionStartTick;
    }

    bool SampleRunner::HasMissedRenderQualityPerformanceSessionStart(
        const bool sampleReady,
        const bool deterministicEpochActive,
        const uint64 deterministicTick,
        const uint64 sessionStartTick) noexcept
    {
        if (!deterministicEpochActive || sessionStartTick == 0)
        {
            return false;
        }
        return deterministicTick > sessionStartTick ||
               (deterministicTick == sessionStartTick && !sampleReady);
    }

    bool SampleRunner::ShouldDeferGenericFiniteCompletion(
        const bool renderQualityPerformanceActive) noexcept
    {
        // The exact performance session owns completion independently of the
        // generic Sample `--frames` count used to reach sessionStartTick.
        return renderQualityPerformanceActive;
    }

    void SampleRunner::ConfigureFrameWaitPolicy(
        bool deterministicEpochActive,
        float32 deltaTime,
        RuntimeFrameWaitRequest& request) noexcept
    {
        request.deltaTime = deltaTime;
        request.advanceEngine = !deterministicEpochActive;
        request.pumpRenderProgressOnly = deterministicEpochActive;
    }

    bool SampleRunner::ResolveManifestContract(
        const SampleScenarioManifest& manifest,
        SampleRunnerCLIOptions& options,
        std::string& outError)
    {
        if (manifest.id != options.sampleId ||
            manifest.visualContractVersion == 0 ||
            manifest.fixedTime.numerator == 0 ||
            manifest.fixedTime.denominator == 0)
        {
            outError =
                "The selected Sample manifest contract is incomplete or belongs to another scenario";
            return false;
        }

        if (!options.qualificationBundlePath.empty() &&
            options.sampleId != "render-quality-lab")
        {
            SetError(&outError,
                     "--qualification-bundle requires --sample render-quality-lab");
            return false;
        }
        if (!options.qualificationBundlePath.empty() &&
            options.qualificationBundlePath.filename() != "bundle.json")
        {
            SetError(&outError,
                     "--qualification-bundle must name bundle.json inside its own artifact directory");
            return false;
        }
        if (!options.qualificationSeriesPath.empty())
        {
            if (!manifest.deterministicQualification)
            {
                SetError(&outError,
                         "--qualification-series requires deterministicQualification=true in the selected Sample manifest");
                return false;
            }
            if (options.qualificationSeriesPath.filename() != "series.json")
            {
                SetError(&outError,
                         "--qualification-series must name series.json inside its own artifact directory");
                return false;
            }
            if (options.captureTickExplicit)
            {
                SetError(&outError,
                         "--qualification-series captures every declared manifest tick and forbids --capture-tick");
                return false;
            }
        }
        if (!options.qualificationPerformancePath.empty() &&
            options.sampleId != "render-quality-lab")
        {
            SetError(&outError,
                     "--qualification-performance requires --sample render-quality-lab");
            return false;
        }
        if (!options.qualificationPerformancePath.empty() &&
            options.qualificationPerformancePath.filename() !=
                "performance.json")
        {
            SetError(&outError,
                     "--qualification-performance must name performance.json inside an existing artifact directory");
            return false;
        }

        if (!options.pipelineExplicit)
        {
            options.pipelineRecipe = manifest.defaultRecipe;
        }
        if (!options.qualityExplicit ||
            options.qualityTier == SampleQualityTier::Default)
        {
            options.qualityTier = manifest.defaultQualityProfile;
            options.common.quality =
                GetSampleQualityTierName(options.qualityTier);
        }
        if (options.qualityTier == SampleQualityTier::Default)
        {
            outError =
                "The selected Sample manifest did not resolve a concrete quality profile";
            return false;
        }
        if (!options.qualificationPerformancePath.empty() &&
            (options.pipelineRecipe != SamplePipelineRecipe::DeferredQuality ||
             options.qualityTier != SampleQualityTier::High))
        {
            SetError(&outError,
                     "--qualification-performance requires the RenderQualityLab deferred/high profile");
            return false;
        }

        options.fixedTimeNumerator = manifest.fixedTime.numerator;
        options.fixedTimeDenominator = manifest.fixedTime.denominator;
        options.deterministicSeed = manifest.fixedTime.deterministicSeed;
        const float64 manifestDelta =
            static_cast<float64>(manifest.fixedTime.numerator) /
            static_cast<float64>(manifest.fixedTime.denominator);
        if (options.fixedDeltaExplicit)
        {
            const float64 tolerance =
                std::numeric_limits<float64>::epsilon() *
                std::max<float64>(1.0, std::abs(manifestDelta)) * 8.0;
            if (std::abs(options.fixedDeltaSeconds - manifestDelta) >
                tolerance)
            {
                outError =
                    "--fixed-delta does not match the exact fixedTime declared by the Sample manifest";
                return false;
            }
        }
        options.fixedDeltaSeconds = manifestDelta;

        const bool captureEvidenceRequested =
            !options.common.screenshotPath.empty() ||
            options.common.pixelProbeEnabled ||
            !options.qualificationBundlePath.empty() ||
            !options.qualificationSeriesPath.empty() ||
            !options.qualificationPerformancePath.empty();
        options.deterministicContractActive =
            manifest.deterministicQualification || captureEvidenceRequested ||
            options.fixedDeltaExplicit || options.viewBookmarkExplicit ||
            options.captureTickExplicit;

        const auto findBookmark = [&manifest](std::string_view id)
        {
            return std::find_if(
                manifest.cameraBookmarks.begin(),
                manifest.cameraBookmarks.end(),
                [id](const SampleCameraBookmark& bookmark)
                {
                    return bookmark.id == id;
                });
        };
        if (options.viewBookmarkExplicit)
        {
            if (findBookmark(options.viewBookmarkId) ==
                manifest.cameraBookmarks.end())
            {
                outError = "Unknown --view bookmark '" +
                           options.viewBookmarkId + "' for Sample '" +
                           manifest.id + "'";
                return false;
            }
            options.viewBookmarkResolved = true;
        }
        else if (options.deterministicContractActive)
        {
            if (manifest.cameraBookmarks.empty())
            {
                outError =
                    "The deterministic Sample contract has no camera bookmark";
                return false;
            }
            options.viewBookmarkId = manifest.cameraBookmarks.front().id;
            options.viewBookmarkResolved = true;
        }

        if (options.captureTickExplicit)
        {
            if (std::find(manifest.captureTicks.begin(),
                          manifest.captureTicks.end(),
                          static_cast<uint32>(options.captureTick)) ==
                manifest.captureTicks.end())
            {
                outError = "--capture-tick is not declared by the selected Sample manifest";
                return false;
            }
            options.captureTickResolved = true;
        }
        else if (captureEvidenceRequested)
        {
            if (manifest.captureTicks.empty())
            {
                outError =
                    "The Sample manifest has no capture tick for the requested evidence";
                return false;
            }
            options.captureTick = !options.qualificationSeriesPath.empty()
                ? manifest.captureTicks.back()
                : manifest.captureTicks.front();
            options.captureTickResolved = true;
        }

        if (options.captureTickResolved)
        {
            if (options.captureTick == 0 ||
                options.captureTick > std::numeric_limits<uint32>::max())
            {
                outError = "The resolved Sample capture tick is out of range";
                return false;
            }
            options.common.frames = static_cast<uint32>(options.captureTick);
            if (!options.readyMaxFramesExplicit)
            {
                options.readyMaxFrames = std::max<uint32>(
                    options.readyMaxFrames,
                    static_cast<uint32>(options.captureTick) + 120u);
            }
        }
        options.manifestContractResolved = true;
        return true;
    }

    bool SampleRunner::ValidateManifestAssessmentContract(
        const SampleScenarioManifest& manifest,
        const SampleAssessmentContract& contract,
        std::string& outError)
    {
        const auto findCapability = [&contract](const AssessmentCode& code)
        {
            return std::find_if(
                contract.capabilities.begin(),
                contract.capabilities.end(),
                [&code](const AssessmentCapability& capability)
                {
                    return capability.code == code;
                });
        };
        for (const AssessmentCode& code : manifest.requiredCapabilities)
        {
            const auto capability = findCapability(code);
            if (capability == contract.capabilities.end() ||
                !capability->gating)
            {
                outError = "Manifest required capability '" +
                           code.GetValue() +
                           "' is not a gating capability in the Sample assessment contract";
                return false;
            }
        }
        for (const AssessmentCode& code : manifest.optionalCapabilities)
        {
            const auto capability = findCapability(code);
            if (capability == contract.capabilities.end() ||
                capability->gating)
            {
                outError = "Manifest optional capability '" +
                           code.GetValue() +
                           "' is not a non-gating capability in the Sample assessment contract";
                return false;
            }
        }
        for (const AssessmentCapability& capability : contract.capabilities)
        {
            if (!capability.gating)
            {
                continue;
            }

            const bool declaredRequired = std::find(
                manifest.requiredCapabilities.begin(),
                manifest.requiredCapabilities.end(),
                capability.code) != manifest.requiredCapabilities.end();
            if (!declaredRequired)
            {
                outError = "Gating assessment capability '" +
                           capability.code.GetValue() +
                           "' is absent from the active Sample manifest profile";
                return false;
            }
        }
        if (!manifest.allowedFallbacks.empty())
        {
            outError =
                "Sample fallback declarations require the GFX02 feature resolver and cannot be advertised yet";
            return false;
        }
        return true;
    }

    bool SampleRunner::ApplyHiZReflectionQualificationProfile(
        const std::string_view sampleId,
        const bool probeEnabled,
        SampleScenarioManifest& manifest,
        SampleAssessmentContract& contract,
        std::string& outError)
    {
        if (!probeEnabled)
        {
            return true;
        }
        if (sampleId != "post-processing")
        {
            outError =
                "--hiz-reflection-qualification-probe requires the post-processing sample";
            return false;
        }

        const AssessmentCode capabilityCode(
            "RENDER.CAPABILITY.HIZ_SCREEN_SPACE_REFLECTION");
        const AssessmentCode gtaoCapabilityCode("RENDER.CAPABILITY.GTAO");
        const auto hasCode = [&capabilityCode](const auto& values)
        {
            return std::find(values.begin(), values.end(), capabilityCode) !=
                values.end();
        };
        const auto hasCapability = [&capabilityCode](
                                       const std::vector<AssessmentCapability>& values)
        {
            return std::find_if(
                       values.begin(), values.end(),
                       [&capabilityCode](const AssessmentCapability& value)
                       {
                           return value.code == capabilityCode;
                       }) != values.end();
        };
        if (hasCode(manifest.requiredCapabilities) ||
            hasCode(manifest.optionalCapabilities) ||
            hasCapability(contract.capabilities))
        {
            outError =
                "The GFX19 Hi-Z reflection qualification profile was activated more than once";
            return false;
        }

        // GFX19 owns the immutable capture-frame receipt for its Hi-Z probe.
        // A GTAO probe owns that same slot, so the active profile replaces
        // GTAO gating instead of asking one frame to prove both readbacks.
        std::erase(manifest.requiredCapabilities, gtaoCapabilityCode);
        std::erase(manifest.optionalCapabilities, gtaoCapabilityCode);
        std::erase_if(contract.capabilities,
                      [&gtaoCapabilityCode](const AssessmentCapability& value)
                      {
                          return value.code == gtaoCapabilityCode;
                      });
        manifest.requiredCapabilities.push_back(capabilityCode);
        contract.capabilities.push_back(
            {capabilityCode,
             "Hi-Z screen-space reflection is requested through the public post-process policy.",
             true,
             "Qualification requires exact presented route-mask and fallback-radiance evidence, not graph scheduling."});
        contract.revision += "-gfx19-hiz";
        return true;
    }

    uint64 SampleRunner::ResolveSceneRemovalPublicationSequence(
        uint64 removalSceneRevision,
        const EngineRenderRuntimeDiagnostics& diagnostics) noexcept
    {
        if (removalSceneRevision == 0 || !diagnostics.available ||
            diagnostics.requiredSceneFrameSequence == 0 ||
            diagnostics.requiredSceneRevision < removalSceneRevision)
        {
            return 0;
        }
        return diagnostics.requiredSceneFrameSequence;
    }

    bool SampleRunner::HasCompletedSceneRemovalPublication(
        bool engineInitialized,
        bool deadlineExpired,
        uint64 removalSceneRevision,
        uint64 removalPublicationSequence,
        const RenderDiagnosticsSnapshot& diagnostics) noexcept
    {
        return engineInitialized && !deadlineExpired &&
               removalSceneRevision != 0 &&
               removalPublicationSequence != 0 &&
               diagnostics.lifecycle == RenderLifecycleState::Running &&
               !(diagnostics.lastFailure.available &&
                 diagnostics.lastFailure.runtime.resultClass >=
                     RenderResultClass::FrameFatal) &&
               diagnostics.lastPublishedFrameSequence >=
                   removalPublicationSequence &&
               diagnostics.lastSubmittedFrameSequence >=
                   removalPublicationSequence &&
               diagnostics.lastPresentedFrameSequence >=
                   removalPublicationSequence &&
               diagnostics.sceneValues.available &&
               diagnostics.sceneValues.frameSequence >=
                   removalPublicationSequence &&
               diagnostics.sceneValues.requiredSceneRevision >=
                   removalSceneRevision &&
               diagnostics.sceneValues.appliedSceneRevision >=
                   removalSceneRevision;
    }

    int SampleRunner::Run(int argc, char* argv[]) const
    {
        SampleRunnerCLIOptions options;
        std::string error;
        std::vector<const char*> arguments;
        arguments.reserve(static_cast<size_t>(std::max(argc, 0)));
        for (int i = 0; i < argc; ++i)
        {
            arguments.push_back(argv[i]);
        }
        if (!ParseSampleRunnerCLI(argc,
                                  arguments.data(),
                                  options,
                                  &error))
        {
            std::cerr << error << "\n";
            PrintSampleRunnerUsage(m_registry, argc > 0 ? argv[0] : nullptr);
            return 2;
        }
        if (options.common.showHelp)
        {
            PrintSampleRunnerUsage(m_registry, argc > 0 ? argv[0] : nullptr);
            return 0;
        }
        if (options.listSamples)
        {
            for (const SampleInfo& info : m_registry.List())
            {
                std::cout << info.id << "\t" << info.description << "\n";
            }
            return 0;
        }
        if (m_composition == nullptr)
        {
            std::cerr << "Sample host composition was not injected\n";
            return 1;
        }
        if (m_composition->RequiresCookedOnly() &&
            !options.runtimeOnlyComposition)
        {
            std::cerr << "Cooked Sample host requires --runtime-only-composition\n";
            return 2;
        }

        const SampleInfo* registeredInfo = m_registry.Find(options.sampleId);
        const SampleScenarioManifest* manifest =
            m_registry.FindManifest(options.sampleId);
        std::unique_ptr<ISample> sample = m_registry.Create(options.sampleId);
        if (!registeredInfo || !manifest || !sample)
        {
            std::cerr << "Unknown or uncontracted sample: "
                      << options.sampleId << "\n";
            PrintSampleRunnerUsage(m_registry, argc > 0 ? argv[0] : nullptr);
            static_cast<void>(WriteEarlyFailureAssessment(
                options.assessmentReportPath,
                sample.get(),
                options.sampleId,
                options.common.backend,
                "SAMPLE.REGISTRY.NOT_FOUND",
                "SAMPLE",
                AssessmentCheckpoints::ScenarioSetup,
                "The requested Product Sample is not registered with a manifest contract.",
                "sampleId='" + options.sampleId + "'"));
            return 2;
        }
        if (!ResolveManifestContract(*manifest, options, error))
        {
            std::cerr << error << "\n";
            static_cast<void>(WriteEarlyFailureAssessment(
                options.assessmentReportPath,
                sample.get(),
                options.sampleId,
                options.common.backend,
                "SAMPLE.MANIFEST.RESOLUTION_FAILED",
                "SAMPLE",
                AssessmentCheckpoints::ScenarioSetup,
                "The Product Sample manifest could not resolve the requested execution contract.",
                error));
            return 2;
        }
        const SampleCameraBookmark* selectedBookmark = nullptr;
        if (options.viewBookmarkResolved)
        {
            const auto bookmark = std::find_if(
                manifest->cameraBookmarks.begin(),
                manifest->cameraBookmarks.end(),
                [&options](const SampleCameraBookmark& candidate)
                {
                    return candidate.id == options.viewBookmarkId;
                });
            if (bookmark == manifest->cameraBookmarks.end())
            {
                std::cerr << "Resolved Sample camera bookmark disappeared\n";
                return 2;
            }
            selectedBookmark = &*bookmark;
        }
        const float32 sampleDeltaTime =
            static_cast<float32>(options.fixedDeltaSeconds);
        if (!std::isfinite(sampleDeltaTime) || sampleDeltaTime <= 0.0f)
        {
            std::cerr << "The resolved fixed delta is not representable as float32\n";
            return 2;
        }

        StartupTimelineReporter startupTimeline(options.startupReportPath);
        const Diagnostics::TraceContext startupTraceContext =
            startupTimeline.GetContext();

        if (sample->GetInfo().id != registeredInfo->id)
        {
            std::cerr << "Sample registry metadata mismatch: "
                      << registeredInfo->id << "\n";
            static_cast<void>(WriteEarlyFailureAssessment(
                options.assessmentReportPath,
                sample.get(),
                options.sampleId,
                options.common.backend,
                "SAMPLE.REGISTRY.METADATA_MISMATCH",
                "SAMPLE",
                AssessmentCheckpoints::ScenarioSetup,
                "The Sample factory and registry metadata disagree.",
                "registry='" + registeredInfo->id + "', factory='" +
                    sample->GetInfo().id + "'"));
            return 1;
        }
        startupTimeline.SetMetadata("sample", registeredInfo->id);
        startupTimeline.SetMetadata(
            "requestedBackend",
            std::string(GetSampleBackendName(options.common.backend)));

        SampleSceneOptions sceneOptions;
        ResolvedSampleAssets resolvedAssets;
        Diagnostics::TraceSpan resolveAssetsSpan = Diagnostics::BeginTraceSpan(
            startupTraceContext,
            "AssetResolve");
        if (!ResolveSceneOptions(
                *registeredInfo,
                options,
                GetExecutableDirectory(argc > 0 ? argv[0] : nullptr),
                sceneOptions,
                resolvedAssets,
                error))
        {
            resolveAssetsSpan.SetAttribute("result", "failed");
            std::cerr << error << "\n";
            static_cast<void>(WriteEarlyFailureAssessment(
                options.assessmentReportPath,
                sample.get(),
                registeredInfo->id,
                options.common.backend,
                "SAMPLE.ASSET.RESOLUTION_FAILED",
                "RESOURCE",
                AssessmentCheckpoints::ScenarioSetup,
                "The Sample asset selection could not be resolved.",
                error));
            return 1;
        }
        resolveAssetsSpan.SetAttribute("result", "resolved");
        resolveAssetsSpan.End();

        const bool effectiveReadyWait = options.RequiresReadinessWait();
        const SampleReadinessWaitBudget readinessWaitBudget =
            ResolveSampleReadinessWaitBudget(
                options,
                effectiveReadyWait && sceneOptions.workloadProfile.has_value()
                    ? &*sceneOptions.workloadProfile
                    : nullptr);

        Log::Initialize();
        RVX_CORE_INFO("Starting sample '{}' with requested backend '{}'",
                      registeredInfo->id,
                      GetSampleBackendName(options.common.backend));
        RVX_CORE_INFO("Resolved model asset: {}", sceneOptions.modelPath.string());
        if (!sceneOptions.environmentPath.empty())
        {
            RVX_CORE_INFO("Resolved environment asset: {}",
                          sceneOptions.environmentPath.string());
        }

        SampleRuntimeCompositionEvidence runtimeComposition;
        runtimeComposition.runtimeOnlyComposition =
            options.runtimeOnlyComposition;
        std::filesystem::path cookedResourceRoot;
        if (options.runtimeOnlyComposition)
        {
            const ResolvedSampleAsset& primaryModel = resolvedAssets.model;
            if (!primaryModel.selected || !primaryModel.cookAdmission.has_value() ||
                !primaryModel.cookAdmission->IsAccepted() ||
                !primaryModel.entry.cook.declared ||
                !primaryModel.entry.cook.packageOnly ||
                primaryModel.entry.cook.cookedRoot.empty())
            {
                std::cerr << "Runtime-only Sample execution requires an admitted "
                             "package-only primary cooked model\n";
                static_cast<void>(WriteEarlyFailureAssessment(
                    options.assessmentReportPath,
                    sample.get(),
                    registeredInfo->id,
                    options.common.backend,
                    "RESOURCE.RUNTIME_ONLY_PRIMARY_MODEL_REJECTED",
                    "RESOURCE",
                    AssessmentCheckpoints::EngineBaseline,
                    "Runtime-only Sample composition requires the primary model's "
                    "accepted cooked package.",
                    "The selected primary model did not retain an accepted CookManifest "
                    "admission receipt."));
                static_cast<void>(startupTimeline.Finalize());
                Log::Shutdown();
                return 1;
            }

            std::error_code filesystemError;
            // Registry requests are package-relative (`cooked://...`) rather
            // than catalog absolute paths.  Mount the catalog asset root so
            // every admitted package entry resolves beneath one policy root;
            // the individual CookManifest remains the admission authority.
            cookedResourceRoot = primaryModel.assetRoot.lexically_normal();
            const std::filesystem::path cookedShaderRoot =
                options.cookedShaderRoot.lexically_normal();
            if (!std::filesystem::is_directory(cookedResourceRoot,
                                               filesystemError) ||
                filesystemError)
            {
                std::cerr << "Runtime-only Sample cooked resource root is unavailable: "
                          << cookedResourceRoot.string() << "\n";
                static_cast<void>(startupTimeline.Finalize());
                Log::Shutdown();
                return 1;
            }
            filesystemError.clear();
            if (!std::filesystem::is_directory(cookedShaderRoot,
                                               filesystemError) ||
                filesystemError)
            {
                std::cerr << "Runtime-only Sample cooked shader root is unavailable: "
                          << cookedShaderRoot.string() << "\n";
                static_cast<void>(startupTimeline.Finalize());
                Log::Shutdown();
                return 1;
            }

            runtimeComposition.cookedShaderProvider = true;
            runtimeComposition.sourceShaderFallbackEnabled = false;
            runtimeComposition.cookedResourcePolicy = true;
            runtimeComposition.cookedShaderRoot = cookedShaderRoot;
            runtimeComposition.cookedResourceRoot = cookedResourceRoot;
            runtimeComposition.shaderProgramProvider = "CookedShaderArtifactV2";
        }

        EngineRuntimeDependencies engineDependencies;
        if (!m_composition->CreateEngineRuntimeDependencies(
                runtimeComposition, engineDependencies, error))
        {
            RVX_CORE_ERROR("Sample host composition could not create Engine dependencies: {}",
                           error);
            static_cast<void>(startupTimeline.Finalize());
            Log::Shutdown();
            return 1;
        }
        Engine engine(std::move(engineDependencies));
        const std::string applicationName =
            "RenderVerseX - " + registeredInfo->displayName;
        EngineConfig engineConfig;
        engineConfig.appName = applicationName.c_str();
        engineConfig.windowWidth = options.common.width;
        engineConfig.windowHeight = options.common.height;
        engineConfig.enableJobSystem = false;
        engineConfig.startupTraceContext = startupTraceContext;
        engineConfig.renderRuntime.backendType = options.common.backend;
        engineConfig.renderRuntime.swapChain.vsync = options.common.frames == 0;
        engineConfig.renderRuntime.enableValidation =
            options.common.enableValidation;
        RenderPipelineRecipe requestedRecipe{};
        RenderQualityTier requestedQuality{};
        const bool pipelineRequestMapped =
            TryMapSamplePipelineRecipe(
                sceneOptions.pipelineRecipe, requestedRecipe) &&
            TryMapSampleQualityTier(
                sceneOptions.qualityTier, requestedQuality);
        RVX_ASSERT_MSG(
            pipelineRequestMapped,
            "Resolved Sample pipeline contract must map to Render contracts");
        engineConfig.initialRenderFrameSettings.pipelineRequest.recipe =
            requestedRecipe;
        engineConfig.initialRenderFrameSettings.pipelineRequest.quality =
            requestedQuality;
        const RenderExecutionBuildIdentity validationBuildIdentity =
            ReadValidationExecutionBuildIdentity();
        engineConfig.renderRuntime.executionBuildIdentity =
            validationBuildIdentity;
        engineConfig.effectDeterministicSeed = options.deterministicSeed;
        engineConfig.renderRuntime.startupTraceContext = startupTraceContext;
        auto* window = engine.AddSubsystem<WindowSubsystem>();
        WindowConfig windowConfig;
        windowConfig.title = applicationName.c_str();
        windowConfig.width = options.common.width;
        windowConfig.height = options.common.height;
        windowConfig.resizable = options.common.frames == 0 ||
                                 options.HasLifetimeQualification();
        windowConfig.graphicsApi =
            options.common.backend == RHIBackendType::OpenGL
                ? WindowGraphicsApi::OpenGL
                : WindowGraphicsApi::None;
        window->SetConfig(windowConfig);

        auto* resourceSubsystem =
            engine.AddSubsystem<Resource::ResourceSubsystem>();
        Resource::ResourceManagerConfig resourceConfig;
        if (!m_composition->CreateResourceManagerConfig(
                runtimeComposition, resourceConfig, error))
        {
            RVX_CORE_ERROR("Sample host composition could not configure ResourceManager: {}",
                           error);
            static_cast<void>(startupTimeline.Finalize());
            Log::Shutdown();
            return 1;
        }
        resourceConfig.asyncThreadCount =
            std::max(resourceConfig.asyncThreadCount, 1);
        resourceConfig.startupTraceContext = startupTraceContext;
        engineConfig.resourceManager = resourceConfig;
        engine.SetConfig(engineConfig);
        auto* input = engine.AddSubsystem<InputSubsystem>();
        auto* render = engine.AddSubsystem<RenderSubsystem>();

        if (!engine.Initialize() || !engine.IsInitialized())
        {
            RVX_CORE_ERROR("Failed to initialize sample engine host");
            static_cast<void>(WriteEarlyFailureAssessment(
                options.assessmentReportPath,
                sample.get(),
                registeredInfo->id,
                options.common.backend,
                "ENGINE.INITIALIZATION.FAILED",
                "ENGINE",
                AssessmentCheckpoints::EngineBaseline,
                "The Engine host failed before the baseline checkpoint.",
                "Engine::Initialize returned false."));
            static_cast<void>(startupTimeline.Finalize());
            Log::Shutdown();
            return 1;
        }
        if (!JobSystem::Get().IsInitialized() ||
            JobSystem::Get().GetWorkerCount() == 0)
        {
            RVX_CORE_ERROR(
                "Product Sample host requires at least one asynchronous resource worker");
            engine.Shutdown();
            static_cast<void>(WriteEarlyFailureAssessment(
                options.assessmentReportPath,
                sample.get(),
                registeredInfo->id,
                options.common.backend,
                "RESOURCE.ASYNC_WORKER.UNAVAILABLE",
                "RESOURCE",
                AssessmentCheckpoints::EngineBaseline,
                "The Product Sample host has no asynchronous Resource worker.",
                "Core JobSystem worker count is zero."));
            static_cast<void>(startupTimeline.Finalize());
            Log::Shutdown();
            return 1;
        }

        if (window->GetWindow())
        {
            input->SetWindow(window->GetWindow());
        }

        bool setupSucceeded = false;
        bool screenshotWritten = false;
        bool pixelProbeCaptured = false;
        bool sampleReady = false;
        bool readinessFailed = false;
        bool readinessTimedOut = false;
        bool sampleValidationFailed = false;
        bool deterministicEpochActive = false;
        bool deterministicCameraApplied = false;
        uint64 deterministicTick = 0;
        uint64 captureCarryingFrameSequence = 0;
        uint64 captureSceneRuntimeId = 0;
        uint64 captureCameraEntityPacked = 0;
        uint64 captureSceneRevision = 0;
        uint64 captureSurfaceGeneration = 0;
        uint64 captureConfigurationRevision = 0;
        std::optional<RenderExecutionReceipt> captureExecutionReceipt{};
        bool renderQualityBundleRequestAccepted = false;
        bool renderQualityBundleCompleted = false;
        bool renderQualityBundleWritten = false;
        uint64 renderQualityBundleSubmittedFrame = 0;
        uint64 renderQualityBundleCompletedFrame = 0;
        std::optional<RenderExecutionReceipt> renderQualityBundleReceipt{};
        std::shared_ptr<const QualificationCaptureBundle>
            renderQualityCaptureBundle{};
        const bool renderQualityAttachmentSeriesRequested =
            !options.qualificationSeriesPath.empty();
        bool renderQualityAttachmentSeriesWritten = false;
        size_t renderQualityAttachmentSeriesNextTick = 0;
        std::vector<RenderQualityAttachmentSeriesTick>
            renderQualityAttachmentSeriesTicks{};
        RenderQualityAttachmentSeriesHardwareIdentity
            renderQualityAttachmentSeriesHardware{};
        RenderQualityAttachmentSeriesConfigurationIdentity
            renderQualityAttachmentSeriesConfiguration{};
        std::string renderQualityAttachmentSeriesRunId{};
        std::string renderQualityAttachmentSeriesSourceCommit{};
        std::string renderQualityAttachmentSeriesPreset{};
        std::string renderQualityBundleRenderPath{};
        std::string renderQualityBundleInstancingMode{};
        bool renderQualityPerformanceRequestAccepted = false;
        uint32 renderQualityPerformanceHostFrames = 0;
        std::optional<std::chrono::steady_clock::time_point>
            renderQualityPerformanceDeadline{};
        FrozenRenderQualityPerformanceHostProvenance
            renderQualityPerformanceHostProvenance{};
        uint32 executedFrames = 0;
        const auto hasCompletedRequestedTicks = [&]() noexcept
        {
            if (options.captureTickResolved)
            {
                return deterministicEpochActive &&
                       deterministicTick >= options.captureTick;
            }
            return executedFrames >= options.common.frames;
        };
        bool finalRenderDrainRequested = false;
        bool finalRenderDrainPresentationSubmitted = false;
        bool finalRenderDrainCaptureQueued = false;
        SampleReadiness sampleReadiness =
            SampleReadiness::Pending("Sample setup has not completed");
        std::string readinessReason;
        RenderDiagnosticsSnapshot diagnostics = render->GetDiagnosticsSnapshot();
        SampleScenarioManifest assessmentManifest = *manifest;
        SampleAssessmentContract assessmentContract =
            sample->GetAssessmentContract();
        if (!ApplyHiZReflectionQualificationProfile(
                registeredInfo->id,
                options.common.hiZReflectionQualificationProbeEnabled,
                assessmentManifest,
                assessmentContract,
                error))
        {
            std::cerr << error << "\n";
            engine.Shutdown();
            static_cast<void>(WriteEarlyFailureAssessment(
                options.assessmentReportPath,
                sample.get(),
                registeredInfo->id,
                options.common.backend,
                "SAMPLE.HIZ_REFLECTION.PROFILE_INVALID",
                "SAMPLE",
                AssessmentCheckpoints::ScenarioSetup,
                "The GFX19 Hi-Z reflection qualification profile is invalid.",
                error));
            static_cast<void>(startupTimeline.Finalize());
            Log::Shutdown();
            return 1;
        }
        if (!ValidateManifestAssessmentContract(
                assessmentManifest, assessmentContract, error))
        {
            std::cerr << error << "\n";
            engine.Shutdown();
            static_cast<void>(WriteEarlyFailureAssessment(
                options.assessmentReportPath,
                sample.get(),
                registeredInfo->id,
                options.common.backend,
                "SAMPLE.MANIFEST.CAPABILITY_MISMATCH",
                "SAMPLE",
                AssessmentCheckpoints::ScenarioSetup,
                "The Sample manifest and assessment capability contracts disagree.",
                error));
            static_cast<void>(startupTimeline.Finalize());
            Log::Shutdown();
            return 1;
        }
        const bool graphicsTimestampSupported =
            diagnostics.gpuFrameTiming.timestampFrequency != 0;
        const bool requireGpuTiming =
            options.assessmentProfile != SampleAssessmentProfile::Smoke &&
            graphicsTimestampSupported;
        ExtendAssessmentContractWithTiming(
            assessmentContract,
            options.assessmentProfile,
            graphicsTimestampSupported);
        AssessmentMetadata assessmentMetadata;
        assessmentMetadata.fingerprint.sampleId = registeredInfo->id;
        assessmentMetadata.fingerprint.sampleRevision =
            assessmentContract.revision.empty() ? "invalid"
                                                : assessmentContract.revision;
        assessmentMetadata.fingerprint.engineBuild =
            std::string(__DATE__) + 'T' + __TIME__;
        assessmentMetadata.fingerprint.backend =
            GetSampleBackendName(diagnostics.backend);
        assessmentMetadata.fingerprint.device =
            diagnostics.adapterName.empty()
                ? "unavailable"
                : diagnostics.adapterName + '|' +
                      (diagnostics.driverVersion.empty()
                           ? std::string("driver-unavailable")
                           : diagnostics.driverVersion);
        assessmentMetadata.fingerprint.renderConfiguration =
            std::string(GetSamplePipelineRecipeName(
                sceneOptions.pipelineRecipe)) + "|" +
            GetSampleRenderPathName(sceneOptions.renderPath) +
            "|" + options.common.quality + "|" +
            std::to_string(options.common.width) + "x" +
            std::to_string(options.common.height) + "|instancing=" +
            std::to_string(static_cast<uint32>(options.instancingMode)) +
            "|validation=" +
            (options.common.enableValidation ? "on" : "off");
        assessmentMetadata.fingerprint.renderConfiguration +=
            "|fixed-time=" +
            std::to_string(options.fixedTimeNumerator) + "/" +
            std::to_string(options.fixedTimeDenominator) +
            "|deterministic-seed=" +
            std::to_string(options.deterministicSeed) +
            "|capture-tick=" + std::to_string(options.captureTick) +
            "|view=" +
            (options.viewBookmarkId.empty() ? std::string("default")
                                            : options.viewBookmarkId);
        assessmentMetadata.fingerprint.renderConfiguration +=
            "|sample-manifest-v=" +
            std::to_string(m_registry.GetManifestSchemaVersion()) +
            "|sample-manifest-sha256=" +
            std::string(m_registry.GetManifestContentSha256());
        assessmentMetadata.fingerprint.renderConfiguration +=
            std::string("|assessment-profile=") +
            GetSampleAssessmentProfileName(options.assessmentProfile);
        if (options.common.hiZReflectionQualificationProbeEnabled)
        {
            assessmentMetadata.fingerprint.renderConfiguration +=
                "|feature-qualification-profile=gfx19-reflections";
            assessmentMetadata.values.emplace(
                "featureQualificationProfile", "gfx19-reflections");
        }
        if (effectiveReadyWait)
        {
            assessmentMetadata.fingerprint.renderConfiguration +=
                "|ready-timeout-ms=" +
                std::to_string(readinessWaitBudget.timeoutMs) +
                "|ready-max-frames=" +
                std::to_string(readinessWaitBudget.maximumFrames);
            assessmentMetadata.values.emplace(
                "readyTimeoutMs",
                std::to_string(readinessWaitBudget.timeoutMs));
            assessmentMetadata.values.emplace(
                "readyMaxFrames",
                std::to_string(readinessWaitBudget.maximumFrames));
        }
        if (sceneOptions.workloadProfile.has_value())
        {
            const SampleWorkloadProfile& workload =
                *sceneOptions.workloadProfile;
            const std::string workloadScale =
                GetSampleWorkloadScaleName(workload.scale);
            assessmentMetadata.fingerprint.renderConfiguration +=
                "|workload-scale=" + workloadScale +
                "|objects=" + std::to_string(workload.objectCount) +
                "|dirty=" + std::to_string(workload.dirtyObjectCount) +
                "|churn=" + std::to_string(workload.churnObjectCount) +
                "|gpu-culling-capacity=" +
                std::to_string(workload.gpuCullingCapacity);
            assessmentMetadata.values.emplace("workloadScale",
                                              workloadScale);
            assessmentMetadata.values.emplace(
                "workloadObjectCount",
                std::to_string(workload.objectCount));
            assessmentMetadata.values.emplace(
                "workloadDirtyObjectCount",
                std::to_string(workload.dirtyObjectCount));
            assessmentMetadata.values.emplace(
                "workloadChurnObjectCount",
                std::to_string(workload.churnObjectCount));
            assessmentMetadata.values.emplace(
                "workloadGpuCullingCapacity",
                std::to_string(workload.gpuCullingCapacity));
        }
        // The portable asset set is deliberately late-bound from immutable
        // owner-thread receipts after model readiness. Catalog paths and roots
        // are never used as fingerprint evidence.
        assessmentMetadata.fingerprint.assetSet.clear();
        assessmentMetadata.fingerprint.platform =
            GetAssessmentPlatformFingerprint();
        assessmentMetadata.values.emplace(
            "profile", GetSampleAssessmentProfileName(options.assessmentProfile));
        FrameworkAssessmentSession assessmentSession(
            std::move(assessmentContract), std::move(assessmentMetadata));
        std::unique_ptr<SampleLifetimeQualification> lifetimeQualification;
        std::chrono::steady_clock::time_point lifetimeReadyStart{};
        bool lifetimeReadyStarted = false;
        bool lifetimeReportWritten = true;
        bool resizePending = false;
        bool resizeToAlternateExtent = true;
        bool qualificationCapturePrepared = false;
        bool firstModelFramePresentedRecorded = false;
        bool assessmentActionAppliedRecorded = false;
        bool assessmentStableRecorded = false;
        uint32 pendingResizeWidth = 0;
        uint32 pendingResizeHeight = 0;
        uint32 notifiedViewportWidth = options.common.width;
        uint32 notifiedViewportHeight = options.common.height;
        SampleWorldRequirements worldRequirements =
            sample->GetWorldRequirements();
        worldRequirements.world.name = "SampleWorld";
        World* world = engine.CreateWorld(worldRequirements.world);
        SceneECS::SceneEcsRuntime* scene =
            world != nullptr ? &world->GetSceneEcsRuntime() : nullptr;
        WorldECS::WorldEcsCameraService* cameras =
            world != nullptr ? &world->GetCameraService() : nullptr;
        IWorldEcsRuntimeServices* runtimeServices =
            world != nullptr ? engine.GetWorldEcsRuntimeServices(world) : nullptr;
        const WorldECS::WorldEcsCameraRef camera =
            cameras != nullptr ? cameras->CreateMainCamera() : WorldECS::WorldEcsCameraRef{};
        if (!world || !scene || !cameras || !runtimeServices || !camera.IsValid())
        {
            error = "Failed to create the sample ECS World or active camera";
        }
        else
        {
            engine.SetActiveWorld(world);

            RenderFrameSettings renderSettings = engine.GetRenderFrameSettings();
            const SampleAssetRegistry assetRegistry =
                MakeSampleAssetRegistry(resolvedAssets,
                                        options.runtimeOnlyComposition);
            SampleModelLoader models(
                *runtimeServices,
                *scene,
                assetRegistry,
                MakeModelContentIdentityRegistry(resolvedAssets));
            SampleEnvironmentLoader environments(
                *runtimeServices,
                scene->GetSceneRuntimeId(),
                assetRegistry,
                *m_composition);
            SampleAnimationLoader animations(
                *runtimeServices,
                scene->GetSceneRuntimeId(),
                assetRegistry);
            SampleParticleEffectLoader particleEffects(
                *resourceSubsystem,
                assetRegistry);
            SampleSceneLifetimeScope sceneLifetime(*scene);
            const SceneECS::SceneEcsDiagnosticsSnapshot engineBaselineScene =
                scene->GetDiagnosticsSnapshot();
            const WorldECS::WorldEcsCameraRef engineBaselineCamera = camera;
            const Resource::IResourceDiagnosticsView& resourceDiagnosticsView =
                static_cast<const Resource::IResourceDiagnosticsView&>(
                    *resourceSubsystem);
            SampleContext context{
                *world,
                *scene,
                *runtimeServices,
                static_cast<const Resource::IResourcePublicationView&>(
                    *resourceSubsystem),
                resourceDiagnosticsView,
                *cameras,
                camera,
                renderSettings,
                input,
                models,
                environments,
                animations,
                particleEffects,
                sceneLifetime,
                sceneOptions,
                assessmentSession.GetChannel(),
            };

            const auto applyContractCamera =
                [&](bool markCut, std::string& outError)
            {
                if (selectedBookmark == nullptr)
                {
                    return true;
                }
                constexpr float32 DegreesToRadians =
                    0.017453292519943295769f;
                const float32 aspect =
                    static_cast<float32>(options.common.width) /
                    static_cast<float32>(
                        std::max(options.common.height, 1u));
                Vec3 position(
                    selectedBookmark->position[0],
                    selectedBookmark->position[1],
                    selectedBookmark->position[2]);
                const Vec3 target(
                    selectedBookmark->target[0],
                    selectedBookmark->target[1],
                    selectedBookmark->target[2]);
                if (deterministicEpochActive &&
                    options.common
                        .ambientOcclusionQualificationProbeEnabled)
                {
                    // GTAO qualification must exercise actual temporal
                    // reprojection, not merely sample a static history with a
                    // zero velocity field. Keep this path runner-owned so the
                    // manifest bookmark remains the deterministic authority.
                    const float64 seconds =
                        static_cast<float64>(deterministicTick + 1u) *
                        options.fixedDeltaSeconds;
                    position.x += static_cast<float32>(
                        std::sin(seconds * 2.0) * 0.35);
                    position.y += static_cast<float32>(
                        std::sin(seconds * 1.3) * 0.12);
                }
                if (!cameras->SetPerspective(
                        camera,
                        selectedBookmark->verticalFieldOfViewDegrees *
                            DegreesToRadians,
                        aspect,
                        selectedBookmark->nearPlane,
                        selectedBookmark->farPlane) ||
                    !cameras->SetPose(
                        camera,
                        {.position = position}) ||
                    !cameras->LookAt(camera, target) ||
                    !cameras->SetExposure(
                        camera, selectedBookmark->exposure) ||
                    (markCut && !cameras->MarkCut(camera)))
                {
                    outError = "Failed to apply Sample camera bookmark '" +
                               selectedBookmark->id + "'";
                    return false;
                }
                return true;
            };

            static_cast<void>(assessmentSession.RecordCheckpoint(
                AssessmentCheckpoints::EngineBaseline));
            static_cast<void>(RecordRuntimeAssessmentSnapshot(
                assessmentSession,
                AssessmentCheckpoints::EngineBaseline,
                scene,
                &resourceDiagnosticsView,
                runtimeServices,
                &diagnostics,
                &engine,
                1,
                requireGpuTiming));

            Diagnostics::TraceSpan setupSpan = Diagnostics::BeginTraceSpan(
                startupTraceContext,
                "SampleSetup",
                {{"sample", registeredInfo->id}});
            static_cast<void>(assessmentSession.RecordCheckpoint(
                AssessmentCheckpoints::ActionRequested));
            setupSucceeded = sample->Setup(context, error);
            setupSpan.SetAttribute("result",
                                   setupSucceeded ? "completed" : "failed");
            setupSpan.End();
            Diagnostics::RecordTraceInstant(
                startupTraceContext,
                "SetupReturned",
                {{"success", setupSucceeded}});
            context.renderSettings.instancingMode = options.instancingMode;
            if (setupSucceeded &&
                !engine.SetRenderFrameSettings(context.renderSettings))
            {
                error = "Engine rejected sample render settings";
                setupSucceeded = false;
            }
            if (setupSucceeded &&
                engine.GetRenderFrameSettings().instancingMode !=
                    options.instancingMode)
            {
                error = "Engine did not retain the requested instancing mode";
                setupSucceeded = false;
            }

            diagnostics = render->GetDiagnosticsSnapshot();
            static_cast<void>(assessmentSession.RecordCheckpoint(
                AssessmentCheckpoints::ScenarioSetup));
            static_cast<void>(RecordRuntimeAssessmentSnapshot(
                assessmentSession,
                AssessmentCheckpoints::ScenarioSetup,
                scene,
                &resourceDiagnosticsView,
                runtimeServices,
                &diagnostics,
                &engine,
                1,
                requireGpuTiming));

            RuntimeFrameDriver frameDriver(engine, *render);

            // Qualification is armed immediately before the sole target Tick,
            // never at setup.  That makes its immutable recording identity
            // prove the screenshot/policy frame rather than an early warm-up
            // submission.
            const bool directRasterReadbackQualificationEnabled =
                options.common.directOpaqueRasterReadbackQualificationEnabled;
            const bool qualificationEnabled =
                options.common.gpuSceneCullingQualificationEnabled ||
                directRasterReadbackQualificationEnabled;
            const bool exposureQualificationProbeEnabled =
                options.common.exposureQualificationProbeEnabled;
            const bool ambientOcclusionQualificationProbeEnabled =
                options.common.ambientOcclusionQualificationProbeEnabled;
            const bool hiZReflectionQualificationProbeEnabled =
                options.common.hiZReflectionQualificationProbeEnabled;
            const bool toonOutlineQualificationProbeEnabled =
                options.common.toonOutlineQualificationProbeEnabled;
            const auto hasQualificationComparison =
                [directRasterReadbackQualificationEnabled](
                    const SampleRenderDiagnostics& value)
            {
                return directRasterReadbackQualificationEnabled
                    ? HasDirectRasterReadbackQualificationComparison(value)
                    : HasGPUSceneCullingQualificationComparison(value);
            };
            const auto qualificationMatchesTargetFrame =
                [directRasterReadbackQualificationEnabled](
                    const SampleRenderDiagnostics& value, uint64 target)
            {
                return directRasterReadbackQualificationEnabled
                    ? DirectRasterReadbackQualificationMatchesFrame(value, target)
                    : RequiredGPUCullingQualificationMatchesFrame(value, target);
            };
            const auto hasQualificationMatch =
                [directRasterReadbackQualificationEnabled](
                    const SampleRenderDiagnostics& value)
            {
                return directRasterReadbackQualificationEnabled
                    ? HasDirectRasterReadbackQualificationMatch(value)
                    : HasMatchedGPUSceneCullingQualification(value);
            };
            const auto requestQualification =
                [render, directRasterReadbackQualificationEnabled]()
            {
                return directRasterReadbackQualificationEnabled
                    ? render->RequestDirectOpaqueRasterReadbackQualificationCapture()
                    : render->RequestGPUSceneCullingQualificationCapture();
            };
            const auto hasExposureProbeForTarget =
                [](const RenderDiagnosticsSnapshot& value, uint64 target)
            {
                const RenderExecutionReceipt& receipt =
                    value.frameFeatures.executionReceipt;
                return receipt.IsStructurallyValid() && receipt.available &&
                       receipt.presented && receipt.succeeded &&
                       receipt.frameIdentity.frameSequence == target &&
                       receipt.postProcess.exposureProbeRequested &&
                       receipt.postProcess.exposureProbeReady &&
                       receipt.postProcess.exposureHistoryCandidateProduced &&
                       receipt.postProcess.exposureHistoryCommitted &&
                       !receipt.postProcess.exposureHistoryAbandoned;
            };
            const auto hasAmbientOcclusionProbeForTarget =
                [](const RenderDiagnosticsSnapshot& value, uint64 target)
            {
                const RenderExecutionReceipt& receipt =
                    value.frameFeatures.executionReceipt;
                const RenderAmbientOcclusionExecutionReceipt& ambientOcclusion =
                    receipt.ambientOcclusion;
                const bool nonNeutralProbe =
                    ambientOcclusion.probeSampleCount != 0 &&
                    ambientOcclusion.probeOccludedSampleCount != 0 &&
                    ambientOcclusion.probeOccludedSampleCount <
                        ambientOcclusion.probeSampleCount &&
                    std::isfinite(ambientOcclusion.probeMinVisibility) &&
                    std::isfinite(ambientOcclusion.probeMeanVisibility) &&
                    std::isfinite(ambientOcclusion.probeMaxVisibility) &&
                    ambientOcclusion.probeMinVisibility >= 0.0f &&
                    ambientOcclusion.probeMinVisibility < 1.0f &&
                    ambientOcclusion.probeMeanVisibility >=
                        ambientOcclusion.probeMinVisibility &&
                    ambientOcclusion.probeMeanVisibility < 1.0f &&
                    ambientOcclusion.probeMaxVisibility >=
                        ambientOcclusion.probeMeanVisibility &&
                    ambientOcclusion.probeMaxVisibility <= 1.0f;
                return receipt.IsStructurallyValid() && receipt.available &&
                       receipt.presented && receipt.succeeded &&
                       receipt.frameIdentity.frameSequence == target &&
                       ambientOcclusion.IsStructurallyValid() &&
                       ambientOcclusion.executed &&
                       ambientOcclusion.outputFormat == RHIFormat::R8_UNORM &&
                       ambientOcclusion.probeRequested &&
                       ambientOcclusion.probeReady &&
                       ambientOcclusion.historyAvailableBefore &&
                       ambientOcclusion.historySampled &&
                       ambientOcclusion.velocityInputReady &&
                       ambientOcclusion.velocityProducerExecuted &&
                       ambientOcclusion.velocityFieldIdentity != 0 &&
                       ambientOcclusion.motionReprojectionUsed &&
                       ambientOcclusion.viewProjectionMotionObserved &&
                       ambientOcclusion.historyCandidateProduced &&
                       ambientOcclusion.historyCommitted &&
                       !ambientOcclusion.historyAbandoned &&
                       ambientOcclusion.historyResetReasons ==
                           RenderHistoryResetReason::None &&
                       ambientOcclusion.lightingConsumerExecuted &&
                       ambientOcclusion.aoFieldContentHash != 0 &&
                       ambientOcclusion.aoFieldContentHash ==
                           ambientOcclusion.lightingAOFieldContentHash &&
                       ambientOcclusion.consumerMask == 0x7u &&
                       !ambientOcclusion.fallbackUsed && nonNeutralProbe;
            };
            const auto hasToonOutlineProbeForTarget =
                [](const RenderDiagnosticsSnapshot& value, uint64 target)
            {
                return EvaluateSampleToonOutlineQualificationTarget(
                           value, target)
                    .IsReady();
            };
            const auto hasHiZReflectionProbeForTarget =
                [](const RenderDiagnosticsSnapshot& value, uint64 target)
            {
                const RenderExecutionReceipt& receipt =
                    value.frameFeatures.hiZReflectionQualificationReceipt;
                const RenderReflectionDiagnostics& reflection =
                    value.frameFeatures.hiZReflectionQualification;
                return receipt.IsStructurallyValid() && receipt.available &&
                       receipt.presented && receipt.succeeded &&
                       receipt.frameIdentity.frameSequence == target &&
                       reflection.available && reflection.graphExecutionFinalized &&
                       reflection.frameSequence == target &&
                       reflection.requested && reflection.supported &&
                       reflection.scheduled && reflection.executed &&
                       reflection.hiZBuilt && reflection.globalIBLReady &&
                       reflection.fallbackChainResolved &&
                       reflection.screenSpaceRequested &&
                       reflection.screenSpaceTraceInputsReady &&
                       reflection.screenSpaceTraceScheduled &&
                       reflection.screenSpaceApplied &&
                       reflection.probesApplied &&
                       reflection.readyProbeCount != 0 &&
                       reflection.reflectionRouteEvidenceReady &&
                       reflection.routeMaskRadianceSchemaVersion == 1 &&
                       reflection.localProbeRadianceQualified &&
                       reflection.globalIBLRadianceQualified &&
                       reflection.hiZMipCount != 0 &&
                       reflection.hiZGraphPassCount == reflection.hiZMipCount &&
                       reflection.traceGraphPassCount == 1 &&
                       reflection.resolveGraphPassCount == 1 &&
                       reflection.reflectionPlanIdentity != 0 &&
                       reflection.lightingPlanIdentity != 0 &&
                       reflection.configurationIdentity != 0 &&
                       reflection.pipelineIdentity != 0 &&
                       reflection.depthPyramidIdentity != 0 &&
                       reflection.reflectionSurfaceIdentity != 0 &&
                       reflection.exposureStateContentHash != 0 &&
                       reflection.screenSpaceHitPixelCount != 0 &&
                       reflection.localProbePixelCount != 0 &&
                       reflection.globalIBLPixelCount != 0 &&
                       reflection.reflectionReadbackSampleCount != 0 &&
                       reflection.reflectionRouteSampleCount != 0 &&
                       reflection.screenSpaceHitPixelCount +
                               reflection.localProbePixelCount +
                               reflection.globalIBLPixelCount ==
                           reflection.reflectionRouteSampleCount &&
                       reflection.backgroundPixelCount +
                               reflection.nonReflectivePixelCount +
                               reflection.reflectionRouteSampleCount ==
                           reflection.reflectionReadbackSampleCount &&
                       reflection.fallbackReason.empty();
            };
            const auto hasHiZReflectionCopyPassForTarget =
                [](const RenderDiagnosticsSnapshot& value, uint64 target)
            {
                const RenderExecutionReceipt& receipt =
                    value.frameFeatures.executionReceipt;
                if (!receipt.IsStructurallyValid() || !receipt.available ||
                    receipt.frameIdentity.frameSequence != target)
                {
                    return false;
                }
                const auto reflectionFeature = std::find_if(
                    receipt.features.begin(),
                    receipt.features.end(),
                    [](const RenderFeatureExecutionReceipt& feature)
                    {
                        return feature.featureId ==
                            BuiltinRenderFeatureIds::ReflectionResolve;
                    });
                return reflectionFeature != receipt.features.end() &&
                       std::find(reflectionFeature->graphPassNames.begin(),
                                 reflectionFeature->graphPassNames.end(),
                                 "SSRQualificationRouteMaskCopy") !=
                           reflectionFeature->graphPassNames.end();
            };
            bool gpuSceneCullingQualificationRequestAccepted =
                !qualificationEnabled;
            bool exposureQualificationProbeRequestAccepted =
                !exposureQualificationProbeEnabled;
            bool ambientOcclusionQualificationProbeRequestAccepted =
                !ambientOcclusionQualificationProbeEnabled;
            bool hiZReflectionQualificationProbeRequestAccepted =
                !hiZReflectionQualificationProbeEnabled;
            bool toonOutlineQualificationProbeRequestAccepted =
                !toonOutlineQualificationProbeEnabled;
            uint64 gpuSceneCullingQualificationTargetFrameSequence = 0;
            uint64 gpuSceneCullingQualificationTargetPublishedSequence = 0;
            uint64 gpuSceneCullingQualificationTargetSubmittedSequence = 0;
            uint64 gpuSceneCullingQualificationTargetPresentedSequence = 0;
            uint64 exposureQualificationProbeTargetFrameSequence = 0;
            uint64 ambientOcclusionQualificationProbeTargetFrameSequence = 0;
            uint64 hiZReflectionQualificationProbeTargetFrameSequence = 0;
            uint64 toonOutlineQualificationProbeTargetFrameSequence = 0;

            if (setupSucceeded)
            {
                if (options.HasLifetimeQualification())
                {
                    SampleLifetimeQualificationMetadata metadata;
                    metadata.sampleName = registeredInfo->id;
                    metadata.assetId = sceneOptions.assetId;
                    metadata.backend = options.common.backend;
                    metadata.renderPath =
                        GetSampleRenderPathName(sceneOptions.renderPath);
                    metadata.width = options.common.width;
                    metadata.height = options.common.height;
                    metadata.deterministicOrbit =
                        options.deterministicCameraOrbit;
                    lifetimeQualification =
                        std::make_unique<SampleLifetimeQualification>(
                            options.lifetimeConfig,
                            std::move(metadata));
                }
                const auto readinessDeadline =
                    std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(readinessWaitBudget.timeoutMs);
                while (!engine.ShouldShutdown())
                {
                    if (input->IsKeyPressed(HAL::Key::Escape))
                    {
                        engine.RequestShutdown();
                        break;
                    }

                    if (!options.qualificationPerformancePath.empty() &&
                        renderQualityPerformanceRequestAccepted)
                    {
                        const RenderQualityPerformanceDiagnostics& performance =
                            diagnostics.qualityPerformance;
                        if (performance.session.state ==
                            RenderQualityPerformanceSessionState::Rejected)
                        {
                            error = "GFX24 performance session failed: " +
                                std::string(GetRenderQualityPerformanceFailureName(
                                    performance.session.failure));
                            if (!performance.session.diagnostic.empty())
                            {
                                error += ": " + performance.session.diagnostic;
                            }
                            engine.RequestShutdown();
                            break;
                        }
                        if (performance.session.state ==
                            RenderQualityPerformanceSessionState::Completed)
                        {
                            if (!performance.completedEvidence)
                            {
                                error =
                                    "GFX24 performance session completed without immutable evidence";
                                engine.RequestShutdown();
                                break;
                            }

                            RenderQualityPerformanceWriteRequest writeRequest;
                            if (!BuildRenderQualityPerformanceWriteRequest(
                                    *performance.completedEvidence,
                                    renderQualityPerformanceHostProvenance,
                                    options,
                                    *manifest,
                                    writeRequest,
                                    &error))
                            {
                                engine.RequestShutdown();
                                break;
                            }
                            RenderQualityPerformanceWriteResult writeResult;
                            if (!WriteRenderQualityPerformanceReport(
                                    writeRequest, writeResult, &error))
                            {
                                if (error.empty())
                                {
                                    error =
                                        "Failed to publish GFX24 performance evidence";
                                }
                                engine.RequestShutdown();
                                break;
                            }
                            std::error_code expectedError;
                            std::error_code actualError;
                            const std::filesystem::path expected =
                                std::filesystem::weakly_canonical(
                                    options.qualificationPerformancePath,
                                    expectedError);
                            const std::filesystem::path actual =
                                std::filesystem::weakly_canonical(
                                    writeResult.reportPath,
                                    actualError);
                            if (expectedError || actualError || expected != actual)
                            {
                                error =
                                    "GFX24 performance writer published an unexpected report path";
                                engine.RequestShutdown();
                                break;
                            }
                            engine.RequestShutdown();
                            continue;
                        }

                        if (!renderQualityPerformanceDeadline.has_value() ||
                            std::chrono::steady_clock::now() >=
                                *renderQualityPerformanceDeadline)
                        {
                            readinessTimedOut = true;
                            error =
                                "GFX24 performance session exceeded the readiness deadline";
                            engine.RequestShutdown();
                            break;
                        }
                        if (performance.session.state ==
                            RenderQualityPerformanceSessionState::Draining)
                        {
                            // All 60 warm-up and 300 measured submissions are
                            // already fixed. Draining must not publish another
                            // Engine frame that could alter the session data.
                            diagnostics = frameDriver.PumpRenderProgressOnce();
                            continue;
                        }
                        if (renderQualityPerformanceHostFrames >=
                            readinessWaitBudget.maximumFrames)
                        {
                            error =
                                "GFX24 performance session exceeded its finite host frame budget";
                            engine.RequestShutdown();
                            break;
                        }
                    }

                    if (toonOutlineQualificationProbeEnabled &&
                        toonOutlineQualificationProbeRequestAccepted)
                    {
                        const RenderToonOutlineQualificationRequestResult
                            admission = render
                                ->GetToonOutlineQualificationProbeRequestResult();
                        if (admission.IsRejected())
                        {
                            toonOutlineQualificationProbeRequestAccepted =
                                false;
                            error = admission.diagnostic != nullptr &&
                                    admission.diagnostic[0] != '\0'
                                ? admission.diagnostic
                                : "Render owner rejected the Toon-outline qualification probe";
                            engine.RequestShutdown();
                            break;
                        }
                    }

                    if (hiZReflectionQualificationProbeEnabled &&
                        hiZReflectionQualificationProbeRequestAccepted)
                    {
                        const RenderHiZReflectionQualificationRequestResult
                            admission = render
                                ->GetHiZReflectionQualificationProbeRequestResult();
                        if (admission.IsRejected())
                        {
                            hiZReflectionQualificationProbeRequestAccepted =
                                false;
                            error = admission.diagnostic != nullptr &&
                                    admission.diagnostic[0] != '\0'
                                ? admission.diagnostic
                                : "Render owner rejected the Hi-Z reflection qualification probe";
                            engine.RequestShutdown();
                            break;
                        }
                    }

                    if (exposureQualificationProbeEnabled &&
                        exposureQualificationProbeTargetFrameSequence != 0 &&
                        !finalRenderDrainRequested)
                    {
                        // The target image already presented. Poll only the
                        // Render owner until its explicit probe completion
                        // supplements that exact frame's immutable receipt.
                        if (std::chrono::steady_clock::now() >= readinessDeadline)
                        {
                            readinessTimedOut = true;
                            error =
                                "Exposure qualification probe did not complete for the final target frame";
                            engine.RequestShutdown();
                            break;
                        }
                        diagnostics = frameDriver.PumpRenderProgressOnce();
                        const SampleRenderDiagnostics completionDiagnostics =
                            MakeSampleRenderDiagnostics(diagnostics, engine);
                        sample->ObserveDiagnostics(completionDiagnostics,
                                                   context.assessment);
                        if (hasExposureProbeForTarget(
                                diagnostics,
                                exposureQualificationProbeTargetFrameSequence))
                        {
                            captureExecutionReceipt =
                                diagnostics.frameFeatures.executionReceipt;
                            engine.RequestShutdown();
                        }
                        continue;
                    }

                    if (ambientOcclusionQualificationProbeEnabled &&
                        ambientOcclusionQualificationProbeTargetFrameSequence != 0 &&
                        !finalRenderDrainRequested)
                    {
                        // GTAO uses the same target-frame discipline as the
                        // exposure probe: after presentation, progress only
                        // the Render owner until its immutable receipt is
                        // supplemented by the explicit probe result.
                        if (std::chrono::steady_clock::now() >= readinessDeadline)
                        {
                            readinessTimedOut = true;
                            error =
                                "Ambient-occlusion qualification probe did not complete for the final target frame";
                            engine.RequestShutdown();
                            break;
                        }
                        diagnostics = frameDriver.PumpRenderProgressOnce();
                        const SampleRenderDiagnostics completionDiagnostics =
                            MakeSampleRenderDiagnostics(diagnostics, engine);
                        sample->ObserveDiagnostics(completionDiagnostics,
                                                   context.assessment);
                        if (hasAmbientOcclusionProbeForTarget(
                                diagnostics,
                                ambientOcclusionQualificationProbeTargetFrameSequence))
                        {
                            captureExecutionReceipt =
                                diagnostics.frameFeatures.executionReceipt;
                            engine.RequestShutdown();
                        }
                        continue;
                    }

                    if (hiZReflectionQualificationProbeEnabled &&
                        hiZReflectionQualificationProbeTargetFrameSequence != 0 &&
                        !finalRenderDrainRequested)
                    {
                        // Route-mask completion belongs to the already
                        // presented source frame. Progress only the Render
                        // owner while the physical copy fence resolves.
                        if (std::chrono::steady_clock::now() >= readinessDeadline)
                        {
                            readinessTimedOut = true;
                            error =
                                "Hi-Z reflection qualification probe did not complete for the final target frame";
                            engine.RequestShutdown();
                            break;
                        }
                        diagnostics = frameDriver.PumpRenderProgressOnce();
                        const SampleRenderDiagnostics completionDiagnostics =
                            MakeSampleRenderDiagnostics(diagnostics, engine);
                        sample->ObserveDiagnostics(completionDiagnostics,
                                                   context.assessment);
                        if (hasHiZReflectionProbeForTarget(
                                diagnostics,
                                hiZReflectionQualificationProbeTargetFrameSequence))
                        {
                            captureExecutionReceipt =
                                diagnostics.frameFeatures
                                    .hiZReflectionQualificationReceipt;
                            engine.RequestShutdown();
                        }
                        continue;
                    }

                    if (toonOutlineQualificationProbeEnabled &&
                        toonOutlineQualificationProbeTargetFrameSequence != 0 &&
                        !finalRenderDrainRequested)
                    {
                        // The verifier belongs to the already-presented target
                        // frame. Progress only the Render owner so a replacement
                        // frame cannot steal its immutable receipt identity.
                        if (std::chrono::steady_clock::now() >= readinessDeadline)
                        {
                            readinessTimedOut = true;
                            error =
                                "Toon-outline qualification probe did not complete for the final target frame";
                            engine.RequestShutdown();
                            break;
                        }
                        diagnostics = frameDriver.PumpRenderProgressOnce();
                        const SampleRenderDiagnostics completionDiagnostics =
                            MakeSampleRenderDiagnostics(diagnostics, engine);
                        sample->ObserveDiagnostics(completionDiagnostics,
                                                   context.assessment);
                        const auto targetEvaluation =
                            EvaluateSampleToonOutlineQualificationTarget(
                                diagnostics,
                                toonOutlineQualificationProbeTargetFrameSequence);
                        if (targetEvaluation.IsReady())
                        {
                            captureExecutionReceipt =
                                diagnostics.frameFeatures.executionReceipt;
                            engine.RequestShutdown();
                        }
                        else if (targetEvaluation.IsFailed())
                        {
                            error = targetEvaluation.diagnostic.empty()
                                ? "Toon-outline qualification failed for the final target frame"
                                : targetEvaluation.diagnostic;
                            engine.RequestShutdown();
                            break;
                        }
                        continue;
                    }

                    if (qualificationEnabled &&
                        gpuSceneCullingQualificationTargetFrameSequence != 0 &&
                        !finalRenderDrainRequested)
                    {
                        // The target has presented.  Its post-fence evidence
                        // must now drain on the Render owner only: a further
                        // Engine/sample tick could publish a replacement frame
                        // and detach qualification from the final image.
                        if (std::chrono::steady_clock::now() >= readinessDeadline)
                        {
                            readinessTimedOut = true;
                            error = directRasterReadbackQualificationEnabled
                                ? "Direct Opaque raster readback qualification completion did not arrive for the final target frame"
                                : "GPUScene culling qualification completion did not arrive for the final target frame";
                            engine.RequestShutdown();
                            break;
                        }
                        diagnostics = frameDriver.PumpRenderProgressOnce();
                        const SampleRenderDiagnostics completionDiagnostics =
                            MakeSampleRenderDiagnostics(diagnostics, engine);
                        sample->ObserveDiagnostics(completionDiagnostics,
                                                   context.assessment);
                        if (hasQualificationComparison(completionDiagnostics) &&
                            qualificationMatchesTargetFrame(completionDiagnostics,
                                gpuSceneCullingQualificationTargetFrameSequence))
                        {
                            engine.RequestShutdown();
                        }
                        continue;
                    }

                    if (finalRenderDrainRequested)
                    {
                        constexpr uint64 FinalDrainCaptureRequestId = 2;
                        if (!finalRenderDrainPresentationSubmitted)
                        {
                            const auto drainStart =
                                std::chrono::steady_clock::now();
                            if (drainStart >= readinessDeadline)
                            {
                                readinessTimedOut = true;
                                error =
                                    "Sample readiness wait expired before its final render drain presentation";
                                engine.RequestShutdown();
                                break;
                            }

                            if (!options.common.screenshotPath.empty() ||
                                options.common.pixelProbeEnabled)
                            {
                                RenderFrameCaptureRequest captureRequest;
                                captureRequest.requestId =
                                    FinalDrainCaptureRequestId;
                                captureRequest.kind =
                                    RenderFrameCaptureKind::Color;
                                captureRequest.width = options.common.width;
                                captureRequest.height = options.common.height;
                                captureRequest.includeAlpha = false;
                                captureRequest.pixelProbeEnabled =
                                    options.common.pixelProbeEnabled;
                                captureRequest.pixelProbeX =
                                    options.common.pixelProbeX;
                                captureRequest.pixelProbeY =
                                    options.common.pixelProbeY;
                                finalRenderDrainCaptureQueued =
                                    engine.RequestRenderFrameCapture(
                                        captureRequest);
                                if (!finalRenderDrainCaptureQueued)
                                {
                                    error =
                                        "Failed to queue the final render-drain capture";
                                    engine.RequestShutdown();
                                    break;
                                }
                            }

                            if (qualificationEnabled)
                            {
                                gpuSceneCullingQualificationRequestAccepted =
                                    requestQualification();
                                if (!gpuSceneCullingQualificationRequestAccepted)
                                {
                                    error = directRasterReadbackQualificationEnabled
                                        ? "Render runtime rejected final Direct Opaque raster readback qualification capture"
                                        : "Render runtime rejected final GPUScene culling qualification capture";
                                    engine.RequestShutdown();
                                    break;
                                }
                            }

                            const uint64 publishedBeforeFinalTick =
                                diagnostics.lastPublishedFrameSequence;
                            // This is the final Engine tick in the protocol.
                            // It publishes the capture-carrying frame, after
                            // which all progress is owner-side completion work.
                            diagnostics = frameDriver.TickOnce(sampleDeltaTime);
                            ++executedFrames;
                            if (deterministicEpochActive)
                            {
                                ++deterministicTick;
                            }

                            RuntimeFrameWaitRequest presentationRequest;
                            const uint64 finalPresentationSequence =
                                ResolveFrameWaitTargetSequence(
                                    publishedBeforeFinalTick,
                                    diagnostics.lastPublishedFrameSequence);
                            presentationRequest.minimumPublishedSequence =
                                finalPresentationSequence;
                            presentationRequest.minimumSubmittedSequence =
                                finalPresentationSequence;
                            presentationRequest.minimumPresentedSequence =
                                finalPresentationSequence;
                            presentationRequest.captureRequestId =
                                finalRenderDrainCaptureQueued
                                    ? FinalDrainCaptureRequestId
                                    : 0;
                            // The outer final tick can still be pending when
                            // this wait starts. Permit only the Engine ticks
                            // needed to publish this exact target; the frame
                            // driver switches to Render-only progress as soon
                            // as publication reaches it, so presentation does
                            // not create a replacement frame.
                            ConfigureFrameWaitPolicy(
                                deterministicEpochActive,
                                sampleDeltaTime,
                                presentationRequest);
                            presentationRequest.timeout = std::max(
                                std::chrono::duration_cast<
                                    std::chrono::milliseconds>(
                                    readinessDeadline - drainStart),
                                std::chrono::milliseconds(1));
                            const uint64 remainingDrainMilliseconds =
                                static_cast<uint64>(
                                    presentationRequest.timeout.count());
                            presentationRequest.maxTicks =
                                static_cast<uint32>(std::max<uint64>(
                                    15000u,
                                    remainingDrainMilliseconds >=
                                            static_cast<uint64>(
                                                std::numeric_limits<uint32>::max())
                                        ? static_cast<uint64>(
                                              std::numeric_limits<uint32>::max())
                                        : remainingDrainMilliseconds + 1u));
                            const RuntimeFrameWaitResult presentationResult =
                                frameDriver.WaitFor(presentationRequest);
                            diagnostics = presentationResult.diagnostics;
                            if (qualificationEnabled &&
                                presentationResult.Reached())
                            {
                                gpuSceneCullingQualificationTargetFrameSequence =
                                    finalPresentationSequence;
                                gpuSceneCullingQualificationTargetPublishedSequence =
                                    finalPresentationSequence;
                                gpuSceneCullingQualificationTargetSubmittedSequence =
                                    finalPresentationSequence;
                                gpuSceneCullingQualificationTargetPresentedSequence =
                                    finalPresentationSequence;
                                if (diagnostics.lastPublishedFrameSequence !=
                                        finalPresentationSequence ||
                                    diagnostics.lastSubmittedFrameSequence !=
                                        finalPresentationSequence ||
                                    diagnostics.lastPresentedFrameSequence !=
                                        finalPresentationSequence ||
                                    (finalRenderDrainCaptureQueued &&
                                     diagnostics.lastCapture.frameSequence !=
                                         finalPresentationSequence))
                                {
                                    error =
                                        "Final qualification target did not retain exact presentation provenance";
                                    engine.RequestShutdown();
                                    break;
                                }
                            }
                            if (finalRenderDrainCaptureQueued &&
                                presentationResult.Reached())
                            {
                                captureCarryingFrameSequence =
                                    diagnostics.lastCapture.frameSequence;
                                captureExecutionReceipt =
                                    diagnostics.frameFeatures.executionReceipt;
                                captureConfigurationRevision =
                                    captureExecutionReceipt.has_value() &&
                                        captureExecutionReceipt->available &&
                                        captureExecutionReceipt
                                            ->frameIdentity
                                            .configurationRevision != 0
                                        ? captureExecutionReceipt
                                              ->frameIdentity
                                              .configurationRevision
                                        : 0;
                                const EngineRenderRuntimeDiagnostics
                                    captureEngineDiagnostics =
                                        engine.GetRenderRuntimeDiagnostics();
                                const SceneECS::SceneEcsDiagnosticsSnapshot
                                    captureSceneDiagnostics =
                                        scene->GetDiagnosticsSnapshot();
                                captureSceneRuntimeId =
                                    captureSceneDiagnostics.sceneRuntimeId
                                            .GetValue();
                                captureCameraEntityPacked =
                                    captureEngineDiagnostics.cameraIdentity;
                                captureSceneRevision =
                                    diagnostics.sceneValues
                                        .requiredSceneRevision;
                                captureSurfaceGeneration =
                                    diagnostics.surfaceGeneration;
                                if (!options.common.screenshotPath.empty())
                                {
                                    screenshotWritten = WriteSampleScreenshotPPM(
                                        diagnostics.lastCapture,
                                        options.common.screenshotPath,
                                        &error);
                                }
                                if (options.common.pixelProbeEnabled)
                                {
                                    pixelProbeCaptured =
                                        diagnostics.lastCapture
                                            .HasMatchingCompletedPixelProbe();
                                }
                            }
                            if (!presentationResult.Reached() ||
                                (!options.common.screenshotPath.empty() &&
                                 finalRenderDrainCaptureQueued &&
                                 !screenshotWritten) ||
                                (options.common.pixelProbeEnabled &&
                                 finalRenderDrainCaptureQueued &&
                                 !pixelProbeCaptured))
                            {
                                if (error.empty())
                                {
                                    error = finalRenderDrainCaptureQueued &&
                                                    !diagnostics.lastCapture.message.empty()
                                                ? diagnostics.lastCapture.message
                                                : "Final render-drain presentation did not complete: " +
                                                      DescribeFrameWait(
                                                          presentationResult,
                                                          finalPresentationSequence);
                                }
                                engine.RequestShutdown();
                                break;
                            }
                            finalRenderDrainPresentationSubmitted = true;
                        }
                        else
                        {
                            if (std::chrono::steady_clock::now() >=
                                readinessDeadline)
                            {
                                readinessTimedOut = true;
                                error =
                                    "Sample readiness wait expired while draining final Render retirements";
                                engine.RequestShutdown();
                                break;
                            }
                            diagnostics =
                                frameDriver.PumpRenderProgressOnce();
                        }

                        const SampleRenderDiagnostics drainDiagnostics =
                            MakeSampleRenderDiagnostics(diagnostics, engine);
                        sample->ObserveDiagnostics(drainDiagnostics,
                                                   context.assessment);
                        sampleReadiness = sample->GetReadiness(drainDiagnostics);
                        sampleReady = sampleReadiness.IsReady();
                        readinessReason = sampleReady
                                              ? std::string{}
                                              : sampleReadiness.reason;
                        if (sampleReadiness.IsFailed())
                        {
                            readinessFailed = true;
                            error = readinessReason.empty()
                                        ? "Sample final render drain failed"
                                        : readinessReason;
                            engine.RequestShutdown();
                            break;
                        }

                        if (sampleReady &&
                            diagnostics.lastPresentedFrameSequence > 0 &&
                            !firstModelFramePresentedRecorded)
                        {
                            Diagnostics::RecordTraceInstant(
                                startupTraceContext,
                                "FirstModelFramePresented",
                                {{"frameSequence",
                                  diagnostics.lastPresentedFrameSequence}});
                            firstModelFramePresentedRecorded = true;
                        }
                        if (sampleReady &&
                            diagnostics.lastPresentedFrameSequence > 0 &&
                            !assessmentStableRecorded)
                        {
                            if (!assessmentActionAppliedRecorded)
                            {
                                static_cast<void>(assessmentSession.RecordCheckpoint(
                                    AssessmentCheckpoints::ActionApplied));
                                assessmentActionAppliedRecorded = true;
                            }
                            static_cast<void>(assessmentSession.RecordCheckpoint(
                                AssessmentCheckpoints::ScenarioStable));
                            static_cast<void>(RecordRuntimeAssessmentSnapshot(
                                assessmentSession,
                                AssessmentCheckpoints::ScenarioStable,
                                scene,
                                &resourceDiagnosticsView,
                                runtimeServices,
                                &diagnostics,
                                &engine,
                                1,
                                requireGpuTiming));
                            assessmentStableRecorded = true;
                        }

                        const bool requestedRunComplete =
                            lifetimeQualification
                                ? lifetimeQualification->IsComplete() &&
                                      !lifetimeQualification->HasFailed()
                                : hasCompletedRequestedTicks();
                        if (sampleReady && requestedRunComplete &&
                            (options.common.screenshotPath.empty() ||
                             screenshotWritten) &&
                            (!options.common.pixelProbeEnabled ||
                             pixelProbeCaptured) &&
                            (!qualificationEnabled ||
                             (hasQualificationComparison(drainDiagnostics) &&
                              qualificationMatchesTargetFrame(drainDiagnostics,
                                  gpuSceneCullingQualificationTargetFrameSequence))))
                        {
                            engine.RequestShutdown();
                        }
                        continue;
                    }

                    if (options.deterministicContractActive && sampleReady &&
                        !deterministicEpochActive)
                    {
                        frameDriver.ResetFixedTime();
                        deterministicTick = 0;
                        if (!engine.ResetEffectFrameTime(options.deterministicSeed))
                        {
                            error =
                                "Engine rejected the deterministic Sample effect clock reset";
                            engine.RequestShutdown();
                            break;
                        }
                        if (!sample->BeginDeterministicEpoch(context, error))
                        {
                            if (error.empty())
                            {
                                error =
                                    "Sample rejected the deterministic epoch reset";
                            }
                            engine.RequestShutdown();
                            break;
                        }
                        if (!applyContractCamera(true, error))
                        {
                            engine.RequestShutdown();
                            break;
                        }
                        deterministicCameraApplied =
                            selectedBookmark != nullptr;
                        if (engine.RequestRenderTemporalReset() == 0)
                        {
                            error =
                                "Render runtime rejected the deterministic Sample history reset";
                            engine.RequestShutdown();
                            break;
                        }
                        deterministicEpochActive = true;
                    }

                    // Surface resize is an ordinary runtime event, not a
                    // lifetime-qualification-only event.  Notify every sample
                    // exactly once after Render has accepted a new non-zero
                    // extent, before that extent is used for input/update.
                    if (diagnostics.surfaceWidth != 0 &&
                        diagnostics.surfaceHeight != 0 &&
                        (diagnostics.surfaceWidth != notifiedViewportWidth ||
                         diagnostics.surfaceHeight != notifiedViewportHeight))
                    {
                        sample->OnViewportResize(context,
                                                 diagnostics.surfaceWidth,
                                                 diagnostics.surfaceHeight);
                        notifiedViewportWidth = diagnostics.surfaceWidth;
                        notifiedViewportHeight = diagnostics.surfaceHeight;
                    }

                    if (!deterministicEpochActive)
                    {
                        sample->OnInput(context);
                    }
                    sample->Update(context, sampleDeltaTime);
                    if (deterministicEpochActive &&
                        !applyContractCamera(false, error))
                    {
                        engine.RequestShutdown();
                        break;
                    }

                    const uint64 nextLogicalTick =
                        deterministicEpochActive
                            ? deterministicTick + 1u
                            : static_cast<uint64>(executedFrames) + 1u;
                    const bool explicitCaptureTick =
                        options.captureTickResolved &&
                        deterministicEpochActive &&
                        nextLogicalTick == options.captureTick;
                    const bool fixedCaptureFrame =
                        !lifetimeQualification && !effectiveReadyWait &&
                        options.common.frames > 0 &&
                        (options.captureTickResolved
                             ? explicitCaptureTick
                             : nextLogicalTick >= options.common.frames);
                    const bool readyCaptureFrame =
                        effectiveReadyWait && sampleReady &&
                        ((!lifetimeQualification &&
                          (options.captureTickResolved
                               ? explicitCaptureTick
                               : executedFrames >= options.common.frames)) ||
                         (lifetimeQualification &&
                          lifetimeQualification->IsComplete() &&
                          !lifetimeQualification->HasFailed() &&
                          qualificationCapturePrepared &&
                          !resizePending &&
                          diagnostics.surfaceWidth == options.common.width &&
                          diagnostics.surfaceHeight == options.common.height));
                    const bool exposureProbeTargetFrame =
                        exposureQualificationProbeEnabled &&
                        exposureQualificationProbeTargetFrameSequence == 0 &&
                        (fixedCaptureFrame || readyCaptureFrame);
                    const bool ambientOcclusionProbeTargetFrame =
                        ambientOcclusionQualificationProbeEnabled &&
                        ambientOcclusionQualificationProbeTargetFrameSequence == 0 &&
                        (fixedCaptureFrame || readyCaptureFrame);
                    const bool hiZReflectionProbeTargetFrame =
                        hiZReflectionQualificationProbeEnabled &&
                        hiZReflectionQualificationProbeTargetFrameSequence == 0 &&
                        (fixedCaptureFrame || readyCaptureFrame);
                    const bool toonOutlineProbeTargetFrame =
                        toonOutlineQualificationProbeEnabled &&
                        toonOutlineQualificationProbeTargetFrameSequence == 0 &&
                        (fixedCaptureFrame || readyCaptureFrame);
                    const bool captureFrame =
                        (!options.common.screenshotPath.empty() ||
                         options.common.pixelProbeEnabled) &&
                        (fixedCaptureFrame || readyCaptureFrame);
                    const bool renderQualityAttachmentSeriesTargetFrame =
                        renderQualityAttachmentSeriesRequested && sampleReady &&
                        deterministicEpochActive &&
                        renderQualityAttachmentSeriesNextTick <
                            manifest->captureTicks.size() &&
                        nextLogicalTick == manifest->captureTicks[
                            renderQualityAttachmentSeriesNextTick];
                    const bool renderQualityBundleTargetFrame =
                        renderQualityAttachmentSeriesRequested
                            ? renderQualityAttachmentSeriesTargetFrame
                            : (!options.qualificationBundlePath.empty() &&
                               (fixedCaptureFrame || readyCaptureFrame));
                    constexpr uint64 CaptureRequestId = 1;
                    bool captureQueued = false;
                    if (captureFrame)
                    {
                        RenderFrameCaptureRequest request;
                        request.requestId = CaptureRequestId;
                        request.kind = RenderFrameCaptureKind::Color;
                        request.width = options.common.width;
                        request.height = options.common.height;
                        request.includeAlpha = false;
                        request.pixelProbeEnabled =
                            options.common.pixelProbeEnabled;
                        request.pixelProbeX = options.common.pixelProbeX;
                        request.pixelProbeY = options.common.pixelProbeY;
                        captureQueued = engine.RequestRenderFrameCapture(request);
                        if (!captureQueued)
                        {
                            error = "Failed to queue value-owned frame capture";
                            engine.RequestShutdown();
                            break;
                        }
                    }

                    const bool qualificationTargetFrame =
                        qualificationEnabled &&
                        (fixedCaptureFrame || readyCaptureFrame);
                    if (qualificationTargetFrame)
                    {
                        gpuSceneCullingQualificationRequestAccepted =
                            requestQualification();
                        if (!gpuSceneCullingQualificationRequestAccepted)
                        {
                            error = directRasterReadbackQualificationEnabled
                                ? "Render runtime rejected final Direct Opaque raster readback qualification capture"
                                : "Render runtime rejected final GPUScene culling qualification capture";
                            engine.RequestShutdown();
                            break;
                        }
                    }

                    if (renderQualityBundleTargetFrame)
                    {
                        if (renderQualityAttachmentSeriesRequested &&
                            renderQualityAttachmentSeriesRunId.empty())
                        {
                            renderQualityAttachmentSeriesRunId =
                                ReadEnvironmentString("RVX_VALIDATION_RUN_ID");
                            renderQualityAttachmentSeriesHardware = {
                                .backend = GetSampleBackendName(diagnostics.backend),
                                .adapterName = diagnostics.adapterName,
                                .driverVersion = diagnostics.driverVersion,
                                .osBuild = ResolveHostOperatingSystemBuild(),
                                .capabilityHash =
                                    diagnostics.capabilityReportSha256};
                            if (renderQualityAttachmentSeriesRunId.empty() ||
                                renderQualityAttachmentSeriesHardware.backend.empty() ||
                                renderQualityAttachmentSeriesHardware.adapterName.empty() ||
                                renderQualityAttachmentSeriesHardware.driverVersion.empty() ||
                                renderQualityAttachmentSeriesHardware.osBuild.empty() ||
                                renderQualityAttachmentSeriesHardware.capabilityHash.empty())
                            {
                                error =
                                    "GFX24 attachment series lacks frozen host hardware provenance";
                                engine.RequestShutdown();
                                break;
                            }
                        }
                        renderQualityBundleRequestAccepted =
                            render->RequestQualificationCaptureBundle();
                        if (!renderQualityBundleRequestAccepted)
                        {
                            error =
                                "Render runtime rejected the GFX24 semantic attachment bundle request";
                            engine.RequestShutdown();
                            break;
                        }

                        // The Render owner consumes control requests
                        // asynchronously. Do not publish the carrying frame
                        // until diagnostics prove the one-shot capture is
                        // armed; otherwise a completion-only pump can leave an
                        // Armed request waiting forever for a frame that has
                        // already been presented.
                        while (!diagnostics.qualificationCapture.requestPending &&
                               !engine.ShouldShutdown())
                        {
                            const QualificationCaptureDiagnostics& capture =
                                diagnostics.qualificationCapture.capture;
                            if (capture.state ==
                                QualificationCaptureState::Rejected)
                            {
                                error =
                                    "GFX24 bundle capture admission failed: " +
                                    std::string(
                                        GetQualificationCaptureFailureName(
                                            capture.failure));
                                engine.RequestShutdown();
                                break;
                            }
                            if (std::chrono::steady_clock::now() >=
                                readinessDeadline)
                            {
                                readinessTimedOut = true;
                                error =
                                    "GFX24 bundle capture admission exceeded the sample readiness deadline";
                                engine.RequestShutdown();
                                break;
                            }
                            diagnostics =
                                frameDriver.PumpRenderProgressOnce();
                        }
                        if (engine.ShouldShutdown())
                        {
                            break;
                        }
                    }
                    if (exposureProbeTargetFrame)
                    {
                        exposureQualificationProbeRequestAccepted =
                            render->RequestExposureQualificationProbe();
                        if (!exposureQualificationProbeRequestAccepted)
                        {
                            error =
                                "Render runtime rejected the explicit exposure qualification probe";
                            engine.RequestShutdown();
                            break;
                        }
                    }
                    if (ambientOcclusionProbeTargetFrame)
                    {
                        ambientOcclusionQualificationProbeRequestAccepted =
                            render->RequestAmbientOcclusionQualificationProbe();
                        if (!ambientOcclusionQualificationProbeRequestAccepted)
                        {
                            error =
                                "Render runtime rejected the explicit ambient-occlusion qualification probe";
                            engine.RequestShutdown();
                            break;
                        }
                    }
                    if (hiZReflectionProbeTargetFrame)
                    {
                        hiZReflectionQualificationProbeRequestAccepted =
                            render->RequestHiZReflectionQualificationProbe();
                        if (!hiZReflectionQualificationProbeRequestAccepted)
                        {
                            error =
                                "Render runtime rejected the explicit Hi-Z reflection qualification probe";
                            engine.RequestShutdown();
                            break;
                        }
                    }
                    if (toonOutlineProbeTargetFrame)
                    {
                        toonOutlineQualificationProbeRequestAccepted =
                            render->RequestToonOutlineQualificationProbe();
                        if (!toonOutlineQualificationProbeRequestAccepted)
                        {
                            error =
                                "Render runtime rejected the explicit Toon-outline qualification probe";
                            engine.RequestShutdown();
                            break;
                        }
                    }

                    const uint64 publishedBeforeOuterTick =
                        diagnostics.lastPublishedFrameSequence;
                    diagnostics = frameDriver.TickOnce(sampleDeltaTime);
                    ++executedFrames;
                    if (renderQualityPerformanceRequestAccepted)
                    {
                        ++renderQualityPerformanceHostFrames;
                    }
                    if (deterministicEpochActive)
                    {
                        ++deterministicTick;
                    }

                    if (options.common.frames > 0)
                    {
                        RuntimeFrameWaitRequest request;
                        const uint64 targetSequence =
                            ResolveFrameWaitTargetSequence(
                                publishedBeforeOuterTick,
                                diagnostics.lastPublishedFrameSequence);
                        request.minimumPublishedSequence = targetSequence;
                        request.minimumSubmittedSequence = targetSequence;
                        request.minimumPresentedSequence = targetSequence;
                        request.captureRequestId =
                            captureFrame && captureQueued ? CaptureRequestId : 0;
                        if (effectiveReadyWait)
                        {
                            const auto waitStart =
                                std::chrono::steady_clock::now();
                            if (waitStart >= readinessDeadline)
                            {
                                readinessTimedOut = true;
                                error = "Sample readiness wait expired";
                                if (!readinessReason.empty())
                                {
                                    error += ": " + readinessReason;
                                }
                                engine.RequestShutdown();
                                break;
                            }

                            const auto remainingTimeout =
                                std::chrono::duration_cast<
                                    std::chrono::milliseconds>(
                                    readinessDeadline - waitStart);
                            request.timeout = std::max(
                                remainingTimeout,
                                std::chrono::milliseconds(1));
                            const uint64 remainingTimeoutMilliseconds =
                                static_cast<uint64>(request.timeout.count());
                            const uint64 deadlineTickBudget =
                                remainingTimeoutMilliseconds >=
                                        static_cast<uint64>(
                                            std::numeric_limits<uint32>::max())
                                    ? static_cast<uint64>(
                                          std::numeric_limits<uint32>::max())
                                    : remainingTimeoutMilliseconds + 1u;
                            request.maxTicks = static_cast<uint32>(
                                std::max<uint64>(15000u,
                                                 deadlineTickBudget));
                        }
                        else
                        {
                            request.maxTicks = 15000;
                            request.timeout = std::chrono::milliseconds(15000);
                        }
                        // A finite sample can require more than one bounded
                        // upload-queue iteration before its first renderable
                        // frame exists. The driver runs full Engine ticks only
                        // until this frame is published, then keeps resources
                        // progressing without producing replacement frames.
                        // The armed qualification belongs to the just-issued
                        // target tick. If that outer Tick could not yet
                        // publish (for example while the pipeline is still
                        // pending), let the driver issue only the Engine
                        // ticks necessary to publish the exact target.
                        // SelectTickAction switches to Render-only progress
                        // immediately after target publication, so it cannot
                        // publish a replacement frame while waiting to
                        // submit/present the carrying target.
                        ConfigureFrameWaitPolicy(
                            deterministicEpochActive,
                            sampleDeltaTime,
                            request);
                        const RuntimeFrameWaitResult waitResult =
                            frameDriver.WaitFor(request);
                        diagnostics = waitResult.diagnostics;
                        if (qualificationTargetFrame && waitResult.Reached())
                        {
                            gpuSceneCullingQualificationTargetFrameSequence =
                                targetSequence;
                            gpuSceneCullingQualificationTargetPublishedSequence =
                                targetSequence;
                            gpuSceneCullingQualificationTargetSubmittedSequence =
                                targetSequence;
                            gpuSceneCullingQualificationTargetPresentedSequence =
                                targetSequence;
                            if (diagnostics.lastPublishedFrameSequence !=
                                    targetSequence ||
                                diagnostics.lastSubmittedFrameSequence !=
                                    targetSequence ||
                                diagnostics.lastPresentedFrameSequence !=
                                    targetSequence ||
                                (captureFrame && captureQueued &&
                                 diagnostics.lastCapture.frameSequence !=
                                     targetSequence))
                            {
                                error =
                                    "Qualification target did not retain exact presentation provenance";
                                engine.RequestShutdown();
                            }
                        }
                        if (exposureProbeTargetFrame && waitResult.Reached())
                        {
                            exposureQualificationProbeTargetFrameSequence =
                                targetSequence;
                            if (diagnostics.lastPublishedFrameSequence !=
                                    targetSequence ||
                                diagnostics.lastSubmittedFrameSequence !=
                                    targetSequence ||
                                diagnostics.lastPresentedFrameSequence !=
                                    targetSequence)
                            {
                                error =
                                    "Exposure qualification target did not retain exact presentation provenance";
                                engine.RequestShutdown();
                            }
                        }
                        if (ambientOcclusionProbeTargetFrame && waitResult.Reached())
                        {
                            ambientOcclusionQualificationProbeTargetFrameSequence =
                                targetSequence;
                            if (diagnostics.lastPublishedFrameSequence !=
                                    targetSequence ||
                                diagnostics.lastSubmittedFrameSequence !=
                                    targetSequence ||
                                diagnostics.lastPresentedFrameSequence !=
                                    targetSequence)
                            {
                                error =
                                    "Ambient-occlusion qualification target did not retain exact presentation provenance";
                                engine.RequestShutdown();
                            }
                        }
                        if (hiZReflectionProbeTargetFrame && waitResult.Reached())
                        {
                            const RenderHiZReflectionQualificationRequestResult
                                admission = render
                                    ->GetHiZReflectionQualificationProbeRequestResult();
                            if (!admission.IsAccepted())
                            {
                                hiZReflectionQualificationProbeRequestAccepted =
                                    false;
                                error = admission.diagnostic != nullptr &&
                                        admission.diagnostic[0] != '\0'
                                    ? admission.diagnostic
                                    : "Hi-Z reflection qualification probe did not reach Render-owner admission";
                                engine.RequestShutdown();
                            }
                            else if (admission.targetFrameSequence !=
                                     targetSequence)
                            {
                                hiZReflectionQualificationProbeRequestAccepted =
                                    false;
                                error =
                                    "Hi-Z reflection qualification probe bound a different target frame";
                                engine.RequestShutdown();
                            }
                            else if (diagnostics.lastPublishedFrameSequence !=
                                    targetSequence ||
                                diagnostics.lastSubmittedFrameSequence !=
                                    targetSequence ||
                                diagnostics.lastPresentedFrameSequence !=
                                    targetSequence)
                            {
                                error =
                                    "Hi-Z reflection qualification target did not retain exact presentation provenance";
                                engine.RequestShutdown();
                            }
                            else if (!hasHiZReflectionCopyPassForTarget(
                                         diagnostics, targetSequence))
                            {
                                error =
                                    "Hi-Z reflection qualification target did not register SSRQualificationRouteMaskCopy";
                                engine.RequestShutdown();
                            }
                            else
                            {
                                hiZReflectionQualificationProbeTargetFrameSequence =
                                    targetSequence;
                            }
                        }
                        if (toonOutlineProbeTargetFrame && waitResult.Reached())
                        {
                            toonOutlineQualificationProbeTargetFrameSequence =
                                targetSequence;
                            if (diagnostics.lastPublishedFrameSequence !=
                                    targetSequence ||
                                diagnostics.lastSubmittedFrameSequence !=
                                    targetSequence ||
                                diagnostics.lastPresentedFrameSequence !=
                                    targetSequence)
                            {
                                error =
                                    "Toon-outline qualification target did not retain exact presentation provenance";
                                engine.RequestShutdown();
                            }
                        }
                        if (captureFrame && captureQueued && waitResult.Reached())
                        {
                            captureCarryingFrameSequence =
                                diagnostics.lastCapture.frameSequence;
                            captureExecutionReceipt =
                                diagnostics.frameFeatures.executionReceipt;
                            captureConfigurationRevision =
                                captureExecutionReceipt.has_value() &&
                                    captureExecutionReceipt->available &&
                                    captureExecutionReceipt->frameIdentity
                                            .configurationRevision !=
                                        0
                                    ? captureExecutionReceipt->frameIdentity
                                          .configurationRevision
                                    : 0;
                            const EngineRenderRuntimeDiagnostics
                                captureEngineDiagnostics =
                                    engine.GetRenderRuntimeDiagnostics();
                            const SceneECS::SceneEcsDiagnosticsSnapshot
                                captureSceneDiagnostics =
                                    scene->GetDiagnosticsSnapshot();
                            captureSceneRuntimeId =
                                captureSceneDiagnostics.sceneRuntimeId
                                    .GetValue();
                            captureCameraEntityPacked =
                                captureEngineDiagnostics.cameraIdentity;
                            captureSceneRevision =
                                diagnostics.sceneValues.requiredSceneRevision;
                            captureSurfaceGeneration =
                                diagnostics.surfaceGeneration;
                            if (!options.common.screenshotPath.empty())
                            {
                                screenshotWritten = WriteSampleScreenshotPPM(
                                    diagnostics.lastCapture,
                                    options.common.screenshotPath,
                                    &error);
                            }
                            if (options.common.pixelProbeEnabled)
                            {
                                pixelProbeCaptured = diagnostics.lastCapture
                                                         .HasMatchingCompletedPixelProbe();
                            }
                        }
                        if (renderQualityBundleTargetFrame &&
                            waitResult.Reached())
                        {
                            const RenderExecutionReceipt& receipt =
                                diagnostics.frameFeatures.executionReceipt;
                            if (!receipt.available || !receipt.graphComposed ||
                                !receipt.executionSucceeded ||
                                !receipt.presented || !receipt.succeeded ||
                                !receipt.IsStructurallyValid() ||
                                receipt.frameIdentity.frameSequence !=
                                    targetSequence ||
                                diagnostics.lastSubmittedFrameSequence !=
                                    targetSequence ||
                                diagnostics.lastPresentedFrameSequence !=
                                    targetSequence)
                            {
                                error =
                                    "GFX24 bundle target did not retain an exact successful presentation receipt";
                                engine.RequestShutdown();
                            }
                            else
                            {
                                const SampleRenderDiagnostics
                                    completedFrameDiagnostics =
                                        MakeSampleRenderDiagnostics(
                                            diagnostics, engine);
                                renderQualityBundleRenderPath =
                                    ResolveActualRenderPath(
                                        sceneOptions.renderPath,
                                        completedFrameDiagnostics);
                                renderQualityBundleInstancingMode =
                                    GetRenderInstancingModeName(
                                        engine.GetRenderFrameSettings()
                                            .instancingMode);
                                if (renderQualityBundleRenderPath ==
                                        "unavailable" ||
                                    renderQualityBundleInstancingMode ==
                                        "Invalid")
                                {
                                    error =
                                        "GFX24 bundle target lacks actual render-path or applied instancing evidence";
                                    engine.RequestShutdown();
                                }
                                renderQualityBundleReceipt = receipt;
                                renderQualityBundleSubmittedFrame =
                                    targetSequence;

                                // The capture is a completion-owned artifact,
                                // so its wait must share the same wall-clock
                                // budget as the surrounding readiness wait.
                                // A fixed iteration count made this path
                                // dependent on scheduler timing and could
                                // busy-poll long after the readiness budget
                                // had expired. PumpRenderProgressOnce already
                                // yields a bounded scheduling quantum; the
                                // deadline below is the single source of
                                // truth for how long the host may wait.
                                while (!engine.ShouldShutdown())
                                {
                                    const auto pollNow =
                                        std::chrono::steady_clock::now();
                                    if (pollNow >= readinessDeadline)
                                    {
                                        readinessTimedOut = true;
                                        error =
                                            "GFX24 bundle capture exceeded the sample readiness deadline (state=" +
                                            std::to_string(static_cast<uint32>(
                                                diagnostics
                                                    .qualificationCapture
                                                    .capture.state)) +
                                            ", requestPending=" +
                                            (diagnostics
                                                     .qualificationCapture
                                                     .requestPending
                                                 ? "true"
                                                 : "false") +
                                            ")";
                                        engine.RequestShutdown();
                                        break;
                                    }

                                    const RenderQualificationCaptureDiagnostics&
                                        qualification =
                                        diagnostics.qualificationCapture;
                                    if (qualification.capture.state ==
                                            QualificationCaptureState::Completed &&
                                        qualification.completedBundle)
                                    {
                                        const QualificationCaptureBundle& bundle =
                                            *qualification.completedBundle;
                                        if (bundle.identity.frameIdentity !=
                                                receipt.frameIdentity ||
                                            bundle.identity.viewIdentity !=
                                                receipt.viewIdentity ||
                                            !bundle.identity.IsValid())
                                        {
                                            error =
                                                "Completed GFX24 bundle identity does not match its frozen presentation receipt";
                                            engine.RequestShutdown();
                                        }
                                        else
                                        {
                                            renderQualityCaptureBundle =
                                                qualification.completedBundle;
                                            renderQualityBundleCompletedFrame =
                                                targetSequence;
                                            renderQualityBundleCompleted = true;
                                        }
                                        break;
                                    }
                                    if (qualification.capture.state ==
                                        QualificationCaptureState::Rejected)
                                    {
                                        error =
                                            "GFX24 bundle capture failed: " +
                                            std::string(
                                                GetQualificationCaptureFailureName(
                                                    qualification.capture.failure));
                                        engine.RequestShutdown();
                                        break;
                                    }
                                    diagnostics =
                                        frameDriver.PumpRenderProgressOnce();
                                }
                                if (!renderQualityBundleCompleted &&
                                    error.empty())
                                {
                                    error =
                                        "GFX24 bundle capture did not reach exact GPU completion";
                                    engine.RequestShutdown();
                                }
                                if (renderQualityBundleCompleted &&
                                    renderQualityCaptureBundle &&
                                    renderQualityBundleReceipt.has_value())
                                {
                                    RenderQualityQualificationWriteRequest
                                        writeRequest;
                                    const uint64 renderQualityBundleCaptureTick =
                                        renderQualityAttachmentSeriesRequested
                                            ? manifest->captureTicks[
                                                  renderQualityAttachmentSeriesNextTick]
                                            : options.captureTick;
                                    std::string renderQualityBundleDirectory;
                                    if (renderQualityAttachmentSeriesRequested)
                                    {
                                        const std::string tickText =
                                            std::to_string(
                                                renderQualityBundleCaptureTick);
                                        renderQualityBundleDirectory = "tick-" +
                                            std::string(
                                                tickText.size() < 6U
                                                    ? 6U - tickText.size()
                                                    : 0U,
                                                '0') +
                                            tickText;
                                        const std::filesystem::path seriesRoot =
                                            options.qualificationSeriesPath
                                                    .has_parent_path()
                                                ? options.qualificationSeriesPath
                                                      .parent_path()
                                                : std::filesystem::path(".");
                                        writeRequest.outputRoot = seriesRoot /
                                            renderQualityBundleDirectory;
                                    }
                                    else
                                    {
                                        writeRequest.outputRoot =
                                            options.qualificationBundlePath
                                                    .has_parent_path()
                                                ? options.qualificationBundlePath
                                                      .parent_path()
                                                : std::filesystem::path(".");
                                    }
                                    writeRequest.sampleId = registeredInfo->id;
                                    writeRequest.cameraBookmarkId =
                                        options.viewBookmarkId;
                                    writeRequest.runId =
                                        renderQualityAttachmentSeriesRequested
                                            ? renderQualityAttachmentSeriesRunId
                                            : ReadEnvironmentString(
                                                  "RVX_VALIDATION_RUN_ID");
                                    writeRequest.renderPath =
                                        renderQualityBundleRenderPath;
                                    writeRequest.instancingMode =
                                        renderQualityBundleInstancingMode;
                                    writeRequest.captureTick =
                                        renderQualityBundleCaptureTick;
                                    writeRequest.visualContractVersion =
                                        manifest->visualContractVersion;
                                    writeRequest.submittedFrame =
                                        renderQualityBundleSubmittedFrame;
                                    writeRequest.completedFrame =
                                        renderQualityBundleCompletedFrame;

                                    RenderQualityQualificationWriteResult
                                        writeResult;
                                    if (!WriteRenderQualityQualificationBundle(
                                            writeRequest,
                                            *renderQualityCaptureBundle,
                                            *renderQualityBundleReceipt,
                                            writeResult,
                                            &error))
                                    {
                                        if (error.empty())
                                        {
                                            error =
                                                "Failed to publish the completed GFX24 qualification bundle";
                                        }
                                        engine.RequestShutdown();
                                    }
                                    else
                                    {
                                        std::error_code expectedError;
                                        std::error_code actualError;
                                        const std::filesystem::path expectedOutput =
                                            renderQualityAttachmentSeriesRequested
                                                ? ((options.qualificationSeriesPath
                                                            .has_parent_path()
                                                        ? options
                                                              .qualificationSeriesPath
                                                              .parent_path()
                                                        : std::filesystem::path(".")) /
                                                   renderQualityBundleDirectory /
                                                   "bundle.json")
                                                : options.qualificationBundlePath;
                                        const std::filesystem::path expected =
                                            std::filesystem::weakly_canonical(
                                                expectedOutput, expectedError);
                                        const std::filesystem::path actual =
                                            std::filesystem::weakly_canonical(
                                                writeResult.bundlePath,
                                                actualError);
                                        if (expectedError || actualError ||
                                            expected != actual)
                                        {
                                            error =
                                                "GFX24 qualification writer published an unexpected manifest path";
                                            engine.RequestShutdown();
                                        }
                                        else
                                        {
                                            if (!renderQualityAttachmentSeriesRequested)
                                            {
                                                renderQualityBundleWritten = true;
                                            }
                                            else
                                            {
                                                const RenderExecutionReceipt
                                                    completedReceipt =
                                                        *renderQualityBundleReceipt;
                                                if (writeResult.bundleSha256.size() !=
                                                        64U ||
                                                    completedReceipt.build.sourceCommit.empty() ||
                                                    completedReceipt.build.preset.empty() ||
                                                    writeResult
                                                            .renderConfigurationIdentity
                                                            .renderConfigurationHash
                                                            .size() != 64U ||
                                                    GetSampleBackendName(
                                                        completedReceipt.backend) !=
                                                        renderQualityAttachmentSeriesHardware
                                                            .backend ||
                                                    (!renderQualityAttachmentSeriesSourceCommit
                                                          .empty() &&
                                                     (renderQualityAttachmentSeriesSourceCommit !=
                                                          completedReceipt.build.sourceCommit ||
                                                      renderQualityAttachmentSeriesPreset !=
                                                          completedReceipt.build.preset)) ||
                                                    (!renderQualityAttachmentSeriesConfiguration
                                                          .renderConfigurationHash
                                                          .empty() &&
                                                     renderQualityAttachmentSeriesConfiguration !=
                                                         writeResult
                                                             .renderConfigurationIdentity))
                                                {
                                                    error =
                                                        "GFX24 attachment series bundle does not share frozen build, runtime configuration, or hardware identity";
                                                    engine.RequestShutdown();
                                                }
                                                else
                                                {
                                                    renderQualityAttachmentSeriesSourceCommit =
                                                        completedReceipt.build.sourceCommit;
                                                    renderQualityAttachmentSeriesPreset =
                                                        completedReceipt.build.preset;
                                                    renderQualityAttachmentSeriesConfiguration =
                                                        writeResult
                                                            .renderConfigurationIdentity;
                                                    RenderQualityAttachmentSeriesTick seriesTick;
                                                    seriesTick.captureTick =
                                                        renderQualityBundleCaptureTick;
                                                    seriesTick.bundleId =
                                                        renderQualityCaptureBundle
                                                            ->identity.bundleId;
                                                    seriesTick.bundlePath =
                                                        renderQualityBundleDirectory +
                                                        "/bundle.json";
                                                    seriesTick.bundleSha256 =
                                                        writeResult.bundleSha256;
                                                    seriesTick
                                                        .receiptSha256 =
                                                        writeResult
                                                            .renderExecutionReceiptSha256;
                                                    seriesTick.frameSequence =
                                                        completedReceipt.frameIdentity
                                                            .frameSequence;
                                                    seriesTick.submittedFrame =
                                                        renderQualityBundleSubmittedFrame;
                                                    seriesTick.completedFrame =
                                                        renderQualityBundleCompletedFrame;
                                                    renderQualityAttachmentSeriesTicks
                                                        .push_back(std::move(seriesTick));
                                                    ++renderQualityAttachmentSeriesNextTick;
                                                    renderQualityBundleRequestAccepted = false;
                                                    renderQualityBundleCompleted = false;
                                                    renderQualityCaptureBundle.reset();
                                                    renderQualityBundleReceipt.reset();
                                                    renderQualityBundleSubmittedFrame = 0;
                                                    renderQualityBundleCompletedFrame = 0;
                                                    if (renderQualityAttachmentSeriesNextTick ==
                                                        manifest->captureTicks.size())
                                                    {
                                                        RenderQualityAttachmentSeriesWriteRequest
                                                            seriesRequest;
                                                        seriesRequest.outputPath =
                                                            options.qualificationSeriesPath;
                                                        seriesRequest.sourceCommit =
                                                            renderQualityAttachmentSeriesSourceCommit;
                                                        seriesRequest.runId =
                                                            renderQualityAttachmentSeriesRunId;
                                                        seriesRequest.preset =
                                                            renderQualityAttachmentSeriesPreset;
                                                        seriesRequest.qualificationIdentity = {
                                                            .sampleId = registeredInfo->id,
                                                            .pipeline = GetRenderPipelineRecipeName(
                                                                completedReceipt.resolvedRecipe),
                                                            .quality = GetRenderQualityTierName(
                                                                completedReceipt.quality),
                                                            .backend = GetSampleBackendName(
                                                                completedReceipt.backend),
                                                            .view = options.viewBookmarkId,
                                                            .visualContractVersion =
                                                                manifest->visualContractVersion};
                                                        seriesRequest.hardwareIdentity =
                                                            renderQualityAttachmentSeriesHardware;
                                                        seriesRequest
                                                            .renderConfigurationIdentity =
                                                            renderQualityAttachmentSeriesConfiguration;
                                                        seriesRequest.manifestSha256 = std::string(
                                                            m_registry.GetManifestContentSha256());
                                                        seriesRequest.ticks =
                                                            renderQualityAttachmentSeriesTicks;
                                                        RenderQualityAttachmentSeriesWriteResult
                                                            seriesResult;
                                                        if (!WriteRenderQualityAttachmentSeries(
                                                                seriesRequest,
                                                                seriesResult,
                                                                &error))
                                                        {
                                                            if (error.empty())
                                                            {
                                                                error =
                                                                    "Failed to publish the completed GFX24 attachment series";
                                                            }
                                                            engine.RequestShutdown();
                                                        }
                                                        else
                                                        {
                                                            std::error_code expectedSeriesError;
                                                            std::error_code actualSeriesError;
                                                            const std::filesystem::path expectedSeries =
                                                                std::filesystem::weakly_canonical(
                                                                    options
                                                                        .qualificationSeriesPath,
                                                                    expectedSeriesError);
                                                            const std::filesystem::path actualSeries =
                                                                std::filesystem::weakly_canonical(
                                                                    seriesResult.seriesPath,
                                                                    actualSeriesError);
                                                            if (expectedSeriesError ||
                                                                actualSeriesError ||
                                                                expectedSeries != actualSeries ||
                                                                seriesResult.seriesSha256.size() !=
                                                                    64U)
                                                            {
                                                                error =
                                                                    "GFX24 attachment series writer published an unexpected manifest path";
                                                                engine.RequestShutdown();
                                                            }
                                                            else
                                                            {
                                                                captureExecutionReceipt =
                                                                    completedReceipt;
                                                                captureCarryingFrameSequence =
                                                                    completedReceipt
                                                                        .frameIdentity
                                                                        .frameSequence;
                                                                captureSceneRuntimeId =
                                                                    completedReceipt
                                                                        .viewIdentity
                                                                        .sceneRuntimeId;
                                                                captureCameraEntityPacked =
                                                                    completedReceipt
                                                                        .viewIdentity
                                                                        .cameraEntityPacked;
                                                                captureSceneRevision =
                                                                    completedReceipt
                                                                        .frameIdentity
                                                                        .sceneRevision;
                                                                captureSurfaceGeneration =
                                                                    completedReceipt
                                                                        .frameIdentity
                                                                        .surfaceGeneration;
                                                                captureConfigurationRevision =
                                                                    completedReceipt
                                                                        .frameIdentity
                                                                        .configurationRevision;
                                                                renderQualityAttachmentSeriesWritten = true;
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        if (!waitResult.Reached() ||
                            (!options.common.screenshotPath.empty() &&
                             captureFrame && captureQueued &&
                             !screenshotWritten) ||
                            (options.common.pixelProbeEnabled && captureFrame &&
                             captureQueued && !pixelProbeCaptured))
                        {
                            if (error.empty())
                            {
                                error = captureFrame && captureQueued &&
                                                !diagnostics.lastCapture.message.empty()
                                            ? diagnostics.lastCapture.message
                                            : "Finite sample frame did not complete: " +
                                                  DescribeFrameWait(
                                                      waitResult,
                                                      targetSequence);
                            }
                            engine.RequestShutdown();
                        }
                    }

                    const SampleRenderDiagnostics frameReadinessDiagnostics =
                        MakeSampleRenderDiagnostics(
                            diagnostics,
                            engine);
                    sample->ObserveDiagnostics(
                        frameReadinessDiagnostics,
                        context.assessment);
                    sampleReadiness = sample->GetReadiness(
                        frameReadinessDiagnostics);
                    sampleReady = sampleReadiness.IsReady();
                    readinessReason = sampleReady
                                          ? std::string{}
                                          : sampleReadiness.reason;
                    if (sampleReadiness.IsFailed())
                    {
                        readinessFailed = true;
                        error = readinessReason.empty()
                                    ? "Sample asynchronous asset activation failed"
                                    : readinessReason;
                        engine.RequestShutdown();
                    }
                    if (sampleReady &&
                        !firstModelFramePresentedRecorded &&
                        diagnostics.lastPresentedFrameSequence > 0)
                    {
                        Diagnostics::RecordTraceInstant(
                            startupTraceContext,
                            "FirstModelFramePresented",
                            {{"frameSequence",
                              diagnostics.lastPresentedFrameSequence}});
                        firstModelFramePresentedRecorded = true;
                    }
                    if (sampleReady &&
                        diagnostics.lastPresentedFrameSequence > 0 &&
                        !assessmentStableRecorded)
                    {
                        if (!assessmentActionAppliedRecorded)
                        {
                            static_cast<void>(assessmentSession.RecordCheckpoint(
                                AssessmentCheckpoints::ActionApplied));
                            assessmentActionAppliedRecorded = true;
                        }
                        static_cast<void>(assessmentSession.RecordCheckpoint(
                            AssessmentCheckpoints::ScenarioStable));
                        static_cast<void>(RecordRuntimeAssessmentSnapshot(
                            assessmentSession,
                            AssessmentCheckpoints::ScenarioStable,
                            scene,
                            &resourceDiagnosticsView,
                            runtimeServices,
                            &diagnostics,
                            &engine,
                            1,
                            requireGpuTiming));
                        assessmentStableRecorded = true;
                    }

                    if (sampleReady && lifetimeQualification)
                    {
                        if (!lifetimeReadyStarted)
                        {
                            lifetimeReadyStart =
                                std::chrono::steady_clock::now();
                            lifetimeReadyStarted = true;
                        }

                        if (resizePending &&
                            diagnostics.surfaceWidth == pendingResizeWidth &&
                            diagnostics.surfaceHeight == pendingResizeHeight)
                        {
                            // The post-wait diagnostics already contain the
                            // renderer-accepted extent, while the ordinary
                            // sample viewport callback runs at the start of
                            // the next outer tick. Begin lifetime settling
                            // now so this first resized frame is never checked
                            // against the prior extent's allocation plateau.
                            lifetimeQualification->NotifyResize();
                            resizePending = false;
                        }

                        const uint64 elapsedMs = static_cast<uint64>(
                            std::chrono::duration_cast<
                                std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() -
                                lifetimeReadyStart)
                                .count());
                        lifetimeQualification->Observe(
                            diagnostics,
                            CaptureSampleProcessMemory(),
                            elapsedMs);

                        // A qualification resize may leave the native surface
                        // at its alternate extent when the observation window
                        // completes. Restore the requested baseline before
                        // queuing the final capture so capture metadata, the
                        // swap chain, and the sample camera agree on one extent.
                        if (lifetimeQualification->IsComplete() &&
                            !lifetimeQualification->HasFailed() &&
                            !options.common.screenshotPath.empty() &&
                            !resizePending &&
                            (diagnostics.surfaceWidth != options.common.width ||
                             diagnostics.surfaceHeight != options.common.height))
                        {
                            pendingResizeWidth = options.common.width;
                            pendingResizeHeight = options.common.height;
                            if (!window->RequestResize(pendingResizeWidth,
                                                       pendingResizeHeight))
                            {
                                lifetimeQualification->Fail(
                                    "Failed to restore the capture surface extent");
                                error =
                                    "Failed to restore the capture surface extent";
                                engine.RequestShutdown();
                            }
                            else
                            {
                                resizePending = true;
                            }
                        }

                        if (lifetimeQualification->IsComplete() &&
                            !lifetimeQualification->HasFailed() &&
                            !options.common.screenshotPath.empty() &&
                            !qualificationCapturePrepared &&
                            !resizePending &&
                            diagnostics.surfaceWidth == options.common.width &&
                            diagnostics.surfaceHeight == options.common.height)
                        {
                            if (!sample->PrepareQualificationCapture(
                                    context, error))
                            {
                                if (error.empty())
                                {
                                    error =
                                        "Sample failed to prepare its deterministic qualification capture";
                                }
                                lifetimeQualification->Fail(error);
                                engine.RequestShutdown();
                            }
                            else
                            {
                                qualificationCapturePrepared = true;
                            }
                        }

                        if (lifetimeQualification->HasFailed())
                        {
                            const auto& failures =
                                lifetimeQualification->GetReport().failures;
                            error = failures.empty()
                                        ? "Model Viewer lifetime qualification failed"
                                        : failures.front();
                            engine.RequestShutdown();
                        }
                        else if (!resizePending &&
                                 lifetimeQualification->ShouldRequestResize())
                        {
                            const uint32 alternateWidth =
                                options.common.width + 32u;
                            const uint32 alternateHeight =
                                options.common.height + 18u;
                            pendingResizeWidth = resizeToAlternateExtent
                                                     ? alternateWidth
                                                     : options.common.width;
                            pendingResizeHeight = resizeToAlternateExtent
                                                      ? alternateHeight
                                                      : options.common.height;
                            resizeToAlternateExtent =
                                !resizeToAlternateExtent;
                            if (!window->RequestResize(pendingResizeWidth,
                                                       pendingResizeHeight))
                            {
                                lifetimeQualification->Fail(
                                    "Formal native window resize request failed");
                                error =
                                    "Formal native window resize request failed";
                                engine.RequestShutdown();
                            }
                            else
                            {
                                resizePending = true;
                            }
                        }
                    }

                    if (!options.qualificationPerformancePath.empty() &&
                        !renderQualityPerformanceRequestAccepted &&
                        HasMissedRenderQualityPerformanceSessionStart(
                            sampleReady,
                            deterministicEpochActive,
                            deterministicTick,
                            options.captureTick))
                    {
                        error = sampleReady
                            ? "GFX24 performance session missed its declared deterministic session-start tick"
                            : "GFX24 performance sample was not Ready at its declared deterministic session-start tick";
                        engine.RequestShutdown();
                        break;
                    }

                    if (!options.qualificationPerformancePath.empty() &&
                        !renderQualityPerformanceRequestAccepted &&
                        ShouldArmRenderQualityPerformanceSession(
                            sampleReady,
                            deterministicEpochActive,
                            deterministicTick,
                            options.captureTick))
                    {
                        std::error_code outputExistsError;
                        if (std::filesystem::exists(
                                options.qualificationPerformancePath,
                                outputExistsError) ||
                            outputExistsError)
                        {
                            error =
                                "GFX24 performance output already exists or cannot be inspected";
                            engine.RequestShutdown();
                            break;
                        }

                        renderQualityPerformanceHostProvenance.runId =
                            ReadEnvironmentString("RVX_VALIDATION_RUN_ID");
                        renderQualityPerformanceHostProvenance.adapterName =
                            diagnostics.adapterName;
                        renderQualityPerformanceHostProvenance.driverVersion =
                            diagnostics.driverVersion;
                        renderQualityPerformanceHostProvenance.osBuild =
                            options.qualificationPerformanceOsBuild;
                        renderQualityPerformanceHostProvenance.capabilityHash =
                            diagnostics.capabilityReportSha256;
                        if (renderQualityPerformanceHostProvenance.runId.empty() ||
                            renderQualityPerformanceHostProvenance.adapterName.empty() ||
                            renderQualityPerformanceHostProvenance.driverVersion.empty() ||
                            renderQualityPerformanceHostProvenance.osBuild.empty() ||
                            renderQualityPerformanceHostProvenance.capabilityHash.empty())
                        {
                            error =
                                "GFX24 performance session cannot freeze complete host provenance";
                            engine.RequestShutdown();
                            break;
                        }
                        renderQualityPerformanceRequestAccepted =
                            render->RequestRenderQualityPerformanceSession();
                        if (!renderQualityPerformanceRequestAccepted)
                        {
                            error =
                                "Render runtime rejected the GFX24 performance session request";
                            engine.RequestShutdown();
                            break;
                        }
                        // The scene readiness timeout and this fixed
                        // completion-owned run are independent budgets.  A
                        // slow but valid resource activation must not consume
                        // the time reserved for its 360 session submissions
                        // and terminal GPU drain.
                        renderQualityPerformanceDeadline =
                            std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(
                                readinessWaitBudget.timeoutMs);
                        // The request is consumed by the Render owner. Begin
                        // regular exact Graphics submissions on the next host
                        // iteration so no pre-session frame is counted.
                        continue;
                    }

                    if (ShouldDeferGenericFiniteCompletion(
                            renderQualityPerformanceRequestAccepted))
                    {
                        // Performance owns its exact 60 + 300 submission
                        // lifetime.  The generic finite-frame completion
                        // policy below would otherwise see its one-frame
                        // host sentinel as complete after the first measured
                        // submission and tear down the session before its
                        // Render-owned evidence can drain.
                        continue;
                    }

                    // A drain-capable sample has proved all ordinary scene,
                    // resource, and upload conditions on a completed frame.
                    // Do not let another normal host tick manufacture a new
                    // submission-retirement entry before the final capture
                    // frame is queued.
                    const bool finalDrainQualificationReady =
                        lifetimeQualification == nullptr &&
                        CanBeginFinalRenderDrain(
                            executedFrames,
                            options.common.frames,
                            readinessWaitBudget.maximumFrames);
                    if (effectiveReadyWait && !sampleReady &&
                        finalDrainQualificationReady &&
                        std::chrono::steady_clock::now() < readinessDeadline &&
                        sample->ShouldBeginFinalRenderDrain(
                            frameReadinessDiagnostics))
                    {
                        finalRenderDrainRequested = true;
                        continue;
                    }

                    if (options.common.frames == 0)
                    {
                        continue;
                    }
                    if (!effectiveReadyWait &&
                        hasCompletedRequestedTicks())
                    {
                        engine.RequestShutdown();
                    }
                    else if (effectiveReadyWait)
                    {
                        const bool readyAndCaptured =
                            sampleReady &&
                            (options.common.screenshotPath.empty() ||
                             screenshotWritten) &&
                            (!options.common.pixelProbeEnabled ||
                             pixelProbeCaptured) &&
                            (options.qualificationBundlePath.empty() ||
                             renderQualityBundleWritten) &&
                            (options.qualificationSeriesPath.empty() ||
                             renderQualityAttachmentSeriesWritten) &&
                            (!qualificationEnabled ||
                             (gpuSceneCullingQualificationTargetFrameSequence != 0 &&
                              hasQualificationComparison(
                                  frameReadinessDiagnostics) &&
                              qualificationMatchesTargetFrame(
                                  frameReadinessDiagnostics,
                                  gpuSceneCullingQualificationTargetFrameSequence))) &&
                            (!exposureQualificationProbeEnabled ||
                             (exposureQualificationProbeTargetFrameSequence != 0 &&
                              hasExposureProbeForTarget(
                                  diagnostics,
                                  exposureQualificationProbeTargetFrameSequence))) &&
                            (!ambientOcclusionQualificationProbeEnabled ||
                             (ambientOcclusionQualificationProbeTargetFrameSequence != 0 &&
                              hasAmbientOcclusionProbeForTarget(
                                  diagnostics,
                                  ambientOcclusionQualificationProbeTargetFrameSequence))) &&
                            (!hiZReflectionQualificationProbeEnabled ||
                             (hiZReflectionQualificationProbeTargetFrameSequence != 0 &&
                              hasHiZReflectionProbeForTarget(
                                  diagnostics,
                                  hiZReflectionQualificationProbeTargetFrameSequence))) &&
                            (!toonOutlineQualificationProbeEnabled ||
                             (toonOutlineQualificationProbeTargetFrameSequence != 0 &&
                              hasToonOutlineProbeForTarget(
                                  diagnostics,
                                  toonOutlineQualificationProbeTargetFrameSequence)));
                        const bool requestedRunComplete =
                            lifetimeQualification
                                ? lifetimeQualification->IsComplete() &&
                                      !lifetimeQualification->HasFailed()
                                : hasCompletedRequestedTicks();
                        if (requestedRunComplete &&
                            readyAndCaptured)
                        {
                            engine.RequestShutdown();
                        }
                        else if (!sampleReady &&
                                 (executedFrames >=
                                      readinessWaitBudget.maximumFrames ||
                                  std::chrono::steady_clock::now() >=
                                      readinessDeadline))
                        {
                            readinessTimedOut = true;
                            error = "Sample readiness wait expired";
                            if (!readinessReason.empty())
                            {
                                error += ": " + readinessReason;
                            }
                            engine.RequestShutdown();
                        }
                    }
                }
            }

            // Capture the completed scenario while Render is still live.
            // Teardown below publishes the empty-scene revision before the
            // completion-owned render runtime is drained.
            diagnostics = render->GetDiagnosticsSnapshot();

            SampleRenderDiagnostics sampleRenderDiagnostics =
                MakeSampleRenderDiagnostics(
                    diagnostics,
                    engine);
            if (qualificationEnabled)
            {
                if (directRasterReadbackQualificationEnabled)
                {
                    sampleRenderDiagnostics
                        .directOpaqueRasterReadbackQualificationTargetFrameSequence =
                        gpuSceneCullingQualificationTargetFrameSequence;
                    sampleRenderDiagnostics
                        .directOpaqueRasterReadbackQualificationTargetPublishedSequence =
                        gpuSceneCullingQualificationTargetPublishedSequence;
                    sampleRenderDiagnostics
                        .directOpaqueRasterReadbackQualificationTargetSubmittedSequence =
                        gpuSceneCullingQualificationTargetSubmittedSequence;
                    sampleRenderDiagnostics
                        .directOpaqueRasterReadbackQualificationTargetPresentedSequence =
                        gpuSceneCullingQualificationTargetPresentedSequence;
                }
                else
                {
                    sampleRenderDiagnostics.gpuSceneQualificationTargetFrameSequence =
                        gpuSceneCullingQualificationTargetFrameSequence;
                    sampleRenderDiagnostics.gpuSceneQualificationTargetPublishedSequence =
                        gpuSceneCullingQualificationTargetPublishedSequence;
                    sampleRenderDiagnostics.gpuSceneQualificationTargetSubmittedSequence =
                        gpuSceneCullingQualificationTargetSubmittedSequence;
                    sampleRenderDiagnostics.gpuSceneQualificationTargetPresentedSequence =
                        gpuSceneCullingQualificationTargetPresentedSequence;
                }
            }
            if (setupSucceeded)
            {
                sample->ObserveDiagnostics(
                    sampleRenderDiagnostics,
                    context.assessment);
            }
            const RenderFrameFeatureDiagnostics& features =
                diagnostics.frameFeatures;
            sampleReadiness = setupSucceeded
                                  ? sample->GetReadiness(
                                        sampleRenderDiagnostics)
                                  : SampleReadiness::Failed(
                                        error.empty()
                                            ? "Sample setup failed"
                                            : error);
            sampleReady = setupSucceeded && sampleReadiness.IsReady();
            readinessReason = sampleReady
                                  ? std::string{}
                                  : sampleReadiness.reason;
            if (sampleReadiness.IsFailed() && error.empty())
            {
                error = readinessReason.empty()
                            ? "Sample asynchronous asset activation failed"
                            : readinessReason;
            }
            std::string validationError;
            const bool sampleResultValid =
                setupSucceeded && sampleReady &&
                sample->ValidateResult(sampleRenderDiagnostics,
                                       validationError);
            sampleValidationFailed =
                setupSucceeded && sampleReady && !sampleResultValid;
            if (!sampleResultValid && error.empty())
            {
                error = validationError.empty()
                            ? "Sample-specific completion contract failed"
                            : validationError;
            }
            bool succeeded = error.empty() && sampleResultValid &&
                              diagnostics.backend != RHIBackendType::None &&
                              diagnostics.lastPresentedFrameSequence > 0 &&
                             features.available && features.renderAttempted &&
                             features.rendered && features.graphBuilt &&
                             features.graphCompiled &&
                             features.executionReceipt.available &&
                             features.executionReceipt.presented &&
                             features.executionReceipt.succeeded;
            if (lifetimeQualification)
            {
                if (!lifetimeQualification->IsComplete())
                {
                    lifetimeQualification->Fail(
                        error.empty()
                            ? "Lifetime qualification ended before completion"
                            : error);
                }
                succeeded &= lifetimeQualification->GetReport().pass;
            }
            if (options.common.backend != RHIBackendType::Auto)
            {
                succeeded &= diagnostics.backend == options.common.backend;
            }
            if (!options.common.screenshotPath.empty())
            {
                succeeded &= screenshotWritten;
            }
            if (options.common.pixelProbeEnabled)
            {
                succeeded &= pixelProbeCaptured &&
                             diagnostics.lastCapture
                                 .HasMatchingCompletedPixelProbe();
                if (!pixelProbeCaptured && error.empty())
                {
                    error = diagnostics.lastCapture.pixelProbe.message.empty()
                                ? "Pixel probe did not complete for the final capture frame"
                                : diagnostics.lastCapture.pixelProbe.message;
                }
            }
            if (qualificationEnabled)
            {
                const bool qualificationCompared =
                    hasQualificationComparison(sampleRenderDiagnostics);
                const bool qualificationMatched =
                    hasQualificationMatch(sampleRenderDiagnostics);
                succeeded &= gpuSceneCullingQualificationRequestAccepted &&
                             gpuSceneCullingQualificationTargetFrameSequence != 0 &&
                             gpuSceneCullingQualificationTargetPublishedSequence ==
                                 gpuSceneCullingQualificationTargetFrameSequence &&
                             gpuSceneCullingQualificationTargetSubmittedSequence ==
                                 gpuSceneCullingQualificationTargetFrameSequence &&
                             gpuSceneCullingQualificationTargetPresentedSequence ==
                                 gpuSceneCullingQualificationTargetFrameSequence &&
                             qualificationMatchesTargetFrame(
                                 sampleRenderDiagnostics,
                                 gpuSceneCullingQualificationTargetFrameSequence) &&
                             qualificationCompared && qualificationMatched;
                if (!gpuSceneCullingQualificationRequestAccepted &&
                    error.empty())
                {
                    error = directRasterReadbackQualificationEnabled
                        ? "Render runtime rejected the explicit Direct Opaque raster readback qualification request"
                        : "Render runtime rejected the explicit GPUScene culling qualification request";
                }
                else if (!qualificationCompared && error.empty())
                {
                    error = directRasterReadbackQualificationEnabled
                        ? "Direct Opaque raster readback qualification did not reach a post-fence comparison"
                        : "GPUScene culling qualification did not reach a post-fence comparison";
                }
                else if (!qualificationMatched && error.empty())
                {
                    error = directRasterReadbackQualificationEnabled
                        ? "Direct Opaque raster readback qualification reported a value mismatch"
                        : "GPUScene culling qualification reported a value mismatch";
                }
            }
            if (exposureQualificationProbeEnabled)
            {
                const bool exposureProbeCompleted =
                    exposureQualificationProbeTargetFrameSequence != 0 &&
                    hasExposureProbeForTarget(
                        diagnostics,
                        exposureQualificationProbeTargetFrameSequence) &&
                    captureExecutionReceipt.has_value() &&
                    captureExecutionReceipt->frameIdentity.frameSequence ==
                        exposureQualificationProbeTargetFrameSequence &&
                    captureExecutionReceipt->postProcess.exposureProbeRequested &&
                    captureExecutionReceipt->postProcess.exposureProbeReady;
                succeeded &= exposureQualificationProbeRequestAccepted &&
                             exposureProbeCompleted;
                if (!exposureQualificationProbeRequestAccepted && error.empty())
                {
                    error =
                        "Render runtime rejected the explicit exposure qualification probe";
                }
                else if (!exposureProbeCompleted && error.empty())
                {
                    error =
                        "Exposure qualification probe did not complete for the capture frame";
                }
            }
            if (ambientOcclusionQualificationProbeEnabled)
            {
                const bool ambientOcclusionProbeCompleted =
                    ambientOcclusionQualificationProbeTargetFrameSequence != 0 &&
                    hasAmbientOcclusionProbeForTarget(
                        diagnostics,
                        ambientOcclusionQualificationProbeTargetFrameSequence) &&
                    captureExecutionReceipt.has_value() &&
                    captureExecutionReceipt->frameIdentity.frameSequence ==
                        ambientOcclusionQualificationProbeTargetFrameSequence &&
                    captureExecutionReceipt->ambientOcclusion.probeRequested &&
                    captureExecutionReceipt->ambientOcclusion.probeReady;
                succeeded &= ambientOcclusionQualificationProbeRequestAccepted &&
                             ambientOcclusionProbeCompleted;
                if (!ambientOcclusionQualificationProbeRequestAccepted &&
                    error.empty())
                {
                    error =
                        "Render runtime rejected the explicit ambient-occlusion qualification probe";
                }
                else if (!ambientOcclusionProbeCompleted && error.empty())
                {
                    error =
                        "Ambient-occlusion qualification probe did not complete for the capture frame";
                }
            }
            if (hiZReflectionQualificationProbeEnabled)
            {
                const bool hiZReflectionProbeCompleted =
                    hiZReflectionQualificationProbeTargetFrameSequence != 0 &&
                    hasHiZReflectionProbeForTarget(
                        diagnostics,
                        hiZReflectionQualificationProbeTargetFrameSequence) &&
                    captureExecutionReceipt.has_value() &&
                    captureExecutionReceipt->frameIdentity.frameSequence ==
                        hiZReflectionQualificationProbeTargetFrameSequence;
                succeeded &= hiZReflectionQualificationProbeRequestAccepted &&
                             hiZReflectionProbeCompleted;
                if (!hiZReflectionQualificationProbeRequestAccepted &&
                    error.empty())
                {
                    error =
                        "Render runtime rejected the explicit Hi-Z reflection qualification probe";
                }
                else if (!hiZReflectionProbeCompleted && error.empty())
                {
                    error =
                        "Hi-Z reflection qualification probe did not complete for the capture frame";
                }
            }
            if (toonOutlineQualificationProbeEnabled)
            {
                const bool toonOutlineProbeCompleted =
                    toonOutlineQualificationProbeTargetFrameSequence != 0 &&
                    hasToonOutlineProbeForTarget(
                        diagnostics,
                        toonOutlineQualificationProbeTargetFrameSequence) &&
                    captureExecutionReceipt.has_value() &&
                    captureExecutionReceipt->frameIdentity.frameSequence ==
                        toonOutlineQualificationProbeTargetFrameSequence &&
                    captureExecutionReceipt->toonLighting.IsExecutionReady() &&
                    captureExecutionReceipt->toonOutline.IsQualificationReady();
                succeeded &= toonOutlineQualificationProbeRequestAccepted &&
                             toonOutlineProbeCompleted;
                if (!toonOutlineQualificationProbeRequestAccepted &&
                    error.empty())
                {
                    error =
                        "Render runtime rejected the explicit Toon-outline qualification probe";
                }
                else if (!toonOutlineProbeCompleted && error.empty())
                {
                    error =
                        "Toon-outline qualification probe did not complete for the capture frame";
                }
            }
            if (!succeeded && error.empty())
            {
                error = "Sample did not produce a presented, compiled render frame";
            }

            if (!setupSucceeded)
            {
                static_cast<void>(assessmentSession.RecordFinding(
                    MakeHostFinding(
                        "SAMPLE.SETUP.FAILED",
                        "SAMPLE",
                        "SAMPLE.SCENARIO.COMPLETED",
                        AssessmentCheckpoints::ScenarioSetup,
                        FindingClass::ContractViolation,
                        FindingSeverity::Error,
                        FindingConfidence::Confirmed,
                        "Sample setup did not establish the requested scenario.",
                        "Setup completes and the scenario can enter its update loop.",
                        error.empty() ? "Setup returned false." : error,
                        true,
                        "The assessment scenario was not established.")));
            }
            if (readinessFailed)
            {
                static_cast<void>(assessmentSession.RecordFinding(
                    MakeHostFinding(
                        "SAMPLE.READINESS.FAILED",
                        "SAMPLE",
                        "SAMPLE.SCENARIO.COMPLETED",
                        AssessmentCheckpoints::ScenarioStable,
                        FindingClass::ContractViolation,
                        FindingSeverity::Error,
                        FindingConfidence::Confirmed,
                        "Sample readiness entered a terminal failure state.",
                        "The scenario reaches Ready with all required assets resident.",
                        readinessReason.empty() ? error : readinessReason,
                        true,
                        "A failed readiness state invalidates the qualification run.")));
            }
            if (readinessTimedOut)
            {
                static_cast<void>(assessmentSession.RecordFinding(
                    MakeHostFinding(
                        "SAMPLE.READINESS.TIMEOUT",
                        "SAMPLE",
                        "SAMPLE.SCENARIO.COMPLETED",
                        AssessmentCheckpoints::ScenarioStable,
                        FindingClass::ContractViolation,
                        FindingSeverity::Error,
                        FindingConfidence::Confirmed,
                        "Sample readiness did not complete within the bounded run.",
                        "The scenario reaches Ready before its frame and time limits.",
                        error,
                        true,
                        "The requested assessment actions never reached a stable checkpoint.")));
            }
            if (sampleValidationFailed)
            {
                static_cast<void>(assessmentSession.RecordFinding(
                    MakeHostFinding(
                        "SAMPLE.VALIDATION.FAILED",
                        "SAMPLE",
                        "SAMPLE.SCENARIO.COMPLETED",
                        AssessmentCheckpoints::ScenarioStable,
                        FindingClass::ContractViolation,
                        FindingSeverity::Error,
                        FindingConfidence::Confirmed,
                        "The sample-specific completion contract failed.",
                        "ValidateResult accepts the fully resident, presented scenario.",
                        validationError.empty() ? error : validationError,
                        true,
                        "The scenario-specific architecture probe failed.")));
            }

            const AssetContentVerificationSummary assetContentVerification =
                BuildAssetContentVerificationSummary(
                    resolvedAssets,
                    models,
                    environments,
                    animations,
                    particleEffects,
                    assessmentSession);
            const auto primaryModelContentReceipt =
                GetModelContentVerificationReceipt(models, resolvedAssets.model);
            // Preserve the long-standing Model primary-asset contract, but do
            // not manufacture a Model identity for effect-only scenarios.
            // A mixed scenario keeps its selected Model as primary; otherwise
            // the explicitly selected effect becomes the report's primary
            // asset and is backed by its own owner-thread verification receipt.
            const ResolvedSampleAsset* primaryAsset =
                resolvedAssets.model.selected ? &resolvedAssets.model : nullptr;
            std::optional<Resource::ResourceContentVerificationReceipt>
                primaryAssetContentReceipt = primaryModelContentReceipt;
            if (primaryAsset == nullptr && !sceneOptions.effectAssetId.empty())
            {
                const auto selectedEffect = std::find_if(
                    resolvedAssets.effects.begin(),
                    resolvedAssets.effects.end(),
                    [&sceneOptions](const ResolvedSampleAsset& asset)
                    {
                        return asset.selected &&
                               asset.entry.id == sceneOptions.effectAssetId;
                    });
                if (selectedEffect != resolvedAssets.effects.end())
                {
                    primaryAsset = &*selectedEffect;
                    primaryAssetContentReceipt =
                        GetEffectContentVerificationReceipt(
                            particleEffects, *selectedEffect);
                }
            }
            const SceneECS::SceneEcsDiagnosticsSnapshot reportSceneDiagnostics =
                scene->GetDiagnosticsSnapshot();
            const WorldEcsRuntimeServicesDiagnostics reportServiceDiagnostics =
                runtimeServices->GetRuntimeDiagnostics();

            SampleReport report;
            // The report is refined by successive fail-closed conjunctions
            // below.  Start from the neutral value so a fully successful run
            // is not permanently forced to false by the default member value.
            report.pass = true;
            report.schemaVersion = options.deterministicContractActive
                                       ? RVX_SAMPLE_REPORT_SCHEMA_VERSION
                                       : (options.common.pixelProbeEnabled &&
                                                  !options.common
                                                       .directOpaqueRasterReadbackQualificationEnabled
                                              ? RVX_SAMPLE_REPORT_PIXEL_PROBE_SCHEMA_VERSION
                                              : RVX_SAMPLE_REPORT_PRE_VISUAL_CONTRACT_SCHEMA_VERSION);
            report.validationRunId =
                ReadEnvironmentString("RVX_VALIDATION_RUN_ID");
            report.scenario.contractRevision = 0;
            report.scenario.phase = "not-applicable";
            report.sampleName = registeredInfo->id;
            report.category = "visual";
            report.requestedBackend = options.common.backend;
            report.backend = diagnostics.backend;
            startupTimeline.SetMetadata(
                "backend",
                std::string(GetSampleBackendName(diagnostics.backend)));
            if (resourceSubsystem != nullptr)
            {
                const Resource::ModelTextureStreamingStats streaming =
                    resourceSubsystem->GetManager()
                        .GetModelTextureStreamingStats();
                startupTimeline.SetMetadata(
                    "modelTextureDecodedByteBudget",
                    streaming.decodedByteBudget);
                startupTimeline.SetMetadata(
                    "modelTexturePeakDecodedBytes",
                    streaming.peakReservedDecodedBytes);
                startupTimeline.SetMetadata(
                    "modelTextureReservedDecodedBytes",
                    streaming.reservedDecodedBytes);
                startupTimeline.SetMetadata(
                    "modelTextureCompletedDecodedBytes",
                    streaming.completedDecodedBytes);
                startupTimeline.SetMetadata(
                    "modelTextureMaxConcurrentDecodes",
                    static_cast<uint64>(streaming.maxConcurrentDecodes));
                startupTimeline.SetMetadata(
                    "modelTextureActiveDecodes",
                    static_cast<uint64>(streaming.activeDecodes));
                startupTimeline.SetMetadata(
                    "modelTexturePeakConcurrentDecodes",
                    static_cast<uint64>(streaming.peakActiveDecodes));
                startupTimeline.SetMetadata(
                    "modelTextureQueuedDecodes",
                    static_cast<uint64>(streaming.queuedDecodes));
                startupTimeline.SetMetadata(
                    "modelTexturePendingPublications",
                    static_cast<uint64>(streaming.pendingPublications));
                startupTimeline.SetMetadata(
                    "modelTextureCompletedDecodes",
                    streaming.completedDecodes);
                startupTimeline.SetMetadata(
                    "modelTextureFailedDecodes",
                    streaming.failedDecodes);
                startupTimeline.SetMetadata(
                    "modelTextureCancelledDecodes",
                    streaming.cancelledDecodes);
            }
            report.frameCount = executedFrames;
            report.submittedFrameSequence =
                diagnostics.lastSubmittedFrameSequence;
            report.presentedFrameSequence =
                diagnostics.lastPresentedFrameSequence;
            report.width = options.common.width;
            report.height = options.common.height;
            report.quality = options.common.quality;
            report.requestedRenderPath =
                GetSampleRenderPathName(sceneOptions.renderPath);
            // Retain the established field as a compatibility alias. It must
            // never be repurposed to report the observed execution path.
            report.renderPath = report.requestedRenderPath;
            report.actualRenderPath = ResolveActualRenderPath(
                sceneOptions.renderPath, sampleRenderDiagnostics);
            // Performance qualification is intentionally mutually exclusive
            // with screenshot, pixel-probe, and visual-bundle production. Its
            // own immutable report binds the manifest, fixed epoch, build,
            // backend, and every completion-owned execution-receipt digest.
            // Do not require a visual capture receipt after that report has
            // already published successfully.
            if (options.deterministicContractActive &&
                options.qualificationPerformancePath.empty())
            {
                SampleVisualContractReceipt receipt;
                receipt.manifestSchema =
                    std::string(RVX_SAMPLE_MANIFEST_SCHEMA);
                receipt.manifestSchemaVersion =
                    m_registry.GetManifestSchemaVersion();
                receipt.manifestContentSha256 =
                    std::string(m_registry.GetManifestContentSha256());
                receipt.visualContractVersion =
                    manifest->visualContractVersion;
                receipt.domain = manifest->domain;
                receipt.intent = manifest->intent;
                receipt.requestedPipeline = sceneOptions.pipelineRecipe;
                const RenderExecutionReceipt* executionReceipt =
                    captureExecutionReceipt.has_value()
                        ? &*captureExecutionReceipt
                        : nullptr;
                if (executionReceipt != nullptr)
                {
                    receipt.renderExecutionReceipt = *executionReceipt;
                }
                RenderPipelineRecipe requestedRenderRecipe{};
                RenderQualityTier requestedRenderQuality{};
                SamplePipelineRecipe observedSampleRecipe{};
                const bool requestedProfileMapped =
                    TryMapSamplePipelineRecipe(
                        sceneOptions.pipelineRecipe,
                        requestedRenderRecipe) &&
                    TryMapSampleQualityTier(
                        sceneOptions.qualityTier,
                        requestedRenderQuality);
                const bool observedRecipeMapped =
                    TryMapRenderPipelineRecipe(
                        executionReceipt != nullptr
                            ? executionReceipt->resolvedRecipe
                            : RenderPipelineRecipe::Count,
                        observedSampleRecipe);
                const bool validationBuildIdentityAccepted =
                    validationBuildIdentity.IsEmpty()
                        ? executionReceipt != nullptr &&
                              executionReceipt->build.IsEmpty()
                        : validationBuildIdentity.IsStructurallyValid() &&
                              executionReceipt != nullptr &&
                              executionReceipt->build ==
                                  validationBuildIdentity;
                const bool executionMatchesCapture =
                    requestedProfileMapped && observedRecipeMapped &&
                    executionReceipt != nullptr &&
                    executionReceipt->IsStructurallyValid() &&
                    executionReceipt->available &&
                    executionReceipt->graphComposed &&
                    executionReceipt->executionSucceeded &&
                    executionReceipt->presented &&
                    executionReceipt->succeeded &&
                    executionReceipt->frameIdentity.frameSequence ==
                        captureCarryingFrameSequence &&
                    executionReceipt->frameIdentity.sceneRevision ==
                        captureSceneRevision &&
                    executionReceipt->frameIdentity.surfaceGeneration ==
                        captureSurfaceGeneration &&
                    executionReceipt->frameIdentity.configurationRevision ==
                        captureConfigurationRevision &&
                    executionReceipt->viewIdentity.sceneRuntimeId ==
                        captureSceneRuntimeId &&
                    executionReceipt->viewIdentity.cameraEntityPacked ==
                        captureCameraEntityPacked &&
                    executionReceipt->viewIdentity.subViewIndex == 0 &&
                    executionReceipt->viewIdentity.role == ViewRole::Main &&
                    executionReceipt->backend == diagnostics.backend &&
                    executionReceipt->requestedRecipe ==
                        requestedRenderRecipe &&
                    executionReceipt->resolvedRecipe ==
                        requestedRenderRecipe &&
                    executionReceipt->quality == requestedRenderQuality &&
                    executionReceipt->profileHash ==
                        ComputeRenderPipelineProfileHash(
                            executionReceipt->resolvedRecipe,
                            requestedRenderQuality) &&
                    validationBuildIdentityAccepted;
                if (executionMatchesCapture)
                {
                    receipt.observedPipeline = observedSampleRecipe;
                }
                receipt.quality = options.qualityTier;
                receipt.cameraBookmarkId = options.viewBookmarkId;
                receipt.fixedTimeNumerator = options.fixedTimeNumerator;
                receipt.fixedTimeDenominator = options.fixedTimeDenominator;
                receipt.deterministicSeed = options.deterministicSeed;
                receipt.captureTick =
                    static_cast<uint32>(options.captureTick);
                receipt.captureCarryingFrameSequence =
                    captureCarryingFrameSequence;
                receipt.captureSceneRuntimeId = captureSceneRuntimeId;
                receipt.captureCameraEntityPacked =
                    captureCameraEntityPacked;
                receipt.captureSceneRevision = captureSceneRevision;
                receipt.captureSurfaceGeneration =
                    captureSurfaceGeneration;
                receipt.captureConfigurationRevision =
                    captureConfigurationRevision;
                receipt.deterministicQualification =
                    manifest->deterministicQualification;
                receipt.satisfied =
                    options.manifestContractResolved &&
                    options.viewBookmarkResolved &&
                    options.captureTickResolved &&
                    deterministicEpochActive && deterministicCameraApplied &&
                    deterministicTick >= options.captureTick &&
                    captureCarryingFrameSequence != 0 &&
                    captureConfigurationRevision != 0 &&
                    captureCarryingFrameSequence <=
                        diagnostics.lastPresentedFrameSequence &&
                    receipt.observedPipeline.has_value() &&
                    receipt.manifestContentSha256.size() == 64u &&
                    ((!options.common.screenshotPath.empty() &&
                      screenshotWritten) ||
                     (options.common.pixelProbeEnabled &&
                      pixelProbeCaptured) ||
                     renderQualityAttachmentSeriesWritten);
                if (!receipt.satisfied)
                {
                    succeeded = false;
                    if (error.empty())
                    {
                        error =
                            "The deterministic Sample visual contract did not produce exact execution evidence";
                    }
                }
                report.visualContractReceipt = std::move(receipt);
            }
            report.ecsDiagnostics = MakeEcsRuntimeDiagnostics(
                reportSceneDiagnostics, reportServiceDiagnostics);
            report.physicsDiagnosticsAvailable =
                reportServiceDiagnostics.available &&
                reportServiceDiagnostics.physicsInitialized;
            if (report.physicsDiagnosticsAvailable)
            {
                report.requestedPhysicsBackend = GetPhysicsBackendName(
                    reportServiceDiagnostics.requestedPhysicsBackend);
                report.actualPhysicsBackend = GetPhysicsBackendName(
                    reportServiceDiagnostics.activePhysicsBackend);
                report.physicsBackendFallbackActive =
                    reportServiceDiagnostics.physicsBackendFallbackActive;
            }
            report.diagnostics = options.common.diagnostics;
            report.screenshotPath = options.common.screenshotPath;
            if (primaryAsset != nullptr)
            {
                report.assetId = primaryAsset->entry.id;
                report.assetPath = primaryAsset->entry.resolvedPath;
                report.assetKind =
                    GetSampleAssetKindName(primaryAsset->entry.kind);
                report.catalogPath = primaryAsset->catalogPath;
                report.assetRoot = primaryAsset->assetRoot;
                report.assetLicenseSpdx =
                    primaryAsset->entry.license.spdxId;
                report.assetLicenseFile =
                    primaryAsset->entry.license.resolvedFile;
                report.assetSourceName = primaryAsset->entry.source.name;
                report.assetSourceUri = primaryAsset->entry.source.uri;
                report.assetAuthor = primaryAsset->entry.source.author;
                report.assetAttribution = primaryAsset->entry.attribution;
                report.assetRedistributable =
                    primaryAsset->entry.redistributable;
                report.assetLoaded =
                    primaryAssetContentReceipt.has_value() &&
                    primaryAssetContentReceipt->IsVerified();
            }
            report.readiness.waitRequested = effectiveReadyWait;
            report.readiness.ready = sampleReady;
            report.readiness.minimumFrames = options.common.frames;
            report.readiness.maximumFrames = effectiveReadyWait
                                                 ? readinessWaitBudget.maximumFrames
                                                 : options.common.frames;
            report.readiness.timeoutMs = effectiveReadyWait
                                             ? readinessWaitBudget.timeoutMs
                                             : 0;
            report.readiness.reason = readinessReason;
            report.runtimeComposition = runtimeComposition;
            if (resolvedAssets.model.selected)
            {
                report.assets.push_back(MakeReportAsset("model",
                                                        resolvedAssets.model,
                                                        primaryModelContentReceipt));
            }
            for (const ResolvedSampleAsset& additional :
                 resolvedAssets.additionalModels)
            {
                report.assets.push_back(MakeReportAsset(
                    "gallery-model",
                    additional,
                    GetModelContentVerificationReceipt(models, additional)));
            }
            if (resolvedAssets.environment.selected)
            {
                report.assets.push_back(MakeReportAsset(
                    "environment",
                    resolvedAssets.environment,
                    GetEnvironmentContentVerificationReceipt(
                        environments, resolvedAssets.environment)));
            }
            for (const ResolvedSampleAsset& animation :
                 resolvedAssets.animations)
            {
                report.assets.push_back(MakeReportAsset(
                    "animation",
                    animation,
                    GetAnimationContentVerificationReceipt(
                        animations, animation)));
            }
            for (const ResolvedSampleAsset& effect : resolvedAssets.effects)
            {
                report.assets.push_back(MakeReportAsset(
                    "effect", effect,
                    GetEffectContentVerificationReceipt(particleEffects, effect)));
            }
            report.renderDiagnostics = sampleRenderDiagnostics;
            if (options.common.pixelProbeEnabled)
            {
                report.pixelProbe =
                    MakeSamplePixelProbeReport(diagnostics.lastCapture);
            }
            report.pass = report.pass && succeeded;

            SampleFeatureReporter reporter(report);
            sample->AppendReport(reporter);
            AppendRuntimeReport(diagnostics, reporter);
            reporter.ResourceDiagnostic(
                "requested backend=" +
                std::string(GetSampleBackendName(options.common.backend)));
            reporter.ResourceDiagnostic(
                "realized backend=" +
                std::string(GetSampleBackendName(diagnostics.backend)));
            if (!error.empty())
            {
                reporter.ResourceDiagnostic("failure=" + error);
            }

            static_cast<void>(assessmentSession.RecordCheckpoint(
                AssessmentCheckpoints::TeardownBefore));
            static_cast<void>(RecordRuntimeAssessmentSnapshot(
                assessmentSession,
                AssessmentCheckpoints::TeardownBefore,
                scene,
                &resourceDiagnosticsView,
                runtimeServices,
                &diagnostics,
                &engine,
                1,
                requireGpuTiming));

            // Samples release their exact request refs before the Engine starts
            // the World-owned shutdown protocol.  The presentation camera stays
            // live until the composition has published every removal snapshot.
            sample->Shutdown(context);
            const bool baselineCameraRestored = cameras->Activate(engineBaselineCamera);
            static_cast<void>(sceneLifetime.RequestDestroyAll());
            sample.reset();

            const uint32 teardownPumpFrameLimit =
                std::max<uint32>(120u, readinessWaitBudget.maximumFrames);
            const auto teardownDeadline = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(std::max<uint32>(
                    5000u, readinessWaitBudget.timeoutMs));
            uint32 teardownPumpFrames = 0;
            // Activate writes the next ECS snapshot. Publish one removal-capable
            // frame before observing the selected camera or lifetime receipts.
            diagnostics = frameDriver.TickOnce(sampleDeltaTime);
            ++teardownPumpFrames;
            while (teardownPumpFrames < teardownPumpFrameLimit &&
                   std::chrono::steady_clock::now() < teardownDeadline)
            {
                sceneLifetime.Collect();
                const WorldEcsRuntimeServicesDiagnostics serviceDiagnostics =
                    runtimeServices->GetRuntimeDiagnostics();
                const bool requestsDrained =
                    serviceDiagnostics.trackedModelRequestCount == 0 &&
                    serviceDiagnostics.trackedEnvironmentRequestCount == 0 &&
                    serviceDiagnostics.trackedAnimationRequestCount == 0 &&
                    serviceDiagnostics.activeAnimationRequestCount == 0;
                if (!sceneLifetime.HasUnresolvedDestroyWork() && requestsDrained)
                {
                    break;
                }
                diagnostics = frameDriver.TickOnce(sampleDeltaTime);
                ++teardownPumpFrames;
            }

            sceneLifetime.Collect();
            const SampleSceneLifetimeDiagnostics scopeDiagnostics =
                sceneLifetime.GetDiagnostics();
            const SceneECS::SceneEcsDiagnosticsSnapshot postSceneDiagnostics =
                scene->GetDiagnosticsSnapshot();
            const WorldEcsRuntimeServicesDiagnostics serviceDiagnostics =
                runtimeServices->GetRuntimeDiagnostics();
            const bool requestsDrained =
                serviceDiagnostics.trackedModelRequestCount == 0 &&
                serviceDiagnostics.trackedEnvironmentRequestCount == 0 &&
                serviceDiagnostics.trackedAnimationRequestCount == 0 &&
                serviceDiagnostics.activeAnimationRequestCount == 0;
            const bool scopeDrained = !sceneLifetime.HasUnresolvedDestroyWork();

            static_cast<void>(assessmentSession.RecordCheckpoint(
                AssessmentCheckpoints::TeardownSceneComplete));
            static_cast<void>(RecordRuntimeAssessmentSnapshot(
                assessmentSession,
                AssessmentCheckpoints::TeardownSceneComplete,
                scene,
                &resourceDiagnosticsView,
                runtimeServices,
                &diagnostics,
                &engine,
                1,
                requireGpuTiming));
            if (!scopeDrained || !requestsDrained)
            {
                succeeded = false;
                static_cast<void>(assessmentSession.RecordFinding(MakeHostFinding(
                    "SAMPLE.ECS_SCOPE.TEARDOWN_INCOMPLETE",
                    "SAMPLE",
                    "ENGINE.TEARDOWN.CLEAN",
                    AssessmentCheckpoints::TeardownSceneComplete,
                    FindingClass::LifetimeLeak,
                    FindingSeverity::Error,
                    FindingConfidence::Confirmed,
                    "Sample ECS ownership did not quiesce before World shutdown.",
                    "Sample entities recycle and all Engine-owned request handles drain.",
                    "owned=" + std::to_string(scopeDiagnostics.ownedEntityCount) +
                        ", alive=" + std::to_string(scopeDiagnostics.aliveEntityCount) +
                        ", pending=" + std::to_string(scopeDiagnostics.pendingDestroyEntityCount) +
                        ", trackedModels=" + std::to_string(serviceDiagnostics.trackedModelRequestCount) +
                        ", trackedEnvironments=" + std::to_string(serviceDiagnostics.trackedEnvironmentRequestCount) +
                        ", trackedAnimations=" + std::to_string(serviceDiagnostics.trackedAnimationRequestCount),
                    true,
                    "World teardown would conceal unresolved ECS ownership.")));
            }
            if (!baselineCameraRestored || cameras->GetActiveCamera() != engineBaselineCamera)
            {
                succeeded = false;
                static_cast<void>(assessmentSession.RecordFinding(MakeHostFinding(
                    "SCENE.TEARDOWN.BASELINE_CAMERA_MISMATCH",
                    "SCENE",
                    "ENGINE.TEARDOWN.CLEAN",
                    AssessmentCheckpoints::TeardownSceneComplete,
                    FindingClass::StaleIdentity,
                    FindingSeverity::Error,
                    FindingConfidence::Confirmed,
                    "Sample teardown altered the host ECS camera selection.",
                    "The exact baseline camera remains selected.",
                    "restore=" + std::string(baselineCameraRestored ? "true" : "false"),
                    true,
                    "A sample changed the host-owned ECS camera state.")));
            }
            if (postSceneDiagnostics.entityCount != engineBaselineScene.entityCount ||
                postSceneDiagnostics.pendingDestroyCount != engineBaselineScene.pendingDestroyCount ||
                postSceneDiagnostics.cleanupRequiredCount != engineBaselineScene.cleanupRequiredCount ||
                postSceneDiagnostics.retiringCount != engineBaselineScene.retiringCount ||
                postSceneDiagnostics.recyclableCount != engineBaselineScene.recyclableCount)
            {
                succeeded = false;
                Finding finding = MakeHostFinding(
                    "SCENE.TEARDOWN.RESIDUAL_ENTITIES",
                    "SCENE",
                    "ENGINE.TEARDOWN.CLEAN",
                    AssessmentCheckpoints::TeardownSceneComplete,
                    FindingClass::LifetimeLeak,
                    FindingSeverity::Error,
                    FindingConfidence::Confirmed,
                    "ECS teardown retained sample-owned entities.",
                    "ECS lifecycle counts return to the baseline.",
                    "baselineEntities=" + std::to_string(engineBaselineScene.entityCount) +
                        ", observedEntities=" + std::to_string(postSceneDiagnostics.entityCount) +
                        ", pending=" + std::to_string(postSceneDiagnostics.pendingDestroyCount) +
                        ", cleanup=" + std::to_string(postSceneDiagnostics.cleanupRequiredCount) +
                        ", retiring=" + std::to_string(postSceneDiagnostics.retiringCount),
                    true,
                    "Sample ECS entities survived the teardown boundary.");
                finding.frameBegin = DiagnosticValue<uint64>::Available(executedFrames);
                finding.frameEnd = DiagnosticValue<uint64>::Available(engine.GetFrameNumber());
                finding.sceneRevision = DiagnosticValue<uint64>::Available(
                    postSceneDiagnostics.sceneSnapshotRevision);
                static_cast<void>(assessmentSession.RecordFinding(std::move(finding)));
            }

            engine.DestroyWorld(worldRequirements.world.name);
            uint32 worldShutdownPumpFrames = 0;
            while (engine.GetWorld(worldRequirements.world.name) != nullptr &&
                   worldShutdownPumpFrames < teardownPumpFrameLimit &&
                   std::chrono::steady_clock::now() < teardownDeadline)
            {
                diagnostics = frameDriver.TickOnce(sampleDeltaTime);
                ++worldShutdownPumpFrames;
            }
            const bool worldDestroyed =
                engine.GetWorld(worldRequirements.world.name) == nullptr;
            if (!worldDestroyed)
            {
                succeeded = false;
                static_cast<void>(assessmentSession.RecordFinding(MakeHostFinding(
                    "ENGINE.ECS_WORLD.TEARDOWN_INCOMPLETE",
                    "ENGINE",
                    "ENGINE.TEARDOWN.CLEAN",
                    AssessmentCheckpoints::TeardownRenderDrained,
                    FindingClass::LifetimeLeak,
                    FindingSeverity::Error,
                    FindingConfidence::Confirmed,
                    "Engine did not complete the ECS World shutdown drain.",
                    "The named World is absent after bounded owner ticks.",
                    "pumpFrames=" + std::to_string(worldShutdownPumpFrames),
                    true,
                    "Engine shutdown cannot proceed with an unresolved ECS World.")));
            }
            diagnostics = frameDriver.PumpRenderProgressOnce();
            static_cast<void>(assessmentSession.RecordCheckpoint(
                AssessmentCheckpoints::TeardownRenderDrained));
            static_cast<void>(RecordRuntimeAssessmentSnapshot(
                assessmentSession,
                AssessmentCheckpoints::TeardownRenderDrained,
                nullptr,
                &resourceDiagnosticsView,
                nullptr,
                &diagnostics,
                &engine,
                worldDestroyed ? 0 : 1,
                requireGpuTiming));
            if (diagnostics.lastPresentedFrameSequence == 0)
            {
                static_cast<void>(assessmentSession.RecordFinding(
                    MakeHostFinding(
                        "RENDER.FRAME.NOT_PRESENTED",
                        "RENDER",
                        "ENGINE.FRAME.PRESENTED",
                        AssessmentCheckpoints::ScenarioStable,
                        FindingClass::ContractViolation,
                        FindingSeverity::Error,
                        FindingConfidence::Confirmed,
                        "The scenario never presented a complete frame.",
                        "Presented frame sequence is greater than zero.",
                        "presented sequence=0",
                        true,
                        "No complete frame reached presentation.")));
            }
            else
            {
                static_cast<void>(assessmentSession.RecordInvariantObservation(
                    {{AssessmentCode("ENGINE.FRAME.PRESENTED"),
                      "At least one fully submitted frame reaches presentation."},
                     AssessmentCheckpoints::ScenarioStable}));
            }
            if (diagnostics.nativeValidation.available &&
                (diagnostics.nativeValidation.errorCount != 0 ||
                 diagnostics.nativeValidation.corruptionCount != 0))
            {
                static_cast<void>(assessmentSession.RecordFinding(
                    MakeHostFinding(
                        "RHI.VALIDATION.ERROR",
                        "RHI",
                        "ENGINE.TEARDOWN.CLEAN",
                        AssessmentCheckpoints::TeardownRenderDrained,
                        FindingClass::ContractViolation,
                        diagnostics.nativeValidation.corruptionCount != 0
                            ? FindingSeverity::Fatal
                            : FindingSeverity::Error,
                        FindingConfidence::Confirmed,
                        "The native validation layer reported errors.",
                        "Native validation error and corruption counts are zero.",
                        "errors=" +
                            std::to_string(
                                diagnostics.nativeValidation.errorCount) +
                            ", corruption=" +
                            std::to_string(
                                diagnostics.nativeValidation.corruptionCount),
                        true,
                        "DX12 Debug Layer or Vulkan Validation reported an error.")));
            }
            const bool assetContentHashRequired =
                !options.assessmentBaselinePath.empty() ||
                options.assessmentProfile ==
                    SampleAssessmentProfile::Qualification ||
                options.assessmentProfile ==
                    SampleAssessmentProfile::Benchmark;
            const bool deviceIdentityAvailable =
                !diagnostics.adapterName.empty() &&
                !diagnostics.driverVersion.empty() &&
                diagnostics.driverVersion != "driver-unavailable";
            if (!deviceIdentityAvailable)
            {
                static_cast<void>(assessmentSession.RecordFinding(
                    MakeHostFinding(
                        "INSTRUMENTATION.DEVICE_IDENTITY.UNAVAILABLE",
                        "INSTRUMENTATION",
                        "INSTRUMENTATION.REQUIRED_AVAILABLE",
                        AssessmentCheckpoints::EngineBaseline,
                        FindingClass::InstrumentationGap,
                        assetContentHashRequired ? FindingSeverity::Error
                                                 : FindingSeverity::Warning,
                        FindingConfidence::Confirmed,
                        "Backend device identity is incomplete.",
                        "Adapter and driver identifiers are both available.",
                        "adapter='" + diagnostics.adapterName +
                            "', driver='" + diagnostics.driverVersion + "'",
                        assetContentHashRequired,
                        assetContentHashRequired
                            ? "Baseline, qualification, and benchmark comparisons require adapter and driver identity."
                            : std::string{})));
            }

            const bool assetContentIdentityAvailable =
                assetContentVerification.allSelectedAssetsVerified &&
                assessmentSession.BindVerifiedAssetSet(
                    assetContentVerification.assetSet);
            if (!assetContentIdentityAvailable)
            {
                static_cast<void>(assessmentSession.RecordFinding(
                    MakeHostFinding(
                        "INSTRUMENTATION.ASSET_CONTENT_HASH.UNAVAILABLE",
                        "INSTRUMENTATION",
                        "INSTRUMENTATION.REQUIRED_AVAILABLE",
                        AssessmentCheckpoints::EngineBaseline,
                        FindingClass::InstrumentationGap,
                        assetContentHashRequired ? FindingSeverity::Error
                                                 : FindingSeverity::Warning,
                        FindingConfidence::Confirmed,
                        "The selected workload does not provide a verified portable asset-content fingerprint.",
                        "The workload fingerprint contains a verified source/cooked content hash for every selected asset.",
                        "assetSet=" +
                            (assetContentVerification.assetSet.empty()
                                 ? std::string("unavailable")
                                 : assetContentVerification.assetSet),
                        assetContentHashRequired,
                        assetContentHashRequired
                            ? "Baseline, qualification, and benchmark comparisons require verified asset content identity."
                            : std::string{})));
            }
            if (!assetContentHashRequired ||
                (deviceIdentityAvailable && assetContentIdentityAvailable))
            {
                static_cast<void>(assessmentSession.RecordInvariantObservation(
                    {{AssessmentCode("INSTRUMENTATION.REQUIRED_AVAILABLE"),
                      "Required diagnostics explicitly report availability."},
                     AssessmentCheckpoints::TeardownRenderDrained}));
            }

            if (!options.assessmentBaselinePath.empty())
            {
                std::string baselineError;
                const std::optional<AssessmentFingerprint> baseline =
                    ReadAssessmentFingerprint(
                        options.assessmentBaselinePath, baselineError);
                if (!baseline.has_value())
                {
                    static_cast<void>(assessmentSession.RecordFinding(
                        MakeHostFinding(
                            "ASSESSMENT.BASELINE.UNREADABLE",
                            "INSTRUMENTATION",
                            "INSTRUMENTATION.REQUIRED_AVAILABLE",
                            AssessmentCheckpoints::ScenarioStable,
                            FindingClass::InstrumentationGap,
                            FindingSeverity::Error,
                            FindingConfidence::Confirmed,
                            "The requested assessment baseline could not be read.",
                            "A complete RVX.FrameworkAssessmentReport fingerprint is readable.",
                            baselineError,
                            true,
                            "The explicitly requested baseline was unavailable.")));
                }
                else
                {
                    const BaselineComparisonResult comparison =
                        assessmentSession.CompareBaseline(*baseline);
                    if (!comparison.IsComparable())
                    {
                        static_cast<void>(assessmentSession.RecordFinding(
                            MakeHostFinding(
                                "ASSESSMENT.BASELINE.FINGERPRINT_MISMATCH",
                                "INSTRUMENTATION",
                                "INSTRUMENTATION.REQUIRED_AVAILABLE",
                                AssessmentCheckpoints::ScenarioStable,
                                FindingClass::InstrumentationGap,
                                FindingSeverity::Error,
                                FindingConfidence::Confirmed,
                                "The baseline fingerprint is not comparable.",
                                "Sample, revision, backend, device, render configuration, verified assets, and platform match exactly.",
                                comparison.reason,
                                true,
                                "An explicitly requested baseline requires a compatible schema-v2 fingerprint.")));
                    }
                }
            }

            uint32 engineShutdownAttempts = 0;
            do
            {
                engine.Shutdown();
                ++engineShutdownAttempts;
                if (engine.IsInitialized())
                {
                    diagnostics = frameDriver.PumpRenderProgressOnce();
                }
            } while (engine.IsInitialized() &&
                     engineShutdownAttempts < teardownPumpFrameLimit &&
                     std::chrono::steady_clock::now() < teardownDeadline);
            const EngineShutdownDiagnostics& shutdownDiagnostics =
                engine.GetLastShutdownDiagnostics();
            const RenderDiagnosticsSnapshot finalRenderDiagnostics =
                render->GetDiagnosticsSnapshot();
            const RenderNativeValidationDiagnostics& finalNativeValidation =
                finalRenderDiagnostics.nativeValidation;
            report.renderDiagnostics.nativeValidationAvailable =
                finalNativeValidation.available;
            report.renderDiagnostics.nativeValidationEnabled =
                finalNativeValidation.enabled;
            report.renderDiagnostics.nativeValidationReadComplete =
                finalNativeValidation.readComplete;
            report.renderDiagnostics.nativeValidationWarningCount =
                finalNativeValidation.warningCount;
            report.renderDiagnostics.nativeValidationErrorCount =
                finalNativeValidation.errorCount;
            report.renderDiagnostics.nativeValidationCorruptionCount =
                finalNativeValidation.corruptionCount;
            static_cast<void>(assessmentSession.RecordCheckpoint(
                AssessmentCheckpoints::EngineShutdownComplete));
            AssessmentSnapshot shutdownSnapshot;
            shutdownSnapshot.checkpoint =
                AssessmentCheckpoints::EngineShutdownComplete;
            shutdownSnapshot.metrics.push_back(MakeAssessmentMetric(
                "ENGINE.WORLD_COUNT", "count",
                "World count at the checkpoint.",
                static_cast<uint64>(
                    shutdownDiagnostics.worldCountAfterShutdown)));
            shutdownSnapshot.metrics.push_back(MakeAssessmentMetric(
                "ENGINE.SHUTDOWN_CLEAN", "boolean",
                "Whether Engine shutdown satisfied all lifetime gates.",
                shutdownDiagnostics.clean));
            const ResourceShutdownDiagnostics& resourceShutdownDiagnostics =
                resourceSubsystem->GetLastShutdownDiagnostics();
            if (resourceShutdownDiagnostics.available)
            {
                shutdownSnapshot.metrics.push_back(MakeAssessmentMetric(
                    "RESOURCE.SHUTDOWN_CLEAN",
                    "boolean",
                    "Whether Resource shutdown retired CPU and completion-owned work.",
                    resourceShutdownDiagnostics.clean));
            }
            else
            {
                shutdownSnapshot.metrics.push_back(
                    MakeUnavailableAssessmentMetric(
                        "RESOURCE.SHUTDOWN_CLEAN",
                        "boolean",
                        "Whether Resource shutdown retired CPU and completion-owned work.",
                        "ResourceSubsystem did not retain a shutdown snapshot."));
            }
            static_cast<void>(assessmentSession.RecordSnapshot(
                std::move(shutdownSnapshot)));
            if (!resourceShutdownDiagnostics.available ||
                !resourceShutdownDiagnostics.clean)
            {
                const ResourceDiagnosticsSnapshot& remaining =
                    resourceShutdownDiagnostics.afterManagerShutdown;
                static_cast<void>(assessmentSession.RecordFinding(
                    MakeHostFinding(
                        "RESOURCE.TEARDOWN.NOT_CLEAN",
                        "RESOURCE",
                        "ENGINE.TEARDOWN.CLEAN",
                        AssessmentCheckpoints::EngineShutdownComplete,
                        FindingClass::LifetimeLeak,
                        FindingSeverity::Fatal,
                        FindingConfidence::Confirmed,
                        "ResourceSubsystem did not quiesce its CPU and GPU handoff domains.",
                        "ResourceManager is stopped, its lifecycle callback is detached, and no operation, job, publication, lease, or retirement remains.",
                        "available=" +
                            std::string(resourceShutdownDiagnostics.available
                                            ? "true"
                                            : "false") +
                            ", managerStopped=" +
                            (resourceShutdownDiagnostics.managerStopped
                                 ? "true"
                                 : "false") +
                            ", activeOperations=" +
                            std::to_string(remaining.activeOperations) +
                            ", pendingJobs=" +
                            std::to_string(remaining.pendingAsyncJobs) +
                            ", pendingRetirements=" +
                            std::to_string(remaining.pendingRetirementCount) +
                            ", activeLeases=" +
                            std::to_string(remaining.activeLeaseCount),
                        true,
                        "Resource lifetime work survived ResourceSubsystem shutdown.")));
            }
            if (!shutdownDiagnostics.available || !shutdownDiagnostics.clean)
            {
                static_cast<void>(assessmentSession.RecordFinding(
                    MakeHostFinding(
                        "ENGINE.TEARDOWN.NOT_CLEAN",
                        "ENGINE",
                        "ENGINE.TEARDOWN.CLEAN",
                        AssessmentCheckpoints::EngineShutdownComplete,
                        FindingClass::LifetimeLeak,
                        FindingSeverity::Fatal,
                        FindingConfidence::Confirmed,
                        "Engine shutdown did not retire all owned lifetime domains.",
                        "No Worlds, active World, JobSystem, or Render runtime remains.",
                        "worlds=" +
                            std::to_string(
                                shutdownDiagnostics.worldCountAfterShutdown) +
                            ", activeWorldCleared=" +
                            (shutdownDiagnostics.activeWorldCleared ? "true"
                                                                    : "false") +
                            ", subsystemsStopped=" +
                            (shutdownDiagnostics.subsystemsStopped ? "true"
                                                                   : "false") +
                            ", resourceClean=" +
                            (shutdownDiagnostics.resourceSubsystemClean
                                 ? "true"
                                 : "false") +
                            ", jobSystemStopped=" +
                            (shutdownDiagnostics.jobSystemStopped ? "true"
                                                                   : "false"),
                        true,
                        "Engine-owned work survived PostEngineShutdown.")));
            }
            else
            {
                static_cast<void>(assessmentSession.RecordInvariantObservation(
                    {{AssessmentCode("ENGINE.TEARDOWN.CLEAN"),
                      "Scene, render, jobs, and worlds retire before shutdown ends."},
                     AssessmentCheckpoints::EngineShutdownComplete}));
            }

            if (finalNativeValidation.available &&
                (finalNativeValidation.errorCount != 0 ||
                 finalNativeValidation.corruptionCount != 0))
            {
                succeeded = false;
                if (error.empty())
                {
                    error =
                        "Native graphics validation reported an error during shutdown";
                }
                static_cast<void>(assessmentSession.RecordFinding(
                    MakeHostFinding(
                        "RHI.VALIDATION.SHUTDOWN_ERROR",
                        "RHI",
                        "ENGINE.TEARDOWN.CLEAN",
                        AssessmentCheckpoints::EngineShutdownComplete,
                        FindingClass::ContractViolation,
                        finalNativeValidation.corruptionCount != 0
                            ? FindingSeverity::Fatal
                            : FindingSeverity::Error,
                        FindingConfidence::Confirmed,
                        "The final native validation snapshot reported errors after RHI child teardown.",
                        "Final native validation error and corruption counts are zero.",
                        "errors=" +
                            std::to_string(finalNativeValidation.errorCount) +
                            ", corruption=" +
                            std::to_string(
                                finalNativeValidation.corruptionCount),
                        true,
                        "Shutdown validation failures must fail the Sample process and report.")));
            }

            const SampleAssessmentReport assessmentReport =
                assessmentSession.Finalize();
            bool assessmentReportWritten = true;
            if (options.HasFrameworkAssessment())
            {
                std::string assessmentError;
                assessmentReportWritten = FrameworkAssessmentJsonWriter::WriteFile(
                    options.assessmentReportPath,
                    assessmentReport,
                    &assessmentError);
                if (!assessmentReportWritten)
                {
                    RVX_CORE_ERROR("{}", assessmentError);
                }
            }

            uint32 blockingFindingCount = 0;
            uint32 advisoryFindingCount = 0;
            for (const Finding& finding : assessmentReport.findings)
            {
                if (finding.IsBlocking())
                {
                    ++blockingFindingCount;
                }
                else
                {
                    ++advisoryFindingCount;
                }
            }
            report.assessment.enabled = options.HasFrameworkAssessment();
            report.assessment.reportPath = options.assessmentReportPath;
            report.assessment.blockGrade =
                GetAssessmentBlockGradeCode(assessmentReport.blockGrade);
            report.assessment.findingCount = static_cast<uint32>(
                assessmentReport.findings.size());
            report.assessment.advisoryCount = advisoryFindingCount;
            report.assessment.blockingCount = blockingFindingCount;
            report.assessment.droppedEventCount =
                assessmentReport.droppedEventCount;
            report.assessment.pass =
                !IsBlockingGrade(assessmentReport.blockGrade);
            if (options.HasFrameworkAssessment())
            {
                succeeded &= report.assessment.pass && assessmentReportWritten;
                if (!report.assessment.pass && error.empty())
                {
                    error = "Framework assessment reported a blocking finding";
                }
            }
            report.pass = report.pass && succeeded;

            bool reportWritten = true;
            if (!options.common.reportPath.empty())
            {
                std::string reportError;
                reportWritten = WriteSampleReportJson(
                    report,
                    options.common.reportPath,
                    &reportError);
                if (!reportWritten)
                {
                    RVX_CORE_ERROR("{}", reportError);
                }
            }
            if (options.common.diagnostics && options.common.reportPath.empty())
            {
                WriteSampleReportJson(std::cout, report);
            }

            if (lifetimeQualification)
            {
                std::string lifetimeReportError;
                lifetimeReportWritten =
                    WriteSampleLifetimeQualificationReport(
                        lifetimeQualification->GetReport(),
                        options.lifetimeReportPath,
                        &lifetimeReportError);
                if (!lifetimeReportWritten)
                {
                    RVX_CORE_ERROR("{}", lifetimeReportError);
                }
            }

            const bool startupReportWritten = startupTimeline.Finalize();
            if (!startupReportWritten)
            {
                RVX_CORE_ERROR("Failed to write startup timeline report");
            }

            if (!succeeded)
            {
                RVX_CORE_ERROR("Sample '{}' failed: {}",
                               registeredInfo->id,
                               error);
            }
            Log::Shutdown();
            return succeeded && reportWritten && lifetimeReportWritten &&
                           startupReportWritten && assessmentReportWritten
                       ? 0
                       : 1;
        }

        RVX_CORE_ERROR("{}", error);
        static_cast<void>(assessmentSession.RecordFinding(MakeHostFinding(
            "SAMPLE.HOST.WORLD_SETUP_FAILED",
            "ENGINE",
            "SAMPLE.HOST.INITIALIZED",
            AssessmentCheckpoints::EngineBaseline,
            FindingClass::ContractViolation,
            FindingSeverity::Fatal,
            FindingConfidence::Confirmed,
            "The Sample World or active ECS camera could not be established.",
            "A configured World, authoritative ECS scene, and active camera are available.",
            error,
            true,
            "The scenario cannot execute without its World and Camera ownership boundary.")));
        sample.reset();
        if (world != nullptr)
        {
            engine.DestroyWorld(worldRequirements.world.name);
            for (uint32 pump = 0;
                 pump < 120u && engine.GetWorld(worldRequirements.world.name) != nullptr;
                 ++pump)
            {
                engine.Tick(sampleDeltaTime);
            }
        }
        engine.Shutdown();
        static_cast<void>(assessmentSession.RecordCheckpoint(
            AssessmentCheckpoints::EngineShutdownComplete));
        AssessmentSnapshot failedShutdownSnapshot;
        failedShutdownSnapshot.checkpoint =
            AssessmentCheckpoints::EngineShutdownComplete;
        failedShutdownSnapshot.metrics.push_back(MakeAssessmentMetric(
            "ENGINE.WORLD_COUNT",
            "count",
            "World count at the checkpoint.",
            static_cast<uint64>(
                engine.GetLastShutdownDiagnostics().worldCountAfterShutdown)));
        failedShutdownSnapshot.metrics.push_back(MakeAssessmentMetric(
            "ENGINE.SHUTDOWN_CLEAN",
            "boolean",
            "Whether Engine shutdown satisfied all lifetime gates.",
            engine.GetLastShutdownDiagnostics().clean));
        static_cast<void>(assessmentSession.RecordSnapshot(
            std::move(failedShutdownSnapshot)));
        const SampleAssessmentReport failedAssessment =
            assessmentSession.Finalize();
        if (options.HasFrameworkAssessment())
        {
            std::string assessmentError;
            if (!FrameworkAssessmentJsonWriter::WriteFile(
                    options.assessmentReportPath,
                    failedAssessment,
                    &assessmentError))
            {
                RVX_CORE_ERROR("{}", assessmentError);
            }
        }
        static_cast<void>(startupTimeline.Finalize());
        Log::Shutdown();
        return 1;
    }
} // namespace RVX
