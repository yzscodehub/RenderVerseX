#pragma once

/** @file CookedAudioLoader.h @brief Cooked/package PCM16 audio loader */

#include "Resource/ResourceManager.h"
#include "Resource/Types/AudioResource.h"

namespace RVX::Resource
{
    class CookedAudioLoader final : public IResourceLoader
    {
    public:
        explicit CookedAudioLoader(ResourceManager* manager)
            : m_manager(manager)
        {
        }

        ResourceType GetResourceType() const override
        {
            return ResourceType::Audio;
        }
        std::vector<std::string> GetSupportedExtensions() const override;
        IResource* Load(const std::string& path) override;
        bool Prepare(const ResourceLoadPreparationContext& context,
                     PreparedResourceBundle& outBundle,
                     ResourceLoadError& outError) override;
        bool SupportsPreparedLoading() const override { return true; }
        bool CanLoad(const std::string& path) const override;
        bool Supports(const ResourceLoaderRequest& request) const override;

    private:
        ResourceManager* m_manager = nullptr;
    };
} // namespace RVX::Resource
