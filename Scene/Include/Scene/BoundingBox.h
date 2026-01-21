#pragma once

/**
 * @file BoundingBox.h
 * @brief Backward compatibility header
 * 
 * @deprecated Use Core/Math/AABB.h directly for new code.
 * 
 * The AABB class has been moved to Core/Math for better module organization.
 * BoundingBox is now an alias for AABB in the RVX namespace.
 */

#ifdef _MSC_VER
#pragma message("Warning: Scene/BoundingBox.h is deprecated. Use Core/Math/AABB.h instead.")
#endif

#include "Core/Math/AABB.h"
