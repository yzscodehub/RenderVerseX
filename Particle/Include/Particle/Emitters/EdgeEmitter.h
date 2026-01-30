#pragma once

/**
 * @file EdgeEmitter.h
 * @brief Edge emitter - emits particles along a line segment
 */

#include "Particle/Emitters/IEmitter.h"

namespace RVX::Particle
{
    /**
     * @brief Emits particles along a line segment
     */
    class EdgeEmitter : public IEmitter
    {
    public:
        /// Length of the edge
        float length = 1.0f;

        const char* GetTypeName() const override { return "EdgeEmitter"; }
        EmitterShape GetShape() const override { return EmitterShape::Edge; }

        void GetEmitParams(EmitterGPUData& outData) const override
        {
            FillCommonParams(outData);
            outData.emitterShape = static_cast<uint32>(EmitterShape::Edge);
            outData.shapeParams = Vec4(length, 0.0f, 0.0f, 0.0f);
        }
    };

} // namespace RVX::Particle
