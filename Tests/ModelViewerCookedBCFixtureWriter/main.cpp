#include "Core/Log.h"
#include "Tools/AssetPipeline.h"

#include <array>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

namespace
{
    constexpr uint32_t kTextureWidth = 8;
    constexpr uint32_t kTextureHeight = 8;

    using TexturePixels = std::vector<uint8_t>;

    bool WriteRgbaTga(const std::filesystem::path& path,
                      uint32_t width,
                      uint32_t height,
                      const TexturePixels& pixels)
    {
        if (width == 0 || height == 0 || pixels.size() != static_cast<size_t>(width) * height * 4u)
        {
            return false;
        }

        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec)
        {
            return false;
        }

        std::ofstream file(path, std::ios::binary);
        if (!file)
        {
            return false;
        }

        std::array<uint8_t, 18> header = {};
        header[2] = 2; // Uncompressed true-color image.
        header[12] = static_cast<uint8_t>(width & 0xffu);
        header[13] = static_cast<uint8_t>((width >> 8u) & 0xffu);
        header[14] = static_cast<uint8_t>(height & 0xffu);
        header[15] = static_cast<uint8_t>((height >> 8u) & 0xffu);
        header[16] = 32;
        header[17] = 0x20; // Top-left origin.
        file.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));

        for (uint32_t y = 0; y < height; ++y)
        {
            for (uint32_t x = 0; x < width; ++x)
            {
                const size_t offset = (static_cast<size_t>(y) * width + x) * 4u;
                const uint8_t bgra[4] = {
                    pixels[offset + 2],
                    pixels[offset + 1],
                    pixels[offset + 0],
                    pixels[offset + 3]
                };
                file.write(reinterpret_cast<const char*>(bgra), sizeof(bgra));
            }
        }

        return file.good();
    }

    TexturePixels MakeBaseColorBC7()
    {
        TexturePixels pixels(static_cast<size_t>(kTextureWidth) * kTextureHeight * 4u);
        for (uint32_t y = 0; y < kTextureHeight; ++y)
        {
            for (uint32_t x = 0; x < kTextureWidth; ++x)
            {
                const size_t offset = (static_cast<size_t>(y) * kTextureWidth + x) * 4u;
                pixels[offset + 0] = static_cast<uint8_t>(32u + x * 24u);
                pixels[offset + 1] = static_cast<uint8_t>(72u + y * 18u);
                pixels[offset + 2] = ((x + y) % 2u == 0u) ? 224u : 80u;
                pixels[offset + 3] = 255u;
            }
        }
        return pixels;
    }

    TexturePixels MakeMetallicRoughness()
    {
        TexturePixels pixels(static_cast<size_t>(kTextureWidth) * kTextureHeight * 4u);
        for (uint32_t y = 0; y < kTextureHeight; ++y)
        {
            for (uint32_t x = 0; x < kTextureWidth; ++x)
            {
                const size_t offset = (static_cast<size_t>(y) * kTextureWidth + x) * 4u;
                pixels[offset + 0] = 255u;
                pixels[offset + 1] = static_cast<uint8_t>(64u + y * 20u); // Roughness.
                pixels[offset + 2] = static_cast<uint8_t>(24u + x * 20u); // Metallic.
                pixels[offset + 3] = 255u;
            }
        }
        return pixels;
    }

    TexturePixels MakeNormal()
    {
        TexturePixels pixels(static_cast<size_t>(kTextureWidth) * kTextureHeight * 4u);
        for (uint32_t y = 0; y < kTextureHeight; ++y)
        {
            for (uint32_t x = 0; x < kTextureWidth; ++x)
            {
                const size_t offset = (static_cast<size_t>(y) * kTextureWidth + x) * 4u;
                pixels[offset + 0] = static_cast<uint8_t>(112u + x * 4u);
                pixels[offset + 1] = static_cast<uint8_t>(144u - y * 4u);
                pixels[offset + 2] = 240u;
                pixels[offset + 3] = 255u;
            }
        }
        return pixels;
    }

    TexturePixels MakeAmbientOcclusion()
    {
        TexturePixels pixels(static_cast<size_t>(kTextureWidth) * kTextureHeight * 4u);
        for (uint32_t y = 0; y < kTextureHeight; ++y)
        {
            for (uint32_t x = 0; x < kTextureWidth; ++x)
            {
                const size_t offset = (static_cast<size_t>(y) * kTextureWidth + x) * 4u;
                const uint8_t value = static_cast<uint8_t>(128u + ((x * y) % 8u) * 12u);
                pixels[offset + 0] = value;
                pixels[offset + 1] = value;
                pixels[offset + 2] = value;
                pixels[offset + 3] = 255u;
            }
        }
        return pixels;
    }

    TexturePixels MakeEmissive()
    {
        TexturePixels pixels(static_cast<size_t>(kTextureWidth) * kTextureHeight * 4u);
        for (uint32_t y = 0; y < kTextureHeight; ++y)
        {
            for (uint32_t x = 0; x < kTextureWidth; ++x)
            {
                const size_t offset = (static_cast<size_t>(y) * kTextureWidth + x) * 4u;
                pixels[offset + 0] = static_cast<uint8_t>(32u + y * 12u);
                pixels[offset + 1] = static_cast<uint8_t>(32u + x * 18u);
                pixels[offset + 2] = ((x + y) % 2u == 0u) ? 96u : 220u;
                pixels[offset + 3] = 255u;
            }
        }
        return pixels;
    }

    bool ReadFile(const std::filesystem::path& path, std::string& outText)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            return false;
        }

        outText.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        return true;
    }

    bool ContainsField(const std::string& text, const std::string& key, const std::string& value)
    {
        return text.find(key + "=" + value + "\n") != std::string::npos;
    }

    std::string QuoteCommandArgument(const std::filesystem::path& path)
    {
        std::string value = path.string();
        std::string quoted = "\"";
        for (char ch : value)
        {
            if (ch == '"')
            {
                quoted += "\\\"";
            }
            else
            {
                quoted += ch;
            }
        }
        quoted += "\"";
        return quoted;
    }

    std::string WrapSystemCommand(std::string command)
    {
#if defined(_WIN32)
        return "\"" + command + "\"";
#else
        return command;
#endif
    }

    bool VerifyTextureArtifact(const std::filesystem::path& path,
                               const char* expectedFormat,
                               const char* expectedUsage,
                               const char* expectedCompression,
                               std::string& outError)
    {
        std::string artifact;
        if (!ReadFile(path, artifact))
        {
            outError = "Failed to read cooked texture artifact: " + path.string();
            return false;
        }

        if (!ContainsField(artifact, "format", expectedFormat) ||
            !ContainsField(artifact, "usage", expectedUsage) ||
            !ContainsField(artifact, "compression", expectedCompression))
        {
            outError = "Cooked texture artifact has unexpected metadata: " + path.string();
            return false;
        }

        return true;
    }

    bool CookTexture(const std::filesystem::path& source,
                     const std::filesystem::path& output,
                     const TexturePixels& pixels,
                     RVX::Tools::TextureCompressionMode compressionMode,
                     const char* expectedFormat,
                     const char* expectedUsage,
                     const char* expectedCompression,
                     std::string& outError)
    {
        if (!WriteRgbaTga(source, kTextureWidth, kTextureHeight, pixels))
        {
            outError = "Failed to write texture source: " + source.string();
            return false;
        }

        RVX::Tools::TextureImporter importer;
        RVX::Tools::TextureImportOptions options;
        options.generateMipmaps = true;
        options.compress = true;
        options.compressionMode = compressionMode;

        const RVX::Tools::ImportResult result = importer.Import(source, output, &options);
        if (!result.success)
        {
            outError = result.error.empty()
                ? "Texture import failed: " + source.string()
                : result.error;
            return false;
        }

        for (const std::string& warning : result.warnings)
        {
            std::cout << "Texture import warning: " << warning << "\n";
        }

        return VerifyTextureArtifact(output,
                                     expectedFormat,
                                     expectedUsage,
                                     expectedCompression,
                                     outError);
    }

    bool WriteCookProfile(const std::filesystem::path& outputPath)
    {
        std::error_code ec;
        std::filesystem::create_directories(outputPath.parent_path(), ec);
        if (ec)
        {
            return false;
        }

        std::ofstream file(outputPath, std::ios::binary);
        if (!file)
        {
            return false;
        }

        file << "RVX_COOK_PROFILE_V1\n"
             << "texture.compression=none\n"
             << "texture.compression[BaseColorBC7.tga]=bc7\n"
             << "texture.compression[MetallicRoughness.tga]=bc3\n"
             << "texture.compression[Normal.tga]=bc5\n"
             << "texture.compression[AO.tga]=bc1\n"
             << "texture.compression[Emissive.tga]=bc1\n";

        return file.good();
    }

    bool WriteGltf(const std::filesystem::path& outputPath)
    {
        std::ofstream file(outputPath, std::ios::binary);
        if (!file)
        {
            return false;
        }

        file << R"gltf({
  "asset": {
    "version": "2.0",
    "generator": "RenderVerseX SP14 cooked BC material fixture"
  },
  "scene": 0,
  "scenes": [
    {
      "name": "SP14CookedBCMaterialScene",
      "nodes": [0]
    }
  ],
  "nodes": [
    {
      "name": "SP14CookedBCMaterialSwatch",
      "mesh": 0
    }
  ],
  "meshes": [
    {
      "name": "SP14CookedBCMaterialMesh",
      "primitives": [
        {
          "attributes": {
            "POSITION": 0,
            "NORMAL": 1,
            "TEXCOORD_0": 2,
            "TANGENT": 3
          },
          "indices": 4,
          "material": 0,
          "mode": 4
        }
      ]
    }
  ],
  "materials": [
    {
      "name": "SP14CookedBCMaterial",
      "pbrMetallicRoughness": {
        "baseColorFactor": [1.0, 1.0, 1.0, 1.0],
        "baseColorTexture": { "index": 0 },
        "metallicFactor": 0.75,
        "roughnessFactor": 1.0,
        "metallicRoughnessTexture": { "index": 1 }
      },
      "normalTexture": {
        "index": 2,
        "scale": 1.0
      },
      "occlusionTexture": {
        "index": 3,
        "strength": 1.0
      },
      "emissiveFactor": [0.35, 0.35, 0.35],
      "emissiveTexture": { "index": 4 },
      "alphaMode": "OPAQUE"
    }
  ],
  "textures": [
    { "source": 0 },
    { "source": 1 },
    { "source": 2 },
    { "source": 3 },
    { "source": 4 }
  ],
  "images": [
    { "name": "SP14BaseColorBC7", "uri": "BaseColorBC7.tga" },
    { "name": "SP14MetallicRoughnessBC3", "uri": "MetallicRoughness.tga" },
    { "name": "SP14NormalBC5", "uri": "Normal.tga" },
    { "name": "SP14OcclusionBC1", "uri": "AO.tga" },
    { "name": "SP14EmissiveBC1", "uri": "Emissive.tga" }
  ],
  "buffers": [
    {
      "byteLength": 204,
      "uri": "data:application/octet-stream;base64,AACAvwAAQL8AAAAAAACAPwAAQL8AAAAAAACAPwAAQD8AAAAAAACAvwAAQD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAgD8AAIA/AACAPwAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AACAPwAAAAAAAAAAAACAPwAAgD8AAAAAAAAAAAAAgD8AAIA/AAAAAAAAAAAAAIA/AAABAAIAAAACAAMA"
    }
  ],
  "bufferViews": [
    { "buffer": 0, "byteOffset": 0, "byteLength": 48, "target": 34962 },
    { "buffer": 0, "byteOffset": 48, "byteLength": 48, "target": 34962 },
    { "buffer": 0, "byteOffset": 96, "byteLength": 32, "target": 34962 },
    { "buffer": 0, "byteOffset": 128, "byteLength": 64, "target": 34962 },
    { "buffer": 0, "byteOffset": 192, "byteLength": 12, "target": 34963 }
  ],
  "accessors": [
    {
      "bufferView": 0,
      "byteOffset": 0,
      "componentType": 5126,
      "count": 4,
      "type": "VEC3",
      "min": [-1.0, -0.75, 0.0],
      "max": [1.0, 0.75, 0.0]
    },
    { "bufferView": 1, "byteOffset": 0, "componentType": 5126, "count": 4, "type": "VEC3" },
    { "bufferView": 2, "byteOffset": 0, "componentType": 5126, "count": 4, "type": "VEC2" },
    { "bufferView": 3, "byteOffset": 0, "componentType": 5126, "count": 4, "type": "VEC4" },
    {
      "bufferView": 4,
      "byteOffset": 0,
      "componentType": 5123,
      "count": 6,
      "type": "SCALAR",
      "min": [0],
      "max": [3]
    }
  ]
}
)gltf";

        return file.good();
    }

    bool RunRVXCook(const std::filesystem::path& rvxCookPath,
                    const std::filesystem::path& sourceDir,
                    const std::filesystem::path& outputDir,
                    const std::filesystem::path& profilePath,
                    const std::filesystem::path& manifestPath,
                    std::string& outError)
    {
        const std::string command = WrapSystemCommand(
            QuoteCommandArgument(rvxCookPath) +
            " --source " + QuoteCommandArgument(sourceDir) +
            " --output " + QuoteCommandArgument(outputDir) +
            " --manifest " + QuoteCommandArgument(manifestPath) +
            " --profile " + QuoteCommandArgument(profilePath) +
            " --rewrite-gltf-texture-uris" +
            " --fail-on-errors");

        const int exitCode = std::system(command.c_str());
        if (exitCode != 0)
        {
            outError = "RVXCook failed with exit code " + std::to_string(exitCode) + ": " + command;
            return false;
        }

        return true;
    }

    bool VerifyRewrittenGltf(const std::filesystem::path& path, std::string& outError)
    {
        std::string gltf;
        if (!ReadFile(path, gltf))
        {
            outError = "Failed to read rewritten glTF fixture: " + path.string();
            return false;
        }

        const bool hasCookedUris =
            gltf.find("\"uri\": \"BaseColorBC7.rva\"") != std::string::npos &&
            gltf.find("\"uri\": \"MetallicRoughness.rva\"") != std::string::npos &&
            gltf.find("\"uri\": \"Normal.rva\"") != std::string::npos &&
            gltf.find("\"uri\": \"AO.rva\"") != std::string::npos &&
            gltf.find("\"uri\": \"Emissive.rva\"") != std::string::npos;
        const bool hasSourceUris =
            gltf.find(".tga\"") != std::string::npos;
        if (!hasCookedUris || hasSourceUris)
        {
            outError = "Rewritten glTF did not redirect all texture URIs to cooked .rva artifacts: " + path.string();
            return false;
        }

        return true;
    }

    bool WriteFixture(const std::filesystem::path& outputDir,
                      const std::filesystem::path& rvxCookPath,
                      std::string& outError)
    {
        if (outputDir.empty())
        {
            outError = "Output directory is empty";
            return false;
        }

        std::error_code ec;
        std::filesystem::create_directories(outputDir, ec);
        if (ec)
        {
            outError = "Failed to create output directory: " + ec.message();
            return false;
        }

        if (rvxCookPath.empty() || !std::filesystem::exists(rvxCookPath))
        {
            outError = "RVXCook executable path is missing or does not exist: " + rvxCookPath.string();
            return false;
        }

        std::filesystem::remove_all(outputDir, ec);
        if (ec)
        {
            outError = "Failed to clear output directory before writing fixture: " + ec.message();
            return false;
        }

        std::filesystem::create_directories(outputDir, ec);
        if (ec)
        {
            outError = "Failed to recreate output directory: " + ec.message();
            return false;
        }

        const std::filesystem::path sourceDir = outputDir / "_Source";
        const std::filesystem::path profilePath = outputDir / "CookProfile.rvxprofile";
        const std::filesystem::path manifestPath = outputDir / "CookManifest.rvxmanifest";

        if (!WriteRgbaTga(sourceDir / "BaseColorBC7.tga", kTextureWidth, kTextureHeight, MakeBaseColorBC7()) ||
            !WriteRgbaTga(sourceDir / "MetallicRoughness.tga", kTextureWidth, kTextureHeight, MakeMetallicRoughness()) ||
            !WriteRgbaTga(sourceDir / "Normal.tga", kTextureWidth, kTextureHeight, MakeNormal()) ||
            !WriteRgbaTga(sourceDir / "AO.tga", kTextureWidth, kTextureHeight, MakeAmbientOcclusion()) ||
            !WriteRgbaTga(sourceDir / "Emissive.tga", kTextureWidth, kTextureHeight, MakeEmissive()))
        {
            outError = "Failed to write one or more authored texture sources";
            return false;
        }

        if (!WriteGltf(sourceDir / "CookedBCMaterial.gltf"))
        {
            outError = "Failed to write authored glTF fixture source";
            return false;
        }

        if (!WriteCookProfile(profilePath))
        {
            outError = "Failed to write cook profile: " + profilePath.string();
            return false;
        }

        if (!RunRVXCook(rvxCookPath, sourceDir, outputDir, profilePath, manifestPath, outError))
        {
            return false;
        }

        if (!VerifyTextureArtifact(outputDir / "BaseColorBC7.rva", "BC7", "Color", "BC7", outError) ||
            !VerifyTextureArtifact(outputDir / "MetallicRoughness.rva", "BC3", "Data", "BC3", outError) ||
            !VerifyTextureArtifact(outputDir / "Normal.rva", "BC5", "Normal", "BC5", outError) ||
            !VerifyTextureArtifact(outputDir / "AO.rva", "BC1", "Data", "BC1", outError) ||
            !VerifyTextureArtifact(outputDir / "Emissive.rva", "BC1", "Color", "BC1", outError))
        {
            return false;
        }

        return VerifyRewrittenGltf(outputDir / "CookedBCMaterial.gltf", outError);
    }
} // namespace

