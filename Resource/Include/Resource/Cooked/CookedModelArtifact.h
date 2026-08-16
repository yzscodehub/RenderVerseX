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

#include <span>
#include <string>
#include <vector>

namespace RVX::Resource
{
    inline constexpr const char* RVX_MODEL_PREBAKE_LEGACY_MAGIC =
        "RVX_MODEL_PREBAKE_V1";
    inline constexpr uint32 RVX_MODEL_PREBAKE_LEGACY_SCHEMA_VERSION = 1;
    inline constexpr const char* RVX_MODEL_PREBAKE_LEGACY_BUILD_FINGERPRINT =
        "RVX_STATIC_GLTF_MODEL_V1";
    inline constexpr const char* RVX_MODEL_PREBAKE_MAGIC =
        "RVX_MODEL_PREBAKE_V2";
    inline constexpr uint32 RVX_MODEL_PREBAKE_SCHEMA_VERSION = 2;
    inline constexpr const char* RVX_MODEL_PREBAKE_BUILD_FINGERPRINT =
        "RVX_GLTF_MODEL_WITH_ANIMATION_V2";

    /** @brief Parsed data carried by an RVX_MODEL_PREBAKE artifact. */
    struct CookedModelArtifact
    {
        std::string sourcePath;
        std::string meshArtifactPath;
        /** @brief Optional contained .rvxanim dependency (schema v2). */
        std::string animationArtifactPath;
        std::vector<TextureReference> textures;
        std::vector<Material::Ptr> materials;
        Node::Ptr rootNode;
        BoundingBox bounds;
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
