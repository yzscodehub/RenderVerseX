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
        IResource* Load(const std::string& path) override;
        bool Prepare(const ResourceLoadPreparationContext& context,
                     PreparedResourceBundle& outBundle,
                     ResourceLoadError& outError) override;
        bool SupportsPreparedLoading() const override { return true; }
        bool CanLoad(const std::string& path) const override;
    };
} // namespace RVX::Resource
