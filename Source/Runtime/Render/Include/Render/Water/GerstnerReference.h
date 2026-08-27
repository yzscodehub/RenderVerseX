#pragma once

/**
 * @file GerstnerReference.h
 * @brief CPU reference for the canonical RenderContracts Gerstner convention.
 */

#include "Core/MathTypes.h"
#include "RenderContracts/WaterGerstnerMath.h"
#include "RenderContracts/WaterRenderSnapshot.h"

#include <algorithm>
#include <cmath>

namespace RVX
{
    struct WaterGerstnerReferenceSample
    {
        Vec3 displacement{0.0f, 0.0f, 0.0f};
        Vec3 normal{0.0f, 1.0f, 0.0f};
        float32 crest = 0.0f;
    };

    /**
     * Evaluates the sole GFX18 wave convention.
     *
     * Every wave reduces the frozen float64 frame time by its own period
     * before phase evaluation. This mirrors the renderer upload convention:
     * shaders receive bounded per-wave float32 k and temporal phase values,
     * never a raw float time that would lose precision in a long-running
     * session.
     * Horizontal amplitude is steepness / k; packet validation bounds the
     * aggregate steepness of active waves at one.
     *
     * @param localPositionXZ Position relative to the water surface origin.
     *        Translating a water surface must not change its wave phase.
     */
    [[nodiscard]] inline bool EvaluateWaterGerstnerReference(
        const WaterGerstnerWaveSet& waves,
        const Vec2& localPositionXZ,
        float64 simulationSeconds,
        WaterGerstnerReferenceSample& outSample) noexcept
    {
        outSample = {};
        if (!waves.IsStructurallyValid() || !std::isfinite(simulationSeconds))
        {
            return false;
        }
        WaterGerstnerSurfaceMathSample mathSample;
        if (!EvaluateWaterGerstnerSurfaceMath(
                waves, localPositionXZ, simulationSeconds, mathSample))
        {
            return false;
        }
        outSample.displacement = mathSample.displacement;
        outSample.crest = mathSample.crest;
        const Vec3 unnormalized = cross(mathSample.tangentZ, mathSample.tangentX);
        const float32 normalLength = length(unnormalized);
        if (!std::isfinite(normalLength) || normalLength <= 1.0e-6f)
        {
            outSample = {};
            return false;
        }
        outSample.normal = unnormalized / normalLength;
        return true;
    }

    /**
     * Reduces the four scrolling detail-normal temporal offsets independently.
     *
     * The shader adds these offsets directly. Reducing one shared time and
     * multiplying it by non-integral rates would introduce a discontinuity at
     * that shared time's period.
     */
    [[nodiscard]] inline bool EvaluateWaterDetailTemporalPhases(
        float64 simulationSeconds, Vec4& outPhases) noexcept
    {
        outPhases = {};
        if (!std::isfinite(simulationSeconds))
        {
            return false;
        }
        constexpr float64 twoPi = 6.28318530717958647692;
        const auto reduceRate = [simulationSeconds, twoPi](float64 rate,
                                                            float32& outPhase) noexcept
        {
            const float64 period = twoPi / std::abs(rate);
            const float64 reducedTime = std::remainder(simulationSeconds, period);
            const float64 phase = std::remainder(rate * reducedTime, twoPi);
            const float32 narrowedPhase = static_cast<float32>(phase);
            if (!std::isfinite(reducedTime) || !std::isfinite(phase) ||
                !std::isfinite(narrowedPhase))
            {
                return false;
            }
            outPhase = narrowedPhase;
            return true;
        };
        return reduceRate(0.07, outPhases.x) &&
            reduceRate(-0.05, outPhases.y) &&
            reduceRate(-0.11, outPhases.z) &&
            reduceRate(0.09, outPhases.w);
    }

    /** Beer-Lambert refraction identity used by WaterSurface.hlsl. */
    [[nodiscard]] inline Vec3 EvaluateWaterTransmission(
        const Vec3& refractedColor,
        const Vec3& absorptionCoefficient,
        float32 thicknessMeters) noexcept
    {
        const float32 thickness = std::max(thicknessMeters, 0.0f);
        const Vec3 transmission{
            std::exp(-absorptionCoefficient.x * thickness),
            std::exp(-absorptionCoefficient.y * thickness),
            std::exp(-absorptionCoefficient.z * thickness)};
        return refractedColor * transmission;
    }

    /** Opaque-composition reference: a composed water pixel is never blended twice. */
    [[nodiscard]] inline Vec3 ComposeWaterSurfaceColor(
        const Vec3& refractedColor,
        const Vec3& waterResponseColor,
        float32 response) noexcept
    {
        return refractedColor + (waterResponseColor - refractedColor) *
            std::clamp(response, 0.0f, 1.0f);
    }
} // namespace RVX
