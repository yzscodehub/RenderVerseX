#pragma once

/**
 * @file MeshLoader.h
 * @brief Runtime loader for cooked mesh artifacts
 */

#include "Resource/ResourceManager.h"
#include "Resource/Types/MeshResource.h"

#include <string>
#include <vector>

namespace RVX::Resource
{
    /**
     * @brief Load cooked RenderVerseX mesh artifacts.
     */
    class MeshLoader : public IResourceLoader
    {
    public:
        explicit MeshLoader(ResourceManager* manager);
        ~MeshLoader() override = default;

        ResourceType GetResourceType() const override { return ResourceType::Mesh; }
        std::vector<std::string> GetSupportedExtensions() const override;
        IResource* Load(const std::string& path) override;
        bool CanLoad(const std::string& path) const override;

        const std::string& GetLastLoadError() const { return m_lastLoadError; }

    private:
        ResourceManager* m_manager = nullptr;
        std::string m_lastLoadError;
    };
} // namespace RVX::Resource
