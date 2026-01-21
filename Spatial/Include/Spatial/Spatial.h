#pragma once

/**
 * @file Spatial.h
 * @brief Unified header for Spatial module
 * 
 * The Spatial module provides:
 * - Spatial index interfaces and implementations (BVH, Octree)
 * - Unified query API for culling, picking, and range queries
 * 
 * Note: Geometric primitives (AABB, Sphere, Frustum, Ray) are now in Core/Math.
 * Include "Core/Math/Geometry.h" for those types.
 */

// Geometry (from Core/Math)
#include "Core/Math/Geometry.h"

// Query
#include "Spatial/Query/QueryFilter.h"
#include "Spatial/Query/SpatialQuery.h"

// Index
#include "Spatial/Index/ISpatialEntity.h"
#include "Spatial/Index/ISpatialIndex.h"
#include "Spatial/Index/SpatialFactory.h"
