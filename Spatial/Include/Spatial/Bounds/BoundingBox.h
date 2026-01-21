#pragma once

/**
 * @file BoundingBox.h
 * @brief Backward compatibility header
 * 
 * @deprecated This header is deprecated. Use Core/Math/AABB.h directly.
 * 
 * The AABB class has been moved to Core/Math for better module organization.
 * BoundingBox is now an alias for AABB in the RVX namespace.
 */

#ifdef _MSC_VER
#pragma message("Warning: Spatial/Bounds/BoundingBox.h is deprecated. Use Core/Math/AABB.h instead.")
#endif

#include "Core/Math/AABB.h"

// For backward compatibility with code using RVX::Spatial::BoundingBox
namespace RVX::Spatial
{
    using BoundingBox = ::RVX::AABB;
}
