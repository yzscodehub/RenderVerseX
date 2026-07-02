#include "Common/ImageCompare.h"
#include "Common/ImageFile.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace
{
    struct Options
    {
        std::filesystem::path expected;
        std::filesystem::path actual;
        std::filesystem::path diff;
        std::filesystem::path report;
        float tolerance = 0.0f;
        RVX::uint32 maxDifferentPixels = 0;
        RVX::uint32 minDifferentPixels = 0;
        bool showHelp = false;
    };

    void PrintUsage()
    {
        std::cout
            << "VisualGoldenValidation\n"
            << "  --expected <path>  Golden PPM image\n"
            << "  --actual <path>    Actual PPM image\n"
            << "  --diff <path>      Diff PPM artifact path\n"
            << "  --report <path>    JSON report artifact path\n"
            << "  --tolerance <0-1>  Per-channel tolerance, default 0\n"
            << "  --max-different-pixels <count>  Allowed different pixels, default 0\n"
            << "  --min-different-pixels <count>  Required different pixels, default 0\n";
    }

    void WriteJsonString(std::ostream& stream, const std::filesystem::path& path)
    {
        const std::string value = path.string();
        stream << '"';
        for (char c : value)
        {
            if (c == '\\' || c == '"')
            {
                stream << '\\';
            }
            stream << c;
        }
        stream << '"';
    }

    bool WriteReport(const Options& options,
                     const RVX::Test::ImageData& expected,
                     const RVX::Test::ImageData& actual,
                     const RVX::Test::ImageCompareResult& result,
                     bool passed,
                     std::string& error)
    {
        if (options.report.empty())
        {
            return true;
        }

        const std::filesystem::path parent = options.report.parent_path();
        if (!parent.empty())
        {
            std::filesystem::create_directories(parent);
        }

        std::ofstream stream(options.report, std::ios::trunc);
        if (!stream.is_open())
        {
            error = "Failed to open visual golden report: " + options.report.string();
            return false;
        }

        stream << "{\n";
        stream << "  \"expected\": ";
        WriteJsonString(stream, options.expected);
        stream << ",\n  \"actual\": ";
        WriteJsonString(stream, options.actual);
        stream << ",\n  \"diff\": ";
        WriteJsonString(stream, options.diff);
        stream << ",\n  \"passed\": " << (passed ? "true" : "false") << ",\n";
        stream << "  \"expectedWidth\": " << expected.width << ",\n";
        stream << "  \"expectedHeight\": " << expected.height << ",\n";
        stream << "  \"actualWidth\": " << actual.width << ",\n";
        stream << "  \"actualHeight\": " << actual.height << ",\n";
        stream << "  \"tolerance\": " << options.tolerance << ",\n";
        stream << "  \"maxDifferentPixels\": " << options.maxDifferentPixels << ",\n";
        stream << "  \"minDifferentPixels\": " << options.minDifferentPixels << ",\n";
        stream << "  \"differentPixels\": " << result.differentPixels << ",\n";
        stream << "  \"mse\": " << result.mse << ",\n";
        stream << "  \"psnr\": " << result.psnr << "\n";
        stream << "}\n";
        return true;
    }

    bool ParseOptions(int argc, char** argv, Options& options)
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i];
            const auto requireValue = [&](const char* name) -> const char*
            {
                if (i + 1 >= argc)
                {
                    std::cerr << "Missing value for " << name << "\n";
                    return nullptr;
                }
                return argv[++i];
            };

            if (arg == "--help" || arg == "-h")
            {
                options.showHelp = true;
            }
            else if (arg == "--expected")
            {
                const char* value = requireValue("--expected");
                if (!value) return false;
                options.expected = value;
            }
            else if (arg == "--actual")
            {
                const char* value = requireValue("--actual");
                if (!value) return false;
                options.actual = value;
            }
            else if (arg == "--diff")
            {
                const char* value = requireValue("--diff");
                if (!value) return false;
                options.diff = value;
            }
            else if (arg == "--report")
            {
                const char* value = requireValue("--report");
                if (!value) return false;
                options.report = value;
            }
            else if (arg == "--tolerance")
            {
                const char* value = requireValue("--tolerance");
                if (!value) return false;
                options.tolerance = std::stof(value);
            }
            else if (arg == "--max-different-pixels")
            {
                const char* value = requireValue("--max-different-pixels");
                if (!value) return false;
                options.maxDifferentPixels = static_cast<RVX::uint32>(std::stoul(value));
            }
            else if (arg == "--min-different-pixels")
            {
                const char* value = requireValue("--min-different-pixels");
                if (!value) return false;
                options.minDifferentPixels = static_cast<RVX::uint32>(std::stoul(value));
            }
            else
            {
                std::cerr << "Unknown argument: " << arg << "\n";
                return false;
            }
        }

        if (options.showHelp)
        {
            return true;
        }

        if (options.expected.empty() || options.actual.empty())
        {
            std::cerr << "--expected and --actual are required\n";
            return false;
        }

        if (options.diff.empty())
        {
            options.diff = options.actual;
            options.diff.replace_extension(".diff.ppm");
        }

        return true;
    }
} // namespace

int main(int argc, char** argv)
{
    Options options;
    if (!ParseOptions(argc, argv, options))
    {
        PrintUsage();
        return 2;
    }

    if (options.showHelp)
    {
        PrintUsage();
        return 0;
    }

    RVX::Test::ImageData expected;
    RVX::Test::ImageData actual;
    std::string error;

    if (!RVX::Test::LoadPPM(options.expected, expected, &error))
    {
        std::cerr << error << "\n";
        return 1;
    }

    if (!RVX::Test::LoadPPM(options.actual, actual, &error))
    {
        std::cerr << error << "\n";
        return 1;
    }

    const RVX::Test::ImageCompareResult result =
        RVX::Test::CompareImages(expected.pixels.data(), expected.width, expected.height,
                                 actual.pixels.data(), actual.width, actual.height,
                                 3, options.tolerance);

    std::cout << "VisualGoldenValidation\n"
              << "  expected: " << options.expected << "\n"
              << "  actual:   " << options.actual << "\n"
              << "  diff:     " << options.diff << "\n"
              << "  report:   " << (options.report.empty() ? "<none>" : options.report.string()) << "\n"
              << "  mse:      " << result.mse << "\n"
              << "  psnr:     " << result.psnr << "\n"
              << "  pixels:   " << result.differentPixels << "\n";

    const bool meetsMinimumDifference = result.differentPixels >= options.minDifferentPixels;
    const bool withinMaximumDifference = result.identical || result.differentPixels <= options.maxDifferentPixels;
    const bool passed = meetsMinimumDifference && withinMaximumDifference;
    if (!WriteReport(options, expected, actual, result, passed, error))
    {
        std::cerr << error << "\n";
        return 1;
    }

    if (result.differentPixels < options.minDifferentPixels)
    {
        std::cerr << "Visual difference below required minimum\n";
        return 1;
    }

    if (!result.identical && result.differentPixels > options.maxDifferentPixels)
    {
        if (!RVX::Test::SaveDiffPPM(options.diff, expected, actual, &error))
        {
            std::cerr << error << "\n";
        }
        std::cerr << "Visual golden mismatch\n";
        return 1;
    }

    return 0;
}
