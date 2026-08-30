#include "Samples/RenderQualityPerformanceWriter.h"

#include "Core/Diagnostics/JsonWriter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <locale>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

namespace RVX
{
namespace
{
    inline constexpr uint32 RequiredWarmupFrameCount = 60U;
    inline constexpr uint32 RequiredMeasuredFrameCount = 300U;
    inline constexpr float64 QualificationSpanRelativeTolerance = 1e-9;

    void SetError(std::string* outError, std::string value)
    {
        if (outError != nullptr)
        {
            *outError = std::move(value);
        }
    }

    [[nodiscard]] bool IsNonEmptyText(
        const std::string& value, const size_t maximumLength = 1024U) noexcept
    {
        return !value.empty() && value.size() <= maximumLength;
    }

    [[nodiscard]] bool IsSafeIdentifier(const std::string& value) noexcept
    {
        if (!IsNonEmptyText(value, 128U))
        {
            return false;
        }
        return std::all_of(value.begin(), value.end(), [](const char character)
        {
            return (character >= 'a' && character <= 'z') ||
                   (character >= 'A' && character <= 'Z') ||
                   (character >= '0' && character <= '9') ||
                   character == '-' || character == '_' || character == '.';
        });
    }

    [[nodiscard]] bool IsLowercaseHex(
        const std::string& value, const size_t expectedLength) noexcept
    {
        if (value.size() != expectedLength)
        {
            return false;
        }
        return std::all_of(value.begin(), value.end(), [](const char character)
        {
            return (character >= '0' && character <= '9') ||
                   (character >= 'a' && character <= 'f');
        });
    }

    [[nodiscard]] bool IsFiniteNonNegative(const float64 value) noexcept
    {
        return std::isfinite(value) && value >= 0.0;
    }

    [[nodiscard]] bool HasExpectedQualificationSpan(
        const RenderQualityPerformanceReportSample& sample) noexcept
    {
        if (!IsFiniteNonNegative(
                sample.graphicsTerminalCommandSpanMilliseconds) ||
            !IsFiniteNonNegative(sample.renderGraphPassEnvelopeMilliseconds) ||
            !IsFiniteNonNegative(sample.graphicsQualificationSpanMilliseconds))
        {
            return false;
        }
        const float64 expected = std::max(
            sample.graphicsTerminalCommandSpanMilliseconds,
            sample.renderGraphPassEnvelopeMilliseconds);
        const float64 scale = std::max(
            {1.0, std::abs(expected),
             std::abs(sample.graphicsQualificationSpanMilliseconds)});
        return std::abs(sample.graphicsQualificationSpanMilliseconds - expected) <=
               QualificationSpanRelativeTolerance * scale;
    }

    /**
     * Policies use the public RHI spelling (DX12/Vulkan/Metal), while the
     * Render session uses lower-case machine identifiers. Normalize only this
     * stable enum vocabulary at the host boundary; arbitrary strings fail.
     */
    [[nodiscard]] std::string_view GetCanonicalBackendName(
        const std::string& value) noexcept
    {
        if (value == "dx11" || value == "DX11") return "DX11";
        if (value == "dx12" || value == "DX12") return "DX12";
        if (value == "vulkan" || value == "Vulkan") return "Vulkan";
        if (value == "metal" || value == "Metal") return "Metal";
        if (value == "opengl" || value == "OpenGL") return "OpenGL";
        return {};
    }

    [[nodiscard]] bool ValidateIdentity(
        const RenderQualityPerformanceReportIdentity& identity,
        std::string* outError)
    {
        if (identity.sampleId != "render-quality-lab" ||
            !IsSafeIdentifier(identity.pipeline) ||
            !IsSafeIdentifier(identity.quality) ||
            GetCanonicalBackendName(identity.backend).empty() ||
            !IsSafeIdentifier(identity.view) ||
            identity.visualContractVersion == 0)
        {
            SetError(outError,
                     "RenderQuality performance qualification identity is invalid");
            return false;
        }
        return true;
    }

