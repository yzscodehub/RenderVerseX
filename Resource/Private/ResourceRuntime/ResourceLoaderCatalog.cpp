/** @file ResourceLoaderCatalog.cpp @brief Deterministic loader selection */

#include "ResourceRuntime/ResourceLoaderCatalog.h"

#include <exception>
#include <utility>

namespace RVX::Resource
{
bool ResourceLoaderCatalog::Add(
    std::unique_ptr<IResourceLoader> loader,
    std::string& outError)
{
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

ResourceLoaderResolveResult ResourceLoaderCatalog::Resolve(
    const ResourceLoaderRequest& request) const
{
    ResourceLoaderResolveResult result;
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
        bool supports = false;
        try
        {
            supports = loader && loader->Supports(request);
        }
        catch (const std::exception& error)
        {
            result.loader.reset();
            result.status = ResourceLoaderResolveStatus::MatcherFailure;
            result.diagnostic =
                "Resource loader threw while matching request: " +
                std::string(error.what());
            return result;
        }
        catch (...)
        {
            result.loader.reset();
            result.status = ResourceLoaderResolveStatus::MatcherFailure;
            result.diagnostic =
                "Resource loader threw while matching request";
            return result;
        }
        if (!supports)
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

IResourceLoader* ResourceLoaderCatalog::FindOnlyByType(
    ResourceType type) const
{
    IResourceLoader* result = nullptr;
    for (const std::shared_ptr<IResourceLoader>& loader : m_loaders)
    {
        if (!loader || loader->GetResourceType() != type)
        {
            continue;
        }
        if (result != nullptr)
        {
            return nullptr;
        }
        result = loader.get();
    }
    return result;
}
} // namespace RVX::Resource
