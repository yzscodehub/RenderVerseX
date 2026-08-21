#pragma once

/** @file AnimationLoader.h  @brief Loader for deterministic .rvxanim artifacts. */

#include "Resource/ResourceManager.h"
#include "Resource/Types/AnimationResource.h"

#include <string>
#include <vector>

namespace RVX::Resource
{
    class AnimationLoader final : public IResourceLoader
    {
    public:
        ResourceType GetResourceType() const override
        {
            return AnimationResource::StaticResourceType;
        }
        std::vector<std::string> GetSupportedExtensions() const override;
        bool Supports(
            const ResourceLoaderRequest& request) const noexcept override;
        bool Prepare(const ResourceLoadPreparationContext& context,
                     PreparedResourceBundle& outBundle,
                     ResourceLoadError& outError) override;
    };
} // namespace RVX::Resource
