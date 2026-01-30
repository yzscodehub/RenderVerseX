#pragma once

/**
 * @file VelocityOverLifetimeModule.h
 * @brief Velocity over lifetime module - modifies particle velocity during lifetime
 */

#include "Particle/Modules/IParticleModule.h"
#include "Particle/Curves/AnimationCurve.h"
#include "Core/MathTypes.h"

namespace RVX::Particle
{
    /**
     * @brief Velocity space for velocity modifications
     */
    enum class VelocitySpace : uint8
    {
        Local,      ///< Relative to emitter transform
        World       ///< World space
    };

    /**
     * @brief GPU data for velocity over lifetime
     */
    struct VelocityOverLifetimeGPUData
    {
        Vec4 linearVelocity;    ///< xyz=velocity, w=space (0=local, 1=world)
        Vec4 orbitalVelocity;   ///< xyz=orbital axis, w=speed
        Vec4 radialVelocity;    ///< x=radial speed, y=speedModifier, zw=unused
    };

    /**
     * @brief Modifies particle velocity over lifetime
     */
    class VelocityOverLifetimeModule : public IParticleModule
    {
    public:
        /// Linear velocity to add
        Vec3 linearVelocity{0.0f, 0.0f, 0.0f};

        /// Velocity space
        VelocitySpace space = VelocitySpace::Local;

        /// Speed multiplier curve over lifetime
        AnimationCurve speedModifier = AnimationCurve::One();

        /// Orbital velocity (rotation around a point)
        Vec3 orbitalVelocity{0.0f, 0.0f, 0.0f};

        /// Orbital offset from particle position
        Vec3 orbitalOffset{0.0f, 0.0f, 0.0f};

        /// Radial velocity (towards/away from emitter)
        float radialVelocity = 0.0f;

        const char* GetTypeName() const override { return "VelocityOverLifetimeModule"; }

        size_t GetGPUDataSize() const override 
        { 
            return sizeof(VelocityOverLifetimeGPUData);
        }

        void GetGPUData(void* outData, size_t maxSize) const override
        {
            if (maxSize < sizeof(VelocityOverLifetimeGPUData)) return;
            
            VelocityOverLifetimeGPUData data;
            data.linearVelocity = Vec4(linearVelocity, 
                space == VelocitySpace::World ? 1.0f : 0.0f);
            data.orbitalVelocity = Vec4(orbitalVelocity, 0.0f);
            data.radialVelocity = Vec4(radialVelocity, 0.0f, 0.0f, 0.0f);
            
            std::memcpy(outData, &data, sizeof(data));
        }
    };

} // namespace RVX::Particle
