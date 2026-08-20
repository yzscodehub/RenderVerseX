#pragma once

/** @file ResourceLoader.h @brief Runtime resource loader contract */

#include "AssetContracts/ResourceLoaderRequest.h"
#include "Core/Types.h"

#include <memory>
#include <string>
#include <vector>

namespace RVX::Resource
{
    class IResource;
    class ResourceLoadPreparationState;
    class PreparedResourceBundle;
    struct ResourceLoadError;
    struct ResourceLoadPreparationContext;

    using ResourceLoadPreparationStateRef =
        std::shared_ptr<const ResourceLoadPreparationState>;

    class IResourceLoader
    {
    public:
        virtual ~IResourceLoader() = default;

        virtual ResourceType GetResourceType() const = 0;
        virtual std::vector<std::string> GetSupportedExtensions() const = 0;
        virtual IResource* Load(const std::string& path) = 0;

        virtual bool Prepare(
            const ResourceLoadPreparationContext& context,
            PreparedResourceBundle& outBundle,
            ResourceLoadError& outError);

        virtual bool SupportsPreparedLoading() const { return false; }

        virtual bool CapturePreparationState(
            uint64 requestedImportOptionsHash,
            ResourceLoadPreparationStateRef& outState,
            uint64& outCanonicalImportOptionsHash,
            ResourceLoadError& outError) const;

        virtual bool ValidatePreparationState(
            uint64 requestedImportOptionsHash,
            ResourceLoadPreparationStateRef suppliedState,
            ResourceLoadPreparationStateRef& outState,
            uint64& outCanonicalImportOptionsHash,
            ResourceLoadError& outError) const;

        virtual bool CanLoad(const std::string& path) const;

        /** @brief Match one exact type/domain/path request. */
        virtual bool Supports(const ResourceLoaderRequest& request) const
        {
            return request.type == GetResourceType() &&
                CanLoad(request.resolvedPath);
        }
    };
} // namespace RVX::Resource

namespace RVX
{
    using Resource::IResourceLoader;
    using Resource::ResourceLoadPreparationStateRef;
} // namespace RVX
