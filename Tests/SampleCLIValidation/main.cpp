#include "Samples/SampleCLI.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <iterator>
#include <sstream>
#include <string>

namespace
{
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
        report.pass = true;

        std::ostringstream stream;
        RVX::WriteSampleReportJson(stream, report);
        const std::string json = stream.str();

        EXPECT_NE(json.find("\"schemaId\": \"RVX.SampleReport\""), std::string::npos);
        EXPECT_NE(json.find("\"schemaVersion\": 1"), std::string::npos);
        EXPECT_NE(json.find("\"sampleName\": \"BasicRHI\""), std::string::npos);
        EXPECT_NE(json.find("\"category\": \"sample\""), std::string::npos);
        EXPECT_NE(json.find("\"requestedBackend\": \"dx12\""), std::string::npos);
        EXPECT_NE(json.find("\"backend\": \"dx12\""), std::string::npos);
        EXPECT_NE(json.find("\"frameCount\": 1"), std::string::npos);
        EXPECT_NE(json.find("\"renderPath\": \"gpu-driven\""),
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
        EXPECT_NE(json.find("\"renderDiagnostics\": {"), std::string::npos);
        EXPECT_NE(json.find("\"renderGraphTotalPasses\": 3"), std::string::npos);
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
} // namespace
