#pragma once

/** @file ResourceManagerDependencies.h @brief Move-only runtime composition */

#include "ResourceRuntime/ResourceLoaderCatalog.h"

namespace RVX::Resource
{
    struct ResourceManagerDependencies
    {
        ResourceLoaderCatalog loaderCatalog;

        ResourceManagerDependencies() = default;
        ~ResourceManagerDependencies() = default;
        ResourceManagerDependencies(const ResourceManagerDependencies&) = delete;
        ResourceManagerDependencies& operator=(const ResourceManagerDependencies&) = delete;
        ResourceManagerDependencies(ResourceManagerDependencies&&) noexcept = default;
        ResourceManagerDependencies& operator=(ResourceManagerDependencies&&) noexcept = default;
    };
} // namespace RVX::Resource

namespace RVX
{
    using Resource::ResourceManagerDependencies;
} // namespace RVX
