#pragma once

/**
 * @file SampleModelLoader.h
 * @brief Thin sample adapter for the production model-to-scene resource path.
 */

#include "Resource/ResourceHandle.h"
#include "Resource/Types/ModelResource.h"
#include "ResourceSceneAdapters/SceneAssetLoadCoordinator.h"

#include <filesystem>
#include <string>

namespace RVX
{
    class SceneEntity;
    class Scene;

    namespace Resource
    {
        class ResourceManager;
        class ResourceSubsystem;
    }

    struct LoadedSampleModel
    {
        Resource::ResourceHandle<Resource::ModelResource> resource;
        SceneAssetInstance instance;
        SceneAssetLoadHandle loadHandle = InvalidSceneAssetLoadHandle;
        SceneAssetStatus status;
        std::filesystem::path sourcePath;

        [[nodiscard]] SceneEntity* ResolveRoot(Scene& scene) const;
        [[nodiscard]] bool IsCPUReady() const noexcept
        {
            return status.IsActive() &&
                   status.residency >= SceneAssetResidency::CPUReady;
        }
        [[nodiscard]] bool IsFullyResident() const noexcept
        {
            return status.IsFullyResident();
        }
    };

    /**
     * @brief Validates a concrete path and invokes ResourceManager ->
     * ModelResource -> ResourceSceneAdapters -> Scene without fallback.
     */
    class SampleModelLoader final
    {
    public:
        explicit SampleModelLoader(
            SceneAssetLoadCoordinator& coordinator) noexcept;

        bool Request(const std::filesystem::path& path,
                     LoadedSampleModel& outModel,
                     std::string& outError,
                     bool activateWhenResident = true) const;

        bool Instantiate(
            const Resource::ResourceHandle<Resource::ModelResource>& resource,
            const std::filesystem::path& sourcePath,
            LoadedSampleModel& outModel,
            std::string& outError,
            bool activateWhenResident = true) const;

        [[nodiscard]] SceneAssetStatus UpdateReadiness(
            LoadedSampleModel& model) const;

        [[nodiscard]] bool Cancel(LoadedSampleModel& model) const;

    private:
        SceneAssetLoadCoordinator& m_coordinator;
    };
} // namespace RVX
