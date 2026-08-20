#pragma once

/** @file ResourceSourceComposition.h @brief Development source dependencies */

#include "ResourceRuntime/ResourceManagerDependencies.h"

#include <string>

namespace RVX::Resource
{
    class ResourceManager;
}

namespace RVX::ResourceSource
{
    /**
     * @brief Add source-only Texture/Model importers and one change monitor.
     *
     * Existing cooked/package loaders remain untouched. The call fails rather
     * than replacing a monitor or accepting an ambiguous loader catalog.
     */
    [[nodiscard]] bool AddSourceResourceDependencies(
        ResourceManagerDependencies& dependencies,
        Resource::ResourceManager& manager,
        std::string& outError);
} // namespace RVX::ResourceSource
