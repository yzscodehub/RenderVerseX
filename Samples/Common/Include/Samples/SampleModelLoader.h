#pragma once

/**
 * @file SampleModelLoader.h
 * @brief Thin sample adapter for the production model-to-scene resource path.
 */

#include "Resource/ResourceHandle.h"
#include "Resource/Types/ModelResource.h"
#include "ResourceSceneAdapters/SceneAssetInstantiation.h"

#include <filesystem>
#include <string>

namespace RVX
{
    class SceneEntity;
    class Scene;

    namespace Resource
    {
        class ResourceManager;
    }

    struct LoadedSampleModel
    {
        Resource::ResourceHandle<Resource::ModelResource> resource;
        SceneAssetInstance instance;
        std::filesystem::path sourcePath;

        [[nodiscard]] SceneEntity* ResolveRoot(Scene& scene) const;
    };

    /**
     * @brief Validates a concrete path and invokes ResourceManager ->
     * ModelResource -> ResourceSceneAdapters -> Scene without fallback.
     */
    class SampleModelLoader final
    {
    public:
        explicit SampleModelLoader(Resource::ResourceManager& resources) noexcept;

        bool Load(const std::filesystem::path& path,
                  Scene& scene,
                  LoadedSampleModel& outModel,
                  std::string& outError) const;

        bool Instantiate(
            const Resource::ResourceHandle<Resource::ModelResource>& resource,
            const std::filesystem::path& sourcePath,
            Scene& scene,
            LoadedSampleModel& outModel,
            std::string& outError) const;

    private:
        Resource::ResourceManager& m_resources;
    };
} // namespace RVX
