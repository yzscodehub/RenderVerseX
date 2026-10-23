#pragma once

/** @file CookedTextureLoader.h @brief Cooked/package texture loader */

#include "Resource/ResourceManager.h"
#include "Resource/Types/TextureResource.h"

#include <string>

namespace RVX::Resource
{
    class CookedTextureLoader final : public IResourceLoader
    {
    public:
        ResourceType GetResourceType() const override
        {
            return ResourceType::Texture;
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
