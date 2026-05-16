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
#include "Scene/Actor.h"
#include "Scene/SceneManager.h"
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace RVX
{
    // Forward declarations
    class Camera;
    struct RaycastHit;

    namespace Spatial { class ISpatialIndex; }

    class SpatialSubsystem;

    /**
     * @brief World configuration
     */
    struct WorldConfig
    {
        std::string name = "World";
        bool autoInitializeSpatial = true;
    };

    /**
     * @brief World - container for all scene content
     * 
     * The World provides:
     * - Scene management (via SceneManager)
     * - Spatial queries (via SpatialSubsystem)
     * - Picking services
     * - WorldSubsystem lifecycle management
     * 
     * Usage:
     * @code
     * World world;
     * world.Initialize();
     * 
     * // Get scene manager
     * SceneManager* scene = world.GetSceneManager();
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
        void Initialize(const WorldConfig& config = {});

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

        /// Get the scene manager
        SceneManager* GetSceneManager() const { return m_sceneManager.get(); }

        /// Spawn a scene-owned actor.
        SceneEntity* SpawnActor(const ActorSpawnParams& params = {});

        /// Spawn a typed actor.
        template<typename T = SceneEntity>
        T* SpawnActor(const ActorSpawnParams& params = {})
        {
            static_assert(std::is_base_of_v<Actor, T>, "T must derive from Actor");

            if (!m_initialized)
                return nullptr;

            if constexpr (std::is_base_of_v<SceneEntity, T>)
            {
                return m_sceneManager ? m_sceneManager->SpawnActor<T>(params) : nullptr;
            }
            else
            {
                return SpawnPureActor<T>(params);
            }
        }

        /// Destroy a scene-owned actor.
        bool DestroyActor(Actor* actor);

        /// Get a world actor by handle.
        Actor* GetActor(Actor::Handle handle) const;

        /// Get non-spatial actor count owned directly by the world.
        size_t GetActorCount() const { return m_actors.size(); }

        /// Iterate over pure world actors and scene-owned actors.
        void ForEachActor(const std::function<void(Actor*)>& callback);

        /// Get the spatial subsystem
        SpatialSubsystem* GetSpatial() const;

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
         * @brief Create a new camera
         * @param name Name of the camera (default: "Main")
         * @return Pointer to the created camera
         */
        Camera* CreateCamera(const std::string& name = "Main");

        /**
         * @brief Get a camera by name
         * @param name Name of the camera
         * @return Pointer to the camera or nullptr if not found
         */
        Camera* GetCamera(const std::string& name = "Main") const;

        /**
         * @brief Destroy a camera by name
         * @param name Name of the camera to destroy
         */
        void DestroyCamera(const std::string& name);

        /**
         * @brief Set the active camera for rendering
         * @param camera The camera to set as active (must be owned by this world)
         */
        void SetActiveCamera(Camera* camera);

        /**
         * @brief Get the currently active camera
         * @return Pointer to the active camera or nullptr
         */
        Camera* GetActiveCamera() const { return m_activeCamera; }

        // =====================================================================
        // Properties
        // =====================================================================

        const std::string& GetName() const { return m_config.name; }
        const WorldConfig& GetConfig() const { return m_config; }

    private:
        template<typename T>
        T* SpawnPureActor(const ActorSpawnParams& params)
        {
            static_assert(std::is_base_of_v<Actor, T>, "T must derive from Actor");
            static_assert(!std::is_base_of_v<SceneEntity, T>, "SceneEntity actors must use SceneManager");

            if (params.parent)
                return nullptr;

            auto actor = std::make_unique<T>(params.name);
            auto* spawned = actor.get();
            spawned->SetAutoRegisterComponents(true);
            spawned->RegisterAllComponents();
            spawned->SetPosition(params.localPosition);
            spawned->SetRotation(params.localRotation);
            spawned->SetScale(params.localScale);

            const auto handle = spawned->GetHandle();
            m_actors[handle] = std::move(actor);
            return spawned;
        }

        bool IsActorDestroyPending(Actor::Handle handle) const;
        void QueuePendingActorDestroy(Actor::Handle handle);
        void FlushPendingActorDestroys();
        void DestroyPureActorImmediate(Actor::Handle handle);
        void UpdatePureActorLifecycles(float deltaTime);
        void ClearPureActors();

        WorldConfig m_config;
        SubsystemCollection<WorldSubsystem> m_subsystems;
        std::unique_ptr<SceneManager> m_sceneManager;
        std::unordered_map<Actor::Handle, std::unique_ptr<Actor>> m_actors;
        std::vector<Actor::Handle> m_pendingDestroyActors;

        // Camera management
        std::unordered_map<std::string, std::unique_ptr<Camera>> m_cameras;
        Camera* m_activeCamera = nullptr;

        bool m_isDispatchingActorLifecycles = false;
        bool m_initialized = false;
    };

} // namespace RVX
