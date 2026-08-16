#pragma once

/**
 * @file SampleEnvironmentLoader.h
 * @brief Value-only sample glue for ECS-owned environment IBL loading.
 */

#include "ECS/Entity.h"
#include "Engine/ECS/IWorldEcsRuntimeServices.h"
#include "Resource/ResourceContentIdentity.h"
#include "ResourceSceneAdapters/ECS/EcsEnvironmentLoadCoordinator.h"
#include "Samples/SampleContext.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace RVX
{
    struct SampleEnvironmentLoadOptions
    {
        std::string quality = "default";
        bool smoke = false;
        float32 exposure = 1.0f;
    };

    /** @brief Value-owned status for one environment whose Skybox is ECS-owned. */
    struct LoadedSampleEnvironment
    {
        std::filesystem::path sourcePath;
        ResourceSceneAdapters::EcsEnvironmentLoadRef request;
        ResourceSceneAdapters::EcsEnvironmentLoadStatus status;
        std::optional<Resource::ResourceContentVerificationReceipt>
            contentVerificationReceipt;
        uint32 environmentResolution = 0;
        uint32 irradianceResolution = 0;
        uint32 prefilteredResolution = 0;
        uint32 prefilteredMipLevels = 0;
        uint32 brdfLUTResolution = 0;
        float32 exposure = 1.0f;

        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] bool IsCPUReady() const noexcept;
    };

    /**
     * @brief Catalog-gated Environment requests for one exact ECS Scene runtime.
     *
     * The coordinator creates, publishes, retires, and recycles its dedicated
     * Skybox entity. Samples retain only the request identity and immutable
     * published values; they cannot obtain Resource handles or mutate Skybox
     * fragments directly.
     */
    class SampleEnvironmentLoader final
    {
    public:
        SampleEnvironmentLoader(IWorldEcsRuntimeServices& runtimeServices,
                                ECS::SceneRuntimeId sceneRuntimeId) noexcept;

        SampleEnvironmentLoader(IWorldEcsRuntimeServices& runtimeServices,
                                ECS::SceneRuntimeId sceneRuntimeId,
                                SampleAssetRegistry assetRegistry);

        /**
         * @brief Request a runner-validated catalog environment by logical id.
         *
         * Unknown and wrong-kind ids return a typed lookup result without
         * creating an ECS environment request.
         */
        bool RequestByAssetId(
            std::string_view assetId,
            const SampleEnvironmentLoadOptions& options,
            LoadedSampleEnvironment& output,
            std::string& outError,
            SampleAssetRegistryLookupResult* outLookup = nullptr) const;

        /** @brief Request an HDR/EXR path only when its exact content identity is supplied. */
        bool Request(const std::filesystem::path& path,
                     const Resource::ResourceContentIdentity& contentIdentity,
                     const SampleEnvironmentLoadOptions& options,
                     LoadedSampleEnvironment& output,
                     std::string& outError) const;

        [[nodiscard]] ResourceSceneAdapters::EcsEnvironmentLoadStatus
        UpdateReadiness(LoadedSampleEnvironment& environment) const;

        [[nodiscard]] bool Cancel(LoadedSampleEnvironment& environment) const;

        [[nodiscard]] std::optional<Resource::ResourceContentVerificationReceipt>
        GetContentVerificationReceipt(const std::filesystem::path& path) const;

    private:
        bool RequestResolved(
            const std::filesystem::path& path,
            const Resource::ResourceContentIdentity& expectedContentIdentity,
            const SampleEnvironmentLoadOptions& options,
            LoadedSampleEnvironment& output,
            std::string& outError) const;

        [[nodiscard]] static std::string MakeLogicalPathKey(
            const std::filesystem::path& path);

        IWorldEcsRuntimeServices& m_runtimeServices;
        ECS::SceneRuntimeId m_sceneRuntimeId;
        SampleAssetRegistry m_assetRegistry;
        mutable std::map<std::string,
                         Resource::ResourceContentVerificationReceipt,
                         std::less<>> m_contentVerificationReceipts;
    };
} // namespace RVX
