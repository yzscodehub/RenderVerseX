#pragma once

/** @file CookedEnvironmentLoader.h @brief Cooked/package IBL loader */

#include "Resource/ResourceManager.h"
#include "Resource/Types/EnvironmentResource.h"

namespace RVX::Resource
{
    class CookedEnvironmentLoader final : public IResourceLoader
    {
    public:
        ResourceType GetResourceType() const override
        {
            return ResourceType::Environment;
        }
        std::vector<std::string> GetSupportedExtensions() const override;
        IResource* Load(const std::string& path) override;
        bool Prepare(const ResourceLoadPreparationContext& context,
                     PreparedResourceBundle& outBundle,
                     ResourceLoadError& outError) override;
        bool SupportsPreparedLoading() const override { return true; }
        bool CanLoad(const std::string& path) const override;
        bool Supports(const ResourceLoaderRequest& request) const override;
    };
} // namespace RVX::Resource
