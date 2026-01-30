#pragma once

/**
 * @file SizeOverLifetimeModule.h
 * @brief Size over lifetime module - modulates particle size based on age
 */

#include "Particle/Modules/IParticleModule.h"
#include "Particle/Curves/AnimationCurve.h"
#include "Core/MathTypes.h"

namespace RVX::Particle
{
    /// LUT size for GPU curve sampling
    constexpr uint32 SIZE_LUT_SIZE = 32;

    /**
     * @brief Modulates particle size based on normalized lifetime
     */
    class SizeOverLifetimeModule : public IParticleModule
    {
    public:
        /// Size curve over lifetime (multiplied with initial size)
        AnimationCurve sizeCurve = AnimationCurve::One();

        /// Size multiplier (applied to curve result)
        Vec2 sizeMultiplier{1.0f, 1.0f};

        /// Separate X and Y curves
        bool separateAxes = false;

        /// Y-axis curve (only used if separateAxes is true)
        AnimationCurve sizeCurveY = AnimationCurve::One();

        const char* GetTypeName() const override { return "SizeOverLifetimeModule"; }

        size_t GetGPUDataSize() const override 
        { 
            // Multiplier + LUT data
            return sizeof(Vec4) + sizeof(float) * SIZE_LUT_SIZE * (separateAxes ? 2 : 1);
        }

        void GetGPUData(void* outData, size_t maxSize) const override
        {
            uint8* ptr = static_cast<uint8*>(outData);
            
            // Write multiplier and flags
            Vec4 params(sizeMultiplier.x, sizeMultiplier.y, separateAxes ? 1.0f : 0.0f, 0.0f);
            std::memcpy(ptr, &params, sizeof(Vec4));
            ptr += sizeof(Vec4);
            
            // Write X curve LUT
            sizeCurve.BakeToLUT(reinterpret_cast<float*>(ptr), SIZE_LUT_SIZE);
            ptr += sizeof(float) * SIZE_LUT_SIZE;
            
            // Write Y curve LUT if separate axes
            if (separateAxes)
            {
                sizeCurveY.BakeToLUT(reinterpret_cast<float*>(ptr), SIZE_LUT_SIZE);
            }
        }

        /// Evaluate size multiplier at normalized lifetime
        Vec2 Evaluate(float normalizedAge) const
        {
            float x = sizeCurve.Evaluate(normalizedAge) * sizeMultiplier.x;
            float y = separateAxes 
                ? sizeCurveY.Evaluate(normalizedAge) * sizeMultiplier.y
                : sizeCurve.Evaluate(normalizedAge) * sizeMultiplier.y;
            return Vec2(x, y);
        }
    };

} // namespace RVX::Particle
