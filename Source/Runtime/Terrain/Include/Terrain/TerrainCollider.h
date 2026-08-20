#pragma once

/**
 * @file TerrainCollider.h
 * @brief Physics collision for heightmap terrain
 * 
 * Provides efficient collision detection for heightmap-based terrain
 * using hierarchical spatial subdivision.
 */

#include "Core/Types.h"
#include "Core/MathTypes.h"
#include "Core/Math/AABB.h"

#include <memory>
#include <vector>
#include <optional>

namespace RVX
{
    class Heightmap;

    /**
     * @brief Terrain raycast hit result
     */
    struct TerrainRaycastHit
    {
        Vec3 position;          ///< Hit position in world space
        Vec3 normal;            ///< Surface normal at hit point
        float distance;         ///< Distance from ray origin
        Vec2 uv;                ///< UV coordinates on terrain
    };

    /**
     * @brief Terrain collision query options
     */
    struct TerrainCollisionOptions
    {
        float maxDistance = 1000.0f;    ///< Maximum query distance
        bool doubleSided = false;       ///< Check both sides of terrain
    };

    /**
     * @brief Physics collider for heightmap terrain
     * 
     * Provides efficient collision detection using a quadtree structure
     * for spatial acceleration. Supports raycasting, sphere/capsule queries,
     * and contact point generation.
     * 
     * Features:
     * - Hierarchical collision detection
     * - Raycast queries
     * - Sphere/capsule overlap tests
     * - Contact point generation
     * - Height queries at arbitrary positions
     * 
     * Usage:
     * @code
     * auto collider = std::make_unique<TerrainCollider>();
     * collider->Initialize(heightmap, terrainSize, terrainPosition);
     * 
     * // Raycast
     * Vec3 rayOrigin(100, 500, 100);
     * Vec3 rayDir(0, -1, 0);
     * TerrainRaycastHit hit;
     * if (collider->Raycast(rayOrigin, rayDir, 1000.0f, hit)) {
     *     // Handle hit
     * }
     * 
     * // Sphere query
     * Vec3 sphereCenter(100, 50, 100);
     * float sphereRadius = 2.0f;
     * if (collider->SphereOverlap(sphereCenter, sphereRadius)) {
     *     // Handle overlap
     * }
     * @endcode
     */
    class TerrainCollider
    {
    public:
        TerrainCollider() = default;
        ~TerrainCollider() = default;

        // Non-copyable
        TerrainCollider(const TerrainCollider&) = delete;
        TerrainCollider& operator=(const TerrainCollider&) = delete;

        // =====================================================================
        // Initialization
        // =====================================================================

        /**
         * @brief Initialize the collider
         * @param heightmap Terrain heightmap
         * @param terrainSize Terrain size (width, height, depth)
         * @param terrainPosition Terrain world position (center or corner)
         * @return true if initialization succeeded
         */
        bool Initialize(const Heightmap* heightmap, const Vec3& terrainSize, 
                        const Vec3& terrainPosition);

        /**
         * @brief Update heightmap data
         * @param heightmap New heightmap data
         */
        void UpdateHeightmap(const Heightmap* heightmap);

        /**
         * @brief Set terrain transform
         * @param position World position
         * @param rotation World rotation (optional)
         */
        void SetTransform(const Vec3& position, const Quat& rotation = Quat(1, 0, 0, 0));

        // =====================================================================
        // Raycast Queries
        // =====================================================================

        /**
         * @brief Cast a ray against the terrain
         * @param origin Ray origin in world space
         * @param direction Ray direction (normalized)
         * @param maxDistance Maximum ray distance
         * @param outHit Output hit result
         * @return true if the ray hit the terrain
         */
        bool Raycast(const Vec3& origin, const Vec3& direction, float maxDistance,
                     TerrainRaycastHit& outHit) const;

        /**
         * @brief Cast multiple rays against the terrain
         * @param origins Array of ray origins
         * @param directions Array of ray directions
         * @param count Number of rays
         * @param maxDistance Maximum ray distance
         * @param outHits Output hit results
         * @return Number of hits
         */
        uint32 RaycastBatch(const Vec3* origins, const Vec3* directions, uint32 count,
                            float maxDistance, TerrainRaycastHit* outHits) const;

