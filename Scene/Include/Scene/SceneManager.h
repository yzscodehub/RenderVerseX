#pragma once

/**
 * @file SceneManager.h
 * @brief Central manager for scene entities and spatial queries
 */

#include "Scene/SceneEntity.h"
#include "Core/Math/Geometry.h"
#include "Spatial/Index/ISpatialIndex.h"
#include "Spatial/Index/SpatialFactory.h"
#include <unordered_map>
#include <vector>
#include <memory>
#include <functional>

namespace RVX
{
    // Forward declarations for Asset integration
    namespace Asset { class SceneAsset; class ModelAsset; class PrefabAsset; }

    class Camera;
    class Node;

    /**
     * @brief Configuration for SceneManager
     */
    struct SceneConfig
    {
        /// Type of spatial index to use
        Spatial::SpatialIndexType spatialIndexType = Spatial::SpatialIndexType::BVH;

        /// Whether to auto-rebuild spatial index
        bool autoRebuildIndex = true;

        /// Threshold for triggering index rebuild
        float rebuildThreshold = 0.1f; // 10% dirty entities
    };

    /**
     * @brief Raycast hit result
     */
    struct RaycastHit
    {
        SceneEntity* entity = nullptr;
        float distance = 0.0f;
        Vec3 hitPoint{0.0f};
        Vec3 hitNormal{0.0f, 1.0f, 0.0f};

        bool IsValid() const { return entity != nullptr; }
    };

    /**
     * @brief Central manager for scene entities
     * 
     * Responsibilities:
     * - Entity lifecycle management
     * - Spatial indexing and queries
     * - Visibility culling
     * - Ray picking
     */
    class SceneManager
    {
    public:
        SceneManager();
        ~SceneManager();

        // Non-copyable
        SceneManager(const SceneManager&) = delete;
        SceneManager& operator=(const SceneManager&) = delete;

        // =====================================================================
        // Initialization
        // =====================================================================

        void Initialize(const SceneConfig& config = {});
        void Shutdown();
        bool IsInitialized() const { return m_initialized; }

        // =====================================================================
        // Entity Management
        // =====================================================================

        /// Create a new entity
        SceneEntity::Handle CreateEntity(const std::string& name = "Entity");

        /// Create a typed entity
        template<typename T, typename... Args>
        T* CreateEntity(Args&&... args)
        {
            static_assert(std::is_base_of_v<SceneEntity, T>, "T must derive from SceneEntity");
            auto entity = std::make_shared<T>(std::forward<Args>(args)...);
            return static_cast<T*>(AddEntity(entity));
        }

        /// Add an existing entity
        SceneEntity* AddEntity(SceneEntity::Ptr entity);

        /// Destroy an entity by handle
        void DestroyEntity(SceneEntity::Handle handle);

        /// Get entity by handle
        SceneEntity* GetEntity(SceneEntity::Handle handle);
        const SceneEntity* GetEntity(SceneEntity::Handle handle) const;

        /// Get all entities
        const std::unordered_map<SceneEntity::Handle, SceneEntity::Ptr>& GetEntities() const { return m_entities; }

        /// Get entity count
        size_t GetEntityCount() const { return m_entities.size(); }

        // =====================================================================
        // Asset Integration (for future Asset module)
        // =====================================================================

        /// Load scene from asset (replaces current scene)
        // void LoadFromAsset(const Asset::SceneAsset* sceneAsset);

        /// Instantiate a model into the scene
        // SceneEntity* InstantiateModel(const Asset::ModelAsset* modelAsset, 
        //                                const Transform& transform = {});

        /// Instantiate a prefab into the scene
        // SceneEntity* InstantiatePrefab(const Asset::PrefabAsset* prefabAsset,
        //                                 const Transform& transform = {});

        /// Add a node hierarchy
        void AddHierarchy(std::shared_ptr<Node> rootNode);

        // =====================================================================
        // Spatial Queries
        // =====================================================================

        /// Query visible entities using camera's view-projection matrix
        void QueryVisible(const Mat4& viewProj, std::vector<SceneEntity*>& outEntities);

        /// Query visible entities using frustum
        void QueryVisible(const Frustum& frustum, std::vector<SceneEntity*>& outEntities);

        /// Query visible entities using frustum with filter
        void QueryVisible(const Frustum& frustum, 
                         const Spatial::QueryFilter& filter,
                         std::vector<SceneEntity*>& outEntities);

        /// Raycast - find nearest intersection
        bool Raycast(const Ray& ray, RaycastHit& outHit);

        /// Raycast with filter
        bool Raycast(const Ray& ray, 
                    const Spatial::QueryFilter& filter, 
                    RaycastHit& outHit);

        /// Raycast - find all intersections
        void RaycastAll(const Ray& ray, std::vector<RaycastHit>& outHits);

        /// Query entities within a sphere
        void QuerySphere(const Vec3& center, float radius, std::vector<SceneEntity*>& outEntities);

        /// Query entities within a box
        void QueryBox(const AABB& box, std::vector<SceneEntity*>& outEntities);

        // =====================================================================
        // Update
        // =====================================================================

        /// Update scene (call once per frame)
        void Update(float deltaTime);

        /// Force rebuild of spatial index
        void RebuildSpatialIndex();

        // =====================================================================
        // Spatial Index Configuration
        // =====================================================================

        void SetSpatialIndex(Spatial::SpatialIndexPtr index);
        Spatial::ISpatialIndex* GetSpatialIndex() { return m_spatialIndex.get(); }
        const Spatial::ISpatialIndex* GetSpatialIndex() const { return m_spatialIndex.get(); }

        // =====================================================================
        // Iteration
        // =====================================================================

        /// Iterate over all entities
        void ForEachEntity(const std::function<void(SceneEntity*)>& callback);

        /// Iterate over all active entities
        void ForEachActiveEntity(const std::function<void(SceneEntity*)>& callback);

        // =====================================================================
        // Statistics
        // =====================================================================

        struct Stats
        {
            size_t entityCount = 0;
            size_t activeEntityCount = 0;
            size_t dirtyEntityCount = 0;
            Spatial::IndexStats spatialStats;
        };

        Stats GetStats() const;

    private:
        bool m_initialized = false;
        SceneConfig m_config;

        // Entity storage
        std::unordered_map<SceneEntity::Handle, SceneEntity::Ptr> m_entities;

        // Spatial indexing
        Spatial::SpatialIndexPtr m_spatialIndex;
        std::vector<SceneEntity*> m_dirtyEntities;
        bool m_indexNeedsRebuild = false;

        // Helper for building spatial index
        void UpdateDirtyEntities();
        void CollectDirtyEntities();
    };

} // namespace RVX
