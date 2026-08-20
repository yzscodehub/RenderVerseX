/** @file SourceResourceLoaders.cpp @brief Development source loader adapters */

#include "AssetImport/SourceResourceLoaders.h"

namespace RVX::AssetImport
{
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
