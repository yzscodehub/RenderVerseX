#include "Render/GPUScene/GPUSceneDiagnostics.h"
#include "Render/Material/MaterialSystem.h"
#include "Engine/RenderRuntimeDiagnostics.h"
#include "Samples/FrameworkAssessment.h"
#include "Samples/RuntimeFrameDriver.h"
#include "Samples/SampleCLI.h"
#include "Samples/SampleLifetimeQualification.h"
#include "Samples/SampleManifest.h"
#include "Samples/SampleRunner.h"
#include "Samples/SampleScreenshotWriter.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <utility>

namespace RVX
{
    struct RuntimeFrameDriverTestAccess
    {
        [[nodiscard]] static bool UsesFullTick(
            const RuntimeFrameWaitRequest& request,
            const RenderDiagnosticsSnapshot& diagnostics,
            const EngineRenderRuntimeDiagnostics& engineDiagnostics = {}) noexcept
        {
            return RuntimeFrameDriver::SelectTickAction(
                       request, diagnostics, engineDiagnostics) ==
                   RuntimeFrameDriver::TickAction::Full;
        }

        [[nodiscard]] static bool UsesProgressPoll(
            const RuntimeFrameWaitRequest& request,
            const RenderDiagnosticsSnapshot& diagnostics,
            const EngineRenderRuntimeDiagnostics& engineDiagnostics = {}) noexcept
        {
            return RuntimeFrameDriver::SelectTickAction(
                       request, diagnostics, engineDiagnostics) ==
                   RuntimeFrameDriver::TickAction::ProgressPoll;
        }

        [[nodiscard]] static bool ObservesOnly(
            const RuntimeFrameWaitRequest& request,
            const RenderDiagnosticsSnapshot& diagnostics,
            const EngineRenderRuntimeDiagnostics& engineDiagnostics = {}) noexcept
        {
            return RuntimeFrameDriver::SelectTickAction(
                       request, diagnostics, engineDiagnostics) ==
                   RuntimeFrameDriver::TickAction::Observe;
        }

        [[nodiscard]] static bool HasReached(
            const RuntimeFrameWaitRequest& request,
            const RenderDiagnosticsSnapshot& diagnostics) noexcept
        {
            return RuntimeFrameDriver::HasReached(request, diagnostics);
        }
    };

    struct SampleRunnerTestAccess
    {
        [[nodiscard]] static uint64 ResolveFrameWaitTargetSequence(
            uint64 publishedBeforeOuterTick,
            uint64 publishedAfterOuterTick) noexcept
        {
            return SampleRunner::ResolveFrameWaitTargetSequence(
                publishedBeforeOuterTick, publishedAfterOuterTick);
        }

        [[nodiscard]] static bool CanBeginFinalRenderDrain(
            uint32 executedFrames,
            uint32 minimumFrames,
            uint32 maximumFrames) noexcept
        {
            return SampleRunner::CanBeginFinalRenderDrain(
                executedFrames, minimumFrames, maximumFrames);
        }

        static void ConfigureFrameWaitPolicy(
            bool deterministicEpochActive,
            float32 deltaTime,
            RuntimeFrameWaitRequest& request) noexcept
        {
            SampleRunner::ConfigureFrameWaitPolicy(
                deterministicEpochActive, deltaTime, request);
        }

        [[nodiscard]] static bool ResolveManifestContract(
            const SampleScenarioManifest& manifest,
            SampleRunnerCLIOptions& options,
            std::string& outError)
        {
            return SampleRunner::ResolveManifestContract(
                manifest, options, outError);
        }

        [[nodiscard]] static bool ValidateManifestAssessmentContract(
            const SampleScenarioManifest& manifest,
            const SampleAssessmentContract& contract,
            std::string& outError)
        {
            return SampleRunner::ValidateManifestAssessmentContract(
                manifest, contract, outError);
        }

        [[nodiscard]] static bool ApplyHiZReflectionQualificationProfile(
            std::string_view sampleId,
            bool probeEnabled,
            SampleScenarioManifest& manifest,
            SampleAssessmentContract& contract,
            std::string& outError)
        {
            return SampleRunner::ApplyHiZReflectionQualificationProfile(
                sampleId, probeEnabled, manifest, contract, outError);
        }

        [[nodiscard]] static uint64 ResolveSceneRemovalPublicationSequence(
            uint64 removalSceneRevision,
            const EngineRenderRuntimeDiagnostics& diagnostics) noexcept
        {
            return SampleRunner::ResolveSceneRemovalPublicationSequence(
                removalSceneRevision, diagnostics);
        }

        [[nodiscard]] static bool HasCompletedSceneRemovalPublication(
            bool engineInitialized,
            bool deadlineExpired,
            uint64 removalSceneRevision,
            uint64 removalPublicationSequence,
            const RenderDiagnosticsSnapshot& diagnostics) noexcept
        {
            return SampleRunner::HasCompletedSceneRemovalPublication(
                engineInitialized,
                deadlineExpired,
                removalSceneRevision,
                removalPublicationSequence,
                diagnostics);
        }
    };
} // namespace RVX

namespace
{
    RVX::RenderDiagnosticsSnapshot MakeLifetimeSnapshot(
        RVX::uint64 sequence,
        RVX::uint32 textureAllocations = 4)
    {
        RVX::RenderDiagnosticsSnapshot diagnostics;
        diagnostics.frameFeatures.available = true;
        diagnostics.frameFeatures.gpuUsedMemory = 256ULL * 1024ULL * 1024ULL;
        diagnostics.nativeValidation.available = true;
        diagnostics.nativeValidation.enabled = true;
        diagnostics.nativeValidation.readComplete = true;
        auto& graph = diagnostics.renderGraphLifetime;
        graph.available = true;
        graph.frameSequence = sequence;
        graph.planHash = 0x12345678ULL;
        graph.physicalRealizationCount = 7;
        graph.physicalTextureAllocationCount = textureAllocations;
        graph.physicalBufferAllocationCount = 3;
        graph.totalPooledMemoryBytes = 64ULL * 1024ULL * 1024ULL;
        graph.transientViewCount = 12;
        graph.inFlightTextureLeases = 2;
        graph.inFlightBufferLeases = 1;
        graph.leaseCommitCount = sequence * 2ULL;
        graph.completionRetirementCount = sequence;
        graph.descriptors.renderTargets.activeDescriptors = 2;
        graph.descriptors.renderTargets.peakActiveDescriptors = 2;
        graph.descriptors.depthStencils.activeDescriptors = 5;
        graph.descriptors.depthStencils.peakActiveDescriptors = 5;
        return diagnostics;
    }

    RVX::RenderExecutionReceipt MakeSuccessfulRenderExecutionReceipt(
        RVX::uint64 frameSequence = 91)
    {
        RVX::RenderExecutionReceipt receipt;
        receipt.schemaVersion = 8;
        receipt.available = true;
        receipt.graphComposed = true;
        receipt.executionSucceeded = true;
        receipt.presented = true;
        receipt.succeeded = true;
        receipt.build.sourceCommit = std::string(40, 'a');
        receipt.build.preset = "win_x64_pr_debug";
        receipt.frameIdentity.frameSequence = frameSequence;
        receipt.frameIdentity.sceneRevision = 17;
        receipt.frameIdentity.surfaceGeneration = 3;
        receipt.frameIdentity.configurationRevision = 5;
        receipt.viewIdentity.sceneRuntimeId = 7;
        receipt.viewIdentity.cameraEntityPacked = 0x0000000200000001ULL;
        receipt.viewIdentity.subViewIndex = 0;
        receipt.viewIdentity.role = RVX::ViewRole::Main;
        receipt.backend = RVX::RHIBackendType::DX12;
        receipt.requestedRecipe = RVX::RenderPipelineRecipe::DeferredQuality;
        receipt.resolvedRecipe = RVX::RenderPipelineRecipe::DeferredQuality;
        receipt.quality = RVX::RenderQualityTier::High;
        receipt.profileHash = RVX::ComputeRenderPipelineProfileHash(
            receipt.resolvedRecipe, receipt.quality);
        receipt.deferredOpaque.available = true;
        receipt.deferredOpaque.status =
            RVX::RenderDeferredOpaqueExecutionStatus::Completed;
        receipt.deferredOpaque.plannedPacketCount = 2;
        receipt.deferredOpaque.executedPacketCount = 2;
        receipt.lighting.lightingPlanIdentity = 0xABCDULL;
        receipt.lighting.sceneLightStateHash = 0x1234ULL;
        receipt.lighting.primaryDirectionalLightId = 27;
        receipt.lighting.pointLightRequestedCount = 3;
        receipt.lighting.pointLightAdmittedCount = 2;
        receipt.lighting.pointLightCapacity = 2;
        receipt.lighting.pointLightOverflowCount = 1;
        receipt.lighting.spotLightRequestedCount = 2;
        receipt.lighting.spotLightAdmittedCount = 2;
        receipt.lighting.spotLightCapacity = 4;
        receipt.lighting.spotLightOverflowCount = 0;
        receipt.lighting.csmRequested = true;
        receipt.lighting.csmInputsReady = true;
        receipt.lighting.csmExecutionReady = true;
        receipt.lighting.iblRequested = true;
        receipt.lighting.iblInputsReady = true;
        receipt.lighting.iblExecutionReady = true;
        receipt.deformation.deformationContentHash = 0x5678ULL;
        receipt.deformation.skinnedObjectCount = 1;
        receipt.deformation.paletteMatrixCount = 64;
        receipt.deformation.previousPaletteObjectCount = 1;
        receipt.deformation.currentPaletteBufferReady = true;
        receipt.deformation.previousPaletteBufferReady = true;
        receipt.deformation.opaqueSkinnedDrawCount = 1;
        receipt.deformation.shadowSkinnedDrawCount = 2;
        receipt.deformation.velocitySkinnedDrawCount = 1;
        receipt.deformation.velocityPreviousPaletteDrawCount = 1;
        receipt.postProcess.postProcessContentHash = 0x9ABCu;
        receipt.postProcess.exposureStateContentHash = 0xE120u;
        receipt.postProcess.toneMapExposureStateHash = 0xE120u;
        receipt.postProcess.bloomExposureStateHash = 0xE120u;
        receipt.postProcess.taaExposureStateHash = 0;
        receipt.postProcess.exposureMode = RVX::RenderExposureMode::ManualEV100;
        receipt.postProcess.exposureRequested = true;
        receipt.postProcess.exposurePipelineReady = true;
        receipt.postProcess.exposureInputsReady = true;
        receipt.postProcess.exposureExecuted = true;
        receipt.postProcess.exposureHistogramPassCount = 0;
        receipt.postProcess.exposureAdaptationPassCount = 1;
        receipt.postProcess.exposureHistoryAvailableBefore = true;
        receipt.postProcess.exposureHistorySampled = true;
        receipt.postProcess.exposureHistoryCandidateProduced = true;
        receipt.postProcess.exposureHistoryCommitted = true;
        receipt.postProcess.exposureHistoryAbandoned = false;
        receipt.postProcess.exposureHistoryResetReasons =
            RVX::RenderHistoryResetReason::None;
        receipt.postProcess.exposureProbeRequested = true;
        receipt.postProcess.exposureProbeReady = true;
        receipt.postProcess.targetEV100 = 10.0f;
        receipt.postProcess.adaptedEV100 = 10.0f;
        receipt.postProcess.exposureScale = 0.0008138021f;
        receipt.postProcess.preExposure = 0.0008138021f;
        receipt.postProcess.previousPreExposure = 0.0008138021f;
        receipt.postProcess.toneMapPassCount = 1;
        receipt.postProcess.bloomPassCount = 1;
        receipt.postProcess.colorGradingPassCount = 1;
        receipt.postProcess.fxaaPassCount = 1;
        receipt.postProcess.taaPassCount = 0;
        receipt.postProcess.antiAliasingMode =
            RVX::RenderAntiAliasingMode::FXAA;
        receipt.postProcess.inputColorDomain =
            RVX::PostProcessColorDomain::SceneLinearHDR;
        receipt.postProcess.plannedOutputColorDomain =
            RVX::PostProcessColorDomain::DisplayEncodedLDR;
        receipt.postProcess.storedOutputColorDomain =
            RVX::PostProcessColorDomain::DisplayEncodedLDR;
        receipt.postProcess.finalOutputFormat = RVX::RHIFormat::RGBA8_UNORM;
        receipt.postProcess.toneMappingTransfer =
            RVX::PostProcessTransfer::SceneLinearToDisplayEncoded;
        receipt.postProcess.presentationTransfer =
            RVX::PostProcessTransfer::SceneLinearToDisplayEncoded;
        receipt.postProcess.hdrInputReady = true;
        receipt.postProcess.displayLinearOutputReady = false;
        receipt.postProcess.encodedOutputReady = true;
        receipt.postProcess.fallbackUsed = false;

        RVX::RenderFeatureExecutionReceipt feature;
        feature.featureId = RVX::BuiltinRenderFeatureIds::LegacyForwardMainChain;
        feature.featureSchemaVersion = 1;
        feature.phase = RVX::RenderFeaturePhase::Geometry;
        feature.requirement = RVX::RenderFeatureRequirement::Required;
        feature.readiness.compiled = true;
        feature.readiness.hardwareSupported = true;
        feature.readiness.implementationSupported = true;
        feature.readiness.pipelineReady = true;
        feature.readiness.inputsReady = true;
        feature.readiness.executed = true;
        feature.selected = true;
        feature.graphPassCount = 2;
        feature.executedGraphPassCount = 2;
        feature.contractCompletedWithoutGraphWork = false;
        feature.graphPassNames = {"DepthPrepass", "OpaquePass"};
        receipt.features.push_back(std::move(feature));
        return receipt;
    }

    RVX::RenderExecutionReceipt MakeSuccessfulToonRenderExecutionReceipt(
        RVX::uint64 frameSequence = 91)
    {
        RVX::RenderExecutionReceipt receipt =
            MakeSuccessfulRenderExecutionReceipt(frameSequence);
        receipt.schemaVersion = 11;
        receipt.toonLighting.requested = true;
        receipt.toonLighting.pipelineReady = true;
        receipt.toonLighting.inputsReady = true;
        receipt.toonLighting.executed = true;
        receipt.toonLighting.programFamilyHash =
            RVX::GetMaterialProgramFamilyValueIdentity("toon-lit-v1");
        receipt.toonLighting.materialCount = 1;
        receipt.toonLighting.drawCount = 2;
        receipt.toonLighting.forwardShadedDrawCount = 2;
        receipt.toonLighting.deferredShadedDrawCount = 0;
        receipt.toonLighting.standardLitFallbackCount = 0;
        receipt.toonLighting.materialFallbackCount = 0;
        receipt.toonLighting.bandsApplied = true;
        receipt.toonLighting.specularThresholdApplied = true;
        receipt.toonLighting.rimApplied = true;
        receipt.toonLighting.shadowTintApplied = true;
        receipt.toonLighting.materialTableSchemaVersion = 1;
        receipt.toonLighting.materialTableContentHash = 0x7001ULL;

        receipt.toonOutline.requested = true;
        receipt.toonOutline.pipelineReady = true;
        receipt.toonOutline.inputsReady = true;
        receipt.toonOutline.executed = true;
        receipt.toonOutline.depthInputReady = true;
        receipt.toonOutline.worldNormalInputReady = true;
        receipt.toonOutline.materialIdentityInputReady = true;
        receipt.toonOutline.materialTableInputReady = true;
        receipt.toonOutline.temporalOrderingValidated = true;
        receipt.toonOutline.taaSequenceIndex = 3;
        receipt.toonOutline.outlineSequenceIndex = 4;
        receipt.toonOutline.toneMapSequenceIndex = 5;
        receipt.toonOutline.eligibleToonMaterialCount = 1;
        receipt.toonOutline.qualificationRequested = true;
        receipt.toonOutline.qualificationReady = true;
        receipt.toonOutline.outlinedPixelCount = 128;
        receipt.toonOutline.nonToonAffectedPixelCount = 0;
        receipt.toonOutline.resolutionAware = true;
        receipt.toonOutline.renderWidth = 640;
        receipt.toonOutline.renderHeight = 360;
        receipt.toonOutline.settingsHash = 0x7002ULL;
        receipt.toonOutline.materialTableContentHash = 0x7001ULL;
        receipt.toonOutline.fallbackUsed = false;
        return receipt;
    }

    RVX::RenderExecutionReceipt MakeSuccessfulCurrentToonRenderExecutionReceipt(
        RVX::uint64 frameSequence = 91)
    {
        RVX::RenderExecutionReceipt receipt =
            MakeSuccessfulToonRenderExecutionReceipt(frameSequence);
        receipt.schemaVersion =
            RVX::RVX_RENDER_EXECUTION_RECEIPT_SCHEMA_VERSION;
        return receipt;
    }

    RVX::RenderExecutionReceipt MakeSuccessfulHybridRayTracingReceipt(
        RVX::uint64 frameSequence = 91)
    {
        RVX::RenderExecutionReceipt receipt =
            MakeSuccessfulRenderExecutionReceipt(frameSequence);
        receipt.schemaVersion =
            RVX::RVX_RENDER_EXECUTION_RECEIPT_SCHEMA_VERSION;
        receipt.requestedRecipe = RVX::RenderPipelineRecipe::HybridRayTracing;
        receipt.resolvedRecipe = RVX::RenderPipelineRecipe::HybridRayTracing;
        receipt.quality = RVX::RenderQualityTier::High;
        receipt.profileHash = RVX::ComputeRenderPipelineProfileHash(
            receipt.resolvedRecipe, receipt.quality);

        RVX::RenderHybridRayTracingExecutionReceipt& hybrid =
            receipt.hybridRayTracing;
        hybrid.available = true;
        hybrid.planIdentityHash = 0x484252544C4153ULL;
        hybrid.scenePreparationGeneration = 7;
        hybrid.sceneStructuralHash = 0x5343454E455254ULL;
        hybrid.frameIdentity = receipt.frameIdentity;
        hybrid.viewIdentity = receipt.viewIdentity;
        hybrid.fullResolution = {1280, 720};
        hybrid.halfResolution = {640, 360};
        hybrid.historyResetReasons = RVX::RenderHistoryResetReason::None;
        hybrid.recipe = receipt.resolvedRecipe;
        hybrid.quality = receipt.quality;
        hybrid.profileHash = receipt.profileHash;
        hybrid.shadowRoute =
            RVX::RenderHybridRayTracingShadowRoute::RayTraced;
        hybrid.reflectionRoute =
            RVX::RenderHybridRayTracingReflectionRoute::RayTraced;
        hybrid.graphExecutionSucceeded = true;
        hybrid.sceneBuildAccepted = true;
        hybrid.shadowDispatchRecorded = true;
        hybrid.reflectionDispatchRecorded = true;
        hybrid.spatialDenoiseRecorded = true;
        hybrid.temporalResolveRecorded = true;
        hybrid.compositeRecorded = true;
        hybrid.compositeUsedDenoisedSource = true;
        hybrid.submissionAccepted = true;
        hybrid.presentationQualified = true;
        hybrid.succeeded = true;
        return receipt;
    }

    RVX::RenderDiagnosticsSnapshot MakeSuccessfulToonDiagnosticsSnapshot(
        RVX::uint64 frameSequence = 91)
    {
        RVX::RenderDiagnosticsSnapshot snapshot;
        snapshot.frameFeatures.available = true;
        snapshot.frameFeatures.frameSequence = frameSequence;
        snapshot.frameFeatures.executionReceipt =
            MakeSuccessfulCurrentToonRenderExecutionReceipt(frameSequence);
        snapshot.lastPresentedFrameSequence = frameSequence;
        return snapshot;
    }

    RVX::SamplePostProcessExecutionDiagnostics
        MakeQualifiedPostProcessActualExecution(
            RVX::uint64 frameSequence = 91)
    {
        RVX::SamplePostProcessExecutionDiagnostics diagnostics;
        diagnostics.available = true;
        diagnostics.executionReceiptSchemaVersion =
            RVX::RVX_RENDER_EXECUTION_RECEIPT_SCHEMA_VERSION;
        diagnostics.executionReceiptStructurallyValid = true;
        diagnostics.executionReceiptAvailable = true;
        diagnostics.executionReceiptGraphComposed = true;
        diagnostics.executionReceiptExecutionSucceeded = true;
        diagnostics.executionReceiptPresented = true;
        diagnostics.executionReceiptSucceeded = true;
        diagnostics.frameFeatureSequence = frameSequence;
        diagnostics.receiptFrameSequence = frameSequence;
        diagnostics.lastPresentedFrameSequence = frameSequence;
        diagnostics.presentedFrameAligned = true;
        diagnostics.mainView = true;
        diagnostics.sceneRuntimeId = 7;
        diagnostics.cameraEntityPacked = 0x0000000200000001ULL;
        diagnostics.subViewIndex = 0;
        diagnostics.postProcessFeatureFound = true;
        diagnostics.postProcessFeatureSelected = true;
        diagnostics.postProcessFeatureExecuted = true;
        diagnostics.postProcessFeatureFallbackUsed = false;
        diagnostics.postProcessFeatureGraphPassCount = 4;
        diagnostics.postProcessFeatureExecutedGraphPassCount = 4;
        diagnostics.postProcessReceiptStructurallyValid = true;
        diagnostics.planContentHash = 0x9ABCu;
        diagnostics.exposureStateContentHash = 0xE120u;
        diagnostics.toneMapExposureStateHash = 0xE120u;
        diagnostics.bloomExposureStateHash = 0xE120u;
        diagnostics.taaExposureStateHash = 0;
        diagnostics.exposureMode = RVX::RenderExposureMode::ManualEV100;
        diagnostics.exposureRequested = true;
        diagnostics.exposurePipelineReady = true;
        diagnostics.exposureInputsReady = true;
        diagnostics.exposureExecuted = true;
        diagnostics.exposureHistogramPassCount = 0;
        diagnostics.exposureAdaptationPassCount = 1;
        diagnostics.exposureHistoryAvailableBefore = true;
        diagnostics.exposureHistorySampled = true;
        diagnostics.exposureHistoryCandidateProduced = true;
        diagnostics.exposureHistoryCommitted = true;
        diagnostics.exposureHistoryAbandoned = false;
        diagnostics.exposureHistoryResetReasons =
            RVX::RenderHistoryResetReason::None;
        diagnostics.exposureProbeRequested = true;
        diagnostics.exposureProbeReady = true;
        diagnostics.targetEV100 = 10.0f;
        diagnostics.adaptedEV100 = 10.0f;
        diagnostics.exposureScale = 0.0008138021f;
        diagnostics.preExposure = 0.0008138021f;
        diagnostics.previousPreExposure = 0.0008138021f;
        diagnostics.toneMappingPassCount = 1;
        diagnostics.bloomPassCount = 1;
        diagnostics.colorGradingPassCount = 1;
        diagnostics.fxaaPassCount = 1;
        diagnostics.taaPassCount = 0;
        diagnostics.antiAliasingMode = RVX::RenderAntiAliasingMode::FXAA;
        diagnostics.inputColorDomain =
            RVX::PostProcessColorDomain::SceneLinearHDR;
        diagnostics.plannedOutputColorDomain =
            RVX::PostProcessColorDomain::DisplayEncodedLDR;
        diagnostics.storedOutputColorDomain =
            RVX::PostProcessColorDomain::DisplayEncodedLDR;
        diagnostics.finalOutputFormat = RVX::RHIFormat::RGBA8_UNORM;
        diagnostics.toneMappingTransfer =
            RVX::PostProcessTransfer::SceneLinearToDisplayEncoded;
        diagnostics.presentationTransfer =
            RVX::PostProcessTransfer::SceneLinearToDisplayEncoded;
        diagnostics.hdrInputReady = true;
        diagnostics.displayLinearOutputReady = false;
        diagnostics.encodedOutputReady = true;
        diagnostics.fallbackUsed = false;
        return diagnostics;
    }

