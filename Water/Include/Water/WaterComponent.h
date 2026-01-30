#pragma once

/**
 * @file WaterComponent.h
 * @brief Scene component for water volumes
 * 
 * WaterComponent attaches a water body to a SceneEntity, providing
 * water rendering, simulation, and interaction.
 */

#include "Scene/Component.h"
#include "Water/WaterSurface.h"
#include "Water/WaterSimulation.h"

#include <memory>

namespace RVX
{
    class Caustics;
    class Underwater;

    /**
     * @brief Water component settings
     */
    struct WaterSettings
    {
        Vec2 size{100.0f, 100.0f};              ///< Water surface size
        float depth = 20.0f;                     ///< Water depth
        uint32 resolution = 128;                 ///< Mesh resolution
        WaterSurfaceType surfaceType = WaterSurfaceType::Ocean;
        WaterSimulationType simulationType = WaterSimulationType::Gerstner;
        bool enableReflection = true;            ///< Enable planar reflections
        bool enableRefraction = true;            ///< Enable refraction
        bool enableCaustics = true;              ///< Enable underwater caustics
        bool enableUnderwaterEffects = true;     ///< Enable underwater post-process
        bool enableFoam = true;                  ///< Enable foam rendering
    };

    /**
     * @brief Component for scene water
     * 
     * Attaches a water body to a SceneEntity, providing realistic
     * water rendering with waves, reflections, and underwater effects.
     * 
     * Features:
     * - Multiple wave simulation types (FFT, Gerstner, Simple)
     * - Planar reflections and refractions
     * - Underwater caustics
     * - Foam and whitecaps
     * - Underwater post-processing
     * - Buoyancy queries
     * 
     * Usage:
     * @code
     * auto* entity = scene->CreateEntity("Ocean");
     * auto* water = entity->AddComponent<WaterComponent>();
     * 
     * WaterSettings settings;
     * settings.size = Vec2(1000.0f, 1000.0f);
     * settings.surfaceType = WaterSurfaceType::Ocean;
     * settings.simulationType = WaterSimulationType::FFT;
     * water->SetSettings(settings);
     * 
     * // Configure visual properties
     * auto& visual = water->GetSurface()->GetVisualProperties();
     * visual.shallowColor = Vec3(0.0f, 0.5f, 0.6f);
     * visual.deepColor = Vec3(0.0f, 0.1f, 0.15f);
     * @endcode
     */
    class WaterComponent : public Component
    {
    public:
        WaterComponent() = default;
        ~WaterComponent() override = default;

        // =====================================================================
        // Component Interface
        // =====================================================================

        const char* GetTypeName() const override { return "Water"; }

        void OnAttach() override;
        void OnDetach() override;
        void Tick(float deltaTime) override;

        // =====================================================================
        // Spatial Bounds
        // =====================================================================

        bool ProvidesBounds() const override { return true; }
        AABB GetLocalBounds() const override;

        // =====================================================================
        // Settings
        // =====================================================================

        /**
         * @brief Set water settings
         * @param settings Water settings
         */
        void SetSettings(const WaterSettings& settings);

        /**
         * @brief Get water settings
         */
        const WaterSettings& GetSettings() const { return m_settings; }

        // =====================================================================
        // Surface
        // =====================================================================

        /**
         * @brief Get the water surface
         */
        WaterSurface* GetSurface() const { return m_surface.get(); }

        // =====================================================================
        // Simulation
        // =====================================================================

        /**
         * @brief Get the water simulation
         */
        WaterSimulation* GetSimulation() const { return m_simulation.get(); }

        /**
         * @brief Set wind direction and speed
         * @param direction Wind direction (XZ plane)
         * @param speed Wind speed
         */
        void SetWind(const Vec2& direction, float speed);

        // =====================================================================
        // Height/Wave Queries
        // =====================================================================

        /**
         * @brief Get water height at world position
         * @param worldX World X coordinate
         * @param worldZ World Z coordinate
         * @return Water height including waves
         */
        float GetWaterHeightAt(float worldX, float worldZ) const;

        /**
         * @brief Get water normal at world position
         * @param worldX World X coordinate
         * @param worldZ World Z coordinate
         * @return Water surface normal
         */
        Vec3 GetWaterNormalAt(float worldX, float worldZ) const;

        /**
         * @brief Get wave displacement at world position
         * @param worldX World X coordinate
         * @param worldZ World Z coordinate
         * @return 3D displacement vector
         */
        Vec3 GetDisplacementAt(float worldX, float worldZ) const;

        /**
         * @brief Check if a point is underwater
         * @param worldPos World position
         * @return true if point is below water surface
         */
        bool IsUnderwater(const Vec3& worldPos) const;

        /**
         * @brief Get water depth at world position
         * @param worldX World X coordinate
         * @param worldZ World Z coordinate
         * @param terrainHeight Optional terrain height
         * @return Water depth
         */
        float GetDepthAt(float worldX, float worldZ, float terrainHeight = 0.0f) const;

        // =====================================================================
        // Buoyancy
        // =====================================================================

        /**
         * @brief Calculate buoyancy force for an object
         * @param position Object position
         * @param volume Object volume
         * @param objectDensity Object density (water = 1000 kg/m³)
         * @return Buoyancy force vector
         */
        Vec3 CalculateBuoyancy(const Vec3& position, float volume, float objectDensity) const;

        // =====================================================================
        // Effects
        // =====================================================================

        /**
         * @brief Get the caustics renderer
         */
        Caustics* GetCaustics() const { return m_caustics.get(); }

        /**
         * @brief Get the underwater effects
         */
        Underwater* GetUnderwater() const { return m_underwater.get(); }

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
        void UpdateBounds();
        Vec2 WorldToLocal(float worldX, float worldZ) const;

        WaterSettings m_settings;
        
        std::unique_ptr<WaterSurface> m_surface;
        std::unique_ptr<WaterSimulation> m_simulation;
        std::unique_ptr<Caustics> m_caustics;
        std::unique_ptr<Underwater> m_underwater;

        bool m_gpuInitialized = false;
        AABB m_localBounds;
    };

} // namespace RVX
