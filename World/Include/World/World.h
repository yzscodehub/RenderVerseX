#pragma once

/**
 * @file World.h
 * @brief Value-configured owner for one pure Scene ECS runtime.
 */

#include "Physics/PhysicsWorld.h"
#include "Scene/ECS/SceneEcsRuntime.h"
#include "World/ECS/WorldEcsCameraService.h"

#include <string>

namespace RVX
{
    /**
     * @brief Immutable creation contract for one ECS World.
     *
     * Physics settings are data consumed by Engine's per-World ECS composition;
     * World itself never constructs or owns a PhysicsWorld.
     */
    struct WorldConfig
    {
        std::string name = "World";
        Physics::PhysicsWorldConfig physics{};
    };

    /**
     * @brief Sole runtime authority for one ECS scene and its camera value facade.
     *
     * Engine owns all execution policy: bridge composition, resource requests,
     * fixed-step state, and render publication.  World deliberately exposes no
     * legacy Scene, Actor, subsystem, loading, picking, or tick facade.
     */
    class World
    {
    public:
        explicit World(WorldConfig config = {});
        ~World() = default;

        World(const World&) = delete;
        World& operator=(const World&) = delete;
        World(World&&) = delete;
        World& operator=(World&&) = delete;

        [[nodiscard]] const WorldConfig& GetConfig() const noexcept { return m_config; }
        [[nodiscard]] const std::string& GetName() const noexcept { return m_config.name; }

        /** @brief The World-owned, sole mutable Scene authority. */
        [[nodiscard]] SceneECS::SceneEcsRuntime& GetSceneEcsRuntime() noexcept
        {
            return m_sceneRuntime;
        }
        [[nodiscard]] const SceneECS::SceneEcsRuntime& GetSceneEcsRuntime() const noexcept
        {
            return m_sceneRuntime;
        }

        /** @brief Handle-and-value camera authoring for this exact Scene runtime. */
        [[nodiscard]] WorldECS::WorldEcsCameraService& GetCameraService() noexcept
        {
            return m_cameraService;
        }
        [[nodiscard]] const WorldECS::WorldEcsCameraService& GetCameraService() const noexcept
        {
            return m_cameraService;
        }

    private:
        WorldConfig m_config;
        SceneECS::SceneEcsRuntime m_sceneRuntime;
        WorldECS::WorldEcsCameraService m_cameraService;
    };
} // namespace RVX