    RVX::SamplePostProcessExecutionDiagnostics
        MakeQualifiedTemporalAAActualExecution(
            RVX::uint64 frameSequence = 92)
    {
        RVX::SamplePostProcessExecutionDiagnostics diagnostics =
            MakeQualifiedPostProcessActualExecution(frameSequence);
        diagnostics.postProcessFeatureGraphPassCount = 3;
        diagnostics.postProcessFeatureExecutedGraphPassCount = 3;
        diagnostics.bloomPassCount = 0;
        diagnostics.colorGradingPassCount = 0;
        diagnostics.fxaaPassCount = 0;
        diagnostics.taaPassCount = 1;
        diagnostics.antiAliasingMode = RVX::RenderAntiAliasingMode::TAA;
        diagnostics.taaRequested = true;
        diagnostics.taaPipelineReady = true;
        diagnostics.taaInputsReady = true;
        diagnostics.taaExecuted = true;
        diagnostics.historyAvailableBefore = true;
        diagnostics.historySampled = true;
        diagnostics.historyCandidateProduced = true;
        diagnostics.historyCommitted = true;
        diagnostics.currentJitterPixels = RVX::Vec2(0.25f, -0.125f);
        diagnostics.previousJitterPixels = RVX::Vec2(-0.25f, 0.125f);
        diagnostics.taaResolveGraphPassCount = 1;
        diagnostics.taaHistoryStoreGraphPassCount = 1;
        diagnostics.taaExposureStateHash = diagnostics.exposureStateContentHash;
        return diagnostics;
    }

    TEST(SampleCLIValidation, ParsesCommonOptions)
    {
        const char* argv[] = {
            "BasicRHI",
            "--backend",
            "dx11",
            "--smoke",
            "--frames",
            "1",
            "--screenshot",
            "actual.ppm",
            "--pixel-probe",
            "162",
            "19",
            "--report",
            "report.json",
            "--width",
            "320",
            "--height",
            "180",
            "--quality",
            "low",
            "--toon-outline-qualification-probe",
            "--translucency-qualification-probe",
            "--diagnostics",
            "--no-validation",
        };

        RVX::SampleCLIOptions options;
        std::string error;
        ASSERT_TRUE(RVX::ParseSampleCLI(
            static_cast<int>(std::size(argv)),
            argv,
            options,
            &error))
            << error;

        EXPECT_EQ(options.backend, RVX::RHIBackendType::DX11);
        EXPECT_TRUE(options.smoke);
        EXPECT_EQ(options.frames, 1u);
        EXPECT_EQ(options.screenshotPath, "actual.ppm");
        EXPECT_TRUE(options.pixelProbeEnabled);
        EXPECT_EQ(options.pixelProbeX, 162u);
        EXPECT_EQ(options.pixelProbeY, 19u);
        EXPECT_EQ(options.reportPath, "report.json");
        EXPECT_EQ(options.width, 320u);
        EXPECT_EQ(options.height, 180u);
        EXPECT_EQ(options.quality, "low");
        EXPECT_TRUE(options.toonOutlineQualificationProbeEnabled);
        EXPECT_TRUE(options.translucencyQualificationProbeEnabled);
        EXPECT_TRUE(options.diagnostics);
        EXPECT_FALSE(options.enableValidation);
    }

    TEST(SampleCLIValidation,
         PostProcessActualExecutionRejectsSchedulingAndReceiptFailures)
    {
        RVX::SamplePostProcessExecutionDiagnostics diagnostics =
            MakeQualifiedPostProcessActualExecution();
        EXPECT_TRUE(diagnostics.IsQualificationReady());

        // The public post-process contract admits only the current receipt
        // ABI. This prevents an old producer from being relabelled as a
        // complete current-frame qualification.
        diagnostics.executionReceiptSchemaVersion = 10;
        EXPECT_FALSE(diagnostics.IsQualificationReady());

        diagnostics.executionReceiptSchemaVersion =
            RVX::RVX_RENDER_EXECUTION_RECEIPT_SCHEMA_VERSION;
        EXPECT_TRUE(diagnostics.IsQualificationReady());

        diagnostics.executionReceiptSchemaVersion = 7;
        EXPECT_FALSE(diagnostics.IsQualificationReady());

        // Graph registration alone is not execution evidence.
        diagnostics = MakeQualifiedPostProcessActualExecution();
        diagnostics.postProcessFeatureExecuted = false;
        diagnostics.postProcessFeatureExecutedGraphPassCount = 0;
        EXPECT_FALSE(diagnostics.IsQualificationReady());

        diagnostics = MakeQualifiedPostProcessActualExecution();
        diagnostics.postProcessReceiptStructurallyValid = false;
        EXPECT_FALSE(diagnostics.IsQualificationReady());

        diagnostics = MakeQualifiedPostProcessActualExecution();
        diagnostics.presentedFrameAligned = false;
        EXPECT_FALSE(diagnostics.IsQualificationReady());

        // A claimed structural result cannot bypass the projected color contract.
        diagnostics = MakeQualifiedPostProcessActualExecution();
        diagnostics.plannedOutputColorDomain =
            RVX::PostProcessColorDomain::DisplayLinearLDR;
        EXPECT_FALSE(diagnostics.IsQualificationReady());

        diagnostics = MakeQualifiedPostProcessActualExecution();
        diagnostics.toneMappingTransfer =
            RVX::PostProcessTransfer::SceneLinearToDisplayLinear;
        EXPECT_FALSE(diagnostics.IsQualificationReady());

        diagnostics = MakeQualifiedPostProcessActualExecution();
        diagnostics.plannedOutputColorDomain =
            RVX::PostProcessColorDomain::DisplayLinearLDR;
        diagnostics.finalOutputFormat = RVX::RHIFormat::RGBA8_UNORM_SRGB;
        diagnostics.toneMappingTransfer =
            RVX::PostProcessTransfer::SceneLinearToDisplayLinear;
        diagnostics.presentationTransfer =
            RVX::PostProcessTransfer::DisplayLinearToEncoded;
        diagnostics.displayLinearOutputReady = true;
        EXPECT_TRUE(diagnostics.IsQualificationReady());
    }

    TEST(SampleCLIValidation,
         ToonExecutionProjectionRequiresPresentedCurrentSchemaEvidence)
    {
        RVX::RenderDiagnosticsSnapshot snapshot =
            MakeSuccessfulToonDiagnosticsSnapshot(113);
        ASSERT_TRUE(snapshot.frameFeatures.executionReceipt.IsStructurallyValid());

        const RVX::SampleToonLightingExecutionDiagnostics lighting =
            RVX::ProjectSampleToonLightingExecutionDiagnostics(snapshot);
        const RVX::SampleToonOutlineExecutionDiagnostics outline =
            RVX::ProjectSampleToonOutlineExecutionDiagnostics(snapshot);
        EXPECT_TRUE(lighting.IsExecutionReady());
        EXPECT_EQ(lighting.programFamily, "toon-lit-v1");
        EXPECT_TRUE(outline.IsExecutionReady());
        EXPECT_TRUE(outline.IsQualificationReady());
        EXPECT_EQ(outline.outlinedPixelCount, 128u);
        EXPECT_EQ(outline.nonToonAffectedPixelCount, 0u);

        snapshot.frameFeatures.executionReceipt.presented = false;
        EXPECT_FALSE(
            RVX::ProjectSampleToonLightingExecutionDiagnostics(snapshot)
                .IsExecutionReady());
        EXPECT_FALSE(
            RVX::ProjectSampleToonOutlineExecutionDiagnostics(snapshot)
                .IsQualificationReady());

        snapshot = MakeSuccessfulToonDiagnosticsSnapshot(113);
        snapshot.frameFeatures.executionReceipt.schemaVersion = 11;
        EXPECT_FALSE(
            RVX::ProjectSampleToonLightingExecutionDiagnostics(snapshot)
                .IsExecutionReady());
        EXPECT_FALSE(
            RVX::ProjectSampleToonOutlineExecutionDiagnostics(snapshot)
                .IsExecutionReady());
    }

    TEST(SampleCLIValidation,
         SchemaElevenReceiptRejectsExecutedFeatureWithoutRecordedWork)
    {
        RVX::RenderExecutionReceipt receipt =
            MakeSuccessfulToonRenderExecutionReceipt(114u);
        ASSERT_TRUE(receipt.deferredOpaque.IsStructurallyValid());
        ASSERT_TRUE(receipt.lighting.IsStructurallyValid());
        ASSERT_TRUE(receipt.deformation.IsStructurallyValid());
        ASSERT_TRUE(receipt.postProcess.IsStructurallyValid());
        ASSERT_TRUE(receipt.ambientOcclusion.IsEmpty());
        ASSERT_TRUE(receipt.toonLighting.IsStructurallyValid());
        ASSERT_TRUE(receipt.toonLighting.IsExecutionReady());
        ASSERT_TRUE(receipt.toonOutline.IsStructurallyValid());
        ASSERT_TRUE(receipt.toonOutline.IsExecutionReady());
        ASSERT_TRUE(receipt.build.IsStructurallyValid());
        ASSERT_TRUE(receipt.frameIdentity.IsValid());
        ASSERT_TRUE(receipt.viewIdentity.IsValid());
        ASSERT_TRUE(receipt.features.front().readiness.IsReadyToExecute());
        ASSERT_EQ(receipt.features.front().graphPassNames.size(),
                  receipt.features.front().graphPassCount);
        ASSERT_TRUE(receipt.IsStructurallyValid());
        ASSERT_FALSE(receipt.features.empty());

        RVX::RenderFeatureExecutionReceipt& feature = receipt.features.front();
        feature.graphPassCount = 0;
        feature.executedGraphPassCount = 0;
        feature.graphPassNames.clear();
        feature.contractCompletedWithoutGraphWork = false;
        EXPECT_FALSE(receipt.IsStructurallyValid());

        feature.contractCompletedWithoutGraphWork = true;
        EXPECT_TRUE(receipt.IsStructurallyValid());
    }

    TEST(SampleCLIValidation,
         ToonOutlineQualificationTargetEvaluationFailsTerminalEvidenceWithoutTimeout)
    {
        constexpr RVX::uint64 TargetFrame = 114u;
        RVX::RenderDiagnosticsSnapshot snapshot =
            MakeSuccessfulToonDiagnosticsSnapshot(TargetFrame);

        auto evaluation =
            RVX::EvaluateSampleToonOutlineQualificationTarget(
                snapshot, TargetFrame);
        EXPECT_TRUE(evaluation.IsReady());
        EXPECT_TRUE(evaluation.diagnostic.empty());

        RVX::RenderToonOutlineExecutionReceipt& outline =
            snapshot.frameFeatures.executionReceipt.toonOutline;
        outline.qualificationReady = false;
        outline.outlinedPixelCount = 0;
        outline.nonToonAffectedPixelCount = 0;
        outline.resolutionAware = false;
        outline.renderWidth = 0;
        outline.renderHeight = 0;
        ASSERT_TRUE(
            snapshot.frameFeatures.executionReceipt.IsStructurallyValid());
        evaluation = RVX::EvaluateSampleToonOutlineQualificationTarget(
            snapshot, TargetFrame);
        EXPECT_EQ(evaluation.state,
                  RVX::SampleToonOutlineQualificationTargetState::Pending);

        outline.diagnostic =
            "ScreenSpaceOutline qualification probe was abandoned after submission";
        ASSERT_TRUE(
            snapshot.frameFeatures.executionReceipt.IsStructurallyValid());
        evaluation = RVX::EvaluateSampleToonOutlineQualificationTarget(
            snapshot, TargetFrame);
        EXPECT_TRUE(evaluation.IsFailed());
        EXPECT_EQ(evaluation.diagnostic, outline.diagnostic);

        snapshot = MakeSuccessfulToonDiagnosticsSnapshot(TargetFrame);
        snapshot.frameFeatures.executionReceipt.toonOutline.outlinedPixelCount =
            0;
        ASSERT_TRUE(
            snapshot.frameFeatures.executionReceipt.IsStructurallyValid());
        evaluation = RVX::EvaluateSampleToonOutlineQualificationTarget(
            snapshot, TargetFrame);
        EXPECT_TRUE(evaluation.IsFailed());
        EXPECT_NE(evaluation.diagnostic.find("zero outlined"),
                  std::string::npos);

        snapshot = MakeSuccessfulToonDiagnosticsSnapshot(TargetFrame);
        snapshot.frameFeatures.executionReceipt.toonLighting = {};
        EXPECT_FALSE(
            snapshot.frameFeatures.executionReceipt.IsStructurallyValid());
        evaluation = RVX::EvaluateSampleToonOutlineQualificationTarget(
            snapshot, TargetFrame);
        EXPECT_TRUE(evaluation.IsFailed());

        evaluation = RVX::EvaluateSampleToonOutlineQualificationTarget(
            MakeSuccessfulToonDiagnosticsSnapshot(TargetFrame + 1u),
            TargetFrame);
        EXPECT_EQ(evaluation.state,
                  RVX::SampleToonOutlineQualificationTargetState::Unrelated);
    }

    TEST(SampleCLIValidation,
         TemporalAAQualificationRequiresSampledAndPresentedHistory)
    {
        RVX::SamplePostProcessExecutionDiagnostics diagnostics =
            MakeQualifiedTemporalAAActualExecution();
        EXPECT_TRUE(diagnostics.IsTemporalAAQualificationReady());
        EXPECT_FALSE(diagnostics.IsQualificationReady());

        diagnostics.historySampled = false;
        EXPECT_FALSE(diagnostics.IsTemporalAAQualificationReady());

        diagnostics = MakeQualifiedTemporalAAActualExecution();
        diagnostics.historyCommitted = false;
        EXPECT_FALSE(diagnostics.IsTemporalAAQualificationReady());

        diagnostics = MakeQualifiedTemporalAAActualExecution();
        diagnostics.historyAbandoned = true;
        EXPECT_FALSE(diagnostics.IsTemporalAAQualificationReady());

        diagnostics = MakeQualifiedTemporalAAActualExecution();
        diagnostics.taaResolveGraphPassCount = 0;
        EXPECT_FALSE(diagnostics.IsTemporalAAQualificationReady());

        diagnostics = MakeQualifiedTemporalAAActualExecution();
        diagnostics.fxaaPassCount = 1;
        EXPECT_FALSE(diagnostics.IsTemporalAAQualificationReady());

        diagnostics = MakeQualifiedTemporalAAActualExecution();
        diagnostics.previousJitterPixels = diagnostics.currentJitterPixels;
        EXPECT_FALSE(diagnostics.IsTemporalAAQualificationReady());

        diagnostics = MakeQualifiedTemporalAAActualExecution();
        diagnostics.currentJitterPixels = RVX::Vec2(0.0f);
        diagnostics.previousJitterPixels = RVX::Vec2(0.0f);
        EXPECT_FALSE(diagnostics.IsTemporalAAQualificationReady());

        diagnostics = MakeQualifiedTemporalAAActualExecution();
        diagnostics.currentJitterPixels.x =
            std::numeric_limits<RVX::float32>::quiet_NaN();
        EXPECT_FALSE(diagnostics.IsTemporalAAQualificationReady());
    }

    TEST(SampleCLIValidation,
         ExposureQualificationRequiresRendererProjectedStateAndProbe)
    {
        RVX::SamplePostProcessExecutionDiagnostics diagnostics =
            MakeQualifiedTemporalAAActualExecution();
        diagnostics.bloomPassCount = 1;
        diagnostics.bloomExposureStateHash = diagnostics.exposureStateContentHash;
        diagnostics.taaExposureStateHash = diagnostics.exposureStateContentHash;

        EXPECT_TRUE(diagnostics.IsManualEV100ExposureQualificationReady());
        EXPECT_FALSE(diagnostics.IsHistogramAutoExposureQualificationReady());

        const RVX::SamplePostProcessExecutionDiagnostics validManual =
            diagnostics;
        const auto rejectsOutOfRangeExposureScale =
            [&validManual](
                RVX::float32 RVX::SamplePostProcessExecutionDiagnostics::* field,
                RVX::float32 value)
        {
            RVX::SamplePostProcessExecutionDiagnostics invalid = validManual;
            invalid.*field = value;
            return !invalid.IsManualEV100ExposureQualificationReady();
        };
        const RVX::float32 belowMinimumExposure = std::nextafter(
            RVX::RVX_RENDER_EXPOSURE_MIN_SCALE, 0.0f);
        const RVX::float32 aboveMaximumExposure = std::nextafter(
            RVX::RVX_RENDER_EXPOSURE_MAX_SCALE,
            std::numeric_limits<RVX::float32>::infinity());
        EXPECT_TRUE(rejectsOutOfRangeExposureScale(
            &RVX::SamplePostProcessExecutionDiagnostics::exposureScale,
            belowMinimumExposure));
        EXPECT_TRUE(rejectsOutOfRangeExposureScale(
            &RVX::SamplePostProcessExecutionDiagnostics::preExposure,
            belowMinimumExposure));
        EXPECT_TRUE(rejectsOutOfRangeExposureScale(
            &RVX::SamplePostProcessExecutionDiagnostics::previousPreExposure,
            belowMinimumExposure));
        EXPECT_TRUE(rejectsOutOfRangeExposureScale(
            &RVX::SamplePostProcessExecutionDiagnostics::exposureScale,
            aboveMaximumExposure));
        EXPECT_TRUE(rejectsOutOfRangeExposureScale(
            &RVX::SamplePostProcessExecutionDiagnostics::preExposure,
            aboveMaximumExposure));
        EXPECT_TRUE(rejectsOutOfRangeExposureScale(
            &RVX::SamplePostProcessExecutionDiagnostics::previousPreExposure,
            aboveMaximumExposure));

        RVX::float32 lowPhysicalScale = 0.0f;
        ASSERT_TRUE(RVX::TryCalculateRenderExposureScale(
            RVX::RVX_RENDER_EXPOSURE_MAX_EV100,
            0.0f,
            lowPhysicalScale));
        RVX::SamplePostProcessExecutionDiagnostics lowScaleManual = validManual;
        lowScaleManual.targetEV100 = RVX::RVX_RENDER_EXPOSURE_MAX_EV100;
        lowScaleManual.adaptedEV100 = RVX::RVX_RENDER_EXPOSURE_MAX_EV100;
        lowScaleManual.exposureScale = lowPhysicalScale;
        lowScaleManual.preExposure = lowPhysicalScale;
        lowScaleManual.previousPreExposure = lowPhysicalScale;
        EXPECT_TRUE(lowScaleManual.IsManualEV100ExposureQualificationReady());

        RVX::SamplePostProcessExecutionDiagnostics wrongLowScaleManual =
            lowScaleManual;
        const RVX::float32 wrongLowPhysicalScale = lowPhysicalScale * 100.0f;
        wrongLowScaleManual.exposureScale = wrongLowPhysicalScale;
        wrongLowScaleManual.preExposure = wrongLowPhysicalScale;
        wrongLowScaleManual.previousPreExposure = wrongLowPhysicalScale;
        EXPECT_FALSE(
            wrongLowScaleManual.IsManualEV100ExposureQualificationReady());

        diagnostics.exposureProbeReady = false;
        EXPECT_FALSE(diagnostics.IsManualEV100ExposureQualificationReady());

        diagnostics = MakeQualifiedTemporalAAActualExecution();
        diagnostics.bloomPassCount = 1;
        diagnostics.bloomExposureStateHash = diagnostics.exposureStateContentHash;
        diagnostics.taaExposureStateHash = diagnostics.exposureStateContentHash;
        diagnostics.exposureMode = RVX::RenderExposureMode::HistogramAuto;
        diagnostics.exposureHistogramPassCount = 2;
        EXPECT_TRUE(diagnostics.IsHistogramAutoExposureQualificationReady());

        diagnostics.toneMapExposureStateHash = 0;
        EXPECT_FALSE(diagnostics.IsHistogramAutoExposureQualificationReady());

        diagnostics = MakeQualifiedTemporalAAActualExecution();
        diagnostics.bloomPassCount = 1;
        diagnostics.bloomExposureStateHash = diagnostics.exposureStateContentHash;
        diagnostics.taaExposureStateHash = diagnostics.exposureStateContentHash;
        diagnostics.exposureHistoryCommitted = false;
        EXPECT_FALSE(diagnostics.IsManualEV100ExposureQualificationReady());
    }

    TEST(SampleCLIValidation, SmokeDefaultsToEightFrames)
    {
        const char* argv[] = {"BasicRHI", "--smoke"};

        RVX::SampleCLIOptions options;
        std::string error;
        ASSERT_TRUE(RVX::ParseSampleCLI(
            static_cast<int>(std::size(argv)),
            argv,
            options,
            &error))
            << error;

        EXPECT_TRUE(options.smoke);
        EXPECT_EQ(options.frames, 8u);
    }

    TEST(SampleCLIValidation, PixelProbeIsDisabledByDefault)
    {
        const char* argv[] = {"BasicRHI", "--frames", "1"};

        RVX::SampleCLIOptions options;
        std::string error;
        ASSERT_TRUE(RVX::ParseSampleCLI(
            static_cast<int>(std::size(argv)), argv, options, &error))
            << error;

        EXPECT_FALSE(options.pixelProbeEnabled);
        EXPECT_EQ(options.pixelProbeX, 0u);
        EXPECT_EQ(options.pixelProbeY, 0u);
    }

    TEST(SampleCLIValidation,
         GPUSceneCullingQualificationIsExplicitAndIndependentOfPixelProbe)
    {
        const char* defaultArgv[] = {"BasicRHI", "--frames", "1"};
        RVX::SampleCLIOptions defaultOptions;
        std::string error;
        ASSERT_TRUE(RVX::ParseSampleCLI(static_cast<int>(std::size(defaultArgv)),
                                        defaultArgv,
                                        defaultOptions,
                                        &error))
            << error;
        EXPECT_FALSE(defaultOptions.gpuSceneCullingQualificationEnabled);
        EXPECT_FALSE(defaultOptions.pixelProbeEnabled);

        const char* qualificationArgv[] = {
            "BasicRHI",
            "--frames",
            "1",
            "--gpu-scene-culling-qualification",
        };
        RVX::SampleCLIOptions qualificationOptions;
        ASSERT_TRUE(RVX::ParseSampleCLI(
            static_cast<int>(std::size(qualificationArgv)),
            qualificationArgv,
            qualificationOptions,
            &error))
            << error;
        EXPECT_TRUE(qualificationOptions.gpuSceneCullingQualificationEnabled);
        EXPECT_FALSE(qualificationOptions.pixelProbeEnabled);
    }

