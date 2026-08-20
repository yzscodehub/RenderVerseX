#pragma once

/** @file CookedEnvironmentArtifact.h @brief Float16 IBL environment product */

#include "Core/Hash/SHA256.h"
#include "Core/Types.h"

#include <array>
#include <span>
#include <string>
#include <vector>

namespace RVX::Resource
{
    class TextureResource;

    inline constexpr char RVX_ENVIRONMENT_ARTIFACT_MAGIC[] =
        "RVX_ENVIRONMENT_ARTIFACT_V2";
    inline constexpr uint32 RVX_ENVIRONMENT_ARTIFACT_SCHEMA_VERSION = 2;

    enum class CookedEnvironmentTextureRole : uint8
    {
        Environment = 0,
        Irradiance,
        Prefiltered,
        BrdfLut,
    };

    struct CookedEnvironmentTexture
    {
        CookedEnvironmentTextureRole role =
            CookedEnvironmentTextureRole::Environment;
        uint32 width = 0;
        uint32 height = 0;
        uint32 mipLevels = 0;
        uint32 arrayLayers = 0;
        bool cubemap = false;
        std::vector<uint16> rgba16f;
        Hash::SHA256Digest contentHash{};

        [[nodiscard]] bool IsValid(std::string& outError) const;
    };

    struct CookedEnvironmentArtifact
    {
        std::string sourcePath;
        uint64 importOptionsHash = 0;
        float32 intensity = 1.0f;
        std::array<CookedEnvironmentTexture, 4> textures;
        Hash::SHA256Digest payloadHash{};

        [[nodiscard]] bool IsValid(std::string& outError) const;
    };

    /** @brief Convert one generated RGBA32F/RGBA16F texture to canonical half. */
    [[nodiscard]] bool BuildCookedEnvironmentTexture(
        CookedEnvironmentTextureRole role,
        const TextureResource& texture,
        CookedEnvironmentTexture& outTexture,
        std::string& outError);

    [[nodiscard]] bool SerializeCookedEnvironmentArtifact(
        const CookedEnvironmentArtifact& artifact,
        std::vector<uint8>& outBytes,
        std::string& outError);

    [[nodiscard]] bool DeserializeCookedEnvironmentArtifact(
        std::span<const uint8> bytes,
        CookedEnvironmentArtifact& outArtifact,
        std::string& outError);
} // namespace RVX::Resource