int main(int argc, char* argv[])
{
    RVX::Log::Config logConfig;
    logConfig.enableRotation = false;
    logConfig.logFileName = "ModelViewerCookedBCFixtureWriter.log";
    RVX::Log::Initialize(logConfig);

    std::filesystem::path outputDir;
    std::filesystem::path rvxCookPath;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--output-dir" && i + 1 < argc)
        {
            outputDir = argv[++i];
        }
        else if (arg == "--rvxcook" && i + 1 < argc)
        {
            rvxCookPath = argv[++i];
        }
        else
        {
            std::cerr << "Usage: ModelViewerCookedBCFixtureWriter --output-dir <dir> --rvxcook <path>\n";
            RVX::Log::Shutdown();
            return 2;
        }
    }

    if (outputDir.empty())
    {
        std::cerr << "Missing --output-dir path\n";
        RVX::Log::Shutdown();
        return 2;
    }

    std::string error;
    if (!WriteFixture(outputDir, rvxCookPath, error))
    {
        std::cerr << error << "\n";
        RVX::Log::Shutdown();
        return 1;
    }

    std::cout << "Wrote cooked BC ModelViewer fixture: "
              << (outputDir / "CookedBCMaterial.gltf") << "\n";
    RVX::Log::Shutdown();
    return 0;
}
