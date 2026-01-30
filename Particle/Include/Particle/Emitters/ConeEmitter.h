#pragma once

/**
 * @file ConeEmitter.h
 * @brief Cone emitter - emits particles in a conical pattern
 */

#include "Particle/Emitters/IEmitter.h"

namespace RVX::Particle
{
    /**
     * @brief Emits particles in a cone-shaped pattern
     * 
     * Particles are emitted from the base of the cone and travel
     * outward within the cone angle.
     */
    class ConeEmitter : public IEmitter
    {
    public:
        /// Cone angle in degrees (0 = straight line, 90 = hemisphere)
        float angle = 25.0f;

        /// Base radius
        float radius = 1.0f;

        /// Cone length (for volume emission)
        float length = 5.0f;

        /// Emit from base (true) or throughout volume (false)
        bool emitFromBase = true;

        /// Emit from volume vs edge only
        bool emitFromVolume = true;

        const char* GetTypeName() const override { return "ConeEmitter"; }
        EmitterShape GetShape() const override { return EmitterShape::Cone; }

        void GetEmitParams(EmitterGPUData& outData) const override
        {
            FillCommonParams(outData);
            outData.emitterShape = static_cast<uint32>(EmitterShape::Cone);
            
            // Pack cone parameters
            float angleRad = radians(angle);
            outData.shapeParams = Vec4(
                angleRad,
                radius,
                length,
                (emitFromBase ? 1.0f : 0.0f) + (emitFromVolume ? 2.0f : 0.0f)
            );
        }
    };

} // namespace RVX::Particle
