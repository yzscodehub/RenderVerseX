#pragma once

/**
 * @file SceneAssetInstantiation.h
 * @brief Transactional Resource-to-Scene model instantiation contract.
 */

#include "Core/Types.h"
#include "Resource/ResourceHandle.h"
#include "Resource/ResourceLoadOperation.h"
#include "Resource/Types/MaterialResource.h"
#include "Scene/Actor.h"

#include <string>
#include <vector>

namespace RVX
{
    class Scene;

    namespace Resource
    {
        class ModelResource;
        class ResourceSubsystem;
    }

    enum class SceneAssetLifecycle : uint8
    {
        Loading = 0,
        Active,
        Failed,
        Cancelled
    };

    enum class SceneAssetResidency : uint8
    {
        None = 0,
        CPUReady,
        MinimumResident,
        Streaming,
        FullyResident
    };

    /** @brief Orthogonal CPU/Scene lifetime and GPU residency state. */
    struct SceneAssetStatus
    {
        SceneAssetLifecycle lifecycle = SceneAssetLifecycle::Loading;
        SceneAssetResidency residency = SceneAssetResidency::None;
        float32 progress = 0.0f;
        uint64 revision = 0;
        Resource::ResourceLoadError error;
        std::string diagnostic;

        [[nodiscard]] bool IsActive() const noexcept
        {
            return lifecycle == SceneAssetLifecycle::Active;
        }

        [[nodiscard]] bool IsFailed() const noexcept
        {
            return lifecycle == SceneAssetLifecycle::Failed;
        }

        [[nodiscard]] bool IsMinimumResident() const noexcept
        {
            return IsActive() &&
                   residency >= SceneAssetResidency::MinimumResident;
        }

        [[nodiscard]] bool IsFullyResident() const noexcept
        {
            return IsActive() &&
                   residency == SceneAssetResidency::FullyResident;
        }
    };

    struct SceneAssetInstantiationOptions
    {
        std::vector<Resource::ResourceHandle<Resource::MaterialResource>>
            materialOverrides;
    };

    struct SceneAssetInstance
    {
        Actor::Handle rootActor = Actor::InvalidHandle;
        std::vector<Actor::Handle> actors;
        SceneAssetStatus status;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return rootActor.IsValid() &&
                   status.lifecycle == SceneAssetLifecycle::Active;
        }
        [[nodiscard]] bool IsRenderReady() const noexcept
        {
            return status.IsFullyResident();
        }
    };

    class SceneAssetInstantiator final
    {
    public:
        [[nodiscard]] static SceneAssetInstance InstantiateModel(
            Scene& scene,
            const Resource::ModelResource& model,
            const SceneAssetInstantiationOptions& options = {});

        [[nodiscard]] static SceneAssetStatus UpdateResidency(
            Scene& scene,
            const Resource::ModelResource& model,
            Resource::ResourceSubsystem& resources,
            SceneAssetInstance& instance);

        /** @brief Atomically toggle every renderable owned by an instance. */
        [[nodiscard]] static bool SetRenderablesEnabled(
            Scene& scene,
            const SceneAssetInstance& instance,
            bool enabled);

        [[nodiscard]] static bool Destroy(Scene& scene,
                                          SceneAssetInstance& instance);

        /** @brief Cancel an in-flight instance and roll back all Scene state. */
        [[nodiscard]] static bool Cancel(Scene& scene,
                                         SceneAssetInstance& instance);
    };
} // namespace RVX
