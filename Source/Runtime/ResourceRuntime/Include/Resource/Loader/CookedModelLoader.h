#pragma once

/** @file CookedModelLoader.h @brief Cooked/package model loader */

#include "Resource/ResourceManager.h"
#include "Resource/Types/ModelResource.h"

#include <string>

namespace RVX::Resource
{
    class CookedModelLoader final : public IResourceLoader
    {
    public:
        ResourceType GetResourceType() const override
        {
            return ResourceType::Model;
        }
        std::vector<std::string> GetSupportedExtensions() const override;
        bool Supports(
            const ResourceLoaderRequest& request) const noexcept override;
        bool Prepare(const ResourceLoadPreparationContext& context,
                     PreparedResourceBundle& outBundle,
                     ResourceLoadError& outError) override;

        [[nodiscard]] const std::string& GetLastLoadError() const noexcept
        {
            return m_lastLoadError;
        }

    private:
        std::string m_lastLoadError;
    };
} // namespace RVX::Resource
