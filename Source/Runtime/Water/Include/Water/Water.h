#pragma once

/**
 * @file Water.h
 * @brief Main header for the Water module
 * 
 * The Water module provides a pure deterministic Gerstner query contract.
 * Render owns grid resources, shading, reflection/refraction, and foam passes.
 */

#include "Water/WaterTypes.h"
#include "Water/GerstnerSurface.h"

namespace RVX
{
    constexpr uint32 RVX_WATER_VERSION_MAJOR = 2;
    constexpr uint32 RVX_WATER_VERSION_MINOR = 0;
    constexpr uint32 RVX_WATER_VERSION_PATCH = 0;

} // namespace RVX
