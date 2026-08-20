#pragma once

/** @file CookedTextureArtifact.h @brief Strict cooked texture reader contract */

#include "Core/Types.h"
#include "Resource/Types/TextureResource.h"

#include <span>
#include <string>
#include <vector>

namespace RVX::Resource
{
    struct CookedTextureArtifact
    {
        TextureMetadata metadata;
        std::vector<uint8> pixels;
    };

    /** @brief Parse and validate one RVX_TEXTURE_PREBAKE_V1 product. */
    [[nodiscard]] bool DeserializeCookedTextureArtifact(
        std::span<const uint8> bytes,
        CookedTextureArtifact& outArtifact,
        std::string& outError);
} // namespace RVX::Resource
