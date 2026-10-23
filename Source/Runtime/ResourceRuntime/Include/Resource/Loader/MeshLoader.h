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
        MeshLoader() = default;
        ~MeshLoader() override = default;

        ResourceType GetResourceType() const override { return ResourceType::Mesh; }
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
