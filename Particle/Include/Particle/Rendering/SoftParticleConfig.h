#pragma once

/**
 * @file SoftParticleConfig.h
 * @brief Soft particle configuration for depth-based edge fading
 */

#include "Core/Types.h"

namespace RVX::Particle
{
    /**
     * @brief Soft particle configuration
     * 
     * Soft particles fade out near scene geometry to avoid
     * hard intersection edges. Requires reading from depth buffer.
     */
    struct SoftParticleConfig
    {
        /// Enable soft particles
        bool enabled = true;

        /// Fade distance (world units)
        float fadeDistance = 0.5f;

        /// Fade contrast/power
        float contrastPower = 1.0f;
    };

} // namespace RVX::Particle
