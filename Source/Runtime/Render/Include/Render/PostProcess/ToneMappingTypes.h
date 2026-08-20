#pragma once

/**
 * @file ToneMappingTypes.h
 * @brief Shared tone mapping configuration types
 */

#include "Core/Types.h"

namespace RVX
{
    /**
     * @brief Tone mapping operator types
     */
    enum class ToneMappingOperator : uint8
    {
        Reinhard,           // Simple Reinhard
        ReinhardExtended,   // Extended Reinhard with white point
        ACES,               // ACES filmic
        Uncharted2,         // Filmic curve from Uncharted 2
        Neutral,            // Neutral tonemapper
        None                // No tone mapping (pass-through)
    };

    /**
     * @brief Exposure source used by the tone mapping pass
     */
    enum class ToneMappingExposureMode : uint8
    {
        ManualMultiplier,   // Use PostProcessSettings::exposure as a linear multiplier
        CameraEV100         // Resolve exposure from camera EV100 and compensation stops
    };

    /**
     * @brief Display/output color space written by the tone mapping pass
     */
    enum class ToneMappingOutputColorSpace : uint8
    {
        Linear,             // Keep the tone-mapped value linear for HDR or offscreen consumers
        SRGB                // Apply gamma display conversion for standard UNORM back buffers
    };

} // namespace RVX
