/**
 * @file SampleParticleVfxFixtureWriter.cpp
 * @brief Generates the self-owned authoring inputs for the GFX17 cook transaction.
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace
{
    bool WriteTga(const std::filesystem::path& path, const bool smoke)
    {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) return false;
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        constexpr std::uint16_t Width = 64;
        constexpr std::uint16_t Height = 64;
        const std::array<std::uint8_t, 18> header = {
            0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            static_cast<std::uint8_t>(Width & 0xff),
            static_cast<std::uint8_t>(Width >> 8),
            static_cast<std::uint8_t>(Height & 0xff),
            static_cast<std::uint8_t>(Height >> 8), 32, 0x28};
        output.write(reinterpret_cast<const char*>(header.data()),
                     static_cast<std::streamsize>(header.size()));
        for (std::uint32_t y = 0; y < Height; ++y)
        {
            for (std::uint32_t x = 0; x < Width; ++x)
            {
                const float localX = static_cast<float>(x % 16u) / 15.0f * 2.0f - 1.0f;
                const float localY = static_cast<float>(y % 16u) / 15.0f * 2.0f - 1.0f;
                const float radius = std::sqrt(localX * localX + localY * localY);
                const float falloff = std::clamp(1.0f - radius, 0.0f, 1.0f);
                const float phase = static_cast<float>((x / 16u) +
                                                       (y / 16u) * 4u) / 15.0f;
                const float alpha = smoke ? falloff * 0.82f : falloff;
                const std::uint8_t r = static_cast<std::uint8_t>(std::clamp(
                    smoke ? 118.0f + phase * 42.0f : 255.0f, 0.0f, 255.0f));
                const std::uint8_t g = static_cast<std::uint8_t>(std::clamp(
                    smoke ? 120.0f + phase * 42.0f : 52.0f + phase * 120.0f,
                    0.0f, 255.0f));
                const std::uint8_t b = static_cast<std::uint8_t>(std::clamp(
                    smoke ? 130.0f + phase * 38.0f : 4.0f, 0.0f, 255.0f));
                const std::uint8_t a = static_cast<std::uint8_t>(
                    std::clamp(alpha * 255.0f, 0.0f, 255.0f));
                const std::array<std::uint8_t, 4> pixel = {b, g, r, a};
                output.write(reinterpret_cast<const char*>(pixel.data()), 4);
            }
        }
        return output.good();
    }

    bool WriteAuthoring(const std::filesystem::path& path, const bool smoke)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output << "{\n"
               << "  \"schema\": \"RVX_PARTICLE_EFFECT_AUTHORING_V1\",\n"
               << "  \"logicalAssetId\": \"" << (smoke ? "particle-smoke-v1" : "particle-fire-v1") << "\",\n"
               << "  \"render\": {\"simulation\":\"CPU\",\"renderMode\":\"Billboard\",\"blendMode\":\"" << (smoke ? "Premultiplied" : "Additive") << "\"},\n"
               // The instance seed belongs to the Sample's deterministic ECS
               // configuration.  The cooked effect stays a strictly portable
               // visual descriptor: one color, scalar size range, and no
               // source-side velocity or seed policy.
               << "  \"emitter\": {\"maxParticles\":" << (smoke ? 192 : 256) << ",\"looping\":true,\"durationSeconds\":4.0,\"emissionRate\":" << (smoke ? 22.0 : 46.0) << ",\"position\":[0,0,0],\"speed\":[0.3,1.2],\"lifetime\":[0.8,1.8],\"initialColor\":" << (smoke ? "[0.62,0.66,0.74,0.50]" : "[4.0,0.65,0.05,0.90]") << ",\"size\":[" << (smoke ? "0.22,0.72" : "0.18,0.58") << "],\"rotationDegrees\":[0,360],\"rotationSpeedDegrees\":[-20,20]},\n"
               << "  \"flipbook\": {\"textureArtifactPath\":\"atlas.rva\",\"tiles\":[4,4],\"firstFrame\":0,\"frameCount\":16,\"framesPerSecond\":" << (smoke ? 12.0 : 24.0) << ",\"uvOffset\":[0,0],\"uvScale\":[1,1]},\n"
               << "  \"softDepth\": {\"policy\":\"" << (smoke ? "Required" : "Disabled") << "\",\"fadeDistanceMeters\":" << (smoke ? 0.45 : 0.0) << ",\"contrastPower\":1.0},\n"
               << "  \"distortion\": {\"distortionStrengthPixels\":" << (smoke ? 0.0 : 2.0) << "},\n"
               << "  \"reactiveMask\": {\"reactiveContribution\":" << (smoke ? 0.42 : 0.85) << "},\n"
               << "  \"lightHint\": {\"enabled\":" << (smoke ? "false" : "true") << ",\"color\":" << (smoke ? "[0,0,0]" : "[1.0,0.28,0.04]") << ",\"intensityCandela\":" << (smoke ? 0.0 : 36.0) << ",\"rangeMeters\":" << (smoke ? 0.0 : 2.2) << ",\"flickerFrequencyHz\":" << (smoke ? 0.0 : 1.5) << "}\n"
               << "}\n";
        return output.good();
    }
} // namespace

int main(int argc, char* argv[])
{
    std::filesystem::path outputDirectory;
    bool smoke = false;
    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index] ? argv[index] : "";
        if (argument == "--output-dir" && index + 1 < argc)
        {
            outputDirectory = argv[++index];
        }
        else if (argument == "--kind" && index + 1 < argc)
        {
            const std::string kind = argv[++index];
            if (kind == "smoke") smoke = true;
            else if (kind != "fire") return 2;
        }
        else
        {
            return 2;
        }
    }
    if (outputDirectory.empty() ||
        !WriteTga(outputDirectory / "atlas.tga", smoke) ||
        !WriteAuthoring(outputDirectory / "effect.rvxparticle", smoke))
    {
        std::cerr << "Failed to generate particle VFX cook fixture\n";
        return 1;
    }
    return 0;
}
