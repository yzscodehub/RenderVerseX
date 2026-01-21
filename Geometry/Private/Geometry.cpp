/**
 * @file Geometry.cpp
 * @brief Geometry module implementation
 * 
 * Most geometry types are header-only with inline implementations.
 * This file provides any non-inline implementations and ensures
 * the library has at least one translation unit.
 */

#include "Geometry/Geometry.h"

namespace RVX::Geometry
{

// Module version info (for debugging/diagnostics)
static constexpr int GEOMETRY_VERSION_MAJOR = 1;
static constexpr int GEOMETRY_VERSION_MINOR = 0;
static constexpr int GEOMETRY_VERSION_PATCH = 0;

/**
 * @brief Get the module version string
 */
const char* GetGeometryVersion()
{
    return "1.0.0";
}

} // namespace RVX::Geometry
