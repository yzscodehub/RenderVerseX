#pragma once

/** @file CookedMaterialArtifact.h @brief Strict standalone cooked material contract */

#include "Core/Hash/SHA256.h"
#include "Core/Types.h"
#include "RenderContracts/RenderMaterial.h"
#include "Resource/Loader/TextureReference.h"

#include <span>
#include <string>
#include <vector>

namespace RVX::Resource
{
    inline constexpr std::string_view RVX_MATERIAL_ARTIFACT_MAGIC =
        "RVX_MATERIAL_ARTIFACT_V1";
    inline constexpr uint32 RVX_MATERIAL_ARTIFACT_SCHEMA_VERSION = 1;
    inline constexpr std::string_view RVX_MATERIAL_ARTIFACT_WRITER_ABI =
        "RVX_MATERIAL_ARTIFACT_WRITER_ABI_V1";

    /** @brief One typed material slot and its contained cooked dependency. */
    struct CookedMaterialTextureBinding
    {
        MaterialTextureSlotDefinition definition;
        std::string artifactPath;
        Hash::SHA256Digest expectedContentHash{};
        TextureUsage usage = TextureUsage::Color;
        bool isSRGB = true;
        TextureFallbackSemantic fallbackSemantic =
            TextureFallbackSemantic::White;
    };

    /** @brief Canonical standalone material product consumed by Runtime. */
    struct CookedMaterialArtifact
    {
        std::string logicalMaterialId;
        MaterialDefinition material;
        uint64 permutation = 0;
        std::vector<CookedMaterialTextureBinding> textureBindings;
        Hash::SHA256Digest dependencyHash{};
        Hash::SHA256Digest contentHash{};
    };

    /**
     * @brief Canonicalize binding order and seal layout/dependency hashes.
     *
     * Call this once after authoring an artifact and before serialization.
     */
    [[nodiscard]] bool FinalizeCookedMaterialArtifact(
        CookedMaterialArtifact& artifact,
        std::string& outError);

    /** @brief Strictly validate an already finalized artifact. */
    [[nodiscard]] bool ValidateCookedMaterialArtifact(
        const CookedMaterialArtifact& artifact,
        std::string& outError);

    /** @brief Serialize one deterministic material artifact envelope. */
    [[nodiscard]] bool SerializeCookedMaterialArtifact(
        const CookedMaterialArtifact& artifact,
        std::vector<uint8>& outBytes,
        std::string& outError);

    /** @brief Parse and fully validate one material artifact envelope. */
    [[nodiscard]] bool DeserializeCookedMaterialArtifact(
        std::span<const uint8> bytes,
        CookedMaterialArtifact& outArtifact,
        std::string& outError);
} // namespace RVX::Resource