    TEST(SampleCLIValidation,
         DirectOpaqueRasterReadbackQualificationIsExplicitAndIndependentOfPixelProbe)
    {
        const char* defaultArgv[] = {"BasicRHI", "--frames", "1"};
        RVX::SampleCLIOptions defaultOptions;
        std::string error;
        ASSERT_TRUE(RVX::ParseSampleCLI(static_cast<int>(std::size(defaultArgv)),
                                        defaultArgv,
                                        defaultOptions,
                                        &error))
            << error;
        EXPECT_FALSE(defaultOptions.directOpaqueRasterReadbackQualificationEnabled);
        EXPECT_FALSE(defaultOptions.pixelProbeEnabled);

        const char* qualificationArgv[] = {
            "BasicRHI",
            "--frames",
            "1",
            "--direct-opaque-raster-readback-qualification",
        };
        RVX::SampleCLIOptions qualificationOptions;
        ASSERT_TRUE(RVX::ParseSampleCLI(
            static_cast<int>(std::size(qualificationArgv)),
            qualificationArgv,
            qualificationOptions,
            &error))
            << error;
        EXPECT_TRUE(
            qualificationOptions.directOpaqueRasterReadbackQualificationEnabled);
        EXPECT_FALSE(qualificationOptions.pixelProbeEnabled);
    }

    TEST(SampleCLIValidation,
         ExposureQualificationProbeIsExplicitAndIndependentOfPixelProbe)
    {
        const char* qualificationArgv[] = {
            "BasicRHI",
            "--frames",
            "1",
            "--exposure-qualification-probe",
        };
        RVX::SampleCLIOptions qualificationOptions;
        std::string error;
        ASSERT_TRUE(RVX::ParseSampleCLI(
            static_cast<int>(std::size(qualificationArgv)),
            qualificationArgv,
            qualificationOptions,
            &error))
            << error;
        EXPECT_TRUE(qualificationOptions.exposureQualificationProbeEnabled);
        EXPECT_FALSE(qualificationOptions.pixelProbeEnabled);
    }

    TEST(SampleCLIValidation,
         AmbientOcclusionQualificationProbeIsExplicitAndIndependentOfPixelProbe)
    {
        const char* qualificationArgv[] = {
            "BasicRHI",
            "--frames",
            "1",
            "--ambient-occlusion-qualification-probe",
        };
        RVX::SampleCLIOptions qualificationOptions;
        std::string error;
        ASSERT_TRUE(RVX::ParseSampleCLI(
            static_cast<int>(std::size(qualificationArgv)),
            qualificationArgv,
            qualificationOptions,
            &error))
            << error;
        EXPECT_TRUE(
            qualificationOptions.ambientOcclusionQualificationProbeEnabled);
        EXPECT_FALSE(qualificationOptions.pixelProbeEnabled);
    }

    TEST(SampleCLIValidation,
         HiZReflectionQualificationProbeIsExplicitAndIndependentOfPixelProbe)
    {
        const char* qualificationArgv[] = {
            "BasicRHI",
            "--frames",
            "1",
            "--hiz-reflection-qualification-probe",
        };
        RVX::SampleCLIOptions qualificationOptions;
        std::string error;
        ASSERT_TRUE(RVX::ParseSampleCLI(
            static_cast<int>(std::size(qualificationArgv)),
            qualificationArgv,
            qualificationOptions,
            &error))
            << error;
        EXPECT_TRUE(
            qualificationOptions.hiZReflectionQualificationProbeEnabled);
        EXPECT_FALSE(qualificationOptions.pixelProbeEnabled);
    }

    TEST(SampleCLIValidation,
         RunnerRejectsCombinedExposureAndAmbientOcclusionQualificationProbes)
    {
        const char* argv[] = {
            "RenderVerseSamples",
            "--exposure-qualification-probe",
            "--ambient-occlusion-qualification-probe",
        };

        RVX::SampleRunnerCLIOptions options;
        std::string error;
        EXPECT_FALSE(RVX::ParseSampleRunnerCLI(
            static_cast<int>(std::size(argv)), argv, options, &error));
        EXPECT_NE(error.find("mutually exclusive"), std::string::npos);
    }

    TEST(SampleCLIValidation,
         RunnerRejectsCombinedHiZReflectionAndAmbientOcclusionQualificationProbes)
    {
        const char* argv[] = {
            "RenderVerseSamples",
            "--hiz-reflection-qualification-probe",
            "--ambient-occlusion-qualification-probe",
        };

        RVX::SampleRunnerCLIOptions options;
        std::string error;
        EXPECT_FALSE(RVX::ParseSampleRunnerCLI(
            static_cast<int>(std::size(argv)), argv, options, &error));
        EXPECT_NE(error.find("mutually exclusive"), std::string::npos);
    }

    TEST(SampleCLIValidation,
         ValidationRunIdSerializesAtSchemaTwentyFourBoundary)
    {
        RVX::SampleReport schema24;
        schema24.schemaVersion =
            RVX::RVX_SAMPLE_REPORT_PRE_AMBIENT_OCCLUSION_EXECUTION_SCHEMA_VERSION;
        schema24.validationRunId = std::string(32, 'a');
        std::ostringstream schema24Stream;
        RVX::WriteSampleReportJson(schema24Stream, schema24);
        EXPECT_NE(schema24Stream.str().find(
                      "\"validationRunId\": \"" +
                      std::string(32, 'a') + "\""),
                  std::string::npos);

        RVX::SampleReport schema23;
        schema23.schemaVersion =
            RVX::RVX_SAMPLE_REPORT_PRE_AMBIENT_OCCLUSION_EXECUTION_SCHEMA_VERSION -
            1u;
        schema23.validationRunId = std::string(32, 'b');
        std::ostringstream schema23Stream;
        RVX::WriteSampleReportJson(schema23Stream, schema23);
        EXPECT_EQ(schema23Stream.str().find("\"validationRunId\""),
                  std::string::npos);
    }

    TEST(SampleCLIValidation, PixelProbeRejectsMalformedCoordinates)
    {
        const char* argv[] = {"BasicRHI", "--pixel-probe", "x", "19"};

        RVX::SampleCLIOptions options;
        std::string error;
        EXPECT_FALSE(RVX::ParseSampleCLI(
            static_cast<int>(std::size(argv)), argv, options, &error));
        EXPECT_NE(error.find("--pixel-probe"), std::string::npos);
    }

    TEST(SampleCLIValidation, PixelProbeIdentityRequiresItsCompletedCaptureFrame)
    {
        RVX::RenderFrameCaptureResult capture;
        capture.code = RVX::RenderFrameCaptureResultCode::Completed;
        capture.requestId = 17;
        capture.frameSequence = 19;
        capture.width = 64;
        capture.height = 64;
        capture.rowPitch = 256;
        capture.bytesPerPixel = 4;
        capture.bytes = {0, 0, 0, 0};
        capture.pixelProbe.code = RVX::RenderFramePixelProbeResultCode::Completed;
        capture.pixelProbe.requestId = 17;
        capture.pixelProbe.frameSequence = 19;
        capture.pixelProbe.requiredSceneRevision = 11;
        capture.pixelProbe.runtimeSurfaceGeneration = 7;
        capture.pixelProbe.preToneFormat = RVX::RHIFormat::RGBA16_FLOAT;

        EXPECT_TRUE(capture.HasMatchingCompletedPixelProbe());
        capture.pixelProbe.frameSequence = 20;
        EXPECT_FALSE(capture.HasMatchingCompletedPixelProbe());
        capture.pixelProbe.frameSequence = 19;
        capture.pixelProbe.requestId = 18;
        EXPECT_FALSE(capture.HasMatchingCompletedPixelProbe());
    }

    TEST(SampleCLIValidation,
         ScreenshotWriterNormalizesBottomLeftCaptureRowsToTopDownPpm)
    {
        RVX::RenderFrameCaptureResult capture;
        capture.code = RVX::RenderFrameCaptureResultCode::Completed;
        capture.requestId = 17;
        capture.frameSequence = 19;
        capture.width = 2;
        capture.height = 2;
        capture.rowPitch = 8;
        capture.bytesPerPixel = 4;
        capture.format = RVX::RHIFormat::RGBA8_UNORM;
        capture.originBottomLeft = true;
        capture.bytes = {
            0, 0, 255, 255, 0, 255, 0, 255,
            255, 0, 0, 255, 255, 255, 255, 255,
        };

        const std::filesystem::path output =
            std::filesystem::path(::testing::TempDir()) /
            "rvx-screenshot-bottom-left-normalization.ppm";
        std::error_code removeError;
        std::filesystem::remove(output, removeError);

        std::string error;
        ASSERT_TRUE(RVX::WriteSampleScreenshotPPM(capture, output, &error))
            << error;

        std::ifstream stream(output, std::ios::binary);
        ASSERT_TRUE(stream.is_open());
        const std::string actual((std::istreambuf_iterator<char>(stream)),
                                 std::istreambuf_iterator<char>());
        std::string expected = "P6\n2 2\n255\n";
        const auto appendPixel = [&expected](RVX::uint8 r,
                                             RVX::uint8 g,
                                             RVX::uint8 b)
        {
            expected.push_back(static_cast<char>(r));
            expected.push_back(static_cast<char>(g));
            expected.push_back(static_cast<char>(b));
        };
        appendPixel(255, 0, 0);
        appendPixel(255, 255, 255);
        appendPixel(0, 0, 255);
        appendPixel(0, 255, 0);
        EXPECT_EQ(actual, expected);

        stream.close();
        std::filesystem::remove(output, removeError);
    }

    TEST(SampleCLIValidation, PixelProbeReportEmitsRawBitsOnlyWhenComplete)
    {
        RVX::SampleReport report;
        report.schemaVersion = RVX::RVX_SAMPLE_REPORT_PIXEL_PROBE_SCHEMA_VERSION;
        report.pass = true;
        RVX::SamplePixelProbeReport probe;
        probe.complete = true;
        probe.resultCode = static_cast<RVX::uint32>(
            RVX::RenderFramePixelProbeResultCode::Completed);
        probe.requestId = 17;
        probe.frameSequence = 19;
        probe.requiredSceneRevision = 11;
        probe.runtimeSurfaceGeneration = 7;
        probe.x = 162;
        probe.y = 19;
        probe.preToneRGBA16FloatBits = {0x3C00u, 0x3800u, 0x0000u, 0x3C00u};
        probe.finalBGRA8Bits = {0x20u, 0x1Fu, 0x52u, 0xFFu};
        report.pixelProbe = probe;

        std::ostringstream stream;
        RVX::WriteSampleReportJson(stream, report);
        const std::string json = stream.str();
        EXPECT_NE(json.find("\"schemaVersion\": 16"), std::string::npos);
        EXPECT_NE(json.find("\"preToneRGBA16FloatBits\": [\"0x3C00\", \"0x3800\", \"0x0000\", \"0x3C00\"]"),
                  std::string::npos);
        EXPECT_NE(json.find("\"finalBGRA8Bits\": [\"0x20\", \"0x1F\", \"0x52\", \"0xFF\"]"),
                  std::string::npos);

        RVX::SampleReport ordinary;
        ordinary.schemaVersion = RVX::RVX_SAMPLE_REPORT_SCHEMA_VERSION;
        ordinary.pass = true;
        std::ostringstream ordinaryStream;
        RVX::WriteSampleReportJson(ordinaryStream, ordinary);
        EXPECT_EQ(ordinaryStream.str().find("\"pixelProbe\""),
                  std::string::npos);
    }

    TEST(SampleCLIValidation,
         CurrentReportWritesExactVisualAndRenderExecutionReceipts)
    {
        RVX::SampleReport report;
        report.schemaVersion = RVX::RVX_SAMPLE_REPORT_SCHEMA_VERSION;
        report.validationRunId = std::string(32, '1');
        report.submittedFrameSequence = 91;
        report.presentedFrameSequence = 91;
        report.pass = true;

        RVX::SampleVisualContractReceipt receipt;
        receipt.manifestSchema = "RVX.SampleManifest";
        receipt.manifestSchemaVersion = 1;
        receipt.manifestContentSha256 = std::string(64, 'a');
        receipt.visualContractVersion = 3;
        receipt.domain = RVX::SampleDomain::Rendering;
        receipt.intent = RVX::SampleIntent::Regression;
        receipt.requestedPipeline =
            RVX::SamplePipelineRecipe::DeferredQuality;
        receipt.observedPipeline =
            RVX::SamplePipelineRecipe::DeferredQuality;
        receipt.quality = RVX::SampleQualityTier::High;
        receipt.cameraBookmarkId = "hero";
        receipt.fixedTimeNumerator = 1;
        receipt.fixedTimeDenominator = 60;
        receipt.deterministicSeed =
            std::numeric_limits<RVX::uint32>::max();
        receipt.captureTick = 16;
        receipt.captureCarryingFrameSequence = 91;
        receipt.captureSceneRuntimeId = 7;
        receipt.captureCameraEntityPacked = 0x0000000200000001ULL;
        receipt.captureSceneRevision = 17;
        receipt.captureSurfaceGeneration = 3;
        receipt.captureConfigurationRevision = 5;
        receipt.deterministicQualification = true;
        receipt.renderExecutionReceipt =
            MakeSuccessfulRenderExecutionReceipt(91);
        receipt.renderExecutionReceipt->schemaVersion =
            RVX::RVX_RENDER_EXECUTION_RECEIPT_SCHEMA_VERSION;
        receipt.satisfied = true;
        report.visualContractReceipt = receipt;

        std::ostringstream stream;
        RVX::WriteSampleReportJson(stream, report);
        const std::string json = stream.str();
        EXPECT_NE(json.find("\"schemaVersion\": 33"), std::string::npos);
        EXPECT_NE(json.find("\"waterSurfaceActualExecution\": {"),
                  std::string::npos);
        EXPECT_NE(json.find("\"qualifiedWater\": false"),
                  std::string::npos);
        EXPECT_NE(json.find("\"validationRunId\": \"" +
                            std::string(32, '1') + "\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"visualContractReceipt\": {"),
                  std::string::npos);
        EXPECT_NE(json.find("\"manifestSchema\": \"RVX.SampleManifest\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"manifestSchemaVersion\": 1"),
                  std::string::npos);
        EXPECT_NE(json.find(std::string(64, 'a')), std::string::npos);
        EXPECT_NE(json.find("\"visualContractVersion\": 3"),
                  std::string::npos);
        EXPECT_NE(json.find("\"domain\": \"rendering\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"intent\": \"regression\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"requestedPipeline\": \"deferred\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"observedPipeline\": \"deferred\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"quality\": \"high\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"cameraBookmarkId\": \"hero\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"fixedTimeNumerator\": 1"),
                  std::string::npos);
        EXPECT_NE(json.find("\"fixedTimeDenominator\": 60"),
                  std::string::npos);
        EXPECT_NE(json.find("\"deterministicSeed\": 4294967295"),
                  std::string::npos);
        EXPECT_NE(json.find("\"captureTick\": 16"),
                  std::string::npos);
        EXPECT_NE(json.find("\"captureCarryingFrameSequence\": 91"),
                  std::string::npos);
        EXPECT_NE(json.find("\"captureSceneRuntimeId\": 7"),
                  std::string::npos);
        EXPECT_NE(json.find("\"captureCameraEntityPacked\": 8589934593"),
                  std::string::npos);
        EXPECT_NE(json.find("\"captureSceneRevision\": 17"),
                  std::string::npos);
        EXPECT_NE(json.find("\"captureSurfaceGeneration\": 3"),
                  std::string::npos);
        EXPECT_NE(json.find("\"captureConfigurationRevision\": 5"),
                  std::string::npos);
        EXPECT_NE(json.find("\"deterministicQualification\": true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"renderExecutionReceipt\": {"),
                  std::string::npos);
        EXPECT_NE(json.find("\"schemaVersion\": 12"), std::string::npos);
        EXPECT_NE(json.find("\"sourceCommit\": \"" + std::string(40, 'a') +
                            "\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"preset\": \"win_x64_pr_debug\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"frameSequence\": 91"), std::string::npos);
        EXPECT_NE(json.find("\"sceneRevision\": 17"), std::string::npos);
        EXPECT_NE(json.find("\"surfaceGeneration\": 3"),
                  std::string::npos);
        EXPECT_NE(json.find("\"configurationRevision\": 5"),
                  std::string::npos);
        EXPECT_NE(json.find("\"sceneRuntimeId\": 7"), std::string::npos);
        EXPECT_NE(json.find("\"role\": \"main\""), std::string::npos);
        EXPECT_NE(json.find("\"backend\": \"dx12\""), std::string::npos);
        EXPECT_NE(json.find("\"requestedRecipe\": \"deferred\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"resolvedRecipe\": \"deferred\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"profileHash\": "), std::string::npos);
        EXPECT_NE(json.find("\"deferredOpaque\": {"),
                  std::string::npos);
        EXPECT_NE(json.find("\"status\": \"completed\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"plannedPacketCount\": 2"),
                  std::string::npos);
        EXPECT_NE(json.find("\"executedPacketCount\": 2"),
                  std::string::npos);
        EXPECT_NE(json.find("\"lighting\": {"), std::string::npos);
        EXPECT_NE(json.find("\"lightingPlanIdentity\": 43981"),
                  std::string::npos);
        EXPECT_NE(json.find("\"sceneLightStateHash\": 4660"),
                  std::string::npos);
        EXPECT_NE(json.find("\"primaryDirectionalLightId\": 27"),
                  std::string::npos);
        EXPECT_NE(json.find("\"pointLightRequestedCount\": 3"),
                  std::string::npos);
        EXPECT_NE(json.find("\"pointLightOverflowCount\": 1"),
                  std::string::npos);
        EXPECT_NE(json.find("\"spotLightCapacity\": 4"),
                  std::string::npos);
        EXPECT_NE(json.find("\"csmInputsReady\": true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"csmExecutionReady\": true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"iblInputsReady\": true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"iblExecutionReady\": true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"deformation\": {"), std::string::npos);
        EXPECT_NE(json.find("\"deformationContentHash\": 22136"),
                  std::string::npos);
        EXPECT_NE(json.find("\"skinnedObjectCount\": 1"),
                  std::string::npos);
        EXPECT_NE(json.find("\"paletteMatrixCount\": 64"),
                  std::string::npos);
        EXPECT_NE(json.find("\"previousPaletteObjectCount\": 1"),
                  std::string::npos);
        EXPECT_NE(json.find("\"currentPaletteBufferReady\": true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"previousPaletteBufferReady\": true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"opaqueSkinnedDrawCount\": 1"),
                  std::string::npos);
        EXPECT_NE(json.find("\"shadowSkinnedDrawCount\": 2"),
                  std::string::npos);
        EXPECT_NE(json.find("\"velocitySkinnedDrawCount\": 1"),
                  std::string::npos);
        EXPECT_NE(json.find("\"velocityPreviousPaletteDrawCount\": 1"),
                  std::string::npos);
        EXPECT_NE(json.find("\"postProcess\": {"), std::string::npos);
        EXPECT_NE(json.find("\"planContentHash\": 39612"),
                  std::string::npos);
        EXPECT_NE(json.find("\"toneMappingPassCount\": 1"),
                  std::string::npos);
        EXPECT_NE(json.find("\"bloomPassCount\": 1"),
                  std::string::npos);
        EXPECT_NE(json.find("\"colorGradingPassCount\": 1"),
                  std::string::npos);
        EXPECT_NE(json.find("\"fxaaPassCount\": 1"),
                  std::string::npos);
        EXPECT_NE(json.find("\"taaPassCount\": 0"),
                  std::string::npos);
        EXPECT_NE(json.find("\"antiAliasingMode\": \"fxaa\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"taaRequested\": false"),
                  std::string::npos);
        EXPECT_NE(json.find("\"inputColorDomain\": \"scene-linear-hdr\""),
                  std::string::npos);
        EXPECT_NE(json.find(
                      "\"plannedOutputColorDomain\": \"display-encoded-ldr\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"storedOutputColorDomain\": \"display-encoded-ldr\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"finalOutputFormat\": \"rgba8-unorm\""),
                  std::string::npos);
        EXPECT_NE(json.find(
                      "\"toneMappingTransfer\": \"scene-linear-to-display-encoded\""),
                  std::string::npos);
        EXPECT_NE(json.find(
                      "\"presentationTransfer\": \"scene-linear-to-display-encoded\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"displayLinearOutputReady\": false"),
                  std::string::npos);
        EXPECT_NE(json.find("\"executionSucceeded\": true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"presented\": true"), std::string::npos);
        EXPECT_NE(json.find("\"succeeded\": true"), std::string::npos);
        EXPECT_NE(json.find("\"features\": ["), std::string::npos);
        EXPECT_NE(json.find("\"featureId\": 1"), std::string::npos);
        EXPECT_NE(json.find("\"phase\": \"geometry\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"requirement\": \"required\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"selectedFallbackFeatureId\": 0"),
                  std::string::npos);
        EXPECT_NE(json.find("\"fallbackForFeatureId\": 0"),
                  std::string::npos);
        EXPECT_NE(json.find("\"executedGraphPassCount\": 2"),
                  std::string::npos);
        EXPECT_NE(json.find("\"satisfied\": true"), std::string::npos);

        EXPECT_NE(json.find("\"ambientOcclusion\": {"),
                  std::string::npos);
        EXPECT_NE(json.find("\"hybridRayTracing\": {"),
                  std::string::npos);
        EXPECT_NE(json.find("\"shadowRoute\": \"none\""),
                  std::string::npos);

        RVX::RenderExecutionReceipt legacyV2Receipt =
            MakeSuccessfulRenderExecutionReceipt(91);
        legacyV2Receipt.schemaVersion = 2;
        legacyV2Receipt.lighting = {};
        legacyV2Receipt.deformation = {};
        legacyV2Receipt.postProcess = {};
        report.visualContractReceipt->renderExecutionReceipt = legacyV2Receipt;
        std::ostringstream legacyV2Stream;
        RVX::WriteSampleReportJson(legacyV2Stream, report);
        EXPECT_NE(legacyV2Stream.str().find("\"schemaVersion\": 2"),
                  std::string::npos);
        EXPECT_EQ(legacyV2Stream.str().find("\"lighting\":"),
                  std::string::npos);
        EXPECT_EQ(legacyV2Stream.str().find("\"deformation\":"),
                  std::string::npos);
        EXPECT_EQ(legacyV2Stream.str().find("\"postProcess\":"),
                  std::string::npos);

        RVX::RenderExecutionReceipt legacyV4Receipt =
            MakeSuccessfulRenderExecutionReceipt(91);
        legacyV4Receipt.schemaVersion = 4;
        legacyV4Receipt.deformation = {};
        legacyV4Receipt.postProcess = {};
        report.visualContractReceipt->renderExecutionReceipt = legacyV4Receipt;
        std::ostringstream legacyV4Stream;
        RVX::WriteSampleReportJson(legacyV4Stream, report);
        EXPECT_NE(legacyV4Stream.str().find("\"schemaVersion\": 4"),
                  std::string::npos);
        EXPECT_NE(legacyV4Stream.str().find("\"lighting\":"),
                  std::string::npos);
        EXPECT_EQ(legacyV4Stream.str().find("\"deformation\":"),
                  std::string::npos);
        EXPECT_EQ(legacyV4Stream.str().find("\"postProcess\":"),
                  std::string::npos);

        RVX::RenderExecutionReceipt unreleasedV3Receipt = legacyV2Receipt;
        unreleasedV3Receipt.schemaVersion = 3;
        report.visualContractReceipt->renderExecutionReceipt = unreleasedV3Receipt;
        std::ostringstream unreleasedV3Stream;
        RVX::WriteSampleReportJson(unreleasedV3Stream, report);
        EXPECT_EQ(unreleasedV3Stream.str().find("\"deferredOpaque\":"),
                  std::string::npos);
        EXPECT_EQ(unreleasedV3Stream.str().find("\"lighting\":"),
                  std::string::npos);

        RVX::RenderExecutionReceipt legacyV1Receipt = legacyV2Receipt;
        legacyV1Receipt.schemaVersion = 1;
        legacyV1Receipt.deferredOpaque = {};
        report.visualContractReceipt->renderExecutionReceipt = legacyV1Receipt;
        std::ostringstream legacyV1Stream;
        RVX::WriteSampleReportJson(legacyV1Stream, report);
        EXPECT_NE(legacyV1Stream.str().find("\"schemaVersion\": 1"),
                  std::string::npos);
        EXPECT_EQ(legacyV1Stream.str().find("\"deferredOpaque\":"),
                  std::string::npos);
        EXPECT_EQ(legacyV1Stream.str().find("\"lighting\":"),
                  std::string::npos);
    }

