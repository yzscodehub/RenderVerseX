/** @file ParticleEffectImporter.cpp @brief Strict CPU billboard effect cooker. */

#include "Tools/Importers/ParticleEffectImporter.h"

#include "Core/Hash/SHA256.h"
#include "Resource/Cooked/CookedParticleEffectArtifact.h"
#include "Resource/Cooked/CookedTextureArtifact.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <span>
#include <string_view>
#include <utility>

namespace RVX::Tools
{
namespace
{
    using Json = nlohmann::json;

    constexpr std::string_view AuthoringSchema =
        "RVX_PARTICLE_EFFECT_AUTHORING_V1";

    bool RequireExactObject(const Json& value,
                            std::initializer_list<std::string_view> fields,
                            std::string_view label,
                            std::string& outError)
    {
        if (!value.is_object())
        {
            outError = std::string(label) + " must be an object";
            return false;
        }
        std::set<std::string> expected;
        for (const std::string_view field : fields)
        {
            expected.emplace(field);
            if (!value.contains(std::string(field)))
            {
                outError = std::string(label) + " is missing required field '" +
                    std::string(field) + "'";
                return false;
            }
        }
        for (const auto& [key, ignored] : value.items())
        {
            (void)ignored;
            if (!expected.contains(key))
            {
                outError = std::string(label) + " contains unsupported field '" + key + "'";
                return false;
            }
        }
        return true;
    }

    bool ReadString(const Json& value,
                    std::string& outValue,
                    std::string_view label,
                    std::string& outError)
    {
        if (!value.is_string())
        {
            outError = std::string(label) + " must be a string";
            return false;
        }
        outValue = value.get<std::string>();
        return true;
    }

    bool ReadBool(const Json& value,
                  bool& outValue,
                  std::string_view label,
                  std::string& outError)
    {
        if (!value.is_boolean())
        {
            outError = std::string(label) + " must be a boolean";
            return false;
        }
        outValue = value.get<bool>();
        return true;
    }

    bool ReadU32(const Json& value,
                 uint32& outValue,
                 std::string_view label,
                 std::string& outError)
    {
        if (!value.is_number_integer() && !value.is_number_unsigned())
        {
            outError = std::string(label) + " must be an unsigned integer";
            return false;
        }
        const int64 parsed = value.get<int64>();
        if (parsed < 0 || static_cast<uint64>(parsed) > UINT32_MAX)
        {
            outError = std::string(label) + " is outside uint32 range";
            return false;
        }
        outValue = static_cast<uint32>(parsed);
        return true;
    }

    bool ReadFloat(const Json& value,
                   float32& outValue,
                   std::string_view label,
                   std::string& outError)
    {
        if (!value.is_number())
        {
            outError = std::string(label) + " must be a finite number";
            return false;
        }
        const double parsed = value.get<double>();
        if (!std::isfinite(parsed) || parsed < -std::numeric_limits<float32>::max() ||
            parsed > std::numeric_limits<float32>::max())
        {
            outError = std::string(label) + " must be finite and representable as float32";
            return false;
        }
        outValue = static_cast<float32>(parsed);
        return true;
    }

    template <size_t N>
    bool ReadFloatArray(const Json& value,
                        std::array<float32, N>& outValue,
                        std::string_view label,
                        std::string& outError)
    {
        if (!value.is_array() || value.size() != N)
        {
            outError = std::string(label) + " must be an array of exactly " +
                std::to_string(N) + " numbers";
            return false;
        }
        for (size_t index = 0; index < N; ++index)
        {
            if (!ReadFloat(value[index], outValue[index], label, outError))
            {
                return false;
            }
        }
        return true;
    }

    bool ReadVec2(const Json& value, Vec2& outValue,
                  std::string_view label, std::string& outError)
    {
        std::array<float32, 2> components{};
        if (!ReadFloatArray(value, components, label, outError)) return false;
        outValue = {components[0], components[1]};
        return true;
    }

    bool ReadVec3(const Json& value, Vec3& outValue,
                  std::string_view label, std::string& outError)
    {
        std::array<float32, 3> components{};
        if (!ReadFloatArray(value, components, label, outError)) return false;
        outValue = {components[0], components[1], components[2]};
        return true;
    }

