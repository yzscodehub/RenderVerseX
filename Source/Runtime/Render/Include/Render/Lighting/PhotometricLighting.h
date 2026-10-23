#pragma once

/**
 * @file PhotometricLighting.h
 * @brief CPU reference for the renderer's finite-range photometric falloff.
 */

#include "Core/Types.h"

#include <algorithm>
#include <cmath>

namespace RVX
{
    /**
     * @brief Shared CPU/GPU denominator clamp for positive local-light distances.
     *
     * A zero (or invalid) distance has no defined light direction and contributes
     * zero. Every positive distance uses this denominator clamp instead of a
     * second near-zero cutoff.
     */
    inline constexpr float32 RVX_PHOTOMETRIC_MIN_DISTANCE_SQUARED = 1.0e-4f;

    /** @brief Physically based inverse-square term used for candela lights. */
    [[nodiscard]] inline float32 EvaluatePhotometricInverseSquare(
        float32 distanceMeters) noexcept
    {
        if (!std::isfinite(distanceMeters) || distanceMeters <= 0.0f)
        {
            return 0.0f;
        }
        return 1.0f / std::max(
            distanceMeters * distanceMeters,
            RVX_PHOTOMETRIC_MIN_DISTANCE_SQUARED);
    }

    /**
     * @brief Independent smooth finite-influence window for local lights.
     *
     * This window is an implementation/culling policy. It never changes the
     * meaning of one candela at a given distance inside an effectively
     * unbounded range.
     */
    [[nodiscard]] inline float32 EvaluateFiniteLightRangeWindow(
        float32 distanceMeters,
        float32 rangeMeters) noexcept
    {
        if (!std::isfinite(distanceMeters) ||
            !std::isfinite(rangeMeters) ||
            distanceMeters < 0.0f || rangeMeters <= 1.0e-4f ||
            distanceMeters >= rangeMeters)
        {
            return 0.0f;
        }
        const float32 normalizedDistance = distanceMeters / rangeMeters;
        const float32 base = std::clamp(
            1.0f - normalizedDistance * normalizedDistance,
            0.0f,
            1.0f);
        return base * base;
    }

    /** @brief Canonical local-light attenuation reference. */
    [[nodiscard]] inline float32 EvaluatePhotometricLocalLightAttenuation(
        float32 distanceMeters,
        float32 rangeMeters) noexcept
    {
        return EvaluatePhotometricInverseSquare(distanceMeters) *
               EvaluateFiniteLightRangeWindow(distanceMeters, rangeMeters);
    }
} // namespace RVX