    TEST(SampleCLIValidation,
         SchemaTenRenderReceiptSerializesAmbientOcclusionVelocityPayload)
    {
        RVX::SampleReport report;
        report.schemaVersion = RVX::RVX_SAMPLE_REPORT_SCHEMA_VERSION;
        report.visualContractReceipt.emplace();
        report.visualContractReceipt->renderExecutionReceipt =
            MakeSuccessfulRenderExecutionReceipt(91);
        RVX::RenderExecutionReceipt& receipt =
            *report.visualContractReceipt->renderExecutionReceipt;
        receipt.schemaVersion = 10;
        receipt.ambientOcclusion.planContentHash = 0xA013ULL;
        receipt.ambientOcclusion.requested = true;
        receipt.ambientOcclusion.pipelineReady = true;
        receipt.ambientOcclusion.inputsReady = true;
        receipt.ambientOcclusion.executed = true;
        receipt.ambientOcclusion.linearDepthInputReady = true;
        receipt.ambientOcclusion.worldNormalInputReady = true;
        receipt.ambientOcclusion.velocityInputReady = true;
        receipt.ambientOcclusion.velocityProducerExecuted = true;
        receipt.ambientOcclusion.outputPublished = true;
        receipt.ambientOcclusion.outputFormat = RVX::RHIFormat::R8_UNORM;
        receipt.ambientOcclusion.horizonSearchGraphPassCount = 1;
        receipt.ambientOcclusion.bilateralDenoiseGraphPassCount = 2;
        receipt.ambientOcclusion.temporalResolveGraphPassCount = 1;
        receipt.ambientOcclusion.historyAvailableBefore = true;
        receipt.ambientOcclusion.historySampled = true;
        receipt.ambientOcclusion.motionReprojectionUsed = true;
        receipt.ambientOcclusion.viewProjectionMotionObserved = true;
        receipt.ambientOcclusion.historyCandidateProduced = true;
        receipt.ambientOcclusion.historyCommitted = true;
        receipt.ambientOcclusion.historyResetReasons =
            RVX::RenderHistoryResetReason::None;
        receipt.ambientOcclusion.aoFieldContentHash = 0xA0FULL;
        receipt.ambientOcclusion.lightingConsumerExecuted = true;
        receipt.ambientOcclusion.lightingAOFieldContentHash = 0xA0FULL;
        receipt.ambientOcclusion.velocityFieldIdentity = 0xF013ULL;
        receipt.ambientOcclusion.consumerMask = 0x7u;
        receipt.ambientOcclusion.probeRequested = true;
        receipt.ambientOcclusion.probeReady = true;
        receipt.ambientOcclusion.probeSampleCount = 16;
        receipt.ambientOcclusion.probeOccludedSampleCount = 6;
        receipt.ambientOcclusion.probeMinVisibility = 0.1f;
        receipt.ambientOcclusion.probeMeanVisibility = 0.58f;
        receipt.ambientOcclusion.probeMaxVisibility = 0.95f;

        std::ostringstream stream;
        RVX::WriteSampleReportJson(stream, report);
        const std::string json = stream.str();
        EXPECT_NE(json.find("\"schemaVersion\": 10"),
                  std::string::npos);
        EXPECT_NE(json.find("\"ambientOcclusion\": {\"planContentHash\": 40979"),
                  std::string::npos);
        EXPECT_NE(json.find("\"outputFormat\": \"r8-unorm\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"bilateralDenoiseGraphPassCount\": 2"),
                  std::string::npos);
        EXPECT_NE(json.find("\"lightingConsumerExecuted\": true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"velocityInputReady\": true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"velocityProducerExecuted\": true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"motionReprojectionUsed\": true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"viewProjectionMotionObserved\": true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"velocityFieldIdentity\": 61459"),
                  std::string::npos);
        EXPECT_NE(json.find("\"consumerMask\": 7"),
                  std::string::npos);
        EXPECT_NE(json.find("\"probeMeanVisibility\": 0.58"),
                  std::string::npos);
        EXPECT_NE(json.find("\"contractCompletedWithoutGraphWork\": false"),
                  std::string::npos);
    }

    TEST(SampleCLIValidation,
         SchemaElevenRenderReceiptSerializesToonExecutionPayloads)
    {
        RVX::SampleReport report;
        report.schemaVersion = RVX::RVX_SAMPLE_REPORT_SCHEMA_VERSION;
        report.visualContractReceipt.emplace();
        report.visualContractReceipt->renderExecutionReceipt =
            MakeSuccessfulToonRenderExecutionReceipt(91);

        std::ostringstream stream;
        RVX::WriteSampleReportJson(stream, report);
        const std::string json = stream.str();
        EXPECT_NE(json.find("\"schemaVersion\": 11"), std::string::npos);
        EXPECT_NE(json.find("\"toonLighting\": {"), std::string::npos);
        EXPECT_NE(json.find("\"programFamilyHash\": "), std::string::npos);
        EXPECT_NE(json.find("\"toonOutline\": {"), std::string::npos);
        EXPECT_NE(json.find("\"qualificationRequested\": true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"qualificationReady\": true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"renderWidth\": 640"), std::string::npos);
        EXPECT_NE(json.find("\"renderHeight\": 360"), std::string::npos);
    }

    TEST(SampleCLIValidation,
         SchemaTwelveRenderReceiptSerializesHybridRayTracingExecutionPayload)
    {
        RVX::SampleReport report;
        report.schemaVersion = RVX::RVX_SAMPLE_REPORT_SCHEMA_VERSION;
        report.visualContractReceipt.emplace();
        report.visualContractReceipt->renderExecutionReceipt =
            MakeSuccessfulHybridRayTracingReceipt(91);

        std::ostringstream stream;
        RVX::WriteSampleReportJson(stream, report);
        const std::string json = stream.str();
        EXPECT_NE(json.find("\"schemaVersion\": 12"), std::string::npos);
        EXPECT_NE(json.find("\"hybridRayTracing\": {"),
                  std::string::npos);
        EXPECT_NE(json.find("\"planIdentityHash\": "),
                  std::string::npos);
        EXPECT_NE(json.find("\"scenePreparationGeneration\": 7"),
                  std::string::npos);
        EXPECT_NE(json.find("\"sceneStructuralHash\": "),
                  std::string::npos);
        EXPECT_NE(json.find("\"shadowRoute\": \"ray-traced\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"reflectionRoute\": \"ray-traced\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"spatialDenoiseRecorded\": true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"temporalResolveRecorded\": true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"compositeUsedDenoisedSource\": true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"submissionAccepted\": true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"presentationQualified\": true"),
                  std::string::npos);
    }

    TEST(SampleCLIValidation,
         RenderExecutionReceiptIsFailClosedInV21AndV20RemainsLegacy)
    {
        RVX::SampleReport current;
        current.schemaVersion = RVX::RVX_SAMPLE_REPORT_SCHEMA_VERSION;
        std::ostringstream currentStream;
        RVX::WriteSampleReportJson(currentStream, current);
        EXPECT_NE(currentStream.str().find(
                      "\"visualContractReceipt\": null"),
                  std::string::npos);

        RVX::SampleReport schema20;
        schema20.schemaVersion =
            RVX::RVX_SAMPLE_REPORT_PRE_RENDER_EXECUTION_RECEIPT_SCHEMA_VERSION;
        schema20.visualContractReceipt.emplace();
        schema20.visualContractReceipt->renderExecutionReceipt =
            MakeSuccessfulRenderExecutionReceipt();
        std::ostringstream schema20Stream;
        RVX::WriteSampleReportJson(schema20Stream, schema20);
        EXPECT_NE(schema20Stream.str().find("\"visualContractReceipt\": {"),
                  std::string::npos);
        EXPECT_EQ(schema20Stream.str().find("\"renderExecutionReceipt\""),
                  std::string::npos);

        RVX::SampleReport legacy;
        legacy.schemaVersion = 19;
        legacy.visualContractReceipt.emplace();
        std::ostringstream legacyStream;
        RVX::WriteSampleReportJson(legacyStream, legacy);
        EXPECT_EQ(legacyStream.str().find("\"visualContractReceipt\""),
                  std::string::npos);

        RVX::SampleReport unavailable;
        unavailable.visualContractReceipt.emplace();
        std::ostringstream unavailableStream;
        RVX::WriteSampleReportJson(unavailableStream, unavailable);
        EXPECT_NE(unavailableStream.str().find(
                      "\"observedPipeline\": \"unavailable\""),
                  std::string::npos);
        EXPECT_NE(unavailableStream.str().find(
                      "\"renderExecutionReceipt\": null"),
                  std::string::npos);
    }

    TEST(SampleCLIValidation, WorkloadScaleMappingIsDeterministic)
    {
        struct ExpectedProfile
        {
            RVX::SampleWorkloadScale scale;
            RVX::uint32 objectCount;
            RVX::uint32 dirtyObjectCount;
            RVX::uint32 churnObjectCount;
            RVX::uint32 gpuCullingCapacity;
            const char* name;
        };
        constexpr ExpectedProfile expected[] = {
            {RVX::SampleWorkloadScale::PullRequest, 1000u, 10u, 100u, 2048u,
             "pr"},
            {RVX::SampleWorkloadScale::Nightly, 10000u, 100u, 1000u, 16384u,
             "nightly"},
            {RVX::SampleWorkloadScale::Qualification,
             100000u,
             1000u,
             10000u,
             131072u,
             "qualification"},
        };

        for (const ExpectedProfile& item : expected)
        {
            const RVX::SampleWorkloadProfile profile =
                RVX::BuildSampleWorkloadProfile(item.scale);
            EXPECT_EQ(profile.scale, item.scale);
            EXPECT_EQ(profile.objectCount, item.objectCount);
            EXPECT_EQ(profile.dirtyObjectCount, item.dirtyObjectCount);
            EXPECT_EQ(profile.churnObjectCount, item.churnObjectCount);
            EXPECT_EQ(profile.gpuCullingCapacity, item.gpuCullingCapacity);
            EXPECT_STREQ(RVX::GetSampleWorkloadScaleName(item.scale),
                         item.name);
        }
    }

    TEST(SampleCLIValidation, ParsesWorkloadScale)
    {
        const char* argv[] = {
            "RenderVerseSamples",
            "--sample",
            "gpu-driven",
            "--workload-scale",
            "nightly",
        };

        RVX::SampleRunnerCLIOptions options;
        std::string error;
        ASSERT_TRUE(RVX::ParseSampleRunnerCLI(
            static_cast<int>(std::size(argv)), argv, options, &error))
            << error;
        EXPECT_EQ(options.workloadScale, RVX::SampleWorkloadScale::Nightly);
        EXPECT_TRUE(options.workloadScaleExplicit);
    }

    TEST(SampleCLIValidation,
         RunnerQualificationCaptureUsesBoundedReadinessWait)
    {
        const char* argv[] = {
            "RenderVerseSamples",
            "--gpu-scene-culling-qualification",
        };

        RVX::SampleRunnerCLIOptions options;
        std::string error;
        ASSERT_TRUE(RVX::ParseSampleRunnerCLI(
            static_cast<int>(std::size(argv)), argv, options, &error))
            << error;
        EXPECT_TRUE(options.common.gpuSceneCullingQualificationEnabled);
        EXPECT_TRUE(options.waitReady);
        EXPECT_EQ(options.common.frames, 1u);
        EXPECT_GE(options.readyMaxFrames, options.common.frames);
    }

    TEST(SampleCLIValidation,
         RunnerExposureProbeUsesBoundedReadinessWait)
    {
        const char* argv[] = {
            "RenderVerseSamples",
            "--exposure-qualification-probe",
        };

        RVX::SampleRunnerCLIOptions options;
        std::string error;
        ASSERT_TRUE(RVX::ParseSampleRunnerCLI(
            static_cast<int>(std::size(argv)), argv, options, &error))
            << error;
        EXPECT_TRUE(options.common.exposureQualificationProbeEnabled);
        EXPECT_TRUE(options.waitReady);
        EXPECT_EQ(options.common.frames, 1u);
        EXPECT_GE(options.readyMaxFrames, options.common.frames);
    }

    TEST(SampleCLIValidation,
         RunnerAmbientOcclusionProbeUsesBoundedReadinessWait)
    {
        const char* argv[] = {
            "RenderVerseSamples",
            "--ambient-occlusion-qualification-probe",
        };

        RVX::SampleRunnerCLIOptions options;
        std::string error;
        ASSERT_TRUE(RVX::ParseSampleRunnerCLI(
            static_cast<int>(std::size(argv)), argv, options, &error))
            << error;
        EXPECT_TRUE(
            options.common.ambientOcclusionQualificationProbeEnabled);
        EXPECT_TRUE(options.waitReady);
        EXPECT_EQ(options.common.frames, 1u);
        EXPECT_GE(options.readyMaxFrames, options.common.frames);
    }

    TEST(SampleCLIValidation,
         RequiredGPUCullingQualificationLanesFailClosedWithoutOrMasking)
    {
        const auto makeCompletedLane = []()
        {
            RVX::SampleGPUSceneCullingQualification lane;
            lane.requested = true;
            lane.required = true;
            lane.submissionAccepted = true;
            lane.completionObserved = true;
            lane.compared = true;
            lane.matched = true;
            lane.inputCoverageCompared = true;
            lane.inputCoverageMatched = true;
            lane.directVisibilityCoverageCompared = true;
            lane.directVisibilityCoverageMatched = true;
            lane.cullOutputsCompared = true;
            lane.cullOutputsMatched = true;
            lane.indirectArgumentsCompared = true;
            lane.indirectArgumentsMatched = true;
            return lane;
        };

        RVX::SampleRenderDiagnostics diagnostics;
        diagnostics.gpuSceneDepthQualification = makeCompletedLane();
        diagnostics.gpuSceneOpaqueQualification = makeCompletedLane();
        EXPECT_TRUE(RVX::HasRequiredGPUCullingQualificationComparison(
            diagnostics));
        EXPECT_TRUE(RVX::HasRequiredGPUCullingQualificationMatch(diagnostics));

        // A successful Depth lane must not OR-mask an Opaque value mismatch.
        diagnostics.gpuSceneOpaqueQualification.matched = false;
        diagnostics.gpuSceneOpaqueQualification.cullOutputsMatched = false;
        EXPECT_TRUE(RVX::HasRequiredGPUCullingQualificationComparison(
            diagnostics));
        EXPECT_FALSE(RVX::HasRequiredGPUCullingQualificationMatch(diagnostics));

        // A completed Depth lane must also not mask a required Opaque lane
        // that has neither completed nor compared its post-fence evidence.
        diagnostics.gpuSceneOpaqueQualification = makeCompletedLane();
        diagnostics.gpuSceneOpaqueQualification.completionObserved = false;
        diagnostics.gpuSceneOpaqueQualification.compared = false;
        diagnostics.gpuSceneOpaqueQualification.inputCoverageCompared = false;
        diagnostics.gpuSceneOpaqueQualification.directVisibilityCoverageCompared = false;
        diagnostics.gpuSceneOpaqueQualification.cullOutputsCompared = false;
        diagnostics.gpuSceneOpaqueQualification.indirectArgumentsCompared = false;
        EXPECT_FALSE(RVX::HasRequiredGPUCullingQualificationComparison(
            diagnostics));
        EXPECT_FALSE(RVX::HasRequiredGPUCullingQualificationMatch(diagnostics));

        diagnostics = {};
        EXPECT_FALSE(RVX::HasRequiredGPUCullingQualificationComparison(
            diagnostics));
    }

    TEST(SampleCLIValidation, RejectsInvalidWorkloadScale)
    {
        const char* argv[] = {
            "RenderVerseSamples",
            "--workload-scale",
            "weekly",
        };

        RVX::SampleRunnerCLIOptions options;
        std::string error;
        EXPECT_FALSE(RVX::ParseSampleRunnerCLI(
            static_cast<int>(std::size(argv)), argv, options, &error));
        EXPECT_NE(error.find("--workload-scale"), std::string::npos);
    }

    TEST(SampleCLIValidation, WorkloadScaleDefaultsToPullRequest)
    {
        const char* argv[] = {"RenderVerseSamples"};

        RVX::SampleRunnerCLIOptions options;
        std::string error;
        ASSERT_TRUE(RVX::ParseSampleRunnerCLI(
            static_cast<int>(std::size(argv)), argv, options, &error))
            << error;
        EXPECT_EQ(options.workloadScale,
                  RVX::SampleWorkloadScale::PullRequest);
        EXPECT_FALSE(options.workloadScaleExplicit);

        const RVX::SampleWorkloadProfile profile{};
        EXPECT_EQ(profile.scale, RVX::SampleWorkloadScale::PullRequest);
        EXPECT_EQ(profile.objectCount, 1000u);
        EXPECT_EQ(profile.dirtyObjectCount, 10u);
        EXPECT_EQ(profile.churnObjectCount, 100u);
        EXPECT_EQ(profile.gpuCullingCapacity, 2048u);
    }

    TEST(SampleCLIValidation, ResolvesWorkloadReadinessBudgetDefaults)
    {
        struct ExpectedBudget
        {
            RVX::SampleWorkloadScale scale;
            RVX::uint32 timeoutMs;
            RVX::uint32 maximumFrames;
        };
        constexpr ExpectedBudget expected[] = {
            {RVX::SampleWorkloadScale::PullRequest, 120000u, 600u},
            {RVX::SampleWorkloadScale::Nightly, 900000u, 2000u},
            {RVX::SampleWorkloadScale::Qualification, 3600000u, 10000u},
        };
        const char* argv[] = {
            "RenderVerseSamples",
            "--wait-ready",
            "--frames",
            "1",
        };

        RVX::SampleRunnerCLIOptions options;
        std::string error;
        ASSERT_TRUE(RVX::ParseSampleRunnerCLI(
            static_cast<int>(std::size(argv)), argv, options, &error))
            << error;
        EXPECT_FALSE(options.readyTimeoutExplicit);
        EXPECT_FALSE(options.readyMaxFramesExplicit);

        for (const ExpectedBudget& item : expected)
        {
            const RVX::SampleWorkloadProfile workload =
                RVX::BuildSampleWorkloadProfile(item.scale);
            const RVX::SampleReadinessWaitBudget budget =
                RVX::ResolveSampleReadinessWaitBudget(options, &workload);
            EXPECT_EQ(budget.timeoutMs, item.timeoutMs);
            EXPECT_EQ(budget.maximumFrames, item.maximumFrames);
        }
    }

    TEST(SampleCLIValidation, ExplicitReadinessLimitsOverrideWorkloadDefaults)
    {
        const char* argv[] = {
            "RenderVerseSamples",
            "--wait-ready",
            "--frames",
            "1",
            "--ready-timeout-ms",
            "17",
            "--ready-max-frames",
            "42",
        };

        RVX::SampleRunnerCLIOptions options;
        std::string error;
        ASSERT_TRUE(RVX::ParseSampleRunnerCLI(
            static_cast<int>(std::size(argv)), argv, options, &error))
            << error;
        EXPECT_TRUE(options.readyTimeoutExplicit);
        EXPECT_TRUE(options.readyMaxFramesExplicit);

        const RVX::SampleWorkloadProfile workload =
            RVX::BuildSampleWorkloadProfile(
                RVX::SampleWorkloadScale::Qualification);
        const RVX::SampleReadinessWaitBudget budget =
            RVX::ResolveSampleReadinessWaitBudget(options, &workload);
        EXPECT_EQ(budget.timeoutMs, 17u);
        EXPECT_EQ(budget.maximumFrames, 42u);
    }

    TEST(SampleCLIValidation, ParsesMaximumReadyTimeout)
    {
        const char* accepted[] = {
            "RenderVerseSamples",
            "--ready-timeout-ms",
            "3600000",
        };
        RVX::SampleRunnerCLIOptions options;
        std::string error;
        ASSERT_TRUE(RVX::ParseSampleRunnerCLI(
            static_cast<int>(std::size(accepted)), accepted, options, &error))
            << error;
        EXPECT_EQ(options.readyTimeoutMs, 3600000u);
        EXPECT_TRUE(options.readyTimeoutExplicit);

        const char* rejected[] = {
            "RenderVerseSamples",
            "--ready-timeout-ms",
            "3600001",
        };
        options = {};
        error.clear();
        EXPECT_FALSE(RVX::ParseSampleRunnerCLI(
            static_cast<int>(std::size(rejected)), rejected, options, &error));
        EXPECT_NE(error.find("--ready-timeout-ms"), std::string::npos);
    }

    TEST(SampleCLIValidation, ReadinessBudgetPreservesNonWorkloadCompatibility)
    {
        const char* argv[] = {
            "RenderVerseSamples",
            "--wait-ready",
            "--frames",
            "8",
        };

        RVX::SampleRunnerCLIOptions options;
        std::string error;
        ASSERT_TRUE(RVX::ParseSampleRunnerCLI(
            static_cast<int>(std::size(argv)), argv, options, &error))
            << error;
        const RVX::SampleReadinessWaitBudget budget =
            RVX::ResolveSampleReadinessWaitBudget(options, nullptr);
        EXPECT_EQ(budget.timeoutMs, 120000u);
        EXPECT_EQ(budget.maximumFrames, 120u);
    }

