/** @file ResourceSourceComposition.cpp @brief Development source dependencies */

#include "ResourceSource/ResourceSourceComposition.h"

#include "AssetImport/SourceResourceLoaders.h"
#include "ResourceSource/PollingResourceChangeMonitor.h"

#include <memory>

namespace RVX::ResourceSource
{
bool AddSourceResourceDependencies(
    ResourceManagerDependencies& dependencies,
    std::string& outError)
{
    if (dependencies.changeMonitor)
    {
        outError =
            "Development resource composition already has a change monitor";
        return false;
    }

    if (!dependencies.loaderCatalog.Add(
            std::make_unique<AssetImport::SourceAudioLoader>(),
            outError))
    {
        return false;
    }
    if (!dependencies.loaderCatalog.Add(
            std::make_unique<AssetImport::SourceEnvironmentLoader>(),
            outError))
    {
        return false;
    }
    if (!dependencies.loaderCatalog.Add(
            std::make_unique<AssetImport::SourceTextureLoader>(),
            outError))
    {
        return false;
    }
    if (!dependencies.loaderCatalog.Add(
            std::make_unique<AssetImport::SourceModelLoader>(),
            outError))
    {
        return false;
    }

    if (dependencies.textureSourceDecoder)
    {
        outError =
            "Development resource composition already has a source texture decoder";
        return false;
    }

    dependencies.textureSourceDecoder =
        std::make_unique<AssetImport::SourceTextureLoader>();

    dependencies.changeMonitor =
        std::make_unique<PollingResourceChangeMonitor>();
    outError.clear();
    return true;
}
} // namespace RVX::ResourceSource
