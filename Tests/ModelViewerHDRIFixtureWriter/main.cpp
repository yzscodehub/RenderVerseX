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
        uint8_t rgbe[4] = {};
        if (maxComponent > 0.0f)
        {
            int exponent = 0;
            const float mantissa = std::frexp(maxComponent, &exponent);
            const float scale = mantissa * 256.0f / maxComponent;
            rgbe[0] = static_cast<uint8_t>(std::clamp(r * scale, 0.0f, 255.0f));
            rgbe[1] = static_cast<uint8_t>(std::clamp(g * scale, 0.0f, 255.0f));
            rgbe[2] = static_cast<uint8_t>(std::clamp(b * scale, 0.0f, 255.0f));
            rgbe[3] = static_cast<uint8_t>(exponent + 128);
        }

        out.write(reinterpret_cast<const char*>(rgbe), sizeof(rgbe));
    }

    bool WriteHDRFixture(const std::filesystem::path& outputPath)
    {
        std::filesystem::create_directories(outputPath.parent_path());

        std::ofstream out(outputPath, std::ios::binary);
        if (!out)
        {
            return false;
        }

        constexpr int width = 4;
        constexpr int height = 2;
        out << "#?RADIANCE\n";
        out << "FORMAT=32-bit_rle_rgbe\n";
        out << "\n";
        out << "-Y " << height << " +X " << width << "\n";

        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                const float u = static_cast<float>(x) / static_cast<float>(width - 1);
                const float v = static_cast<float>(y) / static_cast<float>(height - 1);
                const float sky = 1.0f + v * 1.5f;
                WriteRGBE(out,
                          0.15f + u * 1.2f,
                          0.25f + sky,
                          0.55f + (1.0f - u) * 0.7f);
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
        const std::string arg = argv[i];
        if (arg == "--output" && i + 1 < argc)
        {
            outputPath = argv[++i];
        }
        else
        {
            std::cerr << "Usage: ModelViewerHDRIFixtureWriter --output <path>\n";
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
