#pragma once

/**
 * @file CookedModelArtifact.h
 * @brief Versioned CPU-only representation of a cooked model product.
 */

#include "Core/Hash/SHA256.h"
#include "Core/Math/AABB.h"
#include "Core/Types.h"
#include "Geometry/Asset/Node.h"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace RVX::Resource
{
    inline constexpr const char* RVX_MODEL_ARTIFACT_MAGIC =
        "RVX_MODEL_ARTIFACT_V3";
    inline constexpr uint32 RVX_MODEL_ARTIFACT_SCHEMA_VERSION = 3;
    inline constexpr const char* RVX_MODEL_ARTIFACT_WRITER_ABI =
        "RVX_MODEL_ARTIFACT_WRITER_ABI_V3";

    /** @brief One ordered, content-bound standalone material dependency. */
    struct CookedMaterialArtifactReference
    {
        std::string packageRelativePath;
        /** @brief SHA-256 of the complete referenced .rvmat file bytes. */
        Hash::SHA256Digest expectedContentHash{};
    };

    /** @brief Parsed data carried by the single supported model artifact schema. */
    struct CookedModelArtifact
    {
        std::string logicalAssetId;
        std::string meshArtifactPath;
        std::optional<std::string> animationArtifactPath;
        /** @brief Ordered table addressed by each node's material indices. */
        std::vector<CookedMaterialArtifactReference> materialArtifacts;
        Node::Ptr rootNode;
        AABB bounds;
    };

    /** @brief Serialize a deterministic model product including a content hash. */
    bool SerializeCookedModelArtifact(const CookedModelArtifact& artifact,
                                      std::vector<uint8>& outBytes,
                                      std::string& outError);

    /** @brief Parse and validate a deterministic model product. */
    bool DeserializeCookedModelArtifact(std::span<const uint8> bytes,
                                        CookedModelArtifact& outArtifact,
                                        std::string& outError);
} // namespace RVX::Resource
