#pragma once

/**
 * @file SampleModelLoader.h
 * @brief Catalog-gated pure-ECS model request adapter for sample scenes.
 */

#include "Core/Math/AABB.h"
#include "Engine/ECS/IWorldEcsRuntimeServices.h"
#include "Resource/ResourceContentIdentity.h"
#include "ResourceSceneAdapters/ECS/EcsSceneAssetLoadCoordinator.h"
#include "Samples/SampleContext.h"
#include "Scene/ECS/SceneEcsRuntime.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace RVX
{
    /** @brief Value-only state retained by one sample model request. */
    struct LoadedSampleModel
    {
        ResourceSceneAdapters::EcsModelAssetLoadRef request;
        Resource::ResourceLoadRequestId resourceRequestId;
        ResourceSceneAdapters::EcsSceneAssetLoadStatus status;
        std::filesystem::path sourcePath;

        /** @brief Pointer-free root identity from the latest status snapshot. */
        [[nodiscard]] SceneECS::SceneEntityRef GetRootEntityRef() const noexcept
        {
            return {.sceneRuntimeId = request.sceneRuntimeId, .entity = status.rootEntity};
        }

        [[nodiscard]] bool IsCPUReady() const noexcept;
        [[nodiscard]] bool IsFullyResident() const noexcept;
    };

    /**
     * @brief Compute an immediate model world bound from current local hierarchy data.
     *
     * This reads only the exact request member set and ancestor ParentRelation
     * chains. It deliberately does not use resolved RenderWorldTransform or
     * Visibility, so a CPU-hidden model can be placed before presentation.
     */
    [[nodiscard]] bool TryComputeSampleModelRenderableWorldBounds(
        const LoadedSampleModel& model,
        const SceneECS::SceneEcsRuntime& scene,
        AABB& outBounds);

    /** @brief One catalog-backed expectation keyed by a canonical model path. */
    struct SampleModelExpectedContentIdentity
    {
        std::filesystem::path path;
        Resource::ResourceContentIdentity identity;
    };

    /** @brief Value-only retirement tracking for one cancelled ECS model request. */
    struct SampleModelCancellation
    {
        ResourceSceneAdapters::EcsModelAssetLoadRef request;
        Resource::ResourceLoadRequestId resourceRequestId;
        bool requested = false;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return requested && request.IsValid();
        }
    };

    /**
     * @brief Validates catalog/path input and issues exact pure-ECS model requests.
     *
     * The adapter exposes only Scene-qualified generation handles and copied
     * status values. Resource ownership, ECS adoption, render proof, and
     * retirement remain inside Engine's World composition.
     */
    class SampleModelLoader final
    {
    public:
        SampleModelLoader(IWorldEcsRuntimeServices& runtimeServices,
                          const SceneECS::SceneEcsRuntime& scene) noexcept;

        SampleModelLoader(IWorldEcsRuntimeServices& runtimeServices,
                          const SceneECS::SceneEcsRuntime& scene,
                          std::vector<SampleModelExpectedContentIdentity>
                              expectedContentIdentities);

        SampleModelLoader(IWorldEcsRuntimeServices& runtimeServices,
                          const SceneECS::SceneEcsRuntime& scene,
                          SampleAssetRegistry assetRegistry);

        SampleModelLoader(IWorldEcsRuntimeServices& runtimeServices,
                          const SceneECS::SceneEcsRuntime& scene,
                          SampleAssetRegistry assetRegistry,
                          std::vector<SampleModelExpectedContentIdentity>
                              expectedContentIdentities);

        /**
         * @brief Request a runner-admitted model catalog entry.
         *
         * Unknown and wrong-kind ids are rejected before the Engine receives a
         * resource request.
         */
        [[nodiscard]] bool RequestByAssetId(
            std::string_view assetId,
            LoadedSampleModel& outModel,
            std::string& outError,
            SampleAssetRegistryLookupResult* outLookup = nullptr) const;

        /** @brief Request an explicit path under the caller's configured content proof. */
        [[nodiscard]] bool Request(const std::filesystem::path& path,
                                   LoadedSampleModel& outModel,
                                   std::string& outError) const;

        /**
         * @brief Create another ECS instance using the source request's exact descriptor.
         *
         * The source handle must still resolve in this exact Scene. The method
         * copies its immutable request options and performs no resource lookup
         * or pointer-based instantiation.
         */
        [[nodiscard]] bool RequestAdditionalInstance(
            const LoadedSampleModel& source,
            LoadedSampleModel& outModel,
            std::string& outError) const;

        /** @brief Refresh the copied owner-thread status for one live handle. */
        [[nodiscard]] ResourceSceneAdapters::EcsSceneAssetLoadStatus UpdateReadiness(
            LoadedSampleModel& model) const;

        [[nodiscard]] bool Cancel(LoadedSampleModel& model) const;

        /** @brief Cancel while retaining only value retirement evidence. */
        [[nodiscard]] bool CancelForRetirement(
            LoadedSampleModel& model,
            SampleModelCancellation& outCancellation) const;

        /** @brief True after the Engine released the exact retired request handle. */
        [[nodiscard]] bool IsRetirementComplete(
            const SampleModelCancellation& cancellation) const;

        [[nodiscard]] std::optional<ResourceSceneAdapters::EcsSceneAssetLoadStatus>
        QueryRetirementStatus(const SampleModelCancellation& cancellation) const;

        /** @brief Copy the exact content receipt recorded for the canonical source path. */
        [[nodiscard]] std::optional<Resource::ResourceContentVerificationReceipt>
        GetContentVerificationReceipt(const std::filesystem::path& path) const;

    private:
        [[nodiscard]] bool RequestResolved(
            const std::filesystem::path& path,
            Resource::ResourceLoadOptions resourceOptions,
            LoadedSampleModel& outModel,
            std::string& outError) const;

        [[nodiscard]] static std::string MakeLogicalPathKey(
            const std::filesystem::path& path);

        IWorldEcsRuntimeServices& m_runtimeServices;
        const SceneECS::SceneEcsRuntime& m_scene;
        SampleAssetRegistry m_assetRegistry;
        std::map<std::string,
                 Resource::ResourceContentIdentity,
                 std::less<>> m_expectedContentIdentities;
        mutable std::map<std::string,
                         Resource::ResourceContentVerificationReceipt,
                         std::less<>> m_contentVerificationReceipts;
    };
} // namespace RVX
