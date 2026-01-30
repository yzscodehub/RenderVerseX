#pragma once

/**
 * @file CircleEmitter.h
 * @brief Circle emitter - emits particles from a circle edge or area
 */

#include "Particle/Emitters/IEmitter.h"

namespace RVX::Particle
{
    /**
     * @brief Emits particles from a circle (2D ring or disc)
     */
    class CircleEmitter : public IEmitter
    {
    public:
        /// Circle radius
        float radius = 1.0f;

        /// Thickness (0 = edge only, 1 = full disc)
        float radiusThickness = 0.0f;

        /// Arc angle in degrees (360 = full circle)
        float arc = 360.0f;

        const char* GetTypeName() const override { return "CircleEmitter"; }
        EmitterShape GetShape() const override { return EmitterShape::Circle; }

        void GetEmitParams(EmitterGPUData& outData) const override
        {
            FillCommonParams(outData);
            outData.emitterShape = static_cast<uint32>(EmitterShape::Circle);
            outData.shapeParams = Vec4(
                radius,
                radiusThickness,
                radians(arc),
                0.0f
            );
        }
    };

} // namespace RVX::Particle