    bool ReadVec4(const Json& value, Vec4& outValue,
                  std::string_view label, std::string& outError)
    {
        std::array<float32, 4> components{};
        if (!ReadFloatArray(value, components, label, outError)) return false;
        outValue = {components[0], components[1], components[2], components[3]};
        return true;
    }

    bool ReadRange(const Json& value,
                   float32& outMinimum,
                   float32& outMaximum,
                   std::string_view label,
                   std::string& outError)
    {
        std::array<float32, 2> values{};
        if (!ReadFloatArray(value, values, label, outError)) return false;
        outMinimum = values[0];
        outMaximum = values[1];
        return true;
    }

    bool WriteArtifact(const fs::path& outputPath,
                       std::span<const uint8> bytes,
                       std::string& outError)
    {
        if (bytes.empty())
        {
            outError = "Cooked particle effect artifact is empty";
            return false;
        }
        std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
        if (!output.is_open())
        {
            outError = "Cannot create cooked particle effect artifact: " + outputPath.string();
            return false;
        }
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        if (!output.good())
        {
            outError = "Failed to write cooked particle effect artifact: " + outputPath.string();
            return false;
        }
        return true;
    }

    bool ResolveContainedCookedAtlas(const fs::path& cookedRoot,
                                     const std::string& packageRelativePath,
                                     fs::path& outAtlasPath,
                                     std::string& outError)
    {
        if (cookedRoot.empty() || packageRelativePath.empty() ||
            packageRelativePath.find('\\') != std::string::npos ||
            packageRelativePath.find('\0') != std::string::npos ||
            fs::path(packageRelativePath).is_absolute() ||
            fs::path(packageRelativePath).has_root_name() ||
            fs::path(packageRelativePath).generic_string() != packageRelativePath ||
            fs::path(packageRelativePath).extension().string() != ".rva")
        {
            outError = "Particle effect flipbook atlas must be a package-relative .rva path";
            return false;
        }
        for (const fs::path& component : fs::path(packageRelativePath))
        {
            if (component.empty() || component == "." || component == "..")
            {
                outError = "Particle effect flipbook atlas path escapes its cooked package";
                return false;
            }
        }

        std::error_code filesystemError;
        const fs::path canonicalRoot = fs::weakly_canonical(cookedRoot, filesystemError);
        if (filesystemError)
        {
            outError = "Cannot canonicalize particle effect cooked root: " +
                filesystemError.message();
            return false;
        }
        const fs::path candidate = fs::weakly_canonical(
            canonicalRoot / fs::path(packageRelativePath), filesystemError);
        if (filesystemError || !fs::is_regular_file(candidate, filesystemError) || filesystemError)
        {
            outError = "Particle effect referenced cooked atlas is missing: " +
                packageRelativePath;
            return false;
        }
        const fs::path relativeCandidate = fs::relative(candidate, canonicalRoot, filesystemError);
        if (filesystemError || relativeCandidate.empty() || relativeCandidate.is_absolute() ||
            relativeCandidate.has_root_path())
        {
            outError = "Particle effect referenced atlas is not contained by its cooked package";
            return false;
        }
        for (const fs::path& component : relativeCandidate)
        {
            if (component == "..")
            {
                outError = "Particle effect referenced atlas escapes its cooked package";
                return false;
            }
        }
        outAtlasPath = candidate;
        return true;
    }

