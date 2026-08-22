#include "Samples/SampleContext.h"
#include "Samples/SampleRegistry.h"
#include "Samples/SampleRunner.h"
#include "Samples/Sample.h"

#include <gtest/gtest.h>

#include <iterator>
#include <memory>
#include <string>
#include <utility>

namespace
{
    class TestSample final : public RVX::ISample
    {
    public:
        explicit TestSample(RVX::SampleInfo info) : m_info(std::move(info)) {}

        const RVX::SampleInfo& GetInfo() const noexcept override
        {
            return m_info;
        }

        bool Setup(RVX::SampleContext&, std::string&) override { return true; }
        void Update(RVX::SampleContext&, float) override {}
        void OnInput(RVX::SampleContext&) override {}
        void AppendReport(RVX::SampleFeatureReporter&) const override {}
        void Shutdown(RVX::SampleContext&) override {}

    private:
        RVX::SampleInfo m_info;
    };

    RVX::SampleInfo MakeInfo(std::string id)
    {
        RVX::SampleInfo info;
        info.id = std::move(id);
        info.displayName = info.id;
        info.description = info.id + " description";
        info.defaultAssetId = "fixture";
        return info;
    }

    RVX::SampleScenarioManifest MakeManifest(std::string id)
    {
        RVX::SampleScenarioManifest manifest;
        manifest.id = std::move(id);
        manifest.defaultRecipe =
            RVX::SamplePipelineRecipe::ForwardStandard;
        manifest.defaultQualityProfile = RVX::SampleQualityTier::Low;
        manifest.cookedAssetIds = {"fixture"};
        manifest.visualContractVersion = 1;
        return manifest;
    }

    TEST(SampleRegistryValidation, ListsSamplesInStableIdOrder)
    {
        RVX::SampleRegistry registry;
        ASSERT_TRUE(registry.Register(
            MakeInfo("zeta"),
            MakeManifest("zeta"),
            []() { return std::make_unique<TestSample>(MakeInfo("zeta")); }));
        ASSERT_TRUE(registry.Register(
            MakeInfo("alpha"),
            MakeManifest("alpha"),
            []() { return std::make_unique<TestSample>(MakeInfo("alpha")); }));

        const auto samples = registry.List();
        ASSERT_EQ(samples.size(), 2u);
        EXPECT_EQ(samples[0].id, "alpha");
        EXPECT_EQ(samples[1].id, "zeta");
        ASSERT_NE(registry.Find("alpha"), nullptr);
        ASSERT_NE(registry.FindManifest("alpha"), nullptr);
        EXPECT_EQ(registry.Find("alpha")->visualContractId, "alpha");
        ASSERT_NE(registry.Create("alpha"), nullptr);
        EXPECT_EQ(registry.Create("alpha")->GetInfo().id, "alpha");

        RVX::SampleManifestCatalog catalog;
        catalog.contentSha256 = std::string(64, 'b');
        catalog.samples.emplace("alpha", MakeManifest("alpha"));
        catalog.samples.emplace("zeta", MakeManifest("zeta"));
        std::string error;
        ASSERT_TRUE(registry.ValidateManifestCoverage(catalog, &error))
            << error;
        EXPECT_EQ(registry.GetManifestSchemaVersion(), 1u);
        EXPECT_EQ(registry.GetManifestContentSha256(),
                  std::string_view(std::string(64, 'b')));
        EXPECT_FALSE(registry.Register(
            MakeInfo("late"),
            MakeManifest("late"),
            []() { return std::make_unique<TestSample>(MakeInfo("late")); },
            &error));
        EXPECT_NE(error.find("sealed"), std::string::npos);
    }

    TEST(SampleRegistryValidation, RejectsDuplicateAndInvalidEntries)
    {
        RVX::SampleRegistry registry;
        std::string error;
        ASSERT_TRUE(registry.Register(
            MakeInfo("model"),
            MakeManifest("model"),
            []() { return std::make_unique<TestSample>(MakeInfo("model")); },
            &error));
        EXPECT_FALSE(registry.Register(
            MakeInfo("model"),
            MakeManifest("model"),
            []() { return std::make_unique<TestSample>(MakeInfo("model")); },
            &error));
        EXPECT_NE(error.find("Duplicate"), std::string::npos);

        RVX::SampleInfo emptyInfo;
        EXPECT_FALSE(registry.Register(
            emptyInfo,
            MakeManifest("unused"),
            []() { return std::make_unique<TestSample>(MakeInfo("unused")); },
            &error));
        EXPECT_NE(error.find("must not be empty"), std::string::npos);
    }

