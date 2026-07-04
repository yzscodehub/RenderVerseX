/**
 * @file SampleCLI.cpp
 * @brief Shared command-line and JSON report helpers for sample applications.
 */

#include "Samples/SampleCLI.h"

#include "Core/Diagnostics/JsonWriter.h"
#include "RHI/RHI.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <iostream>
#include <ostream>
#include <string>
#include <system_error>
#include <utility>

namespace RVX
{
    namespace
    {
        std::string ToLower(std::string text)
        {
            std::transform(text.begin(),
                           text.end(),
                           text.begin(),
                           [](unsigned char ch)
                           {
                               return static_cast<char>(std::tolower(ch));
                           });
            return text;
        }

        bool ParsePositiveUInt(const char* text, uint32& outValue)
        {
            if (!text || text[0] == '\0')
            {
                return false;
            }

            uint32 value = 0;
            const char* end = text + std::char_traits<char>::length(text);
            const std::from_chars_result result = std::from_chars(text, end, value);
            if (result.ec != std::errc{} || result.ptr != end)
            {
                return false;
            }

            outValue = value;
            return true;
        }

        void SetError(std::string* outError, const std::string& error)
        {
            if (outError)
            {
                *outError = error;
            }
        }

        void WriteStringArray(std::ostream& stream,
                              const char* name,
                              const std::vector<std::string>& values,
                              const char* suffix)
        {
            stream << "  \"" << name << "\": [";
            for (size_t i = 0; i < values.size(); ++i)
            {
                if (i > 0)
                {
                    stream << ", ";
                }
                stream << Diagnostics::JsonString(values[i]);
            }
            stream << "]" << suffix << "\n";
        }
    } // namespace

    SampleFeatureReporter::SampleFeatureReporter(SampleReport& report)
        : m_report(&report)
    {
    }

    void SampleFeatureReporter::Enable(std::string feature)
    {
        if (m_report)
        {
            m_report->enabledFeatures.push_back(std::move(feature));
        }
    }

    void SampleFeatureReporter::Unsupported(std::string feature)
    {
        if (m_report)
        {
            m_report->unsupportedFeatures.push_back(std::move(feature));
        }
    }

    void SampleFeatureReporter::Fallback(std::string reason)
    {
        if (m_report)
        {
            m_report->fallbackReasons.push_back(std::move(reason));
        }
    }

    void SampleFeatureReporter::ResourceDiagnostic(std::string diagnostic)
    {
        if (m_report)
        {
            m_report->resourceDiagnostics.push_back(std::move(diagnostic));
        }
    }

    bool ParseSampleBackend(const std::string& text, RHIBackendType& outBackend)
    {
        const std::string value = ToLower(text);
        if (value == "auto")
        {
            outBackend = RHIBackendType::Auto;
            return true;
        }
        if (value == "dx11" || value == "d3d11" || value == "directx11")
        {
            outBackend = RHIBackendType::DX11;
            return true;
        }
        if (value == "dx12" || value == "d3d12" || value == "directx12")
        {
            outBackend = RHIBackendType::DX12;
            return true;
        }
        if (value == "vulkan" || value == "vk")
        {
            outBackend = RHIBackendType::Vulkan;
            return true;
        }
        if (value == "metal" || value == "mtl")
        {
            outBackend = RHIBackendType::Metal;
            return true;
        }
        if (value == "opengl" || value == "gl")
        {
            outBackend = RHIBackendType::OpenGL;
            return true;
        }

        return false;
    }

    const char* GetSampleBackendName(RHIBackendType backend)
    {
        switch (backend)
        {
            case RHIBackendType::Auto:
                return "auto";
            case RHIBackendType::DX11:
                return "dx11";
            case RHIBackendType::DX12:
                return "dx12";
            case RHIBackendType::Vulkan:
                return "vulkan";
            case RHIBackendType::Metal:
                return "metal";
            case RHIBackendType::OpenGL:
                return "opengl";
            default:
                return "unknown";
        }
    }

    bool ParseSampleCLI(int argc,
                        const char* const* argv,
                        SampleCLIOptions& options,
                        std::string* outError)
    {
        bool framesSpecified = false;

        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i] ? argv[i] : "";
            const auto requireValue = [&](const char* name) -> const char*
            {
                if (i + 1 >= argc)
                {
                    SetError(outError, std::string("Missing value for ") + name);
                    return nullptr;
                }
                return argv[++i];
            };

