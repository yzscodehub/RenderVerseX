/** @file ResourceLoaderCatalog.cpp @brief Deterministic loader selection */

#include "ResourceRuntime/ResourceLoaderCatalog.h"

#include <utility>

namespace RVX::Resource
{
bool ResourceLoaderCatalog::Add(
    std::unique_ptr<IResourceLoader> loader,
    std::string& outError)
{
    if (m_sealed)
    {
        outError = "Cannot register a resource loader after the catalog is sealed";
        return false;
    }
    if (!loader)
    {
        outError = "Cannot register a null resource loader";
        return false;
    }
    if (loader->GetResourceType() == ResourceType::Unknown)
    {
        outError = "Resource loader must declare a concrete resource type";
        return false;
    }
    m_loaders.emplace_back(std::move(loader));
    outError.clear();
    return true;
}

bool ResourceLoaderCatalog::Seal(std::string& outError) noexcept
{
    if (m_sealed)
    {
        outError = "Resource loader catalog is already sealed";
        return false;
    }
    m_sealed = true;
    outError.clear();
    return true;
}

ResourceLoaderResolveResult ResourceLoaderCatalog::Resolve(
    const ResourceLoaderRequest& request) const
{
    ResourceLoaderResolveResult result;
    if (!m_sealed)
    {
        result.diagnostic =
            "Resource loader catalog must be sealed before resolution";
        return result;
    }
    if (request.type == ResourceType::Unknown ||
        request.domain == ResourceLoadDomain::Unknown ||
        request.resolvedPath.empty())
    {
        result.diagnostic =
            "Resource loader request requires type, domain, and resolved path";
        return result;
    }

    for (const std::shared_ptr<IResourceLoader>& loader : m_loaders)
    {
        if (!loader || !loader->Supports(request))
        {
            continue;
        }
        ++result.matchCount;
        if (result.matchCount == 1)
        {
            result.loader = loader;
        }
    }

    if (result.matchCount == 0)
    {
        result.status = ResourceLoaderResolveStatus::NotFound;
        result.diagnostic = "No resource loader matched the exact request";
        return result;
    }
    if (result.matchCount != 1)
    {
        result.loader.reset();
        result.status = ResourceLoaderResolveStatus::Ambiguous;
        result.diagnostic =
            "Multiple resource loaders matched the exact request";
        return result;
    }
    result.status = ResourceLoaderResolveStatus::Success;
    result.diagnostic.clear();
    return result;
}
} // namespace RVX::Resource
