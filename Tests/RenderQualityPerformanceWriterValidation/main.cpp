#include "Samples/RenderQualityPerformanceWriter.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

namespace RVX
{
namespace
{
    [[nodiscard]] std::filesystem::path MakeTemporaryRoot()
    {
        const uint64 stamp = static_cast<uint64>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        return std::filesystem::temp_directory_path() /
            ("rvx-render-quality-performance-" + std::to_string(stamp));
    }

    class TemporaryRoot final
    {
    public:
        TemporaryRoot()
            : m_path(MakeTemporaryRoot())
        {
            std::error_code error;
            std::filesystem::create_directories(m_path, error);
        }

        ~TemporaryRoot()
        {
            std::error_code error;
            std::filesystem::remove_all(m_path, error);
        }

        [[nodiscard]] const std::filesystem::path& Get() const noexcept
        {
            return m_path;
        }

    private:
        std::filesystem::path m_path;
    };

    [[nodiscard]] std::string MakeDigest(const uint64 value)
    {
        std::ostringstream stream;
        stream << std::hex << std::setfill('0') << std::setw(64) << value;
        return stream.str();
    }

    [[nodiscard]] RenderQualityPerformanceWriteRequest MakeRequest(
        const std::filesystem::path& root)
    {
        RenderQualityPerformanceWriteRequest request;
        request.outputPath = root / "performance.json";
        request.sourceCommit = std::string(40, 'a');
        request.runId = "writer-validation-run";
        request.preset = "win_x64_debug_gfx24_qualification";
        request.qualificationIdentity = {
            "render-quality-lab", "deferred", "high", "dx12", "hero",
            2560, 1440, 0, 1};
        request.hardwareIdentity = {
            "dx12", "Test Adapter", "1.2.3", "Windows-10.0.19045",
            std::string(64, 'b')};
        request.telemetryCoverage = {
            RVX_RENDER_QUALITY_PERFORMANCE_TELEMETRY_COVERAGE_SCHEMA_VERSION,
            RVX_RENDER_QUALITY_PERFORMANCE_REQUIRED_TRACKED_UPLOAD_COVERAGE,
            RVX_RENDER_QUALITY_PERFORMANCE_REQUIRED_TRACKED_FOOTPRINT_COVERAGE};
        request.baselineIdentity = "candidate-dx12-test-v1";
        request.completedWarmupFrames = 60;
        request.samples.reserve(300);
        for (uint32 index = 0; index < 300; ++index)
        {
            RenderQualityPerformanceReportSample sample;
            sample.frameSequence = index + 1;
            sample.submittedFrame = 60U + index;
            sample.completedFrame = 60U + index;
            sample.renderExecutionReceiptSha256 = MakeDigest(index + 1U);
            sample.completionOwned = true;
            sample.extractionMilliseconds = 0.1;
            sample.visibilityMilliseconds = 0.2;
            sample.graphCompileMilliseconds = 0.3;
            sample.submissionMilliseconds = 0.4;
            sample.graphicsTerminalCommandSpanMilliseconds = 1.0;
            sample.renderGraphPassEnvelopeMilliseconds = 0.9;
            sample.graphicsQualificationSpanMilliseconds = 1.0;
            // A non-zero timestamp pair may quantize to an exact 0 ms pass;
            // the Graphics terminal-command span remains advancing and
            // comparable even when an individual pass quantizes to zero.
            sample.passGpuTimings = {
                {.executionIndex = 0, .name = "Geometry", .durationMilliseconds = 0.0},
                {.executionIndex = 1, .name = "Lighting", .durationMilliseconds = 0.4},
                {.executionIndex = 2, .name = "Geometry", .durationMilliseconds = 0.2}};
            sample.trackedUploadBytes = 1024;
            sample.trackedUploadCoverageMask =
                RVX_RENDER_QUALITY_PERFORMANCE_REQUIRED_TRACKED_UPLOAD_COVERAGE;
            sample.trackedEstimatedFootprint = {
                RVX_RENDER_QUALITY_PERFORMANCE_TELEMETRY_COVERAGE_SCHEMA_VERSION,
                RVX_RENDER_QUALITY_PERFORMANCE_REQUIRED_TRACKED_FOOTPRINT_COVERAGE,
                4096,
                4096,
                88473600,
                2048};
            sample.resourceCountsSchemaVersion =
                RVX_RENDER_QUALITY_PERFORMANCE_TELEMETRY_COVERAGE_SCHEMA_VERSION;
            sample.resourceCounts = {
                {"stableImportedBuffers", 1},
                {"stableImportedTextures", 7},
                {"residentMaterials", 6},
                {"residentMeshes", 4},
                {"residentTextures", 5},
                {"transientBuffers", 2},
                {"transientTextures", 3}};
            request.samples.push_back(std::move(sample));
        }
        return request;
    }
} // namespace

TEST(RenderQualityPerformanceWriterValidation,
     PublishesFixedSessionWithCanonicalBackendSpelling)
{
    TemporaryRoot temporary;
    RenderQualityPerformanceWriteResult result;
    std::string error;
    ASSERT_TRUE(WriteRenderQualityPerformanceReport(
        MakeRequest(temporary.Get()), result, &error)) << error;
    EXPECT_EQ(temporary.Get() / "performance.json", result.reportPath);

    std::ifstream stream(result.reportPath, std::ios::binary);
    const std::string json((std::istreambuf_iterator<char>(stream)), {});
    EXPECT_NE(std::string::npos,
              json.find("\"schema\": \"RVX.RenderQualityPerformanceReport\""));
    EXPECT_NE(std::string::npos, json.find("\"schemaVersion\": 3"));
    EXPECT_NE(std::string::npos, json.find("\"completedWarmupFrames\": 60"));
    EXPECT_NE(std::string::npos, json.find("\"backend\": \"DX12\""));
    EXPECT_NE(std::string::npos,
              json.find("\"qualificationWidth\": 2560"));
    EXPECT_NE(std::string::npos,
              json.find("\"qualificationHeight\": 1440"));
    EXPECT_NE(std::string::npos,
              json.find("\"trackedUploadCoverageMask\": 7"));
    EXPECT_NE(std::string::npos,
              json.find("\"trackedEstimatedFootprint\""));
    EXPECT_NE(std::string::npos,
              json.find("\"trackedEstimatedFootprintCoverageMask\": 15"));
    EXPECT_NE(std::string::npos, json.find("\"schemaVersion\": 2"));
    EXPECT_NE(std::string::npos,
              json.find("\"importedHistoryEstimatedBytes\": 88473600"));
    EXPECT_NE(std::string::npos,
              json.find("\"importedPersistentEstimatedBytes\": 4096"));
    EXPECT_EQ(std::string::npos,
              json.find("\"importedHistoryNamedEstimatedBytes\""));
    EXPECT_NE(std::string::npos,
              json.find("\"graphicsTerminalCommandSpanMs\": 1"));
    EXPECT_NE(std::string::npos,
              json.find("\"renderGraphPassEnvelopeMs\": "));
    EXPECT_NE(std::string::npos,
              json.find("\"graphicsQualificationSpanMs\": 1"));
    EXPECT_NE(std::string::npos, json.find("\"passes\": ["));
    EXPECT_NE(std::string::npos, json.find("\"executionIndex\": 2"));
    EXPECT_NE(std::string::npos,
              json.find("\"resourceCounts\": {\n        \"schemaVersion\": 2"));
    EXPECT_NE(std::string::npos,
              json.find("\"stableImportedTextures\": 7"));
    EXPECT_EQ(std::string::npos, json.find("\"importedTextures\""));
    EXPECT_EQ(std::string::npos, json.find("\"uploadBytes\""));
    EXPECT_EQ(std::string::npos, json.find("\"memoryBytes\""));
    EXPECT_EQ(std::string::npos, json.find("\"wholeFrameMs\""));
    EXPECT_EQ(std::string::npos, json.find("\"passMs\""));
    EXPECT_NE(std::string::npos, json.find("\"samples\": ["));
}

TEST(RenderQualityPerformanceWriterValidation,
     RejectsExistingOutputAndNeverOverwritesIt)
{
    TemporaryRoot temporary;
    const std::filesystem::path output = temporary.Get() / "performance.json";
    {
        std::ofstream stream(output, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(stream);
        stream << "foreign";
    }
    RenderQualityPerformanceWriteResult result;
    std::string error;
    EXPECT_FALSE(WriteRenderQualityPerformanceReport(
        MakeRequest(temporary.Get()), result, &error));
    EXPECT_NE(std::string::npos, error.find("already exists"));

    std::ifstream stream(output, std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(stream)), {});
    EXPECT_EQ("foreign", content);
}

TEST(RenderQualityPerformanceWriterValidation,
     RejectsRepeatedReceiptCoverageDriftAndPostWarmupGrowth)
{
    TemporaryRoot temporary;
    RenderQualityPerformanceWriteRequest duplicate = MakeRequest(temporary.Get());
    duplicate.samples[1].renderExecutionReceiptSha256 =
        duplicate.samples[0].renderExecutionReceiptSha256;
    RenderQualityPerformanceWriteResult result;
    std::string error;
    EXPECT_FALSE(WriteRenderQualityPerformanceReport(duplicate, result, &error));
    EXPECT_NE(std::string::npos, error.find("reuse"));

    RenderQualityPerformanceWriteRequest coverage = MakeRequest(temporary.Get());
    coverage.samples.back().trackedUploadCoverageMask = 3;
    error.clear();
    EXPECT_FALSE(WriteRenderQualityPerformanceReport(coverage, result, &error));
    EXPECT_NE(std::string::npos, error.find("telemetry"));

    coverage = MakeRequest(temporary.Get());
    coverage.telemetryCoverage.trackedEstimatedFootprintCoverageMask = 7;
    error.clear();
    EXPECT_FALSE(WriteRenderQualityPerformanceReport(coverage, result, &error));
    EXPECT_NE(std::string::npos, error.find("telemetry"));

    coverage = MakeRequest(temporary.Get());
    coverage.samples.back().resourceCounts.erase("stableImportedTextures");
    coverage.samples.back().resourceCounts.emplace("persistentImportedTextures", 7);
    error.clear();
    EXPECT_FALSE(WriteRenderQualityPerformanceReport(coverage, result, &error));
    EXPECT_NE(std::string::npos, error.find("resource counter schema"));

    coverage = MakeRequest(temporary.Get());
    coverage.samples.back().trackedEstimatedFootprint.coverageMask = 3;
    error.clear();
    EXPECT_FALSE(WriteRenderQualityPerformanceReport(coverage, result, &error));
    EXPECT_NE(std::string::npos, error.find("telemetry"));

    coverage = MakeRequest(temporary.Get());
    coverage.samples.back().resourceCountsSchemaVersion =
        RVX_RENDER_QUALITY_PERFORMANCE_TELEMETRY_COVERAGE_SCHEMA_VERSION + 1U;
    error.clear();
    EXPECT_FALSE(WriteRenderQualityPerformanceReport(coverage, result, &error));
    EXPECT_NE(std::string::npos, error.find("telemetry"));

    RenderQualityPerformanceWriteRequest stableCountChange =
        MakeRequest(temporary.Get());
    ++stableCountChange.samples.back().resourceCounts.at(
        "stableImportedTextures");
    error.clear();
    EXPECT_FALSE(WriteRenderQualityPerformanceReport(
        stableCountChange, result, &error));
    EXPECT_NE(std::string::npos, error.find("resource telemetry changed"));

    RenderQualityPerformanceWriteRequest growth = MakeRequest(temporary.Get());
    ++growth.samples.back()
          .trackedEstimatedFootprint.resourceRegistryResidentEstimatedBytes;
    error.clear();
    EXPECT_FALSE(WriteRenderQualityPerformanceReport(growth, result, &error));
    EXPECT_NE(std::string::npos, error.find("changed"));

    RenderQualityPerformanceWriteRequest importedHistoryGrowth =
        MakeRequest(temporary.Get());
    ++importedHistoryGrowth.samples.back()
          .trackedEstimatedFootprint.importedHistoryEstimatedBytes;
    error.clear();
    EXPECT_FALSE(WriteRenderQualityPerformanceReport(
        importedHistoryGrowth, result, &error));
    EXPECT_NE(std::string::npos, error.find("changed"));

    RenderQualityPerformanceWriteRequest importedPersistentGrowth =
        MakeRequest(temporary.Get());
    ++importedPersistentGrowth.samples.back()
          .trackedEstimatedFootprint.importedPersistentEstimatedBytes;
    error.clear();
    EXPECT_FALSE(WriteRenderQualityPerformanceReport(
        importedPersistentGrowth, result, &error));
    EXPECT_NE(std::string::npos, error.find("changed"));

    RenderQualityPerformanceWriteRequest importedPersistentDecrease =
        MakeRequest(temporary.Get());
    --importedPersistentDecrease.samples.back()
          .trackedEstimatedFootprint.importedPersistentEstimatedBytes;
    error.clear();
    EXPECT_FALSE(WriteRenderQualityPerformanceReport(
        importedPersistentDecrease, result, &error));
    EXPECT_NE(std::string::npos, error.find("changed"));

    RenderQualityPerformanceWriteRequest mismatchedQualificationSpan =
        MakeRequest(temporary.Get());
    mismatchedQualificationSpan.samples.front()
        .graphicsQualificationSpanMilliseconds = 0.5;
    error.clear();
    EXPECT_FALSE(WriteRenderQualityPerformanceReport(
        mismatchedQualificationSpan, result, &error));
    EXPECT_NE(std::string::npos, error.find("conservative maximum"));

    RenderQualityPerformanceWriteRequest passSchemaDrift =
        MakeRequest(temporary.Get());
    passSchemaDrift.samples.back().passGpuTimings[1].name = "Changed";
    error.clear();
    EXPECT_FALSE(WriteRenderQualityPerformanceReport(
        passSchemaDrift, result, &error));
    EXPECT_NE(std::string::npos, error.find("schema changed"));

    RenderQualityPerformanceWriteRequest invalidPassIndex =
        MakeRequest(temporary.Get());
    invalidPassIndex.samples.front().passGpuTimings[1].executionIndex = 2;
    error.clear();
    EXPECT_FALSE(WriteRenderQualityPerformanceReport(
        invalidPassIndex, result, &error));
    EXPECT_NE(std::string::npos, error.find("ordered GPU"));

    RenderQualityPerformanceWriteRequest zeroExtent =
        MakeRequest(temporary.Get());
    zeroExtent.qualificationIdentity.qualificationWidth = 0;
    error.clear();
    EXPECT_FALSE(WriteRenderQualityPerformanceReport(zeroExtent, result, &error));
    EXPECT_NE(std::string::npos, error.find("identity"));
}

TEST(RenderQualityPerformanceWriterValidation,
     PermitsTimestampResolutionQuantizedZeroDurationSpans)
{
    TemporaryRoot temporary;
    RenderQualityPerformanceWriteRequest request = MakeRequest(temporary.Get());
    for (RenderQualityPerformanceReportSample& sample : request.samples)
    {
        sample.graphicsTerminalCommandSpanMilliseconds = 0.0;
        sample.renderGraphPassEnvelopeMilliseconds = 0.0;
        sample.graphicsQualificationSpanMilliseconds = 0.0;
    }

    RenderQualityPerformanceWriteResult result;
    std::string error;
    EXPECT_TRUE(WriteRenderQualityPerformanceReport(request, result, &error))
        << error;
}

TEST(RenderQualityPerformanceWriterValidation,
     RejectsUnderBoundFullResolutionTAAHistory)
{
    TemporaryRoot temporary;
    RenderQualityPerformanceWriteRequest request = MakeRequest(temporary.Get());
    request.samples.front().passGpuTimings[1].name = "TAAResolve";
    request.samples.front().trackedEstimatedFootprint
        .importedHistoryEstimatedBytes = 88473599ULL;

    RenderQualityPerformanceWriteResult result;
    std::string error;
    EXPECT_FALSE(WriteRenderQualityPerformanceReport(request, result, &error));
    EXPECT_NE(std::string::npos, error.find("below 88473600"));
}

TEST(RenderQualityPerformanceWriterValidation,
     PermitsRenderGraphEnvelopeBeyondTheTerminalCriticalPath)
{
    TemporaryRoot temporary;
    RenderQualityPerformanceWriteRequest request = MakeRequest(temporary.Get());
    for (RenderQualityPerformanceReportSample& sample : request.samples)
    {
        sample.graphicsTerminalCommandSpanMilliseconds = 1.0;
        sample.renderGraphPassEnvelopeMilliseconds = 1.25;
        sample.graphicsQualificationSpanMilliseconds = 1.25;
    }

    RenderQualityPerformanceWriteResult result;
    std::string error;
    EXPECT_TRUE(WriteRenderQualityPerformanceReport(request, result, &error))
        << error;
}
} // namespace RVX
