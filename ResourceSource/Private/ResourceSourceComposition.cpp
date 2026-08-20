/** @file ResourceSourceComposition.cpp @brief Development source dependencies */

#include "ResourceSource/ResourceSourceComposition.h"

#include "AssetImport/SourceResourceLoaders.h"
#include "Resource/ResourceManager.h"
#include "ResourceSource/PollingResourceChangeMonitor.h"

#include <memory>

namespace RVX::ResourceSource
{
bool AddSourceResourceDependencies(
    ResourceManagerDependencies& dependencies,
    Resource::ResourceManager& manager,
    std::string& outError)
{
    if (dependencies.changeMonitor)
    {
        outError =
            "Development resource composition already has a change monitor";
        return false;
    }

    if (!dependencies.loaderCatalog.Add(
            std::make_unique<AssetImport::SourceTextureLoader>(&manager),
            outError))
    {
        return false;
    }
    if (!dependencies.loaderCatalog.Add(
            std::make_unique<AssetImport::SourceModelLoader>(&manager),
            outError))
    {
        return false;
    }

    dependencies.changeMonitor =
        std::make_unique<PollingResourceChangeMonitor>();
    outError.clear();
    return true;
}
} // namespace RVX::ResourceSource