    [[nodiscard]] bool ValidateHardwareIdentity(
        const SampleRenderQualityPerformanceHardwareIdentity& identity,
        const std::string& expectedBackend,
        std::string* outError)
    {
        if (GetCanonicalBackendName(identity.backend).empty() ||
            GetCanonicalBackendName(identity.backend) !=
                GetCanonicalBackendName(expectedBackend) ||
            !IsNonEmptyText(identity.adapterName) ||
            !IsNonEmptyText(identity.driverVersion) ||
            !IsNonEmptyText(identity.osBuild) ||
            !IsLowercaseHex(identity.capabilityHash, 64U))
        {
            SetError(outError,
                     "RenderQuality performance hardware identity is invalid or does not match the frozen backend");
            return false;
        }
        return true;
    }

    [[nodiscard]] bool ValidateTelemetryCoverage(
        const RenderQualityPerformanceTelemetryCoverage& coverage,
        std::string* outError)
    {
        if (coverage.schemaVersion !=
                RVX_RENDER_QUALITY_PERFORMANCE_TELEMETRY_COVERAGE_SCHEMA_VERSION ||
            coverage.trackedUploadCoverageMask !=
                RVX_RENDER_QUALITY_PERFORMANCE_REQUIRED_TRACKED_UPLOAD_COVERAGE ||
            coverage.trackedEstimatedFootprintCoverageMask !=
                RVX_RENDER_QUALITY_PERFORMANCE_REQUIRED_TRACKED_FOOTPRINT_COVERAGE)
        {
            SetError(outError,
                     "RenderQuality performance telemetry coverage is incomplete or unsupported");
            return false;
        }
        return true;
    }

