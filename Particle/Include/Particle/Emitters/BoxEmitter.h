#pragma once

/**
 * @file BoxEmitter.h
 * @brief Box emitter - emits particles from a box volume
 */

#include "Particle/Emitters/IEmitter.h"

namespace RVX::Particle
{
    /**
     * @brief Emits particles from a box-shaped volume
     */
    class BoxEmitter : public IEmitter
    {
    public:
        /// Box half-extents (size is 2x these values)
        Vec3 halfExtents{0.5f, 0.5f, 0.5f};

        /// Emit from surface only
        bool emitFromSurface = false;

        const char* GetTypeName() const override { return "BoxEmitter"; }
        EmitterShape GetShape() const override { return EmitterShape::Box; }

        void GetEmitParams(EmitterGPUData& outData) const override
        {
            FillCommonParams(outData);
            outData.emitterShape = static_cast<uint32>(EmitterShape::Box);
            outData.shapeParams = Vec4(
                halfExtents.x,
                halfExtents.y,
                halfExtents.z,
                emitFromSurface ? 1.0f : 0.0f
            );
        }
    };

} // namespace RVX::Particle
