#pragma once

/**
 * @file TerrainTypes.h
 * @brief Runtime-facing terrain contracts without Render/RHI types
 */

#include "Core/MathTypes.h"
#include "Core/Types.h"

namespace RVX
{
    /**
     * @brief Terrain component settings
     */
    struct TerrainSettings
    {
        Vec3 size{1000.0f, 100.0f, 1000.0f};    ///< Terrain size (width, height, depth)
        float lodBias = 0.0f;                    ///< LOD bias (negative = higher quality)
        uint32 patchSize = 32;                   ///< Patch size in vertices (power of 2)
        uint32 maxLODLevels = 8;                 ///< Maximum LOD levels
        bool castShadows = true;                 ///< Whether terrain casts shadows
        bool receiveShadows = true;              ///< Whether terrain receives shadows
    };

} // namespace RVX