    bool BindCookedAtlasHash(const fs::path& effectProductDirectory,
                             Resource::CookedParticleEffectArtifact& artifact,
                             fs::path& outAtlasPath,
                             std::string& outError)
    {
        outAtlasPath.clear();
        if (effectProductDirectory.empty())
        {
            outError = "Particle effect cooking requires an effect product directory";
            return false;
        }
        fs::path atlasPath;
        if (!ResolveContainedCookedAtlas(effectProductDirectory,
                                         artifact.flipbook.textureArtifactPath,
                                         atlasPath,
                                         outError))
        {
            return false;
        }

        std::ifstream atlas(atlasPath, std::ios::binary | std::ios::ate);
        if (!atlas.is_open())
        {
            outError = "Cannot open particle effect cooked atlas: " + atlasPath.string();
            return false;
        }
        const std::streamsize byteCount = atlas.tellg();
        constexpr uint64 MaxAtlasBytes = 512ull * 1024ull * 1024ull;
        if (byteCount <= 0 || static_cast<uint64>(byteCount) > MaxAtlasBytes)
        {
            outError = "Particle effect cooked atlas byte count is invalid";
            return false;
        }
        atlas.seekg(0, std::ios::beg);
        std::vector<uint8> atlasBytes(static_cast<size_t>(byteCount));
        if (!atlas.read(reinterpret_cast<char*>(atlasBytes.data()), byteCount))
        {
            outError = "Failed to read particle effect cooked atlas";
            return false;
        }

        Resource::CookedTextureArtifact atlasArtifact;
        if (!Resource::DeserializeCookedTextureArtifact(
                atlasBytes, atlasArtifact, outError))
        {
            outError = "Particle effect cooked atlas is invalid: " + outError;
            return false;
        }
        if (atlasArtifact.metadata.usage != Resource::TextureUsage::Color)
        {
            outError = "Particle effect cooked atlas must use TextureUsage::Color";
            return false;
        }
        const uint64 tilesX = static_cast<uint64>(artifact.flipbook.tilesX);
        const uint64 tilesY = static_cast<uint64>(artifact.flipbook.tilesY);
        if (tilesX == 0u || tilesY == 0u ||
            atlasArtifact.metadata.width < tilesX ||
            atlasArtifact.metadata.height < tilesY ||
            atlasArtifact.metadata.width % tilesX != 0u ||
            atlasArtifact.metadata.height % tilesY != 0u)
        {
            outError = "Particle effect cooked atlas dimensions do not contain an integral flipbook tile grid";
            return false;
        }
        artifact.flipbook.expectedContentHash = Hash::ComputeSHA256(
            atlasBytes.data(), atlasBytes.size());
        outAtlasPath = std::move(atlasPath);
        return true;
    }

