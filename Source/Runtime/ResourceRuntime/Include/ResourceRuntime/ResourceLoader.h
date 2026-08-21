#pragma once

/** @file ResourceLoader.h @brief Runtime resource loader contract */

#include "AssetContracts/ResourceLoaderRequest.h"
#include "Core/Types.h"

#include <memory>
#include <string>
#include <vector>

namespace RVX::Resource
{
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

        /** @brief Match an exact type/domain/path without I/O or side effects. */
        virtual bool Supports(
            const ResourceLoaderRequest& request) const noexcept = 0;

        /**
         * @brief Build a complete unpublished CPU bundle without manager access.
         *
         * Implementations must observe cancellation, remain safe for concurrent
         * requests, and leave no publishable partial state when returning false.
         * Validation, cache mutation, registry mutation, and lifecycle callbacks
         * are exclusively ResourceManager responsibilities.
         */
        virtual bool Prepare(
            const ResourceLoadPreparationContext& context,
            PreparedResourceBundle& outBundle,
            ResourceLoadError& outError) = 0;

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

    };
} // namespace RVX::Resource

namespace RVX
{
    using Resource::IResourceLoader;
    using Resource::ResourceLoadPreparationStateRef;
} // namespace RVX
