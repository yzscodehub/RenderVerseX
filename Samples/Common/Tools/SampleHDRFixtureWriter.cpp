/**
 * @file SampleHDRFixtureWriter.cpp
 * @brief Writes a tiny deterministic HDR environment for hermetic sample smoke.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace
{
    void WriteRGBE(std::ofstream& out, float r, float g, float b)
    {
        const float maxComponent = std::max({r, g, b});
        std::uint8_t rgbe[4] = {};
        if (maxComponent > 0.0f)
        {
            int exponent = 0;
            const float mantissa = std::frexp(maxComponent, &exponent);
            const float scale = mantissa * 256.0f / maxComponent;
            rgbe[0] = static_cast<std::uint8_t>(
                std::clamp(r * scale, 0.0f, 255.0f));
            rgbe[1] = static_cast<std::uint8_t>(
                std::clamp(g * scale, 0.0f, 255.0f));
            rgbe[2] = static_cast<std::uint8_t>(
                std::clamp(b * scale, 0.0f, 255.0f));
            rgbe[3] = static_cast<std::uint8_t>(exponent + 128);
        }
        out.write(reinterpret_cast<const char*>(rgbe), sizeof(rgbe));
    }

    bool WriteHDRFixture(const std::filesystem::path& outputPath)
    {
        std::error_code error;
        std::filesystem::create_directories(outputPath.parent_path(), error);
        if (error)
        {
            return false;
        }

        std::ofstream out(outputPath, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            return false;
        }

        constexpr int Width = 32;
        constexpr int Height = 16;
        out << "#?RADIANCE\n";
        out << "FORMAT=32-bit_rle_rgbe\n\n";
        out << "-Y " << Height << " +X " << Width << "\n";

        for (int y = 0; y < Height; ++y)
        {
            for (int x = 0; x < Width; ++x)
            {
                const float u = (static_cast<float>(x) + 0.5f) /
                                static_cast<float>(Width);
                const float v = (static_cast<float>(y) + 0.5f) /
                                static_cast<float>(Height);
                const float wrappedU = std::min(std::abs(u - 0.72f),
                                                1.0f - std::abs(u - 0.72f));
                const float deltaV = v - 0.38f;
                const float key = 14.0f *
                                  std::exp(-(wrappedU * wrappedU * 420.0f +
                                             deltaV * deltaV * 260.0f));
                const float horizon =
                    std::exp(-std::abs(v - 0.52f) * 9.0f);
                const float floor = std::clamp((v - 0.5f) * 2.0f,
                                               0.0f,
                                               1.0f);
                const float upperHemisphere = 1.0f - floor;

                WriteRGBE(out,
                          0.04f + 0.44f * upperHemisphere +
                              0.14f * horizon + 0.04f * floor + key,
                          0.05f + 0.50f * upperHemisphere +
                              0.16f * horizon + 0.03f * floor + key * 0.82f,
                          0.07f + 0.58f * upperHemisphere +
                              0.18f * horizon + 0.02f * floor + key * 0.58f);
            }
        }
        return out.good();
    }
} // namespace

int main(int argc, char* argv[])
{
    std::filesystem::path outputPath;
    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i] ? argv[i] : "";
        if (argument == "--output" && i + 1 < argc)
        {
            outputPath = argv[++i];
        }
        else
        {
            std::cerr << "Usage: SampleHDRFixtureWriter --output <path>\n";
            return 2;
        }
    }

    if (outputPath.empty())
    {
        std::cerr << "Missing --output path\n";
        return 2;
    }
    if (!WriteHDRFixture(outputPath))
    {
        std::cerr << "Failed to write HDR fixture: " << outputPath << "\n";
        return 1;
    }

    std::cout << "Wrote HDR fixture: " << outputPath << "\n";
    return 0;
}
