#include "Common/ImageFile.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <unordered_set>

namespace
{
    struct Options
    {
        std::filesystem::path image;
        RVX::uint32 minWidth = 1;
        RVX::uint32 minHeight = 1;
        RVX::uint32 minUniqueColors = 2;
        RVX::uint32 sampleStep = 1;
        double minLumaRange = 1.0;
        double nearBlackThreshold = 8.0;
        double maxNearBlackRatio = 0.98;
        bool showHelp = false;
    };

    void PrintUsage()
    {
        std::cout
            << "ImageContentValidation\n"
            << "  --image <path>                 PPM image to inspect\n"
            << "  --min-width <pixels>           Minimum expected width\n"
            << "  --min-height <pixels>          Minimum expected height\n"
            << "  --min-unique-colors <count>    Minimum sampled unique RGB colors\n"
            << "  --min-luma-range <0-255>       Minimum sampled luma range\n"
            << "  --near-black-threshold <0-255> Luma threshold for near-black pixels\n"
            << "  --max-near-black-ratio <0-1>   Maximum sampled near-black ratio\n"
            << "  --sample-step <pixels>         Sample every N pixels, default 1\n";
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
            else if (arg == "--image")
            {
                const char* value = requireValue("--image");
                if (!value) return false;
                options.image = value;
            }
            else if (arg == "--min-width")
            {
                const char* value = requireValue("--min-width");
                if (!value) return false;
                options.minWidth = static_cast<RVX::uint32>(std::stoul(value));
            }
            else if (arg == "--min-height")
            {
                const char* value = requireValue("--min-height");
                if (!value) return false;
                options.minHeight = static_cast<RVX::uint32>(std::stoul(value));
            }
            else if (arg == "--min-unique-colors")
            {
                const char* value = requireValue("--min-unique-colors");
                if (!value) return false;
                options.minUniqueColors = static_cast<RVX::uint32>(std::stoul(value));
            }
            else if (arg == "--min-luma-range")
            {
                const char* value = requireValue("--min-luma-range");
                if (!value) return false;
                options.minLumaRange = std::stod(value);
            }
            else if (arg == "--near-black-threshold")
            {
                const char* value = requireValue("--near-black-threshold");
                if (!value) return false;
                options.nearBlackThreshold = std::stod(value);
            }
            else if (arg == "--max-near-black-ratio")
            {
                const char* value = requireValue("--max-near-black-ratio");
                if (!value) return false;
                options.maxNearBlackRatio = std::stod(value);
            }
            else if (arg == "--sample-step")
            {
                const char* value = requireValue("--sample-step");
                if (!value) return false;
                options.sampleStep = std::max<RVX::uint32>(
                    1u,
                    static_cast<RVX::uint32>(std::stoul(value)));
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

        if (options.image.empty())
        {
            std::cerr << "--image is required\n";
            return false;
        }

        return true;
    }

    double Luma(RVX::uint8 r, RVX::uint8 g, RVX::uint8 b)
    {
        return 0.2126 * static_cast<double>(r) +
               0.7152 * static_cast<double>(g) +
               0.0722 * static_cast<double>(b);
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

    RVX::Test::ImageData image;
    std::string error;
    if (!RVX::Test::LoadPPM(options.image, image, &error))
    {
        std::cerr << error << "\n";
        return 1;
    }

    if (image.width < options.minWidth || image.height < options.minHeight)
    {
        std::cerr << "Image dimensions below required minimum: "
                  << image.width << "x" << image.height << "\n";
        return 1;
    }

    std::unordered_set<RVX::uint32> uniqueColors;
    RVX::uint64 sampledPixels = 0;
    RVX::uint64 nearBlackPixels = 0;
    double minLuma = std::numeric_limits<double>::max();
    double maxLuma = std::numeric_limits<double>::lowest();

    for (RVX::uint32 y = 0; y < image.height; y += options.sampleStep)
    {
        for (RVX::uint32 x = 0; x < image.width; x += options.sampleStep)
        {
            const RVX::uint64 offset =
                (static_cast<RVX::uint64>(y) * image.width + x) * image.bytesPerPixel;
            const RVX::uint8 r = image.pixels[static_cast<size_t>(offset + 0)];
            const RVX::uint8 g = image.pixels[static_cast<size_t>(offset + 1)];
            const RVX::uint8 b = image.pixels[static_cast<size_t>(offset + 2)];
            const double luma = Luma(r, g, b);

            minLuma = std::min(minLuma, luma);
            maxLuma = std::max(maxLuma, luma);
            if (luma <= options.nearBlackThreshold)
            {
                ++nearBlackPixels;
            }

            if (uniqueColors.size() < options.minUniqueColors)
            {
                const RVX::uint32 color =
                    (static_cast<RVX::uint32>(r) << 16u) |
                    (static_cast<RVX::uint32>(g) << 8u) |
                    static_cast<RVX::uint32>(b);
                uniqueColors.insert(color);
            }

            ++sampledPixels;
        }
    }

    if (sampledPixels == 0)
    {
        std::cerr << "No pixels sampled\n";
        return 1;
    }

    const double lumaRange = maxLuma - minLuma;
    const double nearBlackRatio =
        static_cast<double>(nearBlackPixels) / static_cast<double>(sampledPixels);

    std::cout << "ImageContentValidation\n"
              << "  image:            " << options.image << "\n"
              << "  dimensions:       " << image.width << "x" << image.height << "\n"
              << "  sampled pixels:   " << sampledPixels << "\n"
              << "  unique colors:    " << uniqueColors.size() << "\n"
              << "  luma range:       " << lumaRange << "\n"
              << "  near-black ratio: " << nearBlackRatio << "\n";

    if (uniqueColors.size() < options.minUniqueColors)
    {
        std::cerr << "Image has too few unique colors\n";
        return 1;
    }

    if (lumaRange < options.minLumaRange)
    {
        std::cerr << "Image luma range is too small\n";
        return 1;
    }

    if (nearBlackRatio > options.maxNearBlackRatio)
    {
        std::cerr << "Image is too close to black\n";
        return 1;
    }

    return 0;
}
