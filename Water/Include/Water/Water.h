#pragma once

/**
 * @file Water.h
 * @brief Main header for the Water module
 * 
 * The Water module provides realistic water surface rendering with:
 * - FFT-based ocean wave simulation
 * - Gerstner wave support
 * - Reflection and refraction
 * - Caustics rendering
 * - Underwater effects
 * - Foam and whitecaps
 */

#include "Water/WaterComponent.h"
#include "Water/WaterSurface.h"
#include "Water/WaterSimulation.h"
#include "Water/Caustics.h"
#include "Water/Underwater.h"

namespace RVX
{
    /**
     * @brief Water module version
     */
    constexpr uint32 RVX_WATER_VERSION_MAJOR = 1;
    constexpr uint32 RVX_WATER_VERSION_MINOR = 0;
    constexpr uint32 RVX_WATER_VERSION_PATCH = 0;

    /**
     * @brief Water quality presets
     */
    enum class WaterQuality : uint8
    {
        Low,        ///< Mobile/low-end: simple waves, no caustics
        Medium,     ///< Medium: Gerstner waves, basic reflections
        High,       ///< High: FFT waves, full reflections/refractions
        Ultra       ///< Ultra: Full simulation, screen-space reflections, caustics
    };

} // namespace RVX
