#pragma once

/** @file CookedMaterialPreparation.h @brief Shared CPU-only cooked material preparation */

#include "Core/Hash/SHA256.h"
#include "Core/Types.h"
#include "Resource/IResource.h"
#include "Resource/ResourceHandle.h"
#include "Resource/Types/MaterialResource.h"
#include "Resource/Types/TextureResource.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace RVX::Resource
{
    /** @brief Reusable prepared texture ownership for related material sidecars. */
    struct CookedMaterialPreparationCache
    {
        std::unordered_map<ResourceId, ResourceHandle<TextureResource>> textures;
        std::unordered_map<ResourceId, Hash::SHA256Digest> textureContentHashes;
    };

    /** @brief Unpublished resources built from one strict cooked material product. */
    struct CookedMaterialPreparedResources
    {
        ResourceHandle<MaterialResource> material;
        std::vector<ResourceHandle<TextureResource>> textures;
        std::vector<uint8> artifactBytes;
    };

    /**
     * @brief Parse one .rvmat and all contained .rva texture dependencies.
     *
     * This helper deliberately has no ResourceManager, cache, registry, scene,
     * or render dependency so future Model loading can reuse the exact same
     * Prepare-only artifact contract.
     */
    [[nodiscard]] bool PrepareCookedMaterialResources(
        const std::string& materialArtifactPath,
        const std::string& materialIdentityPath,
        ResourceId materialId,
        const std::string& materialResourcePath,
        const std::string& materialName,
        CookedMaterialPreparationCache* preparationCache,
        CookedMaterialPreparedResources& outResources,
        std::string& outError);
} // namespace RVX::Resource
