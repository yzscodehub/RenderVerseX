#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    struct Options
    {
        std::filesystem::path startupReport;
        std::filesystem::path lifetimeReport;
        std::filesystem::path sampleReport;
        std::string backend;
        std::string renderPath;
        std::string assetMode;
        uint64_t expectedTextureCount = 27;
        uint64_t expectedVisibleObjectCount = 75;
        uint64_t maximumFirstPresentMs = 5000;
        bool requireResize = false;
    };

    struct TraceEvent
    {
        std::string name;
        uint64_t timestampNs = 0;
        std::string json;
    };

    bool ParseUnsigned(std::string_view text, uint64_t& outValue)
    {
        if (text.empty())
            return false;
        uint64_t value = 0;
        for (const char character : text)
        {
            if (character < '0' || character > '9')
                return false;
            const uint64_t digit = static_cast<uint64_t>(character - '0');
            if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10u)
                return false;
            value = value * 10u + digit;
        }
        outValue = value;
        return true;
    }

    bool ParseOptions(int argc, char* argv[], Options& options)
    {
        for (int index = 1; index < argc; ++index)
        {
            const std::string argument = argv[index] ? argv[index] : "";
            const auto value = [&](const char* name) -> const char*
            {
                if (index + 1 >= argc || argv[index + 1] == nullptr)
                {
                    std::cerr << "Missing value for " << name << "\n";
                    return nullptr;
                }
                return argv[++index];
            };

            if (argument == "--startup-report")
            {
                const char* parsed = value("--startup-report");
                if (!parsed) return false;
                options.startupReport = parsed;
            }
            else if (argument == "--lifetime-report")
            {
                const char* parsed = value("--lifetime-report");
                if (!parsed) return false;
                options.lifetimeReport = parsed;
            }
            else if (argument == "--sample-report")
            {
                const char* parsed = value("--sample-report");
                if (!parsed) return false;
                options.sampleReport = parsed;
            }
            else if (argument == "--backend")
            {
                const char* parsed = value("--backend");
                if (!parsed) return false;
                options.backend = parsed;
            }
            else if (argument == "--render-path")
            {
                const char* parsed = value("--render-path");
                if (!parsed) return false;
                options.renderPath = parsed;
            }
            else if (argument == "--asset-mode")
            {
                const char* parsed = value("--asset-mode");
                if (!parsed) return false;
                options.assetMode = parsed;
            }
            else if (argument == "--expected-texture-count")
            {
                const char* parsed = value("--expected-texture-count");
                if (!parsed || !ParseUnsigned(parsed, options.expectedTextureCount))
                {
                    std::cerr << "Invalid --expected-texture-count\n";
                    return false;
                }
            }
            else if (argument == "--max-first-present-ms")
            {
                const char* parsed = value("--max-first-present-ms");
                if (!parsed || !ParseUnsigned(parsed, options.maximumFirstPresentMs))
                {
                    std::cerr << "Invalid --max-first-present-ms\n";
                    return false;
                }
            }
            else if (argument == "--expected-visible-objects")
            {
                const char* parsed = value("--expected-visible-objects");
                if (!parsed ||
                    !ParseUnsigned(parsed, options.expectedVisibleObjectCount))
                {
                    std::cerr << "Invalid --expected-visible-objects\n";
                    return false;
                }
            }
            else if (argument == "--require-resize")
            {
                options.requireResize = true;
            }
            else
            {
                std::cerr << "Unknown argument: " << argument << "\n";
                return false;
            }
        }

        if (options.startupReport.empty() || options.lifetimeReport.empty() ||
            options.sampleReport.empty() ||
            options.backend.empty() || options.renderPath.empty() ||
            (options.assetMode != "source" && options.assetMode != "cooked"))
        {
            std::cerr << "Startup/lifetime/sample reports, backend, render path and "
                         "asset mode are required\n";
            return false;
        }
        return true;
    }

    bool ReadText(const std::filesystem::path& path, std::string& outText)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            std::cerr << "Failed to open report: " << path << "\n";
            return false;
        }
        std::ostringstream stream;
        stream << file.rdbuf();
        outText = stream.str();
        return true;
    }

    std::optional<std::string> ReadStringField(const std::string& json,
                                               std::string_view field)
    {
        const std::string marker = "\"" + std::string(field) + "\":";
        size_t cursor = json.find(marker);
        if (cursor == std::string::npos)
            return std::nullopt;
        cursor += marker.size();
        while (cursor < json.size() &&
               (json[cursor] == ' ' || json[cursor] == '\t'))
        {
            ++cursor;
        }
        if (cursor >= json.size() || json[cursor] != '"')
            return std::nullopt;
        const size_t begin = ++cursor;
        bool escaped = false;
        for (; cursor < json.size(); ++cursor)
        {
            if (!escaped && json[cursor] == '"')
                return json.substr(begin, cursor - begin);
            escaped = !escaped && json[cursor] == '\\';
            if (json[cursor] != '\\')
                escaped = false;
        }
        return std::nullopt;
    }

    std::optional<uint64_t> ReadUnsignedField(const std::string& json,
                                              std::string_view field,
                                              size_t start = 0)
    {
        const std::string marker = "\"" + std::string(field) + "\":";
        size_t cursor = json.find(marker, start);
        if (cursor == std::string::npos)
            return std::nullopt;
        cursor += marker.size();
        while (cursor < json.size() &&
               (json[cursor] == ' ' || json[cursor] == '\t'))
        {
            ++cursor;
        }
        size_t end = cursor;
        while (end < json.size() && json[end] >= '0' && json[end] <= '9')
            ++end;
        uint64_t value = 0;
        if (!ParseUnsigned(std::string_view(json).substr(cursor, end - cursor), value))
            return std::nullopt;
        return value;
    }

    bool RequireContains(const std::string& json,
                         std::string_view expected,
                         const char* label)
    {
        if (json.find(expected) != std::string::npos)
            return true;
        std::cerr << "Report is missing " << label << ": " << expected << "\n";
        return false;
    }

    bool RequireUnsigned(const std::string& json,
                         std::string_view field,
                         uint64_t expected)
    {
        const std::optional<uint64_t> value = ReadUnsignedField(json, field);
        if (value && *value == expected)
            return true;
        std::cerr << "Field " << field << " is "
                  << (value ? std::to_string(*value) : "missing")
                  << ", expected " << expected << "\n";
        return false;
    }

    bool RequireEveryUnsignedEquals(const std::string& json,
                                    std::string_view field,
                                    uint64_t expected)
    {
        const std::string marker = "\"" + std::string(field) + "\":";
        size_t cursor = 0;
        size_t count = 0;
        bool passed = true;
        while ((cursor = json.find(marker, cursor)) != std::string::npos)
        {
            const std::optional<uint64_t> value =
                ReadUnsignedField(json, field, cursor);
            if (!value || *value != expected)
            {
                std::cerr << "Occurrence of " << field
                          << " does not equal " << expected << "\n";
                passed = false;
            }
            cursor += marker.size();
            ++count;
        }
        if (count == 0)
        {
            std::cerr << "Report is missing field " << field << "\n";
            return false;
        }
        return passed;
    }

    std::vector<TraceEvent> ParseTraceEvents(const std::string& json)
    {
        std::vector<TraceEvent> events;
        std::istringstream stream(json);
        std::string line;
        while (std::getline(stream, line))
        {
            const std::optional<std::string> name = ReadStringField(line, "name");
            const std::optional<uint64_t> timestamp =
                ReadUnsignedField(line, "timestampNs");
            if (name && timestamp)
                events.push_back({*name, *timestamp, std::move(line)});
        }
        return events;
    }

    std::vector<const TraceEvent*> FindEvents(
        const std::vector<TraceEvent>& events,
        std::string_view name)
    {
        std::vector<const TraceEvent*> found;
        for (const TraceEvent& event : events)
        {
            if (event.name == name)
                found.push_back(&event);
        }
        return found;
    }

    const TraceEvent* RequireUniqueEvent(const std::vector<TraceEvent>& events,
                                         std::string_view name,
                                         bool& passed)
    {
        const std::vector<const TraceEvent*> found = FindEvents(events, name);
        if (found.size() != 1)
        {
            std::cerr << "Expected exactly one " << name << " event, found "
                      << found.size() << "\n";
            passed = false;
            return nullptr;
        }
        return found.front();
    }

    bool ValidateStartup(const Options& options, const std::string& json)
    {
        bool passed = true;
        passed &= RequireContains(json,
                                  "\"schemaId\": \"RVX.SampleStartupTimeline\"",
                                  "startup schema");
        passed &= RequireContains(json, "\"schemaVersion\": 1", "startup schema version");
        passed &= RequireContains(json, "\"sample\": \"model-viewer\"", "sample metadata");
        passed &= RequireContains(json,
                                  "\"backend\": \"" + options.backend + "\"",
                                  "backend metadata");

        const std::vector<TraceEvent> events = ParseTraceEvents(json);
        const TraceEvent* setup = RequireUniqueEvent(events, "SetupReturned", passed);
        const TraceEvent* firstPublished =
            RequireUniqueEvent(events, "FirstFramePublished", passed);
        const TraceEvent* firstSubmitted =
            RequireUniqueEvent(events, "FirstFrameSubmitted", passed);
        const TraceEvent* firstPresent =
            RequireUniqueEvent(events, "FirstSwapchainPresentAccepted", passed);
        const TraceEvent* cpuPublish = RequireUniqueEvent(events, "CPUPublish", passed);
        const TraceEvent* sceneInstantiate =
            RequireUniqueEvent(events, "SceneInstantiate", passed);
        const TraceEvent* minimumResident =
            RequireUniqueEvent(events, "MinimumResident", passed);
        const TraceEvent* fullyResident =
            RequireUniqueEvent(events, "FullyResident", passed);
        const TraceEvent* modelPresented =
            RequireUniqueEvent(events, "FirstModelFramePresented", passed);

        for (const std::string_view required : {
                 "ProcessStart", "EngineInitialize", "WindowReady",
                 "RenderRuntimeReady", "SwapchainReady", "SampleSetup",
                 "AssetResolve", "ModelIO", "CPUPrepare", "UploadQueued",
                 "UploadSubmitted"})
        {
            if (FindEvents(events, required).empty())
            {
                std::cerr << "Startup trace is missing event " << required << "\n";
                passed = false;
            }
        }

        if (setup && firstPublished && firstSubmitted && firstPresent && cpuPublish &&
            sceneInstantiate && minimumResident && fullyResident && modelPresented)
        {
            if (!(setup->timestampNs <= firstPublished->timestampNs &&
                  firstPublished->timestampNs <= firstSubmitted->timestampNs &&
                  firstSubmitted->timestampNs <= firstPresent->timestampNs &&
                  firstPresent->timestampNs < cpuPublish->timestampNs &&
                  cpuPublish->timestampNs <= sceneInstantiate->timestampNs &&
                  sceneInstantiate->timestampNs <= minimumResident->timestampNs &&
                  minimumResident->timestampNs <= fullyResident->timestampNs &&
                  fullyResident->timestampNs <= modelPresented->timestampNs))
            {
                std::cerr << "Startup, first-frame and residency milestones are out of order\n";
                passed = false;
            }
            const uint64_t firstPresentDelta =
                firstPresent->timestampNs - setup->timestampNs;
            if (firstPresentDelta > options.maximumFirstPresentMs * 1000000ull)
            {
                std::cerr << "First present took " << firstPresentDelta / 1000000ull
                          << " ms after SetupReturned\n";
                passed = false;
            }
            passed &= RequireUnsigned(firstPublished->json, "sequence", 1);
            passed &= RequireUnsigned(firstPresent->json, "sequence", 1);
        }

        const std::vector<const TraceEvent*> encodedReads =
            FindEvents(events, "TextureEncodedRead");
        const std::vector<const TraceEvent*> decodes =
            FindEvents(events, "TextureDecode");
        const std::vector<const TraceEvent*> parses =
            FindEvents(events, "GltfParse");
        const uint64_t expectedDecodeCount =
            options.assetMode == "source" ? options.expectedTextureCount : 0;
        const uint64_t expectedParseCount = options.assetMode == "source" ? 1 : 0;
        if (encodedReads.size() != options.expectedTextureCount ||
            decodes.size() != expectedDecodeCount ||
            parses.size() != expectedParseCount)
        {
            std::cerr << "Unexpected source/cooked parse pipeline counts: encoded="
                      << encodedReads.size() << ", decode=" << decodes.size()
                      << ", gltf=" << parses.size() << "\n";
            passed = false;
        }

        std::set<std::string> decodedPaths;
        for (const TraceEvent* event : decodes)
        {
            const std::optional<std::string> path =
                ReadStringField(event->json, "path");
            if (!path || !decodedPaths.insert(*path).second)
            {
                std::cerr << "Texture decode path is missing or duplicated\n";
                passed = false;
            }
        }
        for (const TraceEvent* event : encodedReads)
        {
            const std::optional<std::string> path =
                ReadStringField(event->json, "path");
            const bool cookedPath = path && path->find(".rva") != std::string::npos;
            if ((options.assetMode == "cooked") != cookedPath)
            {
                std::cerr << "Texture read did not use the expected source/cooked product kind\n";
                passed = false;
            }
        }

        passed &= RequireUnsigned(json,
                                  "modelTextureCompletedDecodes",
                                  expectedDecodeCount);
        passed &= RequireUnsigned(json, "modelTextureFailedDecodes", 0);
        passed &= RequireUnsigned(json, "modelTextureCancelledDecodes", 0);
        passed &= RequireUnsigned(json, "modelTextureActiveDecodes", 0);
        passed &= RequireUnsigned(json, "modelTextureQueuedDecodes", 0);
        passed &= RequireUnsigned(json, "modelTexturePendingPublications", 0);
        passed &= RequireUnsigned(json, "modelTextureReservedDecodedBytes", 0);

        const std::optional<uint64_t> budget =
            ReadUnsignedField(json, "modelTextureDecodedByteBudget");
        const std::optional<uint64_t> peak =
            ReadUnsignedField(json, "modelTexturePeakDecodedBytes");
        const std::optional<uint64_t> completedBytes =
            ReadUnsignedField(json, "modelTextureCompletedDecodedBytes");
        const std::optional<uint64_t> peakConcurrent =
            ReadUnsignedField(json, "modelTexturePeakConcurrentDecodes");
        if (!budget || !peak || !completedBytes || !peakConcurrent || *budget == 0 ||
            *peak > *budget || *peakConcurrent > 4 ||
            (options.assetMode == "source" &&
             (*peak == 0 || *completedBytes == 0 || *peakConcurrent == 0)) ||
            (options.assetMode == "cooked" &&
             (*peak != 0 || *completedBytes != 0 || *peakConcurrent != 0)))
        {
            std::cerr << "Decoded texture budget/accounting contract failed\n";
            passed = false;
        }
        return passed;
    }

    bool ValidateLifetime(const Options& options, const std::string& json)
    {
        bool passed = true;
        passed &= RequireContains(json,
                                  "\"schemaId\": \"RVX.SampleLifetimeQualification\"",
                                  "lifetime schema");
        passed &= RequireContains(json, "\"schemaVersion\": 1", "lifetime schema version");
        passed &= RequireContains(json, "\"sampleName\": \"model-viewer\"", "sample name");
        passed &= RequireContains(json,
                                  "\"backend\": \"" + options.backend + "\"",
                                  "lifetime backend");
        passed &= RequireContains(json,
                                  "\"renderPath\": \"" + options.renderPath + "\"",
                                  "lifetime render path");
        passed &= RequireContains(json, "\"deterministicOrbit\": true", "orbit mode");
        passed &= RequireContains(json, "\"failures\": []", "empty failure list");
        passed &= RequireContains(json, "\"complete\": true", "complete result");
        passed &= RequireContains(json, "\"pass\": true", "passing result");
        passed &= RequireUnsigned(json, "warmupFrames", 120);
        passed &= RequireUnsigned(json, "observationFrames", 480);
        passed &= RequireUnsigned(json, "warmupFramesObserved", 120);
        passed &= RequireUnsigned(json, "observationFramesObserved", 480);

        const std::optional<uint64_t> readyFrames =
            ReadUnsignedField(json, "readyFrames");
        const std::optional<uint64_t> resizeCount =
            ReadUnsignedField(json, "resizeCount");
        const std::optional<uint64_t> plateauCount =
            ReadUnsignedField(json, "plateauCount");
        if (!readyFrames || *readyFrames < 600)
        {
            std::cerr << "Lifetime report observed fewer than 600 ready frames\n";
            passed = false;
        }
        if (options.requireResize &&
            (!resizeCount || *resizeCount == 0 || !plateauCount || *plateauCount < 2))
        {
            std::cerr << "Lifetime report did not establish a post-resize plateau\n";
            passed = false;
        }

        passed &= RequireEveryUnsignedEquals(json, "nativeValidationErrorCount", 0);
        passed &= RequireEveryUnsignedEquals(json, "nativeValidationCorruptionCount", 0);
        passed &= RequireEveryUnsignedEquals(json, "leaseAbortCount", 0);
        passed &= RequireEveryUnsignedEquals(json, "surfaceIncompatibleFrameDrops", 0);
        return passed;
    }

    bool ValidateSample(const Options& options, const std::string& json)
    {
        bool passed = true;
        passed &= RequireContains(json,
                                  "\"schemaId\": \"RVX.SampleReport\"",
                                  "sample report schema");
        passed &= RequireContains(json, "\"schemaVersion\": 2", "sample schema version");
        passed &= RequireContains(json, "\"sampleName\": \"model-viewer\"", "sample name");
        passed &= RequireContains(json,
                                  "\"backend\": \"" + options.backend + "\"",
                                  "sample backend");
        passed &= RequireContains(json,
                                  "\"renderPath\": \"" + options.renderPath + "\"",
                                  "sample render path");
        passed &= RequireContains(json, "\"ready\": true", "ready result");
        passed &= RequireContains(json, "\"rendered\": true", "render result");
        passed &= RequireContains(json, "\"graphCompiled\": true", "compiled graph");
        passed &= RequireContains(json, "\"materialReady\": true", "material readiness");
        passed &= RequireContains(json,
                                  "\"materialUsedFallback\": false",
                                  "final material binding");
        passed &= RequireContains(json,
                                  "\"opaqueExecutionCompleted\": true",
                                  "opaque execution");
        passed &= RequireContains(json,
                                  "\"opaqueMaterialBindingsAvailable\": true",
                                  "opaque material bindings");
        passed &= RequireContains(json, "\"pass\": true", "passing sample report");
        passed &= RequireUnsigned(json,
                                  "visibleObjectCount",
                                  options.expectedVisibleObjectCount);
        passed &= RequireUnsigned(json, "opaqueMaterialBindingCount", 51);
        passed &= RequireUnsigned(json, "opaqueMaterialFallbackBindingCount", 0);
        passed &= RequireContains(json, "model meshes=75", "model mesh topology");
        passed &= RequireContains(json, "model materials=14", "model material topology");
        passed &= RequireContains(json, "model nodes=141", "model node topology");
        passed &= RequireContains(json,
                                  "QualificationCaptureReset",
                                  "deterministic capture reset");

        if (options.renderPath == "direct")
        {
            passed &= RequireUnsigned(json, "opaqueExecutedDrawCount", 51);
            passed &= RequireUnsigned(json, "gpuDrivenOpaqueDirectDrawCount", 51);
            passed &= RequireUnsigned(json, "gpuDrivenOpaqueIndirectBatchCount", 0);
            passed &= RequireContains(json,
                                      "\"gpuDrivenOpaqueIndirectSubmitted\": false",
                                      "direct submission policy");
            passed &= RequireContains(json,
                                      "\"renderPolicyExecutedTier\": \"Direct\"",
                                      "direct execution tier");
        }
        else
        {
            passed &= RequireUnsigned(json, "gpuDrivenCpuReferenceVisibleCount", 51);
            passed &= RequireUnsigned(json, "gpuDrivenOpaqueDirectDrawCount", 0);
            passed &= RequireUnsigned(json, "gpuDrivenOpaqueIndirectBatchCount", 51);
            passed &= RequireContains(json,
                                      "\"gpuDrivenExecutionRecorded\": true",
                                      "GPU-driven execution");
            passed &= RequireContains(json,
                                      "\"gpuDrivenOpaqueIndirectSubmitted\": true",
                                      "indirect submission");
            passed &= RequireContains(
                json,
                "\"renderPolicyExecutedTier\": \"GPUResidentScene\"",
                "GPU-driven execution tier");
        }
        return passed;
    }
} // namespace

int main(int argc, char* argv[])
{
    Options options;
    if (!ParseOptions(argc, argv, options))
        return 2;

    std::string startupJson;
    std::string lifetimeJson;
    std::string sampleJson;
    if (!ReadText(options.startupReport, startupJson) ||
        !ReadText(options.lifetimeReport, lifetimeJson) ||
        !ReadText(options.sampleReport, sampleJson))
    {
        return 1;
    }

    const bool startupPassed = ValidateStartup(options, startupJson);
    const bool lifetimePassed = ValidateLifetime(options, lifetimeJson);
    const bool samplePassed = ValidateSample(options, sampleJson);
    if (!startupPassed || !lifetimePassed || !samplePassed)
        return 1;

    std::cout << "Sample startup/residency/lifetime qualification passed for "
              << options.assetMode << ' ' << options.backend << ' '
              << options.renderPath << "\n";
    return 0;
}
