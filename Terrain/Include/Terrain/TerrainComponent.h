#pragma once

/**
 * @file TerrainComponent.h
 * @brief Scene component for terrain attachment
 * 
 * TerrainComponent attaches a terrain to a SceneEntity, providing
 * terrain rendering and collision in the scene graph.
 */

#include "Scene/Component.h"
#include "Terrain/TerrainTypes.h"

#include <memory>

namespace RVX
{
    class IRHIDevice;
    class Heightmap;
    class TerrainCollider;
    class TerrainLOD;
    class TerrainMaterial;

    /**
     * @brief Component for scene terrain
     * 
     * Attaches a terrain to a SceneEntity, providing heightmap-based
     * terrain with multi-layer texturing and physics collision.
     * 
     * Features:
     * - Heightmap-based terrain mesh generation
     * - CDLOD (Continuous Distance-dependent Level of Detail)
     * - Multi-layer texture splatting
     * - Physics collision integration
     * - Shadow casting and receiving
     * 
     * Usage:
     * @code
     * auto* entity = scene->CreateEntity("Terrain");
     * auto* terrain = entity->AddComponent<TerrainComponent>();
     * 
     * // Setup heightmap
     * auto heightmap = std::make_shared<Heightmap>();
     * heightmap->LoadFromImage("terrain_height.png");
     * terrain->SetHeightmap(heightmap);
     * 
     * // Setup material
     * auto material = std::make_shared<TerrainMaterial>();
     * material->AddLayer("grass", grassTexture, 10.0f);
     * material->AddLayer("rock", rockTexture, 8.0f);
     * terrain->SetMaterial(material);
     * 
     * // Configure
     * TerrainSettings settings;
     * settings.size = Vec3(2000.0f, 500.0f, 2000.0f);
     * terrain->SetSettings(settings);
     * @endcode
     */
    class TerrainComponent : public Component
    {
    public:
        TerrainComponent();
        ~TerrainComponent() override;

        // =====================================================================
        // Component Interface
        // =====================================================================

        const char* GetTypeName() const override { return "Terrain"; }

        void OnAttach() override;
        void OnDetach() override;
        void Tick(float deltaTime) override;

        // =====================================================================
        // Spatial Bounds
        // =====================================================================

        bool ProvidesBounds() const override { return true; }
        AABB GetLocalBounds() const override;

        // =====================================================================
        // Heightmap
        // =====================================================================

        /**
         * @brief Set the heightmap
         * @param heightmap Heightmap to use
         */
        void SetHeightmap(std::shared_ptr<Heightmap> heightmap);

        /**
         * @brief Get the heightmap
         */
        std::shared_ptr<Heightmap> GetHeightmap() const { return m_heightmap; }

        // =====================================================================
        // Material
        // =====================================================================

        /**
         * @brief Set the terrain material
         * @param material Material to use
         */
        void SetMaterial(std::shared_ptr<TerrainMaterial> material);

        /**
         * @brief Get the terrain material
         */
        std::shared_ptr<TerrainMaterial> GetMaterial() const { return m_material; }

        // =====================================================================
        // Settings
        // =====================================================================

        /**
         * @brief Set terrain settings
         * @param settings Terrain settings
         */
        void SetSettings(const TerrainSettings& settings);

        /**
         * @brief Get terrain settings
         */
        const TerrainSettings& GetSettings() const { return m_settings; }

        // =====================================================================
        // Height Queries
        // =====================================================================

        /**
         * @brief Get height at world position
         * @param worldX World X coordinate
         * @param worldZ World Z coordinate
         * @return Height value, or 0 if outside terrain bounds
         */
        float GetHeightAt(float worldX, float worldZ) const;

        /**
         * @brief Get normal at world position
         * @param worldX World X coordinate
         * @param worldZ World Z coordinate
         * @return Surface normal vector
         */
        Vec3 GetNormalAt(float worldX, float worldZ) const;

        /**
         * @brief Check if a world position is within terrain bounds
         * @param worldX World X coordinate
         * @param worldZ World Z coordinate
         * @return true if within bounds
         */
        bool IsWithinBounds(float worldX, float worldZ) const;

        // =====================================================================
        // LOD System
        // =====================================================================

        /**
         * @brief Get the LOD system
         */
        TerrainLOD* GetLODSystem() const { return m_lodSystem.get(); }

        /**
         * @brief Update LOD based on camera position
         * @param cameraPosition Camera world position
         */
        void UpdateLOD(const Vec3& cameraPosition);

        // =====================================================================
        // Collision
        // =====================================================================

        /**
         * @brief Enable/disable collision
         */
        void SetCollisionEnabled(bool enabled);
        bool IsCollisionEnabled() const { return m_collisionEnabled; }

        /**
         * @brief Get the terrain collider
         */
        TerrainCollider* GetCollider() const { return m_collider.get(); }

        // =====================================================================
        // GPU Resources
        // =====================================================================

        /**
         * @brief Initialize GPU resources
         * @param device RHI device
         * @return true if initialization succeeded
         */
        bool InitializeGPU(IRHIDevice* device);

        /**
         * @brief Check if GPU resources are initialized
         */
        bool IsGPUInitialized() const { return m_gpuInitialized; }

    private:
        void RebuildMesh();
        void UpdateBounds();

        std::shared_ptr<Heightmap> m_heightmap;
        std::shared_ptr<TerrainMaterial> m_material;
        TerrainSettings m_settings;

        std::unique_ptr<TerrainLOD> m_lodSystem;
        std::unique_ptr<TerrainCollider> m_collider;

        bool m_collisionEnabled = true;
        bool m_gpuInitialized = false;
        bool m_needsRebuild = true;

        AABB m_localBounds;
    };

} // namespace RVX