    TEST(SampleRegistryValidation, FailsClosedOnManifestMismatchAndOrphans)
    {
        RVX::SampleRegistry registry;
        std::string error;
        EXPECT_FALSE(registry.Register(
            MakeInfo("model"),
            MakeManifest("other"),
            []() { return std::make_unique<TestSample>(MakeInfo("model")); },
            &error));
        EXPECT_NE(error.find("does not match"), std::string::npos);

        RVX::SampleInfo missingAsset = MakeInfo("missing-asset");
        RVX::SampleScenarioManifest incomplete =
            MakeManifest("missing-asset");
        incomplete.cookedAssetIds.clear();
        error.clear();
        EXPECT_FALSE(registry.Register(
            std::move(missingAsset),
            std::move(incomplete),
            []()
            {
                return std::make_unique<TestSample>(
                    MakeInfo("missing-asset"));
            },
            &error));
        EXPECT_NE(error.find("omits catalog asset"), std::string::npos);

        ASSERT_TRUE(registry.Register(
            MakeInfo("alpha"),
            MakeManifest("alpha"),
            []() { return std::make_unique<TestSample>(MakeInfo("alpha")); },
            &error));
        RVX::SampleManifestCatalog catalog;
        catalog.contentSha256 = std::string(64, 'a');
        catalog.samples.emplace("alpha", MakeManifest("alpha"));
        catalog.samples.emplace("orphan", MakeManifest("orphan"));
        EXPECT_FALSE(registry.ValidateManifestCoverage(catalog, &error));
        EXPECT_NE(error.find("count mismatch"), std::string::npos);
    }

    TEST(SampleRegistryValidation, ParsesHostAndCommonOptionsTogether)
    {
        const char* argv[] = {
            "RenderVerseSamples",
            "--sample",
            "model-rendering",
            "--asset",
            "fixture",
            "--environment",
            "studio",
            "--backend",
            "dx12",
            "--smoke",
            "--quality",
            "low",
            "--catalog",
            "catalog.json",
            "--asset-root",
            "assets",
            "--render-path",
            "gpu-driven",
            "--instancing",
            "auto",
            "--wait-ready",
            "--ready-timeout-ms",
            "30000",
            "--ready-max-frames",
            "64",
        };

        RVX::SampleRunnerCLIOptions options;
        std::string error;
        ASSERT_TRUE(RVX::ParseSampleRunnerCLI(
            static_cast<int>(std::size(argv)), argv, options, &error))
            << error;
        EXPECT_EQ(options.sampleId, "model-rendering");
        EXPECT_EQ(options.assetId, "fixture");
        EXPECT_EQ(options.environmentId, "studio");
        EXPECT_EQ(options.common.backend, RVX::RHIBackendType::DX12);
        EXPECT_EQ(options.common.frames, 8u);
        EXPECT_EQ(options.common.quality, "low");
        EXPECT_EQ(options.catalogPath, "catalog.json");
        EXPECT_EQ(options.assetRoot, "assets");
        EXPECT_TRUE(options.renderPathExplicit);
        EXPECT_EQ(options.renderPath, RVX::SampleRenderPath::GPUDriven);
        EXPECT_EQ(options.instancingMode, RVX::RenderInstancingMode::Auto);
        EXPECT_TRUE(options.waitReady);
        EXPECT_EQ(options.readyTimeoutMs, 30000u);
        EXPECT_EQ(options.readyMaxFrames, 64u);
    }

