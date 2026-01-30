#pragma once

/**
 * @file ColorOverLifetimeModule.h
 * @brief Color over lifetime module - modulates particle color based on age
 */

#include "Particle/Modules/IParticleModule.h"
#include "Particle/Curves/GradientCurve.h"

namespace RVX::Particle
{
    /// LUT size for GPU gradient sampling
    constexpr uint32 COLOR_LUT_SIZE = 64;

    /**
     * @brief Modulates particle color based on normalized lifetime
     */
    class ColorOverLifetimeModule : public IParticleModule
    {
    public:
        /// Color gradient over lifetime
        GradientCurve colorGradient = GradientCurve::FadeOut();

        const char* GetTypeName() const override { return "ColorOverLifetimeModule"; }

        size_t GetGPUDataSize() const override 
        { 
            return sizeof(Vec4) * COLOR_LUT_SIZE; 
        }

        void GetGPUData(void* outData, size_t maxSize) const override
        {
            if (maxSize < sizeof(Vec4) * COLOR_LUT_SIZE) return;
            colorGradient.BakeToLUT(static_cast<Vec4*>(outData), COLOR_LUT_SIZE);
        }

        /// Evaluate color at normalized lifetime
        Vec4 Evaluate(float normalizedAge) const
        {
            return colorGradient.Evaluate(normalizedAge);
        }
    };

} // namespace RVX::Particle
