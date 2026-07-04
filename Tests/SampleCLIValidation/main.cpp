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
        report.backend = RVX::RHIBackendType::DX12;
        report.frameCount = 1;
        report.width = 320;
        report.height = 180;
        report.quality = "default";
        report.enabledFeatures = {"RHI", "RenderGraph"};
        report.unsupportedFeatures = {"ScreenshotCapture"};
        report.fallbackReasons = {"BasicRHI does not capture screenshots yet"};
        report.resourceDiagnostics = {"fixture loaded"};
        report.renderDiagnostics.available = true;
        report.renderDiagnostics.renderAttempted = true;
        report.renderDiagnostics.rendered = true;
        report.renderDiagnostics.graphBuilt = true;
        report.renderDiagnostics.graphCompiled = true;
        report.renderDiagnostics.renderGraphTotalPasses = 3;
        report.pass = true;

        std::ostringstream stream;
        RVX::WriteSampleReportJson(stream, report);
        const std::string json = stream.str();

        EXPECT_NE(json.find("\"sampleName\": \"BasicRHI\""), std::string::npos);
        EXPECT_NE(json.find("\"category\": \"sample\""), std::string::npos);
        EXPECT_NE(json.find("\"backend\": \"dx12\""), std::string::npos);
        EXPECT_NE(json.find("\"frameCount\": 1"), std::string::npos);
        EXPECT_NE(json.find("\"enabledFeatures\": [\"RHI\", \"RenderGraph\"]"),
                  std::string::npos);
        EXPECT_NE(json.find("\"unsupportedFeatures\": [\"ScreenshotCapture\"]"),
                  std::string::npos);
        EXPECT_NE(json.find("\"fallbackReasons\": [\"BasicRHI does not capture screenshots yet\"]"),
                  std::string::npos);
        EXPECT_NE(json.find("\"resourceDiagnostics\": [\"fixture loaded\"]"),
                  std::string::npos);
        EXPECT_NE(json.find("\"renderDiagnostics\": {"), std::string::npos);
        EXPECT_NE(json.find("\"renderGraphTotalPasses\": 3"), std::string::npos);
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