    TEST(SampleCLIValidation,
         ReadinessBudgetDoesNotApplyWorkloadDefaultsWithoutReadinessWait)
    {
        const char* argv[] = {"RenderVerseSamples"};

        RVX::SampleRunnerCLIOptions options;
        std::string error;
        ASSERT_TRUE(RVX::ParseSampleRunnerCLI(
            static_cast<int>(std::size(argv)), argv, options, &error))
            << error;

        const RVX::SampleWorkloadProfile workload =
            RVX::BuildSampleWorkloadProfile(
                RVX::SampleWorkloadScale::Qualification);
        const RVX::SampleReadinessWaitBudget budget =
            RVX::ResolveSampleReadinessWaitBudget(options, &workload);
        EXPECT_EQ(budget.timeoutMs, options.readyTimeoutMs);
        EXPECT_EQ(budget.maximumFrames, options.readyMaxFrames);
    }

    TEST(SampleCLIValidation, ParsesModelViewerLifetimeQualification)
    {
        const char* argv[] = {
            "RenderVerseSamples",
            "--sample", "model-viewer",
            "--backend", "dx12",
            "--render-path", "gpu-driven",
            "--deterministic-orbit",
            "--lifetime-report", "artifacts/lifetime.json",
            "--lifetime-warmup-frames", "120",
            "--lifetime-observation-frames", "480",
            "--lifetime-min-duration-ms", "300000",
            "--lifetime-resize-interval", "2400",
            "--lifetime-resize-settle-frames", "60",
        };

        RVX::SampleRunnerCLIOptions options;
        std::string error;
        ASSERT_TRUE(RVX::ParseSampleRunnerCLI(
            static_cast<int>(std::size(argv)), argv, options, &error))
            << error;
        EXPECT_TRUE(options.HasLifetimeQualification());
        EXPECT_TRUE(options.waitReady);
        EXPECT_TRUE(options.deterministicCameraOrbit);
        EXPECT_EQ(options.common.frames, 1u);
        EXPECT_EQ(options.renderPath, RVX::SampleRenderPath::GPUDriven);
        EXPECT_EQ(options.lifetimeConfig.warmupFrames, 120u);
        EXPECT_EQ(options.lifetimeConfig.observationFrames, 480u);
        EXPECT_EQ(options.lifetimeConfig.minimumDurationMs, 300000u);
        EXPECT_EQ(options.lifetimeConfig.resizeIntervalFrames, 2400u);
        EXPECT_EQ(options.lifetimeConfig.resizeSettleFrames, 60u);
    }

    TEST(SampleCLIValidation, LifetimeQualificationRequiresFormalOrbitPath)
    {
        const char* argv[] = {
            "RenderVerseSamples",
            "--sample", "model-viewer",
            "--backend", "dx12",
            "--lifetime-report", "artifacts/lifetime.json",
        };

        RVX::SampleRunnerCLIOptions options;
        std::string error;
        EXPECT_FALSE(RVX::ParseSampleRunnerCLI(
            static_cast<int>(std::size(argv)), argv, options, &error));
        EXPECT_NE(error.find("--deterministic-orbit"), std::string::npos);
    }

    TEST(SampleCLIValidation, ParsesFrameworkAssessmentProfileAndBaseline)
    {
        const char* argv[] = {
            "RenderVerseSamples",
            "--sample", "scene-lifecycle",
            "--backend", "vulkan",
            "--assessment-report", "artifacts/assessment.json",
            "--assessment-profile", "qualification",
            "--assessment-baseline", "baselines/vulkan.json",
        };

        RVX::SampleRunnerCLIOptions options;
        std::string error;
        ASSERT_TRUE(RVX::ParseSampleRunnerCLI(
            static_cast<int>(std::size(argv)), argv, options, &error))
            << error;
        EXPECT_TRUE(options.HasFrameworkAssessment());
        EXPECT_TRUE(options.waitReady);
        EXPECT_EQ(options.common.frames, 600u);
        EXPECT_EQ(options.assessmentProfile,
                  RVX::SampleAssessmentProfile::Qualification);
        EXPECT_EQ(options.assessmentBaselinePath,
                  "baselines/vulkan.json");
        EXPECT_STREQ(RVX::GetSampleAssessmentProfileName(
                         options.assessmentProfile),
                     "qualification");
    }

    TEST(SampleCLIValidation, AssessmentBaselineRequiresAssessmentReport)
    {
        const char* argv[] = {
            "RenderVerseSamples",
            "--assessment-baseline", "baseline.json",
        };

        RVX::SampleRunnerCLIOptions options;
        std::string error;
        EXPECT_FALSE(RVX::ParseSampleRunnerCLI(
            static_cast<int>(std::size(argv)), argv, options, &error));
        EXPECT_NE(error.find("--assessment-report"), std::string::npos);
    }

    TEST(SampleCLIValidation, ParsesStrictRuntimeOnlyCookedShaderRoot)
    {
        const char* argv[] = {
            "RenderVerseSamples",
            "--sample", "water-surface",
            "--backend", "dx12",
            "--runtime-only-composition",
            "--cooked-shader-root", "Artifacts/Shaders/DX12",
        };

        RVX::SampleRunnerCLIOptions options;
        std::string error;
        ASSERT_TRUE(RVX::ParseSampleRunnerCLI(
            static_cast<int>(std::size(argv)), argv, options, &error))
            << error;
        EXPECT_TRUE(options.runtimeOnlyComposition);
        EXPECT_EQ(options.cookedShaderRoot,
                  std::filesystem::path("Artifacts/Shaders/DX12"));
    }

    TEST(SampleCLIValidation, StrictRuntimeOnlyCookedShaderRootIsFailClosed)
    {
        const char* missingRoot[] = {
            "RenderVerseSamples", "--runtime-only-composition",
        };
        RVX::SampleRunnerCLIOptions missingRootOptions;
        std::string missingRootError;
        EXPECT_FALSE(RVX::ParseSampleRunnerCLI(
            static_cast<int>(std::size(missingRoot)), missingRoot,
            missingRootOptions, &missingRootError));
        EXPECT_NE(missingRootError.find("--cooked-shader-root"),
                  std::string::npos);

        const char* missingMode[] = {
            "RenderVerseSamples", "--cooked-shader-root", "Artifacts/Shaders/DX12",
        };
        RVX::SampleRunnerCLIOptions missingModeOptions;
        std::string missingModeError;
        EXPECT_FALSE(RVX::ParseSampleRunnerCLI(
            static_cast<int>(std::size(missingMode)), missingMode,
            missingModeOptions, &missingModeError));
        EXPECT_NE(missingModeError.find("--runtime-only-composition"),
                  std::string::npos);
    }

    TEST(SampleCLIValidation, CurrentReportWritesRuntimeOnlyCompositionEvidence)
    {
        RVX::SampleReport report;
        report.schemaVersion = RVX::RVX_SAMPLE_REPORT_SCHEMA_VERSION;
        report.runtimeComposition.runtimeOnlyComposition = true;
        report.runtimeComposition.cookedShaderProvider = true;
        report.runtimeComposition.sourceShaderFallbackEnabled = false;
        report.runtimeComposition.cookedResourcePolicy = true;
        report.runtimeComposition.cookedShaderRoot = "Artifacts/Shaders/DX12";
        report.runtimeComposition.cookedResourceRoot = "Assets/Samples/cooked/water";
        report.runtimeComposition.shaderProgramProvider = "CookedShaderArtifactV2";

        std::ostringstream stream;
        RVX::WriteSampleReportJson(stream, report);
        const std::string json = stream.str();
        EXPECT_NE(json.find("\"runtimeComposition\": {"), std::string::npos);
        EXPECT_NE(json.find("\"runtimeOnlyComposition\": true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"cookedShaderProvider\": true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"sourceShaderFallbackEnabled\": false"),
                  std::string::npos);
        EXPECT_NE(json.find("\"qualifiedCookedShaderProvider\": true"),
                  std::string::npos);
    }

    TEST(SampleCLIValidation, NonSmokeAssessmentProfilesRequireAssessmentReport)
    {
        for (const char* profile : {"qualification", "benchmark"})
        {
            const char* argv[] = {
                "RenderVerseSamples",
                "--sample", "rendering-stress",
                "--assessment-profile", profile,
            };

            RVX::SampleRunnerCLIOptions options;
            std::string error;
            EXPECT_FALSE(RVX::ParseSampleRunnerCLI(
                static_cast<int>(std::size(argv)), argv, options, &error));
            EXPECT_NE(error.find("--assessment-report"), std::string::npos);
        }

        const char* accepted[] = {
            "RenderVerseSamples",
            "--sample", "rendering-stress",
            "--assessment-profile", "benchmark",
            "--assessment-report", "artifacts/benchmark-assessment.json",
        };
        RVX::SampleRunnerCLIOptions options;
        std::string error;
        EXPECT_TRUE(RVX::ParseSampleRunnerCLI(
            static_cast<int>(std::size(accepted)), accepted, options, &error))
            << error;
        EXPECT_TRUE(options.HasFrameworkAssessment());
        EXPECT_EQ(options.assessmentProfile,
                  RVX::SampleAssessmentProfile::Benchmark);
    }

    TEST(SampleCLIValidation, LifetimeQualificationAcceptsStablePlateau)
    {
        RVX::SampleLifetimeQualificationConfig config;
        config.warmupFrames = 2;
        config.observationFrames = 3;
        RVX::SampleLifetimeQualificationMetadata metadata;
        metadata.sampleName = "model-viewer";
        metadata.assetId = "porsche";
        metadata.backend = RVX::RHIBackendType::DX12;
        metadata.renderPath = "direct";
        metadata.deterministicOrbit = true;
        RVX::SampleLifetimeQualification qualification(config, metadata);
        const RVX::SampleProcessMemorySnapshot memory{
            true,
            512ULL * 1024ULL * 1024ULL,
        };

        for (RVX::uint64 frame = 1; frame <= 5; ++frame)
        {
            qualification.Observe(MakeLifetimeSnapshot(frame),
                                  memory,
                                  frame * 16ULL);
        }

        ASSERT_TRUE(qualification.IsComplete());
        EXPECT_FALSE(qualification.HasFailed());
        const auto& report = qualification.GetReport();
        EXPECT_TRUE(report.pass);
        EXPECT_EQ(report.warmupFramesObserved, 2u);
        EXPECT_EQ(report.observationFramesObserved, 3u);
        EXPECT_EQ(report.plateauCount, 1u);
        EXPECT_TRUE(report.failures.empty());
    }

    TEST(SampleCLIValidation, LifetimeQualificationFailsOnPoolGrowth)
    {
        RVX::SampleLifetimeQualificationConfig config;
        config.warmupFrames = 1;
        config.observationFrames = 2;
        RVX::SampleLifetimeQualification qualification(config, {});
        const RVX::SampleProcessMemorySnapshot memory{};

        qualification.Observe(MakeLifetimeSnapshot(1, 4), memory, 1);
        qualification.Observe(MakeLifetimeSnapshot(2, 5), memory, 2);

        ASSERT_TRUE(qualification.HasFailed());
        ASSERT_FALSE(qualification.GetReport().failures.empty());
        EXPECT_NE(qualification.GetReport().failures.front().find(
                      "Physical texture allocation count"),
                  std::string::npos);
    }

    TEST(SampleCLIValidation, LifetimeQualificationFailsOnNativeValidationError)
    {
        RVX::SampleLifetimeQualificationConfig config;
        config.warmupFrames = 1;
        config.observationFrames = 1;
        RVX::SampleLifetimeQualification qualification(config, {});
        RVX::RenderDiagnosticsSnapshot diagnostics = MakeLifetimeSnapshot(1);
        diagnostics.nativeValidation.errorCount = 1;

        qualification.Observe(diagnostics, {}, 1);

        ASSERT_TRUE(qualification.HasFailed());
        ASSERT_FALSE(qualification.GetReport().failures.empty());
        EXPECT_NE(qualification.GetReport().failures.front().find(
                      "Native graphics validation"),
                  std::string::npos);
    }

    TEST(SampleCLIValidation,
         LifetimeQualificationRejectsUnexpectedSurfaceFrameDrops)
    {
        RVX::SampleLifetimeQualificationConfig config;
        config.warmupFrames = 1;
        config.observationFrames = 2;
        RVX::SampleLifetimeQualification qualification(config, {});

        qualification.Observe(MakeLifetimeSnapshot(1), {}, 1);
        RVX::RenderDiagnosticsSnapshot dropped = MakeLifetimeSnapshot(2);
        dropped.frameTransport.surfaceIncompatibleDrops = 1;
        qualification.Observe(dropped, {}, 2);

        ASSERT_TRUE(qualification.HasFailed());
        ASSERT_FALSE(qualification.GetReport().failures.empty());
        EXPECT_NE(qualification.GetReport().failures.front().find(
                      "Surface-incompatible frame drop count"),
                  std::string::npos);
    }

    TEST(SampleCLIValidation, RejectsInvalidDimensions)
    {
        const char* argv[] = {"BasicRHI", "--width", "0"};

        RVX::SampleCLIOptions options;
        std::string error;
        EXPECT_FALSE(RVX::ParseSampleCLI(
            static_cast<int>(std::size(argv)),
            argv,
            options,
            &error));
        EXPECT_NE(error.find("--width"), std::string::npos);
    }

