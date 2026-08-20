/** @file SourceResourceLoaders.cpp @brief Development source loader adapters */

#include "AssetImport/SourceResourceLoaders.h"

namespace RVX::AssetImport
{
SourceAudioLoader::SourceAudioLoader(Resource::ResourceManager* manager)
    : Resource::AudioLoader(manager)
{
}

SourceEnvironmentLoader::SourceEnvironmentLoader(
    Resource::EnvironmentLoadOptions options)
    : Resource::EnvironmentLoader(options)
{
}

SourceTextureLoader::SourceTextureLoader(Resource::ResourceManager* manager)
    : Resource::TextureLoader(
          manager,
          false,
          Resource::ResourceLoaderDomainPolicy::SourceOnly)
{
}

SourceModelLoader::SourceModelLoader(Resource::ResourceManager* manager)
    : Resource::ModelLoader(
          manager,
          Resource::ResourceLoaderDomainPolicy::SourceOnly)
{
}
} // namespace RVX::AssetImport
