#pragma once

/**
 * @file PointEmitter.h
 * @brief Point emitter - emits particles from a single point
 */

#include "Particle/Emitters/IEmitter.h"

namespace RVX::Particle
{
    /**
     * @brief Emits particles from a single point in space
     */
    class PointEmitter : public IEmitter
    {
    public:
        const char* GetTypeName() const override { return "PointEmitter"; }
        EmitterShape GetShape() const override { return EmitterShape::Point; }

        void GetEmitParams(EmitterGPUData& outData) const override
        {
            FillCommonParams(outData);
            outData.emitterShape = static_cast<uint32>(EmitterShape::Point);
            outData.shapeParams = Vec4(0.0f, 0.0f, 0.0f, 0.0f);
        }
    };

} // namespace RVX::Particle
