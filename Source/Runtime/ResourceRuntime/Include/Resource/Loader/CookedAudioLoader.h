#pragma once

/** @file CookedAudioLoader.h @brief Cooked/package PCM16 audio loader */

#include "Resource/ResourceManager.h"
#include "Resource/Types/AudioResource.h"

namespace RVX::Resource
{
    class CookedAudioLoader final : public IResourceLoader
    {
    public:
        ResourceType GetResourceType() const override
        {
            return ResourceType::Audio;
        }
        std::vector<std::string> GetSupportedExtensions() const override;
        bool Supports(
            const ResourceLoaderRequest& request) const noexcept override;
        bool Prepare(const ResourceLoadPreparationContext& context,
                     PreparedResourceBundle& outBundle,
                     ResourceLoadError& outError) override;
    };
} // namespace RVX::Resource