    TEST(SampleRegistryValidation, RejectsAmbiguousAssetsAndAutoSmoke)
    {
        const char* ambiguousArgv[] = {
            "RenderVerseSamples",
            "--asset",
            "fixture",
            "--model",
            "fixture.gltf",
        };
        RVX::SampleRunnerCLIOptions ambiguousOptions;
        std::string error;
        EXPECT_FALSE(RVX::ParseSampleRunnerCLI(
            static_cast<int>(std::size(ambiguousArgv)),
            ambiguousArgv,
            ambiguousOptions,
            &error));
        EXPECT_NE(error.find("mutually exclusive"), std::string::npos);

        const char* unboundedReadinessArgv[] = {
            "RenderVerseSamples",
            "--wait-ready",
        };
        RVX::SampleRunnerCLIOptions unboundedReadinessOptions;
        error.clear();
        EXPECT_FALSE(RVX::ParseSampleRunnerCLI(
            static_cast<int>(std::size(unboundedReadinessArgv)),
            unboundedReadinessArgv,
            unboundedReadinessOptions,
            &error));
        EXPECT_NE(error.find("finite --frames"), std::string::npos);

        const char* invalidReadinessRangeArgv[] = {
            "RenderVerseSamples",
            "--frames",
            "8",
            "--wait-ready",
            "--ready-max-frames",
            "4",
        };
        RVX::SampleRunnerCLIOptions invalidReadinessRangeOptions;
        error.clear();
        EXPECT_FALSE(RVX::ParseSampleRunnerCLI(
            static_cast<int>(std::size(invalidReadinessRangeArgv)),
            invalidReadinessRangeArgv,
            invalidReadinessRangeOptions,
            &error));
        EXPECT_NE(error.find("greater than or equal"), std::string::npos);

        const char* invalidRenderPathArgv[] = {
            "RenderVerseSamples",
            "--render-path",
            "backend-specific-magic",
        };
        RVX::SampleRunnerCLIOptions invalidRenderPathOptions;
        error.clear();
        EXPECT_FALSE(RVX::ParseSampleRunnerCLI(
            static_cast<int>(std::size(invalidRenderPathArgv)),
            invalidRenderPathArgv,
            invalidRenderPathOptions,
            &error));
        EXPECT_NE(error.find("Invalid --render-path"), std::string::npos);

        const char* invalidInstancingArgv[] = {
            "RenderVerseSamples",
            "--instancing",
            "backend-specific-magic",
        };
        RVX::SampleRunnerCLIOptions invalidInstancingOptions;
        error.clear();
        EXPECT_FALSE(RVX::ParseSampleRunnerCLI(
            static_cast<int>(std::size(invalidInstancingArgv)),
            invalidInstancingArgv,
            invalidInstancingOptions,
            &error));
        EXPECT_NE(error.find("Invalid --instancing"), std::string::npos);

        const char* smokeArgv[] = {"RenderVerseSamples", "--smoke"};
        RVX::SampleRunnerCLIOptions smokeOptions;
        error.clear();
        EXPECT_FALSE(RVX::ParseSampleRunnerCLI(
            static_cast<int>(std::size(smokeArgv)),
            smokeArgv,
            smokeOptions,
            &error));
        EXPECT_NE(error.find("explicit --backend"), std::string::npos);

        const char* environmentAmbiguousArgv[] = {
            "RenderVerseSamples",
            "--environment",
            "studio",
            "--environment-file",
            "studio.hdr",
        };
        RVX::SampleRunnerCLIOptions environmentAmbiguousOptions;
        error.clear();
        EXPECT_FALSE(RVX::ParseSampleRunnerCLI(
            static_cast<int>(std::size(environmentAmbiguousArgv)),
            environmentAmbiguousArgv,
            environmentAmbiguousOptions,
            &error));
        EXPECT_NE(error.find("mutually exclusive"), std::string::npos);
    }

    TEST(SampleRegistryValidation, AllowsExplicitModelWithCatalogEnvironment)
    {
        const char* argv[] = {
            "RenderVerseSamples",
            "--model",
            "fixture.gltf",
            "--environment",
            "studio",
            "--catalog",
            "catalog.json",
            "--asset-root",
            "assets",
        };
        RVX::SampleRunnerCLIOptions options;
        std::string error;
        EXPECT_TRUE(RVX::ParseSampleRunnerCLI(
            static_cast<int>(std::size(argv)), argv, options, &error))
            << error;
        EXPECT_EQ(options.modelPath, "fixture.gltf");
        EXPECT_EQ(options.environmentId, "studio");
    }

