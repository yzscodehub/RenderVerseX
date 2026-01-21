#pragma once

/**
 * @file BoundsTests.h
 * @brief Backward compatibility header
 * 
 * @deprecated This header is deprecated. Use Core/Math/Intersection.h directly.
 * 
 * The intersection test functions have been moved to Core/Math/Intersection.h
 * for better module organization.
 */

#ifdef _MSC_VER
#pragma message("Warning: Spatial/Bounds/BoundsTests.h is deprecated. Use Core/Math/Intersection.h instead.")
#endif

#include "Core/Math/Intersection.h"

// For backward compatibility, import functions into RVX::Spatial namespace
namespace RVX::Spatial
{
    using ::RVX::Overlaps;
    using ::RVX::Contains;
    using ::RVX::Distance;
    using ::RVX::DistanceSquared;
}
