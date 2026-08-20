#pragma once

/** @file ResourceLoaderCatalog.h @brief Fail-closed runtime loader selection */

#include "ResourceRuntime/ResourceLoader.h"

#include <memory>
#include <string>
#include <vector>

namespace RVX::Resource
{
    enum class ResourceLoaderResolveStatus : uint8
    {
        Success = 0,
        InvalidRequest,
        NotFound,
        Ambiguous,
        MatcherFailure,
    };

    struct ResourceLoaderResolveResult
    {
        std::shared_ptr<IResourceLoader> loader;
        ResourceLoaderResolveStatus status =
            ResourceLoaderResolveStatus::InvalidRequest;
        size_t matchCount = 0;
        std::string diagnostic;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return status == ResourceLoaderResolveStatus::Success &&
                loader != nullptr;
        }
    };

    class ResourceLoaderCatalog
    {
    public:
        ResourceLoaderCatalog() = default;
        ~ResourceLoaderCatalog() = default;

        ResourceLoaderCatalog(const ResourceLoaderCatalog&) = delete;
        ResourceLoaderCatalog& operator=(const ResourceLoaderCatalog&) = delete;
        ResourceLoaderCatalog(ResourceLoaderCatalog&&) noexcept = default;
        ResourceLoaderCatalog& operator=(ResourceLoaderCatalog&&) noexcept = default;

        [[nodiscard]] bool Add(
            std::unique_ptr<IResourceLoader> loader,
            std::string& outError);

        [[nodiscard]] ResourceLoaderResolveResult Resolve(
            const ResourceLoaderRequest& request) const;

        [[nodiscard]] IResourceLoader* FindOnlyByType(
            ResourceType type) const;

        void Clear() noexcept { m_loaders.clear(); }
        [[nodiscard]] size_t Size() const noexcept { return m_loaders.size(); }

    private:
        std::vector<std::shared_ptr<IResourceLoader>> m_loaders;
    };
} // namespace RVX::Resource

namespace RVX
{
    using Resource::ResourceLoaderCatalog;
    using Resource::ResourceLoaderResolveResult;
    using Resource::ResourceLoaderResolveStatus;
} // namespace RVX
