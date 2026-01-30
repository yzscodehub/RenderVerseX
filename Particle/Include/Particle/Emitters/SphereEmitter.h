#pragma once

/**
 * @file SphereEmitter.h
 * @brief Sphere emitter - emits particles from a sphere volume/surface
 */

#include "Particle/Emitters/IEmitter.h"

namespace RVX::Particle
{
    /**
     * @brief Emits particles from a sphere-shaped volume or surface
     */
    class SphereEmitter : public IEmitter
    {
    public:
        /// Outer radius
        float radius = 1.0f;

        /// Thickness (0 = surface only, 1 = solid sphere)
        float radiusThickness = 1.0f;

        /// Emit from shell/surface only
        bool emitFromShell = false;

        /// Hemisphere mode (emit from upper half only)
        bool hemisphere = false;

        const char* GetTypeName() const override { return "SphereEmitter"; }
        EmitterShape GetShape() const override 
        { 
            return hemisphere ? EmitterShape::Hemisphere : EmitterShape::Sphere; 
        }

        void GetEmitParams(EmitterGPUData& outData) const override
        {
            FillCommonParams(outData);
            outData.emitterShape = static_cast<uint32>(GetShape());
            outData.shapeParams = Vec4(
                radius,
                radiusThickness,
                emitFromShell ? 1.0f : 0.0f,
                hemisphere ? 1.0f : 0.0f
            );
        }
    };

} // namespace RVX::Particle
