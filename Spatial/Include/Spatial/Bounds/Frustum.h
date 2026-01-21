#pragma once

/**
 * @file Frustum.h
 * @brief Backward compatibility header
 * 
 * @deprecated This header is deprecated. Use Core/Math/Frustum.h directly.
 * 
 * The Frustum, Plane, FrustumPlane, and IntersectionResult types have been 
 * moved to Core/Math for better module organization.
 */

#ifdef _MSC_VER
#pragma message("Warning: Spatial/Bounds/Frustum.h is deprecated. Use Core/Math/Frustum.h instead.")
#endif

#include "Core/Math/Plane.h"
#include "Core/Math/Frustum.h"

// For backward compatibility with code using RVX::Spatial:: types
namespace RVX::Spatial
{
    using Plane = ::RVX::Plane;
    using Frustum = ::RVX::Frustum;
    using FrustumPlane = ::RVX::FrustumPlane;
    using IntersectionResult = ::RVX::IntersectionResult;
}