    [[nodiscard]] bool ValidateSample(
        const RenderQualityPerformanceReportSample& sample,
        const RenderQualityPerformanceTelemetryCoverage& coverage,
        const uint32 index,
        std::string* outError)
    {
        if (!HasExpectedQualificationSpan(sample))
        {
            SetError(
                outError,
                "RenderQuality performance sample " + std::to_string(index) +
                    " does not equal the conservative maximum of its two "
                    "completion-owned Graphics spans");
            return false;
        }
        if (sample.frameSequence == 0 || sample.submittedFrame == 0 ||
            sample.completedFrame < sample.submittedFrame ||
            !sample.completionOwned ||
            !IsLowercaseHex(sample.renderExecutionReceiptSha256, 64U) ||
            !IsFiniteNonNegative(sample.extractionMilliseconds) ||
            !IsFiniteNonNegative(sample.visibilityMilliseconds) ||
            !IsFiniteNonNegative(sample.graphCompileMilliseconds) ||
            !IsFiniteNonNegative(sample.submissionMilliseconds) ||
            !IsFiniteNonNegative(
                sample.graphicsTerminalCommandSpanMilliseconds) ||
            !IsFiniteNonNegative(sample.renderGraphPassEnvelopeMilliseconds) ||
            !IsFiniteNonNegative(sample.graphicsQualificationSpanMilliseconds) ||
            sample.passGpuTimings.empty() ||
            sample.trackedUploadCoverageMask !=
                coverage.trackedUploadCoverageMask ||
            sample.trackedEstimatedFootprint.schemaVersion !=
                coverage.schemaVersion ||
            sample.trackedEstimatedFootprint.coverageMask !=
                coverage.trackedEstimatedFootprintCoverageMask ||
            sample.resourceCountsSchemaVersion != coverage.schemaVersion ||
            sample.resourceCounts.empty())
        {
            SetError(outError,
                     "RenderQuality performance sample " + std::to_string(index) +
                         " lacks completion-owned timing or telemetry");
            return false;
        }

        for (size_t passIndex = 0;
             passIndex < sample.passGpuTimings.size(); ++passIndex)
        {
            const RenderQualityPerformanceReportPassTiming& pass =
                sample.passGpuTimings[passIndex];
            if (pass.executionIndex != passIndex || !IsNonEmptyText(pass.name) ||
                !IsFiniteNonNegative(pass.durationMilliseconds))
            {
                SetError(outError,
                         "RenderQuality performance sample has an invalid ordered GPU pass measurement");
                return false;
            }
        }
        for (const auto& [name, count] : sample.resourceCounts)
        {
            static_cast<void>(count);
            if (!IsNonEmptyText(name))
            {
                SetError(outError,
                         "RenderQuality performance sample has an invalid resource counter");
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool HasMatchingPassSchema(
        const std::vector<RenderQualityPerformanceReportPassTiming>& expected,
        const std::vector<RenderQualityPerformanceReportPassTiming>& actual)
        noexcept
    {
        if (actual.size() != expected.size())
        {
            return false;
        }
        for (size_t index = 0; index < expected.size(); ++index)
        {
            if (actual[index].executionIndex != expected[index].executionIndex ||
                actual[index].name != expected[index].name)
            {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool ValidateRequest(
        const RenderQualityPerformanceWriteRequest& request,
        std::string* outError)
    {
        if (request.outputPath.empty() ||
            request.outputPath.filename() != "performance.json" ||
            (!IsLowercaseHex(request.sourceCommit, 40U) &&
             !IsLowercaseHex(request.sourceCommit, 64U)) ||
            !IsSafeIdentifier(request.runId) || !IsSafeIdentifier(request.preset) ||
            !IsSafeIdentifier(request.baselineIdentity) ||
            request.completedWarmupFrames != RequiredWarmupFrameCount ||
             request.samples.size() != RequiredMeasuredFrameCount ||
             !ValidateIdentity(request.qualificationIdentity, outError) ||
             !ValidateHardwareIdentity(request.hardwareIdentity,
                                       request.qualificationIdentity.backend,
                                       outError) ||
             !ValidateTelemetryCoverage(request.telemetryCoverage, outError))
        {
            if (outError != nullptr && outError->empty())
            {
                *outError =
                    "RenderQuality performance writer request is incomplete or invalid";
            }
            return false;
        }

        uint64 previousFrameSequence = 0;
        uint64 previousSubmittedFrame = 0;
        uint64 previousCompletedFrame = request.completedWarmupFrames - 1U;
        std::vector<RenderQualityPerformanceReportPassTiming> passSchema;
        std::map<std::string, uint64> resourceSchema;
        std::array<uint64, 3> trackedFootprintCeilings{};
        std::set<std::string> observedReceipts;
        for (uint32 index = 0; index < request.samples.size(); ++index)
        {
            const RenderQualityPerformanceReportSample& sample =
                request.samples[index];
            if (!ValidateSample(sample, request.telemetryCoverage, index, outError) ||
                sample.frameSequence <= previousFrameSequence ||
                sample.submittedFrame <= previousSubmittedFrame ||
                sample.completedFrame <= previousCompletedFrame ||
                !observedReceipts.insert(
                    sample.renderExecutionReceiptSha256).second)
            {
                if (outError != nullptr && outError->empty())
                {
                    *outError =
                        "RenderQuality performance samples are stale, unordered, or reuse receipt evidence";
                }
                return false;
            }
            if (index == 0)
            {
                passSchema = sample.passGpuTimings;
                resourceSchema = sample.resourceCounts;
                trackedFootprintCeilings = {
                    sample.trackedEstimatedFootprint
                        .transientPoolPhysicalEstimatedBytes,
                    sample.trackedEstimatedFootprint
                        .importedHistoryNamedEstimatedBytes,
                    sample.trackedEstimatedFootprint
                        .resourceRegistryResidentEstimatedBytes};
            }
            else if (sample.passGpuTimings.size() != passSchema.size() ||
                     sample.resourceCounts.size() != resourceSchema.size())
            {
                SetError(outError,
                         "RenderQuality performance sample telemetry schema changed after warmup");
                return false;
            }
            else
            {
                if (!HasMatchingPassSchema(passSchema, sample.passGpuTimings))
                {
                    SetError(outError,
                             "RenderQuality performance GPU pass schema changed after warmup");
                    return false;
                }
                for (const auto& [name, ignored] : resourceSchema)
                {
                    static_cast<void>(ignored);
                    const auto candidate = sample.resourceCounts.find(name);
                    if (candidate == sample.resourceCounts.end() ||
                        candidate->second > resourceSchema.at(name))
                    {
                        SetError(outError,
                                 "RenderQuality performance resource telemetry grew or changed after warmup");
                        return false;
                    }
                }
                if (sample.trackedEstimatedFootprint
                        .transientPoolPhysicalEstimatedBytes >
                        trackedFootprintCeilings[0] ||
                    sample.trackedEstimatedFootprint
                        .importedHistoryNamedEstimatedBytes >
                        trackedFootprintCeilings[1] ||
                    sample.trackedEstimatedFootprint
                        .resourceRegistryResidentEstimatedBytes >
                        trackedFootprintCeilings[2])
                {
                    SetError(outError,
                             "RenderQuality performance tracked estimated footprint grew after warmup");
                    return false;
                }
            }
            previousFrameSequence = sample.frameSequence;
            previousSubmittedFrame = sample.submittedFrame;
            previousCompletedFrame = sample.completedFrame;
        }
        return true;
    }

    [[nodiscard]] bool PrepareOutputPath(
        const std::filesystem::path& requestedPath,
        std::filesystem::path& outPath,
        std::string* outError)
    {
        std::error_code error;
        const std::filesystem::path absolute =
            std::filesystem::absolute(requestedPath, error);
        if (error || absolute.empty())
        {
            SetError(outError,
                     "Cannot resolve RenderQuality performance output path");
            return false;
        }

        const std::filesystem::path root = absolute.parent_path();
        if (root.empty() || !std::filesystem::exists(root, error) || error ||
            !std::filesystem::is_directory(root, error) || error)
        {
            SetError(outError,
                     "RenderQuality performance output parent must already exist");
            return false;
        }
        const std::filesystem::path canonicalRoot =
            std::filesystem::weakly_canonical(root, error);
        if (error || canonicalRoot.empty())
        {
            SetError(outError,
                     "Cannot canonicalize RenderQuality performance output parent");
            return false;
        }
        const std::filesystem::path candidate =
            canonicalRoot / requestedPath.filename();
        if (candidate.parent_path() != canonicalRoot ||
            candidate.filename() != "performance.json" ||
            std::filesystem::exists(candidate, error) || error)
        {
            SetError(outError,
                     "RenderQuality performance output already exists or escapes its parent");
            return false;
        }
        outPath = candidate;
        return true;
    }

    void WriteMilliseconds(std::ostream& stream, const float64 value)
    {
        stream << std::setprecision(std::numeric_limits<float64>::max_digits10)
               << value;
    }

    void WriteSampleJson(
        std::ostream& stream,
        const RenderQualityPerformanceReportSample& sample)
    {
        stream << "    {\n";
        stream << "      \"frameSequence\": " << sample.frameSequence << ",\n";
        stream << "      \"submittedFrame\": " << sample.submittedFrame
               << ",\n";
        stream << "      \"completedFrame\": " << sample.completedFrame
               << ",\n";
        stream << "      \"renderExecutionReceiptSha256\": "
               << Diagnostics::JsonString(sample.renderExecutionReceiptSha256)
               << ",\n";
        stream << "      \"completionOwned\": true,\n";
        stream << "      \"cpuMs\": {\n";
        stream << "        \"extraction\": ";
        WriteMilliseconds(stream, sample.extractionMilliseconds);
        stream << ",\n        \"visibility\": ";
        WriteMilliseconds(stream, sample.visibilityMilliseconds);
        stream << ",\n        \"graphCompile\": ";
        WriteMilliseconds(stream, sample.graphCompileMilliseconds);
        stream << ",\n        \"submission\": ";
        WriteMilliseconds(stream, sample.submissionMilliseconds);
        stream << "\n      },\n";
        stream << "      \"gpu\": {\n"
               << "        \"graphicsTerminalCommandSpanMs\": ";
        WriteMilliseconds(stream, sample.graphicsTerminalCommandSpanMilliseconds);
        stream << ",\n        \"renderGraphPassEnvelopeMs\": ";
        WriteMilliseconds(stream, sample.renderGraphPassEnvelopeMilliseconds);
        stream << ",\n        \"graphicsQualificationSpanMs\": ";
        WriteMilliseconds(stream, sample.graphicsQualificationSpanMilliseconds);
        stream << ",\n        \"passes\": [\n";
        for (size_t index = 0; index < sample.passGpuTimings.size(); ++index)
        {
            const RenderQualityPerformanceReportPassTiming& pass =
                sample.passGpuTimings[index];
            stream << "          {\"executionIndex\": "
                   << pass.executionIndex << ", \"name\": "
                   << Diagnostics::JsonString(pass.name)
                   << ", \"durationMs\": ";
            WriteMilliseconds(stream, pass.durationMilliseconds);
            stream << "}" << (index + 1U < sample.passGpuTimings.size()
                                    ? ",\n"
                                    : "\n");
        }
        stream << "        ]\n      },\n";
        stream << "      \"trackedUploadBytes\": "
               << sample.trackedUploadBytes << ",\n";
        stream << "      \"trackedUploadCoverageMask\": "
               << sample.trackedUploadCoverageMask << ",\n";
        stream << "      \"trackedEstimatedFootprint\": {\n";
        stream << "        \"schemaVersion\": "
               << sample.trackedEstimatedFootprint.schemaVersion << ",\n";
        stream << "        \"coverageMask\": "
               << sample.trackedEstimatedFootprint.coverageMask << ",\n";
        stream << "        \"transientPoolPhysicalEstimatedBytes\": "
               << sample.trackedEstimatedFootprint
                      .transientPoolPhysicalEstimatedBytes
               << ",\n";
        stream << "        \"importedHistoryNamedEstimatedBytes\": "
               << sample.trackedEstimatedFootprint
                      .importedHistoryNamedEstimatedBytes
               << ",\n";
        stream << "        \"resourceRegistryResidentEstimatedBytes\": "
               << sample.trackedEstimatedFootprint
                      .resourceRegistryResidentEstimatedBytes
               << "\n      },\n";
        stream << "      \"resourceCounts\": {\n";
        stream << "        \"schemaVersion\": "
               << sample.resourceCountsSchemaVersion << ",\n";
        stream << "        \"counts\": {\n";
        for (auto iterator = sample.resourceCounts.begin();
             iterator != sample.resourceCounts.end(); ++iterator)
        {
            stream << "          " << Diagnostics::JsonString(iterator->first)
                   << ": " << iterator->second;
            stream << (std::next(iterator) != sample.resourceCounts.end()
                           ? ",\n"
                           : "\n");
        }
        stream << "        }\n      }\n    }";
    }
} // namespace

bool WriteRenderQualityPerformanceReport(
    const RenderQualityPerformanceWriteRequest& request,
    RenderQualityPerformanceWriteResult& outResult,
    std::string* outError)
{
    outResult = {};
    if (!ValidateRequest(request, outError))
    {
        return false;
    }

    std::filesystem::path outputPath;
    if (!PrepareOutputPath(request.outputPath, outputPath, outError))
    {
        return false;
    }
    const std::filesystem::path temporaryPath =
        outputPath.parent_path() / ".performance.json.rvx-tmp";
    std::error_code error;
    if (std::filesystem::exists(temporaryPath, error) || error)
    {
        SetError(outError,
                 "RenderQuality performance temporary output already exists");
        return false;
    }

    std::ostringstream json;
    json.imbue(std::locale::classic());
    json << "{\n";
    json << "  \"schema\": \"RVX.RenderQualityPerformanceReport\",\n";
    json << "  \"schemaVersion\": "
         << RVX_RENDER_QUALITY_PERFORMANCE_REPORT_SCHEMA_VERSION << ",\n";
    json << "  \"sourceCommit\": "
         << Diagnostics::JsonString(request.sourceCommit) << ",\n";
    json << "  \"runId\": " << Diagnostics::JsonString(request.runId)
         << ",\n";
    json << "  \"preset\": " << Diagnostics::JsonString(request.preset)
         << ",\n";
    json << "  \"qualificationIdentity\": {\n";
    json << "    \"sampleId\": "
         << Diagnostics::JsonString(request.qualificationIdentity.sampleId)
         << ",\n";
    json << "    \"pipeline\": "
         << Diagnostics::JsonString(request.qualificationIdentity.pipeline)
         << ",\n";
    json << "    \"quality\": "
         << Diagnostics::JsonString(request.qualificationIdentity.quality)
         << ",\n";
    json << "    \"backend\": "
         << Diagnostics::JsonString(std::string(
                GetCanonicalBackendName(request.qualificationIdentity.backend)))
         << ",\n";
    json << "    \"view\": "
         << Diagnostics::JsonString(request.qualificationIdentity.view) << ",\n";
    json << "    \"captureTick\": "
         << request.qualificationIdentity.captureTick << ",\n";
    json << "    \"visualContractVersion\": "
         << request.qualificationIdentity.visualContractVersion << "\n";
    json << "  },\n";
    json << "  \"hardwareIdentity\": {\n";
    json << "    \"backend\": "
         << Diagnostics::JsonString(std::string(
                GetCanonicalBackendName(request.hardwareIdentity.backend)))
         << ",\n";
    json << "    \"adapterName\": "
         << Diagnostics::JsonString(request.hardwareIdentity.adapterName)
         << ",\n";
    json << "    \"driverVersion\": "
         << Diagnostics::JsonString(request.hardwareIdentity.driverVersion)
         << ",\n";
    json << "    \"osBuild\": "
         << Diagnostics::JsonString(request.hardwareIdentity.osBuild) << ",\n";
    json << "    \"capabilityHash\": "
         << Diagnostics::JsonString(request.hardwareIdentity.capabilityHash)
         << "\n";
    json << "  },\n";
    json << "  \"telemetryCoverage\": {\n";
    json << "    \"schemaVersion\": "
         << request.telemetryCoverage.schemaVersion << ",\n";
    json << "    \"trackedUploadCoverageMask\": "
         << request.telemetryCoverage.trackedUploadCoverageMask << ",\n";
    json << "    \"trackedEstimatedFootprintCoverageMask\": "
         << request.telemetryCoverage.trackedEstimatedFootprintCoverageMask
         << "\n";
    json << "  },\n";
    json << "  \"baselineIdentity\": "
         << Diagnostics::JsonString(request.baselineIdentity) << ",\n";
    json << "  \"completedWarmupFrames\": "
         << request.completedWarmupFrames << ",\n";
    json << "  \"samples\": [\n";
    for (size_t index = 0; index < request.samples.size(); ++index)
    {
        WriteSampleJson(json, request.samples[index]);
        json << (index + 1U < request.samples.size() ? ",\n" : "\n");
    }
    json << "  ]\n}\n";

    {
        std::ofstream stream(temporaryPath, std::ios::binary | std::ios::trunc);
        if (!stream)
        {
            SetError(outError,
                     "Cannot open RenderQuality performance temporary output");
            return false;
        }
        const std::string payload = json.str();
        stream.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        stream.flush();
        if (!stream)
        {
            stream.close();
            std::filesystem::remove(temporaryPath, error);
            SetError(outError,
                     "Cannot write RenderQuality performance temporary output");
            return false;
        }
    }

    std::filesystem::rename(temporaryPath, outputPath, error);
    if (error)
    {
        std::filesystem::remove(temporaryPath, error);
        SetError(outError,
                 "Cannot atomically publish RenderQuality performance output");
        return false;
    }
    outResult.reportPath = outputPath;
    return true;
}
} // namespace RVX