    TEST(SampleCLIValidation, WritesReportJsonContract)
    {
        RVX::SampleReport report;
        report.sampleName = "BasicRHI";
        report.requestedBackend = RVX::RHIBackendType::DX12;
        report.backend = RVX::RHIBackendType::DX12;
        report.frameCount = 1;
        report.width = 320;
        report.height = 180;
        report.quality = "default";
        report.renderPath = "gpu-driven";
        report.requestedRenderPath = "gpu-driven";
        report.actualRenderPath = "gpu-driven";
        report.requestedPhysicsBackend = "auto";
        report.actualPhysicsBackend = "built-in";
        report.physicsBackendFallbackActive = false;
        report.physicsDiagnosticsAvailable = true;
        report.ecsDiagnostics.available = true;
        report.ecsDiagnostics.sceneRuntimeId = 71;
        report.ecsDiagnostics.sceneSnapshotRevision = 19;
        report.ecsDiagnostics.entityCount = 4;
        report.ecsDiagnostics.physicsBodySideTableEntryCount = 2;
        report.ecsDiagnostics.legacyFallbackCount = 0;
        report.enabledFeatures = {"RHI", "RenderGraph"};
        report.unsupportedFeatures = {"ScreenshotCapture"};
        report.fallbackReasons = {"BasicRHI does not capture screenshots yet"};
        report.resourceDiagnostics = {"fixture loaded"};
        report.assetId = "fixture";
        report.assetPath = "fixture.gltf";
        report.assetKind = "model";
        report.catalogPath = "catalog.json";
        report.assetRoot = "Assets/Samples";
        report.assetLicenseSpdx = "CC0-1.0";
        report.assetLicenseFile = "LICENSE.txt";
        report.assetSourceName = "Generated fixture";
        report.assetSourceUri = "https://example.invalid/fixture";
        report.assetAuthor = "RenderVerseX contributors";
        report.assetAttribution = "Generated for automated validation.";
        report.assetRedistributable = true;
        report.assetLoaded = true;
        RVX::SampleReportAsset environment;
        environment.role = "environment";
        environment.id = "studio";
        environment.path = "studio.hdr";
        environment.kind = "environment";
        environment.licenseSpdx = "CC0-1.0";
        environment.redistributable = true;
        environment.loaded = true;
        report.assets.push_back(environment);
        report.readiness.waitRequested = true;
        report.readiness.ready = true;
        report.readiness.minimumFrames = 8;
        report.readiness.maximumFrames = 120;
        report.readiness.timeoutMs = 30000;
        report.renderDiagnostics.available = true;
        report.renderDiagnostics.renderAttempted = true;
        report.renderDiagnostics.rendered = true;
        report.renderDiagnostics.graphBuilt = true;
        report.renderDiagnostics.graphCompiled = true;
        report.renderDiagnostics.renderGraphTotalPasses = 3;
        report.renderDiagnostics.postProcessActualExecution =
            MakeQualifiedPostProcessActualExecution(67);
        const RVX::RenderDiagnosticsSnapshot toonSnapshot =
            MakeSuccessfulToonDiagnosticsSnapshot(67);
        report.renderDiagnostics.toonLightingActualExecution =
            RVX::ProjectSampleToonLightingExecutionDiagnostics(toonSnapshot);
        report.renderDiagnostics.toonOutlineActualExecution =
            RVX::ProjectSampleToonOutlineExecutionDiagnostics(toonSnapshot);
        report.renderDiagnostics.transparentAlphaBlendDrawCount = 1;
        report.renderDiagnostics.transparentPremultipliedDrawCount = 2;
        report.renderDiagnostics.transparentAdditiveDrawCount = 3;
        report.renderDiagnostics.transparentThinTransmissionDrawCount = 4;
        report.renderDiagnostics.transparentOpaqueSceneSnapshotReady = true;
        report.renderDiagnostics.transparentOpaqueSceneSnapshotCopied = true;
        report.renderDiagnostics.transparentReactiveMaskAttachmentBound = true;
        report.renderDiagnostics.transparentReactiveMaskWriteDrawCount = 4;
        report.renderDiagnostics.transparentPriorityBucketCount = 4;
        report.renderDiagnostics.transparentFallbackCount = 0;
        report.renderDiagnostics.nativeValidationAvailable = true;
        report.renderDiagnostics.nativeValidationEnabled = true;
        report.renderDiagnostics.nativeValidationReadComplete = true;
        report.renderDiagnostics.nativeValidationWarningCount = 2;
        report.renderDiagnostics.nativeValidationErrorCount = 3;
        report.renderDiagnostics.nativeValidationCorruptionCount = 4;
        report.renderDiagnostics.directionalShadowSamplingEnabled = true;
        report.renderDiagnostics.directionalShadowReason = "Enabled";
        report.renderDiagnostics.gpuDrivenPolicyDecisionAvailable = true;
        report.renderDiagnostics.gpuDrivenRequestedMode = "ForceEnabled";
        report.renderDiagnostics.gpuDrivenPolicyReason = "None";
        report.renderDiagnostics.gpuDrivenQualification = "Qualified";
        report.renderDiagnostics.gpuDrivenEnabled = true;
        report.renderDiagnostics.gpuDrivenGraphPassRecorded = true;
        report.renderDiagnostics.gpuDrivenExecutionRecorded = true;
        report.renderDiagnostics.gpuDrivenOpaqueIndirectSubmitted = true;
        report.renderDiagnostics.gpuDrivenOpaqueIndirectBatchCount = 2;
        report.renderDiagnostics.opaqueExecutionCompleted = true;
        report.renderDiagnostics.opaqueExecutedDrawCountAvailable = true;
        report.renderDiagnostics.opaqueExecutedDrawCount = 12;
        report.renderDiagnostics.opaqueMaterialBindingsAvailable = true;
        report.renderDiagnostics.opaqueMaterialBindingCount = 12;
        report.renderDiagnostics.opaqueMaterialFallbackBindingCount = 0;
        report.renderDiagnostics.opaqueMaterialTextureFlags = 4;
        report.renderDiagnostics.opaqueMaterialFallbackTextureFlags = 0;
        report.renderDiagnostics.renderPolicySelectedTier = "IndirectGrouped";
        report.renderDiagnostics.renderPolicyExecutedTier = "IndirectGrouped";
        report.renderDiagnostics.renderPolicyRequestAvailable = true;
        report.renderDiagnostics.renderPolicyPlanAvailable = true;
        report.renderDiagnostics.renderPolicyReportAvailable = true;
        report.renderDiagnostics.renderPolicyRequestFrameSequence = 1;
        report.renderDiagnostics.renderPolicyPlanFrameSequence = 1;
        report.renderDiagnostics.renderPolicyReportFrameSequence = 1;
        report.renderDiagnostics.completedPresentedFrameSequence = 1;
        report.renderDiagnostics.renderPolicyRequestedMode = "ForceEnabled";
        report.renderDiagnostics.renderPolicyExecutionStatus = "Completed";
        report.renderDiagnostics.gpuSceneOpaqueQualification.requested = true;
        report.renderDiagnostics.gpuSceneOpaqueQualification.required = true;
        report.renderDiagnostics.gpuSceneOpaqueQualification.submissionAccepted = true;
        report.renderDiagnostics.gpuSceneOpaqueQualification.completionObserved = true;
        report.renderDiagnostics.gpuSceneOpaqueQualification.compared = true;
        report.renderDiagnostics.gpuSceneOpaqueQualification.matched = true;
        report.renderDiagnostics.gpuSceneOpaqueQualification.inputCoverageCompared = true;
        report.renderDiagnostics.gpuSceneOpaqueQualification.inputCoverageMatched = true;
        report.renderDiagnostics.gpuSceneOpaqueQualification.directVisibilityCoverageCompared = true;
        report.renderDiagnostics.gpuSceneOpaqueQualification.directVisibilityCoverageMatched = true;
        report.renderDiagnostics.gpuSceneOpaqueQualification.cullOutputsCompared = true;
        report.renderDiagnostics.gpuSceneOpaqueQualification.cullOutputsMatched = true;
        report.renderDiagnostics.gpuSceneOpaqueQualification.indirectArgumentsCompared = true;
        report.renderDiagnostics.gpuSceneOpaqueQualification.indirectArgumentsMatched = true;
        report.renderDiagnostics.gpuSceneOpaqueQualification.rasterPayloadCompared = true;
        report.renderDiagnostics.gpuSceneOpaqueQualification.rasterPayloadMatched = true;
        report.renderDiagnostics.gpuSceneOpaqueQualification.expectedInputPacketCount = 7;
        report.renderDiagnostics.gpuSceneOpaqueQualification.expectedGPUInputPacketCount = 4;
        report.renderDiagnostics.gpuSceneOpaqueQualification.observedGPUInputPacketCount = 4;
        report.renderDiagnostics.gpuSceneOpaqueQualification.expectedDirectVisiblePacketCount = 3;
        report.renderDiagnostics.gpuSceneOpaqueQualification.observedGPUVisiblePacketCount = 4;
        report.renderDiagnostics.gpuSceneOpaqueQualification.missingDirectVisiblePacketCount = 0;
        report.renderDiagnostics.gpuSceneOpaqueQualification.gpuOnlyVisiblePacketCount = 1;
        report.renderDiagnostics.gpuSceneOpaqueQualification.directInputPacketCount = 2;
        report.renderDiagnostics.gpuSceneOpaqueQualification.skippedInputPacketCount = 1;
        report.renderDiagnostics.gpuSceneOpaqueQualification.expectedGPUInputIdentityHash = 17;
        report.renderDiagnostics.gpuSceneOpaqueQualification.observedGPUInputIdentityHash = 17;
        report.renderDiagnostics.gpuSceneOpaqueQualification.planPacketIdentityHash = 19;
        report.renderDiagnostics.gpuSceneOpaqueQualification.expectedDirectVisibleIdentityHash = 23;
        report.renderDiagnostics.gpuSceneOpaqueQualification.observedGPUVisibleIdentityHash = 29;
        report.renderDiagnostics.gpuSceneOpaqueQualification.expectedRasterPayloadHash = 31;
        report.renderDiagnostics.gpuSceneOpaqueQualification.observedRasterPayloadHash = 37;
        report.renderDiagnostics.gpuSceneOpaqueQualification.expectedIndirectArgumentsHash = 41;
        report.renderDiagnostics.gpuSceneOpaqueQualification.observedIndirectArgumentsHash = 43;
        report.renderDiagnostics.directOpaqueRasterTranscript = {
            true, 3u, 47u, 53u, 59u, 61u, 67u, 71u};
        report.renderDiagnostics.gpuSceneOpaqueQualification
            .tierOneRasterTranscript = {true, 3u, 47u, 53u, 59u, 61u, 67u, 71u};
        report.renderDiagnostics.gpuSceneOpaqueQualification
            .tierOneRasterTranscriptReference = {
                true, 3u, 47u, 53u, 59u, 61u, 67u, 71u};
        report.renderDiagnostics.gpuSceneOpaqueQualification
            .tierOneRasterTranscriptCompared = true;
        report.renderDiagnostics.gpuSceneOpaqueQualification
            .tierOneRasterTranscriptMatched = true;
        report.renderDiagnostics.gpuSceneOpaqueQualification
            .firstRasterTranscriptMismatchEntry = RVX::RVX_INVALID_INDEX;
        report.renderDiagnostics.gpuSceneOpaqueQualification.cpuPayloadBytes = 256;
        report.renderDiagnostics.gpuSceneOpaqueQualification.capturedTier = "IndirectGrouped";
        report.renderDiagnostics.gpuSceneQualificationTargetFrameSequence = 67;
        report.renderDiagnostics.gpuSceneQualificationTargetPublishedSequence = 67;
        report.renderDiagnostics.gpuSceneQualificationTargetSubmittedSequence = 67;
        report.renderDiagnostics.gpuSceneQualificationTargetPresentedSequence = 67;
        report.renderDiagnostics.directOpaqueRasterReadbackQualification = {
            true, true, true, true, true, true, true, true, true, true,
            "None", 67u, 9u, 1u, RVX::RVX_INVALID_INDEX,
            RVX::RVX_INVALID_INDEX, 256u, 71u,
            {true, 3u, 47u, 53u, 59u, 61u, 67u, 71u},
            {true, 3u, 47u, 53u, 59u, 61u, 67u, 71u}};
        report.renderDiagnostics
            .directOpaqueRasterReadbackQualificationTargetFrameSequence = 67;
        report.renderDiagnostics
            .directOpaqueRasterReadbackQualificationTargetPublishedSequence = 67;
        report.renderDiagnostics
            .directOpaqueRasterReadbackQualificationTargetSubmittedSequence = 67;
        report.renderDiagnostics
            .directOpaqueRasterReadbackQualificationTargetPresentedSequence = 67;
        report.pass = true;

        std::ostringstream stream;
        RVX::WriteSampleReportJson(stream, report);
        const std::string json = stream.str();

        EXPECT_NE(json.find("\"schemaId\": \"RVX.SampleReport\""), std::string::npos);
        EXPECT_NE(json.find("\"schemaVersion\": 33"), std::string::npos);
        EXPECT_NE(json.find("\"sampleName\": \"BasicRHI\""), std::string::npos);
        EXPECT_NE(json.find("\"category\": \"sample\""), std::string::npos);
        EXPECT_NE(json.find("\"requestedBackend\": \"dx12\""), std::string::npos);
        EXPECT_NE(json.find("\"backend\": \"dx12\""), std::string::npos);
        EXPECT_NE(json.find("\"frameCount\": 1"), std::string::npos);
        EXPECT_NE(json.find("\"renderPath\": \"gpu-driven\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"requestedRenderPath\": \"gpu-driven\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"actualRenderPath\": \"gpu-driven\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"required\": true"), std::string::npos);
        EXPECT_NE(json.find("\"inputCoverageMatched\": true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"directVisibilityCoverageMatched\": true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"cullOutputsMatched\": true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"indirectArgumentsMatched\": true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"rasterPayloadMatched\": true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"gpuOnlyVisiblePacketCount\": 1"),
                  std::string::npos);
        EXPECT_NE(json.find("\"expectedIndirectArgumentsHash\": 41"),
                  std::string::npos);
        EXPECT_NE(json.find("\"directOpaqueRasterTranscript\": {"),
                  std::string::npos);
        EXPECT_NE(json.find("\"directOpaqueRasterReadbackQualification\": {"),
                  std::string::npos);
        EXPECT_NE(json.find("\"allDirectDrawsInstanced\": true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"directOpaqueRasterReadbackQualificationTargetFrameSequence\": 67"),
                  std::string::npos);
        EXPECT_NE(json.find("\"tierOneRasterTranscript\": {"),
                  std::string::npos);
        EXPECT_NE(json.find("\"orderedIdentityHash\": 47"),
                  std::string::npos);
        EXPECT_NE(json.find("\"consumedPayloadHash\": 53"),
                  std::string::npos);
        EXPECT_NE(json.find("\"unorderedIdentityHash\": 59"),
                  std::string::npos);
        EXPECT_NE(json.find("\"unorderedConsumedPayloadHash\": 67"),
                  std::string::npos);
        EXPECT_NE(json.find("\"gpuSceneQualificationTargetFrameSequence\": 67"),
                  std::string::npos);
        EXPECT_NE(json.find("\"gpuSceneQualificationTargetPublishedSequence\": 67"),
                  std::string::npos);
        EXPECT_NE(json.find("\"capturedTier\": \"IndirectGrouped\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"requestedPhysicsBackend\": \"auto\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"actualPhysicsBackend\": \"built-in\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"physicsBackendFallbackActive\": false"),
                  std::string::npos);
        EXPECT_NE(json.find("\"physicsDiagnosticsAvailable\": true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"ecsDiagnostics\": {"), std::string::npos);
        EXPECT_NE(json.find("\"sceneRuntimeId\": 71"), std::string::npos);
        EXPECT_NE(json.find("\"sceneSnapshotRevision\": 19"),
                  std::string::npos);
        EXPECT_NE(json.find("\"entityCount\": 4"), std::string::npos);
        EXPECT_NE(json.find("\"physicsBodySideTableEntryCount\": 2"),
                  std::string::npos);
        EXPECT_NE(json.find("\"legacyFallbackCount\": 0"),
                  std::string::npos);
        EXPECT_NE(json.find("\"enabledFeatures\": [\"RHI\", \"RenderGraph\"]"),
                  std::string::npos);
        EXPECT_NE(json.find("\"unsupportedFeatures\": [\"ScreenshotCapture\"]"),
                  std::string::npos);
        EXPECT_NE(json.find("\"fallbackReasons\": [\"BasicRHI does not capture screenshots yet\"]"),
                  std::string::npos);
        EXPECT_NE(json.find("\"resourceDiagnostics\": [\"fixture loaded\"]"),
                  std::string::npos);
        EXPECT_NE(json.find("\"assetId\": \"fixture\""), std::string::npos);
        EXPECT_NE(json.find("\"assetKind\": \"model\""), std::string::npos);
        EXPECT_NE(json.find("\"catalogPath\": \"catalog.json\""), std::string::npos);
        EXPECT_NE(json.find("\"assetRoot\": \"Assets/Samples\""), std::string::npos);
        EXPECT_NE(json.find("\"assetLicenseSpdx\": \"CC0-1.0\""), std::string::npos);
        EXPECT_NE(json.find("\"assetLicenseFile\": \"LICENSE.txt\""), std::string::npos);
        EXPECT_NE(json.find("\"assetSourceName\": \"Generated fixture\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"assetSourceUri\": \"https://example.invalid/fixture\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"assetAuthor\": \"RenderVerseX contributors\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"assetAttribution\": \"Generated for automated validation.\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"assetRedistributable\": true"), std::string::npos);
        EXPECT_NE(json.find("\"assetLoaded\": true"), std::string::npos);
        EXPECT_NE(json.find("\"assets\": ["), std::string::npos);
        EXPECT_NE(json.find("\"role\": \"environment\", \"id\": \"studio\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"readiness\": {"), std::string::npos);
        EXPECT_NE(json.find("\"waitRequested\": true"), std::string::npos);
        EXPECT_NE(json.find("\"ready\": true"), std::string::npos);
        EXPECT_NE(json.find("\"minimumFrames\": 8"), std::string::npos);
        EXPECT_NE(json.find("\"maximumFrames\": 120"), std::string::npos);
        EXPECT_NE(json.find("\"timeoutMs\": 30000"), std::string::npos);
        EXPECT_NE(json.find("\"scenario\": {"), std::string::npos);
        EXPECT_NE(json.find("\"contractRevision\": 0"), std::string::npos);
        EXPECT_NE(json.find("\"phase\": \"not-applicable\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"renderDiagnostics\": {"), std::string::npos);
        EXPECT_NE(json.find("\"renderGraphTotalPasses\": 3"), std::string::npos);
        EXPECT_NE(json.find("\"postProcessActualExecution\": {"),
                  std::string::npos);
        EXPECT_NE(json.find("\"executionReceiptSchemaVersion\": 12"),
                  std::string::npos);
        EXPECT_NE(json.find("\"presentedFrameAligned\": true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"postProcessFeatureExecuted\": true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"postProcessReceiptStructurallyValid\": true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"inputColorDomain\": \"scene-linear-hdr\""),
                  std::string::npos);
        EXPECT_NE(json.find(
                      "\"plannedOutputColorDomain\": \"display-encoded-ldr\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"storedOutputColorDomain\": \"display-encoded-ldr\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"finalOutputFormat\": \"rgba8-unorm\""),
                  std::string::npos);
        EXPECT_NE(json.find(
                      "\"toneMappingTransfer\": \"scene-linear-to-display-encoded\""),
                  std::string::npos);
        EXPECT_NE(json.find(
                      "\"presentationTransfer\": \"scene-linear-to-display-encoded\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"qualified\": true"), std::string::npos);
        EXPECT_NE(json.find("\"toonLightingActualExecution\": {"),
                  std::string::npos);
        EXPECT_NE(json.find("\"programFamily\": \"toon-lit-v1\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"toonOutlineActualExecution\": {"),
                  std::string::npos);
        EXPECT_NE(json.find("\"outlinedPixelCount\": 128"),
                  std::string::npos);
        EXPECT_NE(json.find("\"transparentAlphaBlendDrawCount\": 1"),
                  std::string::npos);
        EXPECT_NE(json.find("\"transparentPremultipliedDrawCount\": 2"),
                  std::string::npos);
        EXPECT_NE(json.find("\"transparentAdditiveDrawCount\": 3"),
                  std::string::npos);
        EXPECT_NE(json.find("\"transparentThinTransmissionDrawCount\": 4"),
                  std::string::npos);
        EXPECT_NE(json.find("\"transparentOpaqueSceneSnapshotCopied\": true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"transparentReactiveMaskWriteDrawCount\": 4"),
                  std::string::npos);
        EXPECT_EQ(json.find("\"renderWidth\""), std::string::npos);
        EXPECT_NE(json.find("\"temporalAAQualified\": false"),
                  std::string::npos);
        EXPECT_NE(json.find("\"nativeValidationAvailable\": true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"nativeValidationEnabled\": true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"nativeValidationReadComplete\": true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"nativeValidationWarningCount\": 2"),
                  std::string::npos);
        EXPECT_NE(json.find("\"nativeValidationErrorCount\": 3"),
                  std::string::npos);
        EXPECT_NE(json.find("\"nativeValidationCorruptionCount\": 4"),
                  std::string::npos);
        EXPECT_NE(json.find("\"directionalShadowSamplingEnabled\": true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"gpuDrivenRequestedMode\": \"ForceEnabled\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"gpuDrivenGraphPassRecorded\": true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"gpuDrivenOpaqueIndirectSubmitted\": true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"renderPolicyExecutedTier\": \"IndirectGrouped\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"renderPolicyRequestAvailable\": true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"renderPolicyRequestFrameSequence\": 1"),
                  std::string::npos);
        EXPECT_NE(json.find("\"renderPolicyExecutionStatus\": \"Completed\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"opaqueExecutionCompleted\": true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"opaqueExecutedDrawCount\": 12"),
                  std::string::npos);
        EXPECT_NE(json.find("\"opaqueMaterialBindingsAvailable\": true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"opaqueMaterialBindingCount\": 12"),
                  std::string::npos);
        EXPECT_NE(json.find("\"opaqueMaterialFallbackBindingCount\": 0"),
                  std::string::npos);
        EXPECT_NE(json.find("\"opaqueMaterialTextureFlags\": 4"),
                  std::string::npos);
        EXPECT_NE(json.find("\"opaqueMaterialFallbackTextureFlags\": 0"),
                  std::string::npos);
        EXPECT_NE(json.find("\"pass\": true"), std::string::npos);
    }

    TEST(SampleCLIValidation,
         AssetResidencyEvidenceRemainsIndependentFromScenarioReadiness)
    {
        RVX::SampleReport report;
        report.assetLoaded = true;
        report.readiness.waitRequested = true;
        report.readiness.ready = false;
        report.readiness.reason = "Waiting for final Render retirement";
        RVX::SampleReportAsset model;
        model.role = "model";
        model.id = "verified-model";
        model.loaded = true;
        model.verified = true;
        report.assets.push_back(model);

        std::ostringstream stream;
        RVX::WriteSampleReportJson(stream, report);
        const std::string json = stream.str();

        EXPECT_NE(json.find("\"assetLoaded\": true"), std::string::npos);
        EXPECT_NE(json.find("\"ready\": false"), std::string::npos);
        EXPECT_NE(json.find("\"loaded\": true"), std::string::npos);
        EXPECT_NE(json.find("Waiting for final Render retirement"),
                  std::string::npos);
    }

    TEST(SampleCLIValidation, BuildReportOnlySampleAddsCategoryAndFallbacks)
    {
        RVX::SampleAppDesc desc;
        desc.sampleName = "BackendInfoSample";
        desc.category = "basic";
        desc.enabledFeatures = {"BackendSelection"};

        RVX::SampleRunContext context;
        context.resolvedBackend = RVX::RHIBackendType::DX11;
        context.frameCount = 2;
        context.options.width = 320;
        context.options.height = 180;
        context.options.quality = "low";
        context.options.screenshotPath = "actual.ppm";

        const RVX::SampleReport report = RVX::BuildSampleReport(desc, context);
        EXPECT_EQ(report.sampleName, "BackendInfoSample");
        EXPECT_EQ(report.category, "basic");
        EXPECT_EQ(report.backend, RVX::RHIBackendType::DX11);
        EXPECT_EQ(report.frameCount, 2u);
        EXPECT_EQ(report.width, 320u);
        EXPECT_EQ(report.height, 180u);
        EXPECT_EQ(report.schemaVersion, RVX::RVX_SAMPLE_REPORT_SCHEMA_VERSION);
        EXPECT_EQ(report.renderPath, "auto");
        EXPECT_EQ(report.requestedRenderPath, "auto");
        EXPECT_EQ(report.actualRenderPath, "unavailable");
        EXPECT_EQ(report.requestedPhysicsBackend, "unavailable");
        EXPECT_EQ(report.actualPhysicsBackend, "unavailable");
        EXPECT_FALSE(report.physicsDiagnosticsAvailable);
        EXPECT_FALSE(report.physicsBackendFallbackActive);
        EXPECT_EQ(report.scenario.contractRevision, 0u);
        EXPECT_EQ(report.scenario.phase, "not-applicable");
        EXPECT_TRUE(report.pass);
        ASSERT_EQ(report.enabledFeatures.size(), 1u);
        EXPECT_EQ(report.enabledFeatures[0], "BackendSelection");
        EXPECT_NE(std::find(report.unsupportedFeatures.begin(),
                            report.unsupportedFeatures.end(),
                            "ScreenshotCapture"),
                  report.unsupportedFeatures.end());
        EXPECT_NE(std::find(report.unsupportedFeatures.begin(),
                            report.unsupportedFeatures.end(),
                            "QualityProfile"),
                  report.unsupportedFeatures.end());
        ASSERT_GE(report.fallbackReasons.size(), 2u);
    }

    TEST(SampleCLIValidation,
         ScenarioReportReceiptsAreSortedAndDuplicateNamesFailClosed)
    {
        RVX::SampleReport report;
        report.pass = true;
        RVX::SampleFeatureReporter reporter(report);
        reporter.SetScenarioContractRevision(7);
        reporter.SetScenarioPhase("complete");
        ASSERT_TRUE(reporter.AppendScenarioAction(
            {"zebra", 9, 12, 9, true}));
        ASSERT_TRUE(reporter.AppendScenarioAction(
            {"alpha", 4, 8, 4, true}));
        ASSERT_TRUE(reporter.AppendScenarioInvariant(
            {"zebra-invariant", true, "zebra evidence"}));
        ASSERT_TRUE(reporter.AppendScenarioInvariant(
            {"alpha-invariant", true, "alpha evidence"}));
        ASSERT_TRUE(reporter.AppendScenarioMetric({"zebra-metric", 2}));
        ASSERT_TRUE(reporter.AppendScenarioMetric({"alpha-metric", 1}));

        std::ostringstream stream;
        RVX::WriteSampleReportJson(stream, report);
        const std::string json = stream.str();

        EXPECT_NE(json.find("\"schemaVersion\": 33"), std::string::npos);
        EXPECT_NE(json.find("\"contractRevision\": 7"), std::string::npos);
        EXPECT_NE(json.find("\"phase\": \"complete\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"completedPresentationSequence\": 8"),
                  std::string::npos);
        EXPECT_NE(json.find("\"evidence\": \"alpha evidence\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"value\": 1"), std::string::npos);
        EXPECT_LT(json.find("\"name\": \"alpha\""),
                  json.find("\"name\": \"zebra\""));
        EXPECT_LT(json.find("\"name\": \"alpha-invariant\""),
                  json.find("\"name\": \"zebra-invariant\""));
        EXPECT_LT(json.find("\"name\": \"alpha-metric\""),
                  json.find("\"name\": \"zebra-metric\""));

        EXPECT_FALSE(reporter.AppendScenarioAction(
            {"alpha", 10, 13, 10, true}));
        EXPECT_FALSE(report.pass);
        EXPECT_EQ(report.scenario.actions.size(), 2u);
    }

    TEST(SampleCLIValidation,
         VersionFourReportDistinguishesObservedZeroFromUnavailableSequences)
    {
        RVX::SampleReport report;
        report.schemaVersion = 4;
        report.renderDiagnostics.lastSubmittedFrameSequence =
            RVX::DiagnosticValue<RVX::uint64>::Available(0U);
        report.renderDiagnostics.lastPresentedFrameSequence =
            RVX::DiagnosticValue<RVX::uint64>::Unavailable(
                "Render runtime has not been configured.");
        report.renderDiagnostics.engineUpdateTickCount =
            RVX::DiagnosticValue<RVX::uint64>::Available(2U);

        std::ostringstream stream;
        RVX::WriteSampleReportJson(stream, report);
        const std::string json = stream.str();

        EXPECT_NE(json.find(
                      "\"lastSubmittedFrameSequence\": {\"available\":true,\"reason\":\"\",\"value\":0}"),
                  std::string::npos);
        EXPECT_NE(json.find(
                      "\"lastPresentedFrameSequence\": {\"available\":false,\"reason\":\"Render runtime has not been configured.\"}"),
                  std::string::npos);
        EXPECT_NE(json.find(
                      "\"engineUpdateTickCount\": {\"available\":true,\"reason\":\"\",\"value\":2}"),
                  std::string::npos);
    }

    TEST(SampleCLIValidation,
         VersionFiveReportPreservesInteriorRenderEvidence)
    {
        RVX::SampleReport report;
        report.schemaVersion = 5;
        auto& diagnostics = report.renderDiagnostics;
        diagnostics.directionalShadowAvailable = true;
        diagnostics.directionalShadowRequested = true;
        diagnostics.directionalShadowSupported = true;
        diagnostics.directionalShadowOutputReady = true;
        diagnostics.directionalShadowRequestedCascadeCount = 3;
        diagnostics.directionalShadowProducedCascadeCount = 3;
        diagnostics.directionalShadowResolvedCascadeCount = 3;
        diagnostics.directionalShadowMapSize = 2048;
        diagnostics.directionalShadowCasterCount = 51;
        diagnostics.directionalShadowDrawCount = 51;
        diagnostics.localLightingAvailable = true;
        diagnostics.pointLightRequestedCount = 8;
        diagnostics.pointLightAdmittedCount = 8;
        diagnostics.pointLightCapacity = 256;
        diagnostics.spotLightRequestedCount = 4;
        diagnostics.spotLightAdmittedCount = 4;
        diagnostics.spotLightCapacity = 128;
        diagnostics.pointShadowRequestedCount = 8;
        diagnostics.pointShadowReason =
            "Point-light shadow atlas is not implemented.";
        diagnostics.spotShadowRequestedCount = 4;
        diagnostics.spotShadowReason =
            "Spot-light shadow atlas is not implemented.";
        diagnostics.hzbRequested = true;
        diagnostics.hzbReason =
            "HZB production and validation are not implemented.";
        diagnostics.transparentAvailable = true;
        diagnostics.transparentOrderValid = true;
        diagnostics.transparentOrderHash = 12345;
        diagnostics.transparentCandidateDrawItemCount = 2;
        diagnostics.transparentPreparedDrawItemCount = 2;
        diagnostics.transparentExecutedPacketCount = 2;
        diagnostics.transparentExecutedDrawCount = 2;
        diagnostics.transparentMaterialBindingCount = 2;

        std::ostringstream stream;
        RVX::WriteSampleReportJson(stream, report);
        const std::string json = stream.str();

        EXPECT_NE(json.find("\"schemaVersion\": 5"), std::string::npos);
        EXPECT_NE(json.find("\"directionalShadowOutputReady\": true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"directionalShadowResolvedCascadeCount\": 3"),
                  std::string::npos);
        EXPECT_NE(json.find("\"pointLightAdmittedCount\": 8"),
                  std::string::npos);
        EXPECT_NE(json.find("\"pointLightOverflowCount\": 0"),
                  std::string::npos);
        EXPECT_NE(json.find("\"pointShadowSupported\": false"),
                  std::string::npos);
        EXPECT_NE(json.find("\"hzbRequested\": true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"transparentOrderHash\": 12345"),
                  std::string::npos);
        EXPECT_NE(json.find("\"transparentExecutedDrawCount\": 2"),
                  std::string::npos);
        EXPECT_NE(json.find("\"transparentExecutionFailed\": false"),
                  std::string::npos);
    }