            if (arg == "--help" || arg == "-h")
            {
                options.showHelp = true;
            }
            else if (arg == "--backend")
            {
                const char* value = requireValue("--backend");
                if (!value)
                {
                    return false;
                }
                if (!ParseSampleBackend(value, options.backend))
                {
                    SetError(outError, std::string("Invalid --backend value: ") + value);
                    return false;
                }
            }
            else if (arg == "--dx11" || arg == "-d11")
            {
                options.backend = RHIBackendType::DX11;
            }
            else if (arg == "--dx12" || arg == "-d12")
            {
                options.backend = RHIBackendType::DX12;
            }
            else if (arg == "--vulkan" || arg == "-vk")
            {
                options.backend = RHIBackendType::Vulkan;
            }
            else if (arg == "--metal" || arg == "-mtl")
            {
                options.backend = RHIBackendType::Metal;
            }
            else if (arg == "--opengl" || arg == "-gl")
            {
                options.backend = RHIBackendType::OpenGL;
            }
            else if (arg == "--smoke")
            {
                options.smoke = true;
            }
            else if (arg == "--frames")
            {
                const char* value = requireValue("--frames");
                if (!value)
                {
                    return false;
                }
                if (!ParsePositiveUInt(value, options.frames))
                {
                    SetError(outError, std::string("Invalid --frames value: ") + value);
                    return false;
                }
                framesSpecified = true;
            }
            else if (arg == "--screenshot")
            {
                const char* value = requireValue("--screenshot");
                if (!value)
                {
                    return false;
                }
                options.screenshotPath = value;
            }
            else if (arg == "--report")
            {
                const char* value = requireValue("--report");
                if (!value)
                {
                    return false;
                }
                options.reportPath = value;
            }
            else if (arg == "--width")
            {
                const char* value = requireValue("--width");
                if (!value)
                {
                    return false;
                }
                if (!ParsePositiveUInt(value, options.width) || options.width == 0)
                {
                    SetError(outError, std::string("Invalid --width value: ") + value);
                    return false;
                }
            }
            else if (arg == "--height")
            {
                const char* value = requireValue("--height");
                if (!value)
                {
                    return false;
                }
                if (!ParsePositiveUInt(value, options.height) || options.height == 0)
                {
                    SetError(outError, std::string("Invalid --height value: ") + value);
                    return false;
                }
            }
            else if (arg == "--quality")
            {
                const char* value = requireValue("--quality");
                if (!value)
                {
                    return false;
                }
                options.quality = value;
            }
            else if (arg == "--diagnostics")
            {
                options.diagnostics = true;
            }
            else
            {
                SetError(outError, std::string("Unknown argument: ") + arg);
                return false;
            }
        }

        if (options.smoke && !framesSpecified && options.frames == 0)
        {
            options.frames = 8;
        }

        return true;
    }

    void PrintSampleCLIUsage(std::ostream& stream, const char* executableName)
    {
        stream
            << executableName << "\n"
            << "  --backend <auto|dx11|dx12|vulkan|metal|opengl>\n"
            << "  --smoke\n"
            << "  --frames <count>\n"
            << "  --screenshot <path.ppm>\n"
            << "  --report <path.json>\n"
            << "  --width <pixels>\n"
            << "  --height <pixels>\n"
            << "  --quality <name>\n"
            << "  --diagnostics\n";
    }

    void WriteSampleReportJson(std::ostream& stream, const SampleReport& report)
    {
        stream << "{\n";
        stream << "  \"sampleName\": " << Diagnostics::JsonString(report.sampleName) << ",\n";
        stream << "  \"category\": " << Diagnostics::JsonString(report.category) << ",\n";
        stream << "  \"backend\": " << Diagnostics::JsonString(GetSampleBackendName(report.backend)) << ",\n";
        stream << "  \"frameCount\": " << report.frameCount << ",\n";
        stream << "  \"width\": " << report.width << ",\n";
        stream << "  \"height\": " << report.height << ",\n";
        stream << "  \"quality\": " << Diagnostics::JsonString(report.quality) << ",\n";
        stream << "  \"diagnostics\": " << Diagnostics::JsonBool(report.diagnostics) << ",\n";
        stream << "  \"screenshotPath\": " << Diagnostics::JsonString(report.screenshotPath.string()) << ",\n";
        WriteStringArray(stream, "enabledFeatures", report.enabledFeatures, ",");
        WriteStringArray(stream, "unsupportedFeatures", report.unsupportedFeatures, ",");
        WriteStringArray(stream, "fallbackReasons", report.fallbackReasons, ",");
        WriteStringArray(stream, "resourceDiagnostics", report.resourceDiagnostics, ",");
        const SampleRenderDiagnostics& renderDiagnostics = report.renderDiagnostics;
        stream << "  \"renderDiagnostics\": {\n";
        stream << "    \"available\": " << Diagnostics::JsonBool(renderDiagnostics.available) << ",\n";
        stream << "    \"renderAttempted\": " << Diagnostics::JsonBool(renderDiagnostics.renderAttempted) << ",\n";
        stream << "    \"rendered\": " << Diagnostics::JsonBool(renderDiagnostics.rendered) << ",\n";
        stream << "    \"graphBuilt\": " << Diagnostics::JsonBool(renderDiagnostics.graphBuilt) << ",\n";
        stream << "    \"graphCompiled\": " << Diagnostics::JsonBool(renderDiagnostics.graphCompiled) << ",\n";
        stream << "    \"renderGraphTotalPasses\": " << renderDiagnostics.renderGraphTotalPasses << ",\n";
        stream << "    \"visibleObjectCount\": " << renderDiagnostics.visibleObjectCount << ",\n";
        stream << "    \"renderSceneLightCount\": " << renderDiagnostics.renderSceneLightCount << ",\n";
        stream << "    \"requestedPostProcessEffectCount\": "
               << renderDiagnostics.requestedPostProcessEffectCount << ",\n";
        stream << "    \"enabledPostProcessEffectCount\": "
               << renderDiagnostics.enabledPostProcessEffectCount << ",\n";
        stream << "    \"unsupportedPostProcessSkippedCount\": "
               << renderDiagnostics.unsupportedPostProcessSkippedCount << ",\n";
        stream << "    \"postProcessGraphPassCount\": " << renderDiagnostics.postProcessGraphPassCount << ",\n";
        stream << "    \"clusteredLightingInitialized\": "
               << Diagnostics::JsonBool(renderDiagnostics.clusteredLightingInitialized) << ",\n";
        stream << "    \"clusteredLightingActiveClusters\": "
               << renderDiagnostics.clusteredLightingActiveClusters << ",\n";
        stream << "    \"textureIBLEnabled\": " << Diagnostics::JsonBool(renderDiagnostics.textureIBLEnabled) << "\n";
        stream << "  },\n";
        stream << "  \"pass\": " << Diagnostics::JsonBool(report.pass) << "\n";
        stream << "}\n";
    }

    bool WriteSampleReportJson(const SampleReport& report,
                               const std::filesystem::path& path,
                               std::string* outError)
    {
        std::error_code error;
        const std::filesystem::path parent = path.parent_path();
        if (!parent.empty())
        {
            std::filesystem::create_directories(parent, error);
            if (error)
            {
                SetError(outError, "Failed to create report directory: " + error.message());
                return false;
            }
        }

        std::ofstream stream(path, std::ios::out | std::ios::trunc);
        if (!stream)
        {
            SetError(outError, "Failed to open sample report: " + path.string());
            return false;
        }

        WriteSampleReportJson(stream, report);
        return true;
    }

    SampleReport BuildSampleReport(const SampleAppDesc& desc, const SampleRunContext& context)
    {
        SampleReport report;
        report.sampleName = desc.sampleName;
        report.category = desc.category;
        report.backend = context.resolvedBackend;
        report.frameCount = context.frameCount;
        report.width = context.options.width;
        report.height = context.options.height;
        report.quality = context.options.quality;
        report.diagnostics = context.options.diagnostics;
        report.screenshotPath = context.options.screenshotPath;
        report.enabledFeatures = desc.enabledFeatures;
        report.unsupportedFeatures = desc.unsupportedFeatures;
        report.fallbackReasons = desc.fallbackReasons;
        report.resourceDiagnostics = desc.resourceDiagnostics;
        report.renderDiagnostics = desc.renderDiagnostics;
        report.pass = true;

        SampleFeatureReporter reporter(report);
        if (!context.options.screenshotPath.empty() && !desc.supportsScreenshot)
        {
            reporter.Unsupported("ScreenshotCapture");
            reporter.Fallback(desc.sampleName + " is a report-driven sample and does not capture screenshots yet");
        }
        if (context.options.quality != "default" && !desc.supportsQualityProfiles)
        {
            reporter.Unsupported("QualityProfile");
            reporter.Fallback(desc.sampleName + " reports fixed capability diagnostics for all quality presets");
        }

        return report;
    }

    int RunReportOnlySample(int argc, char* argv[], const SampleAppDesc& desc)
    {
        SampleCLIOptions options;
        std::string parseError;
        if (!ParseSampleCLI(argc, argv, options, &parseError))
        {
            if (!parseError.empty())
            {
                std::cerr << parseError << "\n";
            }
            PrintSampleCLIUsage(std::cerr, desc.sampleName.c_str());
            return 2;
        }

        if (options.showHelp)
        {
            PrintSampleCLIUsage(std::cout, desc.sampleName.c_str());
            return 0;
        }

        SampleRunContext context;
        context.options = options;
        context.resolvedBackend =
            options.backend == RHIBackendType::Auto ? SelectBestBackend() : options.backend;
        context.frameCount = options.frames > 0 ? options.frames : 1;

        SampleReport report = BuildSampleReport(desc, context);
        if (!options.reportPath.empty())
        {
            std::string reportError;
            if (!WriteSampleReportJson(report, options.reportPath, &reportError))
            {
                std::cerr << reportError << "\n";
                return 1;
            }
        }
        else if (options.diagnostics)
        {
            WriteSampleReportJson(std::cout, report);
        }

        return report.pass ? 0 : 1;
    }
} // namespace RVX