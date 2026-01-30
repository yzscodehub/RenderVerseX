#pragma once

/**
 * @file ForceModule.h
 * @brief Force module - applies gravity, constant forces, and drag
 */

#include "Particle/Modules/IParticleModule.h"
#include "Core/MathTypes.h"
#include <cstring>

namespace RVX::Particle
{
    /**
     * @brief GPU data for force module
     */
    struct ForceModuleGPUData
    {
        Vec4 gravity;           ///< xyz=gravity, w=unused
        Vec4 constantForce;     ///< xyz=constant force, w=drag
        Vec4 wind;              ///< xyz=wind direction * strength, w=turbulence
    };

    /**
     * @brief Applies forces to particles (gravity, wind, drag)
     */
    class ForceModule : public IParticleModule
    {
    public:
        /// Gravity force
        Vec3 gravity{0.0f, -9.81f, 0.0f};

        /// Constant force (like wind direction)
        Vec3 constantForce{0.0f, 0.0f, 0.0f};

        /// Drag coefficient (0 = no drag, 1 = heavy drag)
        float drag = 0.0f;

        /// Wind force
        Vec3 wind{0.0f, 0.0f, 0.0f};

        /// Wind turbulence (randomness)
        float windTurbulence = 0.0f;

        const char* GetTypeName() const override { return "ForceModule"; }

        size_t GetGPUDataSize() const override { return sizeof(ForceModuleGPUData); }

        void GetGPUData(void* outData, size_t maxSize) const override
        {
            if (maxSize < sizeof(ForceModuleGPUData)) return;
            
            ForceModuleGPUData data;
            data.gravity = Vec4(gravity, 0.0f);
            data.constantForce = Vec4(constantForce, drag);
            data.wind = Vec4(wind, windTurbulence);
            
            std::memcpy(outData, &data, sizeof(data));
        }
    };

} // namespace RVX::Particle
