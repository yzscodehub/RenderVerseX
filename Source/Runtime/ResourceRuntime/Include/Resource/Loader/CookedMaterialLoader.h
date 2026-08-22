#pragma once

/** @file CookedMaterialLoader.h @brief Cooked/package material loader */

#include "Resource/ResourceManager.h"

namespace RVX::Resource
{
    class CookedMaterialLoader final : public IResourceLoader
    {
    public:
        ResourceType GetResourceType() const override
        {
            return ResourceType::Material;
        }
        std::vector<std::string> GetSupportedExtensions() const override;
        bool Supports(
            const ResourceLoaderRequest& request) const noexcept override;
        bool Prepare(const ResourceLoadPreparationContext& context,
                     PreparedResourceBundle& outBundle,
                     ResourceLoadError& outError) override;
    };
} // namespace RVX::Resource
