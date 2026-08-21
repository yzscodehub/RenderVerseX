#pragma once

/**
 * @file CookedModelArtifact.h
 * @brief Versioned CPU-only representation of a cooked model product.
 */

#include "Core/Math/AABB.h"
#include "Core/Types.h"
#include "Geometry/Asset/Material.h"
#include "Geometry/Asset/Node.h"
#include "Resource/Loader/TextureReference.h"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace RVX::Resource
{
    inline constexpr const char* RVX_MODEL_ARTIFACT_MAGIC =
        "RVX_MODEL_ARTIFACT_V2";
    inline constexpr uint32 RVX_MODEL_ARTIFACT_SCHEMA_VERSION = 2;
    inline constexpr const char* RVX_MODEL_ARTIFACT_WRITER_ABI =
        "RVX_MODEL_ARTIFACT_WRITER_ABI_V2";

    /** @brief Parsed data carried by the single supported model artifact schema. */
    struct CookedModelArtifact
    {
        std::string logicalAssetId;
        std::string meshArtifactPath;
        std::optional<std::string> animationArtifactPath;
        std::vector<TextureReference> textures;
        std::vector<Material::Ptr> materials;
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
