#pragma once

/** @file SourceResourceLoaders.h @brief Development source loader adapters */

#include "Resource/Loader/ModelLoader.h"
#include "Resource/Loader/TextureLoader.h"

namespace RVX::AssetImport
{
    /** @brief Texture importer selected exclusively for Source requests. */
    class SourceTextureLoader final : public Resource::TextureLoader
    {
    public:
        explicit SourceTextureLoader(Resource::ResourceManager* manager);
    };

    /** @brief Model importer selected exclusively for Source requests. */
    class SourceModelLoader final : public Resource::ModelLoader
    {
    public:
        explicit SourceModelLoader(Resource::ResourceManager* manager);
    };
} // namespace RVX::AssetImport