    TEST(SampleRegistryValidation, ParsesDeterministicGraphicsContractOptions)
    {
        const char* argv[] = {
            "RenderVerseSamples",
            "--pipeline",
            "forward-plus",
            "--quality",
            "high",
            "--view",
            "hero",
            "--fixed-delta",
            "0.0166666666666667",
            "--capture-tick",
            "32",
            "--screenshot",
            "capture.ppm",
        };

        RVX::SampleRunnerCLIOptions options;
        std::string error;
        ASSERT_TRUE(RVX::ParseSampleRunnerCLI(
            static_cast<int>(std::size(argv)), argv, options, &error))
            << error;
        EXPECT_TRUE(options.pipelineExplicit);
        EXPECT_EQ(options.pipelineRecipe,
                  RVX::SamplePipelineRecipe::ForwardStandard);
        EXPECT_TRUE(options.qualityExplicit);
        EXPECT_EQ(options.qualityTier, RVX::SampleQualityTier::High);
        EXPECT_TRUE(options.viewBookmarkExplicit);
        EXPECT_EQ(options.viewBookmarkId, "hero");
        EXPECT_TRUE(options.fixedDeltaExplicit);
        EXPECT_DOUBLE_EQ(options.fixedDeltaSeconds, 0.0166666666666667);
        EXPECT_TRUE(options.captureTickExplicit);
        EXPECT_EQ(options.captureTick, 32u);
        EXPECT_EQ(options.common.frames, 32u);
    }

    TEST(SampleRegistryValidation, RejectsInvalidGraphicsContractOptions)
    {
        const auto rejects = [](const char* option,
                                const char* value,
                                std::string_view diagnostic)
        {
            const char* argv[] = {
                "RenderVerseSamples",
                option,
                value,
            };
            RVX::SampleRunnerCLIOptions options;
            std::string error;
            EXPECT_FALSE(RVX::ParseSampleRunnerCLI(
                static_cast<int>(std::size(argv)), argv, options, &error));
            EXPECT_NE(error.find(diagnostic), std::string::npos) << error;
        };

        rejects("--pipeline", "backend-private", "Invalid --pipeline");
        rejects("--quality", "cinematic-ish", "Invalid --quality");
        rejects("--fixed-delta", "nan", "Invalid --fixed-delta");
        rejects("--fixed-delta", "0", "Invalid --fixed-delta");
        rejects("--capture-tick", "0", "Invalid --capture-tick");

        const char* captureWithoutImage[] = {
            "RenderVerseSamples",
            "--capture-tick",
            "8",
        };
        RVX::SampleRunnerCLIOptions options;
        std::string error;
        EXPECT_FALSE(RVX::ParseSampleRunnerCLI(
            static_cast<int>(std::size(captureWithoutImage)),
            captureWithoutImage,
            options,
            &error));
        EXPECT_NE(error.find("requires --screenshot"), std::string::npos);
    }

    TEST(SampleRegistryValidation, ListDoesNotRequireBackendSelection)
    {
        const char* argv[] = {"RenderVerseSamples", "--list", "--smoke"};
        RVX::SampleRunnerCLIOptions options;
        std::string error;
        EXPECT_TRUE(RVX::ParseSampleRunnerCLI(
            static_cast<int>(std::size(argv)), argv, options, &error))
            << error;
        EXPECT_TRUE(options.listSamples);
    }

    TEST(SampleRegistryValidation, ReadinessIsTriStateAndCaptureImplicitlyWaits)
    {
        const RVX::SampleReadiness pending =
            RVX::SampleReadiness::Pending("loading");
        const RVX::SampleReadiness ready = RVX::SampleReadiness::Ready();
        const RVX::SampleReadiness failed =
            RVX::SampleReadiness::Failed("decode failed");
        EXPECT_FALSE(pending.IsReady());
        EXPECT_FALSE(pending.IsFailed());
        EXPECT_TRUE(ready.IsReady());
        EXPECT_TRUE(failed.IsFailed());
        EXPECT_EQ(failed.reason, "decode failed");

        RVX::SampleRunnerCLIOptions options;
        EXPECT_FALSE(options.RequiresReadinessWait());
        options.common.screenshotPath = "capture.ppm";
        EXPECT_TRUE(options.RequiresReadinessWait());
        options.common.screenshotPath.clear();
        options.waitReady = true;
        EXPECT_TRUE(options.RequiresReadinessWait());
        options.waitReady = false;
        options.lifetimeReportPath = "lifetime.json";
        EXPECT_TRUE(options.RequiresReadinessWait());
    }
} // namespace
