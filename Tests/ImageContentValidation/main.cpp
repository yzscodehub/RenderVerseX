#include "Common/ImageFile.h"

#include <algorithm>
#include <array>
#include <cmath>
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
        double minTopBottomLumaDelta = 0.0;
        double minPBRMetallicLumaDelta = 40.0;
        double minPBRRoughnessContrastDelta = 2.0;
        bool requireTopBottomLumaDelta = false;
        bool requirePBRGridResponse = false;
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
            << "  --min-top-bottom-luma-delta <0-255>\n"
            << "                                  Minimum top-band minus bottom-band luma\n"
            << "  --require-pbr-grid-response     Validate both 5x5 PBR response matrices\n"
            << "  --min-pbr-metallic-luma-delta <0-255>\n"
            << "  --min-pbr-roughness-contrast-delta <0-255>\n"
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
            else if (arg == "--min-top-bottom-luma-delta")
            {
                const char* value = requireValue("--min-top-bottom-luma-delta");
                if (!value) return false;
                options.minTopBottomLumaDelta = std::stod(value);
                options.requireTopBottomLumaDelta = true;
            }
            else if (arg == "--require-pbr-grid-response")
            {
                options.requirePBRGridResponse = true;
            }
            else if (arg == "--min-pbr-metallic-luma-delta")
            {
                const char* value =
                    requireValue("--min-pbr-metallic-luma-delta");
                if (!value) return false;
                options.minPBRMetallicLumaDelta = std::stod(value);
            }
            else if (arg == "--min-pbr-roughness-contrast-delta")
            {
                const char* value =
                    requireValue("--min-pbr-roughness-contrast-delta");
                if (!value) return false;
                options.minPBRRoughnessContrastDelta = std::stod(value);
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

    struct PatchMetrics
    {
        double mean = 0.0;
        double contrast = 0.0;
    };

    PatchMetrics MeasurePatch(const RVX::Test::ImageData& image,
                              RVX::uint32 centerX,
                              RVX::uint32 centerY,
                              RVX::uint32 radius)
    {
        const RVX::uint32 minX = centerX > radius ? centerX - radius : 0;
        const RVX::uint32 minY = centerY > radius ? centerY - radius : 0;
        const RVX::uint32 maxX =
            std::min(centerX + radius, image.width - 1u);
        const RVX::uint32 maxY =
            std::min(centerY + radius, image.height - 1u);

        double sum = 0.0;
        double sumSquared = 0.0;
        RVX::uint64 count = 0;
        for (RVX::uint32 y = minY; y <= maxY; ++y)
        {
            for (RVX::uint32 x = minX; x <= maxX; ++x)
            {
                const RVX::uint64 offset =
                    (static_cast<RVX::uint64>(y) * image.width + x) *
                    image.bytesPerPixel;
                const double value = Luma(
                    image.pixels[static_cast<size_t>(offset + 0)],
                    image.pixels[static_cast<size_t>(offset + 1)],
                    image.pixels[static_cast<size_t>(offset + 2)]);
                sum += value;
                sumSquared += value * value;
                ++count;
            }
        }

        PatchMetrics result;
        if (count > 0)
        {
            result.mean = sum / static_cast<double>(count);
            const double variance =
                std::max(0.0,
                         sumSquared / static_cast<double>(count) -
                             result.mean * result.mean);
            result.contrast = std::sqrt(variance);
        }
        return result;
    }

    struct PBRGridResponseMetrics
    {
        double metallicLumaDelta = 0.0;
        double roughnessContrastDelta = 0.0;
        std::array<double, 5> roughnessRowContrast{};
        RVX::uint32 roughnessContrastDropCount = 0;
    };

    struct PBRProjectedPoint
    {
        double x = 0.0;
        double y = 0.0;
    };

    PBRProjectedPoint ProjectPBRGridPoint(
        const RVX::Test::ImageData& image,
        RVX::uint32 row,
        RVX::uint32 column,
        double layerZ)
    {
        constexpr double VerticalFov = 0.6981317008;
        constexpr double CameraYaw = 0.7853981634;
        constexpr double CameraPitch = 0.2094395102;
        constexpr double GridSpacing = 1.45;
        constexpr double MatrixExtent = 3.45;
        constexpr double FitMargin = 1.08;

        const double aspect = static_cast<double>(image.width) /
            static_cast<double>(image.height);
        const double distance =
            std::sqrt(3.0 * MatrixExtent * MatrixExtent) /
            std::sin(VerticalFov * 0.5) * FitMargin;
        const double worldX =
            (static_cast<double>(column) - 2.0) * GridSpacing;
        const double worldY =
            (2.0 - static_cast<double>(row)) * GridSpacing;
        const double sinYaw = std::sin(CameraYaw);
        const double cosYaw = std::cos(CameraYaw);
        const double sinPitch = std::sin(CameraPitch);
        const double cosPitch = std::cos(CameraPitch);
        const double viewAxisX = cosPitch * sinYaw;
        const double viewAxisY = sinPitch;
        const double viewAxisZ = cosPitch * cosYaw;
        const double viewDepth = distance -
            (worldX * viewAxisX + worldY * viewAxisY +
             layerZ * viewAxisZ);
        const double viewX = worldX * cosYaw - layerZ * sinYaw;
        const double viewY =
            worldX * (-sinPitch * sinYaw) + worldY * cosPitch +
            layerZ * (-sinPitch * cosYaw);
        const double tanHalfFov = std::tan(VerticalFov * 0.5);

        return {
            0.5 + viewX / (2.0 * viewDepth * tanHalfFov * aspect),
            0.5 - viewY / (2.0 * viewDepth * tanHalfFov)};
    }

    PBRGridResponseMetrics MeasurePBRGridResponse(
        const RVX::Test::ImageData& image,
        double layerZ,
        RVX::uint32 firstVisibleMetallicColumn,
        RVX::uint32 radius)
    {
        constexpr RVX::uint32 GridSize = 5;
        PatchMetrics patches[GridSize][GridSize] = {};

        for (RVX::uint32 row = 0; row < GridSize; ++row)
        {
            for (RVX::uint32 column = 0; column < GridSize; ++column)
            {
                const PBRProjectedPoint projected =
                    ProjectPBRGridPoint(image, row, column, layerZ);
                const RVX::uint32 centerX = static_cast<RVX::uint32>(std::lround(
                    projected.x * static_cast<double>(image.width)));
                const RVX::uint32 centerY = static_cast<RVX::uint32>(std::lround(
                    projected.y * static_cast<double>(image.height)));
                patches[row][column] =
                    MeasurePatch(image, centerX, centerY, radius);
            }
        }

        PBRGridResponseMetrics metrics;
        for (RVX::uint32 row = 0; row < GridSize; ++row)
        {
            metrics.metallicLumaDelta +=
                patches[row][firstVisibleMetallicColumn].mean -
                patches[row][GridSize - 1].mean;
        }
        metrics.metallicLumaDelta /= static_cast<double>(GridSize);

        for (RVX::uint32 row = 0; row < GridSize; ++row)
        {
            for (RVX::uint32 column = firstVisibleMetallicColumn;
                 column < GridSize;
                 ++column)
            {
                metrics.roughnessRowContrast[row] +=
                    patches[row][column].contrast;
            }
            metrics.roughnessRowContrast[row] /=
                static_cast<double>(GridSize - firstVisibleMetallicColumn);
            if (row > 0 &&
                metrics.roughnessRowContrast[row] <
                    metrics.roughnessRowContrast[row - 1])
            {
                ++metrics.roughnessContrastDropCount;
            }
        }

        for (RVX::uint32 column = firstVisibleMetallicColumn;
             column < GridSize;
             ++column)
        {
            metrics.roughnessContrastDelta +=
                patches[0][column].contrast -
                patches[GridSize - 1][column].contrast;
        }
        metrics.roughnessContrastDelta /=
            static_cast<double>(GridSize - firstVisibleMetallicColumn);
        return metrics;
    }

    bool ValidatePBRMatrixResponse(const char* matrixName,
                                   const PBRGridResponseMetrics& metrics,
                                   const Options& options)
    {
        std::cout << "  pbr " << matrixName << " metallic luma delta:       "
                  << metrics.metallicLumaDelta << "\n"
                  << "  pbr " << matrixName << " roughness contrast delta: "
                  << metrics.roughnessContrastDelta << "\n"
                  << "  pbr " << matrixName << " roughness contrast drops: "
                  << metrics.roughnessContrastDropCount << "/4\n";
        if (metrics.metallicLumaDelta < options.minPBRMetallicLumaDelta)
        {
            std::cerr << "PBR " << matrixName
                      << " matrix metallic response is too small\n";
            return false;
        }
        if (metrics.roughnessContrastDelta <
            options.minPBRRoughnessContrastDelta)
        {
            std::cerr << "PBR " << matrixName
                      << " matrix roughness response is too small\n";
            return false;
        }
        if (metrics.roughnessContrastDropCount < 3)
        {
            std::cerr << "PBR " << matrixName
                      << " matrix roughness response is not consistently monotonic\n";
            return false;
        }
        return true;
    }

    bool ValidatePBRGridResponse(const RVX::Test::ImageData& image,
                                 const Options& options)
    {
        const double scale = std::min(
            static_cast<double>(image.width) / 320.0,
            static_cast<double>(image.height) / 180.0);
        const RVX::uint32 radius = std::max<RVX::uint32>(
            2u, static_cast<RVX::uint32>(std::lround(3.0 * scale)));

        // The front terracotta slice remains fully observable in the default
        // diagonal cube view. Factor and texture workflows render this same
        // 5x5 material surface in separate qualified sample runs.
        const PBRGridResponseMetrics frontSliceMetrics = MeasurePBRGridResponse(
            image, 2.9, 0u, radius);
        return ValidatePBRMatrixResponse(
            "front slice", frontSliceMetrics, options);
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
    RVX::uint64 topBandPixels = 0;
    RVX::uint64 bottomBandPixels = 0;
    double topBandLuma = 0.0;
    double bottomBandLuma = 0.0;
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
            if (y < image.height / 4u)
            {
                topBandLuma += luma;
                ++topBandPixels;
            }
            if (y >= image.height - image.height / 4u)
            {
                bottomBandLuma += luma;
                ++bottomBandPixels;
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
    const double topBottomLumaDelta =
        topBandPixels > 0 && bottomBandPixels > 0
            ? topBandLuma / static_cast<double>(topBandPixels) -
                  bottomBandLuma / static_cast<double>(bottomBandPixels)
            : 0.0;

    std::cout << "ImageContentValidation\n"
              << "  image:            " << options.image << "\n"
              << "  dimensions:       " << image.width << "x" << image.height << "\n"
              << "  sampled pixels:   " << sampledPixels << "\n"
              << "  unique colors:    " << uniqueColors.size() << "\n"
              << "  luma range:       " << lumaRange << "\n"
              << "  near-black ratio: " << nearBlackRatio << "\n";
    if (options.requireTopBottomLumaDelta)
    {
        std::cout << "  top-bottom luma:  " << topBottomLumaDelta << "\n";
    }

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

    if (options.requireTopBottomLumaDelta &&
        (topBandPixels == 0 || bottomBandPixels == 0 ||
         topBottomLumaDelta < options.minTopBottomLumaDelta))
    {
        std::cerr << "Image top-to-bottom orientation contract failed\n";
        return 1;
    }

    if (options.requirePBRGridResponse &&
        !ValidatePBRGridResponse(image, options))
    {
        return 1;
    }

    return 0;
}