        // =====================================================================
        // Overlap Queries
        // =====================================================================

        /**
         * @brief Test sphere overlap with terrain
         * @param center Sphere center in world space
         * @param radius Sphere radius
         * @return true if sphere overlaps terrain
         */
        bool SphereOverlap(const Vec3& center, float radius) const;

        /**
         * @brief Test capsule overlap with terrain
         * @param start Capsule start point
         * @param end Capsule end point
         * @param radius Capsule radius
         * @return true if capsule overlaps terrain
         */
        bool CapsuleOverlap(const Vec3& start, const Vec3& end, float radius) const;

        /**
         * @brief Test AABB overlap with terrain
         * @param aabb Axis-aligned bounding box
         * @return true if AABB overlaps terrain
         */
        bool AABBOverlap(const AABB& aabb) const;

        // =====================================================================
        // Contact Generation
        // =====================================================================

        /**
         * @brief Contact point for physics resolution
         */
        struct ContactPoint
        {
            Vec3 position;          ///< Contact position
            Vec3 normal;            ///< Contact normal (pointing away from terrain)
            float penetration;      ///< Penetration depth
        };

        /**
         * @brief Generate contact points for sphere collision
         * @param center Sphere center
         * @param radius Sphere radius
         * @param outContacts Output contact array
         * @param maxContacts Maximum contacts to generate
         * @return Number of contacts generated
         */
        uint32 GenerateSphereContacts(const Vec3& center, float radius,
                                       ContactPoint* outContacts, uint32 maxContacts) const;

        /**
         * @brief Generate contact points for capsule collision
         * @param start Capsule start
         * @param end Capsule end
         * @param radius Capsule radius
         * @param outContacts Output contact array
         * @param maxContacts Maximum contacts to generate
         * @return Number of contacts generated
         */
        uint32 GenerateCapsuleContacts(const Vec3& start, const Vec3& end, float radius,
                                        ContactPoint* outContacts, uint32 maxContacts) const;

        // =====================================================================
        // Height Queries
        // =====================================================================

        /**
         * @brief Get height at world position
         * @param worldX World X coordinate
         * @param worldZ World Z coordinate
         * @return Height value, or nullopt if outside bounds
         */
        std::optional<float> GetHeightAt(float worldX, float worldZ) const;

        /**
         * @brief Get normal at world position
         * @param worldX World X coordinate
         * @param worldZ World Z coordinate
         * @return Surface normal, or nullopt if outside bounds
         */
        std::optional<Vec3> GetNormalAt(float worldX, float worldZ) const;

        // =====================================================================
        // Bounds
        // =====================================================================

        /**
         * @brief Get world-space bounding box
         */
        const AABB& GetBounds() const { return m_worldBounds; }

        /**
         * @brief Check if a point is within terrain bounds (XZ only)
         */
        bool IsWithinBounds(float worldX, float worldZ) const;

    private:
        // Quadtree node for spatial acceleration
        struct QuadNode
        {
            AABB bounds;
            uint32 children[4] = {0, 0, 0, 0};  // Child indices (0 = none)
            bool isLeaf = true;
        };

        void BuildQuadTree();
        bool RaycastNode(uint32 nodeIndex, const Vec3& origin, const Vec3& invDir,
                         float tMin, float tMax, TerrainRaycastHit& outHit) const;
        bool RaycastTriangle(const Vec3& v0, const Vec3& v1, const Vec3& v2,
                             const Vec3& origin, const Vec3& direction,
                             float& outT, Vec3& outNormal) const;
        Vec3 WorldToLocal(const Vec3& worldPos) const;
        Vec3 LocalToWorld(const Vec3& localPos) const;
        Vec2 WorldToUV(float worldX, float worldZ) const;

        const Heightmap* m_heightmap = nullptr;
        Vec3 m_terrainSize{1.0f};
        Vec3 m_terrainPosition{0.0f};
        Quat m_terrainRotation{1, 0, 0, 0};

        std::vector<QuadNode> m_quadTree;
        AABB m_worldBounds;

        // Cached inverse transform for queries
        Mat4 m_worldToLocal = Mat4(1.0f);
        Mat4 m_localToWorld = Mat4(1.0f);
    };

} // namespace RVX