    TEST(SampleCLIValidation,
         VersionSixReportPreservesIncrementalSceneAndGpuWorkEvidence)
    {
        RVX::SampleReport report;
        report.schemaVersion = 6;
        auto& diagnostics = report.renderDiagnostics;
        diagnostics.gpuDrivenInstanceUploadBytes = 448000;
        diagnostics.gpuDrivenCandidateUploadBytes = 48000;
        diagnostics.directRasterInstanceUploadBytes = 224000;
        diagnostics.directRasterInstanceIndexUploadBytes = 4000;
        diagnostics.extractionAvailable = true;
        diagnostics.extractionActorRebuildCount = 10;
        diagnostics.extractionProxyVisitCount = 10;
        diagnostics.sceneWorkAvailable = true;
        diagnostics.sceneIncrementalUpdateCount = 4;
        diagnostics.sceneStaticReuseCount = 7;
        diagnostics.sceneLastRebuiltObjectCount = 10;
        diagnostics.drawPacketBuildCount = 10;
        diagnostics.drawPacketInvalidationCount = 10;
        diagnostics.gpuSceneAvailable = true;
        diagnostics.gpuScenePublicationAttempted = true;
        diagnostics.gpuScenePublicationPublished = true;
        diagnostics.gpuSceneUpdateCount = 10;
        diagnostics.gpuSceneNoOpCount = 990;
        diagnostics.gpuSceneFrameUploadBytes = 2240;
        diagnostics.gpuSceneFrameUploadRangeCount = 10;

        std::ostringstream stream;
        RVX::WriteSampleReportJson(stream, report);
        const std::string json = stream.str();

        EXPECT_NE(json.find("\"schemaVersion\": 6"), std::string::npos);
        EXPECT_NE(json.find("\"gpuDrivenInstanceUploadBytes\": 448000"),
                  std::string::npos);
        EXPECT_NE(json.find("\"directRasterInstanceUploadBytes\": 224000"),
                  std::string::npos);
        EXPECT_NE(json.find("\"extractionAvailable\": true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"extractionActorRebuildCount\": 10"),
                  std::string::npos);
        EXPECT_NE(json.find("\"sceneWorkAvailable\": true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"sceneLastRebuiltObjectCount\": 10"),
                  std::string::npos);
        EXPECT_NE(json.find("\"drawPacketInvalidationCount\": 10"),
                  std::string::npos);
        EXPECT_NE(json.find("\"gpuScenePublicationPublished\": true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"gpuSceneNoOpCount\": 990"),
                  std::string::npos);
        EXPECT_NE(json.find("\"gpuSceneFrameUploadBytes\": 2240"),
                  std::string::npos);
        EXPECT_EQ(json.find("\"gpuDrivenActiveRowUploadBytes\""),
                  std::string::npos);
    }

    TEST(SampleCLIValidation,
         VersionSevenReportPreservesStableGpuCullingRowEvidence)
    {
        RVX::SampleReport report;
        report.schemaVersion = 7;
        auto& diagnostics = report.renderDiagnostics;
        diagnostics.gpuDrivenActiveRowUploadBytes = 640;
        diagnostics.gpuDrivenActiveRowCount = 20;
        diagnostics.gpuDrivenActiveRowHighWatermark = 24;
        diagnostics.gpuDrivenInstancePatchedRowCount = 4;
        diagnostics.gpuDrivenCandidatePatchedRowCount = 4;
        diagnostics.gpuDrivenActiveRowPatchedRowCount = 2;
        diagnostics.gpuDrivenInstanceFullMaterializationCount = 1;
        diagnostics.gpuDrivenCandidateFullMaterializationCount = 2;
        diagnostics.gpuDrivenActiveRowFullMaterializationCount = 3;
        diagnostics.gpuDrivenContinuityFullMaterializationCount = 4;
        diagnostics.gpuDrivenCapacityFullMaterializationCount = 5;

        std::ostringstream stream;
        RVX::WriteSampleReportJson(stream, report);
        const std::string json = stream.str();

        EXPECT_NE(json.find("\"schemaVersion\": 7"), std::string::npos);
        EXPECT_NE(json.find("\"gpuDrivenActiveRowUploadBytes\": 640"),
                  std::string::npos);
        EXPECT_NE(json.find("\"gpuDrivenActiveRowCount\": 20"),
                  std::string::npos);
        EXPECT_NE(json.find("\"gpuDrivenActiveRowHighWatermark\": 24"),
                  std::string::npos);
        EXPECT_NE(json.find("\"gpuDrivenInstancePatchedRowCount\": 4"),
                  std::string::npos);
        EXPECT_NE(
            json.find("\"gpuDrivenCandidateFullMaterializationCount\": 2"),
            std::string::npos);
        EXPECT_NE(
            json.find("\"gpuDrivenContinuityFullMaterializationCount\": 4"),
            std::string::npos);
        EXPECT_EQ(json.find("\"directRasterInstancePatchedRowCount\""),
                  std::string::npos);
    }

    TEST(SampleCLIValidation,
         VersionEightReportPreservesDirectPersistentRowEvidence)
    {
        RVX::SampleReport report;
        report.schemaVersion = 8;
        auto& diagnostics = report.renderDiagnostics;
        diagnostics.directRasterInstancePatchedRowCount = 10;
        diagnostics.directRasterIndexPatchedRowCount = 4;
        diagnostics.directRasterActiveInstanceCount = 1000;
        diagnostics.directRasterActiveInstanceCapacity = 1024;
        diagnostics.directRasterInstanceFullMaterializationCount = 2;
        diagnostics.directRasterIndexFullMaterializationCount = 1;

        std::ostringstream stream;
        RVX::WriteSampleReportJson(stream, report);
        const std::string json = stream.str();

        EXPECT_NE(json.find("\"schemaVersion\": 8"), std::string::npos);
        EXPECT_NE(json.find("\"directRasterInstancePatchedRowCount\": 10"),
                  std::string::npos);
        EXPECT_NE(json.find("\"directRasterIndexPatchedRowCount\": 4"),
                  std::string::npos);
        EXPECT_NE(json.find("\"directRasterActiveInstanceCount\": 1000"),
                  std::string::npos);
        EXPECT_NE(json.find("\"directRasterActiveInstanceCapacity\": 1024"),
                  std::string::npos);
        EXPECT_NE(
            json.find("\"directRasterInstanceFullMaterializationCount\": 2"),
            std::string::npos);
        EXPECT_NE(
            json.find("\"directRasterIndexFullMaterializationCount\": 1"),
                  std::string::npos);
    }

    TEST(SampleCLIValidation,
         VersionNineReportPreservesRangeReceiptEvidenceAndUnavailableState)
    {
        RVX::SampleReport report;
        report.schemaVersion = 9;
        auto& canonical =
            report.renderDiagnostics.gpuDrivenCanonicalInstanceUploadWork;
        canonical.cpuCopiedPayloadBytes = 96;
        canonical.committedPayloadBytes = 96;
        canonical.mappedRangeCount = 2;
        canonical.committedRangeCount = 2;
        canonical.hostVisibilitySynchronizedBytes =
            RVX::DiagnosticValue<RVX::uint64>::Available(128);
        canonical.hostVisibilitySynchronizationScopeRangeCounts[1] = 2;
        canonical.hostVisibilitySynchronizationScopeBytes[1] = 96;
        report.renderDiagnostics.gpuSceneUploadWork.cpuCopiedPayloadBytes = 64;
        report.renderDiagnostics.gpuSceneUploadWork.committedPayloadBytes = 64;
        report.renderDiagnostics.gpuSceneUploadWork.gpuCopyBytes = 64;
        report.renderDiagnostics.gpuSceneUploadWork.gpuCopyRangeCount = 1;
        report.renderDiagnostics.gpuSceneTableUploadWork[
            static_cast<RVX::uint32>(RVX::GPUSceneDiagnosticsTable::Primitives)]
            .committedPayloadBytes = 64;

        std::ostringstream stream;
        RVX::WriteSampleReportJson(stream, report);
        const std::string json = stream.str();

        EXPECT_NE(json.find("\"schemaVersion\": 9"), std::string::npos);
        EXPECT_NE(json.find("\"gpuDrivenCanonicalInstanceUploadWork\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"cpuCopiedPayloadBytes\": 96"),
                  std::string::npos);
        EXPECT_NE(json.find("\"hostVisibilitySynchronizedBytes\": {\"available\":true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"gpuSceneUploadWork\""), std::string::npos);
        EXPECT_NE(json.find("\"gpuCopyBytes\": 64"), std::string::npos);
        EXPECT_NE(json.find("\"gpuScenePrimitivesUploadWork\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"available\":false"), std::string::npos);
    }

    TEST(SampleCLIValidation,
         VersionTenReportPreservesAcceptedExtractionProvenanceAndTotals)
    {
        RVX::SampleReport report;
        report.schemaVersion = 10;
        auto& diagnostics = report.renderDiagnostics;
        diagnostics.acceptedExtractionDiagnosticsAvailable = true;
        diagnostics.acceptedExtractionPublicationCount = 17;
        diagnostics.acceptedExtractionLastSourceFrameSequence = 42;
        diagnostics.acceptedExtractionLastSceneRevision = 91;
        diagnostics.acceptedExtractionCumulativeFullScanCount = 1;
        diagnostics.acceptedExtractionCumulativeChangeFeedChangeCount = 128;
        diagnostics.acceptedExtractionCumulativeActorRebuildCount = 64;
        diagnostics.acceptedExtractionCumulativeProxyVisitCount = 64;
        diagnostics.acceptedExtractionCumulativeComponentVisitCount = 0;
        diagnostics.acceptedExtractionCumulativeFeatureProviderVisitCount = 0;
        diagnostics.acceptedExtractionContinuityLossCount = 0;

        std::ostringstream stream;
        RVX::WriteSampleReportJson(stream, report);
        const std::string json = stream.str();

        EXPECT_NE(json.find("\"schemaVersion\": 10"), std::string::npos);
        EXPECT_NE(json.find("\"acceptedExtractionDiagnosticsAvailable\": true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"acceptedExtractionPublicationCount\": 17"),
                  std::string::npos);
        EXPECT_NE(json.find("\"acceptedExtractionLastSourceFrameSequence\": 42"),
                  std::string::npos);
        EXPECT_NE(json.find("\"acceptedExtractionLastSceneRevision\": 91"),
                  std::string::npos);
        EXPECT_NE(json.find("\"acceptedExtractionCumulativeActorRebuildCount\": 64"),
                  std::string::npos);
        EXPECT_NE(json.find("\"acceptedExtractionCumulativeProxyVisitCount\": 64"),
                  std::string::npos);
    }

