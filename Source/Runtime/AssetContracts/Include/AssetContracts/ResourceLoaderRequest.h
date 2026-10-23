#pragma once

/** @file ResourceLoaderRequest.h @brief Exact loader selection request */

#include "AssetContracts/ResourceTypes.h"

#include <string>

namespace RVX::Resource
{
    struct ResourceLoaderRequest
    {
        ResourceType type = ResourceType::Unknown;
        ResourceLoadDomain domain = ResourceLoadDomain::Unknown;
        std::string resolvedPath;
    };
} // namespace RVX::Resource

namespace RVX
{
    using Resource::ResourceLoaderRequest;
} // namespace RVX
