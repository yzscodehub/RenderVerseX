#pragma once

/**
 * @file ParticleRenderStats.h
 * @brief Runtime-visible particle render diagnostics without Render/RHI types
 */

#include "Particle/ParticleTypes.h"

#include <string>

namespace RVX::Particle
{
    /**
     * @brief State from the most recent particle draw attempt.
     */
    struct ParticleRenderDrawStats
    {
        ParticleDepthMode depthMode = ParticleDepthMode::None;
        uint32 submittedVertexCount = 0;
        uint32 submittedIndexCount = 0;
        uint32 submittedInstanceCount = 0;
        bool usedRealSceneDepth = false;
        bool sceneDepthTestEnabled = false;
        bool softParticlesEnabled = false;
        bool drawSubmitted = false;
        bool indexedDraw = false;
        bool indirectDraw = false;
        std::string softParticleFallbackReason;
    };

} // namespace RVX::Particle
