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

        /** @brief Freeze registration before the catalog becomes observable. */
        [[nodiscard]] bool Seal(std::string& outError) noexcept;

        [[nodiscard]] ResourceLoaderResolveResult Resolve(
            const ResourceLoaderRequest& request) const;

        void Clear() noexcept
        {
            m_loaders.clear();
            m_sealed = false;
        }
        [[nodiscard]] size_t Size() const noexcept { return m_loaders.size(); }
        [[nodiscard]] bool IsSealed() const noexcept { return m_sealed; }

    private:
        std::vector<std::shared_ptr<IResourceLoader>> m_loaders;
        bool m_sealed = false;
    };
} // namespace RVX::Resource

namespace RVX
{
    using Resource::ResourceLoaderCatalog;
    using Resource::ResourceLoaderResolveResult;
    using Resource::ResourceLoaderResolveStatus;
} // namespace RVX
