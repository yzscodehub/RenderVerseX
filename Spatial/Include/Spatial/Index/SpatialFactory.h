#pragma once

/**
 * @file SpatialFactory.h
 * @brief Factory for creating spatial index instances
 */

#include "Spatial/Index/ISpatialIndex.h"
#include "Spatial/Index/BVHIndex.h"
#include <string>

namespace RVX::Spatial
{
    /**
     * @brief Type of spatial index
     */
    enum class SpatialIndexType
    {
        BVH,        // Bounding Volume Hierarchy (default)
        Octree,     // Octree (future)
        Grid        // Uniform grid (future)
    };

    /**
     * @brief Factory for creating spatial indices
     */
    class SpatialFactory
    {
    public:
        /// Create a spatial index of the specified type
        static SpatialIndexPtr Create(SpatialIndexType type = SpatialIndexType::BVH);

        /// Create a BVH with specific configuration
        static SpatialIndexPtr CreateBVH(const BVHConfig& config = {});

        /// Get name of spatial index type
        static const char* GetTypeName(SpatialIndexType type);
    };

} // namespace RVX::Spatial
