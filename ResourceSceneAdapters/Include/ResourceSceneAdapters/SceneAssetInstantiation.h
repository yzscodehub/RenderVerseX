#pragma once

/**
 * @file SceneAssetInstantiation.h
 * @brief Transactional Resource-to-Scene model instantiation contract.
 */

#include "Core/Types.h"
#include "Resource/ResourceHandle.h"
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

    enum class SceneAssetReadiness : uint8
    {
        Loading = 0,
        CPUReady,
        GPUUploadPending,
        RenderReady,
        Failed
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
        SceneAssetReadiness readiness = SceneAssetReadiness::Loading;
        std::string diagnostic;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return rootActor.IsValid() &&
                   readiness != SceneAssetReadiness::Failed;
        }
        [[nodiscard]] bool IsRenderReady() const noexcept
        {
            return readiness == SceneAssetReadiness::RenderReady;
        }
    };

    class SceneAssetInstantiator final
    {
    public:
        [[nodiscard]] static SceneAssetInstance InstantiateModel(
            Scene& scene,
            const Resource::ModelResource& model,
            const SceneAssetInstantiationOptions& options = {});

        [[nodiscard]] static SceneAssetReadiness UpdateReadiness(
            Scene& scene,
            const Resource::ModelResource& model,
            Resource::ResourceSubsystem& resources,
            SceneAssetInstance& instance);

        [[nodiscard]] static bool Destroy(Scene& scene,
                                          SceneAssetInstance& instance);

        /** @brief Cancel an in-flight instance and roll back all Scene state. */
        [[nodiscard]] static bool Cancel(Scene& scene,
                                         SceneAssetInstance& instance);
    };
} // namespace RVX
