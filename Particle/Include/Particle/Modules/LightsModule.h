#pragma once

/**
 * @file LightsModule.h
 * @brief Lights module - particles emit dynamic lights
 */

#include "Particle/Modules/IParticleModule.h"
#include "Particle/Curves/AnimationCurve.h"
#include "Core/MathTypes.h"

namespace RVX::Particle
{
    /**
     * @brief Dynamic lights emitted by particles
     * 
     * Note: This module requires integration with the lighting system
     * and is CPU-side for light management.
     */
    class LightsModule : public IParticleModule
    {
    public:
        /// Light intensity
        float intensity = 1.0f;

        /// Light range
        float range = 5.0f;

        /// Ratio of particles that emit light (0-1)
        float ratio = 0.1f;

        /// Maximum number of particle lights
        uint32 maxLights = 10;

        /// Use particle color for light
        bool useParticleColor = true;

        /// Light color (if not using particle color)
        Vec4 lightColor{1.0f, 1.0f, 1.0f, 1.0f};

        /// Intensity over lifetime curve
        AnimationCurve intensityOverLifetime = AnimationCurve::One();

        /// Range over lifetime curve
        AnimationCurve rangeOverLifetime = AnimationCurve::One();

        /// Random distribution of lights
        bool randomDistribution = true;

        /// Affect specular
        bool affectsSpecular = true;

        /// Shadow casting (expensive!)
        bool castShadows = false;

        const char* GetTypeName() const override { return "LightsModule"; }
        ModuleStage GetStage() const override { return ModuleStage::Render; }
        
        /// Lights are managed on CPU side
        bool IsGPUModule() const override { return false; }

        size_t GetGPUDataSize() const override { return 0; }
    };

} // namespace RVX::Particle
