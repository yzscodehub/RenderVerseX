#pragma once

/** @file ResourceManagerDependencies.h @brief Move-only runtime composition */

#include "ResourceRuntime/ResourceChangeMonitor.h"
#include "ResourceRuntime/ResourceLoaderCatalog.h"
#include "ResourceRuntime/TextureSourceDecode.h"

#include <memory>

namespace RVX::Resource
{
    struct ResourceManagerDependencies
    {
        ResourceLoaderCatalog loaderCatalog;
        std::unique_ptr<IResourceChangeMonitor> changeMonitor;
        std::unique_ptr<ITextureSourceDecoder> textureSourceDecoder;

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
