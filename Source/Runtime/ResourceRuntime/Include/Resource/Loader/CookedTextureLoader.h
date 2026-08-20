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
        explicit CookedTextureLoader(ResourceManager* manager = nullptr)
            : m_manager(manager)
        {
        }

        ResourceType GetResourceType() const override
        {
            return ResourceType::Texture;
        }
        std::vector<std::string> GetSupportedExtensions() const override;
        TextureResource* Load(const std::string& path) override;
        bool Prepare(const ResourceLoadPreparationContext& context,
                     PreparedResourceBundle& outBundle,
                     ResourceLoadError& outError) override;
        bool SupportsPreparedLoading() const override { return true; }
        bool CanLoad(const std::string& path) const override;
        bool Supports(const ResourceLoaderRequest& request) const override;

        [[nodiscard]] const std::string& GetLastLoadError() const noexcept
        {
            return m_lastLoadError;
        }

    private:
        ResourceManager* m_manager = nullptr;
        std::string m_lastLoadError;
    };
} // namespace RVX::Resource
