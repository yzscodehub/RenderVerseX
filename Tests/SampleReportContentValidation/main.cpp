#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    struct Options
    {
        std::filesystem::path reportPath;
        std::string sampleName;
        std::string backend;
        std::string quality;
        uint32_t frameCount = 0;
        std::vector<std::string> requiredEnabledFeatures;
        std::vector<std::string> requiredResourceDiagnostics;
    };

    bool ReadTextFile(const std::filesystem::path& path, std::string& outText)
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

    bool ParseUInt(const std::string& text, uint32_t& outValue)
    {
        if (text.empty())
        {
            return false;
        }

        uint32_t value = 0;
        for (char ch : text)
        {
            if (ch < '0' || ch > '9')
            {
                return false;
            }
            value = value * 10u + static_cast<uint32_t>(ch - '0');
        }
        outValue = value;
        return true;
    }

    bool ParseOptions(int argc, char* argv[], Options& options)
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i] ? argv[i] : "";
            const auto requireValue = [&](const char* name) -> const char*
            {
                if (i + 1 >= argc)
                {
                    std::cerr << "Missing value for " << name << "\n";
                    return nullptr;
                }
                return argv[++i];
            };

            if (arg == "--report")
            {
                const char* value = requireValue("--report");
                if (!value) return false;
                options.reportPath = value;
            }
            else if (arg == "--sample-name")
            {
                const char* value = requireValue("--sample-name");
                if (!value) return false;
                options.sampleName = value;
            }
            else if (arg == "--backend")
            {
                const char* value = requireValue("--backend");
                if (!value) return false;
                options.backend = value;
            }
            else if (arg == "--quality")
            {
                const char* value = requireValue("--quality");
                if (!value) return false;
                options.quality = value;
            }
            else if (arg == "--frame-count")
            {
                const char* value = requireValue("--frame-count");
                if (!value) return false;
                if (!ParseUInt(value, options.frameCount))
                {
                    std::cerr << "Invalid --frame-count value: " << value << "\n";
                    return false;
                }
            }
            else if (arg == "--require-enabled-feature")
            {
                const char* value = requireValue("--require-enabled-feature");
                if (!value) return false;
                options.requiredEnabledFeatures.emplace_back(value);
            }
            else if (arg == "--require-resource-diagnostic")
            {
                const char* value = requireValue("--require-resource-diagnostic");
                if (!value) return false;
                options.requiredResourceDiagnostics.emplace_back(value);
            }
            else
            {
                std::cerr << "Unknown argument: " << arg << "\n";
                return false;
            }
        }

        if (options.reportPath.empty())
        {
            std::cerr << "--report is required\n";
            return false;
        }
        return true;
    }

    bool RequireContains(const std::string& json, const std::string& needle, const char* label)
    {
        if (json.find(needle) == std::string::npos)
        {
            std::cerr << "Report missing " << label << ": " << needle << "\n";
            return false;
        }
        return true;
    }
} // namespace

int main(int argc, char* argv[])
{
    Options options;
    if (!ParseOptions(argc, argv, options))
    {
        return 2;
    }

    std::string json;
    if (!ReadTextFile(options.reportPath, json))
    {
        return 1;
    }

    bool passed = true;
    if (!options.sampleName.empty())
    {
        passed &= RequireContains(json, "\"sampleName\": \"" + options.sampleName + "\"", "sampleName");
    }
    if (!options.backend.empty())
    {
        passed &= RequireContains(json, "\"backend\": \"" + options.backend + "\"", "backend");
    }
    if (!options.quality.empty())
    {
        passed &= RequireContains(json, "\"quality\": \"" + options.quality + "\"", "quality");
    }
    if (options.frameCount > 0)
    {
        passed &= RequireContains(json, "\"frameCount\": " + std::to_string(options.frameCount), "frameCount");
    }

    passed &= RequireContains(json, "\"enabledFeatures\": [", "enabledFeatures array");
    passed &= RequireContains(json, "\"unsupportedFeatures\": [", "unsupportedFeatures array");
    passed &= RequireContains(json, "\"fallbackReasons\": [", "fallbackReasons array");
    passed &= RequireContains(json, "\"resourceDiagnostics\": [", "resourceDiagnostics array");
    passed &= RequireContains(json, "\"renderDiagnostics\": {", "renderDiagnostics object");
    passed &= RequireContains(json, "\"available\": true", "available render diagnostics");
    passed &= RequireContains(json, "\"graphCompiled\": true", "compiled render graph diagnostics");
    passed &= RequireContains(json, "\"pass\": true", "pass status");

    for (const std::string& feature : options.requiredEnabledFeatures)
    {
        passed &= RequireContains(json, "\"" + feature + "\"", "required enabled feature");
    }

    for (const std::string& diagnostic : options.requiredResourceDiagnostics)
    {
        passed &= RequireContains(json, diagnostic, "required resource diagnostic");
    }

    if (!passed)
    {
        return 1;
    }

    std::cout << "Sample report content validation passed: " << options.reportPath << "\n";
    return 0;
}
