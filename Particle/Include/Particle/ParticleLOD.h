#pragma once

/**
 * @file ParticleLOD.h
 * @brief LOD (Level of Detail) configuration for particle systems
 */

#include "Core/Types.h"
#include <vector>
#include <algorithm>

namespace RVX::Particle
{
    /**
     * @brief Single LOD level configuration
     */
    struct ParticleLODLevel
    {
        /// Distance at which this LOD level activates
        float distance = 0.0f;

        /// Emission rate multiplier (0.0 - 1.0)
        float emissionRateMultiplier = 1.0f;

        /// Simulation rate multiplier (for reduced update frequency)
        float simulationRateMultiplier = 1.0f;

        /// Maximum particles for this LOD level
        uint32 maxParticles = 10000;

        /// Disable collision at this LOD
        bool disableCollision = false;

        /// Disable noise at this LOD
        bool disableNoise = false;

        /// Disable trails at this LOD
        bool disableTrail = false;

        /// Disable lights at this LOD
        bool disableLights = false;

        /// Disable sub-emitters at this LOD
        bool disableSubEmitters = false;
    };

    /**
     * @brief LOD configuration for a particle system
     */
    class ParticleLODConfig
    {
    public:
        /// LOD levels (sorted by distance)
        std::vector<ParticleLODLevel> levels;

        /// Enable smooth transitions between LOD levels
        bool fadeTransition = true;

        /// Distance range for fade transition
        float fadeRange = 5.0f;

        /// Distance at which particles are completely culled
        float cullDistance = 100.0f;

        /// Enable LOD system
        bool enabled = true;

        // =====================================================================
        // Methods
        // =====================================================================

        /// Add a LOD level
        void AddLevel(const ParticleLODLevel& level)
        {
            levels.push_back(level);
            SortLevels();
        }

        /// Get the LOD level for a given distance
        const ParticleLODLevel& GetLevel(float distance) const
        {
            if (levels.empty())
            {
                static ParticleLODLevel defaultLevel;
                return defaultLevel;
            }

            for (size_t i = levels.size(); i > 0; --i)
            {
                if (distance >= levels[i - 1].distance)
                    return levels[i - 1];
            }
            return levels[0];
        }

        /// Get LOD level index for a given distance
        uint32 GetLevelIndex(float distance) const
        {
            if (levels.empty()) return 0;

            for (size_t i = levels.size(); i > 0; --i)
            {
                if (distance >= levels[i - 1].distance)
                    return static_cast<uint32>(i - 1);
            }
            return 0;
        }

        /// Calculate transition alpha between LOD levels
        float GetTransitionAlpha(float distance) const
        {
            if (!fadeTransition || levels.size() < 2)
                return 1.0f;

            uint32 currentLevel = GetLevelIndex(distance);
            if (currentLevel >= levels.size() - 1)
                return 1.0f;

            float nextLevelDistance = levels[currentLevel + 1].distance;
            float transitionStart = nextLevelDistance - fadeRange;

            if (distance < transitionStart)
                return 1.0f;

            return 1.0f - std::clamp(
                (distance - transitionStart) / fadeRange, 0.0f, 1.0f);
        }

        /// Check if should be culled at distance
        bool ShouldCull(float distance) const
        {
            return distance >= cullDistance;
        }

        // =====================================================================
        // Presets
        // =====================================================================

        /// Default LOD configuration
        static ParticleLODConfig Default()
        {
            ParticleLODConfig config;
            
            ParticleLODLevel lod0;
            lod0.distance = 0.0f;
            lod0.emissionRateMultiplier = 1.0f;
            lod0.maxParticles = 10000;
            config.levels.push_back(lod0);

            ParticleLODLevel lod1;
            lod1.distance = 25.0f;
            lod1.emissionRateMultiplier = 0.5f;
            lod1.maxParticles = 5000;
            lod1.disableLights = true;
            config.levels.push_back(lod1);

            ParticleLODLevel lod2;
            lod2.distance = 50.0f;
            lod2.emissionRateMultiplier = 0.25f;
            lod2.maxParticles = 2000;
            lod2.disableCollision = true;
            lod2.disableNoise = true;
            lod2.disableLights = true;
            lod2.disableTrail = true;
            config.levels.push_back(lod2);

            config.cullDistance = 100.0f;
            return config;
        }

        /// High performance LOD (aggressive reduction)
        static ParticleLODConfig HighPerformance()
        {
            ParticleLODConfig config;

            ParticleLODLevel lod0;
            lod0.distance = 0.0f;
            lod0.emissionRateMultiplier = 0.75f;
            lod0.maxParticles = 5000;
            lod0.disableLights = true;
            config.levels.push_back(lod0);

            ParticleLODLevel lod1;
            lod1.distance = 15.0f;
            lod1.emissionRateMultiplier = 0.25f;
            lod1.maxParticles = 1000;
            lod1.disableCollision = true;
            lod1.disableNoise = true;
            lod1.disableLights = true;
            lod1.disableTrail = true;
            config.levels.push_back(lod1);

            config.cullDistance = 50.0f;
            return config;
        }

        /// High quality LOD (minimal reduction)
        static ParticleLODConfig HighQuality()
        {
            ParticleLODConfig config;

            ParticleLODLevel lod0;
            lod0.distance = 0.0f;
            config.levels.push_back(lod0);

            ParticleLODLevel lod1;
            lod1.distance = 50.0f;
            lod1.emissionRateMultiplier = 0.75f;
            lod1.maxParticles = 7500;
            config.levels.push_back(lod1);

            ParticleLODLevel lod2;
            lod2.distance = 100.0f;
            lod2.emissionRateMultiplier = 0.5f;
            lod2.maxParticles = 5000;
            lod2.disableLights = true;
            config.levels.push_back(lod2);

            config.cullDistance = 200.0f;
            return config;
        }

    private:
        void SortLevels()
        {
            std::sort(levels.begin(), levels.end(),
                [](const ParticleLODLevel& a, const ParticleLODLevel& b)
                {
                    return a.distance < b.distance;
                });
        }
    };

} // namespace RVX::Particle
