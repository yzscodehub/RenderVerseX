#pragma once

/** @file ResourceLoaderDomainPolicy.h @brief Source/runtime loader partition */

#include "AssetContracts/ResourceTypes.h"

namespace RVX::Resource
{
    enum class ResourceLoaderDomainPolicy : uint8
    {
        SourceAndRuntime = 0,
        SourceOnly,
        RuntimeOnly,
    };

    [[nodiscard]] constexpr bool SupportsResourceLoadDomain(
        ResourceLoaderDomainPolicy policy,
        ResourceLoadDomain domain) noexcept
    {
        switch (policy)
        {
            case ResourceLoaderDomainPolicy::SourceAndRuntime:
                return domain == ResourceLoadDomain::Source ||
                    domain == ResourceLoadDomain::Cooked ||
                    domain == ResourceLoadDomain::Package;
            case ResourceLoaderDomainPolicy::SourceOnly:
                return domain == ResourceLoadDomain::Source;
            case ResourceLoaderDomainPolicy::RuntimeOnly:
                return domain == ResourceLoadDomain::Cooked ||
                    domain == ResourceLoadDomain::Package;
            default:
                return false;
        }
    }
} // namespace RVX::Resource

namespace RVX
{
    using Resource::ResourceLoaderDomainPolicy;
} // namespace RVX