    TEST(SampleCLIValidation,
         VersionElevenReportSeparatesExpectedObservedAndVerificationStatus)
    {
        const auto makeIdentity = [](const std::string& digest)
        {
            RVX::Resource::ResourceContentIdentity identity;
            identity.schemaVersion =
                RVX::Resource::RVX_RESOURCE_CONTENT_IDENTITY_SCHEMA_VERSION;
            identity.domain =
                RVX::Resource::ResourceContentIdentityDomain::Source;
            identity.scope =
                RVX::Resource::ResourceContentIdentityScope::SelfContainedArtifact;
            identity.algorithm =
                RVX::Resource::ResourceContentHashAlgorithm::SHA256;
            identity.digest = digest;
            identity.byteCount = 4;
            identity.fileCount = 1;
            return identity;
        };

        const RVX::Resource::ResourceContentIdentity expected = makeIdentity(
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        const RVX::Resource::ResourceContentIdentity observed = makeIdentity(
            "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
        RVX::SampleReport report;
        report.schemaVersion = 11;

        RVX::SampleReportAsset notRequested;
        notRequested.role = "catalog-only";
        notRequested.expectedContentIdentity = expected;
        report.assets.push_back(notRequested);

        RVX::SampleReportAsset observedOnly;
        observedOnly.role = "observed";
        observedOnly.observedContentIdentity = observed;
        observedOnly.verificationStatus =
            RVX::Resource::ResourceContentVerificationStatus::Observed;
        report.assets.push_back(observedOnly);

        RVX::SampleReportAsset verified;
        verified.role = "verified";
        verified.expectedContentIdentity = expected;
        verified.observedContentIdentity = expected;
        verified.verificationStatus =
            RVX::Resource::ResourceContentVerificationStatus::Verified;
        verified.verified = true;
        verified.packageContentId.schemaVersion = 1;
        verified.packageContentId.algorithm = "sha256";
        verified.packageContentId.digest =
            "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
        verified.cookedAdmissionRequired = true;
        verified.cookedAdmissionAccepted = true;
        report.assets.push_back(verified);

        RVX::SampleReportAsset unavailable;
        unavailable.role = "unavailable";
        report.assets.push_back(unavailable);

        std::ostringstream stream;
        RVX::WriteSampleReportJson(stream, report);
        const std::string json = stream.str();

        EXPECT_NE(json.find("\"schemaVersion\": 11"), std::string::npos);
        EXPECT_NE(json.find("\"expectedContentIdentity\": {\"schemaVersion\":1"),
                  std::string::npos);
        EXPECT_NE(json.find("\"observedContentIdentity\": {\"schemaVersion\":1"),
                  std::string::npos);
        EXPECT_NE(json.find("\"verificationStatus\": \"not-requested\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"verificationStatus\": \"observed\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"verificationStatus\": \"verified\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"verified\": true"), std::string::npos);
        EXPECT_NE(json.find("\"verified\": false"), std::string::npos);
        EXPECT_EQ(json.find("\"packageContentId\""), std::string::npos);
        EXPECT_EQ(json.find("\"cook\": {"), std::string::npos);
    }

    TEST(SampleCLIValidation, VersionTwelveReportPreservesCatalogPackageContentId)
    {
        RVX::SampleReport report;
        report.schemaVersion = RVX::RVX_SAMPLE_REPORT_SCHEMA_VERSION;
        RVX::SampleReportAsset asset;
        asset.id = "water-bottle";
        asset.sourceContentId.schemaVersion = 1;
        asset.sourceContentId.algorithm = "sha256";
        asset.sourceContentId.digest =
            "4df540b802067f192a7aa38aed524dda141dc23d9cb7a7e6272fc021694198ed";
        asset.sourceContentId.byteCount = 8966700;
        asset.sourceContentId.fileCount = 1;
        report.assets.push_back(asset);

        std::ostringstream stream;
        RVX::WriteSampleReportJson(stream, report);
        const std::string json = stream.str();
        EXPECT_NE(json.find("\"sourceContentId\""), std::string::npos);
        EXPECT_NE(json.find(asset.sourceContentId.digest), std::string::npos);
        EXPECT_NE(json.find("\"byteCount\": 8966700"), std::string::npos);
        EXPECT_NE(json.find("\"fileCount\": 1"), std::string::npos);
    }

    TEST(SampleCLIValidation,
         VersionTwelveReportSerializesCookAdmissionEvidence)
    {
        const auto makeIdentity = [](RVX::Resource::ResourceContentIdentityDomain domain,
                                     const std::string& digest)
        {
            RVX::Resource::ResourceContentIdentity identity;
            identity.schemaVersion =
                RVX::Resource::RVX_RESOURCE_CONTENT_IDENTITY_SCHEMA_VERSION;
            identity.domain = domain;
            identity.scope =
                RVX::Resource::ResourceContentIdentityScope::SelfContainedArtifact;
            identity.algorithm =
                RVX::Resource::ResourceContentHashAlgorithm::SHA256;
            identity.digest = digest;
            identity.byteCount = 17;
            identity.fileCount = 1;
            return identity;
        };

        RVX::SampleReport report;
        report.schemaVersion = RVX::RVX_SAMPLE_REPORT_SCHEMA_VERSION;
        RVX::SampleReportAsset asset;
        asset.id = "casual-female";
        asset.kind = "model";
        asset.packageContentId.schemaVersion = 1;
        asset.packageContentId.algorithm = "sha256";
        asset.packageContentId.digest =
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
        asset.packageContentId.byteCount = 1234;
        asset.packageContentId.fileCount = 5;
        asset.cookedAdmissionRequired = true;
        asset.cookedAdmissionAccepted = true;
        asset.cookPackageOnly = true;
        asset.actualLoadDomain = "CookedArtifact";
        asset.cookedAdmissionCode = "accepted";
        asset.cookedAdmissionDetail = "manifest and mounted closure verified";
        asset.cookManifestPath = "cooked/CookManifest.rvxmanifest";
        asset.cookedRoot = "cooked";
        asset.declaredCookSourceContentIdentity = makeIdentity(
            RVX::Resource::ResourceContentIdentityDomain::Source,
            "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
        asset.declaredCookedContentIdentity = makeIdentity(
            RVX::Resource::ResourceContentIdentityDomain::CookedArtifact,
            "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");
        asset.declaredCookManifestContentIdentity = makeIdentity(
            RVX::Resource::ResourceContentIdentityDomain::CookManifest,
            "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd");
        asset.observedCookSourceContentIdentity =
            asset.declaredCookSourceContentIdentity;
        asset.observedCookedContentIdentity =
            asset.declaredCookedContentIdentity;
        asset.observedCookManifestContentIdentity =
            asset.declaredCookManifestContentIdentity;
        asset.cookSettingsHash = "settings-hash";
        asset.cookRecipeHash = "recipe-hash";
        asset.cookToolName = "RVXCook";
        asset.cookToolVersion = "2.0.0";
        report.assets.push_back(asset);

        std::ostringstream stream;
        RVX::WriteSampleReportJson(stream, report);
        const std::string json = stream.str();

        EXPECT_NE(json.find("\"schemaVersion\": 33"), std::string::npos);
        EXPECT_NE(json.find("\"packageContentId\""), std::string::npos);
        EXPECT_NE(json.find(asset.packageContentId.digest), std::string::npos);
        EXPECT_NE(json.find("\"cook\": {\"required\": true, \"accepted\": true, \"packageOnly\": true, \"actualLoadDomain\": \"CookedArtifact\", \"code\": \"accepted\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"declaredSourceContentIdentity\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"declaredCookedContentIdentity\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"declaredManifestContentIdentity\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"observedSourceContentIdentity\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"observedCookedContentIdentity\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"observedManifestContentIdentity\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"cookSettingsHash\": \"settings-hash\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"recipeHash\": \"recipe-hash\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"toolName\": \"RVXCook\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"toolVersion\": \"2.0.0\""),
                  std::string::npos);
    }

    TEST(SampleCLIValidation,
         VersionTwelveReportProjectsPresentedSkinningPaletteReceipts)
    {
        RVX::SampleReport report;
        report.schemaVersion = RVX::RVX_SAMPLE_REPORT_SCHEMA_VERSION;
        auto& diagnostics = report.renderDiagnostics;
        diagnostics.presentedSkinningPalettesAvailable = true;
        diagnostics.presentedSkinningPalettes.push_back(
            {0x0000000500000001ULL,
             812,
             9,
             0x9ABCDEF012345678ULL,
             2,
             RVX::RenderSkinningPaletteExecutionLane::Direct,
             40,
             40});

        std::ostringstream stream;
        RVX::WriteSampleReportJson(stream, report);
        const std::string json = stream.str();
        EXPECT_NE(json.find("\"presentedSkinningPalettesAvailable\": true"),
                  std::string::npos);
        EXPECT_NE(json.find("\"providerComponentId\":21474836481"),
                  std::string::npos);
        EXPECT_NE(json.find("\"sourceModelResourceId\":812"),
                  std::string::npos);
        EXPECT_NE(json.find("\"lane\":\"Direct\""),
                  std::string::npos);
    }

    TEST(SampleCLIValidation,
         CompletionQualifiedMutationEvidenceRemainsAnInProcessContract)
    {
        RVX::SampleReport report;
        report.schemaVersion = 11;
        auto& evidence = report.renderDiagnostics.mutationEvidence;
        evidence.available = true;
        evidence.evidenceEpoch = 3u;
        evidence.completedPresentationCount = 9u;
        evidence.completedFrameSequence = 17u;
        evidence.appliedSceneRevision = 29u;
        evidence.sceneRemovedObjectCount = 256u;
        evidence.gpuCullingInstanceUploadedRowCount = 512u;
        evidence.gpuSceneSubmittedUploadedRowCount[
            static_cast<RVX::uint32>(RVX::GPUSceneDiagnosticsTable::Draws)] =
            256u;

        std::ostringstream stream;
        RVX::WriteSampleReportJson(stream, report);
        const std::string json = stream.str();

        EXPECT_TRUE(evidence.available);
        EXPECT_EQ(evidence.completedPresentationCount, 9u);
        EXPECT_EQ(evidence.sceneRemovedObjectCount, 256u);
        EXPECT_EQ(evidence.gpuCullingInstanceUploadedRowCount, 512u);
        EXPECT_EQ(evidence.gpuSceneSubmittedUploadedRowCount[
                      static_cast<RVX::uint32>(
                          RVX::GPUSceneDiagnosticsTable::Draws)],
                  256u);
        EXPECT_EQ(json.find("\"mutationEvidence\""), std::string::npos);
    }

    TEST(SampleCLIValidation,
         RuntimeFrameWaitUsesBarrierAwareEngineActions)
    {
        RVX::RuntimeFrameWaitRequest request;
        request.minimumPublishedSequence = 12;
        request.minimumSubmittedSequence = 12;
        request.minimumPresentedSequence = 12;

        RVX::RenderDiagnosticsSnapshot diagnostics;
        diagnostics.lastPublishedFrameSequence = 11;
        diagnostics.lastSubmittedFrameSequence = 11;
        diagnostics.lastPresentedFrameSequence = 11;
        EXPECT_TRUE(RVX::RuntimeFrameDriverTestAccess::UsesFullTick(
            request, diagnostics));
        EXPECT_FALSE(RVX::RuntimeFrameDriverTestAccess::HasReached(
            request, diagnostics));

        diagnostics.lastPublishedFrameSequence = 12;
        RVX::EngineRenderRuntimeDiagnostics engineDiagnostics;
        engineDiagnostics.available = true;
        engineDiagnostics.requiredSceneFrameSequence = 12;
        EXPECT_TRUE(RVX::RuntimeFrameDriverTestAccess::UsesProgressPoll(
            request, diagnostics, engineDiagnostics));
        EXPECT_FALSE(RVX::RuntimeFrameDriverTestAccess::HasReached(
            request, diagnostics));

        diagnostics.lastSubmittedFrameSequence = 12;
        diagnostics.lastPresentedFrameSequence = 12;
        EXPECT_TRUE(RVX::RuntimeFrameDriverTestAccess::ObservesOnly(
            request, diagnostics, engineDiagnostics));
        EXPECT_TRUE(RVX::RuntimeFrameDriverTestAccess::HasReached(
            request, diagnostics));

        request.advanceEngine = false;
        diagnostics.lastSubmittedFrameSequence = 11;
        EXPECT_TRUE(RVX::RuntimeFrameDriverTestAccess::ObservesOnly(
            request, diagnostics, engineDiagnostics));

        request.pumpRenderProgressOnly = true;
        EXPECT_TRUE(RVX::RuntimeFrameDriverTestAccess::UsesProgressPoll(
            request, diagnostics, engineDiagnostics));
    }

    TEST(SampleCLIValidation,
         QualificationTargetWaitPublishesAfterPendingOuterTickThenUsesRenderOnlyProgress)
    {
        constexpr RVX::uint64 TargetSequence = 73u;
        RVX::RuntimeFrameWaitRequest request;
        request.minimumPublishedSequence = TargetSequence;
        request.minimumSubmittedSequence = TargetSequence;
        request.minimumPresentedSequence = TargetSequence;
        // The carrying outer Tick was pending and did not publish. The wait
        // must permit precisely the next Engine-only publication attempt.
        request.advanceEngine = true;

        RVX::RenderDiagnosticsSnapshot diagnostics;
        diagnostics.lastPublishedFrameSequence = TargetSequence - 1u;
        diagnostics.lastSubmittedFrameSequence = TargetSequence - 1u;
        diagnostics.lastPresentedFrameSequence = TargetSequence - 1u;
        RVX::EngineRenderRuntimeDiagnostics engineDiagnostics;
        engineDiagnostics.available = true;
        engineDiagnostics.requiredSceneFrameSequence = TargetSequence - 1u;

        EXPECT_TRUE(RVX::RuntimeFrameDriverTestAccess::UsesFullTick(
            request, diagnostics, engineDiagnostics));
        // Model the one extra Engine tick that finally publishes the armed
        // target. Submission/presentation are still pending.
        diagnostics.lastPublishedFrameSequence = TargetSequence;
        engineDiagnostics.requiredSceneFrameSequence = TargetSequence;
        EXPECT_TRUE(RVX::RuntimeFrameDriverTestAccess::UsesProgressPoll(
            request, diagnostics, engineDiagnostics));
        EXPECT_FALSE(RVX::RuntimeFrameDriverTestAccess::UsesFullTick(
            request, diagnostics, engineDiagnostics));

        diagnostics.lastSubmittedFrameSequence = TargetSequence;
        diagnostics.lastPresentedFrameSequence = TargetSequence;
        EXPECT_TRUE(RVX::RuntimeFrameDriverTestAccess::ObservesOnly(
            request, diagnostics, engineDiagnostics));
        EXPECT_TRUE(RVX::RuntimeFrameDriverTestAccess::HasReached(
            request, diagnostics));
    }

    TEST(SampleCLIValidation,
         FinalDrainPresentationWaitPublishesPendingTargetThenUsesRenderOnlyProgress)
    {
        constexpr RVX::uint64 TargetSequence = 74u;
        RVX::RuntimeFrameWaitRequest presentationRequest;
        presentationRequest.minimumPublishedSequence = TargetSequence;
        presentationRequest.minimumSubmittedSequence = TargetSequence;
        presentationRequest.minimumPresentedSequence = TargetSequence;
        presentationRequest.advanceEngine = true;

        RVX::RenderDiagnosticsSnapshot diagnostics;
        diagnostics.lastPublishedFrameSequence = TargetSequence - 1u;
        diagnostics.lastSubmittedFrameSequence = TargetSequence - 1u;
        diagnostics.lastPresentedFrameSequence = TargetSequence - 1u;
        RVX::EngineRenderRuntimeDiagnostics engineDiagnostics;
        engineDiagnostics.available = true;
        engineDiagnostics.requiredSceneFrameSequence = TargetSequence - 1u;

        EXPECT_TRUE(RVX::RuntimeFrameDriverTestAccess::UsesFullTick(
            presentationRequest, diagnostics, engineDiagnostics));
        // The final-drain outer Tick did not publish. Its sole necessary
        // successor publishes the exact target; no later engine tick is used
        // while submission and presentation finish.
        diagnostics.lastPublishedFrameSequence = TargetSequence;
        engineDiagnostics.requiredSceneFrameSequence = TargetSequence;
        EXPECT_TRUE(RVX::RuntimeFrameDriverTestAccess::UsesProgressPoll(
            presentationRequest, diagnostics, engineDiagnostics));
        EXPECT_FALSE(RVX::RuntimeFrameDriverTestAccess::UsesFullTick(
            presentationRequest, diagnostics, engineDiagnostics));
    }

    TEST(SampleCLIValidation,
         FinalRenderDrainReservesOneFrameInsideReadinessBudget)
    {
        EXPECT_FALSE(RVX::SampleRunnerTestAccess::CanBeginFinalRenderDrain(
            7, 8, 600));
        EXPECT_TRUE(RVX::SampleRunnerTestAccess::CanBeginFinalRenderDrain(
            8, 8, 600));
        EXPECT_TRUE(RVX::SampleRunnerTestAccess::CanBeginFinalRenderDrain(
            599, 8, 600));
        EXPECT_FALSE(RVX::SampleRunnerTestAccess::CanBeginFinalRenderDrain(
            600, 8, 600));
        EXPECT_FALSE(RVX::SampleRunnerTestAccess::CanBeginFinalRenderDrain(
            8, 8, 8));
    }

    TEST(SampleCLIValidation,
         RuntimeFrameWaitUsesProducerPublicationBarrierDuringLongRender)
    {
        RVX::RuntimeFrameWaitRequest request;
        request.minimumPublishedSequence = 60;
        request.minimumSubmittedSequence = 60;
        request.minimumPresentedSequence = 60;

        RVX::RenderDiagnosticsSnapshot diagnostics;
        diagnostics.lastPublishedFrameSequence = 59;
        diagnostics.lastSubmittedFrameSequence = 59;
        diagnostics.lastPresentedFrameSequence = 59;

        RVX::EngineRenderRuntimeDiagnostics engineDiagnostics;
        engineDiagnostics.available = true;
        engineDiagnostics.requiredSceneFrameSequence = 66;
        EXPECT_TRUE(RVX::RuntimeFrameDriverTestAccess::UsesProgressPoll(
            request, diagnostics, engineDiagnostics));
        EXPECT_FALSE(RVX::RuntimeFrameDriverTestAccess::HasReached(
            request, diagnostics));

        engineDiagnostics.available = false;
        EXPECT_TRUE(RVX::RuntimeFrameDriverTestAccess::UsesFullTick(
            request, diagnostics, engineDiagnostics));
    }

    TEST(SampleCLIValidation,
         DeterministicFrameContractNeverIssuesHiddenSimulationTicks)
    {
        RVX::RuntimeFrameWaitRequest ordinaryRequest;
        RVX::SampleRunnerTestAccess::ConfigureFrameWaitPolicy(
            false, 1.0f / 60.0f, ordinaryRequest);
        EXPECT_TRUE(ordinaryRequest.advanceEngine);
        EXPECT_FALSE(ordinaryRequest.pumpRenderProgressOnly);

        RVX::RuntimeFrameWaitRequest deterministicRequest;
        RVX::SampleRunnerTestAccess::ConfigureFrameWaitPolicy(
            true, 1.0f / 120.0f, deterministicRequest);
        EXPECT_FALSE(deterministicRequest.advanceEngine);
        EXPECT_TRUE(deterministicRequest.pumpRenderProgressOnly);
        EXPECT_FLOAT_EQ(deterministicRequest.deltaTime, 1.0f / 120.0f);

        RVX::RenderDiagnosticsSnapshot diagnostics;
        diagnostics.lastPublishedFrameSequence = 9;
        deterministicRequest.minimumPublishedSequence = 10;
        deterministicRequest.minimumSubmittedSequence = 10;
        deterministicRequest.minimumPresentedSequence = 10;
        EXPECT_TRUE(RVX::RuntimeFrameDriverTestAccess::UsesProgressPoll(
            deterministicRequest, diagnostics));
        EXPECT_FALSE(RVX::RuntimeFrameDriverTestAccess::UsesFullTick(
            deterministicRequest, diagnostics));
    }

    TEST(SampleCLIValidation,
         ManifestContractResolvesExactDeterministicCaptureDefaults)
    {
        RVX::SampleScenarioManifest manifest;
        manifest.id = "model-rendering";
        manifest.defaultRecipe =
            RVX::SamplePipelineRecipe::ForwardStandard;
        manifest.defaultQualityProfile = RVX::SampleQualityTier::High;
        manifest.fixedTime = {1u, 60u, 0x1234u};
        RVX::SampleCameraBookmark bookmark;
        bookmark.id = "hero";
        bookmark.position = {0.0f, 1.0f, 4.0f};
        bookmark.target = {0.0f, 1.0f, 0.0f};
        manifest.cameraBookmarks.push_back(bookmark);
        manifest.captureTicks = {8u};
        manifest.visualContractVersion = 1u;

        RVX::SampleRunnerCLIOptions options;
        options.sampleId = manifest.id;
        options.common.screenshotPath = "capture.ppm";
        options.common.frames = 100u;
        std::string error;
        ASSERT_TRUE(RVX::SampleRunnerTestAccess::ResolveManifestContract(
            manifest, options, error))
            << error;
        EXPECT_TRUE(options.manifestContractResolved);
        EXPECT_TRUE(options.deterministicContractActive);
        EXPECT_TRUE(options.viewBookmarkResolved);
        EXPECT_TRUE(options.captureTickResolved);
        EXPECT_EQ(options.viewBookmarkId, "hero");
        EXPECT_EQ(options.captureTick, 8u);
        EXPECT_EQ(options.common.frames, 8u);
        EXPECT_EQ(options.qualityTier, RVX::SampleQualityTier::High);
        EXPECT_EQ(options.common.quality, "high");
        EXPECT_EQ(options.fixedTimeNumerator, 1u);
        EXPECT_EQ(options.fixedTimeDenominator, 60u);
        EXPECT_EQ(options.deterministicSeed, 0x1234u);
        EXPECT_DOUBLE_EQ(options.fixedDeltaSeconds, 1.0 / 60.0);
    }

    TEST(SampleCLIValidation,
         ManifestContractRejectsUnknownViewTickAndInexactTime)
    {
        RVX::SampleScenarioManifest manifest;
        manifest.id = "model-rendering";
        manifest.defaultRecipe =
            RVX::SamplePipelineRecipe::ForwardStandard;
        manifest.defaultQualityProfile = RVX::SampleQualityTier::Low;
        manifest.fixedTime = {1u, 60u, 7u};
        RVX::SampleCameraBookmark bookmark;
        bookmark.id = "hero";
        manifest.cameraBookmarks.push_back(bookmark);
        manifest.captureTicks = {8u};
        manifest.visualContractVersion = 1u;

        RVX::SampleRunnerCLIOptions unknownView;
        unknownView.sampleId = manifest.id;
        unknownView.viewBookmarkExplicit = true;
        unknownView.viewBookmarkId = "missing";
        std::string error;
        EXPECT_FALSE(RVX::SampleRunnerTestAccess::ResolveManifestContract(
            manifest, unknownView, error));
        EXPECT_NE(error.find("Unknown --view"), std::string::npos);

        RVX::SampleRunnerCLIOptions unknownTick;
        unknownTick.sampleId = manifest.id;
        unknownTick.common.screenshotPath = "capture.ppm";
        unknownTick.captureTickExplicit = true;
        unknownTick.captureTick = 9u;
        error.clear();
        EXPECT_FALSE(RVX::SampleRunnerTestAccess::ResolveManifestContract(
            manifest, unknownTick, error));
        EXPECT_NE(error.find("not declared"), std::string::npos);

        RVX::SampleRunnerCLIOptions inexactTime;
        inexactTime.sampleId = manifest.id;
        inexactTime.fixedDeltaExplicit = true;
        inexactTime.fixedDeltaSeconds = 1.0 / 30.0;
        error.clear();
        EXPECT_FALSE(RVX::SampleRunnerTestAccess::ResolveManifestContract(
            manifest, inexactTime, error));
        EXPECT_NE(error.find("does not match"), std::string::npos);
    }

    TEST(SampleCLIValidation,
         ManifestCapabilitiesMustMatchAssessmentGatingPolicy)
    {
        RVX::SampleScenarioManifest manifest;
        manifest.requiredCapabilities = {
            RVX::AssessmentCode("RENDER.CAPABILITY.REQUIRED")};
        manifest.optionalCapabilities = {
            RVX::AssessmentCode("RENDER.CAPABILITY.OPTIONAL")};
        RVX::SampleAssessmentContract contract =
            RVX::MakeDefaultSampleAssessmentContract();
        contract.capabilities = {
            {RVX::AssessmentCode("RENDER.CAPABILITY.REQUIRED"),
             "Required capability", true, "Required by scenario"},
            {RVX::AssessmentCode("RENDER.CAPABILITY.OPTIONAL"),
             "Optional capability", false, "Optional by scenario"}};

        std::string error;
        EXPECT_TRUE(
            RVX::SampleRunnerTestAccess::ValidateManifestAssessmentContract(
                manifest, contract, error))
            << error;
        contract.capabilities[0].gating = false;
        error.clear();
        EXPECT_FALSE(
            RVX::SampleRunnerTestAccess::ValidateManifestAssessmentContract(
                manifest, contract, error));
        EXPECT_NE(error.find("not a gating capability"),
                  std::string::npos);
    }

    TEST(SampleCLIValidation,
         Gfx19HiZReflectionProfileReplacesTheMutuallyExclusiveGtaoGate)
    {
        RVX::SampleScenarioManifest manifest;
        manifest.requiredCapabilities = {
            RVX::AssessmentCode("RENDER.CAPABILITY.POST_PROCESS_BLOOM"),
            RVX::AssessmentCode("RENDER.CAPABILITY.POST_PROCESS_TONE_MAPPING"),
            RVX::AssessmentCode("RENDER.CAPABILITY.POST_PROCESS_COLOR_GRADING"),
            RVX::AssessmentCode("RENDER.CAPABILITY.POST_PROCESS_FXAA"),
            RVX::AssessmentCode("RENDER.CAPABILITY.GTAO")};
        RVX::SampleAssessmentContract contract =
            RVX::MakeDefaultSampleAssessmentContract();
        contract.revision = "post-processing-v3";
        contract.capabilities = {
            {RVX::AssessmentCode("RENDER.CAPABILITY.POST_PROCESS_BLOOM"),
             "Bloom base qualification", true, "Base scenario contract"},
            {RVX::AssessmentCode("RENDER.CAPABILITY.POST_PROCESS_TONE_MAPPING"),
             "Tone mapping base qualification", true, "Base scenario contract"},
            {RVX::AssessmentCode("RENDER.CAPABILITY.POST_PROCESS_COLOR_GRADING"),
             "Color grading base qualification", true, "Base scenario contract"},
            {RVX::AssessmentCode("RENDER.CAPABILITY.POST_PROCESS_FXAA"),
             "FXAA base qualification", true, "Base scenario contract"},
            {RVX::AssessmentCode("RENDER.CAPABILITY.GTAO"),
             "GTAO base qualification", true, "Base scenario contract"}};

        std::string error;
        EXPECT_TRUE(
            RVX::SampleRunnerTestAccess::ApplyHiZReflectionQualificationProfile(
                "post-processing", false, manifest, contract, error))
            << error;
        EXPECT_EQ(manifest.requiredCapabilities.size(), 5u);
        EXPECT_EQ(contract.capabilities.size(), 5u);

        EXPECT_TRUE(
            RVX::SampleRunnerTestAccess::ApplyHiZReflectionQualificationProfile(
                "post-processing", true, manifest, contract, error))
            << error;
        ASSERT_EQ(manifest.requiredCapabilities.size(), 5u);
        ASSERT_EQ(contract.capabilities.size(), 5u);
        const auto hasRequired = [&manifest](std::string_view code)
        {
            return std::any_of(
                manifest.requiredCapabilities.begin(),
                manifest.requiredCapabilities.end(),
                [code](const RVX::AssessmentCode& value)
                {
                    return value.GetValue() == code;
                });
        };
        EXPECT_TRUE(hasRequired("RENDER.CAPABILITY.POST_PROCESS_BLOOM"));
        EXPECT_TRUE(hasRequired("RENDER.CAPABILITY.POST_PROCESS_TONE_MAPPING"));
        EXPECT_TRUE(hasRequired("RENDER.CAPABILITY.POST_PROCESS_COLOR_GRADING"));
        EXPECT_TRUE(hasRequired("RENDER.CAPABILITY.POST_PROCESS_FXAA"));
        EXPECT_TRUE(hasRequired("RENDER.CAPABILITY.HIZ_SCREEN_SPACE_REFLECTION"));
        EXPECT_FALSE(hasRequired("RENDER.CAPABILITY.GTAO"));
        EXPECT_TRUE(contract.capabilities.back().gating);
        EXPECT_TRUE(
            RVX::SampleRunnerTestAccess::ValidateManifestAssessmentContract(
                manifest, contract, error))
            << error;

        RVX::SampleScenarioManifest wrongSampleManifest = manifest;
        RVX::SampleAssessmentContract wrongSampleContract = contract;
        error.clear();
        EXPECT_FALSE(
            RVX::SampleRunnerTestAccess::ApplyHiZReflectionQualificationProfile(
                "model-viewer", true, wrongSampleManifest,
                wrongSampleContract, error));
        EXPECT_NE(error.find("post-processing"), std::string::npos);
    }

    TEST(SampleCLIValidation,
         RuntimeFrameWaitCaptureBarrierUsesSaturatingPublicationTarget)
    {
        constexpr RVX::uint64 PublishedBeforeOuterTick = 41;
        constexpr RVX::uint64 CaptureRequestId = 7;
        const RVX::uint64 target =
            RVX::SampleRunnerTestAccess::ResolveFrameWaitTargetSequence(
                PublishedBeforeOuterTick, PublishedBeforeOuterTick);
        EXPECT_EQ(target, PublishedBeforeOuterTick + 1u);
        EXPECT_EQ(
            RVX::SampleRunnerTestAccess::ResolveFrameWaitTargetSequence(
                PublishedBeforeOuterTick, target),
            target);
        EXPECT_EQ(
            RVX::SampleRunnerTestAccess::ResolveFrameWaitTargetSequence(
                std::numeric_limits<RVX::uint64>::max(),
                std::numeric_limits<RVX::uint64>::max()),
            std::numeric_limits<RVX::uint64>::max());

        RVX::RuntimeFrameWaitRequest request;
        request.minimumPublishedSequence = target;
        request.minimumSubmittedSequence = target;
        request.minimumPresentedSequence = target;
        request.captureRequestId = CaptureRequestId;

        RVX::RenderDiagnosticsSnapshot diagnostics;
        diagnostics.lastPublishedFrameSequence = PublishedBeforeOuterTick;
        diagnostics.lastSubmittedFrameSequence = PublishedBeforeOuterTick;
        diagnostics.lastPresentedFrameSequence = PublishedBeforeOuterTick;
        diagnostics.lastCapture.code = RVX::RenderFrameCaptureResultCode::Completed;
        diagnostics.lastCapture.requestId = CaptureRequestId;
        diagnostics.lastCapture.frameSequence = target;
        diagnostics.lastCapture.width = 1;
        diagnostics.lastCapture.height = 1;
        diagnostics.lastCapture.rowPitch = 4;
        diagnostics.lastCapture.bytesPerPixel = 4;
        diagnostics.lastCapture.bytes = {0, 0, 0, 0};

        EXPECT_TRUE(RVX::RuntimeFrameDriverTestAccess::UsesFullTick(
            request, diagnostics));
        EXPECT_FALSE(RVX::RuntimeFrameDriverTestAccess::HasReached(
            request, diagnostics));

        diagnostics.lastPublishedFrameSequence = target;
        diagnostics.lastSubmittedFrameSequence = target;
        diagnostics.lastPresentedFrameSequence = target;
        EXPECT_TRUE(RVX::RuntimeFrameDriverTestAccess::HasReached(
            request, diagnostics));
    }

    TEST(SampleCLIValidation,
         SceneRemovalBarrierPinsAcceptedRevisionBeforePresentation)
    {
        constexpr RVX::uint64 RemovalSceneRevision = 93;
        constexpr RVX::uint64 CarryingPublication = 57;
        RVX::EngineRenderRuntimeDiagnostics engineDiagnostics;
        engineDiagnostics.available = true;
        engineDiagnostics.requiredSceneFrameSequence = CarryingPublication;
        engineDiagnostics.requiredSceneRevision = RemovalSceneRevision - 1u;

        EXPECT_EQ(
            RVX::SampleRunnerTestAccess::ResolveSceneRemovalPublicationSequence(
                RemovalSceneRevision, engineDiagnostics),
            0u);

        engineDiagnostics.requiredSceneRevision = RemovalSceneRevision;
        const RVX::uint64 pinnedPublication =
            RVX::SampleRunnerTestAccess::ResolveSceneRemovalPublicationSequence(
                RemovalSceneRevision, engineDiagnostics);
        EXPECT_EQ(pinnedPublication, CarryingPublication);

        RVX::RenderDiagnosticsSnapshot diagnostics;
        diagnostics.sceneValues.available = true;
        diagnostics.sceneValues.frameSequence = CarryingPublication;
        diagnostics.sceneValues.requiredSceneRevision = RemovalSceneRevision;
        diagnostics.sceneValues.appliedSceneRevision = RemovalSceneRevision;
        diagnostics.lastPublishedFrameSequence = CarryingPublication;
        diagnostics.lastSubmittedFrameSequence = CarryingPublication;
        diagnostics.lastPresentedFrameSequence = CarryingPublication;

        diagnostics.lifecycle = RVX::RenderLifecycleState::Running;
        EXPECT_TRUE(
            RVX::SampleRunnerTestAccess::HasCompletedSceneRemovalPublication(
                true,
                false,
                RemovalSceneRevision,
                pinnedPublication,
                diagnostics));

        diagnostics.sceneValues.appliedSceneRevision =
            RemovalSceneRevision - 1u;
        EXPECT_FALSE(
            RVX::SampleRunnerTestAccess::HasCompletedSceneRemovalPublication(
                true,
                false,
                RemovalSceneRevision,
                pinnedPublication,
                diagnostics));

        diagnostics.sceneValues.appliedSceneRevision = RemovalSceneRevision;
        diagnostics.lastSubmittedFrameSequence = CarryingPublication - 1u;
        EXPECT_FALSE(
            RVX::SampleRunnerTestAccess::HasCompletedSceneRemovalPublication(
                true,
                false,
                RemovalSceneRevision,
                pinnedPublication,
                diagnostics));

        diagnostics.lastSubmittedFrameSequence = CarryingPublication;
        diagnostics.lastPresentedFrameSequence = CarryingPublication - 1u;
        EXPECT_FALSE(
            RVX::SampleRunnerTestAccess::HasCompletedSceneRemovalPublication(
                true,
                false,
                RemovalSceneRevision,
                pinnedPublication,
                diagnostics));

        diagnostics.lastPresentedFrameSequence = CarryingPublication;
        diagnostics.lastFailure.available = true;
        diagnostics.lastFailure.runtime.resultClass =
            RVX::RenderResultClass::FrameFatal;
        EXPECT_FALSE(
            RVX::SampleRunnerTestAccess::HasCompletedSceneRemovalPublication(
                true,
                false,
                RemovalSceneRevision,
                pinnedPublication,
                diagnostics));

        diagnostics.lastFailure.available = false;
        diagnostics.lifecycle = RVX::RenderLifecycleState::Stopped;
        EXPECT_FALSE(
            RVX::SampleRunnerTestAccess::HasCompletedSceneRemovalPublication(
                true,
                false,
                RemovalSceneRevision,
                pinnedPublication,
                diagnostics));

        diagnostics.lifecycle = RVX::RenderLifecycleState::Running;
        EXPECT_FALSE(
            RVX::SampleRunnerTestAccess::HasCompletedSceneRemovalPublication(
                false,
                false,
                RemovalSceneRevision,
                pinnedPublication,
                diagnostics));
        EXPECT_FALSE(
            RVX::SampleRunnerTestAccess::HasCompletedSceneRemovalPublication(
                true,
                true,
                RemovalSceneRevision,
                pinnedPublication,
                diagnostics));
    }
} // namespace
