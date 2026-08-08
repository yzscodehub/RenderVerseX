#pragma once

/**
 * @file World.h
 * @brief World container - manages scene, spatial, and picking
 * 
 * The World is the container for all scene content including:
 * - Scene entities and hierarchy
 * - Spatial indexing for queries
 * - Picking/raycasting services
 */

#include "Core/Subsystem/SubsystemCollection.h"
#include "Core/Subsystem/WorldSubsystem.h"
#include "Core/Math/Geometry.h"
#include "Physics/PhysicsWorld.h"
#include "Scene/SceneRuntime.h"
#include <functional>
#include <memory>
#include <string>
#include <type_traits>

namespace RVX
{
    // Forward declarations
    class Camera;
    class CameraComponent;
    struct RaycastHit;

    namespace Spatial { class ISpatialIndex; }

    class SpatialSubsystem;
    class PhysicsSubsystem;

    /**
     * @brief World configuration
     */
    struct WorldConfig
    {
        std::string name = "World";
        bool autoInitializeSpatial = true;
        bool autoInitializePhysics = true;
        Physics::PhysicsWorldConfig physics;
    };

    /**
     * @brief World - container for all scene content
     * 
     * The World provides:
     * - Scene management (via the authoritative Scene runtime)
     * - Spatial queries (via SpatialSubsystem)
     * - Picking services
     * - WorldSubsystem lifecycle management
     * 
     * Usage:
     * @code
     * World world;
     * world.Initialize();
     * 
     * // Get the authoritative scene runtime
     * Scene* scene = world.GetScene();
     * 
     * // Spatial queries
     * auto* spatial = world.GetSubsystem<SpatialSubsystem>();
     * RaycastHit hit;
     * if (spatial->Raycast(ray, hit)) {
     *     // Handle hit
     * }
     * 
     * world.Shutdown();
     * @endcode
     */
    class World
    {
    public:
        World();
        ~World();

        // Non-copyable
        World(const World&) = delete;
        World& operator=(const World&) = delete;

        // =====================================================================
        // Lifecycle
        // =====================================================================

        /// Initialize the world
        bool Initialize(const WorldConfig& config = {});

        /// Load world content from a path/asset
        void Load(const std::string& path);

        /// Unload current world content
        void Unload();

        /// Tick the world (update all subsystems)
        void Tick(float deltaTime);

        /// Shutdown the world
        void Shutdown();

        /// Check if initialized
        bool IsInitialized() const { return m_initialized; }

        // =====================================================================
        // Subsystem Management
        // =====================================================================

        /// Add a world subsystem
        template<typename T, typename... Args>
        T* AddSubsystem(Args&&... args)
        {
            auto* subsystem = m_subsystems.AddSubsystem<T>(std::forward<Args>(args)...);
            subsystem->SetWorld(this);
            return subsystem;
        }

        /// Get a subsystem by type
        template<typename T>
        T* GetSubsystem() const
        {
            return m_subsystems.GetSubsystem<T>();
        }

        /// Check if subsystem exists
        template<typename T>
        bool HasSubsystem() const
        {
            return m_subsystems.HasSubsystem<T>();
        }

        /// Get all subsystems
        SubsystemCollection<WorldSubsystem>& GetSubsystems() { return m_subsystems; }

        // =====================================================================
        // Scene Access
        // =====================================================================

        /// Get the authoritative scene runtime.
        Scene* GetScene() const { return m_scene.get(); }

        /// Compatibility facade for legacy spatial SceneEntity APIs.
        SceneManager* GetSceneManager() const
        {
            return m_scene ? m_scene->GetSceneManager() : nullptr;
        }

        /// Spawn a scene-owned actor.
        SceneEntity* SpawnActor(const ActorSpawnParams& params = {});

        /// Spawn an actor by registered ActorFactory class name.
        Actor* SpawnActorByClassName(const std::string& className,
                                     const ActorSpawnParams& params = {});

        /// Spawn a typed actor.
        template<typename T = SceneEntity>
        T* SpawnActor(const ActorSpawnParams& params = {})
        {
            static_assert(std::is_base_of_v<Actor, T>, "T must derive from Actor");

            if (!m_initialized)
                return nullptr;

            if constexpr (std::is_base_of_v<SceneEntity, T>)
            {
                return m_scene ? m_scene->SpawnActor<T>(params) : nullptr;
            }
            else
            {
                return m_scene ? m_scene->SpawnActor<T>(params) : nullptr;
            }
        }

        /// Destroy a scene-owned actor.
        bool DestroyActor(Actor* actor);

        /// Get a world actor by handle.
        Actor* GetActor(Actor::Handle handle) const;

        /// Get non-spatial actor count owned directly by the world.
        size_t GetActorCount() const { return m_scene ? m_scene->GetPureActorCount() : 0; }

        /// Iterate over pure world actors and scene-owned actors.
        void ForEachActor(const std::function<void(Actor*)>& callback);

        /// Get the spatial subsystem
        SpatialSubsystem* GetSpatial() const;

        /// Get the physics subsystem
        PhysicsSubsystem* GetPhysics() const;

        // =====================================================================
        // Picking (convenience methods)
        // =====================================================================

        /// Pick with a ray
        bool Pick(const Ray& ray, RaycastHit& outResult);

        /// Pick from screen coordinates
        bool PickScreen(const Camera& camera, 
                       float screenX, float screenY,
                       float screenWidth, float screenHeight,
                       RaycastHit& outResult);

        // =====================================================================
        // Camera Management
        // =====================================================================

        /**
         * @brief Create a legacy Camera facade backed by a CameraComponent
         * @param name Name of the camera (default: "Main")
         * @return Pointer to the created camera
         */
        Camera* CreateCamera(const std::string& name = "Main");

        /**
         * @brief Get a legacy Camera facade by name
         * @param name Name of the camera
         * @return Pointer to the camera or nullptr if not found
         */
        Camera* GetCamera(const std::string& name = "Main") const;

        /**
         * @brief Destroy a legacy Camera facade and its CameraComponent actor
         * @param name Name of the camera to destroy
         */
        void DestroyCamera(const std::string& name);

        /**
         * @brief Compatibility adapter selecting the facade's CameraComponent
         * @param camera The camera to set as active (must be owned by this world)
         */
        void SetActiveCamera(Camera* camera);

        /** @brief Select a scene-owned CameraComponent as active. */
        bool SetActiveCamera(ComponentHandle camera);

        [[nodiscard]] ComponentHandle GetActiveCameraHandle() const
        {
            return m_scene ? m_scene->GetActiveCameraHandle()
                           : InvalidComponentHandle;
        }

        [[nodiscard]] CameraComponent* GetActiveCameraComponent() const
        {
            return m_scene ? m_scene->GetActiveCameraComponent() : nullptr;
        }

        /**
         * @brief Get the active legacy Camera facade, when one selected it
         * @return Pointer to the active camera or nullptr
         */
        Camera* GetActiveCamera() const
        {
            return m_scene ? m_scene->GetActiveCamera() : nullptr;
        }

        // =====================================================================
        // Properties
        // =====================================================================

        const std::string& GetName() const { return m_config.name; }
        const WorldConfig& GetConfig() const { return m_config; }

    private:
        WorldConfig m_config;
        SubsystemCollection<WorldSubsystem> m_subsystems;
        std::unique_ptr<Scene> m_scene;
        bool m_initialized = false;
    };

} // namespace RVX
