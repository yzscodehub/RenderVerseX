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
#include <memory>
#include <string>
#include <unordered_map>

namespace RVX
{
    // Forward declarations
    class SceneManager;
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
        WorldConfig m_config;
        SubsystemCollection<WorldSubsystem> m_subsystems;
        std::unique_ptr<SceneManager> m_sceneManager;

        // Camera management
        std::unordered_map<std::string, std::unique_ptr<Camera>> m_cameras;
        Camera* m_activeCamera = nullptr;

        bool m_initialized = false;
    };

} // namespace RVX
