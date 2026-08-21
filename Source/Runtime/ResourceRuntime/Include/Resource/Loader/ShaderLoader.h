#pragma once

/**
 * @file ShaderLoader.h
 * @brief Runtime loader for cooked shader artifacts
 */

#include "Resource/ResourceManager.h"
#include "Resource/Types/ShaderResource.h"

#include <string>
#include <vector>

namespace RVX::Resource
{
    /**
     * @brief Load cooked RenderVerseX shader artifacts.
     */
    class ShaderLoader : public IResourceLoader
    {
    public:
        ShaderLoader() = default;
        ~ShaderLoader() override = default;

        ResourceType GetResourceType() const override { return ResourceType::Shader; }
        std::vector<std::string> GetSupportedExtensions() const override;
        bool Supports(
            const ResourceLoaderRequest& request) const noexcept override;
        bool Prepare(const ResourceLoadPreparationContext& context,
                     PreparedResourceBundle& outBundle,
                     ResourceLoadError& outError) override;

        const std::string& GetLastLoadError() const { return m_lastLoadError; }

    private:
        std::string m_lastLoadError;
    };
} // namespace RVX::Resource
