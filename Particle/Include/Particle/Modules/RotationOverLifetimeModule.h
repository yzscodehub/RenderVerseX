#pragma once

/**
 * @file RotationOverLifetimeModule.h
 * @brief Rotation over lifetime module - modifies particle rotation
 */

#include "Particle/Modules/IParticleModule.h"
#include "Particle/Curves/AnimationCurve.h"
#include "Particle/ParticleTypes.h"

namespace RVX::Particle
{
    /**
     * @brief GPU data for rotation over lifetime
     */
    struct RotationOverLifetimeGPUData
    {
        float angularVelocityMin;
        float angularVelocityMax;
        float separateAxes;
        float pad;
    };

    /**
     * @brief Modifies particle rotation over lifetime
     */
    class RotationOverLifetimeModule : public IParticleModule
    {
    public:
        /// Angular velocity range (degrees per second)
        FloatRange angularVelocity{0.0f, 360.0f};

        /// Angular velocity curve (multiplier)
        AnimationCurve angularVelocityCurve = AnimationCurve::One();

        const char* GetTypeName() const override { return "RotationOverLifetimeModule"; }

        size_t GetGPUDataSize() const override 
        { 
            return sizeof(RotationOverLifetimeGPUData);
        }

        void GetGPUData(void* outData, size_t maxSize) const override
        {
            if (maxSize < sizeof(RotationOverLifetimeGPUData)) return;
            
            RotationOverLifetimeGPUData data;
            data.angularVelocityMin = radians(angularVelocity.min);
            data.angularVelocityMax = radians(angularVelocity.max);
            data.separateAxes = 0.0f;
            data.pad = 0.0f;
            
            std::memcpy(outData, &data, sizeof(data));
        }
    };

} // namespace RVX::Particle
