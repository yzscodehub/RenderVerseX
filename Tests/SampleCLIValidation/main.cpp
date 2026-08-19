#include "Render/GPUScene/GPUSceneDiagnostics.h"
#include "Engine/RenderRuntimeDiagnostics.h"
#include "Samples/RuntimeFrameDriver.h"
#include "Samples/SampleCLI.h"
#include "Samples/SampleLifetimeQualification.h"
#include "Samples/SampleRunner.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>

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
        EXPECT_TRUE(options.diagnostics);
        EXPECT_FALSE(options.enableValidation);
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
        EXPECT_NE(json.find("\"schemaVersion\": 19"), std::string::npos);
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

        EXPECT_NE(json.find("\"schemaVersion\": 19"), std::string::npos);
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

        EXPECT_NE(json.find("\"schemaVersion\": 19"), std::string::npos);
        EXPECT_NE(json.find("\"packageContentId\""), std::string::npos);
        EXPECT_NE(json.find(asset.packageContentId.digest), std::string::npos);
        EXPECT_NE(json.find("\"cook\": {\"required\": true, \"accepted\": true, \"code\": \"accepted\""),
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
