#pragma once

/**
 * @file WaterTypes.h
 * @brief Compatibility-free aliases for the canonical Water packet vocabulary.
 *
 * Water supports a bounded Gerstner surface in GFX18. Deferred water features
 * have no placeholder runtime representation.
 */

#include "RenderContracts/WaterRenderSnapshot.h"

namespace RVX::Water
{
    using GerstnerWave = WaterGerstnerWave;
    using GerstnerWaveSet = WaterGerstnerWaveSet;
} // namespace RVX::Water
