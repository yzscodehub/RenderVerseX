#pragma once

/** @file SourceResourceLoaders.h @brief Development source loader adapters */

#include "AssetImport/AudioLoader.h"
#include "Resource/Loader/EnvironmentLoader.h"
#include "Resource/Loader/ModelLoader.h"
#include "Resource/Loader/TextureLoader.h"

namespace RVX::AssetImport
{
    /** @brief Audio decoder selected exclusively for Source requests. */
    class SourceAudioLoader final : public Resource::AudioLoader
    {
    public:
        explicit SourceAudioLoader(Resource::ResourceManager* manager);
    };

    /** @brief HDR/EXR IBL importer selected exclusively for Source requests. */
    class SourceEnvironmentLoader final : public Resource::EnvironmentLoader
    {
    public:
        explicit SourceEnvironmentLoader(
            Resource::EnvironmentLoadOptions options = {});
    };

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
