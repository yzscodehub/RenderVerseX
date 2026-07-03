#pragma once

/**
 * @file Terrain.h
 * @brief Main header for the Terrain module
 * 
 * The Terrain module provides heightmap-based terrain rendering with:
 * - Multi-resolution heightmap support
 * - CDLOD (Continuous Distance-dependent Level of Detail)
 * - Multi-layer texture splatting
 * - Physics collision integration
 * - Normal map generation
 */

#include "Terrain/TerrainComponent.h"
#include "Terrain/TerrainCollider.h"
#include "Terrain/TerrainTypes.h"

namespace RVX
{
    /**
     * @brief Terrain module version
     */
    constexpr uint32 RVX_TERRAIN_VERSION_MAJOR = 1;
    constexpr uint32 RVX_TERRAIN_VERSION_MINOR = 0;
    constexpr uint32 RVX_TERRAIN_VERSION_PATCH = 0;

} // namespace RVX