    bool ParseAuthoring(const Json& source,
                        Resource::CookedParticleEffectArtifact& outArtifact,
                        std::string& outError)
    {
        outArtifact = {};
        if (!RequireExactObject(source,
                {"schema", "logicalAssetId", "render", "emitter", "flipbook",
                 "softDepth", "distortion", "reactiveMask", "lightHint"},
                "particle effect root", outError))
        {
            return false;
        }
        std::string schema;
        if (!ReadString(source.at("schema"), schema, "schema", outError) ||
            schema != AuthoringSchema ||
            !ReadString(source.at("logicalAssetId"), outArtifact.logicalAssetId,
                        "logicalAssetId", outError))
        {
            if (outError.empty()) outError = "Particle effect authoring schema is unsupported";
            return false;
        }

        const Json& render = source.at("render");
        if (!RequireExactObject(render, {"simulation", "renderMode", "blendMode"},
                                "render", outError)) return false;
        std::string simulation;
        std::string renderMode;
        std::string blendMode;
        if (!ReadString(render.at("simulation"), simulation, "render.simulation", outError) ||
            !ReadString(render.at("renderMode"), renderMode, "render.renderMode", outError) ||
            !ReadString(render.at("blendMode"), blendMode, "render.blendMode", outError) ||
            simulation != "CPU" || renderMode != "Billboard")
        {
            if (outError.empty()) outError = "Particle effect supports only CPU Billboard rendering";
            return false;
        }
        if (blendMode == "AlphaBlend") outArtifact.blendMode = Resource::CookedParticleEffectBlendMode::AlphaBlend;
        else if (blendMode == "Premultiplied") outArtifact.blendMode = Resource::CookedParticleEffectBlendMode::Premultiplied;
        else if (blendMode == "Additive") outArtifact.blendMode = Resource::CookedParticleEffectBlendMode::Additive;
        else
        {
            outError = "Particle effect blendMode must be AlphaBlend, Premultiplied, or Additive";
            return false;
        }

        const Json& emitter = source.at("emitter");
        if (!RequireExactObject(emitter,
                {"maxParticles", "looping", "durationSeconds", "emissionRate", "position",
                 "speed", "lifetime", "initialColor", "size", "rotationDegrees",
                 "rotationSpeedDegrees"},
                "emitter", outError)) return false;
        Resource::CookedParticleEffectEmitter& destination = outArtifact.emitter;
        if (!ReadU32(emitter.at("maxParticles"), destination.maxParticles, "emitter.maxParticles", outError) ||
            !ReadBool(emitter.at("looping"), destination.looping, "emitter.looping", outError) ||
            !ReadFloat(emitter.at("durationSeconds"), destination.durationSeconds, "emitter.durationSeconds", outError) ||
            !ReadFloat(emitter.at("emissionRate"), destination.emissionRate, "emitter.emissionRate", outError) ||
            !ReadVec3(emitter.at("position"), destination.position, "emitter.position", outError) ||
            !ReadRange(emitter.at("speed"), destination.speedMin, destination.speedMax,
                       "emitter.speed", outError) ||
            !ReadRange(emitter.at("lifetime"), destination.lifetimeMin, destination.lifetimeMax,
                       "emitter.lifetime", outError) ||
            !ReadVec4(emitter.at("initialColor"), destination.initialColor,
                      "emitter.initialColor", outError) ||
            !ReadRange(emitter.at("size"), destination.sizeMin, destination.sizeMax,
                       "emitter.size", outError) ||
            !ReadRange(emitter.at("rotationDegrees"), destination.rotationDegreesMin,
                       destination.rotationDegreesMax, "emitter.rotationDegrees", outError) ||
            !ReadRange(emitter.at("rotationSpeedDegrees"), destination.rotationSpeedDegreesMin,
                       destination.rotationSpeedDegreesMax,
                       "emitter.rotationSpeedDegrees", outError)) return false;

        const Json& flipbook = source.at("flipbook");
        if (!RequireExactObject(flipbook,
                {"textureArtifactPath", "tiles", "firstFrame", "frameCount", "framesPerSecond",
                 "uvOffset", "uvScale"},
                "flipbook", outError) ||
            !ReadString(flipbook.at("textureArtifactPath"), outArtifact.flipbook.textureArtifactPath,
                        "flipbook.textureArtifactPath", outError) ||
            !ReadU32(flipbook.at("firstFrame"), outArtifact.flipbook.firstFrame,
                     "flipbook.firstFrame", outError) ||
            !ReadU32(flipbook.at("frameCount"), outArtifact.flipbook.frameCount,
                     "flipbook.frameCount", outError) ||
            !ReadFloat(flipbook.at("framesPerSecond"), outArtifact.flipbook.framesPerSecond,
                       "flipbook.framesPerSecond", outError) ||
            !ReadVec2(flipbook.at("uvOffset"), outArtifact.flipbook.uvOffset,
                      "flipbook.uvOffset", outError) ||
            !ReadVec2(flipbook.at("uvScale"), outArtifact.flipbook.uvScale,
                      "flipbook.uvScale", outError)) return false;
        const Json& tiles = flipbook.at("tiles");
        if (!tiles.is_array() || tiles.size() != 2u ||
            !ReadU32(tiles[0], outArtifact.flipbook.tilesX, "flipbook.tiles[0]", outError) ||
            !ReadU32(tiles[1], outArtifact.flipbook.tilesY, "flipbook.tiles[1]", outError))
        {
            if (outError.empty()) outError = "flipbook.tiles must contain exactly two uint32 values";
            return false;
        }

        const Json& softDepth = source.at("softDepth");
        if (!RequireExactObject(softDepth, {"policy", "fadeDistanceMeters", "contrastPower"},
                                "softDepth", outError)) return false;
        std::string softDepthPolicy;
        if (!ReadString(softDepth.at("policy"), softDepthPolicy, "softDepth.policy", outError) ||
            !ReadFloat(softDepth.at("fadeDistanceMeters"), outArtifact.softDepth.fadeDistanceMeters,
                       "softDepth.fadeDistanceMeters", outError) ||
            !ReadFloat(softDepth.at("contrastPower"), outArtifact.softDepth.contrastPower,
                       "softDepth.contrastPower", outError)) return false;
        if (softDepthPolicy == "Disabled") outArtifact.softDepth.policy = Resource::CookedParticleEffectSoftDepthPolicy::Disabled;
        else if (softDepthPolicy == "Optional") outArtifact.softDepth.policy = Resource::CookedParticleEffectSoftDepthPolicy::Optional;
        else if (softDepthPolicy == "Required") outArtifact.softDepth.policy = Resource::CookedParticleEffectSoftDepthPolicy::Required;
        else
        {
            outError = "softDepth.policy must be Disabled, Optional, or Required";
            return false;
        }

        const Json& distortion = source.at("distortion");
        if (!RequireExactObject(distortion, {"distortionStrengthPixels"}, "distortion", outError) ||
            !ReadFloat(distortion.at("distortionStrengthPixels"), outArtifact.distortion.distortionStrengthPixels,
                       "distortion.distortionStrengthPixels", outError)) return false;
        const Json& reactiveMask = source.at("reactiveMask");
        if (!RequireExactObject(reactiveMask, {"reactiveContribution"}, "reactiveMask", outError) ||
            !ReadFloat(reactiveMask.at("reactiveContribution"), outArtifact.reactiveMask.reactiveContribution,
                       "reactiveMask.reactiveContribution", outError)) return false;

        const Json& lightHint = source.at("lightHint");
        if (!RequireExactObject(lightHint, {"enabled", "color", "intensityCandela", "rangeMeters", "flickerFrequencyHz"},
                                "lightHint", outError) ||
            !ReadBool(lightHint.at("enabled"), outArtifact.lightHint.enabled,
                      "lightHint.enabled", outError) ||
            !ReadVec3(lightHint.at("color"), outArtifact.lightHint.color,
                      "lightHint.color", outError) ||
            !ReadFloat(lightHint.at("intensityCandela"), outArtifact.lightHint.intensityCandela,
                       "lightHint.intensityCandela", outError) ||
            !ReadFloat(lightHint.at("rangeMeters"), outArtifact.lightHint.rangeMeters,
                       "lightHint.rangeMeters", outError) ||
            !ReadFloat(lightHint.at("flickerFrequencyHz"), outArtifact.lightHint.flickerFrequencyHz,
                       "lightHint.flickerFrequencyHz", outError)) return false;

        // The cooker binds the exact already-produced sidecar atlas bytes only
        // after parsing. The runtime uses the same effect package root.
        // IsValid() then enforces the non-zero dependency hash before serialize.
        outError.clear();
        return true;
    }
} // namespace

ImportResult ParticleEffectImporter::Import(const fs::path& sourcePath,
                                            const fs::path& outputPath,
                                            const void* options)
{
    (void)options;
    ImportResult result;
    std::ifstream input(sourcePath, std::ios::binary | std::ios::ate);
    if (!input.is_open())
    {
        result.error = "Cannot open particle effect source: " + sourcePath.string();
        return result;
    }
    const std::streamsize byteCount = input.tellg();
    if (byteCount <= 0 || static_cast<uint64>(byteCount) > 8ull * 1024ull * 1024ull)
    {
        result.error = "Particle effect source size is invalid";
        return result;
    }
    input.seekg(0, std::ios::beg);
    std::string bytes(static_cast<size_t>(byteCount), '\0');
    if (!input.read(bytes.data(), byteCount))
    {
        result.error = "Failed to read particle effect source";
        return result;
    }

    fs::path atlasProductPath;
    try
    {
        const Json source = Json::parse(bytes, nullptr, true, true);
        Resource::CookedParticleEffectArtifact artifact;
        if (!ParseAuthoring(source, artifact, result.error) ||
            !BindCookedAtlasHash(
                outputPath.parent_path(), artifact, atlasProductPath, result.error) ||
            !artifact.IsValid(result.error))
        {
            return result;
        }
        std::vector<uint8> cooked;
        if (!Resource::SerializeCookedParticleEffectArtifact(artifact, cooked, result.error) ||
            !WriteArtifact(outputPath, cooked, result.error))
        {
            return result;
        }
    }
    catch (const Json::exception& exception)
    {
        result.error = "Particle effect authoring JSON is invalid: " + std::string(exception.what());
        return result;
    }

    result.success = true;
    result.outputPaths.push_back(outputPath.string());
    // The atlas is owned by the texture source entry.  It is an exact cooked
    // dependency of this effect, not a second product of the effect source.
    result.dependencyOutputPaths.push_back(atlasProductPath.string());
    return result;
}
} // namespace RVX::Tools
