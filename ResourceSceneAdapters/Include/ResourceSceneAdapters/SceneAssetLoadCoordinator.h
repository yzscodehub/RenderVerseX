#pragma once

/**
 * @file SceneAssetLoadCoordinator.h
 * @brief Owner-thread bridge from asynchronous resources to transactional Scene assets.
 */

#include "Core/Handle.h"
#include "Resource/Loader/EnvironmentLoader.h"
#include "Resource/ResourceSubsystem.h"
#include "Resource/Types/EnvironmentResource.h"
#include "Resource/Types/ModelResource.h"
#include "ResourceSceneAdapters/SceneAssetInstantiation.h"
#include "Scene/SceneIdentity.h"

#include <memory>
#include <string>
#include <vector>

namespace RVX
{
    class Scene;

    struct SceneAssetLoadHandleTag final
    {
    };
    using SceneAssetLoadHandle = Handle<SceneAssetLoadHandleTag, uint32>;
    inline constexpr SceneAssetLoadHandle InvalidSceneAssetLoadHandle =
        SceneAssetLoadHandle::Invalid();

    enum class SceneAssetKind : uint8
    {
        Model = 0,
        Environment
    };

    struct SceneModelLoadDesc
    {
        std::string path;
        Resource::ResourceLoadOptions resourceOptions;
        SceneAssetInstantiationOptions instantiationOptions;
        bool activateWhenResident = true;
    };

    struct SceneEnvironmentLoadDesc
    {
        std::string path;
        Resource::EnvironmentLoadOptions environmentOptions;
        Resource::ResourceLoadOptions resourceOptions;
        ComponentHandle targetSkybox = InvalidComponentHandle;
    };

    /**
     * @brief Coordinates worker preparation, owner publication, Scene mutation,
     * GPU residency and cancellation without exposing RHI to Scene or Samples.
     */
    class SceneAssetLoadCoordinator final
    {
    public:
        SceneAssetLoadCoordinator(Scene& scene,
                                  Resource::ResourceSubsystem& resources) noexcept;
        ~SceneAssetLoadCoordinator();

        SceneAssetLoadCoordinator(const SceneAssetLoadCoordinator&) = delete;
        SceneAssetLoadCoordinator& operator=(
            const SceneAssetLoadCoordinator&) = delete;

        [[nodiscard]] SceneAssetLoadHandle RequestModel(
            SceneModelLoadDesc desc,
            std::string& outError);

        [[nodiscard]] SceneAssetLoadHandle RequestEnvironment(
            SceneEnvironmentLoadDesc desc,
            std::string& outError);

        /** @brief Create another Scene instance from an already published model. */
        [[nodiscard]] SceneAssetLoadHandle InstantiateModel(
            Resource::ResourceHandle<Resource::ModelResource> model,
            SceneAssetInstantiationOptions options,
            bool activateWhenResident,
            std::string& outError);

        /** @brief Advance all requests. Must run on the Scene update thread. */
        [[nodiscard]] bool Update();

        /** @brief Cancel one subscriber and roll back its Scene/environment state. */
        [[nodiscard]] bool Cancel(SceneAssetLoadHandle handle);

        /** @brief Cancel and release every request before World/Scene teardown. */
        void CancelAll();

        [[nodiscard]] bool IsValid(SceneAssetLoadHandle handle) const;
        [[nodiscard]] SceneAssetKind GetKind(SceneAssetLoadHandle handle) const;
        [[nodiscard]] const SceneAssetStatus* GetStatus(
            SceneAssetLoadHandle handle) const;
        [[nodiscard]] Resource::ResourceLoadRequestId GetResourceRequestId(
            SceneAssetLoadHandle handle) const;
        [[nodiscard]] Resource::ResourceHandle<Resource::ModelResource> GetModel(
            SceneAssetLoadHandle handle) const;
        [[nodiscard]] const SceneAssetInstance* GetModelInstance(
            SceneAssetLoadHandle handle) const;
        [[nodiscard]] Resource::EnvironmentHandle GetEnvironment(
            SceneAssetLoadHandle handle) const;

    private:
        struct Entry;

        [[nodiscard]] Entry* Resolve(SceneAssetLoadHandle handle);
        [[nodiscard]] const Entry* Resolve(SceneAssetLoadHandle handle) const;
        [[nodiscard]] SceneAssetLoadHandle AllocateEntry(
            std::unique_ptr<Entry> entry);
        bool UpdateModel(Entry& entry);
        bool UpdateEnvironment(Entry& entry);
        void Fail(Entry& entry,
                  Resource::ResourceLoadError error,
                  std::string diagnostic);
        void RestoreEnvironment(Entry& entry);

        Scene& m_scene;
        Resource::ResourceSubsystem& m_resources;
        HandlePool<SceneAssetLoadHandle> m_handles;
        std::vector<std::unique_ptr<Entry>> m_entries;
    };
} // namespace RVX
