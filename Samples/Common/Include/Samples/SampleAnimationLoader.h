#pragma once

/**
 * @file SampleAnimationLoader.h
 * @brief Catalog-gated pure-ECS animation request adapter for sample scenes.
 */

#include "AnimationSceneAdapters/ECS/EcsAnimationAssetService.h"
#include "Engine/ECS/IWorldEcsRuntimeServices.h"
#include "Samples/SampleContext.h"
#include "Scene/ECS/SceneEcsRuntime.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace RVX
{
    /** @brief Value-only state retained by one sample animation request. */
    struct LoadedSampleAnimation
    {
        std::filesystem::path sourcePath;
        AnimationSceneAdapters::EcsAnimationAssetLoadRef request;
        AnimationSceneAdapters::EcsAnimationAssetLoadStatus status;

        [[nodiscard]] bool IsReady() const noexcept
        {
            return request.IsValid() &&
                   status.state ==
                       AnimationSceneAdapters::EcsAnimationAssetLoadState::Ready &&
                   status.sceneRuntimeId == request.sceneRuntimeId &&
                   status.animationAssetValue != 0 &&
                   status.contentVerification.IsVerified();
        }
    };

    /**
     * @brief Requests runner-admitted animations for one exact ECS Scene runtime.
     *
     * Samples retain only a Scene-qualified generation handle and copied status
     * values. Resource ownership, immutable evaluator leases, and target binding
     * mutation remain behind Engine's per-World service boundary.
     */
    class SampleAnimationLoader final
    {
    public:
        SampleAnimationLoader(IWorldEcsRuntimeServices& runtimeServices,
                              ECS::SceneRuntimeId sceneRuntimeId,
                              SampleAssetRegistry assetRegistry);

        [[nodiscard]] bool RequestByAssetId(
            std::string_view assetId,
            LoadedSampleAnimation& output,
            std::string& outError,
            SampleAssetRegistryLookupResult* outLookup = nullptr) const;

        [[nodiscard]] AnimationSceneAdapters::EcsAnimationAssetLoadStatus
        UpdateReadiness(LoadedSampleAnimation& animation) const;

        /** @brief Cancel exactly this Scene-qualified request; clear it only on success. */
        [[nodiscard]] bool Cancel(LoadedSampleAnimation& animation) const;

        /**
         * @brief Prove exact skeleton compatibility and bind the selected clip.
         *
         * The Engine service validates that targetEntity is Alive in this Scene,
         * resolves both immutable skeleton resources internally, and atomically
         * replaces only the target AnimationSkeletonBinding fragment.
         */
        [[nodiscard]] AnimationSceneAdapters::EcsAnimationBindingPreparationResult
        PrepareCompatibleBinding(
            const LoadedSampleAnimation& animation,
            SceneECS::SceneEntityRef targetEntity,
            uint32 animationClipOrdinal = 0) const;

        [[nodiscard]] std::optional<Resource::ResourceContentVerificationReceipt>
        GetContentVerificationReceipt(const std::filesystem::path& path) const;

    private:
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
