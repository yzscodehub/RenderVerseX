#pragma once

/**
 * @file BoundingSphere.h
 * @brief Backward compatibility header
 * 
 * @deprecated This header is deprecated. Use Core/Math/Sphere.h directly.
 * 
 * The Sphere class has been moved to Core/Math for better module organization.
 * BoundingSphere is now an alias for Sphere in the RVX namespace.
 */

#ifdef _MSC_VER
#pragma message("Warning: Spatial/Bounds/BoundingSphere.h is deprecated. Use Core/Math/Sphere.h instead.")
#endif

#include "Core/Math/Sphere.h"

// For backward compatibility with code using RVX::Spatial::BoundingSphere
namespace RVX::Spatial
{
    using BoundingSphere = ::RVX::Sphere;
}
