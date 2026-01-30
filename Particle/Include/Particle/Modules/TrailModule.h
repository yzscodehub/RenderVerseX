#pragma once

/**
 * @file TrailModule.h
 * @brief Trail module - particle trail/ribbon rendering configuration
 */

#include "Particle/Modules/IParticleModule.h"
#include "Particle/Curves/AnimationCurve.h"
#include "Particle/Curves/GradientCurve.h"
#include "Core/MathTypes.h"

namespace RVX::Particle
{
    /**
     * @brief Trail texture mode
     */
    enum class TrailTextureMode : uint8
    {
        Stretch,        ///< Stretch texture along trail
        Tile,           ///< Tile texture along trail
        DistributePerSegment,   ///< Distribute UV per segment
        RepeatPerSegment        ///< Repeat UV per segment
    };

    /**
     * @brief Trail/ribbon configuration for particles
     * 
     * Note: Trail rendering is CPU-intensive and requires special
     * handling in the renderer. Each particle stores a history
     * of positions to form the trail.
     */
    class TrailModule : public IParticleModule
    {
    public:
        /// Trail width
        float width = 0.5f;

        /// Width curve over trail length (0 = head, 1 = tail)
        AnimationCurve widthOverTrail = AnimationCurve::FadeOut();

        /// Trail lifetime (how long trail segments persist)
        float lifetime = 1.0f;

        /// Maximum number of trail points per particle
        uint32 maxPoints = 50;

        /// Minimum distance between trail vertices
        float minVertexDistance = 0.1f;

        /// Color over trail length
        GradientCurve colorOverTrail = GradientCurve::FadeOut();

        /// Inherit color from particle
        bool inheritParticleColor = true;

        /// Trail dies when particle dies
        bool dieWithParticle = true;

        /// Texture mode
        TrailTextureMode textureMode = TrailTextureMode::Stretch;

        /// Ratio (0 = trail starts at particle, 1 = trail follows behind)
        float ratio = 1.0f;

        /// Generate lighting normals
        bool generateLightingNormals = false;

        /// Split sub-emitter trails
        bool splitSubEmitterRibbons = false;

        /// Attach ribbons to transform
        bool attachRibbonsToTransform = false;

        const char* GetTypeName() const override { return "TrailModule"; }
        ModuleStage GetStage() const override { return ModuleStage::Render; }
        
        /// Trail requires special CPU-side handling
        bool IsGPUModule() const override { return false; }

        size_t GetGPUDataSize() const override { return 0; }
    };

} // namespace RVX::Particle
