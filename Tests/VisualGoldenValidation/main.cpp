#include "Common/ImageCompare.h"
#include "Common/ImageFile.h"

#include <filesystem>
#include <iostream>
#include <string>

namespace
{
    struct Options
    {
        std::filesystem::path expected;
        std::filesystem::path actual;
        std::filesystem::path diff;
        float tolerance = 0.0f;
        RVX::uint32 maxDifferentPixels = 0;
        bool showHelp = false;
    };

    void PrintUsage()
    {
        std::cout
            << "VisualGoldenValidation\n"
            << "  --expected <path>  Golden PPM image\n"
            << "  --actual <path>    Actual PPM image\n"
            << "  --diff <path>      Diff PPM artifact path\n"
            << "  --tolerance <0-1>  Per-channel tolerance, default 0\n"
            << "  --max-different-pixels <count>  Allowed different pixels, default 0\n";
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
              << "  mse:      " << result.mse << "\n"
              << "  psnr:     " << result.psnr << "\n"
              << "  pixels:   " << result.differentPixels << "\n";

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
