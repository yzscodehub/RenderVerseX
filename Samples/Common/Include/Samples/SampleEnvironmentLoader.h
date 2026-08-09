#pragma once

/**
 * @file SampleEnvironmentLoader.h
 * @brief Backend-neutral sample glue for loading and publishing environment IBL.
 */

#include "Core/Types.h"
#include "Resource/Types/TextureResource.h"
#include "Resource/Types/EnvironmentResource.h"
#include "Resource/ResourceHandle.h"
#include "ResourceSceneAdapters/SceneAssetLoadCoordinator.h"

#include <filesystem>
#include <string>

namespace RVX
{
    class SkyboxComponent;

    namespace Resource
    {
        class ResourceManager;
        class ResourceSubsystem;
    }

    struct SampleEnvironmentLoadOptions
    {
        std::string quality = "default";
        bool smoke = false;
        float32 exposure = 1.0f;
    };

    /** @brief Strong handles retained for one loaded sample environment. */
    struct LoadedSampleEnvironment
    {
        std::filesystem::path sourcePath;
        Resource::EnvironmentHandle resource;
        Resource::TextureHandle environment;
        Resource::TextureHandle irradiance;
        Resource::TextureHandle prefiltered;
        Resource::TextureHandle brdfLUT;
        SceneAssetLoadHandle loadHandle = InvalidSceneAssetLoadHandle;
        SceneAssetStatus status;
        uint32 environmentResolution = 0;
        uint32 irradianceResolution = 0;
        uint32 prefilteredResolution = 0;
        uint32 prefilteredMipLevels = 0;
        uint32 brdfLUTResolution = 0;

        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] bool IsCPUReady() const noexcept;
    };

    /**
     * @brief Loads IBL through Resource and publishes it through ResourceSubsystem.
     *
     * This service does not expose RHI devices, command lists, RenderGraph, or
     * backend identity to sample scenes.
     */
    class SampleEnvironmentLoader final
    {
    public:
        explicit SampleEnvironmentLoader(
            SceneAssetLoadCoordinator& coordinator) noexcept;

        bool Request(const std::filesystem::path& path,
                     const SampleEnvironmentLoadOptions& options,
                     SkyboxComponent& targetSkybox,
                     LoadedSampleEnvironment& output,
                     std::string& outError) const;

        [[nodiscard]] SceneAssetStatus UpdateReadiness(
            LoadedSampleEnvironment& environment) const;

        [[nodiscard]] bool Cancel(
            LoadedSampleEnvironment& environment) const;

    private:
        SceneAssetLoadCoordinator& m_coordinator;
    };
} // namespace RVX
