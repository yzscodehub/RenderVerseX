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
        explicit ShaderLoader(ResourceManager* manager);
        ~ShaderLoader() override = default;

        ResourceType GetResourceType() const override { return ResourceType::Shader; }
        std::vector<std::string> GetSupportedExtensions() const override;
        IResource* Load(const std::string& path) override;
        bool CanLoad(const std::string& path) const override;

        const std::string& GetLastLoadError() const { return m_lastLoadError; }

    private:
        ResourceManager* m_manager = nullptr;
        std::string m_lastLoadError;
    };
} // namespace RVX::Resource
