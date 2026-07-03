#pragma once

/**
 * @file SampleCLI.h
 * @brief Shared command-line and JSON report helpers for sample applications.
 */

#include "Core/Types.h"
#include "RHI/RHIDefinitions.h"

#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

namespace RVX
{
    struct SampleCLIOptions
    {
        RHIBackendType backend = RHIBackendType::Auto;
        bool smoke = false;
        uint32 frames = 0;
        std::filesystem::path screenshotPath;
        std::filesystem::path reportPath;
        uint32 width = 1280;
        uint32 height = 720;
        std::string quality = "default";
        bool diagnostics = false;
        bool showHelp = false;
    };

    struct SampleReport
    {
        std::string sampleName;
        RHIBackendType backend = RHIBackendType::Auto;
        uint32 frameCount = 0;
        uint32 width = 0;
        uint32 height = 0;
        std::string quality = "default";
        bool diagnostics = false;
        std::filesystem::path screenshotPath;
        std::vector<std::string> enabledFeatures;
        std::vector<std::string> unsupportedFeatures;
        std::vector<std::string> fallbackReasons;
        bool pass = false;
    };

    bool ParseSampleBackend(const std::string& text, RHIBackendType& outBackend);
    const char* GetSampleBackendName(RHIBackendType backend);

    bool ParseSampleCLI(int argc,
                        const char* const* argv,
                        SampleCLIOptions& options,
                        std::string* outError = nullptr);

    void PrintSampleCLIUsage(std::ostream& stream, const char* executableName);

    void WriteSampleReportJson(std::ostream& stream, const SampleReport& report);
    bool WriteSampleReportJson(const SampleReport& report,
                               const std::filesystem::path& path,
                               std::string* outError = nullptr);
} // namespace RVX
