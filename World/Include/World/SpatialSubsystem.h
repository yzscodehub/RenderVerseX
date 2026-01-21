#pragma once

/**
 * @file SpatialSubsystem.h
 * @brief Spatial query subsystem for World
 */

#include "Core/Subsystem/WorldSubsystem.h"
#include "Core/Math/Geometry.h"
#include "Spatial/Index/ISpatialIndex.h"
#include "Spatial/Index/ISpatialEntity.h"
#include "Spatial/Query/QueryFilter.h"
#include <memory>
#include <vector>

namespace RVX
{
    class Camera;
    class SceneEntity;
    struct RaycastHit;

    /**
     * @brief Spatial query subsystem
     * 
     * Provides spatial acceleration and queries for the world:
     * - Frustum culling for visibility
     * - Raycasting for picking
     * - Box/sphere queries for range detection
     * 
     * Usage:
     * @code
     * auto* spatial = world.GetSubsystem<SpatialSubsystem>();
     * 
     * // Visibility query
     * std::vector<SceneEntity*> visible;
     * spatial->QueryVisible(camera, visible);
     * 
     * // Raycast
     * RaycastHit hit;
     * if (spatial->Raycast(ray, hit)) {
     *     // Process hit
     * }
     * @endcode
     */
    class SpatialSubsystem : public WorldSubsystem
    {
    public:
        const char* GetName() const override { return "SpatialSubsystem"; }

        void Initialize() override;
        void Deinitialize() override;
        void Tick(float deltaTime) override;
        bool ShouldTick() const override { return true; }

        // =====================================================================
        // Index Management
        // =====================================================================

        /// Get the spatial index
        Spatial::ISpatialIndex* GetIndex() { return m_index.get(); }
        const Spatial::ISpatialIndex* GetIndex() const { return m_index.get(); }

        /// Set a custom spatial index
        void SetIndex(Spatial::SpatialIndexPtr index);

        /// Rebuild the spatial index
        void RebuildIndex();

        // =====================================================================
        // Visibility Queries
        // =====================================================================

        /// Query visible entities using camera frustum
        void QueryVisible(const Camera& camera, std::vector<SceneEntity*>& outEntities);

        /// Query visible entities using frustum
        void QueryVisible(const Frustum& frustum, std::vector<SceneEntity*>& outEntities);

        /// Query visible entities with filter
        void QueryVisible(const Frustum& frustum, 
                         const Spatial::QueryFilter& filter,
                         std::vector<SceneEntity*>& outEntities);

        // =====================================================================
        // Raycast
        // =====================================================================

        /// Find nearest intersection
        bool Raycast(const Ray& ray, RaycastHit& outHit);

        /// Raycast with filter
        bool Raycast(const Ray& ray, 
                    const Spatial::QueryFilter& filter,
                    RaycastHit& outHit);

        /// Find all intersections
        void RaycastAll(const Ray& ray, std::vector<RaycastHit>& outHits);

        /// Raycast with filter, all hits
        void RaycastAll(const Ray& ray,
                       const Spatial::QueryFilter& filter,
                       std::vector<RaycastHit>& outHits);

        // =====================================================================
        // Screen Picking
        // =====================================================================

        /// Convert screen coordinates to ray
        static Ray ScreenToRay(const Camera& camera, 
                              float screenX, float screenY,
                              float screenWidth, float screenHeight);

        /// Pick from screen coordinates
        bool PickScreen(const Camera& camera,
                       float screenX, float screenY,
                       float screenWidth, float screenHeight,
                       RaycastHit& outHit);

        // =====================================================================
        // Range Queries
        // =====================================================================

        /// Query entities within sphere
        void QuerySphere(const Vec3& center, float radius, 
                        std::vector<SceneEntity*>& outEntities);

        /// Query entities within box
        void QueryBox(const AABB& box, std::vector<SceneEntity*>& outEntities);

    private:
        Spatial::SpatialIndexPtr m_index;
        bool m_needsRebuild = false;
    };

} // namespace RVX
