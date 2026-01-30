#pragma once

/**
 * @file NoiseModule.h
 * @brief Noise module - applies turbulence to particle movement
 */

#include "Particle/Modules/IParticleModule.h"
#include "Core/MathTypes.h"
#include <cstring>

namespace RVX::Particle
{
    /**
     * @brief Noise quality/type
     */
    enum class NoiseQuality : uint8
    {
        Low,        ///< Simple value noise (fast)
        Medium,     ///< Perlin noise
        High        ///< Simplex noise (best quality)
    };

    /**
     * @brief GPU data for noise module
     */
    struct NoiseModuleGPUData
    {
        Vec4 params;        ///< x=strength, y=frequency, z=scrollSpeed, w=octaves
        Vec4 params2;       ///< x=quality, y=positionAmount, z=rotationAmount, w=sizeAmount
        Vec4 scrollOffset;  ///< xyz=scroll offset, w=time multiplier
    };

    /**
     * @brief Applies noise-based turbulence to particles
     */
    class NoiseModule : public IParticleModule
    {
    public:
        /// Noise strength (affects velocity/position)
        float strength = 1.0f;

        /// Noise frequency (higher = more detail)
        float frequency = 1.0f;

        /// Noise scroll speed (animation)
        float scrollSpeed = 0.0f;

        /// Number of octaves (more = more detail, slower)
        uint32 octaves = 1;

        /// Noise quality
        NoiseQuality quality = NoiseQuality::Medium;

        /// Position influence (0-1)
        float positionAmount = 1.0f;

        /// Rotation influence (0-1)
        float rotationAmount = 0.0f;

        /// Size influence (0-1)
        float sizeAmount = 0.0f;

        /// Separate axes (different noise per axis)
        bool separateAxes = false;

        /// Remap range (remap noise output)
        Vec2 remapRange{0.0f, 1.0f};

        const char* GetTypeName() const override { return "NoiseModule"; }

        size_t GetGPUDataSize() const override 
        { 
            return sizeof(NoiseModuleGPUData);
        }

        void GetGPUData(void* outData, size_t maxSize) const override
        {
            if (maxSize < sizeof(NoiseModuleGPUData)) return;
            
            NoiseModuleGPUData data;
            data.params = Vec4(strength, frequency, scrollSpeed, 
                               static_cast<float>(octaves));
            data.params2 = Vec4(static_cast<float>(quality), 
                                positionAmount, rotationAmount, sizeAmount);
            data.scrollOffset = Vec4(0.0f, 0.0f, 0.0f, 1.0f);
            
            std::memcpy(outData, &data, sizeof(data));
        }
    };

} // namespace RVX::Particle
