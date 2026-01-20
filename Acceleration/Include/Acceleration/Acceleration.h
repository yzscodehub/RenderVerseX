/**
 * @file Acceleration.h
 * @brief Unified Acceleration Module Header
 * 
 * Provides ray casting and spatial acceleration structures for picking.
 */

#pragma once

#include "Acceleration/Ray.h"
#include "Acceleration/Intersection.h"
#include "Acceleration/BVH.h"

namespace RVX
{

/**
 * @brief Acceleration module version
 */
struct AccelerationInfo
{
    static constexpr const char* kVersionString = "1.0.0";
    static const char* GetVersionString() { return kVersionString; }
};

} // namespace RVX
