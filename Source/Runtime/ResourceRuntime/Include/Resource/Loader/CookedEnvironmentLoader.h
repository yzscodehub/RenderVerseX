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
        bool Supports(
            const ResourceLoaderRequest& request) const noexcept override;
        bool Prepare(const ResourceLoadPreparationContext& context,
                     PreparedResourceBundle& outBundle,
                     ResourceLoadError& outError) override;
    };
} // namespace RVX::Resource
